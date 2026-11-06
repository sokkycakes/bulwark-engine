//! bwlive-server: Bulwark Live rendezvous, relay and matchmaking server.
//!
//!   bwlive-server [--bind ADDR] [--http ADDR] [--playlists FILE]
//!                 [--relay-only] [--max-players N] [--relay-kbps N]
//!
//! One UDP port (default 0.0.0.0:27950). With --http, a JSON status page.

use bwlive_core::Playlist;
use bwlive_server::{Config, Live};
use std::io::{Read, Write};
use std::net::{TcpListener, UdpSocket};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

fn usage() -> ! {
    eprintln!(
        "usage: bwlive-server [--bind 0.0.0.0:27950] [--http 127.0.0.1:27951]\n\
         \x20                    [--playlists playlists.json] [--relay-only]\n\
         \x20                    [--max-players 4096] [--relay-kbps 64]"
    );
    std::process::exit(2)
}

fn main() {
    let mut bind = "0.0.0.0:27950".to_string();
    let mut http: Option<String> = None;
    let mut cfg = Config::default();
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        let mut val = || args.next().unwrap_or_else(|| usage());
        match a.as_str() {
            "--bind" => bind = val(),
            "--http" => http = Some(val()),
            "--relay-only" => cfg.relay_only = true,
            "--max-players" => cfg.max_players = val().parse().unwrap_or_else(|_| usage()),
            "--relay-kbps" => cfg.relay_rate = val().parse::<f64>().unwrap_or_else(|_| usage()) * 1024.0,
            "--playlists" => {
                let path = val();
                let text = std::fs::read_to_string(&path).unwrap_or_else(|e| {
                    eprintln!("{path}: {e}");
                    std::process::exit(1)
                });
                cfg.playlists = Playlist::list_from_json(&text).unwrap_or_else(|e| {
                    eprintln!("{path}: {e}");
                    std::process::exit(1)
                });
            }
            _ => usage(),
        }
    }

    let sock = UdpSocket::bind(&bind).unwrap_or_else(|e| {
        eprintln!("can't bind {bind}: {e}");
        std::process::exit(1)
    });
    sock.set_read_timeout(Some(Duration::from_millis(50))).unwrap();
    let seed = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos() as u64 ^ (std::process::id() as u64) << 32;
    let playlists: Vec<String> = cfg.playlists.iter().map(|p| p.id.clone()).collect();
    eprintln!(
        "bwlive-server on udp {}{}{}",
        sock.local_addr().unwrap(),
        if cfg.relay_only { " (relay only)".to_string() } else { format!(", playlists {}", playlists.join(" ")) },
        http.as_deref().map(|h| format!(", status http://{h}/")).unwrap_or_default()
    );
    let mut live = Live::new(cfg, seed);

    let status = Arc::new(Mutex::new(live.status_json()));
    if let Some(h) = http {
        let listener = TcpListener::bind(&h).unwrap_or_else(|e| {
            eprintln!("can't bind http {h}: {e}");
            std::process::exit(1)
        });
        let status = status.clone();
        std::thread::spawn(move || {
            for conn in listener.incoming().flatten() {
                let mut conn = conn;
                let _ = conn.set_read_timeout(Some(Duration::from_secs(2)));
                let mut buf = [0u8; 1024];
                let _ = conn.read(&mut buf);
                let body = status.lock().unwrap().clone();
                let _ = write!(
                    conn,
                    "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                    body.len(),
                    body
                );
            }
        });
    }

    let mut buf = [0u8; 2048];
    let mut next_tick = Instant::now() + Duration::from_secs(1);
    loop {
        match sock.recv_from(&mut buf) {
            Ok((n, from)) => {
                for (to, data) in live.handle(from, &buf[..n], Instant::now()) {
                    let _ = sock.send_to(&data, to);
                }
            }
            Err(e) if matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => {}
            Err(_) => {} // e.g. ICMP port unreachable surfaced as an error
        }
        let now = Instant::now();
        if now >= next_tick {
            next_tick = now + Duration::from_secs(1);
            for (to, data) in live.tick(now) {
                let _ = sock.send_to(&data, to);
            }
            *status.lock().unwrap() = live.status_json();
        }
    }
}
