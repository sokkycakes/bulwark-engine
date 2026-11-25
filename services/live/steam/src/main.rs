//! bwlive-steam: Bulwark Live on Steamworks.
//!
//!   bwlive-steam [--bind 127.0.0.1:27950] [--steam-lib PATH] [--app-id 480]
//!                [--game stiletto] [--playlists FILE] [--records FILE]
//!                [--tunnel] [--game-port 27500] [--tunnel-port 27960]
//!                [--on-match-host CMD] [--on-match-join CMD] [--accept-all]
//!
//! It is a Bulwark Live server for one game on this computer: the engine
//! runs `live_connect 127.0.0.1` and everything it does (parties, room
//! codes, invites, queues, game traffic to live:<id>, set reports, fighter
//! cards) works as with bwlive-server, but on Steam:
//!
//! - player ids are Steam account ids;
//! - a party is a Steam lobby, the room code is derived from its id;
//! - a ticket is a public Steam lobby that parties find with a lobby
//!   search, widening by the playlist's stages; the lobby's owner forms the
//!   match with the same rules as bwlive-core's matcher;
//! - game traffic goes through ISteamNetworkingMessages (Steam does the NAT
//!   traversal and relays);
//! - fighter records are kept by each player in Steam Cloud, published to a
//!   leaderboard, and exchanged with the other players of a match.
//!
//! For an engine without a Live client (stock FTEQW), --tunnel carries
//! plain UDP game traffic: the host's server and the clients connect to
//! 127.0.0.1 and the bridge forwards over Steam. Commands typed on stdin
//! (`help`) drive the party and the queue then.
//!
//! docs/spec/live-steam.md has the details. ISC licence.

mod bridge;
mod steam;

use std::time::Duration;

fn usage() -> ! {
    eprintln!(
        "usage: bwlive-steam [--bind 127.0.0.1:27950] [--steam-lib libsteam_api.so] [--app-id 480]\n\
         \x20                   [--game stiletto] [--playlists playlists.json] [--records records.json]\n\
         \x20                   [--tunnel] [--game-port 27500] [--tunnel-port 27960]\n\
         \x20                   [--on-match-host CMD] [--on-match-join CMD] [--accept-all]"
    );
    std::process::exit(2)
}

fn main() {
    let mut cfg = bridge::Config::default();
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        let mut val = || args.next().unwrap_or_else(|| usage());
        match a.as_str() {
            "--bind" => cfg.bind = val(),
            "--steam-lib" => cfg.steam_lib = val(),
            "--app-id" => cfg.app_id = val().parse().unwrap_or_else(|_| usage()),
            "--game" => cfg.game = val(),
            "--records" => cfg.records = Some(val()),
            "--tunnel" => cfg.tunnel = true,
            "--game-port" => cfg.game_port = val().parse().unwrap_or_else(|_| usage()),
            "--tunnel-port" => cfg.tunnel_port = val().parse().unwrap_or_else(|_| usage()),
            "--on-match-host" => cfg.on_host = Some(val()),
            "--on-match-join" => cfg.on_join = Some(val()),
            "--accept-all" => cfg.accept_all = true,
            "--quiet" => cfg.quiet = true,
            "--playlists" => {
                let path = val();
                let text = std::fs::read_to_string(&path).unwrap_or_else(|e| {
                    eprintln!("{path}: {e}");
                    std::process::exit(1)
                });
                cfg.playlists = bwlive_core::Playlist::list_from_json(&text).unwrap_or_else(|e| {
                    eprintln!("{path}: {e}");
                    std::process::exit(1)
                });
            }
            "-h" | "--help" => usage(),
            _ => usage(),
        }
    }
    // SteamAPI_Init reads the app id from steam_appid.txt in the working
    // directory; write it for Spacewar if it isn't there, as a dev build does.
    if std::fs::metadata("steam_appid.txt").is_err() && std::env::var("SteamAppId").is_err() {
        if let Err(e) = std::fs::write("steam_appid.txt", format!("{}\n", cfg.app_id)) {
            eprintln!("can't write steam_appid.txt: {e}");
        }
    }
    let mut b = match bridge::Bridge::new(cfg) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("bwlive-steam: {e}");
            std::process::exit(1)
        }
    };
    let lines = bridge::stdin_lines();
    loop {
        while let Ok(line) = lines.try_recv() {
            if !b.command(&line) {
                b.shutdown();
                return;
            }
        }
        b.frame();
        std::thread::sleep(Duration::from_millis(2));
    }
}
