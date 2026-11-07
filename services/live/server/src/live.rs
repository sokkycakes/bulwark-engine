//! All server state and message handling.

use bwlive_core::wire::{self, err, gone, t, Client, Listed, Member, Seat, Server, MAX_GUESTS, STAGE_NONE};
use bwlive_core::{code, match_tickets, party, Playlist, Ticket};
use std::collections::{HashMap, HashSet};
use std::net::SocketAddr;
use std::time::{Duration, Instant};

/// Seconds between `hello`s that the server asks for.
pub const HELLO_INTERVAL: u16 = 5;
/// A player who hasn't sent `hello` for this long is dropped.
pub const TIMEOUT: Duration = Duration::from_secs(20);
/// `hello` must be padded to at least this many bytes, so the server's
/// answer is never bigger than the question (no amplification).
pub const MIN_HELLO: usize = 64;

#[derive(Clone, Debug)]
pub struct Config {
    pub relay_only: bool,
    pub max_players: usize,
    /// Relay bytes per second per player, and the burst (seconds' worth).
    pub relay_rate: f64,
    pub relay_burst_secs: f64,
    /// Control messages per second per player.
    pub msg_rate: f64,
    pub playlists: Vec<Playlist>,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            relay_only: false,
            max_players: 4096,
            relay_rate: 64.0 * 1024.0,
            relay_burst_secs: 4.0,
            msg_rate: 20.0,
            playlists: Playlist::defaults(),
        }
    }
}

/// One datagram to send.
pub type Out = (SocketAddr, Vec<u8>);

struct Bucket {
    level: f64,
    rate: f64,
    burst: f64,
    at: Instant,
}

impl Bucket {
    fn new(rate: f64, burst: f64, now: Instant) -> Self {
        Bucket { level: burst, rate, burst, at: now }
    }
    fn take(&mut self, n: f64, now: Instant) -> bool {
        let dt = now.saturating_duration_since(self.at).as_secs_f64();
        self.at = now;
        self.level = (self.level + dt * self.rate).min(self.burst);
        if self.level >= n {
            self.level -= n;
            true
        } else {
            false
        }
    }
}

struct Player {
    token: u64,
    addr: SocketAddr,
    game: String,
    name: String,
    lan: Vec<SocketAddr>,
    /// Split-screen guests on this console.
    guests: Vec<String>,
    status: String,
    last_hello: Instant,
    party: Option<u32>,
    rtt_ms: u32,
    ping: Option<(u32, Instant)>,
    relay: Bucket,
    msgs: Bucket,
    rate_warned: bool,
    introduced: HashSet<u32>,
    relayed_bytes: u64,
}

struct Queued {
    playlist: String,
    seniority: u32,
    since: Instant,
}

struct Party {
    code: String,
    game: String,
    leader: u32,
    max: u8,
    flags: u8,
    info: String,
    /// (player, join order)
    members: Vec<(u32, u64)>,
    invited: HashSet<u32>,
    ticket: Option<Queued>,
    matched: Option<u64>,
}

#[derive(Default, Clone, Debug)]
pub struct Stats {
    pub players: usize,
    pub parties: usize,
    pub queued_parties: usize,
    pub matches_made: u64,
    pub relayed_bytes: u64,
    pub relay_dropped: u64,
    pub datagrams_in: u64,
}

pub struct Live {
    cfg: Config,
    players: HashMap<u32, Player>,
    by_addr: HashMap<SocketAddr, u32>,
    by_token: HashMap<u64, u32>,
    parties: HashMap<u32, Party>,
    codes: HashMap<String, u32>,
    /// Recent pairings by token pair (smaller first), for rematch avoidance.
    recent: HashMap<(u64, u64), Instant>,
    /// Each playlist's last (mode, map), so the next match plays something else.
    last_pick: HashMap<String, (String, String)>,
    rng: u64,
    order: u64,
    next_party: u32,
    out: Vec<Out>,
    pub stats: Stats,
}

impl Live {
    pub fn new(cfg: Config, seed: u64) -> Self {
        Live {
            cfg,
            players: HashMap::new(),
            by_addr: HashMap::new(),
            by_token: HashMap::new(),
            parties: HashMap::new(),
            codes: HashMap::new(),
            recent: HashMap::new(),
            last_pick: HashMap::new(),
            rng: seed | 1,
            order: 0,
            next_party: 1,
            out: Vec::new(),
            stats: Stats::default(),
        }
    }

    fn rand(&mut self) -> u64 {
        // xorshift64*: enough for ids, codes and nonces (not secrets).
        let mut x = self.rng;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.rng = x;
        x.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }

    fn send(&mut self, to: u32, msg: Server) {
        if let Some(p) = self.players.get(&to) {
            self.out.push((p.addr, msg.encode()));
        }
    }

    fn error(&mut self, to: u32, code: u8, text: &str) {
        self.send(to, Server::Error { code, text: text.into() });
    }

    /// Handles one datagram; returns what to send.
    pub fn handle(&mut self, from: SocketAddr, data: &[u8], now: Instant) -> Vec<Out> {
        self.stats.datagrams_in += 1;
        if data.len() <= wire::MAX_CARRY {
            if let Some((ver, ty, body)) = wire::header(data) {
                self.dispatch(from, ver, ty, body, data.len(), now);
            }
        }
        std::mem::take(&mut self.out)
    }

    fn dispatch(&mut self, from: SocketAddr, ver: u8, ty: u8, body: &[u8], len: usize, now: Instant) {
        let id = self.by_addr.get(&from).copied();
        if ver != wire::VERSION {
            // Only answer someone we know, or a full-size hello.
            if id.is_some() || (ty == t::HELLO && len >= MIN_HELLO) {
                let msg = Server::Error { code: err::VERSION, text: "wrong protocol version".into() };
                self.out.push((from, msg.encode()));
            }
            return;
        }
        let Some(msg) = Client::decode(ty, body) else { return };

        if let Client::Hello { token, game, name, lan, guests } = msg {
            if len >= MIN_HELLO {
                self.hello(from, token, &game, &name, lan, guests, now);
            }
            return;
        }
        // Anything else needs a hello first. Unknown addresses get no answer.
        let Some(id) = id else { return };

        let is_relay = matches!(msg, Client::Relay { .. });
        if !is_relay {
            let p = self.players.get_mut(&id).unwrap();
            if !p.msgs.take(1.0, now) {
                if !p.rate_warned {
                    p.rate_warned = true;
                    self.error(id, err::RATE, "slow down");
                }
                return;
            }
            p.rate_warned = false;
        }

        if self.cfg.relay_only
            && !matches!(msg, Client::Bye | Client::Relay { .. } | Client::Pong { .. } | Client::Status { .. })
        {
            self.error(id, err::BAD, "this is a relay-only server");
            return;
        }

        match msg {
            Client::Hello { .. } => unreachable!(),
            Client::Bye => self.drop_player(id),
            Client::PartyCreate { max, flags, info } => self.party_create(id, max, flags, &info),
            Client::PartyJoin { code } => self.party_join(id, &code),
            Client::PartyLeave => {
                self.leave_party(id, gone::LEFT);
            }
            Client::PartySet { flags, info } => self.party_set(id, flags, &info),
            Client::Invite { player } => self.invite(id, player),
            Client::Queue { playlist, seniority } => self.queue(id, &playlist, seniority, now),
            Client::QueueCancel => self.queue_cancel(id),
            Client::Relay { to, payload } => self.relay(id, to, payload, now),
            Client::Status { status } => {
                let p = self.players.get_mut(&id).unwrap();
                p.status = party::clip(&status, party::MAX_STATUS);
            }
            Client::List => self.list(id),
            Client::MatchLeft { id: m } => {
                if let Some(pid) = self.players[&id].party {
                    let pt = self.parties.get_mut(&pid).unwrap();
                    if pt.matched == Some(m) {
                        pt.matched = None;
                    }
                }
            }
            Client::Pong { nonce } => {
                let p = self.players.get_mut(&id).unwrap();
                if let Some((n, sent)) = p.ping {
                    if n == nonce {
                        let ms = now.saturating_duration_since(sent).as_millis().min(5000) as u32;
                        p.rtt_ms = if p.rtt_ms == 0 { ms } else { (p.rtt_ms * 3 + ms) / 4 };
                        p.ping = None;
                    }
                }
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn hello(
        &mut self,
        from: SocketAddr,
        token: u64,
        game: &str,
        name: &str,
        lan: Vec<SocketAddr>,
        guests: Vec<String>,
        now: Instant,
    ) {
        let id = match self.by_addr.get(&from).copied() {
            Some(id) if self.players[&id].token == token => id,
            other => {
                // Same token from a new address: the player's NAT rebound
                // or the game restarted. Move them; keep their party.
                if let Some(old) = other {
                    // A different token on a known address: the old player
                    // went away without saying bye.
                    self.drop_player(old);
                }
                if let Some(&id) = self.by_token.get(&token) {
                    let p = self.players.get_mut(&id).unwrap();
                    self.by_addr.remove(&p.addr);
                    p.addr = from;
                    p.introduced.clear();
                    self.by_addr.insert(from, id);
                    id
                } else {
                    if self.players.len() >= self.cfg.max_players {
                        let msg = Server::Error { code: err::FULL, text: "server full".into() };
                        self.out.push((from, msg.encode()));
                        return;
                    }
                    let id = loop {
                        let id = self.rand() as u32;
                        if id != 0 && !self.players.contains_key(&id) {
                            break id;
                        }
                    };
                    let burst = self.cfg.relay_rate * self.cfg.relay_burst_secs;
                    self.players.insert(
                        id,
                        Player {
                            token,
                            addr: from,
                            game: String::new(),
                            name: String::new(),
                            lan: Vec::new(),
                            guests: Vec::new(),
                            status: String::new(),
                            last_hello: now,
                            party: None,
                            rtt_ms: 0,
                            ping: None,
                            relay: Bucket::new(self.cfg.relay_rate, burst, now),
                            msgs: Bucket::new(self.cfg.msg_rate, self.cfg.msg_rate * 2.0, now),
                            rate_warned: false,
                            introduced: HashSet::new(),
                            relayed_bytes: 0,
                        },
                    );
                    self.by_addr.insert(from, id);
                    self.by_token.insert(token, id);
                    id
                }
            }
        };
        let nonce = self.rand() as u32;
        let p = self.players.get_mut(&id).unwrap();
        p.last_hello = now;
        p.game = party::clip(game, 32);
        p.name = party::clean_name(name);
        p.lan = lan.into_iter().take(4).collect();
        let guests: Vec<String> = guests.iter().take(MAX_GUESTS).map(|g| party::clean_name(g)).collect();
        let changed = p.guests != guests;
        p.guests = guests;
        let pid = p.party;
        p.ping = Some((nonce, now));
        self.send(id, Server::Welcome { id, public: from, interval: HELLO_INTERVAL });
        self.send(id, Server::Ping { nonce });
        // A guest joined or left the console: the party's seat count
        // changed, so any search starts over.
        if let (true, Some(pid)) = (changed, pid) {
            self.cancel_ticket(pid);
            self.broadcast_party(pid);
        }
    }

    /// Local players on a console: its own player plus guests.
    fn locals(&self, id: u32) -> usize {
        self.players.get(&id).map(|p| 1 + p.guests.len()).unwrap_or(0)
    }

    /// Players in a party, counting split-screen guests.
    fn seats(&self, pid: u32) -> usize {
        self.parties[&pid].members.iter().map(|m| self.locals(m.0)).sum()
    }

    /// Name of a seat: slot 0 is the console's player, then its guests.
    fn seat_name(&self, id: u32, slot: u8) -> String {
        let Some(p) = self.players.get(&id) else { return String::new() };
        match slot {
            0 => p.name.clone(),
            n => p.guests.get(n as usize - 1).cloned().unwrap_or_default(),
        }
    }

    /// Removes a player entirely (bye or timeout).
    fn drop_player(&mut self, id: u32) {
        self.leave_party(id, gone::LEFT);
        if let Some(p) = self.players.remove(&id) {
            self.by_addr.remove(&p.addr);
            self.by_token.remove(&p.token);
        }
        for p in self.players.values_mut() {
            p.introduced.remove(&id);
        }
    }

    fn party_msg(&self, pid: u32) -> Server {
        let pt = &self.parties[&pid];
        let members = pt
            .members
            .iter()
            .filter_map(|(m, _)| self.players.get(m).map(|p| (m, p)))
            .map(|(m, p)| Member { id: *m, name: p.name.clone(), public: p.addr, lan: p.lan.clone(), guests: p.guests.clone() })
            .collect();
        Server::Party {
            code: pt.code.clone(),
            leader: pt.leader,
            max: pt.max,
            flags: pt.flags,
            info: pt.info.clone(),
            members,
        }
    }

    fn broadcast_party(&mut self, pid: u32) {
        let msg = self.party_msg(pid).encode();
        for (m, _) in &self.parties[&pid].members {
            if let Some(p) = self.players.get(m) {
                self.out.push((p.addr, msg.clone()));
            }
        }
    }

    fn new_party(&mut self, leader: u32, max: u8, flags: u8, info: &str) -> u32 {
        self.leave_party(leader, gone::LEFT);
        let code = loop {
            let c = code::from_bits(self.rand());
            if !self.codes.contains_key(&c) {
                break c;
            }
        };
        let pid = self.next_party;
        self.next_party = self.next_party.wrapping_add(1).max(1);
        self.order += 1;
        let game = self.players[&leader].game.clone();
        self.parties.insert(
            pid,
            Party {
                code: code.clone(),
                game,
                leader,
                max: party::clamp_max(max),
                flags,
                info: party::clip(info, party::MAX_INFO),
                members: vec![(leader, self.order)],
                invited: HashSet::new(),
                ticket: None,
                matched: None,
            },
        );
        self.codes.insert(code, pid);
        self.players.get_mut(&leader).unwrap().party = Some(pid);
        pid
    }

    fn party_create(&mut self, id: u32, max: u8, flags: u8, info: &str) {
        let pid = self.new_party(id, max, flags, info);
        self.broadcast_party(pid);
    }

    fn party_join(&mut self, id: u32, typed: &str) {
        let Some(c) = code::normalize(typed) else {
            return self.error(id, err::NO_CODE, "no such room code");
        };
        let Some(&pid) = self.codes.get(&c) else {
            return self.error(id, err::NO_CODE, "no such room code");
        };
        if self.players[&id].party == Some(pid) {
            return self.send(id, self.party_msg(pid));
        }
        let pt = &self.parties[&pid];
        if pt.game != self.players[&id].game {
            return self.error(id, err::NO_CODE, "no such room code");
        }
        if pt.flags & party::FLAG_JOINABLE == 0 && !pt.invited.contains(&id) {
            return self.error(id, err::NO_CODE, "that party is invite-only");
        }
        if self.seats(pid) + self.locals(id) > pt.max as usize {
            return self.error(id, err::PARTY_FULL, "party full");
        }
        self.leave_party(id, gone::LEFT);
        self.order += 1;
        let order = self.order;
        let pt = self.parties.get_mut(&pid).unwrap();
        pt.invited.remove(&id);
        let others: Vec<u32> = pt.members.iter().map(|m| m.0).collect();
        pt.members.push((id, order));
        self.players.get_mut(&id).unwrap().party = Some(pid);
        self.cancel_ticket(pid);
        self.broadcast_party(pid);
        for o in others {
            self.introduce(id, o);
        }
    }

    /// Takes a player out of their party, if any. The party ends when empty.
    fn leave_party(&mut self, id: u32, reason: u8) {
        let Some(pid) = self.players.get_mut(&id).and_then(|p| p.party.take()) else { return };
        self.send(id, Server::PartyGone { reason });
        let pt = self.parties.get_mut(&pid).unwrap();
        pt.members.retain(|m| m.0 != id);
        if pt.members.is_empty() {
            let pt = self.parties.remove(&pid).unwrap();
            self.codes.remove(&pt.code);
            return;
        }
        if pt.leader == id {
            pt.leader = party::next_leader(&pt.members).unwrap();
        }
        self.cancel_ticket(pid);
        self.broadcast_party(pid);
    }

    fn party_set(&mut self, id: u32, flags: u8, info: &str) {
        let Some(pid) = self.players[&id].party else {
            return self.error(id, err::BAD, "not in a party");
        };
        let pt = self.parties.get_mut(&pid).unwrap();
        if pt.leader != id {
            return self.error(id, err::NOT_LEADER, "only the party leader can do that");
        }
        pt.flags = flags;
        pt.info = party::clip(info, party::MAX_INFO);
        self.broadcast_party(pid);
    }

    fn invite(&mut self, id: u32, target: u32) {
        let Some(pid) = self.players[&id].party else {
            return self.error(id, err::BAD, "create a party first");
        };
        let same_game = self.players.get(&target).map(|p| p.game == self.players[&id].game);
        if same_game != Some(true) || target == id {
            return self.error(id, err::BAD, "no such player");
        }
        let pt = self.parties.get_mut(&pid).unwrap();
        pt.invited.insert(target);
        let code = pt.code.clone();
        let name = self.players[&id].name.clone();
        self.send(target, Server::Invited { from: id, name, code });
    }

    fn queue(&mut self, id: u32, playlist: &str, seniority: u32, now: Instant) {
        let game = self.players[&id].game.clone();
        let Some(pl) = self
            .cfg
            .playlists
            .iter()
            .find(|p| p.id == playlist && (p.game.is_empty() || p.game == game))
            .cloned()
        else {
            return self.error(id, err::NO_PLAYLIST, "no such playlist");
        };
        let pid = match self.players[&id].party {
            Some(pid) => pid,
            None => {
                let pid = self.new_party(id, pl.team_size.max(2), 0, "");
                self.broadcast_party(pid);
                pid
            }
        };
        let seats = self.seats(pid);
        let pt = self.parties.get_mut(&pid).unwrap();
        if pt.leader != id {
            return self.error(id, err::NOT_LEADER, "only the party leader can search");
        }
        if seats > pl.team_size as usize {
            return self.error(id, err::BAD, "party too big for that playlist");
        }
        pt.ticket = Some(Queued { playlist: pl.id.clone(), seniority, since: now });
        pt.matched = None;
        self.send_queue_status(pid, now);
    }

    fn queue_cancel(&mut self, id: u32) {
        if let Some(pid) = self.players[&id].party {
            self.cancel_ticket(pid);
        }
    }

    fn cancel_ticket(&mut self, pid: u32) {
        let pt = self.parties.get_mut(&pid).unwrap();
        if let Some(q) = pt.ticket.take() {
            let msg = Server::QueueStatus { playlist: q.playlist, stage: STAGE_NONE, waited: 0, searching: 0 };
            let members: Vec<u32> = pt.members.iter().map(|m| m.0).collect();
            for m in members {
                self.send(m, msg.clone());
            }
        }
    }

    fn searching(&self, playlist: &str) -> u16 {
        self.parties
            .values()
            .filter(|p| p.ticket.as_ref().is_some_and(|q| q.playlist == playlist))
            .flat_map(|p| p.members.iter().map(|m| self.locals(m.0)))
            .sum::<usize>()
            .min(u16::MAX as usize) as u16
    }

    fn send_queue_status(&mut self, pid: u32, now: Instant) {
        let pt = &self.parties[&pid];
        let Some(q) = &pt.ticket else { return };
        let waited = now.saturating_duration_since(q.since).as_secs() as u32;
        let stage = self
            .cfg
            .playlists
            .iter()
            .find(|p| p.id == q.playlist)
            .map(|p| p.stage_index(waited) as u8)
            .unwrap_or(0);
        let msg = Server::QueueStatus {
            playlist: q.playlist.clone(),
            stage,
            waited: waited.min(u16::MAX as u32) as u16,
            searching: self.searching(&q.playlist),
        };
        let members: Vec<u32> = pt.members.iter().map(|m| m.0).collect();
        for m in members {
            self.send(m, msg.clone());
        }
    }

    /// Tells two players about each other so they can punch.
    fn introduce(&mut self, a: u32, b: u32) {
        if a == b || !self.players.contains_key(&a) || !self.players.contains_key(&b) {
            return;
        }
        let nonce = self.rand();
        let (pa, pb) = (&self.players[&a], &self.players[&b]);
        let to_a = Server::Introduce { peer: b, name: pb.name.clone(), public: pb.addr, lan: pb.lan.clone(), nonce };
        let to_b = Server::Introduce { peer: a, name: pa.name.clone(), public: pa.addr, lan: pa.lan.clone(), nonce };
        self.send(a, to_a);
        self.send(b, to_b);
        self.players.get_mut(&a).unwrap().introduced.insert(b);
        self.players.get_mut(&b).unwrap().introduced.insert(a);
    }

    fn relay(&mut self, id: u32, to: u32, payload: Vec<u8>, now: Instant) {
        if !self.players.contains_key(&to) || to == id {
            return;
        }
        let size = payload.len() as f64 + 28.0; // count IP/UDP headers too
        let p = self.players.get_mut(&id).unwrap();
        if !p.relay.take(size, now) {
            self.stats.relay_dropped += 1;
            return;
        }
        p.relayed_bytes += payload.len() as u64;
        self.stats.relayed_bytes += payload.len() as u64;
        let needs_intro = !p.introduced.contains(&to);
        if needs_intro {
            self.introduce(id, to);
        }
        self.send(to, Server::Forward { from: id, payload });
    }

    fn list(&mut self, id: u32) {
        let game = &self.players[&id].game;
        let list: Vec<Listed> = self
            .players
            .iter()
            .filter(|(pid, p)| **pid != id && &p.game == game)
            .take(64)
            .map(|(pid, p)| {
                let code = p
                    .party
                    .and_then(|x| self.parties.get(&x))
                    .filter(|pt| pt.flags & party::FLAG_JOINABLE != 0)
                    .map(|pt| pt.code.clone())
                    .unwrap_or_default();
                Listed { id: *pid, name: p.name.clone(), status: p.status.clone(), code }
            })
            .collect();
        self.send(id, Server::Players { list });
    }

    /// Runs once a second: timeouts, the matcher and queue status.
    pub fn tick(&mut self, now: Instant) -> Vec<Out> {
        let stale: Vec<u32> = self
            .players
            .iter()
            .filter(|(_, p)| now.saturating_duration_since(p.last_hello) > TIMEOUT)
            .map(|(id, _)| *id)
            .collect();
        for id in stale {
            self.drop_player(id);
        }

        let longest = self.cfg.playlists.iter().map(|p| p.avoid_rematch_minutes).max().unwrap_or(0);
        self.recent
            .retain(|_, at| now.saturating_duration_since(*at) < Duration::from_secs(longest as u64 * 60));

        if !self.cfg.relay_only {
            for pl in self.cfg.playlists.clone() {
                self.run_matcher(&pl, now);
            }
            let queued: Vec<u32> = self.parties.iter().filter(|(_, p)| p.ticket.is_some()).map(|(k, _)| *k).collect();
            for pid in queued {
                self.send_queue_status(pid, now);
            }
        }

        self.stats.players = self.players.len();
        self.stats.parties = self.parties.len();
        self.stats.queued_parties = self.parties.values().filter(|p| p.ticket.is_some()).count();
        std::mem::take(&mut self.out)
    }

    fn run_matcher(&mut self, pl: &Playlist, now: Instant) {
        let mut keys = Vec::new();
        let mut tickets = Vec::new();
        for (pid, pt) in &self.parties {
            let Some(q) = &pt.ticket else { continue };
            if q.playlist != pl.id {
                continue;
            }
            // Leader first, then the rest in join order.
            let mut members: Vec<(u32, u64)> = pt.members.clone();
            members.sort_by_key(|m| (m.0 != pt.leader, m.1));
            let rtt = members.iter().filter_map(|m| self.players.get(&m.0)).map(|p| p.rtt_ms).max().unwrap_or(0);
            keys.push(*pid);
            tickets.push(Ticket {
                key: *pid as u64,
                // One matcher member per seat: console id << 8 | slot.
                members: members
                    .iter()
                    .flat_map(|m| (0..self.locals(m.0) as u64).map(move |slot| (m.0 as u64) << 8 | slot))
                    .collect(),
                seniority: q.seniority,
                rtt_ms: rtt,
                waited: now.saturating_duration_since(q.since).as_secs() as u32,
            });
        }
        if tickets.len() < 1 {
            return;
        }
        let window = Duration::from_secs(pl.avoid_rematch_minutes as u64 * 60);
        let formed = {
            let players = &self.players;
            let recent = &self.recent;
            let tok = |x: u64| players.get(&((x >> 8) as u32)).map(|p| p.token).unwrap_or(0);
            let was_recent = |x: u64, y: u64| {
                let (a, b) = (tok(x), tok(y));
                let k = if a < b { (a, b) } else { (b, a) };
                recent.get(&k).is_some_and(|at| now.saturating_duration_since(*at) < window)
            };
            match_tickets(pl, &tickets, &was_recent)
        };

        for f in formed {
            let mid = self.rand();
            let roll = self.rand();
            let pick = pl.pick(roll, self.last_pick.get(&pl.id).map(|(a, b)| (a.as_str(), b.as_str())));
            if !pl.modes.is_empty() {
                self.last_pick.insert(pl.id.clone(), (pick.mode.clone(), pick.map.clone()));
            }
            let seats: Vec<Seat> = f
                .seats
                .iter()
                .map(|(m, team)| {
                    let (id, slot) = ((*m >> 8) as u32, (*m & 0xff) as u8);
                    Seat { id, slot, team: *team, name: self.seat_name(id, slot) }
                })
                .collect();
            let msg = Server::Match {
                id: mid,
                playlist: pl.id.clone(),
                host: (f.host >> 8) as u32,
                seats: seats.clone(),
                mode: pick.mode,
                map: pick.map,
                settings: pick.settings,
            };
            let mut consoles: Vec<u32> = seats.iter().map(|s| s.id).collect();
            consoles.dedup();
            for c in consoles {
                self.send(c, msg.clone());
            }
            // Introduce and remember every pair from different parties.
            for (i, &ti) in f.tickets.iter().enumerate() {
                for &tj in &f.tickets[i + 1..] {
                    // Consoles, not seats: guests share their console's path.
                    let ca: Vec<u32> = tickets[ti].members.iter().filter(|m| *m & 0xff == 0).map(|m| (m >> 8) as u32).collect();
                    let cb: Vec<u32> = tickets[tj].members.iter().filter(|m| *m & 0xff == 0).map(|m| (m >> 8) as u32).collect();
                    for &a in &ca {
                        for &b in &cb {
                            self.introduce(a, b);
                            let (x, y) = (self.players[&a].token, self.players[&b].token);
                            self.recent.insert(if x < y { (x, y) } else { (y, x) }, now);
                        }
                    }
                }
            }
            for &ti in &f.tickets {
                let pt = self.parties.get_mut(&keys[ti]).unwrap();
                pt.ticket = None;
                pt.matched = Some(mid);
            }
            self.stats.matches_made += 1;
        }
    }

    /// A JSON snapshot for the status page.
    pub fn status_json(&self) -> String {
        let playlists: Vec<serde_json::Value> = self
            .cfg
            .playlists
            .iter()
            .map(|p| {
                let modes: Vec<serde_json::Value> =
                    p.modes.iter().map(|m| serde_json::json!({ "id": m.id, "name": m.name, "maps": m.maps })).collect();
                serde_json::json!({
                    "id": p.id, "name": p.name, "game": p.game,
                    "players": p.players(), "teams": p.teams,
                    "searching": self.searching(&p.id), "modes": modes,
                })
            })
            .collect();
        let top_relays: Vec<serde_json::Value> = {
            let mut v: Vec<(&u32, &Player)> = self.players.iter().filter(|(_, p)| p.relayed_bytes > 0).collect();
            v.sort_by_key(|(_, p)| std::cmp::Reverse(p.relayed_bytes));
            v.iter()
                .take(10)
                .map(|(id, p)| serde_json::json!({ "id": id, "name": p.name, "relayed_bytes": p.relayed_bytes }))
                .collect()
        };
        serde_json::json!({
            "relay_only": self.cfg.relay_only,
            "players": self.players.len(),
            "parties": self.parties.len(),
            "queued_parties": self.parties.values().filter(|p| p.ticket.is_some()).count(),
            "matches_made": self.stats.matches_made,
            "relayed_bytes": self.stats.relayed_bytes,
            "relay_dropped": self.stats.relay_dropped,
            "datagrams_in": self.stats.datagrams_in,
            "playlists": playlists,
            "top_relays": top_relays,
        })
        .to_string()
    }

    /// Tells everyone the server is going away.
    pub fn shutdown(&mut self) -> Vec<Out> {
        let ids: Vec<u32> = self.players.iter().filter(|(_, p)| p.party.is_some()).map(|(k, _)| *k).collect();
        for id in ids {
            self.send(id, Server::PartyGone { reason: gone::RESTART });
        }
        std::mem::take(&mut self.out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Net {
        live: Live,
        now: Instant,
        inbox: HashMap<SocketAddr, Vec<Server>>,
    }

    fn addr(n: u16) -> SocketAddr {
        format!("198.51.100.{}:{}", n, 40000 + n).parse().unwrap()
    }

    fn pad(mut b: Vec<u8>) -> Vec<u8> {
        b.resize(b.len().max(MIN_HELLO), 0);
        b
    }

    impl Net {
        fn new(cfg: Config) -> Self {
            Net { live: Live::new(cfg, 12345), now: Instant::now(), inbox: HashMap::new() }
        }
        fn take(&mut self, out: Vec<Out>) {
            for (a, b) in out {
                let (_, ty, body) = wire::header(&b).unwrap();
                self.inbox.entry(a).or_default().push(Server::decode(ty, body).unwrap());
            }
        }
        fn send(&mut self, from: u16, m: Client) {
            let mut b = m.encode();
            if matches!(m, Client::Hello { .. }) {
                b = pad(b);
            }
            let out = self.live.handle(addr(from), &b, self.now);
            self.take(out);
        }
        fn hello(&mut self, n: u16, name: &str) -> u32 {
            self.hello_with(n, name, &[])
        }
        fn hello_with(&mut self, n: u16, name: &str, guests: &[&str]) -> u32 {
            self.hello_game(n, name, "stiletto", guests)
        }
        fn hello_game(&mut self, n: u16, name: &str, game: &str, guests: &[&str]) -> u32 {
            let guests = guests.iter().map(|g| g.to_string()).collect();
            self.send(n, Client::Hello { token: 1000 + n as u64, game: game.into(), name: name.into(), lan: vec![], guests });
            let id = self.inbox[&addr(n)].iter().find_map(|m| match m {
                Server::Welcome { id, public, .. } => {
                    assert_eq!(*public, addr(n));
                    Some(*id)
                }
                _ => None,
            });
            // Answer the ping straight away (0 ms on this fake network).
            let nonce = self.inbox[&addr(n)].iter().rev().find_map(|m| match m {
                Server::Ping { nonce } => Some(*nonce),
                _ => None,
            });
            self.send(n, Client::Pong { nonce: nonce.unwrap() });
            id.unwrap()
        }
        fn got(&mut self, n: u16) -> Vec<Server> {
            self.inbox.remove(&addr(n)).unwrap_or_default()
        }
        fn tick(&mut self, secs: u64) {
            for _ in 0..secs {
                self.now += Duration::from_secs(1);
                let out = self.live.tick(self.now);
                self.take(out);
            }
        }
    }

    fn party_code(msgs: &[Server]) -> String {
        msgs.iter()
            .rev()
            .find_map(|m| match m {
                Server::Party { code, .. } => Some(code.clone()),
                _ => None,
            })
            .expect("party message")
    }

    #[test]
    fn short_hello_and_strangers_get_nothing() {
        let mut n = Net::new(Config::default());
        let b = Client::Hello { token: 1, game: "g".into(), name: "n".into(), lan: vec![], guests: vec![] }.encode();
        assert!(n.live.handle(addr(1), &b, n.now).is_empty());
        assert!(n.live.handle(addr(1), &Client::List.encode(), n.now).is_empty());
    }

    #[test]
    fn party_by_code_introduces_members() {
        let mut n = Net::new(Config::default());
        let a = n.hello(1, "alice");
        let b = n.hello(2, "bob");
        n.got(1);
        n.got(2);
        n.send(1, Client::PartyCreate { max: 4, flags: party::FLAG_JOINABLE, info: "{\"map\":\"x\"}".into() });
        let code = party_code(&n.got(1));
        n.send(2, Client::PartyJoin { code: code.to_lowercase() });
        let to_b = n.got(2);
        let to_a = n.got(1);
        for msgs in [&to_a, &to_b] {
            let Some(Server::Party { leader, members, .. }) = msgs.iter().find(|m| matches!(m, Server::Party { .. })) else {
                panic!("no party")
            };
            assert_eq!(*leader, a);
            assert_eq!(members.len(), 2);
        }
        assert!(to_a.iter().any(|m| matches!(m, Server::Introduce { peer, .. } if *peer == b)));
        assert!(to_b.iter().any(|m| matches!(m, Server::Introduce { peer, .. } if *peer == a)));

        // Leader leaves: bob leads.
        n.send(1, Client::PartyLeave);
        let to_b = n.got(2);
        assert!(to_b.iter().any(|m| matches!(m, Server::Party { leader, .. } if *leader == b)));
        // Last member leaves: code is freed.
        n.send(2, Client::PartyLeave);
        n.send(1, Client::PartyJoin { code });
        assert!(n.got(1).iter().any(|m| matches!(m, Server::Error { code: err::NO_CODE, .. })));
    }

    #[test]
    fn invite_only_party_needs_invite() {
        let mut n = Net::new(Config::default());
        let _a = n.hello(1, "alice");
        let b = n.hello(2, "bob");
        n.send(1, Client::PartyCreate { max: 4, flags: 0, info: String::new() });
        let code = party_code(&n.got(1));
        n.send(2, Client::PartyJoin { code: code.clone() });
        assert!(n.got(2).iter().any(|m| matches!(m, Server::Error { .. })));
        n.send(1, Client::Invite { player: b });
        assert!(n.got(2).iter().any(|m| matches!(m, Server::Invited { code: c, .. } if *c == code)));
        n.send(2, Client::PartyJoin { code });
        assert!(n.got(2).iter().any(|m| matches!(m, Server::Party { members, .. } if members.len() == 2)));
    }

    fn find_match(msgs: &[Server]) -> Option<(u32, Vec<Seat>, String, String, Vec<(String, String)>)> {
        msgs.iter().find_map(|m| match m {
            Server::Match { host, seats, mode, map, settings, .. } => {
                Some((*host, seats.clone(), mode.clone(), map.clone(), settings.clone()))
            }
            _ => None,
        })
    }

    #[test]
    fn coop_firefight_fills_one_team_and_picks_a_mode() {
        let mut n = Net::new(Config::default());
        let a = n.hello_game(1, "alice", "coop", &["alice (2)"]);
        n.hello_game(2, "bob", "coop", &[]);
        n.hello_game(3, "carol", "coop", &[]);
        n.hello(4, "stiletto player");
        n.send(4, Client::Queue { playlist: "firefight".into(), seniority: 0 });
        assert!(n.got(4).iter().any(|m| matches!(m, Server::Error { code: err::NO_PLAYLIST, .. })));
        n.send(1, Client::Queue { playlist: "duel".into(), seniority: 0 });
        assert!(n.got(1).iter().any(|m| matches!(m, Server::Error { code: err::NO_PLAYLIST, .. })));

        // seniority means nothing in co-op: wildly different values still meet
        n.send(1, Client::Queue { playlist: "firefight".into(), seniority: 1 });
        n.send(2, Client::Queue { playlist: "firefight".into(), seniority: 900 });
        n.send(3, Client::Queue { playlist: "firefight".into(), seniority: 40 });
        for c in 1..=3 {
            n.got(c);
        }
        n.tick(1);
        let (host, seats, mode, map, settings) = find_match(&n.got(1)).expect("match");
        assert_eq!(host, a);
        assert_eq!(seats.len(), 4);
        assert!(seats.iter().all(|s| s.team == 0));
        assert!(seats.iter().any(|s| s.id == a && s.slot == 1 && s.name == "alice (2)"));
        assert!(mode == "firefight" || mode == "grunt_rush", "mode {mode}");
        assert_eq!(map, "coop_arena");
        assert!(settings.contains(&("coop_waves".into(), "1".into())));
        let first = mode.clone();
        // everyone gets the same pick
        for c in 2..=3 {
            let (_, _, m2, map2, _) = find_match(&n.got(c)).expect("match");
            assert_eq!((m2, map2), (mode.clone(), map.clone()));
        }

        // the next firefight match plays the other mode
        n.send(2, Client::Queue { playlist: "firefight".into(), seniority: 0 });
        n.send(3, Client::Queue { playlist: "firefight".into(), seniority: 0 });
        n.got(2);
        // two players aren't a full team, so they wait for the last stage (45 s)
        let mut found = None;
        for i in 0..6 {
            n.tick(10);
            n.hello_game(2, "bob", "coop", &[]);
            n.hello_game(3, "carol", "coop", &[]);
            if let Some(m) = find_match(&n.got(2)) {
                assert!(i >= 4, "matched short-handed after {}0 s", i + 1);
                found = Some(m);
                break;
            }
        }
        let (_, seats, mode, _, _) = found.expect("short-handed match after the last stage");
        assert_eq!(seats.len(), 2);
        assert_ne!(mode, first);
    }

    #[test]
    fn duel_queue_makes_a_match() {
        let mut n = Net::new(Config::default());
        let a = n.hello(1, "alice");
        let b = n.hello(2, "bob");
        n.send(1, Client::Queue { playlist: "duel".into(), seniority: 10 });
        n.send(2, Client::Queue { playlist: "duel".into(), seniority: 11 });
        n.got(1);
        n.got(2);
        n.tick(1);
        let to_a = n.got(1);
        let m = to_a.iter().find_map(|m| match m {
            Server::Match { host, seats, .. } => Some((*host, seats.clone())),
            _ => None,
        });
        let (host, seats) = m.expect("match");
        assert_eq!(host, a);
        assert_eq!(seats.len(), 2);
        assert!(to_a.iter().any(|m| matches!(m, Server::Introduce { peer, .. } if *peer == b)));
        assert_eq!(n.live.stats.matches_made, 1);

        // Requeue at once: rematch avoidance holds them apart until the
        // last stage (60 s for duel).
        n.send(1, Client::Queue { playlist: "duel".into(), seniority: 10 });
        n.send(2, Client::Queue { playlist: "duel".into(), seniority: 11 });
        for i in 0..6 {
            assert_eq!(n.live.stats.matches_made, 1, "rematched after {}0 s", i);
            n.tick(10);
            // Keep them alive past the 20 s timeout.
            n.hello(1, "alice");
            n.hello(2, "bob");
        }
        assert_eq!(n.live.stats.matches_made, 2);
    }

    #[test]
    fn split_screen_guests_take_seats() {
        let mut n = Net::new(Config::default());
        // A console with two local players queues 2v2 against two solos.
        let couch = n.hello_with(1, "sokky", &["sokky (2)"]);
        let b = n.hello(2, "bob");
        let c = n.hello(3, "cat");
        n.send(1, Client::Queue { playlist: "2v2".into(), seniority: 5 });
        let to_couch = n.got(1);
        assert!(to_couch.iter().any(|m| matches!(m, Server::Party { members, .. } if members[0].guests == vec!["sokky (2)".to_string()])));
        assert!(to_couch.iter().any(|m| matches!(m, Server::QueueStatus { searching: 2, .. })));
        n.send(2, Client::Queue { playlist: "2v2".into(), seniority: 5 });
        n.send(3, Client::Queue { playlist: "2v2".into(), seniority: 5 });
        n.got(2);
        n.got(3);
        n.tick(1);
        let to_couch = n.got(1);
        let seats = to_couch
            .iter()
            .find_map(|m| match m {
                Server::Match { seats, host, .. } => {
                    assert_eq!(*host, couch);
                    Some(seats.clone())
                }
                _ => None,
            })
            .expect("match");
        assert_eq!(seats.len(), 4);
        let couch_seats: Vec<&Seat> = seats.iter().filter(|s| s.id == couch).collect();
        assert_eq!(couch_seats.len(), 2);
        assert_eq!(couch_seats[0].team, couch_seats[1].team);
        assert_eq!(couch_seats[1].name, "sokky (2)");
        let other = seats.iter().filter(|s| s.id == b || s.id == c).map(|s| s.team).collect::<Vec<_>>();
        assert!(other.iter().all(|t| *t != couch_seats[0].team));
        // One match message per console, not per seat.
        assert_eq!(to_couch.iter().filter(|m| matches!(m, Server::Match { .. })).count(), 1);

        // Three guests make four local players: too many for a 2v2 party.
        n.hello_with(4, "dan", &["d2", "d3", "d4"]);
        n.got(4);
        n.send(4, Client::Queue { playlist: "2v2".into(), seniority: 1 });
        assert!(n.got(4).iter().any(|m| matches!(m, Server::Error { code: err::BAD, .. })));
    }

    #[test]
    fn guest_counts_against_party_size() {
        let mut n = Net::new(Config::default());
        n.hello(1, "alice");
        n.hello_with(2, "bob", &["bob (2)"]);
        n.send(1, Client::PartyCreate { max: 2, flags: party::FLAG_JOINABLE, info: String::new() });
        let code = party_code(&n.got(1));
        n.got(2);
        n.send(2, Client::PartyJoin { code });
        assert!(n.got(2).iter().any(|m| matches!(m, Server::Error { code: err::PARTY_FULL, .. })));
    }

    #[test]
    fn party_change_cancels_search() {
        let mut n = Net::new(Config::default());
        let _a = n.hello(1, "alice");
        let _b = n.hello(2, "bob");
        n.send(1, Client::PartyCreate { max: 2, flags: 1, info: String::new() });
        let code = party_code(&n.got(1));
        n.send(1, Client::Queue { playlist: "2v2".into(), seniority: 3 });
        assert!(n.got(1).iter().any(|m| matches!(m, Server::QueueStatus { stage: 0, .. })));
        n.send(2, Client::PartyJoin { code });
        assert!(n.got(1).iter().any(|m| matches!(m, Server::QueueStatus { stage: STAGE_NONE, .. })));
        // Bob isn't the leader.
        n.send(2, Client::Queue { playlist: "2v2".into(), seniority: 3 });
        assert!(n.got(2).iter().any(|m| matches!(m, Server::Error { code: err::NOT_LEADER, .. })));
    }

    #[test]
    fn relay_forwards_and_limits() {
        let mut cfg = Config::default();
        cfg.relay_rate = 1000.0;
        cfg.relay_burst_secs = 1.0;
        let mut n = Net::new(cfg);
        let a = n.hello(1, "alice");
        let b = n.hello(2, "bob");
        n.got(1);
        n.got(2);
        n.send(1, Client::Relay { to: b, payload: vec![1, 2, 9, 9] });
        let to_b = n.got(2);
        assert!(to_b.iter().any(|m| matches!(m, Server::Introduce { peer, .. } if *peer == a)));
        assert!(to_b.iter().any(|m| matches!(m, Server::Forward { from, payload } if *from == a && payload == &vec![1, 2, 9, 9])));
        // 1000 B/s: a burst of 500-byte packets is cut off after two.
        for _ in 0..5 {
            n.send(1, Client::Relay { to: b, payload: vec![0; 500] });
        }
        let fwd = n.got(2).iter().filter(|m| matches!(m, Server::Forward { .. })).count();
        assert!(fwd <= 2, "forwarded {fwd}");
        assert!(n.live.stats.relay_dropped >= 3);
    }

    #[test]
    fn relay_only_refuses_parties() {
        let mut cfg = Config::default();
        cfg.relay_only = true;
        let mut n = Net::new(cfg);
        n.hello(1, "alice");
        n.got(1);
        n.send(1, Client::PartyCreate { max: 2, flags: 1, info: String::new() });
        assert!(n.got(1).iter().any(|m| matches!(m, Server::Error { code: err::BAD, .. })));
    }

    #[test]
    fn timeout_and_rebind() {
        let mut n = Net::new(Config::default());
        let a = n.hello(1, "alice");
        n.send(1, Client::PartyCreate { max: 2, flags: 1, info: String::new() });
        // Same token from a new address keeps the id and the party.
        n.send(9, Client::Hello { token: 1001, game: "stiletto".into(), name: "alice".into(), lan: vec![], guests: vec![] });
        assert!(n.got(9).iter().any(|m| matches!(m, Server::Welcome { id, .. } if *id == a)));
        assert_eq!(n.live.parties.len(), 1);
        n.tick(21);
        assert!(n.live.players.is_empty());
        assert!(n.live.parties.is_empty());
    }
}
