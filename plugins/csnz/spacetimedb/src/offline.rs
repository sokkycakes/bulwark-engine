//! Offline accounts: the user database of the real CSNZ client's offline
//! server (csnzserver.dll, hosted by CSOLauncher -offline), mirrored here so
//! the launcher flow keeps its data in this database instead of only in
//! OfflineData/UserDatabase.db3.
//!
//! The server pushes a whole snapshot of its SQLite database at the end of a
//! session and pulls it back at the start of the next one (on any machine).
//! A snapshot is a numbered list of chunks under one sync id: chunk 0 holds
//! the schema ({"user_version", "schema"}), every other chunk part of one
//! table ({"columns", "rows"}). The chunks are opaque JSON here; the server
//! writes and reads them. Uploading is two-phase, so a session that dies
//! mid-upload never replaces the stored snapshot with a partial one:
//! offline_stage each chunk, then offline_commit swaps the new sync id in and
//! drops every other chunk of that account.
//!
//! Original code, ISC licence.

use spacetimedb::{procedure, reducer, table, ProcedureContext, ReducerContext, Table, Timestamp};

use crate::{is_operator, require_operator};

/// Largest chunk the server sends (it aims for about 256 KiB).
const MAX_CHUNK_BYTES: usize = 4 << 20;
/// Largest snapshot, in chunks.
const MAX_CHUNKS: u32 = 4096;

#[table(accessor = offline_account)]
pub struct OfflineAccount {
    #[primary_key]
    pub account: String,
    /// The committed snapshot.
    pub sync_id: String,
    pub chunks: u32,
    pub rows: u64,
    /// Free text from the server, e.g. its host name and build.
    pub source: String,
    pub updated: Timestamp,
}

#[table(accessor = offline_chunk)]
pub struct OfflineChunk {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    #[index(btree)]
    pub account: String,
    pub sync_id: String,
    pub idx: u32,
    /// "" for the schema chunk, else the SQLite table the rows belong to.
    pub table_name: String,
    pub data: String,
    pub staged: Timestamp,
}

fn check_name(what: &str, s: &str) -> Result<(), String> {
    if s.is_empty() || s.len() > 64 || !s.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_' || c == '.') {
        Err(format!("bad {what}"))
    } else {
        Ok(())
    }
}

/// Stores one chunk of a snapshot that isn't committed yet. Sending the same
/// (account, sync_id, idx) again replaces it, so retries are safe.
#[reducer]
pub fn offline_stage(
    ctx: &ReducerContext,
    account: String,
    sync_id: String,
    idx: u32,
    table_name: String,
    data: String,
) -> Result<(), String> {
    require_operator(ctx)?;
    check_name("account", &account)?;
    check_name("sync id", &sync_id)?;
    if !table_name.is_empty() {
        check_name("table", &table_name)?;
    }
    if idx >= MAX_CHUNKS {
        return Err("too many chunks".into());
    }
    if data.len() > MAX_CHUNK_BYTES {
        return Err("chunk too large".into());
    }
    if let Some(account) = ctx.db.offline_account().account().find(account.clone()) {
        if account.sync_id == sync_id {
            return Err("that snapshot is already committed".into());
        }
    }
    let old: Vec<u64> = ctx
        .db
        .offline_chunk()
        .account()
        .filter(&account)
        .filter(|c| c.sync_id == sync_id && c.idx == idx)
        .map(|c| c.id)
        .collect();
    for id in old {
        ctx.db.offline_chunk().id().delete(id);
    }
    ctx.db.offline_chunk().insert(OfflineChunk {
        id: 0,
        account,
        sync_id,
        idx,
        table_name,
        data,
        staged: ctx.timestamp,
    });
    Ok(())
}

/// Makes a staged snapshot the account's current one, once all of its chunks
/// are there, and drops the account's other chunks (the previous snapshot
/// and any upload that never finished).
#[reducer]
pub fn offline_commit(
    ctx: &ReducerContext,
    account: String,
    sync_id: String,
    chunks: u32,
    rows: u64,
    source: String,
) -> Result<(), String> {
    require_operator(ctx)?;
    check_name("account", &account)?;
    check_name("sync id", &sync_id)?;
    if chunks == 0 || chunks > MAX_CHUNKS {
        return Err("bad chunk count".into());
    }
    let mut have = vec![false; chunks as usize];
    let mut drop = Vec::new();
    for c in ctx.db.offline_chunk().account().filter(&account) {
        if c.sync_id != sync_id {
            drop.push(c.id);
        } else if c.idx < chunks {
            have[c.idx as usize] = true;
        } else {
            drop.push(c.id);
        }
    }
    if let Some(missing) = have.iter().position(|h| !h) {
        return Err(format!("chunk {missing} of {chunks} is missing"));
    }
    for id in drop {
        ctx.db.offline_chunk().id().delete(id);
    }
    let row = OfflineAccount { account: account.clone(), sync_id, chunks, rows, source, updated: ctx.timestamp };
    if ctx.db.offline_account().account().find(account).is_some() {
        ctx.db.offline_account().account().update(row);
    } else {
        ctx.db.offline_account().insert(row);
    }
    Ok(())
}

/// One chunk of the account's committed snapshot, as
/// {"sync_id": "...", "chunks": N, "rows": R, "idx": i, "table": "...", "data": <chunk>}.
/// An account with nothing stored gives "".
#[procedure]
pub fn offline_pull(ctx: &mut ProcedureContext, account: String, idx: u32) -> Result<String, String> {
    let sender = ctx.sender();
    ctx.try_with_tx(|tx| {
        if !is_operator(&tx.db, sender) {
            return Err(format!("{sender} is not an operator of this database"));
        }
        let Some(acc) = tx.db.offline_account().account().find(account.clone()) else {
            return Ok(String::new());
        };
        if idx >= acc.chunks {
            return Err(format!("chunk {idx} is out of range ({} chunks)", acc.chunks));
        }
        let chunk = tx
            .db
            .offline_chunk()
            .account()
            .filter(&account)
            .find(|c| c.sync_id == acc.sync_id && c.idx == idx)
            .ok_or_else(|| format!("chunk {idx} is missing"))?;
        let data = if chunk.data.is_empty() { "null" } else { chunk.data.as_str() };
        Ok(format!(
            "{{\"sync_id\":\"{}\",\"chunks\":{},\"rows\":{},\"idx\":{},\"table\":\"{}\",\"data\":{}}}",
            acc.sync_id, acc.chunks, acc.rows, idx, chunk.table_name, data
        ))
    })
}
