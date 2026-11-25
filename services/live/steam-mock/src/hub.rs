//! steam-mock-hub: the shared side of the steam-mock test library (users,
//! lobbies, leaderboards, cloud files, message routing). One line of JSON
//! per request or event over TCP. Test only. ISC licence.
//!
//!   steam-mock-hub [127.0.0.1:27999]

use serde_json::{json, Value};
use std::collections::{BTreeMap, HashMap, HashSet};
use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Mutex};

type Sid = u64;

struct Lobby {
    owner: Sid,
    kind: i64,
    joinable: bool,
    max: i64,
    members: Vec<Sid>,
    data: BTreeMap<String, String>,
    mdata: HashMap<Sid, BTreeMap<String, String>>,
    invited: HashSet<Sid>,
}

#[derive(Default)]
struct User {
    name: String,
    rp: HashMap<String, String>,
    out: Option<Arc<Mutex<TcpStream>>>,
}

#[derive(Default)]
struct Hub {
    users: HashMap<Sid, User>,
    lobbies: BTreeMap<Sid, Lobby>,
    next_lobby: u64,
    boards: HashMap<String, u64>,
    entries: HashMap<u64, HashMap<Sid, (i32, Vec<i32>)>>,
    cloud: HashMap<(Sid, String), String>,
    stats: HashMap<Sid, Value>,
    log: bool,
}

impl Hub {
    fn push(&self, to: Sid, ev: Value) {
        if let Some(out) = self.users.get(&to).and_then(|u| u.out.clone()) {
            let _ = writeln!(out.lock().unwrap(), "{ev}");
        }
    }

    fn leave(&mut self, me: Sid, lobby: Sid) {
        let Some(l) = self.lobbies.get_mut(&lobby) else { return };
        if !l.members.contains(&me) {
            return;
        }
        l.members.retain(|&m| m != me);
        l.mdata.remove(&me);
        if l.members.is_empty() {
            self.lobbies.remove(&lobby);
            return;
        }
        if l.owner == me {
            l.owner = l.members[0];
        }
        let left = l.members.clone();
        for m in left {
            self.push(m, json!({"ev": "chat", "lobby": lobby, "user": me, "change": 2}));
        }
    }

    fn data_changed(&self, lobby: Sid, member: Sid) {
        if let Some(l) = self.lobbies.get(&lobby) {
            for &m in &l.members {
                self.push(m, json!({"ev": "data", "lobby": lobby, "member": member}));
            }
        }
    }

    fn handle(&mut self, me: Sid, v: &Value) -> Option<Value> {
        let s = |k: &str| v[k].as_str().unwrap_or("").to_string();
        let lobby = v["lobby"].as_u64().unwrap_or(0);
        let r = match v["op"].as_str().unwrap_or("") {
            "friends" => json!({"ids": self.users.keys().filter(|&&u| u != me).collect::<Vec<_>>()}),
            "name" => json!({"name": self.users.get(&v["id"].as_u64().unwrap_or(0)).map(|u| u.name.clone())}),
            "rp_set" => {
                if let Some(u) = self.users.get_mut(&me) {
                    u.rp.insert(s("k"), s("v"));
                }
                json!({})
            }
            "rp_get" => {
                let v = self.users.get(&v["id"].as_u64().unwrap_or(0)).and_then(|u| u.rp.get(&s("k")).cloned());
                json!({"v": v.unwrap_or_default()})
            }
            "create" => {
                self.next_lobby += 1;
                let id = 0x0186_0000_0000_0000 | self.next_lobby;
                self.lobbies.insert(
                    id,
                    Lobby {
                        owner: me,
                        kind: v["type"].as_i64().unwrap_or(2),
                        joinable: true,
                        max: v["max"].as_i64().unwrap_or(4).clamp(1, 250),
                        members: vec![me],
                        data: BTreeMap::new(),
                        mdata: HashMap::new(),
                        invited: HashSet::new(),
                    },
                );
                if self.log {
                    eprintln!("hub: {me} made lobby {id}");
                }
                json!({"lobby": id})
            }
            "join" => {
                let resp = match self.lobbies.get_mut(&lobby) {
                    None => 2,
                    Some(l) if l.members.contains(&me) => 1,
                    Some(l) if l.members.len() as i64 >= l.max => 4,
                    Some(l) if !l.joinable => 3,
                    Some(l) if l.kind == 0 && !l.invited.contains(&me) => 3,
                    Some(l) => {
                        l.members.push(me);
                        1
                    }
                };
                if resp == 1 {
                    let members = self.lobbies[&lobby].members.clone();
                    for m in members.into_iter().filter(|&m| m != me) {
                        self.push(m, json!({"ev": "chat", "lobby": lobby, "user": me, "change": 1}));
                    }
                }
                if self.log {
                    eprintln!("hub: {me} joins {lobby}: {resp}");
                }
                json!({"resp": resp})
            }
            "leave" => {
                self.leave(me, lobby);
                json!({})
            }
            "invite" => {
                let to = v["to"].as_u64().unwrap_or(0);
                let ok = match self.lobbies.get_mut(&lobby) {
                    Some(l) if l.members.contains(&me) && self.users.contains_key(&to) => {
                        l.invited.insert(to);
                        true
                    }
                    _ => false,
                };
                if ok {
                    self.push(to, json!({"ev": "invite", "from": me, "lobby": lobby}));
                }
                json!({"ok": ok})
            }
            "info" => match self.lobbies.get(&lobby) {
                None => json!({}),
                Some(l) if l.members.contains(&me) => {
                    let mdata: HashMap<String, &BTreeMap<String, String>> = l.mdata.iter().map(|(k, v)| (k.to_string(), v)).collect();
                    json!({"owner": l.owner, "members": l.members, "max": l.max, "data": l.data, "mdata": mdata})
                }
                // Steam shows a lobby's data, not its members, to outsiders
                Some(l) => json!({"owner": 0, "members": [], "max": l.max, "data": l.data, "mdata": {}}),
            },
            "set" => {
                let ok = match self.lobbies.get_mut(&lobby) {
                    Some(l) if l.owner == me => {
                        l.data.insert(s("k"), s("v"));
                        true
                    }
                    _ => false,
                };
                if ok {
                    self.data_changed(lobby, lobby);
                }
                json!({"ok": ok})
            }
            "mset" => {
                let ok = match self.lobbies.get_mut(&lobby) {
                    Some(l) if l.members.contains(&me) => {
                        l.mdata.entry(me).or_default().insert(s("k"), s("v"));
                        true
                    }
                    _ => false,
                };
                if ok {
                    self.data_changed(lobby, me);
                }
                json!({"ok": ok})
            }
            "attr" => {
                let ok = match self.lobbies.get_mut(&lobby) {
                    Some(l) if l.owner == me => {
                        if let Some(t) = v["type"].as_i64() {
                            l.kind = t;
                        }
                        if let Some(j) = v["joinable"].as_bool() {
                            l.joinable = j;
                        }
                        if let Some(m) = v["max"].as_i64() {
                            l.max = m.clamp(1, 250);
                        }
                        true
                    }
                    _ => false,
                };
                json!({"ok": ok})
            }
            "find" => {
                let filters = v["filters"].as_array().cloned().unwrap_or_default();
                let max = filters.iter().find_map(|f| f["max"].as_u64()).unwrap_or(50) as usize;
                let list: Vec<Sid> = self
                    .lobbies
                    .iter()
                    .filter(|(_, l)| l.kind == 2 && l.joinable && (l.members.len() as i64) < l.max)
                    .filter(|(_, l)| {
                        filters.iter().all(|f| {
                            let Some(k) = f["k"].as_str() else { return true };
                            let have = l.data.get(k);
                            let cmp = f["cmp"].as_i64().unwrap_or(0);
                            if let Some(want) = f["s"].as_str() {
                                let h = have.map(|s| s.as_str()).unwrap_or("");
                                return if cmp == 3 { h != want } else { h == want };
                            }
                            let want = f["n"].as_i64().unwrap_or(0);
                            let Some(h) = have.and_then(|s| s.parse::<i64>().ok()) else { return false };
                            match cmp {
                                -2 => h <= want,
                                -1 => h < want,
                                0 => h == want,
                                1 => h > want,
                                2 => h >= want,
                                _ => h != want,
                            }
                        })
                    })
                    .map(|(&id, _)| id)
                    .take(max)
                    .collect();
                json!({"lobbies": list})
            }
            "send" => {
                let to = v["to"].as_u64().unwrap_or(0);
                self.push(to, json!({"ev": "msg", "from": me, "ch": v["ch"], "data": v["data"]}));
                return None;
            }
            "stats" => {
                self.stats.insert(me, v["stats"].clone());
                json!({})
            }
            "board" => {
                let n = self.boards.len() as u64 + 1;
                let h = *self.boards.entry(s("name")).or_insert(0x77_0000 + n);
                json!({"handle": h})
            }
            "upload" => {
                let board = v["board"].as_u64().unwrap_or(0);
                let d: Vec<i32> = v["details"].as_array().map(|a| a.iter().map(|x| x.as_i64().unwrap_or(0) as i32).collect()).unwrap_or_default();
                self.entries.entry(board).or_default().insert(me, (v["score"].as_i64().unwrap_or(0) as i32, d));
                json!({"ok": true})
            }
            "download" => {
                let board = v["board"].as_u64().unwrap_or(0);
                let all = self.entries.get(&board).cloned().unwrap_or_default();
                let mut ranked: Vec<(&Sid, &(i32, Vec<i32>))> = all.iter().collect();
                ranked.sort_by(|a, b| b.1 .0.cmp(&a.1 .0).then(a.0.cmp(b.0)));
                let want: Vec<u64> = v["users"].as_array().map(|a| a.iter().filter_map(|x| x.as_u64()).collect()).unwrap_or_default();
                let entries: Vec<Value> = ranked
                    .iter()
                    .enumerate()
                    .filter(|(_, (id, _))| want.contains(id))
                    .map(|(i, (id, (score, d)))| json!([id, i + 1, score, d]))
                    .collect();
                json!({"entries": entries})
            }
            "cloud_write" => {
                self.cloud.insert((me, s("name")), s("data"));
                json!({"ok": true})
            }
            "cloud_read" => json!({"data": self.cloud.get(&(me, s("name")))}),
            "bye" => {
                self.drop_user(me);
                json!({})
            }
            _ => json!({}),
        };
        Some(r)
    }

    fn drop_user(&mut self, me: Sid) {
        let ids: Vec<Sid> = self.lobbies.iter().filter(|(_, l)| l.members.contains(&me)).map(|(&id, _)| id).collect();
        for l in ids {
            self.leave(me, l);
        }
        if let Some(u) = self.users.get_mut(&me) {
            u.out = None;
            u.rp.clear();
        }
    }
}

fn serve(hub: Arc<Mutex<Hub>>, stream: TcpStream) {
    let _ = stream.set_nodelay(true);
    let out = Arc::new(Mutex::new(stream.try_clone().unwrap()));
    let mut me: Sid = 0;
    for line in BufReader::new(stream).lines() {
        let Ok(line) = line else { break };
        let Ok(v) = serde_json::from_str::<Value>(&line) else { continue };
        let rid = v["rid"].clone();
        let mut h = hub.lock().unwrap();
        let reply = if v["op"] == "hello" {
            me = v["id"].as_u64().unwrap_or(0);
            let u = h.users.entry(me).or_default();
            u.name = v["name"].as_str().unwrap_or("").to_string();
            u.out = Some(out.clone());
            if h.log {
                eprintln!("hub: hello {me} ({})", v["name"]);
            }
            Some(json!({"ok": me != 0}))
        } else if me == 0 {
            Some(json!({}))
        } else {
            h.handle(me, &v)
        };
        drop(h);
        if let Some(mut r) = reply {
            r["rid"] = rid;
            let _ = writeln!(out.lock().unwrap(), "{r}");
        }
    }
    if me != 0 {
        hub.lock().unwrap().drop_user(me);
    }
}

fn main() {
    let addr = std::env::args().nth(1).unwrap_or_else(|| "127.0.0.1:27999".into());
    let l = TcpListener::bind(&addr).unwrap_or_else(|e| {
        eprintln!("can't bind {addr}: {e}");
        std::process::exit(1)
    });
    let hub = Arc::new(Mutex::new(Hub { log: std::env::var("STEAM_MOCK_LOG").is_ok(), ..Hub::default() }));
    eprintln!("steam-mock-hub on {addr}");
    for s in l.incoming().flatten() {
        let h = hub.clone();
        std::thread::spawn(move || serve(h, s));
    }
}
