//! bwlive: Bulwark Live as a SpacetimeDB module (docs/spec/live.md).
//!
//! It holds parties, invites, queues and matches, and runs the same matcher
//! as the standalone server (bwlive-core) in a scheduled reducer once a
//! second. SpacetimeDB can't send or receive raw UDP, so address reflection,
//! punching help and relaying come from `bwlive-server --relay-only`: a
//! client says hello to the relay first, then passes the `relay_id` and
//! public address it was given to the `hello` reducer here.
//!
//! A player is the SpacetimeDB identity calling the reducers (the client
//! keeps its token, like the standalone form's `live_identity.txt`). All
//! tables are private; a client reads what concerns it with the `state` and
//! `players` procedures, which return JSON. Original code, ISC licence.

use bwlive_core::{code, drop_in, match_tickets, party as rules, Open, Playlist, Ticket as CoreTicket};
use serde_json::{json, Value};
use spacetimedb::{
    procedure, reducer, table, Identity, ProcedureContext, ReducerContext, ScheduleAt, Table, TimeDuration, Timestamp,
};
use std::collections::HashMap;
use std::net::SocketAddr;
use std::time::Duration;

/// Players are dropped this long after their last `hello`.
const TIMEOUT: Duration = Duration::from_secs(20);
/// Invites are forgotten after this long.
const INVITE_LIFE: Duration = Duration::from_secs(300);
/// Matches nobody is linked to any more are deleted after this long.
const MATCH_LIFE: Duration = Duration::from_secs(3600);

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
    pub identity: Identity,
    /// The id the relay server gave this player; also their id to others.
    #[unique]
    pub relay_id: u32,
    pub game: String,
    pub name: String,
    pub status: String,
    /// "ip:port" as the relay server saw it.
    pub public: String,
    pub lan: Vec<String>,
    /// Split-screen guests on this console (at most 3).
    pub guests: Vec<String>,
    pub rtt_ms: u32,
    /// 0 = not in a party.
    pub party: u32,
    pub last_hello: Timestamp,
}

#[table(accessor = party)]
pub struct Party {
    #[primary_key]
    #[auto_inc]
    pub id: u32,
    #[unique]
    pub code: String,
    pub game: String,
    pub leader: Identity,
    pub max: u8,
    pub flags: u8,
    pub info: String,
    /// The match the party was last put in, 0 = none.
    pub matched: u64,
}

#[table(accessor = party_member)]
pub struct PartyMember {
    #[primary_key]
    pub identity: Identity,
    #[index(btree)]
    pub party: u32,
    /// Join order; the earliest present member leads when the leader leaves.
    pub order: u64,
}

#[table(accessor = party_invite)]
pub struct Invite {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    #[index(btree)]
    pub to: Identity,
    pub from: Identity,
    pub party: u32,
    pub at: Timestamp,
}

#[table(accessor = ticket)]
pub struct Ticket {
    #[primary_key]
    pub party: u32,
    pub playlist: String,
    pub seniority: u32,
    pub since: Timestamp,
}

#[table(accessor = game_match)]
pub struct GameMatch {
    #[primary_key]
    pub id: u64,
    pub playlist: String,
    pub host: Identity,
    pub made: Timestamp,
    /// The playlist's pick: game mode id, map and settings as a JSON
    /// object (all empty when the playlist has no modes).
    pub mode: String,
    pub map: String,
    pub settings: String,
    /// A drop-in match that still takes players (its host is still in it).
    pub drop_in: bool,
}

#[table(accessor = match_seat)]
pub struct MatchSeat {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    #[index(btree)]
    pub match_id: u64,
    pub identity: Identity,
    /// 0 = the console's own player, 1-3 = its guests.
    pub slot: u8,
    pub team: u8,
}

#[table(accessor = playlist)]
pub struct PlaylistRow {
    #[primary_key]
    pub id: String,
    /// The playlist as JSON (bwlive_core::Playlist).
    pub json: String,
}

#[table(accessor = recent_pair)]
pub struct RecentPair {
    /// The two identities as hex, smaller first, joined with '|'.
    #[primary_key]
    pub key: String,
    pub at: Timestamp,
}

#[table(accessor = counter)]
pub struct Counter {
    #[primary_key]
    pub name: String,
    pub value: u64,
}

#[table(accessor = matcher_tick, scheduled(tick))]
pub struct MatcherTick {
    #[primary_key]
    #[auto_inc]
    pub scheduled_id: u64,
    pub scheduled_at: ScheduleAt,
}

// --- helpers ------------------------------------------------------------------

type R = Result<(), String>;

fn since(now: Timestamp, then: Timestamp) -> Duration {
    now.duration_since(then).unwrap_or_default()
}

fn next(ctx: &ReducerContext, name: &str) -> u64 {
    let v = ctx.db.counter().name().find(name.to_string()).map(|c| c.value).unwrap_or(0) + 1;
    let row = Counter { name: name.into(), value: v };
    if v == 1 {
        ctx.db.counter().insert(row);
    } else {
        ctx.db.counter().name().update(row);
    }
    v
}

fn me(ctx: &ReducerContext) -> Result<Player, String> {
    ctx.db.player().identity().find(ctx.sender()).ok_or_else(|| "say hello first".to_string())
}

fn is_operator(ctx: &ReducerContext) -> bool {
    ctx.db.operator().identity().find(ctx.sender()).is_some()
}

fn pair_key(a: Identity, b: Identity) -> String {
    let (x, y) = (a.to_hex().to_string(), b.to_hex().to_string());
    if x < y { format!("{x}|{y}") } else { format!("{y}|{x}") }
}

fn playlists(db: &spacetimedb::Local) -> Vec<Playlist> {
    let mut v: Vec<Playlist> = db.playlist().iter().filter_map(|r| Playlist::from_json(&r.json).ok()).collect();
    v.sort_by(|a, b| a.id.cmp(&b.id));
    v
}

fn members_of(db: &spacetimedb::Local, party: u32) -> Vec<PartyMember> {
    let mut m: Vec<PartyMember> = db.party_member().party().filter(&party).collect();
    m.sort_by_key(|m| m.order);
    m
}

/// Local players on a console: its own player plus guests.
fn locals(db: &spacetimedb::Local, who: Identity) -> usize {
    db.player().identity().find(who).map(|p| 1 + p.guests.len()).unwrap_or(0)
}

/// Players in a party, counting split-screen guests.
fn seats(db: &spacetimedb::Local, party: u32) -> usize {
    db.party_member().party().filter(&party).map(|m| locals(db, m.identity)).sum()
}

fn cancel_ticket(ctx: &ReducerContext, party: u32) {
    ctx.db.ticket().party().delete(party);
}

/// Takes the caller (or anyone) out of their party; the party ends when empty.
fn leave(ctx: &ReducerContext, who: Identity) {
    let Some(mut p) = ctx.db.player().identity().find(who) else { return };
    let pid = p.party;
    if pid == 0 {
        return;
    }
    p.party = 0;
    ctx.db.player().identity().update(p);
    ctx.db.party_member().identity().delete(who);
    let rest = members_of(&ctx.db, pid);
    cancel_ticket(ctx, pid);
    if rest.is_empty() {
        ctx.db.party().id().delete(pid);
        let stale: Vec<u64> = ctx.db.party_invite().iter().filter(|i| i.party == pid).map(|i| i.id).collect();
        for id in stale {
            ctx.db.party_invite().id().delete(id);
        }
        return;
    }
    if let Some(mut pt) = ctx.db.party().id().find(pid) {
        if pt.leader == who {
            let order: Vec<(Identity, u64)> = rest.iter().map(|m| (m.identity, m.order)).collect();
            pt.leader = rules::next_leader(&order).unwrap();
            ctx.db.party().id().update(pt);
        }
    }
}

fn new_party(ctx: &ReducerContext, me: &Player, max: u8, flags: u8, info: &str) -> u32 {
    leave(ctx, me.identity);
    let code = loop {
        let c = code::from_bits(ctx.random::<u64>());
        if ctx.db.party().code().find(c.clone()).is_none() {
            break c;
        }
    };
    let pt = ctx.db.party().insert(Party {
        id: 0,
        code,
        game: me.game.clone(),
        leader: me.identity,
        max: rules::clamp_max(max),
        flags,
        info: rules::clip(info, rules::MAX_INFO),
        matched: 0,
    });
    let order = next(ctx, "join");
    ctx.db.party_member().insert(PartyMember { identity: me.identity, party: pt.id, order });
    let mut p = ctx.db.player().identity().find(me.identity).unwrap();
    p.party = pt.id;
    ctx.db.player().identity().update(p);
    pt.id
}

fn drop_player(ctx: &ReducerContext, who: Identity) {
    leave(ctx, who);
    ctx.db.player().identity().delete(who);
    let stale: Vec<u64> = ctx.db.party_invite().iter().filter(|i| i.to == who || i.from == who).map(|i| i.id).collect();
    for id in stale {
        ctx.db.party_invite().id().delete(id);
    }
}

fn check_addr(s: &str) -> Result<String, String> {
    s.parse::<SocketAddr>().map(|a| a.to_string()).map_err(|_| format!("bad address {s:?}"))
}

// --- lifecycle ------------------------------------------------------------------

#[reducer(init)]
pub fn init(ctx: &ReducerContext) {
    ctx.db.operator().insert(Operator { identity: ctx.sender(), note: "publisher".into(), added: ctx.timestamp });
    for pl in Playlist::defaults() {
        let json = serde_json::to_string(&pl).unwrap();
        ctx.db.playlist().insert(PlaylistRow { id: pl.id, json });
    }
    ctx.db.matcher_tick().insert(MatcherTick {
        scheduled_id: 0,
        scheduled_at: TimeDuration::from_micros(1_000_000).into(),
    });
}

// --- player reducers ------------------------------------------------------------

/// Registers or refreshes the caller. Call it every 5 s (it is the
/// keepalive). `relay_id` and `public` come from the relay server's
/// `welcome`; `lan` is the client's own LAN endpoints; `guests` names the
/// console's split-screen guests. A change in guests restarts any search.
#[reducer]
pub fn hello(
    ctx: &ReducerContext,
    game: String,
    name: String,
    relay_id: u32,
    public: String,
    lan: Vec<String>,
    guests: Vec<String>,
) -> R {
    let guests: Vec<String> = guests.iter().take(bwlive_core::wire::MAX_GUESTS).map(|g| rules::clean_name(g)).collect();
    if relay_id == 0 {
        return Err("relay_id comes from the relay server's welcome".into());
    }
    let public = check_addr(&public)?;
    let lan: Vec<String> = lan.iter().take(4).map(|a| check_addr(a)).collect::<Result<_, _>>()?;
    if let Some(other) = ctx.db.player().relay_id().find(relay_id) {
        if other.identity != ctx.sender() {
            if since(ctx.timestamp, other.last_hello) < TIMEOUT {
                return Err("that relay_id belongs to someone else".into());
            }
            drop_player(ctx, other.identity);
        }
    }
    let game = rules::clip(&game, 32);
    let name = rules::clean_name(&name);
    match ctx.db.player().identity().find(ctx.sender()) {
        Some(mut p) => {
            if p.game != game {
                leave(ctx, p.identity);
                p = ctx.db.player().identity().find(ctx.sender()).unwrap();
            }
            p.game = game;
            p.name = name;
            p.relay_id = relay_id;
            p.public = public;
            p.lan = lan;
            if p.guests != guests && p.party != 0 {
                cancel_ticket(ctx, p.party);
            }
            p.guests = guests;
            p.last_hello = ctx.timestamp;
            ctx.db.player().identity().update(p);
        }
        None => {
            ctx.db.player().insert(Player {
                identity: ctx.sender(),
                relay_id,
                game,
                name,
                status: String::new(),
                public,
                lan,
                guests,
                rtt_ms: 0,
                party: 0,
                last_hello: ctx.timestamp,
            });
        }
    }
    Ok(())
}

#[reducer]
pub fn bye(ctx: &ReducerContext) {
    drop_player(ctx, ctx.sender());
}

#[reducer]
pub fn set_status(ctx: &ReducerContext, status: String) -> R {
    let mut p = me(ctx)?;
    p.status = rules::clip(&status, rules::MAX_STATUS);
    ctx.db.player().identity().update(p);
    Ok(())
}

/// The client's own hello-to-welcome round trip to the relay, in ms.
#[reducer]
pub fn set_rtt(ctx: &ReducerContext, ms: u32) -> R {
    let mut p = me(ctx)?;
    p.rtt_ms = ms.min(5000);
    ctx.db.player().identity().update(p);
    Ok(())
}

// --- parties --------------------------------------------------------------------

#[reducer]
pub fn party_create(ctx: &ReducerContext, max: u8, flags: u8, info: String) -> R {
    let p = me(ctx)?;
    new_party(ctx, &p, max, flags, &info);
    Ok(())
}

#[reducer]
pub fn party_join(ctx: &ReducerContext, code: String) -> R {
    let p = me(ctx)?;
    let code = code::normalize(&code).ok_or("no such room code")?;
    let pt = ctx.db.party().code().find(code).ok_or("no such room code")?;
    if p.party == pt.id {
        return Ok(());
    }
    if pt.game != p.game {
        return Err("no such room code".into());
    }
    let invite = ctx.db.party_invite().to().filter(&p.identity).find(|i| i.party == pt.id);
    if pt.flags & rules::FLAG_JOINABLE == 0 && invite.is_none() {
        return Err("that party is invite-only".into());
    }
    if seats(&ctx.db, pt.id) + 1 + p.guests.len() > pt.max as usize {
        return Err("party full".into());
    }
    if let Some(i) = invite {
        ctx.db.party_invite().id().delete(i.id);
    }
    leave(ctx, p.identity);
    let order = next(ctx, "join");
    ctx.db.party_member().insert(PartyMember { identity: p.identity, party: pt.id, order });
    let mut p = ctx.db.player().identity().find(p.identity).unwrap();
    p.party = pt.id;
    ctx.db.player().identity().update(p);
    cancel_ticket(ctx, pt.id);
    Ok(())
}

#[reducer]
pub fn party_leave(ctx: &ReducerContext) -> R {
    me(ctx)?;
    leave(ctx, ctx.sender());
    Ok(())
}

#[reducer]
pub fn party_set(ctx: &ReducerContext, flags: u8, info: String) -> R {
    let p = me(ctx)?;
    let mut pt = ctx.db.party().id().find(p.party).ok_or("not in a party")?;
    if pt.leader != p.identity {
        return Err("only the party leader can do that".into());
    }
    pt.flags = flags;
    pt.info = rules::clip(&info, rules::MAX_INFO);
    ctx.db.party().id().update(pt);
    Ok(())
}

/// Invites a player by their id (relay_id).
#[reducer]
pub fn invite(ctx: &ReducerContext, player: u32) -> R {
    let p = me(ctx)?;
    if p.party == 0 {
        return Err("create a party first".into());
    }
    let target = ctx.db.player().relay_id().find(player).filter(|t| t.game == p.game && t.identity != p.identity);
    let target = target.ok_or("no such player")?;
    if ctx.db.party_invite().to().filter(&target.identity).any(|i| i.party == p.party) {
        return Ok(());
    }
    ctx.db.party_invite().insert(Invite { id: 0, to: target.identity, from: p.identity, party: p.party, at: ctx.timestamp });
    Ok(())
}

// --- queue ----------------------------------------------------------------------

#[reducer]
pub fn queue(ctx: &ReducerContext, playlist: String, seniority: u32) -> R {
    let p = me(ctx)?;
    let pl = playlists(&ctx.db)
        .into_iter()
        .find(|x| x.id == playlist && (x.game.is_empty() || x.game == p.game))
        .ok_or("no such playlist")?;
    let pid = if p.party == 0 { new_party(ctx, &p, pl.team_size.max(2), 0, "") } else { p.party };
    let mut pt = ctx.db.party().id().find(pid).unwrap();
    if pt.leader != p.identity {
        return Err("only the party leader can search".into());
    }
    if seats(&ctx.db, pid) > pl.team_size as usize {
        return Err("party too big for that playlist".into());
    }
    cancel_ticket(ctx, pid);
    ctx.db.ticket().insert(Ticket { party: pid, playlist: pl.id, seniority, since: ctx.timestamp });
    pt.matched = 0;
    ctx.db.party().id().update(pt);
    Ok(())
}

#[reducer]
pub fn queue_cancel(ctx: &ReducerContext) -> R {
    let p = me(ctx)?;
    cancel_ticket(ctx, p.party);
    Ok(())
}

#[reducer]
pub fn match_left(ctx: &ReducerContext, match_id: u64) -> R {
    let p = me(ctx)?;
    if let Some(mut pt) = ctx.db.party().id().find(p.party) {
        if pt.matched == match_id {
            pt.matched = 0;
            ctx.db.party().id().update(pt);
        }
    }
    Ok(())
}

// --- operators ------------------------------------------------------------------

#[reducer]
pub fn add_playlist(ctx: &ReducerContext, json: String) -> R {
    if !is_operator(ctx) {
        return Err("operators only".into());
    }
    let pl = Playlist::from_json(&json)?;
    let row = PlaylistRow { id: pl.id.clone(), json: serde_json::to_string(&pl).unwrap() };
    if ctx.db.playlist().id().find(pl.id.clone()).is_some() {
        ctx.db.playlist().id().update(row);
    } else {
        ctx.db.playlist().insert(row);
    }
    Ok(())
}

#[reducer]
pub fn remove_playlist(ctx: &ReducerContext, id: String) -> R {
    if !is_operator(ctx) {
        return Err("operators only".into());
    }
    let gone: Vec<u32> = ctx.db.ticket().iter().filter(|t| t.playlist == id).map(|t| t.party).collect();
    for p in gone {
        cancel_ticket(ctx, p);
    }
    ctx.db.playlist().id().delete(id);
    Ok(())
}

#[reducer]
pub fn add_operator(ctx: &ReducerContext, identity: Identity, note: String) -> R {
    if !is_operator(ctx) {
        return Err("operators only".into());
    }
    if ctx.db.operator().identity().find(identity).is_none() {
        ctx.db.operator().insert(Operator { identity, note, added: ctx.timestamp });
    }
    Ok(())
}

// --- the matcher ----------------------------------------------------------------

#[reducer]
pub fn tick(ctx: &ReducerContext, _arg: MatcherTick) -> R {
    if ctx.sender() != ctx.database_identity() {
        return Err("the matcher runs on its own".into());
    }
    let now = ctx.timestamp;

    let stale: Vec<Identity> =
        ctx.db.player().iter().filter(|p| since(now, p.last_hello) > TIMEOUT).map(|p| p.identity).collect();
    for who in stale {
        drop_player(ctx, who);
    }
    let old: Vec<u64> = ctx.db.party_invite().iter().filter(|i| since(now, i.at) > INVITE_LIFE).map(|i| i.id).collect();
    for id in old {
        ctx.db.party_invite().id().delete(id);
    }
    let pls = playlists(&ctx.db);
    let window = pls.iter().map(|p| p.avoid_rematch_minutes).max().unwrap_or(0) as u64 * 60;
    let old: Vec<String> =
        ctx.db.recent_pair().iter().filter(|r| since(now, r.at).as_secs() >= window).map(|r| r.key).collect();
    for k in old {
        ctx.db.recent_pair().key().delete(k);
    }
    let linked: Vec<u64> = ctx.db.party().iter().map(|p| p.matched).collect();
    let old: Vec<u64> = ctx
        .db
        .game_match()
        .iter()
        .filter(|m| since(now, m.made) > MATCH_LIFE && !linked.contains(&m.id))
        .map(|m| m.id)
        .collect();
    for id in old {
        let seats: Vec<u64> = ctx.db.match_seat().match_id().filter(&id).map(|s| s.id).collect();
        for s in seats {
            ctx.db.match_seat().id().delete(s);
        }
        ctx.db.game_match().id().delete(id);
    }

    tidy_drop_in(ctx);
    for pl in pls {
        run_matcher(ctx, &pl);
    }
    Ok(())
}

/// Frees the seats of players who left a drop-in match, and stops taking
/// players into one whose host has left.
fn tidy_drop_in(ctx: &ReducerContext) {
    let in_match = |who: Identity, id: u64| {
        ctx.db
            .player()
            .identity()
            .find(who)
            .and_then(|p| ctx.db.party().id().find(p.party))
            .is_some_and(|pt| pt.matched == id)
    };
    let running: Vec<GameMatch> = ctx.db.game_match().iter().filter(|m| m.drop_in).collect();
    for mut m in running {
        if !in_match(m.host, m.id) {
            m.drop_in = false;
            ctx.db.game_match().id().update(m);
            continue;
        }
        let gone: Vec<u64> =
            ctx.db.match_seat().match_id().filter(&m.id).filter(|s| !in_match(s.identity, m.id)).map(|s| s.id).collect();
        for s in gone {
            ctx.db.match_seat().id().delete(s);
        }
    }
}

fn run_matcher(ctx: &ReducerContext, pl: &Playlist) {
    let now = ctx.timestamp;
    // Seats are numbered for the matcher; `who` maps each back to its
    // console's identity and slot (guests get their own seats).
    let mut who: Vec<Identity> = Vec::new();
    let mut slot_of: Vec<u8> = Vec::new();
    let mut parties: Vec<u32> = Vec::new();
    let mut tickets: Vec<CoreTicket> = Vec::new();
    for t in ctx.db.ticket().iter().filter(|t| t.playlist == pl.id) {
        let Some(pt) = ctx.db.party().id().find(t.party) else { continue };
        let mut members = members_of(&ctx.db, t.party);
        members.sort_by_key(|m| (m.identity != pt.leader, m.order));
        let mut nums = Vec::new();
        let mut rtt = 0;
        for m in &members {
            let Some(p) = ctx.db.player().identity().find(m.identity) else { continue };
            rtt = rtt.max(p.rtt_ms);
            for slot in 0..=p.guests.len() {
                nums.push(who.len() as u64);
                who.push(m.identity);
                slot_of.push(slot as u8);
            }
        }
        parties.push(t.party);
        tickets.push(CoreTicket {
            key: t.party as u64,
            members: nums,
            seniority: t.seniority,
            rtt_ms: rtt,
            waited: since(now, t.since).as_secs() as u32,
        });
    }
    if tickets.len() < 1 {
        return;
    }
    if pl.drop_in {
        // join running matches with room first
        let running: Vec<GameMatch> = ctx.db.game_match().iter().filter(|m| m.drop_in && m.playlist == pl.id).collect();
        let open: Vec<Open> = running
            .iter()
            .map(|m| Open {
                key: m.id,
                free: (pl.team_size as usize).saturating_sub(ctx.db.match_seat().match_id().filter(&m.id).count()),
                rtt_ms: ctx.db.player().identity().find(m.host).map(|p| p.rtt_ms).unwrap_or(0),
            })
            .collect();
        let joins = drop_in(pl, &tickets, &open);
        for &(ti, oi) in &joins {
            let mid = running[oi].id;
            for &m in &tickets[ti].members {
                let (identity, slot) = (who[m as usize], slot_of[m as usize]);
                ctx.db.match_seat().insert(MatchSeat { id: 0, match_id: mid, identity, slot, team: 0 });
            }
            let pid = parties[ti];
            cancel_ticket(ctx, pid);
            if let Some(mut pt) = ctx.db.party().id().find(pid) {
                pt.matched = mid;
                ctx.db.party().id().update(pt);
            }
            log::info!("party {pid} dropped into match {mid}");
        }
        let joined: Vec<usize> = joins.into_iter().map(|(t, _)| t).collect();
        let keep: Vec<bool> = (0..tickets.len()).map(|i| !joined.contains(&i)).collect();
        let mut k = keep.iter();
        tickets.retain(|_| *k.next().unwrap());
        let mut k = keep.iter();
        parties.retain(|_| *k.next().unwrap());
        if tickets.is_empty() {
            return;
        }
    }
    let window = Duration::from_secs(pl.avoid_rematch_minutes as u64 * 60);
    let recent: HashMap<String, Timestamp> = ctx.db.recent_pair().iter().map(|r| (r.key, r.at)).collect();
    let was_recent = |a: u64, b: u64| {
        recent.get(&pair_key(who[a as usize], who[b as usize])).is_some_and(|at| since(now, *at) < window)
    };
    let formed = match_tickets(pl, &tickets, &was_recent);

    for f in formed {
        let id = loop {
            let id = ctx.random::<u64>() >> 11; // fits a JSON number exactly
            if id != 0 && ctx.db.game_match().id().find(id).is_none() {
                break id;
            }
        };
        // don't repeat the playlist's previous mode and map
        let last = ctx
            .db
            .game_match()
            .iter()
            .filter(|m| m.playlist == pl.id)
            .max_by_key(|m| m.made)
            .map(|m| (m.mode, m.map));
        let pick = pl.pick(ctx.random::<u64>(), last.as_ref().map(|(a, b)| (a.as_str(), b.as_str())));
        let settings: serde_json::Map<String, Value> =
            pick.settings.into_iter().map(|(k, v)| (k, Value::String(v))).collect();
        ctx.db.game_match().insert(GameMatch {
            id,
            playlist: pl.id.clone(),
            host: who[f.host as usize],
            made: now,
            mode: pick.mode,
            map: pick.map,
            settings: Value::Object(settings).to_string(),
            drop_in: pl.drop_in,
        });
        for (m, team) in &f.seats {
            let (identity, slot) = (who[*m as usize], slot_of[*m as usize]);
            ctx.db.match_seat().insert(MatchSeat { id: 0, match_id: id, identity, slot, team: *team });
        }
        for (i, &ti) in f.tickets.iter().enumerate() {
            for &tj in &f.tickets[i + 1..] {
                for &a in tickets[ti].members.iter().filter(|&&m| slot_of[m as usize] == 0) {
                    for &b in tickets[tj].members.iter().filter(|&&m| slot_of[m as usize] == 0) {
                        let key = pair_key(who[a as usize], who[b as usize]);
                        if ctx.db.recent_pair().key().find(key.clone()).is_some() {
                            ctx.db.recent_pair().key().update(RecentPair { key, at: now });
                        } else {
                            ctx.db.recent_pair().insert(RecentPair { key, at: now });
                        }
                    }
                }
            }
        }
        for &ti in &f.tickets {
            let pid = parties[ti];
            cancel_ticket(ctx, pid);
            if let Some(mut pt) = ctx.db.party().id().find(pid) {
                pt.matched = id;
                ctx.db.party().id().update(pt);
            }
        }
        log::info!("match {id} on {} with {} players", pl.id, f.seats.len());
    }
}

// --- reading --------------------------------------------------------------------

fn member_json(p: &Player) -> Value {
    json!({ "id": p.relay_id, "name": p.name, "public": p.public, "lan": p.lan, "guests": p.guests })
}

/// Everything about the caller as JSON:
/// `{ "id", "public", "party": {code, leader, max, flags, info, members:
/// [{id, name, public, lan, guests}]} | null, "invites": [{from, name,
/// code}], "queue": {playlist, stage, waited, searching} | null, "match":
/// {id, playlist, host, seats: [{id, slot, team, name, public, lan}], mode, map, settings} | null }`.
/// Ids are relay ids. Poll it about once a second while something is going on.
#[procedure]
pub fn state(ctx: &mut ProcedureContext) -> Result<String, String> {
    let sender = ctx.sender();
    let now = ctx.timestamp;
    ctx.try_with_tx(|tx| {
        let db = &tx.db;
        let me = db.player().identity().find(sender).ok_or("say hello first")?;
        let relay_of = |i: Identity| db.player().identity().find(i).map(|p| p.relay_id).unwrap_or(0);

        let mut out = json!({ "id": me.relay_id, "public": me.public, "party": null, "queue": null, "match": null });
        if let Some(pt) = db.party().id().find(me.party) {
            let members: Vec<Value> = members_of(db, pt.id)
                .iter()
                .filter_map(|m| db.player().identity().find(m.identity))
                .map(|p| member_json(&p))
                .collect();
            out["party"] = json!({
                "code": pt.code, "leader": relay_of(pt.leader), "max": pt.max,
                "flags": pt.flags, "info": pt.info, "members": members,
            });
            if let Some(t) = db.ticket().party().find(pt.id) {
                let waited = since(now, t.since).as_secs() as u32;
                let pl = playlists(db).into_iter().find(|p| p.id == t.playlist);
                let searching: usize = db
                    .ticket()
                    .iter()
                    .filter(|x| x.playlist == t.playlist)
                    .map(|x| seats(db, x.party))
                    .sum();
                out["queue"] = json!({
                    "playlist": t.playlist,
                    "stage": pl.map(|p| p.stage_index(waited)).unwrap_or(0),
                    "waited": waited, "searching": searching,
                });
            }
            if let Some(m) = db.game_match().id().find(pt.matched) {
                let seats: Vec<Value> = db
                    .match_seat()
                    .match_id()
                    .filter(&m.id)
                    .filter_map(|s| {
                        db.player().identity().find(s.identity).map(|p| {
                            let name = match s.slot {
                                0 => p.name.clone(),
                                n => p.guests.get(n as usize - 1).cloned().unwrap_or_default(),
                            };
                            json!({ "id": p.relay_id, "slot": s.slot, "team": s.team, "name": name,
                                    "public": p.public, "lan": p.lan })
                        })
                    })
                    .collect();
                let settings: Value = serde_json::from_str(&m.settings).unwrap_or(json!({}));
                out["match"] = json!({ "id": m.id, "playlist": m.playlist, "host": relay_of(m.host), "seats": seats,
                                       "mode": m.mode, "map": m.map, "settings": settings,
                                       "drop_in": m.drop_in });
            }
        }
        let invites: Vec<Value> = db
            .party_invite()
            .to()
            .filter(&sender)
            .filter_map(|i| {
                let from = db.player().identity().find(i.from)?;
                let pt = db.party().id().find(i.party)?;
                Some(json!({ "from": from.relay_id, "name": from.name, "code": pt.code }))
            })
            .collect();
        out["invites"] = json!(invites);
        Ok(out.to_string())
    })
}

/// Players of the caller's game: `[{id, name, status, code}]`, where code
/// is their party's room code if it's joinable (else "").
#[procedure]
pub fn players(ctx: &mut ProcedureContext) -> Result<String, String> {
    let sender = ctx.sender();
    ctx.try_with_tx(|tx| {
        let db = &tx.db;
        let me = db.player().identity().find(sender).ok_or("say hello first")?;
        let list: Vec<Value> = db
            .player()
            .iter()
            .filter(|p| p.game == me.game && p.identity != sender)
            .take(200)
            .map(|p| {
                let code = db
                    .party()
                    .id()
                    .find(p.party)
                    .filter(|pt| pt.flags & rules::FLAG_JOINABLE != 0)
                    .map(|pt| pt.code)
                    .unwrap_or_default();
                json!({ "id": p.relay_id, "name": p.name, "status": p.status, "code": code })
            })
            .collect();
        Ok(Value::Array(list).to_string())
    })
}

/// The playlists, as JSON, for menus.
#[procedure]
pub fn list_playlists(ctx: &mut ProcedureContext) -> Result<String, String> {
    ctx.try_with_tx(|tx| {
        let v: Vec<Value> = playlists(&tx.db)
            .iter()
            .map(|p| {
                let modes: Vec<Value> = p.modes.iter().map(|m| json!({ "id": m.id, "name": m.name, "maps": m.maps })).collect();
                json!({ "id": p.id, "name": p.name, "description": p.description, "game": p.game, "players": p.players(), "min_players": p.min_players(), "teams": p.teams, "modes": modes,
                        "drop_in": p.drop_in })
            })
            .collect();
        Ok::<String, String>(Value::Array(v).to_string())
    })
}
