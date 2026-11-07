//! The BWLV UDP wire format (docs/spec/live.md, "Wire format").

use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};

pub const MAGIC: &[u8; 4] = b"BWLV";
pub const VERSION: u8 = 1;
pub const MAX_DATAGRAM: usize = 1200;
/// `relay`, `forward` and `data` may be bigger, to carry a full game packet
/// (1400 bytes) plus their headers.
pub const MAX_CARRY: usize = 1472;
pub const HEADER: usize = 6;

pub mod t {
    pub const HELLO: u8 = 1;
    pub const BYE: u8 = 2;
    pub const PARTY_CREATE: u8 = 3;
    pub const PARTY_JOIN: u8 = 4;
    pub const PARTY_LEAVE: u8 = 5;
    pub const PARTY_SET: u8 = 6;
    pub const INVITE: u8 = 7;
    pub const QUEUE: u8 = 8;
    pub const QUEUE_CANCEL: u8 = 9;
    pub const RELAY: u8 = 10;
    pub const STATUS: u8 = 11;
    pub const LIST: u8 = 12;
    pub const MATCH_LEFT: u8 = 13;
    pub const PONG: u8 = 14;

    pub const WELCOME: u8 = 64;
    pub const ERROR: u8 = 65;
    pub const PARTY: u8 = 66;
    pub const PARTY_GONE: u8 = 67;
    pub const INVITED: u8 = 68;
    pub const INTRODUCE: u8 = 69;
    pub const FORWARD: u8 = 70;
    pub const QUEUE_STATUS: u8 = 71;
    pub const MATCH: u8 = 72;
    pub const PLAYERS: u8 = 73;
    pub const PING: u8 = 74;

    pub const PUNCH: u8 = 96;
    pub const PUNCH_ACK: u8 = 97;
    pub const DATA: u8 = 98;
}

pub mod err {
    pub const NOT_REGISTERED: u8 = 1;
    pub const VERSION: u8 = 2;
    pub const FULL: u8 = 3;
    pub const NO_CODE: u8 = 4;
    pub const PARTY_FULL: u8 = 5;
    pub const NOT_LEADER: u8 = 6;
    pub const NO_PLAYLIST: u8 = 7;
    pub const RATE: u8 = 8;
    pub const BAD: u8 = 9;
}

/// `party_gone` reasons.
pub mod gone {
    pub const LEFT: u8 = 0;
    pub const KICKED: u8 = 1;
    pub const TIMED_OUT: u8 = 2;
    pub const RESTART: u8 = 3;
}

pub const STAGE_NONE: u8 = 255;
/// Split-screen guests per console (so four local players in all).
pub const MAX_GUESTS: usize = 3;

#[derive(Clone, Debug, PartialEq)]
pub enum Client {
    /// `guests`: names of the other local (split-screen) players on this
    /// console, at most 3. Zero padding reads as no guests.
    Hello { token: u64, game: String, name: String, lan: Vec<SocketAddr>, guests: Vec<String> },
    Bye,
    PartyCreate { max: u8, flags: u8, info: String },
    PartyJoin { code: String },
    PartyLeave,
    PartySet { flags: u8, info: String },
    Invite { player: u32 },
    Queue { playlist: String, seniority: u32 },
    QueueCancel,
    Relay { to: u32, payload: Vec<u8> },
    Status { status: String },
    List,
    MatchLeft { id: u64 },
    Pong { nonce: u32 },
}

#[derive(Clone, Debug, PartialEq)]
pub struct Member {
    pub id: u32,
    pub name: String,
    pub public: SocketAddr,
    pub lan: Vec<SocketAddr>,
    /// Split-screen guests playing on this member's console.
    pub guests: Vec<String>,
}

/// A seat in a match: a console (`id`) and which of its local players
/// (`slot`, 0 = the console's own player, 1-3 = guests).
#[derive(Clone, Debug, PartialEq)]
pub struct Seat {
    pub id: u32,
    pub slot: u8,
    pub team: u8,
    pub name: String,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Listed {
    pub id: u32,
    pub name: String,
    pub status: String,
    pub code: String,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Server {
    Welcome { id: u32, public: SocketAddr, interval: u16 },
    Error { code: u8, text: String },
    Party { code: String, leader: u32, max: u8, flags: u8, info: String, members: Vec<Member> },
    PartyGone { reason: u8 },
    Invited { from: u32, name: String, code: String },
    Introduce { peer: u32, name: String, public: SocketAddr, lan: Vec<SocketAddr>, nonce: u64 },
    Forward { from: u32, payload: Vec<u8> },
    QueueStatus { playlist: String, stage: u8, waited: u16, searching: u16 },
    /// `mode`, `map` and `settings` come from the playlist's pick; all
    /// empty when the playlist has no modes.
    Match {
        id: u64,
        playlist: String,
        host: u32,
        seats: Vec<Seat>,
        mode: String,
        map: String,
        settings: Vec<(String, String)>,
    },
    Players { list: Vec<Listed> },
    Ping { nonce: u32 },
}

#[derive(Clone, Debug, PartialEq)]
pub enum Peer {
    Punch { from: u32, to: u32, nonce: u64 },
    PunchAck { from: u32, to: u32, nonce: u64 },
    Data { from: u32, payload: Vec<u8> },
}

// ---------------------------------------------------------------- writing

pub struct W(pub Vec<u8>);

impl W {
    pub fn new(ty: u8) -> Self {
        let mut v = Vec::with_capacity(64);
        v.extend_from_slice(MAGIC);
        v.push(VERSION);
        v.push(ty);
        W(v)
    }
    pub fn u8(&mut self, x: u8) -> &mut Self {
        self.0.push(x);
        self
    }
    pub fn u16(&mut self, x: u16) -> &mut Self {
        self.0.extend_from_slice(&x.to_be_bytes());
        self
    }
    pub fn u32(&mut self, x: u32) -> &mut Self {
        self.0.extend_from_slice(&x.to_be_bytes());
        self
    }
    pub fn u64(&mut self, x: u64) -> &mut Self {
        self.0.extend_from_slice(&x.to_be_bytes());
        self
    }
    pub fn str(&mut self, s: &str) -> &mut Self {
        let s = crate::party::clip(s, 255);
        self.0.push(s.len() as u8);
        self.0.extend_from_slice(s.as_bytes());
        self
    }
    pub fn addr(&mut self, a: &SocketAddr) -> &mut Self {
        match a.ip() {
            IpAddr::V4(ip) => {
                self.0.push(4);
                self.0.extend_from_slice(&ip.octets());
            }
            IpAddr::V6(ip) => {
                self.0.push(6);
                self.0.extend_from_slice(&ip.octets());
            }
        }
        self.u16(a.port())
    }
    pub fn addrs(&mut self, list: &[SocketAddr]) -> &mut Self {
        let n = list.len().min(8);
        self.0.push(n as u8);
        for a in &list[..n] {
            self.addr(a);
        }
        self
    }
    /// `u8` count, then that many `str`s (at most 3: guests).
    pub fn strs(&mut self, list: &[String]) -> &mut Self {
        let n = list.len().min(MAX_GUESTS);
        self.0.push(n as u8);
        for s in &list[..n] {
            self.str(s);
        }
        self
    }
    pub fn bytes(&mut self, b: &[u8]) -> &mut Self {
        self.0.extend_from_slice(b);
        self
    }
}

// ---------------------------------------------------------------- reading

pub struct R<'a> {
    b: &'a [u8],
    at: usize,
}

impl<'a> R<'a> {
    pub fn new(b: &'a [u8]) -> Self {
        R { b, at: 0 }
    }
    fn take(&mut self, n: usize) -> Option<&'a [u8]> {
        if self.at + n > self.b.len() {
            return None;
        }
        let s = &self.b[self.at..self.at + n];
        self.at += n;
        Some(s)
    }
    pub fn u8(&mut self) -> Option<u8> {
        self.take(1).map(|s| s[0])
    }
    pub fn u16(&mut self) -> Option<u16> {
        self.take(2).map(|s| u16::from_be_bytes([s[0], s[1]]))
    }
    pub fn u32(&mut self) -> Option<u32> {
        self.take(4).map(|s| u32::from_be_bytes(s.try_into().unwrap()))
    }
    pub fn u64(&mut self) -> Option<u64> {
        self.take(8).map(|s| u64::from_be_bytes(s.try_into().unwrap()))
    }
    pub fn str(&mut self) -> Option<String> {
        let n = self.u8()? as usize;
        let s = self.take(n)?;
        Some(String::from_utf8_lossy(s).into_owned())
    }
    pub fn addr(&mut self) -> Option<SocketAddr> {
        let ip: IpAddr = match self.u8()? {
            4 => {
                let o: [u8; 4] = self.take(4)?.try_into().ok()?;
                Ipv4Addr::from(o).into()
            }
            6 => {
                let o: [u8; 16] = self.take(16)?.try_into().ok()?;
                Ipv6Addr::from(o).into()
            }
            _ => return None,
        };
        Some(SocketAddr::new(ip, self.u16()?))
    }
    pub fn addrs(&mut self) -> Option<Vec<SocketAddr>> {
        let n = self.u8()?;
        (0..n).map(|_| self.addr()).collect()
    }
    pub fn strs(&mut self) -> Option<Vec<String>> {
        let n = (self.u8()? as usize).min(MAX_GUESTS);
        (0..n).map(|_| self.str()).collect()
    }
    pub fn rest(&mut self) -> Vec<u8> {
        let r = self.b[self.at..].to_vec();
        self.at = self.b.len();
        r
    }
}

/// Checks the header; returns (version, type, body).
pub fn header(b: &[u8]) -> Option<(u8, u8, &[u8])> {
    if b.len() < HEADER || &b[..4] != MAGIC {
        return None;
    }
    Some((b[4], b[5], &b[HEADER..]))
}

impl Client {
    pub fn encode(&self) -> Vec<u8> {
        let w = match self {
            Client::Hello { token, game, name, lan, guests } => {
                let mut w = W::new(t::HELLO);
                w.u64(*token).str(game).str(name).addrs(lan).strs(guests);
                w
            }
            Client::Bye => W::new(t::BYE),
            Client::PartyCreate { max, flags, info } => {
                let mut w = W::new(t::PARTY_CREATE);
                w.u8(*max).u8(*flags).str(info);
                w
            }
            Client::PartyJoin { code } => {
                let mut w = W::new(t::PARTY_JOIN);
                w.str(code);
                w
            }
            Client::PartyLeave => W::new(t::PARTY_LEAVE),
            Client::PartySet { flags, info } => {
                let mut w = W::new(t::PARTY_SET);
                w.u8(*flags).str(info);
                w
            }
            Client::Invite { player } => {
                let mut w = W::new(t::INVITE);
                w.u32(*player);
                w
            }
            Client::Queue { playlist, seniority } => {
                let mut w = W::new(t::QUEUE);
                w.str(playlist).u32(*seniority);
                w
            }
            Client::QueueCancel => W::new(t::QUEUE_CANCEL),
            Client::Relay { to, payload } => {
                let mut w = W::new(t::RELAY);
                w.u32(*to).bytes(payload);
                w
            }
            Client::Status { status } => {
                let mut w = W::new(t::STATUS);
                w.str(status);
                w
            }
            Client::List => W::new(t::LIST),
            Client::MatchLeft { id } => {
                let mut w = W::new(t::MATCH_LEFT);
                w.u64(*id);
                w
            }
            Client::Pong { nonce } => {
                let mut w = W::new(t::PONG);
                w.u32(*nonce);
                w
            }
        };
        w.0
    }

    /// Decodes a body of type `ty`. None = unknown type or too short.
    pub fn decode(ty: u8, body: &[u8]) -> Option<Client> {
        let mut r = R::new(body);
        Some(match ty {
            t::HELLO => {
                let (token, game, name) = (r.u64()?, r.str()?, r.str()?);
                let lan = r.addrs().unwrap_or_default();
                let guests = r.strs().unwrap_or_default();
                Client::Hello { token, game, name, lan, guests }
            }
            t::BYE => Client::Bye,
            t::PARTY_CREATE => Client::PartyCreate { max: r.u8()?, flags: r.u8()?, info: r.str().unwrap_or_default() },
            t::PARTY_JOIN => Client::PartyJoin { code: r.str()? },
            t::PARTY_LEAVE => Client::PartyLeave,
            t::PARTY_SET => Client::PartySet { flags: r.u8()?, info: r.str()? },
            t::INVITE => Client::Invite { player: r.u32()? },
            t::QUEUE => Client::Queue { playlist: r.str()?, seniority: r.u32()? },
            t::QUEUE_CANCEL => Client::QueueCancel,
            t::RELAY => Client::Relay { to: r.u32()?, payload: r.rest() },
            t::STATUS => Client::Status { status: r.str()? },
            t::LIST => Client::List,
            t::MATCH_LEFT => Client::MatchLeft { id: r.u64()? },
            t::PONG => Client::Pong { nonce: r.u32()? },
            _ => return None,
        })
    }
}

impl Server {
    pub fn encode(&self) -> Vec<u8> {
        let w = match self {
            Server::Welcome { id, public, interval } => {
                let mut w = W::new(t::WELCOME);
                w.u32(*id).addr(public).u16(*interval);
                w
            }
            Server::Error { code, text } => {
                let mut w = W::new(t::ERROR);
                w.u8(*code).str(text);
                w
            }
            Server::Party { code, leader, max, flags, info, members } => {
                let mut w = W::new(t::PARTY);
                w.str(code).u32(*leader).u8(*max).u8(*flags).str(info).u8(members.len() as u8);
                for m in members {
                    w.u32(m.id).str(&m.name).addr(&m.public).addrs(&m.lan).strs(&m.guests);
                }
                w
            }
            Server::PartyGone { reason } => {
                let mut w = W::new(t::PARTY_GONE);
                w.u8(*reason);
                w
            }
            Server::Invited { from, name, code } => {
                let mut w = W::new(t::INVITED);
                w.u32(*from).str(name).str(code);
                w
            }
            Server::Introduce { peer, name, public, lan, nonce } => {
                let mut w = W::new(t::INTRODUCE);
                w.u32(*peer).str(name).addr(public).addrs(lan).u64(*nonce);
                w
            }
            Server::Forward { from, payload } => {
                let mut w = W::new(t::FORWARD);
                w.u32(*from).bytes(payload);
                w
            }
            Server::QueueStatus { playlist, stage, waited, searching } => {
                let mut w = W::new(t::QUEUE_STATUS);
                w.str(playlist).u8(*stage).u16(*waited).u16(*searching);
                w
            }
            Server::Match { id, playlist, host, seats, mode, map, settings } => {
                let mut w = W::new(t::MATCH);
                w.u64(*id).str(playlist).u32(*host).u8(seats.len() as u8);
                for s in seats {
                    w.u32(s.id).u8(s.slot).u8(s.team).str(&s.name);
                }
                w.str(mode).str(map).u8(settings.len() as u8);
                for (k, v) in settings {
                    w.str(k).str(v);
                }
                w
            }
            Server::Players { list } => {
                let mut w = W::new(t::PLAYERS);
                let at = w.0.len();
                w.u8(0);
                let mut n = 0u8;
                for p in list {
                    let need = 4 + 3 + p.name.len() + p.status.len() + p.code.len();
                    if w.0.len() + need > MAX_DATAGRAM || n == 255 {
                        break;
                    }
                    w.u32(p.id).str(&p.name).str(&p.status).str(&p.code);
                    n += 1;
                }
                w.0[at] = n;
                w
            }
            Server::Ping { nonce } => {
                let mut w = W::new(t::PING);
                w.u32(*nonce);
                w
            }
        };
        w.0
    }

    pub fn decode(ty: u8, body: &[u8]) -> Option<Server> {
        let mut r = R::new(body);
        Some(match ty {
            t::WELCOME => Server::Welcome { id: r.u32()?, public: r.addr()?, interval: r.u16()? },
            t::ERROR => Server::Error { code: r.u8()?, text: r.str()? },
            t::PARTY => {
                let code = r.str()?;
                let leader = r.u32()?;
                let max = r.u8()?;
                let flags = r.u8()?;
                let info = r.str()?;
                let n = r.u8()?;
                let mut members = Vec::new();
                for _ in 0..n {
                    members.push(Member { id: r.u32()?, name: r.str()?, public: r.addr()?, lan: r.addrs()?, guests: r.strs()? });
                }
                Server::Party { code, leader, max, flags, info, members }
            }
            t::PARTY_GONE => Server::PartyGone { reason: r.u8()? },
            t::INVITED => Server::Invited { from: r.u32()?, name: r.str()?, code: r.str()? },
            t::INTRODUCE => Server::Introduce { peer: r.u32()?, name: r.str()?, public: r.addr()?, lan: r.addrs()?, nonce: r.u64()? },
            t::FORWARD => Server::Forward { from: r.u32()?, payload: r.rest() },
            t::QUEUE_STATUS => Server::QueueStatus { playlist: r.str()?, stage: r.u8()?, waited: r.u16()?, searching: r.u16()? },
            t::MATCH => {
                let id = r.u64()?;
                let playlist = r.str()?;
                let host = r.u32()?;
                let n = r.u8()?;
                let mut seats = Vec::new();
                for _ in 0..n {
                    seats.push(Seat { id: r.u32()?, slot: r.u8()?, team: r.u8()?, name: r.str()? });
                }
                let mode = r.str()?;
                let map = r.str()?;
                let n = r.u8()?;
                let mut settings = Vec::new();
                for _ in 0..n {
                    settings.push((r.str()?, r.str()?));
                }
                Server::Match { id, playlist, host, seats, mode, map, settings }
            }
            t::PLAYERS => {
                let n = r.u8()?;
                let mut list = Vec::new();
                for _ in 0..n {
                    list.push(Listed { id: r.u32()?, name: r.str()?, status: r.str()?, code: r.str()? });
                }
                Server::Players { list }
            }
            t::PING => Server::Ping { nonce: r.u32()? },
            _ => return None,
        })
    }
}

impl Peer {
    pub fn encode(&self) -> Vec<u8> {
        let w = match self {
            Peer::Punch { from, to, nonce } => {
                let mut w = W::new(t::PUNCH);
                w.u32(*from).u32(*to).u64(*nonce);
                w
            }
            Peer::PunchAck { from, to, nonce } => {
                let mut w = W::new(t::PUNCH_ACK);
                w.u32(*from).u32(*to).u64(*nonce);
                w
            }
            Peer::Data { from, payload } => {
                let mut w = W::new(t::DATA);
                w.u32(*from).bytes(payload);
                w
            }
        };
        w.0
    }

    pub fn decode(ty: u8, body: &[u8]) -> Option<Peer> {
        let mut r = R::new(body);
        Some(match ty {
            t::PUNCH => Peer::Punch { from: r.u32()?, to: r.u32()?, nonce: r.u64()? },
            t::PUNCH_ACK => Peer::PunchAck { from: r.u32()?, to: r.u32()?, nonce: r.u64()? },
            t::DATA => Peer::Data { from: r.u32()?, payload: r.rest() },
            _ => return None,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rt_client(m: Client) {
        let b = m.encode();
        let (v, ty, body) = header(&b).unwrap();
        assert_eq!(v, VERSION);
        assert_eq!(Client::decode(ty, body), Some(m));
    }
    fn rt_server(m: Server) {
        let b = m.encode();
        let (_, ty, body) = header(&b).unwrap();
        assert_eq!(Server::decode(ty, body), Some(m));
    }

    #[test]
    fn round_trips() {
        let a: SocketAddr = "203.0.113.5:27500".parse().unwrap();
        let b6: SocketAddr = "[2001:db8::1]:1234".parse().unwrap();
        rt_client(Client::Hello { token: 0x0102030405060708, game: "stiletto".into(), name: "sokky".into(), lan: vec![a, b6], guests: vec!["sokky (2)".into()] });
        rt_client(Client::PartyCreate { max: 4, flags: 1, info: "{}".into() });
        rt_client(Client::Relay { to: 9, payload: vec![1, 2, 3] });
        rt_client(Client::Queue { playlist: "duel".into(), seniority: 42 });
        rt_client(Client::Pong { nonce: 7 });
        rt_server(Server::Welcome { id: 3, public: a, interval: 5 });
        rt_server(Server::Party {
            code: "ABCDEF".into(), leader: 1, max: 4, flags: 1, info: "x".into(),
            members: vec![Member { id: 1, name: "a".into(), public: a, lan: vec![b6], guests: vec!["g".into()] }],
        });
        rt_server(Server::Introduce { peer: 2, name: "b".into(), public: b6, lan: vec![], nonce: 99 });
        rt_server(Server::Match { id: 5, playlist: "duel".into(), host: 1, seats: vec![
            Seat { id: 1, slot: 0, team: 0, name: "a".into() },
            Seat { id: 1, slot: 1, team: 0, name: "a (2)".into() },
            Seat { id: 2, slot: 0, team: 1, name: "b".into() },
        ], mode: String::new(), map: String::new(), settings: vec![] });
        rt_server(Server::Match { id: 6, playlist: "firefight".into(), host: 2, seats: vec![
            Seat { id: 2, slot: 0, team: 0, name: "b".into() },
        ], mode: "horde".into(), map: "coop_arena".into(),
           settings: vec![("coop_waves".into(), "1".into()), ("coop_survival".into(), "1".into())] });
        rt_server(Server::Players { list: vec![Listed { id: 1, name: "a".into(), status: "idle".into(), code: "".into() }] });
        let p = Peer::Punch { from: 1, to: 2, nonce: 3 };
        let enc = p.encode();
        let (_, ty, body) = header(&enc).unwrap();
        assert_eq!(Peer::decode(ty, body), Some(p));
    }

    #[test]
    fn padded_hello_has_no_guests() {
        let mut b = Client::Hello { token: 1, game: "g".into(), name: "n".into(), lan: vec![], guests: vec![] }.encode();
        b.resize(64, 0);
        let (_, ty, body) = header(&b).unwrap();
        let Some(Client::Hello { guests, .. }) = Client::decode(ty, body) else { panic!() };
        assert!(guests.is_empty());
    }

    #[test]
    fn short_and_unknown_dropped() {
        assert!(header(b"BWL").is_none());
        assert!(header(b"XXXX\x01\x01").is_none());
        assert_eq!(Client::decode(t::INVITE, &[0, 0]), None);
        assert_eq!(Client::decode(200, &[]), None);
    }

    #[test]
    fn players_fit_one_datagram() {
        let list: Vec<Listed> = (0..400)
            .map(|i| Listed { id: i, name: "n".repeat(30), status: "s".repeat(60), code: "ABCDEF".into() })
            .collect();
        let b = Server::Players { list }.encode();
        assert!(b.len() <= MAX_DATAGRAM);
    }
}
