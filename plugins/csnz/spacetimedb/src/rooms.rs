//! Shared rooms for the real CSNZ client. Each player's csnzserver.dll
//! (hosted by CSOLauncher) answers its own client and keeps the lobby here,
//! so every player sees the same channels and rooms. This database only
//! introduces players: a match runs straight between the games, whose
//! adapters punch a UDP path to each other with the endpoints they publish
//! in csnz_peer. No game traffic passes through here.
//!
//! Every write is made by an adapter for itself: a peer row belongs to the
//! identity that wrote it, a member row to its peer, a room to its host.
//! Adapters keep their own identity (the token SpacetimeDB issues on first
//! connect, saved locally), so none of this needs an operator token.
//!
//! Peers send peer_update at least every PEER_TIMEOUT_SECS / 4; a peer that
//! stops is dropped with its memberships, and its rooms change host or
//! close. Channels are the lobby's (lobby.rs).
//!
//! Original code, ISC licence.

use spacetimedb::{reducer, table, Identity, ReducerContext, Table, TimeDuration, Timestamp};

use crate::lobby::channel;

/// Peers without a peer_update for this long are dropped.
const PEER_TIMEOUT_SECS: i64 = 60;
const MAX_TEXT: usize = 64;
/// user_info / settings blobs, written by the adapter and read by adapters
const MAX_BLOB: usize = 8192;
const MAX_ROOM_PLAYERS: u32 = 32;

pub const ROOM_WAITING: u8 = 0;
pub const ROOM_PLAYING: u8 = 1;

/// One running adapter: who it is and where its UDP shim can be reached.
#[table(accessor = csnz_peer, public)]
pub struct CsnzPeer {
    #[primary_key]
    pub identity: Identity,
    /// FNV-1a 32 of the account name: the user id every adapter shows for
    /// this player, so ids from different PCs never collide
    #[unique]
    pub global_id: u32,
    pub account: String,
    pub name: String,
    /// the client's FullUserInfo fields, in the adapter's encoding
    pub user_info: String,
    /// as seen by STUN
    pub public_ip: String,
    pub public_port: u16,
    pub lan_ip: String,
    pub lan_port: u16,
    /// adapter's classification: 0 unknown, 1 open, 2 endpoint-independent
    /// NAT, 3 symmetric NAT (can't be punched)
    pub nat_type: u8,
    pub heartbeat: Timestamp,
}

#[table(accessor = csnz_room, public)]
pub struct CsnzRoom {
    #[primary_key]
    #[auto_inc]
    pub id: u32,
    #[index(btree)]
    pub channel_id: u32,
    /// the room number shown in the list, the lowest free one in the channel
    pub number: u32,
    pub host: Identity,
    pub title: String,
    pub mode: u32,
    pub map: u32,
    pub max_players: u32,
    pub password: bool,
    /// the rest of the room settings, in the adapter's encoding
    pub settings: String,
    pub status: u8,
    /// set by room_start; adapters put it in their punch probes
    pub punch_nonce: u64,
    pub created: Timestamp,
}

/// Room passwords, kept out of the public table.
#[table(accessor = csnz_room_secret)]
pub struct CsnzRoomSecret {
    #[primary_key]
    pub room_id: u32,
    pub password: String,
}

#[table(accessor = csnz_room_member, public)]
pub struct CsnzRoomMember {
    /// a peer is in at most one room
    #[primary_key]
    pub identity: Identity,
    #[index(btree)]
    pub room_id: u32,
    pub team: u8,
    pub ready: bool,
    pub joined: Timestamp,
}

fn fnv1a32(s: &str) -> u32 {
    let mut h: u32 = 0x811c_9dc5;
    for b in s.bytes() {
        h ^= b as u32;
        h = h.wrapping_mul(0x0100_0193);
    }
    // 0 means "no user" to the client
    if h == 0 {
        1
    } else {
        h
    }
}

fn check_text(what: &str, s: &str, max: usize) -> Result<(), String> {
    if s.len() > max || s.contains(['\n', '\r', '\t', '\0']) {
        Err(format!("bad {what}"))
    } else {
        Ok(())
    }
}

fn require_peer(ctx: &ReducerContext) -> Result<CsnzPeer, String> {
    ctx.db.csnz_peer().identity().find(ctx.sender()).ok_or_else(|| "call peer_update first".to_string())
}

fn lowest_free_number(ctx: &ReducerContext, channel_id: u32) -> u32 {
    let mut used: Vec<u32> = ctx.db.csnz_room().channel_id().filter(channel_id).map(|r| r.number).collect();
    used.sort_unstable();
    let mut n = 1;
    for u in used {
        if u == n {
            n += 1;
        } else if u > n {
            break;
        }
    }
    n
}

/// Takes `who` out of its room; the room passes to the earliest member left,
/// or closes when nobody is left.
fn leave_room(ctx: &ReducerContext, who: Identity) {
    let Some(m) = ctx.db.csnz_room_member().identity().find(who) else {
        return;
    };
    ctx.db.csnz_room_member().identity().delete(who);
    let Some(mut room) = ctx.db.csnz_room().id().find(m.room_id) else {
        return;
    };
    if room.host != who {
        return;
    }
    match ctx.db.csnz_room_member().room_id().filter(room.id).min_by_key(|m| m.joined) {
        Some(next) => {
            room.host = next.identity;
            ctx.db.csnz_room().id().update(room);
        }
        None => {
            ctx.db.csnz_room_secret().room_id().delete(room.id);
            ctx.db.csnz_room().id().delete(room.id);
        }
    }
}

fn hosted_room(ctx: &ReducerContext) -> Result<CsnzRoom, String> {
    let m = ctx.db.csnz_room_member().identity().find(ctx.sender()).ok_or("not in a room")?;
    let room = ctx.db.csnz_room().id().find(m.room_id).ok_or("no such room")?;
    if room.host != ctx.sender() {
        return Err("only the room's host can do that".into());
    }
    Ok(room)
}

/// Publishes or refreshes the caller's peer row; also its heartbeat.
#[reducer]
#[allow(clippy::too_many_arguments)]
pub fn peer_update(
    ctx: &ReducerContext,
    account: String,
    name: String,
    user_info: String,
    public_ip: String,
    public_port: u16,
    lan_ip: String,
    lan_port: u16,
    nat_type: u8,
) -> Result<(), String> {
    if account.is_empty() {
        return Err("bad account".into());
    }
    check_text("account", &account, MAX_TEXT)?;
    check_text("name", &name, MAX_TEXT)?;
    check_text("user info", &user_info, MAX_BLOB)?;
    check_text("public ip", &public_ip, MAX_TEXT)?;
    check_text("lan ip", &lan_ip, MAX_TEXT)?;
    let global_id = fnv1a32(&account);
    if let Some(other) = ctx.db.csnz_peer().global_id().find(global_id) {
        if other.identity != ctx.sender() {
            return Err(format!("account {account} is already online"));
        }
    }
    let row = CsnzPeer {
        identity: ctx.sender(),
        global_id,
        account,
        name,
        user_info,
        public_ip,
        public_port,
        lan_ip,
        lan_port,
        nat_type,
        heartbeat: ctx.timestamp,
    };
    if ctx.db.csnz_peer().identity().find(ctx.sender()).is_some() {
        ctx.db.csnz_peer().identity().update(row);
    } else {
        ctx.db.csnz_peer().insert(row);
    }
    Ok(())
}

/// The adapter is going away: drops its peer row and leaves its room.
#[reducer]
pub fn peer_leave(ctx: &ReducerContext) -> Result<(), String> {
    leave_room(ctx, ctx.sender());
    ctx.db.csnz_peer().identity().delete(ctx.sender());
    Ok(())
}

/// Opens a room in a channel with the caller as host (leaving any room it
/// was in). An empty password means none.
#[reducer]
#[allow(clippy::too_many_arguments)]
pub fn room_open(
    ctx: &ReducerContext,
    channel_id: u32,
    title: String,
    mode: u32,
    map: u32,
    max_players: u32,
    password: String,
    settings: String,
) -> Result<(), String> {
    require_peer(ctx)?;
    if ctx.db.channel().id().find(channel_id).is_none() {
        return Err("no such channel".into());
    }
    check_text("title", &title, MAX_TEXT)?;
    check_text("password", &password, MAX_TEXT)?;
    check_text("settings", &settings, MAX_BLOB)?;
    if max_players == 0 || max_players > MAX_ROOM_PLAYERS {
        return Err("bad max players".into());
    }
    leave_room(ctx, ctx.sender());
    let room = ctx.db.csnz_room().insert(CsnzRoom {
        id: 0,
        channel_id,
        number: lowest_free_number(ctx, channel_id),
        host: ctx.sender(),
        title,
        mode,
        map,
        max_players,
        password: !password.is_empty(),
        settings,
        status: ROOM_WAITING,
        punch_nonce: 0,
        created: ctx.timestamp,
    });
    if !password.is_empty() {
        ctx.db.csnz_room_secret().insert(CsnzRoomSecret { room_id: room.id, password });
    }
    ctx.db.csnz_room_member().insert(CsnzRoomMember {
        identity: ctx.sender(),
        room_id: room.id,
        team: 0,
        ready: false,
        joined: ctx.timestamp,
    });
    Ok(())
}

/// The host changes its room. `password` None keeps the current one, "" clears it.
#[reducer]
pub fn room_update(
    ctx: &ReducerContext,
    title: String,
    mode: u32,
    map: u32,
    max_players: u32,
    password: Option<String>,
    settings: String,
) -> Result<(), String> {
    let mut room = hosted_room(ctx)?;
    check_text("title", &title, MAX_TEXT)?;
    check_text("settings", &settings, MAX_BLOB)?;
    let members = ctx.db.csnz_room_member().room_id().filter(room.id).count() as u32;
    if max_players == 0 || max_players > MAX_ROOM_PLAYERS || max_players < members {
        return Err("bad max players".into());
    }
    if let Some(pw) = password {
        check_text("password", &pw, MAX_TEXT)?;
        ctx.db.csnz_room_secret().room_id().delete(room.id);
        room.password = !pw.is_empty();
        if !pw.is_empty() {
            ctx.db.csnz_room_secret().insert(CsnzRoomSecret { room_id: room.id, password: pw });
        }
    }
    room.title = title;
    room.mode = mode;
    room.map = map;
    room.max_players = max_players;
    room.settings = settings;
    ctx.db.csnz_room().id().update(room);
    Ok(())
}

/// Joins a room (leaving any other). Joining a room that is playing is
/// allowed; the adapter then punches to the host with the room's nonce.
#[reducer]
pub fn room_join(ctx: &ReducerContext, room_id: u32, password: String) -> Result<(), String> {
    require_peer(ctx)?;
    let room = ctx.db.csnz_room().id().find(room_id).ok_or("no such room")?;
    if let Some(m) = ctx.db.csnz_room_member().identity().find(ctx.sender()) {
        if m.room_id == room_id {
            return Ok(());
        }
    }
    if room.password {
        let ok = ctx.db.csnz_room_secret().room_id().find(room_id).is_some_and(|s| s.password == password);
        if !ok {
            return Err("wrong password".into());
        }
    }
    if ctx.db.csnz_room_member().room_id().filter(room_id).count() as u32 >= room.max_players {
        return Err("the room is full".into());
    }
    leave_room(ctx, ctx.sender());
    ctx.db.csnz_room_member().insert(CsnzRoomMember {
        identity: ctx.sender(),
        room_id,
        team: 0,
        ready: false,
        joined: ctx.timestamp,
    });
    Ok(())
}

#[reducer]
pub fn room_leave(ctx: &ReducerContext) -> Result<(), String> {
    leave_room(ctx, ctx.sender());
    Ok(())
}

/// The caller's own team and ready state in its room.
#[reducer]
pub fn member_update(ctx: &ReducerContext, team: u8, ready: bool) -> Result<(), String> {
    let mut m = ctx.db.csnz_room_member().identity().find(ctx.sender()).ok_or("not in a room")?;
    m.team = team;
    m.ready = ready;
    ctx.db.csnz_room_member().identity().update(m);
    Ok(())
}

/// The host starts the match. `nonce` is the adapter's random punch nonce.
#[reducer]
pub fn room_start(ctx: &ReducerContext, nonce: u64) -> Result<(), String> {
    let mut room = hosted_room(ctx)?;
    if nonce == 0 {
        return Err("bad nonce".into());
    }
    room.status = ROOM_PLAYING;
    room.punch_nonce = nonce;
    ctx.db.csnz_room().id().update(room);
    Ok(())
}

/// The host's match is over; the room waits again.
#[reducer]
pub fn room_end(ctx: &ReducerContext) -> Result<(), String> {
    let mut room = hosted_room(ctx)?;
    room.status = ROOM_WAITING;
    room.punch_nonce = 0;
    ctx.db.csnz_room().id().update(room);
    Ok(())
}

/// Called from the module's cleanup schedule.
pub fn drop_stale_peers(ctx: &ReducerContext) {
    let cutoff = ctx.timestamp - TimeDuration::from_micros(PEER_TIMEOUT_SECS * 1_000_000);
    let stale: Vec<Identity> = ctx.db.csnz_peer().iter().filter(|p| p.heartbeat < cutoff).map(|p| p.identity).collect();
    for who in stale {
        leave_room(ctx, who);
        ctx.db.csnz_peer().identity().delete(who);
    }
    // members whose peer row is gone (shouldn't happen, but never leave a ghost)
    let orphans: Vec<Identity> = ctx
        .db
        .csnz_room_member()
        .iter()
        .filter(|m| ctx.db.csnz_peer().identity().find(m.identity).is_none())
        .map(|m| m.identity)
        .collect();
    for who in orphans {
        leave_room(ctx, who);
    }
}
