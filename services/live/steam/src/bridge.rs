//! The bridge: Bulwark Live messages from the engine on one side, Steam on
//! the other (docs/spec/live-steam.md).

use crate::steam::{self, account, steamid, ApiCall, Event, Steam, SteamId};
use bwlive_core::matcher::{compatible, pack, Ticket};
use bwlive_core::records::{self, Records, SeatResult};
use bwlive_core::wire::{self, err, gone, Client, Listed, Member, Seat, Server};
use bwlive_core::{code, party, Playlist};
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4, UdpSocket};
use std::sync::mpsc::{channel, Receiver};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

/// Steam networking channels.
const CH_LIVE: i32 = 0; // Live payloads (virtual port bytes first), for live:<id>
const CH_CONTROL: i32 = 1; // JSON: set reports, fighter cards
const CH_TUNNEL: i32 = 2; // plain UDP game packets, for --tunnel

/// How often a waiting ticket looks for others.
const SEARCH_EVERY: Duration = Duration::from_secs(3);
/// Lobby search results asked for.
const SEARCH_MAX: i32 = 50;
/// A local engine that stops saying hello for this long is gone.
const CLIENT_TIMEOUT: Duration = Duration::from_secs(20);
/// Session requests from players we don't know yet are held this long,
/// in case the match that introduces them is still on its way.
const SESSION_HOLD: Duration = Duration::from_secs(15);

/// Lobby data keys. Everything a lobby search filters on starts with bw_.
mod key {
    pub const KIND: &str = "bw_kind";
    pub const GAME: &str = "bw_game";
    pub const CODE: &str = "bw_code";
    pub const MAX: &str = "bw_max";
    pub const FLAGS: &str = "bw_flags";
    pub const INFO: &str = "bw_info";
    pub const QUEUE: &str = "bw_queue";
    pub const MATCH: &str = "bw_match";
    pub const PLAYLIST: &str = "bw_pl";
    pub const SINCE: &str = "bw_t";
    pub const SENIORITY: &str = "bw_sen";
    pub const FREE: &str = "bw_free";
    pub const OPEN: &str = "bw_open";
    pub const DROP_IN: &str = "bw_drop";
    pub const IDS: &str = "bw_ids";
    // member data
    pub const NAME: &str = "name";
    pub const GUESTS: &str = "guests";
    pub const ROSTER: &str = "roster";
    pub const MEMBER_SENIORITY: &str = "sen";
}

pub struct Config {
    pub bind: String,
    pub steam_lib: String,
    pub app_id: u32,
    pub game: String,
    pub playlists: Vec<Playlist>,
    pub records: Option<String>,
    pub tunnel: bool,
    pub game_port: u16,
    pub tunnel_port: u16,
    pub on_host: Option<String>,
    pub on_join: Option<String>,
    pub accept_all: bool,
    pub quiet: bool,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            bind: "127.0.0.1:27950".into(),
            steam_lib: steam::default_lib().into(),
            app_id: 480,
            game: String::new(),
            playlists: Playlist::defaults(),
            records: None,
            tunnel: false,
            game_port: 27500,
            tunnel_port: 27960,
            on_host: None,
            on_join: None,
            accept_all: false,
            quiet: false,
        }
    }
}

/// A match as the ticket lobby's owner announced it.
#[derive(Clone, Debug, PartialEq)]
struct Match {
    id: u64,
    playlist: String,
    host: u32,
    seats: Vec<Seat>,
    mode: String,
    map: String,
    settings: Vec<(String, String)>,
}

impl Match {
    fn to_json(&self) -> String {
        json!({
            "id": self.id.to_string(), "pl": self.playlist, "host": self.host,
            "seats": self.seats.iter().map(|s| json!([s.id, s.slot, s.team, s.name])).collect::<Vec<_>>(),
            "mode": self.mode, "map": self.map,
            "settings": self.settings.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
        })
        .to_string()
    }
    fn from_json(s: &str) -> Option<Match> {
        let v: Value = serde_json::from_str(s).ok()?;
        let seats = v["seats"]
            .as_array()?
            .iter()
            .filter_map(|s| {
                Some(Seat {
                    id: s[0].as_u64()? as u32,
                    slot: s[1].as_u64()? as u8,
                    team: s[2].as_u64()? as u8,
                    name: s[3].as_str().unwrap_or("").to_string(),
                })
            })
            .take(16)
            .collect();
        let settings = v["settings"]
            .as_array()
            .map(|a| {
                a.iter()
                    .filter_map(|p| Some((p[0].as_str()?.to_string(), p[1].as_str()?.to_string())))
                    .take(32)
                    .collect()
            })
            .unwrap_or_default();
        Some(Match {
            id: v["id"].as_str()?.parse().ok()?,
            playlist: v["pl"].as_str()?.to_string(),
            host: v["host"].as_u64()? as u32,
            seats,
            mode: v["mode"].as_str().unwrap_or("").to_string(),
            map: v["map"].as_str().unwrap_or("").to_string(),
            settings,
        })
    }
    fn server(&self) -> Server {
        Server::Match {
            id: self.id,
            playlist: self.playlist.clone(),
            host: self.host,
            seats: self.seats.clone(),
            mode: self.mode.clone(),
            map: self.map.clone(),
            settings: self.settings.clone(),
        }
    }
    fn consoles(&self) -> Vec<u32> {
        let mut v: Vec<u32> = self.seats.iter().map(|s| s.id).collect();
        v.sort();
        v.dedup();
        v
    }
}

/// One local player of a party in a ticket's roster: (console, slot, name).
type RosterSeat = (u32, u8, String);

fn roster_to_json(r: &[RosterSeat]) -> String {
    Value::Array(r.iter().map(|(id, slot, name)| json!([id, slot, name])).collect()).to_string()
}
fn roster_from_json(s: &str) -> Vec<RosterSeat> {
    let Ok(Value::Array(a)) = serde_json::from_str::<Value>(s) else { return Vec::new() };
    a.iter()
        .filter_map(|x| Some((x[0].as_u64()? as u32, x[1].as_u64()? as u8, x[2].as_str().unwrap_or("").to_string())))
        .take(16)
        .collect()
}

#[derive(Debug, Clone)]
enum Pending {
    CreateParty { max: u8, flags: u8, info: String },
    CreateTicket,
    FindCode(String),
    FindTickets,
    FindBoard,
    Download(Vec<u32>),
    Upload,
}

struct Queued {
    playlist: Playlist,
    seniority: u32,
    since: Instant,
    since_unix: u64,
    next_search: Instant,
    searching: u16,
    stage: u8,
    searching_now: bool,
}

struct HeldSession {
    peer: SteamId,
    at: Instant,
}

pub struct Bridge {
    cfg: Config,
    s: Steam,
    me: u32,
    sock: UdpSocket,
    client: Option<SocketAddr>,
    last_hello: Instant,
    welcomed: bool,
    game: String,
    name: String,
    guests: Vec<String>,

    calls: HashMap<ApiCall, Pending>,
    party: Option<SteamId>,
    joining_party: Option<SteamId>,
    creating_party: bool,
    party_sent: String,
    party_queue_seen: String,
    party_match_seen: String,
    known_codes: HashMap<String, SteamId>,

    queue: Option<Queued>,
    ticket: Option<SteamId>,
    joining_ticket: Option<SteamId>,
    creating_ticket: bool,
    last_pick: HashMap<String, (String, String)>,
    recent: Vec<(u32, Instant)>,

    matched: Option<Match>,
    match_over: bool,
    sets_seen: HashSet<(u64, u32)>,
    set_seq: u32,

    records: Records,
    records_path: String,
    board: Option<u64>,
    cards: HashMap<u32, String>,
    card_wanted: HashSet<u32>,

    held: Vec<HeldSession>,
    accepted: HashSet<SteamId>,

    tunnel: Option<UdpSocket>,
    tunnel_client: Option<SocketAddr>,
    tunnel_peers: HashMap<u32, UdpSocket>,
    last_status: Instant,
    /// Live and tunnel bytes over Steam, out and in, and when last logged
    traffic: (u64, u64),
    traffic_logged: (u64, u64, Instant),
}

fn unix_now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

fn rand64() -> u64 {
    use std::collections::hash_map::RandomState;
    use std::hash::{BuildHasher, Hasher};
    let mut h = RandomState::new().build_hasher();
    h.write_u64(unix_now());
    h.write_u128(Instant::now().elapsed().as_nanos());
    h.finish()
}

/// The room code of a party lobby: its id, mixed, cut to 30 bits.
fn code_of(lobby: SteamId) -> String {
    let mut z = lobby.wrapping_add(0x9e37_79b9_7f4a_7c15);
    z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    code::from_bits(z ^ (z >> 31))
}

pub fn stdin_lines() -> Receiver<String> {
    let (tx, rx) = channel();
    std::thread::spawn(move || {
        let mut line = String::new();
        loop {
            line.clear();
            match std::io::stdin().read_line(&mut line) {
                Ok(0) | Err(_) => return,
                Ok(_) => {
                    if tx.send(line.trim().to_string()).is_err() {
                        return;
                    }
                }
            }
        }
    });
    rx
}

impl Bridge {
    pub fn new(cfg: Config) -> Result<Bridge, String> {
        let s = Steam::init(&cfg.steam_lib)?;
        let me = account(s.me());
        if me == 0 {
            return Err("Steam gave no account id (not logged in?)".into());
        }
        let sock = UdpSocket::bind(&cfg.bind).map_err(|e| format!("can't bind {}: {e}", cfg.bind))?;
        sock.set_nonblocking(true).map_err(|e| e.to_string())?;
        let tunnel = if cfg.tunnel {
            let t = UdpSocket::bind(("127.0.0.1", cfg.tunnel_port)).map_err(|e| format!("can't bind the tunnel port {}: {e}", cfg.tunnel_port))?;
            t.set_nonblocking(true).map_err(|e| e.to_string())?;
            Some(t)
        } else {
            None
        };
        let records_path = cfg.records.clone().unwrap_or_else(|| format!("bwlive-steam-records-{me}.json"));
        let name = s.persona();
        let mut b = Bridge {
            game: cfg.game.clone(),
            name,
            cfg,
            me,
            sock,
            client: None,
            last_hello: Instant::now(),
            welcomed: false,
            guests: Vec::new(),
            calls: HashMap::new(),
            party: None,
            joining_party: None,
            creating_party: false,
            party_sent: String::new(),
            party_queue_seen: String::new(),
            party_match_seen: String::new(),
            known_codes: HashMap::new(),
            queue: None,
            ticket: None,
            joining_ticket: None,
            creating_ticket: false,
            last_pick: HashMap::new(),
            recent: Vec::new(),
            matched: None,
            match_over: false,
            sets_seen: HashSet::new(),
            set_seq: 0,
            records: Records::default(),
            records_path,
            board: None,
            cards: HashMap::new(),
            card_wanted: HashSet::new(),
            held: Vec::new(),
            accepted: HashSet::new(),
            tunnel,
            tunnel_client: None,
            tunnel_peers: HashMap::new(),
            last_status: Instant::now(),
            traffic: (0, 0),
            traffic_logged: (0, 0, Instant::now()),
            s,
        };
        b.load_records();
        b.s.request_stats();
        b.log(&format!(
            "on Steam as {} (account {}), app {}; Live on {}{}",
            b.name,
            b.me,
            b.s.app_id(),
            b.cfg.bind,
            if b.cfg.tunnel { format!(", tunnel on 127.0.0.1:{}", b.cfg.tunnel_port) } else { String::new() }
        ));
        if !b.game.is_empty() {
            b.s.set_rich_presence("bw_game", &b.game.clone());
        }
        Ok(b)
    }

    fn log(&self, s: &str) {
        if !self.cfg.quiet {
            println!("bwlive-steam: {s}");
        }
    }

    // --- to the engine ------------------------------------------------------

    fn emit(&mut self, m: Server) {
        match &m {
            Server::Error { text, .. } => self.log(&format!("error: {text}")),
            Server::Invited { name, code, .. } => self.log(&format!("{name} invited you to party {code} (join {code})")),
            Server::Match { id, playlist, host, seats, map, .. } => self.log(&format!(
                "match {id} on {playlist}{}{}, {} seats, host {host}{}",
                if map.is_empty() { "" } else { " on " },
                map,
                seats.len(),
                if *host == self.me { " (you)" } else { "" }
            )),
            _ => {}
        }
        if let Some(to) = self.client {
            let _ = self.sock.send_to(&m.encode(), to);
        }
    }

    fn error(&mut self, code: u8, text: &str) {
        self.emit(Server::Error { code, text: text.to_string() });
    }

    // --- commands typed on stdin ---------------------------------------------

    /// One line from stdin; false to quit.
    pub fn command(&mut self, line: &str) -> bool {
        let mut it = line.split_whitespace();
        let Some(cmd) = it.next() else { return true };
        let args: Vec<&str> = it.collect();
        let arg = |i: usize| args.get(i).copied().unwrap_or("");
        let msg = match cmd {
            "quit" | "exit" => return false,
            "help" => {
                println!(
                    "party [max] [open 0|1] [info] | join CODE | leave | open 0|1 | invite ID | list\n\
                     queue PLAYLIST | cancel | left | card [ID] | guests NAME... | - | status | game NAME | quit"
                );
                return true;
            }
            "game" => {
                self.game = arg(0).to_string();
                self.s.set_rich_presence("bw_game", &self.game.clone());
                return true;
            }
            "status" => {
                self.print_status();
                return true;
            }
            "guests" => {
                // as a game's hello would name them
                let guests = args.iter().filter(|g| **g != "-").map(|g| g.to_string()).collect();
                let game = self.game.clone();
                let name = self.name.clone();
                Client::Hello { token: 0, game, name, lan: Vec::new(), guests }
            }
            "party" => Client::PartyCreate {
                max: arg(0).parse().unwrap_or(4),
                flags: arg(1).parse().unwrap_or(1),
                info: args.get(2..).map(|a| a.join(" ")).unwrap_or_default(),
            },
            "join" => Client::PartyJoin { code: arg(0).to_string() },
            "leave" => Client::PartyLeave,
            "open" => Client::PartySet { flags: arg(0).parse().unwrap_or(1), info: String::new() },
            "invite" => Client::Invite { player: arg(0).parse().unwrap_or(0) },
            "list" => Client::List,
            "queue" => Client::Queue { playlist: arg(0).to_string(), seniority: 0 },
            "cancel" => Client::QueueCancel,
            "left" => match &self.matched {
                Some(m) => Client::MatchLeft { id: m.id },
                None => return true,
            },
            "card" => Client::Card { player: arg(0).parse().unwrap_or(0) },
            _ => {
                println!("unknown command {cmd} (help)");
                return true;
            }
        };
        self.handle(msg);
        true
    }

    fn print_status(&self) {
        println!("account {} ({}), game {:?}", self.me, self.name, self.game);
        match self.party {
            Some(l) => println!("party {} ({} consoles), leader {}", code_of(l), self.s.members(l).len(), account(self.s.owner(l))),
            None => println!("no party"),
        }
        if let Some(q) = &self.queue {
            println!("searching {}: stage {}, {} s, {} searching", q.playlist.id, q.stage, q.since.elapsed().as_secs(), q.searching);
        }
        if let Some(m) = &self.matched {
            let seats: Vec<String> = m.seats.iter().map(|s| format!("{}:{}:{}", s.id, s.slot, s.team)).collect();
            println!(
                "match {} on {}, host {}, seats {}{}",
                m.id,
                m.playlist,
                m.host,
                seats.join(" "),
                if self.match_over { " (left)" } else { "" }
            );
        }
    }

    // --- frame ------------------------------------------------------------------

    pub fn frame(&mut self) {
        let mut buf = [0u8; 2048];
        for _ in 0..256 {
            match self.sock.recv_from(&mut buf) {
                Ok((n, from)) => self.from_engine(&buf[..n], from),
                Err(_) => break,
            }
        }
        for ev in self.s.poll() {
            self.on_event(ev);
        }
        for (from, data) in self.s.receive(CH_LIVE) {
            self.traffic.1 += data.len() as u64;
            if let Some(to) = self.client {
                let m = Server::Forward { from: account(from), payload: data };
                let _ = self.sock.send_to(&m.encode(), to);
            }
        }
        for (from, data) in self.s.receive(CH_CONTROL) {
            self.on_control(account(from), &data);
        }
        self.tunnel_frame();
        self.queue_frame();
        self.ticket_frame();
        self.session_frame();
        if self.traffic_logged.2.elapsed() >= Duration::from_secs(5) && (self.traffic.0, self.traffic.1) != (self.traffic_logged.0, self.traffic_logged.1) {
            self.log(&format!("game traffic over Steam: {} bytes out, {} in", self.traffic.0, self.traffic.1));
            self.traffic_logged = (self.traffic.0, self.traffic.1, Instant::now());
        }
        if self.client.is_some() && self.last_hello.elapsed() > CLIENT_TIMEOUT {
            self.log("the engine stopped saying hello; leaving lobbies");
            self.client = None;
            self.welcomed = false;
            self.leave_all();
        }
    }

    pub fn shutdown(&mut self) {
        self.leave_all();
        for _ in 0..10 {
            self.s.poll();
            std::thread::sleep(Duration::from_millis(20));
        }
        self.s.shutdown();
    }

    fn leave_all(&mut self) {
        self.leave_ticket();
        self.queue = None;
        if let Some(l) = self.party.take() {
            self.s.leave_lobby(l);
        }
        self.s.set_rich_presence("bw_code", "");
    }

    // --- from the engine ------------------------------------------------------

    fn from_engine(&mut self, b: &[u8], from: SocketAddr) {
        if !from.ip().is_loopback() {
            return; // only this computer's games
        }
        let Some((ver, ty, body)) = wire::header(b) else { return };
        if ver != wire::VERSION {
            return;
        }
        if ty == wire::t::HELLO && b.len() < 64 {
            return;
        }
        let Some(msg) = Client::decode(ty, body) else { return };
        if let Client::Hello { .. } = msg {
            self.client = Some(from);
        } else if self.client != Some(from) {
            return; // nothing before hello
        }
        self.handle(msg);
    }

    fn handle(&mut self, msg: Client) {
        match msg {
            Client::Hello { game, name, guests, .. } => {
                self.last_hello = Instant::now();
                let guests: Vec<String> = guests.iter().take(wire::MAX_GUESTS).map(|g| party::clean_name(g)).collect();
                if game != self.game {
                    self.game = party::clip(&game, 32);
                    self.s.set_rich_presence("bw_game", &self.game.clone());
                }
                let name = party::clean_name(&name);
                if !name.is_empty() {
                    self.name = name;
                }
                let changed = guests != self.guests;
                self.guests = guests;
                let client = self.client.unwrap_or_else(|| SocketAddr::V4(SocketAddrV4::new(Ipv4Addr::LOCALHOST, 0)));
                self.emit(Server::Welcome { id: self.me, public: client, interval: 5 });
                self.emit(Server::Ping { nonce: 1 });
                if !self.welcomed {
                    self.welcomed = true;
                    self.log(&format!("engine said hello from {client} (game {:?})", self.game));
                }
                if let Some(l) = self.party {
                    self.publish_member(l);
                }
                if changed {
                    // the party's size changed: a running search starts over
                    if let Some(q) = self.queue.as_ref().map(|q| q.playlist.id.clone()) {
                        self.leave_ticket();
                        self.queue = None;
                        self.handle(Client::Queue { playlist: q, seniority: 0 });
                    }
                }
            }
            Client::Bye => {
                self.leave_all();
                self.client = None;
                self.welcomed = false;
            }
            Client::PartyCreate { max, flags, info } => {
                if self.creating_party {
                    return;
                }
                if let Some(old) = self.party.take() {
                    self.s.leave_lobby(old);
                }
                let max = party::clamp_max(max);
                let kind = if flags & 1 != 0 { steam::lobby_type::PUBLIC } else { steam::lobby_type::FRIENDS_ONLY };
                let call = self.s.create_lobby(kind, max as i32);
                self.creating_party = true;
                self.calls.insert(call, Pending::CreateParty { max, flags, info: party::clip(&info, party::MAX_INFO) });
            }
            Client::PartyJoin { code: typed } => {
                let Some(c) = code::normalize(&typed) else {
                    return self.error(err::NO_CODE, "no such room code");
                };
                if let Some(&l) = self.known_codes.get(&c) {
                    self.join_party(l);
                } else {
                    let game = self.game.clone();
                    let call = self.s.find_lobbies(
                        &[(key::KIND, "party"), (key::CODE, &c), (key::GAME, &game)],
                        &[],
                        steam::distance::WORLDWIDE,
                        5,
                    );
                    self.calls.insert(call, Pending::FindCode(c));
                }
            }
            Client::PartyLeave => {
                if let Some(l) = self.party.take() {
                    self.s.leave_lobby(l);
                    self.leave_ticket();
                    self.queue = None;
                    self.s.set_rich_presence("bw_code", "");
                }
                self.party_sent.clear();
                self.emit(Server::PartyGone { reason: gone::LEFT });
            }
            Client::PartySet { flags, info } => {
                let Some(l) = self.party else { return self.error(err::BAD, "not in a party") };
                if account(self.s.owner(l)) != self.me {
                    return self.error(err::NOT_LEADER, "only the party leader can do that");
                }
                let kind = if flags & 1 != 0 { steam::lobby_type::PUBLIC } else { steam::lobby_type::FRIENDS_ONLY };
                self.s.set_lobby_type(l, kind);
                self.s.set_lobby_data(l, key::FLAGS, &(flags & 1).to_string());
                if !info.is_empty() {
                    self.s.set_lobby_data(l, key::INFO, &party::clip(&info, party::MAX_INFO));
                }
                self.update_presence();
            }
            Client::Invite { player } => {
                let Some(l) = self.party else { return self.error(err::BAD, "make a party first (live_party)") };
                if player == 0 || !self.s.invite(l, steamid(player)) {
                    self.error(err::BAD, "Steam refused the invite");
                }
            }
            Client::Queue { playlist, seniority: _ } => self.start_queue(&playlist),
            Client::QueueCancel => {
                if let Some(l) = self.party {
                    if account(self.s.owner(l)) != self.me {
                        return self.error(err::NOT_LEADER, "only the party leader can do that");
                    }
                    self.s.set_lobby_data(l, key::QUEUE, "");
                }
                let pl = self.queue.take().map(|q| q.playlist.id).unwrap_or_default();
                self.leave_ticket();
                self.emit(Server::QueueStatus { playlist: pl, stage: wire::STAGE_NONE, waited: 0, searching: 0 });
            }
            Client::Relay { to, payload } => {
                if to != 0 && to != self.me {
                    self.traffic.0 += payload.len() as u64;
                    self.s.send(steamid(to), &payload, steam::send::UNRELIABLE_NO_NAGLE | steam::send::AUTO_RESTART, CH_LIVE);
                    self.accepted.insert(steamid(to));
                }
            }
            Client::Status { status } => self.s.set_rich_presence("status", &party::clip(&status, party::MAX_STATUS)),
            Client::List => {
                let game = self.game.clone();
                let list: Vec<Listed> = self
                    .s
                    .friends()
                    .into_iter()
                    .filter(|&f| self.s.rich_presence(f, "bw_game") == game)
                    .take(64)
                    .map(|f| Listed {
                        id: account(f),
                        name: self.s.friend_name(f),
                        status: self.s.rich_presence(f, "status"),
                        code: self.s.rich_presence(f, "bw_code"),
                    })
                    .collect();
                if self.client.is_none() {
                    for p in &list {
                        println!("  {:>10}  {:<24} {} {}", p.id, p.name, p.status, p.code);
                    }
                }
                self.emit(Server::Players { list });
            }
            Client::MatchLeft { id } => {
                if self.matched.as_ref().is_some_and(|m| m.id == id) && !self.match_over {
                    self.match_over = true;
                    self.log(&format!("left match {id}"));
                    self.leave_ticket();
                }
            }
            Client::Pong { .. } => {}
            Client::Playlists => {
                let json = self.playlist_info();
                self.emit(Server::PlaylistInfo { json });
            }
            Client::SetReport { match_id, result, first_to: _, runback, seats } => self.set_report(match_id, result, runback != 0, &seats),
            Client::Card { player } => self.card(player),
        }
    }

    fn playlist_info(&self) -> String {
        let build = |maps: bool| -> String {
            let v: Vec<Value> = self
                .cfg
                .playlists
                .iter()
                .filter(|p| p.game.is_empty() || p.game == self.game)
                .map(|p| {
                    let modes: Vec<Value> = p
                        .modes
                        .iter()
                        .map(|m| if maps { json!({"id": m.id, "name": m.name, "maps": m.maps}) } else { json!({"id": m.id, "name": m.name}) })
                        .collect();
                    let searching = self.queue.as_ref().filter(|q| q.playlist.id == p.id).map(|q| q.searching).unwrap_or(0);
                    json!({"id": p.id, "name": p.name, "description": p.description, "players": p.players(), "teams": p.teams,
                           "drop_in": p.drop_in, "searching": searching, "modes": modes})
                })
                .collect();
            Value::Array(v).to_string()
        };
        let mut json = build(true);
        if json.len() > wire::MAX_DATAGRAM - wire::HEADER {
            json = build(false);
        }
        if json.len() > wire::MAX_DATAGRAM - wire::HEADER {
            json = "[]".into();
        }
        json
    }

    // --- Steam events ----------------------------------------------------------

    fn on_event(&mut self, ev: Event) {
        match ev {
            Event::LobbyCreated { call, ok, lobby } => match self.calls.remove(&call) {
                Some(Pending::CreateParty { max, flags, info }) => {
                    self.creating_party = false;
                    if !ok {
                        return self.error(err::FULL, "Steam couldn't make the lobby");
                    }
                    let l = lobby;
                    self.party = Some(l);
                    let c = code_of(l);
                    self.known_codes.insert(c.clone(), l);
                    let game = self.game.clone();
                    for (k, v) in [
                        (key::KIND, "party".to_string()),
                        (key::GAME, game),
                        (key::CODE, c),
                        (key::MAX, max.to_string()),
                        (key::FLAGS, (flags & 1).to_string()),
                        (key::INFO, info),
                    ] {
                        self.s.set_lobby_data(l, k, &v);
                    }
                    self.publish_member(l);
                    self.update_presence();
                    self.send_party();
                }
                Some(Pending::CreateTicket) => {
                    self.creating_ticket = false;
                    if !ok {
                        return self.error(err::FULL, "Steam couldn't make the search lobby");
                    }
                    let Some(q) = &self.queue else {
                        self.s.leave_lobby(lobby);
                        return;
                    };
                    let (pl, players, sen, since) = (q.playlist.id.clone(), q.playlist.players(), q.seniority, q.since_unix);
                    self.ticket = Some(lobby);
                    let game = self.game.clone();
                    let roster = self.roster();
                    for (k, v) in [
                        (key::KIND, "ticket".to_string()),
                        (key::GAME, game),
                        (key::PLAYLIST, pl.clone()),
                        (key::SINCE, since.to_string()),
                        (key::SENIORITY, sen.to_string()),
                        (key::FREE, (players.saturating_sub(roster.len())).to_string()),
                        (key::OPEN, "1".into()),
                    ] {
                        self.s.set_lobby_data(lobby, k, &v);
                    }
                    self.s.set_member_data(lobby, key::ROSTER, &roster_to_json(&roster));
                    self.s.set_member_data(lobby, key::MEMBER_SENIORITY, &sen.to_string());
                    self.log(&format!("waiting in {pl}"));
                }
                _ => {
                    if ok {
                        self.s.leave_lobby(lobby);
                    }
                }
            },
            Event::LobbyEnter { lobby, response } => self.entered(lobby, response),
            Event::LobbyDataUpdate { lobby, .. } | Event::LobbyChatUpdate { lobby, .. } => {
                if Some(lobby) == self.party {
                    self.party_changed();
                } else if Some(lobby) == self.ticket {
                    self.ticket_changed();
                }
            }
            Event::LobbyMatchList { call, count } => match self.calls.remove(&call) {
                Some(Pending::FindCode(c)) => {
                    let found = (0..count as i32).map(|i| self.s.lobby_at(i)).find(|&l| self.s.lobby_data(l, key::CODE) == c);
                    match found {
                        Some(l) => self.join_party(l),
                        None => self.error(err::NO_CODE, "no such room code"),
                    }
                }
                Some(Pending::FindTickets) => {
                    let found: Vec<SteamId> = (0..count as i32).map(|i| self.s.lobby_at(i)).collect();
                    self.tickets_found(found);
                }
                _ => {}
            },
            Event::LobbyInvite { user, lobby } => {
                let c = code_of(lobby);
                self.known_codes.insert(c.clone(), lobby);
                let name = self.s.friend_name(user);
                self.emit(Server::Invited { from: account(user), name, code: c });
            }
            Event::JoinRequested { lobby, .. } => {
                // the Steam overlay's "Join game" or an accepted invite
                self.known_codes.insert(code_of(lobby), lobby);
                self.join_party(lobby);
            }
            Event::RichPresenceJoin { connect, .. } => {
                if let Some(c) = connect.split_whitespace().last().and_then(code::normalize) {
                    self.handle(Client::PartyJoin { code: c });
                }
            }
            Event::SessionRequest { peer } => {
                if self.allowed(peer) {
                    self.s.accept(peer);
                    self.accepted.insert(peer);
                } else {
                    self.held.push(HeldSession { peer, at: Instant::now() });
                }
            }
            Event::SessionFailed { peer } => {
                self.accepted.remove(&peer);
                self.log(&format!("Steam session with {} failed", account(peer)));
            }
            Event::StatsReceived { .. } => {}
            Event::LeaderboardFound { call, board, found } => {
                if let Some(Pending::FindBoard) = self.calls.remove(&call) {
                    if found && board != 0 {
                        self.board = Some(board);
                        self.publish_record();
                        let want: Vec<u32> = self.card_wanted.iter().copied().collect();
                        if !want.is_empty() {
                            self.download_cards(want);
                        }
                    }
                }
            }
            Event::ScoresDownloaded { call, entries, count, .. } => {
                if let Some(Pending::Download(ids)) = self.calls.remove(&call) {
                    let mut got = HashSet::new();
                    for i in 0..count {
                        if let Some(e) = self.s.board_entry(entries, i) {
                            let id = account(e.user);
                            got.insert(id);
                            let name = self.s.friend_name(e.user);
                            let card = card_from_details(id, &name, &e.details);
                            self.card_wanted.remove(&id);
                            self.cards.entry(id).or_insert(card.clone());
                            self.emit(Server::CardInfo { json: self.cards[&id].clone() });
                        }
                    }
                    for id in ids {
                        if !got.contains(&id) && self.card_wanted.remove(&id) {
                            let name = self.s.friend_name(steamid(id));
                            let json = id_first(&records::Record::default().card_json(id, &name), id);
                            self.emit(Server::CardInfo { json });
                        }
                    }
                }
            }
            Event::ScoreUploaded { call, ok } => {
                if let Some(Pending::Upload) = self.calls.remove(&call) {
                    if !ok {
                        self.log("leaderboard upload failed");
                    }
                }
            }
            Event::Other(_) => {}
        }
    }

    fn entered(&mut self, lobby: SteamId, response: u32) {
        let ok = response == 1;
        if Some(lobby) == self.joining_party {
            self.joining_party = None;
            if !ok {
                return if response == 4 {
                    self.error(err::PARTY_FULL, "that party is full")
                } else {
                    self.error(err::NO_CODE, "couldn't join that party")
                };
            }
            // guests count toward the party's size
            let max: usize = self.s.lobby_data(lobby, key::MAX).parse().unwrap_or(16);
            let members = self.s.members(lobby);
            let others: usize = members
                .iter()
                .filter(|&&m| account(m) != self.me)
                .map(|&m| 1 + split_guests(&self.s.member_data(lobby, m, key::GUESTS)).len())
                .sum();
            if others + 1 + self.guests.len() > max {
                self.s.leave_lobby(lobby);
                return self.error(err::PARTY_FULL, "that party is full");
            }
            if let Some(old) = self.party.replace(lobby) {
                if old != lobby {
                    self.s.leave_lobby(old);
                }
            }
            self.leave_ticket();
            self.queue = None;
            self.known_codes.insert(code_of(lobby), lobby);
            self.publish_member(lobby);
            self.update_presence();
            self.party_changed();
        } else if Some(lobby) == self.joining_ticket {
            self.joining_ticket = None;
            if !ok || self.queue.is_none() {
                if ok {
                    self.s.leave_lobby(lobby);
                }
                return;
            }
            if let Some(old) = self.ticket.replace(lobby) {
                if old != lobby {
                    self.s.leave_lobby(old);
                }
            }
            let roster = self.roster();
            let sen = self.queue.as_ref().map(|q| q.seniority).unwrap_or(0);
            self.s.set_member_data(lobby, key::ROSTER, &roster_to_json(&roster));
            self.s.set_member_data(lobby, key::MEMBER_SENIORITY, &sen.to_string());
            self.log(&format!("joined a waiting ticket ({} there)", self.s.members(lobby).len()));
            self.ticket_changed();
        }
    }

    fn join_party(&mut self, l: SteamId) {
        if Some(l) == self.party {
            return self.send_party();
        }
        self.joining_party = Some(l);
        self.s.join_lobby(l);
    }

    fn publish_member(&self, l: SteamId) {
        self.s.set_member_data(l, key::NAME, &self.name);
        self.s.set_member_data(l, key::GUESTS, &self.guests.join("\n"));
        self.s.set_member_data(l, key::MEMBER_SENIORITY, &self.seniority().to_string());
    }

    fn update_presence(&self) {
        match self.party {
            Some(l) if self.s.lobby_data(l, key::FLAGS) == "1" => {
                let c = code_of(l);
                self.s.set_rich_presence("bw_code", &c);
                self.s.set_rich_presence("connect", &format!("+live_join {c}"));
            }
            _ => {
                self.s.set_rich_presence("bw_code", "");
                self.s.set_rich_presence("connect", "");
            }
        }
    }

    // --- the party ----------------------------------------------------------------

    fn party_msg(&self, l: SteamId) -> Server {
        let members = self
            .s
            .members(l)
            .into_iter()
            .take(16)
            .map(|m| {
                let mut name = self.s.member_data(l, m, key::NAME);
                if name.is_empty() {
                    name = self.s.friend_name(m);
                }
                Member {
                    id: account(m),
                    name,
                    public: SocketAddr::V4(SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, 0)),
                    lan: Vec::new(),
                    guests: split_guests(&self.s.member_data(l, m, key::GUESTS)),
                }
            })
            .collect();
        Server::Party {
            code: code_of(l),
            leader: account(self.s.owner(l)),
            max: self.s.lobby_data(l, key::MAX).parse().unwrap_or(4),
            flags: self.s.lobby_data(l, key::FLAGS).parse().unwrap_or(0),
            info: self.s.lobby_data(l, key::INFO),
            members,
        }
    }

    fn send_party(&mut self) {
        let Some(l) = self.party else { return };
        let m = self.party_msg(l);
        let enc = m.encode();
        if let Server::Party { code, members, max, .. } = &m {
            let sig = format!("{enc:?}");
            if sig != self.party_sent {
                if self.party_sent.is_empty() {
                    self.log(&format!("in party {code} ({} of {max})", members.len()));
                }
                self.party_sent = sig;
            }
        }
        self.emit(m);
    }

    fn party_changed(&mut self) {
        let Some(l) = self.party else { return };
        self.send_party();
        let leader = account(self.s.owner(l)) == self.me;
        if leader {
            // a member joined or left: the ticket's roster changes
            if let Some(t) = self.ticket {
                if self.matched.is_none() || self.match_over {
                    let roster = self.roster();
                    self.s.set_member_data(t, key::ROSTER, &roster_to_json(&roster));
                }
            }
            return;
        }
        // a member follows the leader's search and match
        let q = self.s.lobby_data(l, key::QUEUE);
        if q != self.party_queue_seen {
            self.party_queue_seen = q.clone();
            let p: Vec<&str> = q.split('|').collect();
            if p.len() == 4 {
                self.emit(Server::QueueStatus {
                    playlist: p[0].to_string(),
                    stage: p[1].parse().unwrap_or(0),
                    waited: p[2].parse().unwrap_or(0),
                    searching: p[3].parse().unwrap_or(0),
                });
            } else {
                self.emit(Server::QueueStatus { playlist: String::new(), stage: wire::STAGE_NONE, waited: 0, searching: 0 });
            }
        }
        let m = self.s.lobby_data(l, key::MATCH);
        if m != self.party_match_seen {
            self.party_match_seen = m.clone();
            if let Some(m) = Match::from_json(&m) {
                self.on_match(m);
            }
        }
    }

    /// This console's local players: (id, slot, name).
    fn own_seats(&self) -> Vec<RosterSeat> {
        let mut v = vec![(self.me, 0u8, self.name.clone())];
        for (i, g) in self.guests.iter().enumerate() {
            v.push((self.me, i as u8 + 1, g.clone()));
        }
        v
    }

    /// Everyone the leader brings into a ticket: the party, or just us.
    fn roster(&self) -> Vec<RosterSeat> {
        let Some(l) = self.party else { return self.own_seats() };
        let mut v = Vec::new();
        for m in self.s.members(l) {
            if account(m) == self.me {
                v.extend(self.own_seats());
                continue;
            }
            let mut name = self.s.member_data(l, m, key::NAME);
            if name.is_empty() {
                name = self.s.friend_name(m);
            }
            v.push((account(m), 0, name));
            for (i, g) in split_guests(&self.s.member_data(l, m, key::GUESTS)).into_iter().enumerate() {
                v.push((account(m), i as u8 + 1, g));
            }
        }
        v
    }

    // --- searching ------------------------------------------------------------------

    fn start_queue(&mut self, playlist: &str) {
        let Some(pl) = self.cfg.playlists.iter().find(|p| p.id == playlist && (p.game.is_empty() || p.game == self.game)).cloned() else {
            return self.error(err::NO_PLAYLIST, "no such playlist");
        };
        if let Some(l) = self.party {
            if account(self.s.owner(l)) != self.me {
                return self.error(err::NOT_LEADER, "only the party leader can do that");
            }
        }
        let roster = self.roster();
        if roster.len() > pl.team_size as usize && pl.teams > 1 || roster.len() > pl.players() {
            return self.error(err::PARTY_FULL, "the party is too big for that playlist");
        }
        self.leave_ticket();
        // the party's mean seniority (days with a completed set), as the
        // Live server works it out; what the game sent is not used
        let seniority = match self.party {
            Some(l) => {
                let v: Vec<u32> = self.s.members(l).iter().map(|&m| self.s.member_data(l, m, key::MEMBER_SENIORITY).parse().unwrap_or(0)).collect();
                if v.is_empty() {
                    0
                } else {
                    v.iter().sum::<u32>() / v.len() as u32
                }
            }
            None => self.seniority(),
        };
        self.matched = None;
        self.match_over = false;
        self.queue = Some(Queued {
            playlist: pl,
            seniority,
            since: Instant::now(),
            since_unix: unix_now(),
            next_search: Instant::now(),
            searching: 1,
            stage: 0,
            searching_now: false,
        });
        self.log(&format!("searching {playlist}"));
        self.queue_status();
    }

    fn queue_status(&mut self) {
        let Some(q) = &self.queue else { return };
        let waited = q.since.elapsed().as_secs().min(u16::MAX as u64) as u16;
        let st = Server::QueueStatus { playlist: q.playlist.id.clone(), stage: q.stage, waited, searching: q.searching };
        if let (Some(l), Server::QueueStatus { playlist, stage, waited, searching }) = (self.party, &st) {
            self.s.set_lobby_data(l, key::QUEUE, &format!("{playlist}|{stage}|{waited}|{searching}"));
        }
        self.emit(st);
    }

    fn queue_frame(&mut self) {
        let now = Instant::now();
        let mut status = false;
        if let Some(q) = &mut self.queue {
            let waited = q.since.elapsed().as_secs() as u32;
            let stage = q.playlist.stage_index(waited) as u8;
            if stage != q.stage || self.last_status.elapsed() >= Duration::from_secs(1) {
                q.stage = stage;
                status = true;
            }
        }
        if status {
            self.last_status = now;
            self.queue_status();
        }
        let Some(q) = &self.queue else { return };
        if q.searching_now || now < q.next_search || self.joining_ticket.is_some() || self.creating_ticket {
            return;
        }
        // a member of someone else's ticket waits for its owner
        if let Some(t) = self.ticket {
            let alone = self.s.members(t).len() <= 1;
            if account(self.s.owner(t)) != self.me || !alone {
                return;
            }
        }
        let st = &q.playlist.stages[q.playlist.stage_index(q.since.elapsed().as_secs() as u32)];
        let dist = match st.max_rtt_sum {
            0 => steam::distance::WORLDWIDE,
            1..=150 => steam::distance::DEFAULT,
            151..=300 => steam::distance::FAR,
            _ => steam::distance::WORLDWIDE,
        };
        let size = self.roster().len() as i32;
        let (game, pl) = (self.game.clone(), q.playlist.id.clone());
        let call = self.s.find_lobbies(&[(key::KIND, "ticket"), (key::GAME, &game), (key::PLAYLIST, &pl), (key::OPEN, "1")], &[(key::FREE, size)], dist, SEARCH_MAX);
        self.calls.insert(call, Pending::FindTickets);
        if let Some(q) = &mut self.queue {
            q.searching_now = true;
            q.next_search = now + SEARCH_EVERY;
        }
    }

    fn tickets_found(&mut self, found: Vec<SteamId>) {
        let roster = self.roster();
        let Some(q) = &mut self.queue else { return };
        q.searching_now = false;
        let pl = q.playlist.clone();
        let mine = Ticket {
            key: 0,
            members: roster.iter().map(|r| r.0 as u64).collect(),
            seniority: q.seniority,
            rtt_ms: 0,
            waited: q.since.elapsed().as_secs() as u32,
        };
        let my_since = (q.since_unix, self.ticket.unwrap_or(u64::MAX));
        let now = unix_now();
        let window = Duration::from_secs(pl.avoid_rematch_minutes as u64 * 60);
        let recent: Vec<u32> = self.recent.iter().filter(|(_, at)| at.elapsed() < window).map(|r| r.0).collect();
        let me = self.me;
        let recent_fn = move |a: u64, b: u64| (a as u32 == me && recent.contains(&(b as u32))) || (b as u32 == me && recent.contains(&(a as u32)));
        let size = mine.members.len();
        let mut best: Option<(bool, i64, u64, u64, SteamId)> = None; // (drop-in, -fill, since, id)
        let mut others = 0u16;
        for l in found {
            if Some(l) == self.ticket {
                continue;
            }
            others += 1;
            let free: usize = self.s.lobby_data(l, key::FREE).parse().unwrap_or(0);
            if free < size {
                continue;
            }
            let since: u64 = self.s.lobby_data(l, key::SINCE).parse().unwrap_or(now);
            let running = self.s.lobby_data(l, key::DROP_IN) == "1";
            if running && !pl.drop_in {
                continue;
            }
            let theirs = Ticket {
                key: l,
                members: self.s.lobby_data(l, key::IDS).split(',').filter_map(|x| x.parse().ok()).collect(),
                seniority: self.s.lobby_data(l, key::SENIORITY).parse().unwrap_or(0),
                rtt_ms: 0,
                waited: now.saturating_sub(since) as u32,
            };
            if !running && !compatible(&pl, &mine, &theirs, &recent_fn) {
                continue;
            }
            // our own ticket only gives way to an older one
            if !running && self.ticket.is_some() && (since, l) >= my_since {
                continue;
            }
            // drop-in matches first (fullest first), then the oldest ticket
            let cand = (!running, free as i64, since, l, l);
            if best.is_none_or(|b| (cand.0, cand.1, cand.2, cand.3) < (b.0, b.1, b.2, b.3)) {
                best = Some(cand);
            }
        }
        if let Some(q) = &mut self.queue {
            q.searching = others + 1;
        }
        match best {
            Some((_, _, _, _, l)) => {
                self.joining_ticket = Some(l);
                self.s.join_lobby(l);
            }
            None if self.ticket.is_none() => {
                self.creating_ticket = true;
                let call = self.s.create_lobby(steam::lobby_type::PUBLIC, pl.players() as i32);
                self.calls.insert(call, Pending::CreateTicket);
            }
            None => {}
        }
    }

    fn leave_ticket(&mut self) {
        if let Some(t) = self.ticket.take() {
            self.s.leave_lobby(t);
        }
        self.joining_ticket = None;
    }

    // --- the ticket and the match ---------------------------------------------------

    /// Each member's roster, in member order.
    fn ticket_rosters(&self, t: SteamId) -> Vec<(u32, Vec<RosterSeat>)> {
        self.s.members(t).into_iter().map(|m| (account(m), roster_from_json(&self.s.member_data(t, m, key::ROSTER)))).collect()
    }

    fn ticket_changed(&mut self) {
        let Some(t) = self.ticket else { return };
        let m = self.s.lobby_data(t, key::MATCH);
        if let Some(m) = Match::from_json(&m) {
            let same = self.matched.as_ref().is_some_and(|x| *x == m);
            if !same {
                self.on_match(m);
            }
        }
        self.ticket_frame();
    }

    /// The ticket's owner forms the match, and in a drop-in match keeps the
    /// seats up to date.
    fn ticket_frame(&mut self) {
        let Some(t) = self.ticket else { return };
        if account(self.s.owner(t)) != self.me {
            return;
        }
        let pl_id = self.s.lobby_data(t, key::PLAYLIST);
        let Some(pl) = self.cfg.playlists.iter().find(|p| p.id == pl_id).cloned() else { return };
        let rosters: Vec<(u32, Vec<RosterSeat>)> = self.ticket_rosters(t).into_iter().filter(|r| !r.1.is_empty()).collect();
        let current = Match::from_json(&self.s.lobby_data(t, key::MATCH));
        let players = pl.players();
        match current {
            None => {
                // take parties in member order while they fit
                let mut take: Vec<&Vec<RosterSeat>> = Vec::new();
                let mut total = 0;
                for (_, r) in &rosters {
                    if total + r.len() <= players {
                        total += r.len();
                        take.push(r);
                    }
                }
                let free = players - total;
                if self.s.lobby_data(t, key::FREE) != free.to_string() {
                    self.s.set_lobby_data(t, key::FREE, &free.to_string());
                }
                let mut ids: Vec<u32> = take.iter().flat_map(|r| r.iter().map(|s| s.0)).collect();
                ids.dedup();
                let ids = ids.iter().map(|i| i.to_string()).collect::<Vec<_>>().join(",");
                if self.s.lobby_data(t, key::IDS) != ids {
                    self.s.set_lobby_data(t, key::IDS, &ids);
                }
                let since: u64 = self.s.lobby_data(t, key::SINCE).parse().unwrap_or_else(|_| unix_now());
                let waited = unix_now().saturating_sub(since) as u32;
                let short_ok = pl.min_players > 0 && pl.past_last(waited) && total >= pl.min_players as usize;
                if total == 0 || (total < players && !short_ok) {
                    return;
                }
                let sizes: Vec<usize> = take.iter().map(|r| r.len()).collect();
                let Some(teams) = pack(&sizes, pl.teams as usize, pl.team_size as usize) else { return };
                let mut seats = Vec::new();
                for (r, team) in take.iter().zip(teams) {
                    for (id, slot, name) in r.iter() {
                        seats.push(Seat { id: *id, slot: *slot, team, name: name.clone() });
                    }
                }
                let id = rand64() | 1;
                let last = self.last_pick.get(&pl.id).cloned();
                let pick = pl.pick(id, last.as_ref().map(|(a, b)| (a.as_str(), b.as_str())));
                self.last_pick.insert(pl.id.clone(), (pick.mode.clone(), pick.map.clone()));
                let m = Match { id, playlist: pl.id.clone(), host: self.me, seats, mode: pick.mode, map: pick.map, settings: pick.settings };
                self.s.set_lobby_data(t, key::MATCH, &m.to_json());
                if pl.drop_in {
                    self.s.set_lobby_data(t, key::DROP_IN, "1");
                    self.s.set_lobby_data(t, key::FREE, &(players - m.seats.len()).to_string());
                } else {
                    self.s.set_lobby_data(t, key::OPEN, "0");
                    self.s.set_lobby_data(t, key::FREE, "0");
                    self.s.set_joinable(t, false);
                }
                self.on_match(m);
            }
            Some(mut m) if pl.drop_in && m.host == self.me && !self.match_over => {
                // seats follow who is in the lobby: newcomers drop in, leavers free theirs
                let present: HashSet<u32> = rosters.iter().map(|r| r.0).collect();
                let before = m.seats.clone();
                let in_lobby_ids: HashSet<u32> = rosters.iter().flat_map(|r| r.1.iter().map(|s| s.0)).collect();
                m.seats.retain(|s| in_lobby_ids.contains(&s.id) || present.contains(&s.id));
                for (_, r) in &rosters {
                    if r.iter().all(|s| m.seats.iter().any(|x| x.id == s.0 && x.slot == s.1)) {
                        continue;
                    }
                    if m.seats.len() + r.len() > players {
                        continue;
                    }
                    for (id, slot, name) in r {
                        m.seats.push(Seat { id: *id, slot: *slot, team: 0, name: name.clone() });
                    }
                }
                if m.seats != before {
                    self.s.set_lobby_data(t, key::MATCH, &m.to_json());
                    self.s.set_lobby_data(t, key::FREE, &(players.saturating_sub(m.seats.len())).to_string());
                    self.on_match(m);
                }
            }
            Some(m) if m.host != self.me => {
                // the host's party left: the match takes nobody else
                if self.s.lobby_data(t, key::OPEN) != "0" {
                    self.s.set_lobby_data(t, key::OPEN, "0");
                    self.s.set_lobby_data(t, key::DROP_IN, "0");
                    self.s.set_joinable(t, false);
                }
            }
            _ => {}
        }
    }

    fn on_match(&mut self, m: Match) {
        if !m.seats.iter().any(|s| s.id == self.me) {
            // a race put more parties in the ticket than it holds: look again
            if self.queue.is_some() {
                self.log("that ticket filled up without us; searching again");
                self.leave_ticket();
            }
            return;
        }
        let new = self.matched.as_ref().is_none_or(|x| x.id != m.id);
        if new {
            for s in &m.seats {
                if s.id != self.me {
                    self.recent.push((s.id, Instant::now()));
                }
            }
            self.recent.retain(|r| r.1.elapsed() < Duration::from_secs(3600));
            self.match_over = false;
            if self.queue.take().is_some() {
                if let Some(l) = self.party {
                    self.s.set_lobby_data(l, key::QUEUE, "");
                }
            }
        }
        if let Some(l) = self.party {
            if account(self.s.owner(l)) == self.me {
                self.s.set_lobby_data(l, key::MATCH, &m.to_json());
            }
        }
        for c in m.consoles() {
            if c != self.me {
                self.accepted.insert(steamid(c));
            }
        }
        self.matched = Some(m.clone());
        self.emit(m.server());
        if new {
            self.send_card_to_match();
            self.run_hook(&m);
        }
    }

    /// --on-match-host / --on-match-join, for engines without a Live client.
    fn run_hook(&mut self, m: &Match) {
        let cmd = if m.host == self.me { self.cfg.on_host.clone() } else { self.cfg.on_join.clone() };
        if self.cfg.tunnel && m.host != self.me {
            self.log(&format!("connect 127.0.0.1:{} to play", self.cfg.tunnel_port));
        }
        let Some(cmd) = cmd else { return };
        // unquoted, so a shell's word splitting gives one argument each;
        // values with spaces are left out
        let settings: Vec<String> = m.settings.iter().filter(|(_, v)| !v.contains(' ')).map(|(k, v)| format!("+set {k} {v}")).collect();
        let sh = if cfg!(windows) { ("cmd", "/C") } else { ("sh", "-c") };
        let r = std::process::Command::new(sh.0)
            .arg(sh.1)
            .arg(&cmd)
            .env("BW_MATCH_ID", m.id.to_string())
            .env("BW_HOST", m.host.to_string())
            .env("BW_PLAYLIST", &m.playlist)
            .env("BW_MODE", &m.mode)
            .env("BW_MAP", &m.map)
            .env("BW_SETTINGS", settings.join(" "))
            .env("BW_CONNECT", format!("127.0.0.1:{}", self.cfg.tunnel_port))
            .env("BW_GAME_PORT", self.cfg.game_port.to_string())
            .spawn();
        if let Err(e) = r {
            self.log(&format!("can't run {cmd}: {e}"));
        }
    }

    // --- sessions ------------------------------------------------------------------

    fn allowed(&self, peer: SteamId) -> bool {
        if self.cfg.accept_all || self.accepted.contains(&peer) {
            return true;
        }
        let id = account(peer);
        let in_lobby = |l: Option<SteamId>| l.is_some_and(|l| self.s.members(l).contains(&peer));
        in_lobby(self.party) || in_lobby(self.ticket) || self.matched.as_ref().is_some_and(|m| m.seats.iter().any(|s| s.id == id))
    }

    fn session_frame(&mut self) {
        if self.held.is_empty() {
            return;
        }
        let held = std::mem::take(&mut self.held);
        for h in held {
            if self.allowed(h.peer) {
                self.s.accept(h.peer);
                self.accepted.insert(h.peer);
            } else if h.at.elapsed() < SESSION_HOLD {
                self.held.push(h);
            } else {
                self.s.close_session(h.peer);
            }
        }
    }

    // --- tunnel (FTEQW and other engines without Live) --------------------------------

    fn tunnel_frame(&mut self) {
        if self.tunnel.is_none() {
            return;
        }
        let mut buf = [0u8; 2048];
        // the local client's packets go to the match's host
        loop {
            let r = self.tunnel.as_ref().unwrap().recv_from(&mut buf);
            let Ok((n, from)) = r else { break };
            if !from.ip().is_loopback() {
                continue;
            }
            self.tunnel_client = Some(from);
            if let Some(m) = &self.matched {
                if m.host != self.me {
                    self.traffic.0 += n as u64;
                    self.s.send(steamid(m.host), &buf[..n], steam::send::UNRELIABLE_NO_NAGLE | steam::send::AUTO_RESTART, CH_TUNNEL);
                }
            }
        }
        for (from, data) in self.s.receive(CH_TUNNEL) {
            self.traffic.1 += data.len() as u64;
            let id = account(from);
            let hosting = self.matched.as_ref().is_some_and(|m| m.host == self.me);
            if hosting {
                // a remote client: each gets its own local socket, so the
                // server sees one address per player
                if !self.tunnel_peers.contains_key(&id) {
                    let Ok(s) = UdpSocket::bind("127.0.0.1:0") else { continue };
                    let _ = s.set_nonblocking(true);
                    self.log(&format!("tunnel: {id} connects to the server on port {}", self.cfg.game_port));
                    self.tunnel_peers.insert(id, s);
                }
                let _ = self.tunnel_peers[&id].send_to(&data, ("127.0.0.1", self.cfg.game_port));
            } else if let Some(c) = self.tunnel_client {
                let _ = self.tunnel.as_ref().unwrap().send_to(&data, c);
            }
        }
        let mut out = Vec::new();
        for (&id, s) in &self.tunnel_peers {
            while let Ok((n, _)) = s.recv_from(&mut buf) {
                out.push((id, buf[..n].to_vec()));
            }
        }
        for (id, d) in out {
            self.traffic.0 += d.len() as u64;
            self.s.send(steamid(id), &d, steam::send::UNRELIABLE_NO_NAGLE | steam::send::AUTO_RESTART, CH_TUNNEL);
        }
    }

    // --- fighter records ------------------------------------------------------------

    fn load_records(&mut self) {
        let name = self.cloud_name();
        let text = self
            .s
            .cloud_read(&name)
            .and_then(|b| String::from_utf8(b).ok())
            .or_else(|| std::fs::read_to_string(&self.records_path).ok());
        if let Some(t) = text {
            match Records::from_json(&t) {
                Ok(r) => self.records = r,
                Err(e) => self.log(&format!("records: {e}")),
            }
        }
    }

    fn cloud_name(&self) -> String {
        "bwlive_records.json".into()
    }

    fn save_records(&mut self) {
        let json = self.records.to_json();
        let name = self.cloud_name();
        let cloud = self.s.cloud_write(&name, json.as_bytes());
        let _ = std::fs::write(&self.records_path, &json);
        if !cloud {
            self.log("records saved locally (Steam Cloud refused the write)");
        }
        self.publish_record();
    }

    fn my_key(&self, slot: u8) -> String {
        records::key(steamid(self.me), slot)
    }

    fn seniority(&self) -> u32 {
        let mut v: Vec<u32> = (0..=self.guests.len() as u8).map(|s| self.records.seniority(&self.my_key(s))).collect();
        if v.is_empty() {
            v.push(0);
        }
        v.iter().sum::<u32>() / v.len() as u32
    }

    fn my_card(&self) -> String {
        let json = match self.records.get(&self.my_key(0)) {
            Some(r) => r.card_json(self.me, &self.name),
            None => records::Record::default().card_json(self.me, &self.name),
        };
        id_first(&json, self.me)
    }

    fn board_name(&self) -> String {
        format!("bwlive_{}_sets", if self.game.is_empty() { "bulwark" } else { &self.game })
    }

    /// The record on the leaderboard (score: sets won; details: the counts),
    /// and Spacewar's own stats where they exist.
    fn publish_record(&mut self) {
        let Some(board) = self.board else {
            if !self.calls.values().any(|p| matches!(p, Pending::FindBoard)) {
                let call = self.s.find_board(&self.board_name());
                self.calls.insert(call, Pending::FindBoard);
            }
            return;
        };
        let Some(r) = self.records.get(&self.my_key(0)).cloned() else { return };
        let details = card_details(&r);
        let call = self.s.upload_score(board, r.wins as i32, &details);
        self.calls.insert(call, Pending::Upload);
        for (stat, v) in [("NumGames", r.sets), ("NumWins", r.wins), ("NumLosses", r.losses)] {
            if self.s.stat(stat).is_some() {
                self.s.set_stat(stat, v as i32);
            }
        }
        self.s.store_stats();
    }

    fn set_report(&mut self, mid: u64, result: u8, runback: bool, seats: &[wire::ReportSeat]) {
        let Some(m) = self.matched.clone().filter(|m| m.id == mid) else {
            return self.error(err::BAD, "no such match");
        };
        if m.host != self.me {
            return self.error(err::BAD, "only the match's host reports its sets");
        }
        if result > records::result::FORFEIT || seats.is_empty() || seats.len() > m.seats.len() {
            return self.error(err::BAD, "bad set report");
        }
        let mut seen = Vec::new();
        for s in seats {
            let in_match = m.seats.iter().any(|x| x.id == s.id && x.slot == s.slot);
            if !in_match || seen.contains(&(s.id, s.slot)) || s.outcome > records::outcome::DISCONNECT {
                return self.error(err::BAD, "bad set report");
            }
            seen.push((s.id, s.slot));
        }
        let wins = seats.iter().filter(|s| s.outcome == records::outcome::WIN).count();
        let left = seats.iter().filter(|s| s.outcome >= records::outcome::QUIT).count();
        if wins == 0 || (result == records::result::COMPLETE && left > 0) || (result == records::result::FORFEIT && left == 0) {
            return self.error(err::BAD, "bad set report");
        }
        self.set_seq += 1;
        let msg = json!({
            "t": "set", "match": mid.to_string(), "n": self.set_seq, "result": result, "runback": runback,
            "seats": seats.iter().map(|s| json!([s.id, s.slot, s.outcome, s.behind, s.games, s.hero])).collect::<Vec<_>>(),
        })
        .to_string();
        for c in m.consoles() {
            if c != self.me {
                self.s.send(steamid(c), msg.as_bytes(), steam::send::RELIABLE | steam::send::AUTO_RESTART, CH_CONTROL);
            }
        }
        self.log(&format!("set {} of match {mid} reported to {} players", self.set_seq, m.seats.len()));
        self.on_control(self.me, msg.as_bytes());
    }

    fn on_control(&mut self, from: u32, data: &[u8]) {
        let Ok(v) = serde_json::from_slice::<Value>(data) else { return };
        match v["t"].as_str() {
            Some("set") => {
                let Some(m) = self.matched.clone() else { return };
                let mid: u64 = v["match"].as_str().and_then(|s| s.parse().ok()).unwrap_or(0);
                let n = v["n"].as_u64().unwrap_or(0) as u32;
                // only the host of our match reports, once per set
                if mid != m.id || from != m.host || !self.sets_seen.insert((mid, n)) {
                    return;
                }
                let result = v["result"].as_u64().unwrap_or(0) as u8;
                let runback = v["runback"].as_bool().unwrap_or(false);
                let day = records::day_of(match self.s.server_time() {
                    0 => unix_now(),
                    t => t as u64,
                });
                let mut mine = 0;
                for s in v["seats"].as_array().into_iter().flatten() {
                    let (id, slot) = (s[0].as_u64().unwrap_or(0) as u32, s[1].as_u64().unwrap_or(9) as u8);
                    if id != self.me || slot as usize > self.guests.len() {
                        continue;
                    }
                    let name = m.seats.iter().find(|x| x.id == id && x.slot == slot).map(|x| x.name.clone()).unwrap_or_default();
                    let r = SeatResult {
                        outcome: s[2].as_u64().unwrap_or(1) as u8,
                        behind: s[3].as_u64().unwrap_or(0) != 0,
                        hero: s[5].as_str().unwrap_or("").to_string(),
                        runback,
                    };
                    let k = self.my_key(slot);
                    self.records.apply(&k, &name, &r, result, day);
                    mine += 1;
                }
                if mine > 0 {
                    self.save_records();
                    self.log(&format!("set recorded: {}", self.records.get(&self.my_key(0)).map(|r| r.form.clone()).unwrap_or_default()));
                    if let Some(l) = self.party {
                        self.s.set_member_data(l, key::MEMBER_SENIORITY, &self.seniority().to_string());
                    }
                    self.send_card_to_match();
                    let json = self.my_card();
                    self.emit(Server::CardInfo { json });
                }
            }
            Some("card") => {
                let Some(card) = v["card"].as_str() else { return };
                // a card names its own id; only the sender's own is kept
                let ok = card.starts_with(&format!("{{\"id\":{from},")) && card.len() < 768;
                if ok {
                    self.cards.insert(from, card.to_string());
                    self.card_wanted.remove(&from);
                    self.emit(Server::CardInfo { json: card.to_string() });
                }
            }
            _ => {}
        }
    }

    fn send_card_to_match(&mut self) {
        let Some(m) = &self.matched else { return };
        let msg = json!({"t": "card", "card": self.my_card()}).to_string();
        for c in m.consoles() {
            if c != self.me {
                self.s.send(steamid(c), msg.as_bytes(), steam::send::RELIABLE | steam::send::AUTO_RESTART, CH_CONTROL);
            }
        }
    }

    fn card(&mut self, player: u32) {
        let who = if player == 0 { self.me } else { player };
        if who == self.me {
            let json = self.my_card();
            return self.emit(Server::CardInfo { json });
        }
        if let Some(c) = self.cards.get(&who).cloned() {
            return self.emit(Server::CardInfo { json: c });
        }
        // not in a match with us: what the leaderboard has
        self.card_wanted.insert(who);
        match self.board {
            Some(_) => self.download_cards(vec![who]),
            None => self.publish_record(),
        }
    }

    fn download_cards(&mut self, ids: Vec<u32>) {
        let Some(board) = self.board else { return };
        let users: Vec<SteamId> = ids.iter().map(|&i| steamid(i)).collect();
        let call = self.s.download_scores(board, &users);
        self.calls.insert(call, Pending::Download(ids));
    }
}

/// A card with "id" as its first member: the engine keeps a card only when
/// it starts with {"id":N, and serde_json writes members sorted.
fn id_first(json: &str, id: u32) -> String {
    let Ok(Value::Object(mut m)) = serde_json::from_str::<Value>(json) else { return json.to_string() };
    m.remove("id");
    let rest = Value::Object(m).to_string();
    if rest.len() <= 2 {
        format!("{{\"id\":{id}}}")
    } else {
        format!("{{\"id\":{id},{}", &rest[1..])
    }
}

fn split_guests(s: &str) -> Vec<String> {
    s.split('\n').filter(|g| !g.is_empty()).take(wire::MAX_GUESTS).map(|g| g.to_string()).collect()
}

/// Leaderboard details: the record's counts, then the form strip as two
/// bits a result (W 0, L 1, Q 2, D 3), newest last, and its length.
fn card_details(r: &records::Record) -> Vec<i32> {
    let mut form = 0i32;
    let n = r.form.chars().count().min(records::FORM_LEN);
    for c in r.form.chars().take(n) {
        form = (form << 2)
            | match c {
                'W' => 0,
                'L' => 1,
                'Q' => 2,
                _ => 3,
            };
    }
    vec![
        r.sets as i32,
        r.completed as i32,
        r.wins as i32,
        r.losses as i32,
        r.quits as i32,
        r.quits_behind as i32,
        r.disconnects as i32,
        r.runbacks as i32,
        r.forfeit_wins as i32,
        r.seniority() as i32,
        form,
        n as i32,
    ]
}

/// A card from leaderboard details (no heroes: those stay in the player's
/// own record and arrive with their card in a match).
fn card_from_details(id: u32, name: &str, d: &[i32]) -> String {
    let g = |i: usize| d.get(i).copied().unwrap_or(0).max(0) as u32;
    let n = g(11) as usize;
    let mut form = String::new();
    for i in (0..n.min(records::FORM_LEN)).rev() {
        form.push(['W', 'L', 'Q', 'D'][((g(10) >> (2 * i)) & 3) as usize]);
    }
    let sets = g(0);
    let pct = |x: u32| if sets == 0 { 100 } else { (x * 100 + sets / 2) / sets };
    // the engine keys cards on a leading {"id":N, so id goes first
    let rest = json!({
        "name": name, "sets": sets, "completed": g(1), "completion": pct(g(1)), "wins": g(2), "losses": g(3),
        "quits": g(4), "quits_behind": g(5), "disconnects": g(6), "runbacks": g(7), "forfeit_wins": g(8),
        "active_days": g(9), "form": form, "heroes": [],
    })
    .to_string();
    format!("{{\"id\":{id},{}", &rest[1..])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn match_round_trips() {
        let m = Match {
            id: u64::MAX - 5,
            playlist: "duel".into(),
            host: 7,
            seats: vec![Seat { id: 7, slot: 0, team: 0, name: "ann".into() }, Seat { id: 9, slot: 1, team: 1, name: "bo \"x\"".into() }],
            mode: "set".into(),
            map: "duel_alley".into(),
            settings: vec![("g_gametype".into(), "duel".into())],
        };
        assert_eq!(Match::from_json(&m.to_json()), Some(m));
    }

    #[test]
    fn card_details_round_trip() {
        let mut r = records::Record::default();
        let seat = |o| SeatResult { outcome: o, behind: false, hero: "hero_juliet".into(), runback: false };
        for (i, o) in [0u8, 1, 0, 2, 3, 0].iter().enumerate() {
            r.apply("ann", &seat(*o), if *o >= 2 { 1 } else { 0 }, 100 + i as u32);
        }
        let card: Value = serde_json::from_str(&card_from_details(5, "ann", &card_details(&r))).unwrap();
        let want: Value = serde_json::from_str(&r.card_json(5, "ann")).unwrap();
        for k in ["sets", "completed", "completion", "wins", "losses", "quits", "disconnects", "active_days", "form"] {
            assert_eq!(card[k], want[k], "{k}");
        }
    }

    #[test]
    fn cards_start_with_their_id() {
        let c = id_first(&records::Record::default().card_json(42, "ann"), 42);
        assert!(c.starts_with("{\"id\":42,"), "{c}");
        assert!(serde_json::from_str::<Value>(&c).is_ok());
        assert!(card_from_details(7, "bo", &[]).starts_with("{\"id\":7,"));
    }

    #[test]
    fn codes_are_stable_and_valid() {
        let c = code_of(0x0186_0000_0000_1234);
        assert_eq!(c, code_of(0x0186_0000_0000_1234));
        assert_eq!(code::normalize(&c), Some(c.clone()));
        assert_ne!(c, code_of(0x0186_0000_0000_1235));
    }

    #[test]
    fn rosters_round_trip() {
        let r = vec![(1, 0, "a".to_string()), (1, 1, "b\"".to_string())];
        assert_eq!(roster_from_json(&roster_to_json(&r)), r);
    }

    #[test]
    fn steam_ids() {
        assert_eq!(steamid(22202), 76561197960287930);
        assert_eq!(account(76561197960287930), 22202);
    }
}
