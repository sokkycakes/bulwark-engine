//! csnz: the persistent backend of CSNZ servers on Bulwark, as a SpacetimeDB
//! module. Gameplay stays on the Bulwark server; its QC calls these reducers
//! and the get_profile procedure over SpacetimeDB's HTTP API
//! (POST /v1/database/<db>/call/<name>, a JSON array of arguments) with the
//! engine adding the server's token (stdb_url / stdb_token).
//!
//! Only operators may write: the identity that first published the module,
//! plus any identity an operator adds with add_operator (give each game
//! server its own). Player rows are private; game_server and the match
//! tables are public, so a server browser can read them without a token.
//!
//! Players are keyed by a string the server chooses (today the player's
//! name, as in the QC's local files). Player data goes in and out in the
//! QC's own line formats (see text.rs), stored here as typed rows.
//!
//! Original code, ISC licence.

pub mod lobby;
pub mod offline;
pub mod rooms;
pub mod text;

use spacetimedb::{
    procedure, reducer, table, Identity, ProcedureContext, ReducerContext, ScheduleAt, Table, TimeDuration, Timestamp,
};

/// Servers that haven't sent a heartbeat for this long are dropped.
const SERVER_TIMEOUT_SECS: i64 = 120;

#[table(accessor = operator)]
pub struct Operator {
    #[primary_key]
    pub identity: Identity,
    pub note: String,
    pub added: Timestamp,
}

#[table(accessor = player)]
pub struct Player {
    #[primary_key]
    pub key: String,
    pub display_name: String,
    pub created: Timestamp,
    pub last_seen: Timestamp,
    pub last_server: String,
}

#[table(accessor = progression)]
pub struct Progression {
    #[primary_key]
    pub key: String,
    pub level: u32,
    pub xp: f64,
    pub xp_total: f64,
    pub rank: u32,
    pub kills: u64,
    pub deaths: u64,
    pub wins: u64,
    pub matches: u64,
    pub playtime: f64,
    pub money: i64,
    pub updated: Timestamp,
}

#[table(accessor = inventory_item)]
pub struct InventoryItem {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    #[index(btree)]
    pub key: String,
    pub item: String,
}

#[table(accessor = loadout)]
pub struct Loadout {
    #[primary_key]
    pub key: String,
    pub character: String,
    pub primary: String,
    pub secondary: String,
}

#[table(accessor = achievement)]
pub struct Achievement {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    #[index(btree)]
    pub key: String,
    pub ach_id: u32,
    pub progress: f64,
    pub completed: bool,
}

#[table(accessor = achievement_counter)]
pub struct AchievementCounter {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    #[index(btree)]
    pub key: String,
    pub name: String,
    pub value: f64,
}

/// Per player and CSO mode id: what data/csnz/stats.txt holds locally.
#[table(accessor = mode_stats)]
pub struct ModeStats {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    #[index(btree)]
    pub key: String,
    pub mode: u32,
    pub matches: u64,
    pub wins: u64,
    pub kills: u64,
    pub deaths: u64,
}

#[table(accessor = game_server, public)]
pub struct GameServer {
    /// "host:port" as players would connect to it
    #[primary_key]
    pub address: String,
    pub name: String,
    pub map: String,
    pub mode: u32,
    pub players: u32,
    pub max_players: u32,
    pub version: String,
    pub started: Timestamp,
    pub last_heartbeat: Timestamp,
    pub operator: Identity,
}

#[table(accessor = match_record, public)]
pub struct MatchRecord {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    #[index(btree)]
    pub server: String,
    pub map: String,
    pub mode: u32,
    /// 1 or 2 for team modes, 0 for a draw or a free-for-all
    pub winner_team: u32,
    pub ended: Timestamp,
}

#[table(accessor = match_player, public)]
pub struct MatchPlayer {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    #[index(btree)]
    pub match_id: u64,
    #[index(btree)]
    pub key: String,
    pub team: u32,
    pub kills: u32,
    pub deaths: u32,
    pub won: bool,
}

#[table(accessor = server_cleanup, scheduled(drop_stale_servers))]
pub struct ServerCleanup {
    #[primary_key]
    #[auto_inc]
    pub scheduled_id: u64,
    pub scheduled_at: ScheduleAt,
}

// --- helpers ------------------------------------------------------------------

fn is_operator(db: &spacetimedb::Local, who: Identity) -> bool {
    db.operator().identity().find(who).is_some()
}

fn require_operator(ctx: &ReducerContext) -> Result<(), String> {
    if is_operator(&ctx.db, ctx.sender()) {
        Ok(())
    } else {
        Err(format!("{} is not an operator of this database", ctx.sender()))
    }
}

fn check_key(key: &str) -> Result<(), String> {
    if key.is_empty() || key.len() > 128 || key.contains(['\n', '\r', '\t']) {
        Err("bad player key".into())
    } else {
        Ok(())
    }
}

fn clamp_u32(v: f64) -> u32 {
    v.clamp(0.0, u32::MAX as f64) as u32
}

fn clamp_u64(v: f64) -> u64 {
    v.clamp(0.0, 9.0e15) as u64
}

fn touch_player(ctx: &ReducerContext, key: &str, display_name: Option<&str>, server: Option<&str>) {
    match ctx.db.player().key().find(key.to_string()) {
        Some(mut p) => {
            p.last_seen = ctx.timestamp;
            if let Some(n) = display_name {
                p.display_name = n.to_string();
            }
            if let Some(s) = server {
                p.last_server = s.to_string();
            }
            ctx.db.player().key().update(p);
        }
        None => {
            ctx.db.player().insert(Player {
                key: key.to_string(),
                display_name: display_name.unwrap_or(key).to_string(),
                created: ctx.timestamp,
                last_seen: ctx.timestamp,
                last_server: server.unwrap_or("").to_string(),
            });
        }
    }
}

// --- lifecycle and operators -----------------------------------------------------

#[reducer(init)]
pub fn init(ctx: &ReducerContext) {
    ctx.db.operator().insert(Operator { identity: ctx.sender(), note: "publisher".into(), added: ctx.timestamp });
    ctx.db.server_cleanup().insert(ServerCleanup {
        scheduled_id: 0,
        scheduled_at: TimeDuration::from_micros(30_000_000).into(),
    });
}

/// Adds an operator by identity (hex), e.g. a game server's own identity
/// from POST /v1/identity.
#[reducer]
pub fn add_operator(ctx: &ReducerContext, identity_hex: String, note: String) -> Result<(), String> {
    require_operator(ctx)?;
    let identity = Identity::from_hex(identity_hex.trim()).map_err(|e| format!("bad identity: {e}"))?;
    if ctx.db.operator().identity().find(identity).is_none() {
        ctx.db.operator().insert(Operator { identity, note, added: ctx.timestamp });
    }
    Ok(())
}

#[reducer]
pub fn remove_operator(ctx: &ReducerContext, identity_hex: String) -> Result<(), String> {
    require_operator(ctx)?;
    let identity = Identity::from_hex(identity_hex.trim()).map_err(|e| format!("bad identity: {e}"))?;
    if identity == ctx.sender() {
        return Err("an operator can't remove itself".into());
    }
    ctx.db.operator().identity().delete(identity);
    Ok(())
}

// --- players ----------------------------------------------------------------------

/// A player joined a server.
#[reducer]
pub fn player_seen(ctx: &ReducerContext, key: String, display_name: String, server: String) -> Result<(), String> {
    require_operator(ctx)?;
    check_key(&key)?;
    touch_player(ctx, &key, Some(&display_name), Some(&server));
    Ok(())
}

/// The progression file's text (csnz_progression.qc PROG_Save).
#[reducer]
pub fn save_progression(ctx: &ReducerContext, key: String, text: String) -> Result<(), String> {
    require_operator(ctx)?;
    check_key(&key)?;
    let p = text::Progression::parse(&text);
    let row = Progression {
        key: key.clone(),
        level: clamp_u32(p.level),
        xp: p.xp,
        xp_total: p.xp_total,
        rank: clamp_u32(p.rank),
        kills: clamp_u64(p.kills),
        deaths: clamp_u64(p.deaths),
        wins: clamp_u64(p.wins),
        matches: clamp_u64(p.matches),
        playtime: p.playtime.max(0.0),
        money: p.money.clamp(-9.0e15, 9.0e15) as i64,
        updated: ctx.timestamp,
    };
    if ctx.db.progression().key().find(key.clone()).is_some() {
        ctx.db.progression().key().update(row);
    } else {
        ctx.db.progression().insert(row);
    }
    touch_player(ctx, &key, None, None);
    Ok(())
}

/// The inventory file's text (csnz_inventory.qc INV_Save).
#[reducer]
pub fn save_inventory(ctx: &ReducerContext, key: String, text: String) -> Result<(), String> {
    require_operator(ctx)?;
    check_key(&key)?;
    let inv = text::Inventory::parse(&text);
    if inv.items.len() > 512 {
        return Err("more than 512 items".into());
    }
    let old: Vec<u64> = ctx.db.inventory_item().key().filter(&key).map(|r| r.id).collect();
    for id in old {
        ctx.db.inventory_item().id().delete(id);
    }
    for item in inv.items {
        ctx.db.inventory_item().insert(InventoryItem { id: 0, key: key.clone(), item });
    }
    let row = Loadout { key: key.clone(), character: inv.character, primary: inv.primary, secondary: inv.secondary };
    if ctx.db.loadout().key().find(key.clone()).is_some() {
        ctx.db.loadout().key().update(row);
    } else {
        ctx.db.loadout().insert(row);
    }
    touch_player(ctx, &key, None, None);
    Ok(())
}

/// The achievements file's text (csnz_achievements.qc ACH_Save).
#[reducer]
pub fn save_achievements(ctx: &ReducerContext, key: String, text: String) -> Result<(), String> {
    require_operator(ctx)?;
    check_key(&key)?;
    let a = text::Achievements::parse(&text);
    let old: Vec<u64> = ctx.db.achievement().key().filter(&key).map(|r| r.id).collect();
    for id in old {
        ctx.db.achievement().id().delete(id);
    }
    for (ach_id, progress, completed) in a.ach {
        ctx.db.achievement().insert(Achievement { id: 0, key: key.clone(), ach_id, progress, completed });
    }
    let old: Vec<u64> = ctx.db.achievement_counter().key().filter(&key).map(|r| r.id).collect();
    for id in old {
        ctx.db.achievement_counter().id().delete(id);
    }
    for (name, value) in a.counters {
        ctx.db.achievement_counter().insert(AchievementCounter { id: 0, key: key.clone(), name, value });
    }
    touch_player(ctx, &key, None, None);
    Ok(())
}

/// Everything stored for a player, in the QC's file formats, as sections:
///   [progression] ... [inventory] ... [achievements] ... [stats]
/// with "mode matches wins kills deaths" lines under [stats]. A section is
/// left out when nothing is stored for it, so the QC keeps its defaults
/// (or its local file). An unknown player gives "".
#[procedure]
pub fn get_profile(ctx: &mut ProcedureContext, key: String) -> Result<String, String> {
    let sender = ctx.sender();
    ctx.try_with_tx(|tx| {
        if !is_operator(&tx.db, sender) {
            return Err(format!("{sender} is not an operator of this database"));
        }
        let mut out = String::new();
        if let Some(p) = tx.db.progression().key().find(key.clone()) {
            let t = text::Progression {
                level: p.level as f64,
                xp: p.xp,
                xp_total: p.xp_total,
                rank: p.rank as f64,
                kills: p.kills as f64,
                deaths: p.deaths as f64,
                wins: p.wins as f64,
                matches: p.matches as f64,
                playtime: p.playtime,
                money: p.money as f64,
            };
            out += "[progression]\n";
            out += &t.write();
        }
        if let Some(l) = tx.db.loadout().key().find(key.clone()) {
            let items = tx.db.inventory_item().key().filter(&key).map(|r| r.item).collect();
            let t = text::Inventory { items, character: l.character, primary: l.primary, secondary: l.secondary };
            out += "[inventory]\n";
            out += &t.write();
        }
        let mut ach: Vec<(u32, f64, bool)> =
            tx.db.achievement().key().filter(&key).map(|r| (r.ach_id, r.progress, r.completed)).collect();
        let mut counters: Vec<(u64, String, f64)> =
            tx.db.achievement_counter().key().filter(&key).map(|r| (r.id, r.name, r.value)).collect();
        if !ach.is_empty() || !counters.is_empty() {
            ach.sort_by_key(|a| a.0);
            counters.sort_by_key(|c| c.0);
            let t = text::Achievements { ach, counters: counters.into_iter().map(|(_, n, v)| (n, v)).collect() };
            out += "[achievements]\n";
            out += &t.write();
        }
        let mut stats: Vec<ModeStats> = tx.db.mode_stats().key().filter(&key).collect();
        if !stats.is_empty() {
            stats.sort_by_key(|s| s.mode);
            out += "[stats]\n";
            for s in stats {
                out += &format!("mode {} {} {} {} {}\n", s.mode, s.matches, s.wins, s.kills, s.deaths);
            }
        }
        Ok(out)
    })
}

// --- matches -----------------------------------------------------------------------

/// A finished match. players: one line per (human) player,
/// "key TAB team TAB kills TAB deaths TAB won". With count_stats the
/// players' mode_stats rows are added to, as data/csnz/stats.txt is
/// locally (modes with cs_noStats pass false).
#[reducer]
pub fn record_match(
    ctx: &ReducerContext,
    server: String,
    map: String,
    mode: u32,
    winner_team: u32,
    count_stats: bool,
    players: String,
) -> Result<(), String> {
    require_operator(ctx)?;
    let list = text::match_players(&players);
    for p in &list {
        check_key(&p.key)?;
    }
    let m = ctx.db.match_record().insert(MatchRecord { id: 0, server, map, mode, winner_team, ended: ctx.timestamp });
    for p in list {
        ctx.db.match_player().insert(MatchPlayer {
            id: 0,
            match_id: m.id,
            key: p.key.clone(),
            team: p.team,
            kills: clamp_u32(p.kills),
            deaths: clamp_u32(p.deaths),
            won: p.won,
        });
        if !count_stats {
            continue;
        }
        let found = ctx.db.mode_stats().key().filter(&p.key).find(|s| s.mode == mode);
        match found {
            Some(mut s) => {
                s.matches += 1;
                s.wins += p.won as u64;
                s.kills += clamp_u64(p.kills);
                s.deaths += clamp_u64(p.deaths);
                ctx.db.mode_stats().id().update(s);
            }
            None => {
                ctx.db.mode_stats().insert(ModeStats {
                    id: 0,
                    key: p.key.clone(),
                    mode,
                    matches: 1,
                    wins: p.won as u64,
                    kills: clamp_u64(p.kills),
                    deaths: clamp_u64(p.deaths),
                });
            }
        }
        touch_player(ctx, &p.key, None, None);
    }
    Ok(())
}

// --- server list ---------------------------------------------------------------------

#[reducer]
pub fn server_heartbeat(
    ctx: &ReducerContext,
    address: String,
    name: String,
    map: String,
    mode: u32,
    players: u32,
    max_players: u32,
    version: String,
) -> Result<(), String> {
    require_operator(ctx)?;
    if address.is_empty() || address.len() > 128 {
        return Err("bad server address".into());
    }
    let started = match ctx.db.game_server().address().find(address.clone()) {
        Some(s) if s.operator != ctx.sender() => return Err("another operator owns that address".into()),
        Some(s) => s.started,
        None => ctx.timestamp,
    };
    let row = GameServer {
        address: address.clone(),
        name,
        map,
        mode,
        players,
        max_players,
        version,
        started,
        last_heartbeat: ctx.timestamp,
        operator: ctx.sender(),
    };
    if ctx.db.game_server().address().find(address).is_some() {
        ctx.db.game_server().address().update(row);
    } else {
        ctx.db.game_server().insert(row);
    }
    Ok(())
}

#[reducer]
pub fn server_shutdown(ctx: &ReducerContext, address: String) -> Result<(), String> {
    require_operator(ctx)?;
    ctx.db.game_server().address().delete(address);
    Ok(())
}

#[reducer]
pub fn drop_stale_servers(ctx: &ReducerContext, _arg: ServerCleanup) -> Result<(), String> {
    if ctx.sender() != ctx.database_identity() {
        return Err("only the scheduler runs this".into());
    }
    let cutoff = ctx.timestamp - TimeDuration::from_micros(SERVER_TIMEOUT_SECS * 1_000_000);
    let stale: Vec<String> =
        ctx.db.game_server().iter().filter(|s| s.last_heartbeat < cutoff).map(|s| s.address).collect();
    for a in stale {
        ctx.db.game_server().address().delete(a);
    }
    lobby::drop_stale_rooms(ctx, cutoff);
    rooms::drop_stale_peers(ctx);
    Ok(())
}
