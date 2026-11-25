//! The Steamworks flat API, loaded at run time.
//!
//! Nothing from the Steamworks SDK is needed to build this: the few
//! functions used are looked up by name in `libsteam_api.so` /
//! `steam_api64.dll` when the bridge starts, and callbacks are read with
//! manual dispatch. Struct offsets are the ones the SDK's headers give on
//! each platform (pack 4 on Linux and macOS, pack 8 on Windows); the
//! layouts and how they were checked are in docs/spec/live-steam.md.

#![allow(non_snake_case, dead_code)]

use std::ffi::{c_char, c_void, CStr, CString};

pub type SteamId = u64;
pub type ApiCall = u64;
type Iface = *mut c_void;

/// Callback structs use `#pragma pack(4)` everywhere but Windows.
const P8: bool = cfg!(windows);

/// SteamID64 of an individual account in the public universe.
pub fn steamid(account: u32) -> SteamId {
    0x0110_0001_0000_0000 | account as u64
}
pub fn account(id: SteamId) -> u32 {
    id as u32
}

pub mod lobby_type {
    pub const PRIVATE: i32 = 0;
    pub const FRIENDS_ONLY: i32 = 1;
    pub const PUBLIC: i32 = 2;
}
pub mod cmp {
    pub const EQUAL: i32 = 0;
    pub const GREATER_OR_EQUAL: i32 = 2;
}
pub mod distance {
    pub const DEFAULT: i32 = 1;
    pub const FAR: i32 = 2;
    pub const WORLDWIDE: i32 = 3;
}
pub mod send {
    pub const UNRELIABLE_NO_NAGLE: i32 = 1;
    pub const RELIABLE: i32 = 8;
    pub const AUTO_RESTART: i32 = 32;
}
pub mod cb {
    pub const GAME_LOBBY_JOIN_REQUESTED: i32 = 333;
    pub const GAME_RICH_PRESENCE_JOIN_REQUESTED: i32 = 337;
    pub const LOBBY_INVITE: i32 = 503;
    pub const LOBBY_ENTER: i32 = 504;
    pub const LOBBY_DATA_UPDATE: i32 = 505;
    pub const LOBBY_CHAT_UPDATE: i32 = 506;
    pub const LOBBY_MATCH_LIST: i32 = 510;
    pub const LOBBY_CREATED: i32 = 513;
    pub const API_CALL_COMPLETED: i32 = 703;
    pub const USER_STATS_RECEIVED: i32 = 1101;
    pub const LEADERBOARD_FIND_RESULT: i32 = 1104;
    pub const LEADERBOARD_SCORES_DOWNLOADED: i32 = 1105;
    pub const LEADERBOARD_SCORE_UPLOADED: i32 = 1106;
    pub const NET_SESSION_REQUEST: i32 = 1251;
    pub const NET_SESSION_FAILED: i32 = 1252;
}

/// What a callback or call result says, decoded from its struct.
#[derive(Debug, Clone)]
pub enum Event {
    LobbyCreated { call: ApiCall, ok: bool, lobby: SteamId },
    LobbyEnter { lobby: SteamId, response: u32 },
    LobbyDataUpdate { lobby: SteamId, member: SteamId, ok: bool },
    LobbyChatUpdate { lobby: SteamId, user: SteamId, change: u32 },
    LobbyMatchList { call: ApiCall, count: u32 },
    LobbyInvite { user: SteamId, lobby: SteamId },
    JoinRequested { lobby: SteamId, friend: SteamId },
    RichPresenceJoin { friend: SteamId, connect: String },
    SessionRequest { peer: SteamId },
    SessionFailed { peer: SteamId },
    StatsReceived { ok: bool, user: SteamId },
    LeaderboardFound { call: ApiCall, board: u64, found: bool },
    ScoresDownloaded { call: ApiCall, board: u64, entries: u64, count: i32 },
    ScoreUploaded { call: ApiCall, ok: bool },
    Other(i32),
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    b.get(o..o + 4).map(|s| u32::from_le_bytes(s.try_into().unwrap())).unwrap_or(0)
}
fn u64_at(b: &[u8], o: usize) -> u64 {
    b.get(o..o + 8).map(|s| u64::from_le_bytes(s.try_into().unwrap())).unwrap_or(0)
}
fn u8_at(b: &[u8], o: usize) -> u8 {
    b.get(o).copied().unwrap_or(0)
}
/// The offset of a u64 that follows a 4-byte field at `o`.
fn after4(o: usize) -> usize {
    if P8 {
        (o + 4 + 7) & !7
    } else {
        o + 4
    }
}

pub fn decode(id: i32, call: ApiCall, b: &[u8]) -> Event {
    use cb::*;
    match id {
        LOBBY_CREATED => Event::LobbyCreated { call, ok: u32_at(b, 0) == 1, lobby: u64_at(b, after4(0)) },
        LOBBY_ENTER => Event::LobbyEnter { lobby: u64_at(b, 0), response: u32_at(b, 16) },
        LOBBY_DATA_UPDATE => Event::LobbyDataUpdate { lobby: u64_at(b, 0), member: u64_at(b, 8), ok: u8_at(b, 16) != 0 },
        LOBBY_CHAT_UPDATE => Event::LobbyChatUpdate { lobby: u64_at(b, 0), user: u64_at(b, 8), change: u32_at(b, 24) },
        LOBBY_MATCH_LIST => Event::LobbyMatchList { call, count: u32_at(b, 0) },
        LOBBY_INVITE => Event::LobbyInvite { user: u64_at(b, 0), lobby: u64_at(b, 8) },
        GAME_LOBBY_JOIN_REQUESTED => Event::JoinRequested { lobby: u64_at(b, 0), friend: u64_at(b, 8) },
        GAME_RICH_PRESENCE_JOIN_REQUESTED => {
            let s = b.get(8..8 + 256).unwrap_or(&[]);
            let end = s.iter().position(|&c| c == 0).unwrap_or(s.len());
            Event::RichPresenceJoin { friend: u64_at(b, 0), connect: String::from_utf8_lossy(&s[..end]).into_owned() }
        }
        // SteamNetworkingIdentity: type @0, size @4, steamid @8
        NET_SESSION_REQUEST => Event::SessionRequest { peer: u64_at(b, 8) },
        // SteamNetConnectionInfo_t starts with the remote identity
        NET_SESSION_FAILED => Event::SessionFailed { peer: u64_at(b, 8) },
        USER_STATS_RECEIVED => Event::StatsReceived { ok: u32_at(b, 8) == 1, user: u64_at(b, after4(8)) },
        LEADERBOARD_FIND_RESULT => Event::LeaderboardFound { call, board: u64_at(b, 0), found: u8_at(b, 8) != 0 },
        LEADERBOARD_SCORES_DOWNLOADED => {
            Event::ScoresDownloaded { call, board: u64_at(b, 0), entries: u64_at(b, 8), count: u32_at(b, 16) as i32 }
        }
        LEADERBOARD_SCORE_UPLOADED => Event::ScoreUploaded { call, ok: u8_at(b, 0) != 0 },
        _ => Event::Other(id),
    }
}

/// Size of each call result's struct, for GetAPICallResult.
fn result_size(id: i32) -> usize {
    use cb::*;
    match id {
        LOBBY_CREATED => if P8 { 16 } else { 12 },
        LOBBY_ENTER => 20,
        LOBBY_MATCH_LIST => 4,
        LEADERBOARD_FIND_RESULT => if P8 { 16 } else { 12 },
        LEADERBOARD_SCORES_DOWNLOADED => if P8 { 24 } else { 20 },
        LEADERBOARD_SCORE_UPLOADED => if P8 { 32 } else { 28 },
        USER_STATS_RECEIVED => if P8 { 24 } else { 20 },
        _ => 1024,
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Identity {
    kind: i32,
    size: i32,
    id: u64,
    rest: [u8; 120],
}
const _: () = assert!(std::mem::size_of::<Identity>() == 136);

impl Identity {
    pub fn steam(id: SteamId) -> Self {
        Identity { kind: 16, size: 8, id, rest: [0; 120] }
    }
}

/// CallbackMsg_t
#[repr(C)]
struct CallbackMsg {
    user: i32,
    id: i32,
    param: *mut u8,
    size: i32,
}

/// A leaderboard entry as GetDownloadedLeaderboardEntry fills it.
#[derive(Debug, Clone, Default)]
pub struct BoardEntry {
    pub user: SteamId,
    pub rank: i32,
    pub score: i32,
    pub details: Vec<i32>,
}

macro_rules! api {
    ($( $name:ident : fn($($arg:ty),*) $(-> $ret:ty)? ;)*) => {
        #[allow(dead_code)]
        struct Fns { $( $name: Option<unsafe extern "C" fn($($arg),*) $(-> $ret)?>, )* }
        impl Fns {
            unsafe fn load(lib: &Lib) -> Fns {
                Fns { $( $name: lib.sym(concat!("SteamAPI_", stringify!($name))).map(|p| std::mem::transmute(p)), )* }
            }
        }
    };
}

api! {
    InitFlat: fn(*mut c_char) -> i32;
    Init: fn() -> bool;
    Shutdown: fn();
    GetHSteamPipe: fn() -> i32;
    ManualDispatch_Init: fn();
    ManualDispatch_RunFrame: fn(i32);
    ManualDispatch_GetNextCallback: fn(i32, *mut CallbackMsg) -> bool;
    ManualDispatch_FreeLastCallback: fn(i32);
    ManualDispatch_GetAPICallResult: fn(i32, u64, *mut c_void, i32, i32, *mut bool) -> bool;
    ISteamUser_GetSteamID: fn(Iface) -> u64;
    ISteamFriends_GetPersonaName: fn(Iface) -> *const c_char;
    ISteamFriends_GetFriendCount: fn(Iface, i32) -> i32;
    ISteamFriends_GetFriendByIndex: fn(Iface, i32, i32) -> u64;
    ISteamFriends_GetFriendPersonaName: fn(Iface, u64) -> *const c_char;
    ISteamFriends_SetRichPresence: fn(Iface, *const c_char, *const c_char) -> bool;
    ISteamFriends_GetFriendRichPresence: fn(Iface, u64, *const c_char) -> *const c_char;
    ISteamFriends_RequestUserInformation: fn(Iface, u64, bool) -> bool;
    ISteamUtils_GetAppID: fn(Iface) -> u32;
    ISteamUtils_GetServerRealTime: fn(Iface) -> u32;
    ISteamMatchmaking_CreateLobby: fn(Iface, i32, i32) -> u64;
    ISteamMatchmaking_JoinLobby: fn(Iface, u64) -> u64;
    ISteamMatchmaking_LeaveLobby: fn(Iface, u64);
    ISteamMatchmaking_InviteUserToLobby: fn(Iface, u64, u64) -> bool;
    ISteamMatchmaking_GetNumLobbyMembers: fn(Iface, u64) -> i32;
    ISteamMatchmaking_GetLobbyMemberByIndex: fn(Iface, u64, i32) -> u64;
    ISteamMatchmaking_GetLobbyData: fn(Iface, u64, *const c_char) -> *const c_char;
    ISteamMatchmaking_SetLobbyData: fn(Iface, u64, *const c_char, *const c_char) -> bool;
    ISteamMatchmaking_GetLobbyMemberData: fn(Iface, u64, u64, *const c_char) -> *const c_char;
    ISteamMatchmaking_SetLobbyMemberData: fn(Iface, u64, *const c_char, *const c_char);
    ISteamMatchmaking_GetLobbyOwner: fn(Iface, u64) -> u64;
    ISteamMatchmaking_SetLobbyType: fn(Iface, u64, i32) -> bool;
    ISteamMatchmaking_SetLobbyJoinable: fn(Iface, u64, bool) -> bool;
    ISteamMatchmaking_SetLobbyMemberLimit: fn(Iface, u64, i32) -> bool;
    ISteamMatchmaking_GetLobbyMemberLimit: fn(Iface, u64) -> i32;
    ISteamMatchmaking_RequestLobbyList: fn(Iface) -> u64;
    ISteamMatchmaking_AddRequestLobbyListStringFilter: fn(Iface, *const c_char, *const c_char, i32);
    ISteamMatchmaking_AddRequestLobbyListNumericalFilter: fn(Iface, *const c_char, i32, i32);
    ISteamMatchmaking_AddRequestLobbyListDistanceFilter: fn(Iface, i32);
    ISteamMatchmaking_AddRequestLobbyListResultCountFilter: fn(Iface, i32);
    ISteamMatchmaking_GetLobbyByIndex: fn(Iface, i32) -> u64;
    ISteamMatchmaking_RequestLobbyData: fn(Iface, u64) -> bool;
    ISteamNetworkingMessages_SendMessageToUser: fn(Iface, *const Identity, *const c_void, u32, i32, i32) -> i32;
    ISteamNetworkingMessages_ReceiveMessagesOnChannel: fn(Iface, i32, *mut *mut u8, i32) -> i32;
    ISteamNetworkingMessages_AcceptSessionWithUser: fn(Iface, *const Identity) -> bool;
    ISteamNetworkingMessages_CloseSessionWithUser: fn(Iface, *const Identity) -> bool;
    SteamNetworkingMessage_t_Release: fn(*mut u8);
    ISteamNetworkingUtils_InitRelayNetworkAccess: fn(Iface);
    ISteamUserStats_RequestCurrentStats: fn(Iface) -> bool;
    ISteamUserStats_GetStatInt32: fn(Iface, *const c_char, *mut i32) -> bool;
    ISteamUserStats_SetStatInt32: fn(Iface, *const c_char, i32) -> bool;
    ISteamUserStats_StoreStats: fn(Iface) -> bool;
    ISteamUserStats_FindOrCreateLeaderboard: fn(Iface, *const c_char, i32, i32) -> u64;
    ISteamUserStats_UploadLeaderboardScore: fn(Iface, u64, i32, i32, *const i32, i32) -> u64;
    ISteamUserStats_DownloadLeaderboardEntriesForUsers: fn(Iface, u64, *const u64, i32) -> u64;
    ISteamUserStats_GetDownloadedLeaderboardEntry: fn(Iface, u64, i32, *mut u8, *mut i32, i32) -> bool;
    ISteamRemoteStorage_FileWrite: fn(Iface, *const c_char, *const c_void, i32) -> bool;
    ISteamRemoteStorage_FileRead: fn(Iface, *const c_char, *mut c_void, i32) -> i32;
    ISteamRemoteStorage_GetFileSize: fn(Iface, *const c_char) -> i32;
}

/// A shared library, opened by path or name.
struct Lib(*mut c_void);

#[cfg(unix)]
mod dl {
    use std::ffi::{c_char, c_int, c_void};
    #[cfg_attr(target_os = "linux", link(name = "dl"))]
    extern "C" {
        pub fn dlopen(path: *const c_char, flags: c_int) -> *mut c_void;
        pub fn dlsym(lib: *mut c_void, name: *const c_char) -> *mut c_void;
        pub fn dlerror() -> *const c_char;
    }
    pub const RTLD_NOW: c_int = 2;
}

#[cfg(windows)]
mod dl {
    use std::ffi::{c_char, c_void};
    #[link(name = "kernel32")]
    extern "system" {
        pub fn LoadLibraryA(path: *const c_char) -> *mut c_void;
        pub fn GetProcAddress(lib: *mut c_void, name: *const c_char) -> *mut c_void;
    }
}

impl Lib {
    fn open(path: &str) -> Result<Lib, String> {
        let c = CString::new(path).map_err(|e| e.to_string())?;
        #[cfg(unix)]
        unsafe {
            let h = dl::dlopen(c.as_ptr(), dl::RTLD_NOW);
            if h.is_null() {
                let e = dl::dlerror();
                return Err(if e.is_null() { path.to_string() } else { CStr::from_ptr(e).to_string_lossy().into_owned() });
            }
            Ok(Lib(h))
        }
        #[cfg(windows)]
        unsafe {
            let h = dl::LoadLibraryA(c.as_ptr());
            if h.is_null() {
                return Err(format!("can't load {path}"));
            }
            Ok(Lib(h))
        }
    }
    fn sym(&self, name: &str) -> Option<*mut c_void> {
        let c = CString::new(name).ok()?;
        #[cfg(unix)]
        let p = unsafe { dl::dlsym(self.0, c.as_ptr()) };
        #[cfg(windows)]
        let p = unsafe { dl::GetProcAddress(self.0, c.as_ptr()) };
        (!p.is_null()).then_some(p)
    }
}

pub struct Steam {
    f: Fns,
    pipe: i32,
    user: Iface,
    friends: Iface,
    utils: Iface,
    mm: Iface,
    net: Iface,
    netutils: Iface,
    stats: Iface,
    storage: Iface,
}

fn cs(s: &str) -> CString {
    CString::new(s.replace('\0', "")).unwrap()
}
unsafe fn from_c(p: *const c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

/// The library's default name on this platform.
pub fn default_lib() -> &'static str {
    if cfg!(windows) {
        "steam_api64.dll"
    } else if cfg!(target_os = "macos") {
        "libsteam_api.dylib"
    } else {
        "libsteam_api.so"
    }
}

/// Finds an interface accessor such as SteamAPI_SteamFriends_v018, newest
/// version first.
fn accessor(lib: &Lib, base: &str, versions: &[&str]) -> Result<Iface, String> {
    for v in versions {
        if let Some(p) = lib.sym(&format!("SteamAPI_{base}_{v}")) {
            let f: unsafe extern "C" fn() -> Iface = unsafe { std::mem::transmute(p) };
            let i = unsafe { f() };
            if !i.is_null() {
                return Ok(i);
            }
        }
    }
    Err(format!("{base} isn't available (tried {})", versions.join(", ")))
}

impl Steam {
    /// Loads the library and starts the Steam API. The app id comes from
    /// steam_appid.txt in the working directory (or SteamAppId in the
    /// environment), as with any game started outside Steam.
    pub fn init(path: &str) -> Result<Steam, String> {
        let lib = Lib::open(path)?;
        let f = unsafe { Fns::load(&lib) };
        unsafe {
            if let Some(init) = f.InitFlat {
                let mut err = vec![0 as c_char; 1024];
                let r = init(err.as_mut_ptr());
                if r != 0 {
                    return Err(format!("SteamAPI_InitFlat failed ({r}): {}", from_c(err.as_ptr())));
                }
            } else if let Some(init) = f.Init {
                if !init() {
                    return Err("SteamAPI_Init failed: is Steam running, and is steam_appid.txt here?".into());
                }
            } else {
                return Err(format!("{path} has neither SteamAPI_InitFlat nor SteamAPI_Init"));
            }
            (f.ManualDispatch_Init.ok_or("no manual dispatch in this libsteam_api")?)();
            let pipe = (f.GetHSteamPipe.ok_or("no SteamAPI_GetHSteamPipe")?)();
            let s = Steam {
                user: accessor(&lib, "SteamUser", &["v023", "v022", "v021", "v020"])?,
                friends: accessor(&lib, "SteamFriends", &["v018", "v017"])?,
                utils: accessor(&lib, "SteamUtils", &["v011", "v010", "v009"])?,
                mm: accessor(&lib, "SteamMatchmaking", &["v009"])?,
                net: accessor(&lib, "SteamNetworkingMessages_SteamAPI", &["v002"])?,
                netutils: accessor(&lib, "SteamNetworkingUtils_SteamAPI", &["v004", "v003"]).unwrap_or(std::ptr::null_mut()),
                stats: accessor(&lib, "SteamUserStats", &["v013", "v012", "v011"])?,
                storage: accessor(&lib, "SteamRemoteStorage", &["v016", "v014"]).unwrap_or(std::ptr::null_mut()),
                pipe,
                f,
            };
            std::mem::forget(lib);
            for (name, ok) in [
                ("CreateLobby", s.f.ISteamMatchmaking_CreateLobby.is_some()),
                ("SendMessageToUser", s.f.ISteamNetworkingMessages_SendMessageToUser.is_some()),
                ("ReceiveMessagesOnChannel", s.f.ISteamNetworkingMessages_ReceiveMessagesOnChannel.is_some()),
                ("GetSteamID", s.f.ISteamUser_GetSteamID.is_some()),
            ] {
                if !ok {
                    return Err(format!("{path} has no {name}"));
                }
            }
            if !s.netutils.is_null() {
                if let Some(f) = s.f.ISteamNetworkingUtils_InitRelayNetworkAccess {
                    f(s.netutils);
                }
            }
            Ok(s)
        }
    }

    /// Callbacks and finished calls since the last frame.
    pub fn poll(&self) -> Vec<Event> {
        let mut out = Vec::new();
        unsafe {
            (self.f.ManualDispatch_RunFrame.unwrap())(self.pipe);
            let mut m = CallbackMsg { user: 0, id: 0, param: std::ptr::null_mut(), size: 0 };
            while (self.f.ManualDispatch_GetNextCallback.unwrap())(self.pipe, &mut m) {
                let body = std::slice::from_raw_parts(m.param, m.size.max(0) as usize);
                if m.id == cb::API_CALL_COMPLETED {
                    // SteamAPICallCompleted_t: call @0, callback id @8, size @12
                    let call = u64_at(body, 0);
                    let rid = u32_at(body, 8) as i32;
                    let size = (u32_at(body, 12) as usize).max(result_size(rid));
                    let mut buf = vec![0u8; size];
                    let mut failed = false;
                    if let Some(get) = self.f.ManualDispatch_GetAPICallResult {
                        if get(self.pipe, call, buf.as_mut_ptr() as *mut c_void, size as i32, rid, &mut failed) && !failed {
                            out.push(decode(rid, call, &buf));
                        } else {
                            out.push(decode(rid, call, &[]));
                        }
                    }
                } else {
                    out.push(decode(m.id, 0, body));
                }
                (self.f.ManualDispatch_FreeLastCallback.unwrap())(self.pipe);
            }
        }
        out
    }

    pub fn me(&self) -> SteamId {
        unsafe { (self.f.ISteamUser_GetSteamID.unwrap())(self.user) }
    }
    pub fn app_id(&self) -> u32 {
        unsafe { self.f.ISteamUtils_GetAppID.map(|f| f(self.utils)).unwrap_or(0) }
    }
    /// Unix time by Steam's servers.
    pub fn server_time(&self) -> u32 {
        unsafe { self.f.ISteamUtils_GetServerRealTime.map(|f| f(self.utils)).unwrap_or(0) }
    }
    pub fn persona(&self) -> String {
        unsafe { self.f.ISteamFriends_GetPersonaName.map(|f| from_c(f(self.friends))).unwrap_or_default() }
    }
    pub fn friend_name(&self, id: SteamId) -> String {
        unsafe {
            if let Some(r) = self.f.ISteamFriends_RequestUserInformation {
                r(self.friends, id, true);
            }
            self.f.ISteamFriends_GetFriendPersonaName.map(|f| from_c(f(self.friends, id))).unwrap_or_default()
        }
    }
    /// Steam friends (k_EFriendFlagImmediate).
    pub fn friends(&self) -> Vec<SteamId> {
        unsafe {
            let (Some(n), Some(at)) = (self.f.ISteamFriends_GetFriendCount, self.f.ISteamFriends_GetFriendByIndex) else {
                return Vec::new();
            };
            let count = n(self.friends, 4);
            (0..count).map(|i| at(self.friends, i, 4)).collect()
        }
    }
    pub fn set_rich_presence(&self, key: &str, value: &str) {
        unsafe {
            if let Some(f) = self.f.ISteamFriends_SetRichPresence {
                f(self.friends, cs(key).as_ptr(), cs(value).as_ptr());
            }
        }
    }
    pub fn rich_presence(&self, who: SteamId, key: &str) -> String {
        unsafe { self.f.ISteamFriends_GetFriendRichPresence.map(|f| from_c(f(self.friends, who, cs(key).as_ptr()))).unwrap_or_default() }
    }

    // --- lobbies -----------------------------------------------------------

    pub fn create_lobby(&self, kind: i32, max: i32) -> ApiCall {
        unsafe { (self.f.ISteamMatchmaking_CreateLobby.unwrap())(self.mm, kind, max) }
    }
    pub fn join_lobby(&self, lobby: SteamId) -> ApiCall {
        unsafe { self.f.ISteamMatchmaking_JoinLobby.map(|f| f(self.mm, lobby)).unwrap_or(0) }
    }
    pub fn leave_lobby(&self, lobby: SteamId) {
        unsafe {
            if let Some(f) = self.f.ISteamMatchmaking_LeaveLobby {
                f(self.mm, lobby)
            }
        }
    }
    pub fn invite(&self, lobby: SteamId, who: SteamId) -> bool {
        unsafe { self.f.ISteamMatchmaking_InviteUserToLobby.map(|f| f(self.mm, lobby, who)).unwrap_or(false) }
    }
    pub fn members(&self, lobby: SteamId) -> Vec<SteamId> {
        unsafe {
            let (Some(n), Some(at)) = (self.f.ISteamMatchmaking_GetNumLobbyMembers, self.f.ISteamMatchmaking_GetLobbyMemberByIndex) else {
                return Vec::new();
            };
            (0..n(self.mm, lobby)).map(|i| at(self.mm, lobby, i)).collect()
        }
    }
    pub fn lobby_data(&self, lobby: SteamId, key: &str) -> String {
        unsafe { self.f.ISteamMatchmaking_GetLobbyData.map(|f| from_c(f(self.mm, lobby, cs(key).as_ptr()))).unwrap_or_default() }
    }
    pub fn set_lobby_data(&self, lobby: SteamId, key: &str, value: &str) -> bool {
        unsafe {
            self.f.ISteamMatchmaking_SetLobbyData.map(|f| f(self.mm, lobby, cs(key).as_ptr(), cs(value).as_ptr())).unwrap_or(false)
        }
    }
    pub fn member_data(&self, lobby: SteamId, who: SteamId, key: &str) -> String {
        unsafe {
            self.f.ISteamMatchmaking_GetLobbyMemberData.map(|f| from_c(f(self.mm, lobby, who, cs(key).as_ptr()))).unwrap_or_default()
        }
    }
    pub fn set_member_data(&self, lobby: SteamId, key: &str, value: &str) {
        unsafe {
            if let Some(f) = self.f.ISteamMatchmaking_SetLobbyMemberData {
                f(self.mm, lobby, cs(key).as_ptr(), cs(value).as_ptr())
            }
        }
    }
    pub fn owner(&self, lobby: SteamId) -> SteamId {
        unsafe { self.f.ISteamMatchmaking_GetLobbyOwner.map(|f| f(self.mm, lobby)).unwrap_or(0) }
    }
    pub fn set_lobby_type(&self, lobby: SteamId, kind: i32) {
        unsafe {
            if let Some(f) = self.f.ISteamMatchmaking_SetLobbyType {
                f(self.mm, lobby, kind);
            }
        }
    }
    pub fn set_joinable(&self, lobby: SteamId, yes: bool) {
        unsafe {
            if let Some(f) = self.f.ISteamMatchmaking_SetLobbyJoinable {
                f(self.mm, lobby, yes);
            }
        }
    }
    pub fn set_member_limit(&self, lobby: SteamId, n: i32) {
        unsafe {
            if let Some(f) = self.f.ISteamMatchmaking_SetLobbyMemberLimit {
                f(self.mm, lobby, n);
            }
        }
    }
    pub fn member_limit(&self, lobby: SteamId) -> i32 {
        unsafe { self.f.ISteamMatchmaking_GetLobbyMemberLimit.map(|f| f(self.mm, lobby)).unwrap_or(0) }
    }
    /// Starts a lobby search; filters are (key, value) string matches and
    /// (key, at least) numbers.
    pub fn find_lobbies(&self, strings: &[(&str, &str)], at_least: &[(&str, i32)], dist: i32, max: i32) -> ApiCall {
        unsafe {
            for (k, v) in strings {
                (self.f.ISteamMatchmaking_AddRequestLobbyListStringFilter.unwrap())(self.mm, cs(k).as_ptr(), cs(v).as_ptr(), cmp::EQUAL);
            }
            for (k, v) in at_least {
                (self.f.ISteamMatchmaking_AddRequestLobbyListNumericalFilter.unwrap())(
                    self.mm,
                    cs(k).as_ptr(),
                    *v,
                    cmp::GREATER_OR_EQUAL,
                );
            }
            if let Some(f) = self.f.ISteamMatchmaking_AddRequestLobbyListDistanceFilter {
                f(self.mm, dist);
            }
            if let Some(f) = self.f.ISteamMatchmaking_AddRequestLobbyListResultCountFilter {
                f(self.mm, max);
            }
            (self.f.ISteamMatchmaking_RequestLobbyList.unwrap())(self.mm)
        }
    }
    pub fn lobby_at(&self, i: i32) -> SteamId {
        unsafe { (self.f.ISteamMatchmaking_GetLobbyByIndex.unwrap())(self.mm, i) }
    }
    pub fn request_lobby_data(&self, lobby: SteamId) {
        unsafe {
            if let Some(f) = self.f.ISteamMatchmaking_RequestLobbyData {
                f(self.mm, lobby);
            }
        }
    }

    // --- networking ----------------------------------------------------------

    pub fn send(&self, to: SteamId, data: &[u8], flags: i32, channel: i32) -> i32 {
        let id = Identity::steam(to);
        unsafe {
            (self.f.ISteamNetworkingMessages_SendMessageToUser.unwrap())(
                self.net,
                &id,
                data.as_ptr() as *const c_void,
                data.len() as u32,
                flags,
                channel,
            )
        }
    }
    /// Messages waiting on a channel: (sender, payload).
    pub fn receive(&self, channel: i32) -> Vec<(SteamId, Vec<u8>)> {
        let mut out = Vec::new();
        let mut ptrs = [std::ptr::null_mut::<u8>(); 64];
        loop {
            let n = unsafe {
                (self.f.ISteamNetworkingMessages_ReceiveMessagesOnChannel.unwrap())(self.net, channel, ptrs.as_mut_ptr(), 64)
            };
            if n <= 0 {
                break;
            }
            for &m in &ptrs[..n as usize] {
                unsafe {
                    // SteamNetworkingMessage_t: data @0, size @8, peer identity @16
                    let data = *(m as *const *const u8);
                    let size = *(m.add(8) as *const i32);
                    let peer = *(m.add(16 + 8) as *const u64);
                    let body = if data.is_null() || size <= 0 { Vec::new() } else { std::slice::from_raw_parts(data, size as usize).to_vec() };
                    out.push((peer, body));
                    if let Some(rel) = self.f.SteamNetworkingMessage_t_Release {
                        rel(m);
                    }
                }
            }
            if n < 64 {
                break;
            }
        }
        out
    }
    pub fn accept(&self, peer: SteamId) {
        let id = Identity::steam(peer);
        unsafe {
            if let Some(f) = self.f.ISteamNetworkingMessages_AcceptSessionWithUser {
                f(self.net, &id);
            }
        }
    }
    pub fn close_session(&self, peer: SteamId) {
        let id = Identity::steam(peer);
        unsafe {
            if let Some(f) = self.f.ISteamNetworkingMessages_CloseSessionWithUser {
                f(self.net, &id);
            }
        }
    }

    // --- stats, leaderboards, cloud --------------------------------------------

    pub fn request_stats(&self) {
        unsafe {
            if let Some(f) = self.f.ISteamUserStats_RequestCurrentStats {
                f(self.stats);
            }
        }
    }
    pub fn stat(&self, name: &str) -> Option<i32> {
        let mut v = 0;
        unsafe { self.f.ISteamUserStats_GetStatInt32.and_then(|f| f(self.stats, cs(name).as_ptr(), &mut v).then_some(v)) }
    }
    pub fn set_stat(&self, name: &str, v: i32) -> bool {
        unsafe { self.f.ISteamUserStats_SetStatInt32.map(|f| f(self.stats, cs(name).as_ptr(), v)).unwrap_or(false) }
    }
    pub fn store_stats(&self) -> bool {
        unsafe { self.f.ISteamUserStats_StoreStats.map(|f| f(self.stats)).unwrap_or(false) }
    }
    /// Descending, numeric.
    pub fn find_board(&self, name: &str) -> ApiCall {
        unsafe { self.f.ISteamUserStats_FindOrCreateLeaderboard.map(|f| f(self.stats, cs(name).as_ptr(), 2, 1)).unwrap_or(0) }
    }
    /// Force-updates this user's entry.
    pub fn upload_score(&self, board: u64, score: i32, details: &[i32]) -> ApiCall {
        unsafe {
            self.f
                .ISteamUserStats_UploadLeaderboardScore
                .map(|f| f(self.stats, board, 2, score, details.as_ptr(), details.len() as i32))
                .unwrap_or(0)
        }
    }
    pub fn download_scores(&self, board: u64, users: &[SteamId]) -> ApiCall {
        unsafe {
            self.f
                .ISteamUserStats_DownloadLeaderboardEntriesForUsers
                .map(|f| f(self.stats, board, users.as_ptr(), users.len() as i32))
                .unwrap_or(0)
        }
    }
    pub fn board_entry(&self, entries: u64, i: i32) -> Option<BoardEntry> {
        // LeaderboardEntry_t: user @0, rank @8, score @12, details count @16, ugc after
        let mut e = [0u8; 32];
        let mut details = [0i32; 64];
        let ok = unsafe {
            self.f.ISteamUserStats_GetDownloadedLeaderboardEntry?(self.stats, entries, i, e.as_mut_ptr(), details.as_mut_ptr(), 64)
        };
        if !ok {
            return None;
        }
        let n = (u32_at(&e, 16) as usize).min(64);
        Some(BoardEntry { user: u64_at(&e, 0), rank: u32_at(&e, 8) as i32, score: u32_at(&e, 12) as i32, details: details[..n].to_vec() })
    }
    pub fn cloud_write(&self, name: &str, data: &[u8]) -> bool {
        if self.storage.is_null() {
            return false;
        }
        unsafe {
            self.f
                .ISteamRemoteStorage_FileWrite
                .map(|f| f(self.storage, cs(name).as_ptr(), data.as_ptr() as *const c_void, data.len() as i32))
                .unwrap_or(false)
        }
    }
    pub fn cloud_read(&self, name: &str) -> Option<Vec<u8>> {
        if self.storage.is_null() {
            return None;
        }
        unsafe {
            let size = self.f.ISteamRemoteStorage_GetFileSize?(self.storage, cs(name).as_ptr());
            if size <= 0 {
                return None;
            }
            let mut buf = vec![0u8; size as usize];
            let n = self.f.ISteamRemoteStorage_FileRead?(self.storage, cs(name).as_ptr(), buf.as_mut_ptr() as *mut c_void, size);
            buf.truncate(n.max(0) as usize);
            Some(buf)
        }
    }

    pub fn shutdown(&self) {
        unsafe {
            if let Some(f) = self.f.Shutdown {
                f()
            }
        }
    }
}
