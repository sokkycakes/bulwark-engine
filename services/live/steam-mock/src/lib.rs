//! A stand-in libsteam_api for testing bwlive-steam without a Steam client.
//!
//! It exports the flat Steamworks functions the bridge uses, with the same
//! names, arguments and callback struct layouts (pack 4, as on Linux), and
//! keeps the shared state (users, lobbies, leaderboards, cloud files) in
//! `steam-mock-hub`, one TCP connection per process. It copies the parts of
//! Steam's behaviour the bridge relies on, as Valve documents them:
//!
//! - only a lobby's owner sets lobby data, each member sets its own member
//!   data, and every member hears LobbyDataUpdate for both;
//! - members of a lobby you aren't in are hidden; its data is visible to a
//!   lobby search's results;
//! - a search returns public, joinable, not-full lobbies only;
//! - the owner's leaving hands the lobby to the next member;
//! - a message from someone you haven't talked to raises a session request
//!   and is held until you accept it.
//!
//! Test only: it is not Steam and never talks to Valve. ISC licence.

#![allow(non_snake_case, clippy::missing_safety_doc)]

use serde_json::{json, Value};
use std::collections::{HashMap, HashSet, VecDeque};
use std::ffi::{c_char, c_void, CStr, CString};
use std::io::{BufRead, BufReader, Write};
use std::net::TcpStream;
use std::sync::{Condvar, Mutex, OnceLock};

type Sid = u64;

fn sid(acct: u32) -> Sid {
    0x0110_0001_0000_0000 | acct as u64
}
fn lobby_sid(n: u64) -> Sid {
    0x0186_0000_0000_0000 | n
}

struct Conn {
    out: Mutex<TcpStream>,
    replies: Mutex<HashMap<u64, Value>>,
    cv: Condvar,
    next: Mutex<u64>,
}

struct Pending {
    // messages from peers we haven't accepted yet
    held: HashMap<Sid, Vec<(i32, Vec<u8>)>>,
    accepted: HashSet<Sid>,
    requested: HashSet<Sid>,
    inbox: HashMap<i32, VecDeque<(Sid, Vec<u8>)>>,
}

struct State {
    me: Sid,
    app: u32,
    name: CString,
    callbacks: VecDeque<(i32, Vec<u8>)>,
    results: HashMap<u64, (i32, Vec<u8>)>,
    current: Option<Vec<u8>>,
    next_call: u64,
    list: Vec<Sid>,
    strings: VecDeque<CString>,
    stats: HashMap<String, i32>,
    entries: HashMap<u64, Vec<(Sid, i32, i32, Vec<i32>)>>,
    next_entries: u64,
}

static CONN: OnceLock<Conn> = OnceLock::new();
static ST: Mutex<Option<State>> = Mutex::new(None);
static NET: Mutex<Option<Pending>> = Mutex::new(None);

fn conn() -> &'static Conn {
    CONN.get().expect("SteamAPI_Init first")
}

/// A request to the hub, waiting for its answer.
fn rpc(op: &str, mut args: Value) -> Value {
    let c = conn();
    let rid = {
        let mut n = c.next.lock().unwrap();
        *n += 1;
        *n
    };
    args["op"] = json!(op);
    args["rid"] = json!(rid);
    {
        let mut o = c.out.lock().unwrap();
        let _ = writeln!(o, "{args}");
    }
    let mut r = c.replies.lock().unwrap();
    loop {
        if let Some(v) = r.remove(&rid) {
            return v;
        }
        let (g, t) = c.cv.wait_timeout(r, std::time::Duration::from_secs(5)).unwrap();
        r = g;
        if t.timed_out() && !r.contains_key(&rid) {
            eprintln!("steam-mock: hub didn't answer {op}");
            return json!({});
        }
    }
}

fn st<R>(f: impl FnOnce(&mut State) -> R) -> R {
    f(ST.lock().unwrap().as_mut().expect("SteamAPI_Init first"))
}

/// Keeps a returned string alive for a while, as Steam's own buffers are.
fn keep(s: &str) -> *const c_char {
    st(|s0| {
        s0.strings.push_back(CString::new(s.replace('\0', "")).unwrap());
        if s0.strings.len() > 512 {
            s0.strings.pop_front();
        }
        s0.strings.back().unwrap().as_ptr()
    })
}

unsafe fn arg(p: *const c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

fn push_cb(id: i32, body: Vec<u8>) {
    st(|s| s.callbacks.push_back((id, body)));
}

/// A call result: SteamAPICallCompleted_t now, the result itself on
/// GetAPICallResult.
fn push_result(id: i32, body: Vec<u8>) -> u64 {
    st(|s| {
        s.next_call += 1;
        let call = s.next_call;
        let mut done = Vec::new();
        done.extend_from_slice(&call.to_le_bytes());
        done.extend_from_slice(&id.to_le_bytes());
        done.extend_from_slice(&(body.len() as u32).to_le_bytes());
        s.results.insert(call, (id, body));
        s.callbacks.push_back((703, done));
        call
    })
}

struct B(Vec<u8>);
impl B {
    fn new() -> B {
        B(Vec::new())
    }
    fn u64(mut self, v: u64) -> B {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    fn u32(mut self, v: u32) -> B {
        self.0.extend_from_slice(&v.to_le_bytes());
        self
    }
    fn u8pad(mut self, v: u8) -> B {
        self.0.extend_from_slice(&[v, 0, 0, 0]);
        self
    }
    fn zeros(mut self, n: usize) -> B {
        self.0.extend(std::iter::repeat_n(0, n));
        self
    }
}

fn identity(id: Sid) -> Vec<u8> {
    B::new().u32(16).u32(8).u64(id).zeros(120).0
}

/// Events the hub pushes, turned into callbacks.
fn on_event(v: &Value) {
    let g = |k: &str| v[k].as_u64().unwrap_or(0);
    match v["ev"].as_str().unwrap_or("") {
        "data" => push_cb(505, B::new().u64(g("lobby")).u64(g("member")).u8pad(1).0),
        "chat" => push_cb(506, B::new().u64(g("lobby")).u64(g("user")).u64(g("user")).u32(g("change") as u32).0),
        "invite" => push_cb(503, B::new().u64(g("from")).u64(g("lobby")).u64(480).0),
        "msg" => {
            let from = g("from");
            let ch = v["ch"].as_i64().unwrap_or(0) as i32;
            let data = hex_decode(v["data"].as_str().unwrap_or(""));
            let mut n = NET.lock().unwrap();
            let n = n.as_mut().unwrap();
            if n.accepted.contains(&from) {
                n.inbox.entry(ch).or_default().push_back((from, data));
            } else {
                n.held.entry(from).or_default().push((ch, data));
                if n.requested.insert(from) {
                    push_cb(1251, identity(from));
                }
            }
        }
        _ => {}
    }
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}
fn hex_decode(s: &str) -> Vec<u8> {
    (0..s.len() / 2).filter_map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).ok()).collect()
}

// --- init ---------------------------------------------------------------------

fn app_id() -> u32 {
    if let Ok(v) = std::env::var("SteamAppId") {
        return v.trim().parse().unwrap_or(0);
    }
    std::fs::read_to_string("steam_appid.txt").ok().and_then(|s| s.trim().parse().ok()).unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn SteamAPI_InitFlat(err: *mut c_char) -> i32 {
    let fail = |msg: &str| {
        if !err.is_null() {
            let m = CString::new(msg).unwrap();
            std::ptr::copy_nonoverlapping(m.as_ptr(), err, m.as_bytes().len().min(1000) + 1);
        }
        1 // k_ESteamAPIInitResult_FailedGeneric
    };
    let app = app_id();
    if app == 0 {
        return fail("no app id (steam_appid.txt)");
    }
    let acct: u32 = std::env::var("STEAM_MOCK_ACCOUNT").ok().and_then(|s| s.parse().ok()).unwrap_or(0);
    if acct == 0 {
        return fail("STEAM_MOCK_ACCOUNT isn't set");
    }
    let name = std::env::var("STEAM_MOCK_NAME").unwrap_or_else(|_| format!("player{acct}"));
    let hub = std::env::var("STEAM_MOCK_HUB").unwrap_or_else(|_| "127.0.0.1:27999".into());
    let Ok(stream) = TcpStream::connect(&hub) else {
        return fail("Steam isn't running (steam-mock-hub not reachable)");
    };
    let _ = stream.set_nodelay(true);
    let reader = stream.try_clone().unwrap();
    let _ = CONN.set(Conn { out: Mutex::new(stream), replies: Mutex::new(HashMap::new()), cv: Condvar::new(), next: Mutex::new(0) });
    *ST.lock().unwrap() = Some(State {
        me: sid(acct),
        app,
        name: CString::new(name.clone()).unwrap(),
        callbacks: VecDeque::new(),
        results: HashMap::new(),
        current: None,
        next_call: 0,
        list: Vec::new(),
        strings: VecDeque::new(),
        // Spacewar's integer stats
        stats: [("NumGames", 0), ("NumWins", 0), ("NumLosses", 0)].into_iter().map(|(k, v)| (k.to_string(), v)).collect(),
        entries: HashMap::new(),
        next_entries: 0,
    });
    *NET.lock().unwrap() =
        Some(Pending { held: HashMap::new(), accepted: HashSet::new(), requested: HashSet::new(), inbox: HashMap::new() });
    std::thread::spawn(move || {
        for line in BufReader::new(reader).lines() {
            let Ok(line) = line else { break };
            let Ok(v) = serde_json::from_str::<Value>(&line) else { continue };
            if let Some(rid) = v["rid"].as_u64() {
                let c = conn();
                c.replies.lock().unwrap().insert(rid, v);
                c.cv.notify_all();
            } else {
                on_event(&v);
            }
        }
    });
    let r = rpc("hello", json!({"id": sid(acct), "name": name, "app": app}));
    if r["ok"].as_bool() != Some(true) {
        return fail("hub refused hello");
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn SteamAPI_Shutdown() {
    let _ = rpc("bye", json!({}));
}
#[no_mangle]
pub extern "C" fn SteamAPI_GetHSteamPipe() -> i32 {
    1
}
#[no_mangle]
pub extern "C" fn SteamAPI_ManualDispatch_Init() {}
#[no_mangle]
pub extern "C" fn SteamAPI_ManualDispatch_RunFrame(_pipe: i32) {}

#[repr(C)]
pub struct CallbackMsg {
    user: i32,
    id: i32,
    param: *mut u8,
    size: i32,
}

#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ManualDispatch_GetNextCallback(_pipe: i32, out: *mut CallbackMsg) -> bool {
    st(|s| {
        let Some((id, body)) = s.callbacks.pop_front() else { return false };
        s.current = Some(body);
        let b = s.current.as_mut().unwrap();
        (*out).user = 1;
        (*out).id = id;
        (*out).param = b.as_mut_ptr();
        (*out).size = b.len() as i32;
        true
    })
}
#[no_mangle]
pub extern "C" fn SteamAPI_ManualDispatch_FreeLastCallback(_pipe: i32) {
    st(|s| s.current = None);
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ManualDispatch_GetAPICallResult(
    _pipe: i32,
    call: u64,
    out: *mut c_void,
    size: i32,
    expected: i32,
    failed: *mut bool,
) -> bool {
    st(|s| {
        let Some((id, body)) = s.results.remove(&call) else { return false };
        if id != expected || (size as usize) < body.len() {
            *failed = true;
            return true;
        }
        std::ptr::copy_nonoverlapping(body.as_ptr(), out as *mut u8, body.len());
        *failed = false;
        true
    })
}

// --- interfaces ------------------------------------------------------------------

macro_rules! accessors {
    ($($name:ident = $v:expr;)*) => { $( #[no_mangle] pub extern "C" fn $name() -> *mut c_void { $v as *mut c_void } )* };
}
accessors! {
    SteamAPI_SteamUser_v023 = 1usize;
    SteamAPI_SteamFriends_v018 = 2usize;
    SteamAPI_SteamUtils_v011 = 3usize;
    SteamAPI_SteamMatchmaking_v009 = 4usize;
    SteamAPI_SteamNetworkingMessages_SteamAPI_v002 = 5usize;
    SteamAPI_SteamNetworkingUtils_SteamAPI_v004 = 6usize;
    SteamAPI_SteamUserStats_v013 = 7usize;
    SteamAPI_SteamRemoteStorage_v016 = 8usize;
}

#[no_mangle]
pub extern "C" fn SteamAPI_ISteamUser_GetSteamID(_: *mut c_void) -> u64 {
    st(|s| s.me)
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamUtils_GetAppID(_: *mut c_void) -> u32 {
    st(|s| s.app)
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamUtils_GetServerRealTime(_: *mut c_void) -> u32 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() as u32).unwrap_or(0)
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess(_: *mut c_void) {}

// --- friends ---------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn SteamAPI_ISteamFriends_GetPersonaName(_: *mut c_void) -> *const c_char {
    st(|s| s.name.as_ptr())
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamFriends_GetFriendCount(_: *mut c_void, _flags: i32) -> i32 {
    let r = rpc("friends", json!({}));
    let ids: Vec<Sid> = r["ids"].as_array().map(|a| a.iter().filter_map(|x| x.as_u64()).collect()).unwrap_or_default();
    let n = ids.len() as i32;
    st(|s| s.list = ids);
    n
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamFriends_GetFriendByIndex(_: *mut c_void, i: i32, _flags: i32) -> u64 {
    st(|s| s.list.get(i as usize).copied().unwrap_or(0))
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamFriends_GetFriendPersonaName(_: *mut c_void, id: u64) -> *const c_char {
    let r = rpc("name", json!({"id": id}));
    keep(r["name"].as_str().unwrap_or("[unknown]"))
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamFriends_RequestUserInformation(_: *mut c_void, _id: u64, _name_only: bool) -> bool {
    false
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamFriends_SetRichPresence(_: *mut c_void, k: *const c_char, v: *const c_char) -> bool {
    rpc("rp_set", json!({"k": arg(k), "v": arg(v)}));
    true
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamFriends_GetFriendRichPresence(_: *mut c_void, id: u64, k: *const c_char) -> *const c_char {
    let r = rpc("rp_get", json!({"id": id, "k": arg(k)}));
    keep(r["v"].as_str().unwrap_or(""))
}

// --- matchmaking --------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_CreateLobby(_: *mut c_void, kind: i32, max: i32) -> u64 {
    let r = rpc("create", json!({"type": kind, "max": max}));
    let lobby = r["lobby"].as_u64().unwrap_or(0);
    let call = push_result(513, B::new().u32(if lobby != 0 { 1 } else { 2 }).u64(lobby).0);
    if lobby != 0 {
        push_cb(504, B::new().u64(lobby).u32(0).u8pad(0).u32(1).0);
    }
    call
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_JoinLobby(_: *mut c_void, lobby: u64) -> u64 {
    let r = rpc("join", json!({"lobby": lobby}));
    let resp = r["resp"].as_u64().unwrap_or(5) as u32;
    let body = B::new().u64(lobby).u32(0).u8pad(0).u32(resp).0;
    push_cb(504, body.clone());
    push_result(504, body)
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_LeaveLobby(_: *mut c_void, lobby: u64) {
    rpc("leave", json!({"lobby": lobby}));
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_InviteUserToLobby(_: *mut c_void, lobby: u64, who: u64) -> bool {
    rpc("invite", json!({"lobby": lobby, "to": who}))["ok"].as_bool().unwrap_or(false)
}
fn info(lobby: u64) -> Value {
    rpc("info", json!({"lobby": lobby}))
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(_: *mut c_void, lobby: u64) -> i32 {
    info(lobby)["members"].as_array().map(|a| a.len() as i32).unwrap_or(0)
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(_: *mut c_void, lobby: u64, i: i32) -> u64 {
    info(lobby)["members"][i as usize].as_u64().unwrap_or(0)
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamMatchmaking_GetLobbyData(_: *mut c_void, lobby: u64, k: *const c_char) -> *const c_char {
    let v = info(lobby);
    keep(v["data"][arg(k)].as_str().unwrap_or(""))
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamMatchmaking_SetLobbyData(_: *mut c_void, lobby: u64, k: *const c_char, v: *const c_char) -> bool {
    rpc("set", json!({"lobby": lobby, "k": arg(k), "v": arg(v)}))["ok"].as_bool().unwrap_or(false)
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamMatchmaking_GetLobbyMemberData(
    _: *mut c_void,
    lobby: u64,
    who: u64,
    k: *const c_char,
) -> *const c_char {
    let v = info(lobby);
    keep(v["mdata"][who.to_string()][arg(k)].as_str().unwrap_or(""))
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamMatchmaking_SetLobbyMemberData(_: *mut c_void, lobby: u64, k: *const c_char, v: *const c_char) {
    rpc("mset", json!({"lobby": lobby, "k": arg(k), "v": arg(v)}));
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_GetLobbyOwner(_: *mut c_void, lobby: u64) -> u64 {
    info(lobby)["owner"].as_u64().unwrap_or(0)
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_SetLobbyType(_: *mut c_void, lobby: u64, kind: i32) -> bool {
    rpc("attr", json!({"lobby": lobby, "type": kind}))["ok"].as_bool().unwrap_or(false)
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_SetLobbyJoinable(_: *mut c_void, lobby: u64, yes: bool) -> bool {
    rpc("attr", json!({"lobby": lobby, "joinable": yes}))["ok"].as_bool().unwrap_or(false)
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit(_: *mut c_void, lobby: u64, n: i32) -> bool {
    rpc("attr", json!({"lobby": lobby, "max": n}))["ok"].as_bool().unwrap_or(false)
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit(_: *mut c_void, lobby: u64) -> i32 {
    info(lobby)["max"].as_i64().unwrap_or(0) as i32
}

static FILTERS: Mutex<Vec<Value>> = Mutex::new(Vec::new());

#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter(
    _: *mut c_void,
    k: *const c_char,
    v: *const c_char,
    cmp: i32,
) {
    FILTERS.lock().unwrap().push(json!({"k": arg(k), "s": arg(v), "cmp": cmp}));
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter(_: *mut c_void, k: *const c_char, v: i32, cmp: i32) {
    FILTERS.lock().unwrap().push(json!({"k": arg(k), "n": v, "cmp": cmp}));
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter(_: *mut c_void, _d: i32) {}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter(_: *mut c_void, n: i32) {
    FILTERS.lock().unwrap().push(json!({"max": n}));
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_RequestLobbyList(_: *mut c_void) -> u64 {
    let f = std::mem::take(&mut *FILTERS.lock().unwrap());
    let r = rpc("find", json!({"filters": f}));
    let ids: Vec<Sid> = r["lobbies"].as_array().map(|a| a.iter().filter_map(|x| x.as_u64()).collect()).unwrap_or_default();
    let n = ids.len() as u32;
    st(|s| s.list = ids);
    push_result(510, B::new().u32(n).0)
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_GetLobbyByIndex(_: *mut c_void, i: i32) -> u64 {
    st(|s| s.list.get(i as usize).copied().unwrap_or(0))
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamMatchmaking_RequestLobbyData(_: *mut c_void, lobby: u64) -> bool {
    push_cb(505, B::new().u64(lobby).u64(lobby).u8pad(1).0);
    true
}

// --- networking messages ---------------------------------------------------------------

unsafe fn ident_id(p: *const u8) -> Sid {
    *(p.add(8) as *const u64)
}

#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamNetworkingMessages_SendMessageToUser(
    _: *mut c_void,
    to: *const u8,
    data: *const c_void,
    len: u32,
    _flags: i32,
    ch: i32,
) -> i32 {
    let to = ident_id(to);
    // talking to someone opens the session from our side
    NET.lock().unwrap().as_mut().unwrap().accepted.insert(to);
    flush_held(to);
    let body = std::slice::from_raw_parts(data as *const u8, len as usize);
    let c = conn();
    let mut o = c.out.lock().unwrap();
    let _ = writeln!(o, "{}", json!({"op": "send", "to": to, "ch": ch, "data": hex(body)}));
    1
}

fn flush_held(peer: Sid) {
    let mut n = NET.lock().unwrap();
    let n = n.as_mut().unwrap();
    if let Some(list) = n.held.remove(&peer) {
        for (ch, d) in list {
            n.inbox.entry(ch).or_default().push_back((peer, d));
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(_: *mut c_void, who: *const u8) -> bool {
    let who = ident_id(who);
    NET.lock().unwrap().as_mut().unwrap().accepted.insert(who);
    flush_held(who);
    true
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(_: *mut c_void, who: *const u8) -> bool {
    let who = ident_id(who);
    let mut n = NET.lock().unwrap();
    let n = n.as_mut().unwrap();
    n.accepted.remove(&who);
    n.requested.remove(&who);
    n.held.remove(&who);
    true
}

const MSG_SIZE: usize = 216;

#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(_: *mut c_void, ch: i32, out: *mut *mut u8, max: i32) -> i32 {
    let mut n = NET.lock().unwrap();
    let q = n.as_mut().unwrap().inbox.entry(ch).or_default();
    let mut k = 0;
    while k < max {
        let Some((from, data)) = q.pop_front() else { break };
        let m = Box::into_raw(vec![0u8; MSG_SIZE].into_boxed_slice()) as *mut u8;
        let d = Box::into_raw(data.clone().into_boxed_slice()) as *mut u8;
        *(m as *mut *mut u8) = d;
        *(m.add(8) as *mut i32) = data.len() as i32;
        std::ptr::copy_nonoverlapping(identity(from).as_ptr(), m.add(16), 136);
        *(m.add(192) as *mut i32) = ch;
        *out.add(k as usize) = m;
        k += 1;
    }
    k
}

#[no_mangle]
pub unsafe extern "C" fn SteamAPI_SteamNetworkingMessage_t_Release(m: *mut u8) {
    if m.is_null() {
        return;
    }
    let d = *(m as *mut *mut u8);
    let len = *(m.add(8) as *const i32) as usize;
    if !d.is_null() {
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(d, len)));
    }
    drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(m, MSG_SIZE)));
}

// --- stats, leaderboards, cloud ------------------------------------------------------------

#[no_mangle]
pub extern "C" fn SteamAPI_ISteamUserStats_RequestCurrentStats(_: *mut c_void) -> bool {
    let me = st(|s| s.me);
    push_cb(1101, B::new().u64(480).u32(1).u64(me).0);
    true
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamUserStats_GetStatInt32(_: *mut c_void, k: *const c_char, out: *mut i32) -> bool {
    let k = arg(k);
    st(|s| match s.stats.get(&k) {
        Some(v) => {
            *out = *v;
            true
        }
        None => false,
    })
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamUserStats_SetStatInt32(_: *mut c_void, k: *const c_char, v: i32) -> bool {
    let k = arg(k);
    st(|s| match s.stats.get_mut(&k) {
        Some(x) => {
            *x = v;
            true
        }
        None => false,
    })
}
#[no_mangle]
pub extern "C" fn SteamAPI_ISteamUserStats_StoreStats(_: *mut c_void) -> bool {
    let stats = st(|s| s.stats.clone());
    rpc("stats", json!({"stats": stats}));
    true
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamUserStats_FindOrCreateLeaderboard(_: *mut c_void, name: *const c_char, _sort: i32, _disp: i32) -> u64 {
    let r = rpc("board", json!({"name": arg(name)}));
    let h = r["handle"].as_u64().unwrap_or(0);
    push_result(1104, B::new().u64(h).u8pad(u8::from(h != 0)).0)
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamUserStats_UploadLeaderboardScore(
    _: *mut c_void,
    board: u64,
    _method: i32,
    score: i32,
    details: *const i32,
    n: i32,
) -> u64 {
    let d: Vec<i32> = if details.is_null() { Vec::new() } else { std::slice::from_raw_parts(details, n.clamp(0, 64) as usize).to_vec() };
    let r = rpc("upload", json!({"board": board, "score": score, "details": d}));
    let ok = r["ok"].as_bool().unwrap_or(false);
    push_result(1106, B::new().u8pad(u8::from(ok)).u64(board).u32(score as u32).u8pad(1).u32(1).u32(0).0)
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers(_: *mut c_void, board: u64, users: *const u64, n: i32) -> u64 {
    let ids: Vec<u64> = std::slice::from_raw_parts(users, n.max(0) as usize).to_vec();
    let r = rpc("download", json!({"board": board, "users": ids}));
    let list: Vec<(Sid, i32, i32, Vec<i32>)> = r["entries"]
        .as_array()
        .map(|a| {
            a.iter()
                .map(|e| {
                    (
                        e[0].as_u64().unwrap_or(0),
                        e[1].as_i64().unwrap_or(0) as i32,
                        e[2].as_i64().unwrap_or(0) as i32,
                        e[3].as_array().map(|d| d.iter().map(|x| x.as_i64().unwrap_or(0) as i32).collect()).unwrap_or_default(),
                    )
                })
                .collect()
        })
        .unwrap_or_default();
    let count = list.len() as u32;
    let h = st(|s| {
        s.next_entries += 1;
        s.entries.insert(s.next_entries, list);
        s.next_entries
    });
    push_result(1105, B::new().u64(board).u64(h).u32(count).0)
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry(
    _: *mut c_void,
    h: u64,
    i: i32,
    out: *mut u8,
    details: *mut i32,
    max: i32,
) -> bool {
    st(|s| {
        let Some(e) = s.entries.get(&h).and_then(|l| l.get(i as usize)) else { return false };
        let n = e.3.len().min(max.max(0) as usize);
        let b = B::new().u64(e.0).u32(e.1 as u32).u32(e.2 as u32).u32(e.3.len() as u32).u64(0).0;
        std::ptr::copy_nonoverlapping(b.as_ptr(), out, 28);
        if !details.is_null() {
            std::ptr::copy_nonoverlapping(e.3.as_ptr(), details, n);
        }
        true
    })
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamRemoteStorage_FileWrite(_: *mut c_void, name: *const c_char, data: *const c_void, n: i32) -> bool {
    let b = std::slice::from_raw_parts(data as *const u8, n.max(0) as usize);
    rpc("cloud_write", json!({"name": arg(name), "data": hex(b)}))["ok"].as_bool().unwrap_or(false)
}
fn cloud_get(name: &str) -> Option<Vec<u8>> {
    let r = rpc("cloud_read", json!({"name": name}));
    r["data"].as_str().map(hex_decode)
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamRemoteStorage_GetFileSize(_: *mut c_void, name: *const c_char) -> i32 {
    cloud_get(&arg(name)).map(|d| d.len() as i32).unwrap_or(0)
}
#[no_mangle]
pub unsafe extern "C" fn SteamAPI_ISteamRemoteStorage_FileRead(_: *mut c_void, name: *const c_char, out: *mut c_void, n: i32) -> i32 {
    let Some(d) = cloud_get(&arg(name)) else { return 0 };
    let k = d.len().min(n.max(0) as usize);
    std::ptr::copy_nonoverlapping(d.as_ptr(), out as *mut u8, k);
    k as i32
}

#[allow(dead_code)]
fn _unused() -> Sid {
    lobby_sid(0)
}
