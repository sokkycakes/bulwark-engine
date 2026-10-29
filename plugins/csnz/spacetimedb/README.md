# csnz SpacetimeDB module

The persistent backend for CSNZ servers on Bulwark. Gameplay stays on the
Bulwark server. The CSNZ rules QC (`games/csnz/src/rules/common/csnz_stdb.qc`)
writes player data, the server list and match history here over
SpacetimeDB's HTTP API. Every save also goes to the local files, so a server
keeps working when the database is down.

| Table | What it holds | Visibility |
|---|---|---|
| `player` | key (see Players below), display name, first and last seen | private |
| `progression` | level, xp, rank, kill/death/win/match totals, playtime, money | private |
| `inventory_item`, `loadout` | owned items, equipped character and weapons | private |
| `achievement`, `achievement_counter` | the 30 achievements and their counters | private |
| `mode_stats` | per player and CSO mode: matches, wins, kills, deaths | private |
| `game_server` | live servers (heartbeat every 30 s, dropped after 120 s) | public |
| `match_record`, `match_player` | every finished match and its players | public |
| `offline_account`, `offline_chunk` | the real client's offline server database, as snapshots (see Offline server below) | private |
| `channel_server`, `channel`, `room` | the lobby (see Lobby below) | public |
| `operator` | identities allowed to write | private |

Reducers: `player_seen`, `save_progression`, `save_inventory`,
`save_achievements` (each takes the QC's own save-file text, see
`src/text.rs`), `record_match`, `server_heartbeat`, `server_shutdown`,
`add_operator` and `remove_operator`. The procedure `get_profile(key)` returns
everything stored for a player as `[section]` text. Only operators may call
any of them. The publisher becomes the first operator.

## Setup

1. Build and publish (needs `rustup target add wasm32-unknown-unknown` and
   the `spacetime` CLI, logged in with `spacetime login`):

       ./publish.sh <database-name>            # Maincloud
       ./publish.sh <database-name> <server>   # a self-hosted server

2. Give the game server its own identity, so its token can be revoked
   without touching yours:

       curl -X POST https://maincloud.spacetimedb.com/v1/identity
       # -> {"identity":"c200...","token":"eyJ..."}
       spacetime call <database-name> add_operator '"c200..."' '"game server 1"'

3. On the game server, put the token in `games/csnz/stdb.cfg`. It's
   gitignored (`stdb*.cfg`). Never commit it.

       set stdb_url "https://maincloud.spacetimedb.com"
       set stdb_token "eyJ..."
       set csnz_stdb_db "<database-name>"
       set csnz_stdb_address "your.host:27500"   // for the server list; "" leaves it out
       set csnz_stdb 1

   Then start the server with `+exec stdb.cfg`. `https://` needs the system
   libcurl, which Bulwark loads at run time.

`csnz_stdb_debug 1` prints every call and answer. `csnz_stdb_heartbeat`
sets the server-list interval in seconds.

## Players

Each Bulwark client makes a random `_playerkey` once and keeps it in its
config, like ElDewrito's player key. Servers keep `_` userinfo keys to
themselves (other clients never see them, and `user` prints "(private)").
The QC keys a player by `k` plus 31 hex digits of a SHA-256 of that key
(`CSNZ_PlayerId`), so the secret itself never reaches file names or this
database. Clients without a key (bots, older engines) are keyed by the
letters and digits of their name, as before.

On the first load, a keyed player with no saves of their own takes over the
saves of their name (`data/csnz/players/<name>*.txt`). Only one key may do
that per name: `<name>.claimed` records which. The name's files and rows stay
as they were. Copy `_playerkey` from your config to play as yourself on
another machine; anyone with it can.

## Lobby

Modelled on CSNZ_Server's layout: channel servers hold channels, and a
channel's lobby lists rooms. Here a room is a Bulwark game server that sends
`room_heartbeat` for its channel (`csnz_stdb_channel`, set by the menu's
Create Room in a database channel) and has a public address
(`csnz_stdb_address`); rooms drop after 120 s without one. The lobby starts
with one channel server and one channel, named as CSNZ_Server names them
("Bulwark [1 / 1]", "Bulwark 1-1"); operators add more with
`add_channel_server(name, channels)` and `add_channel(server_id, name,
max_players)`. The procedures `list_channels` and `list_rooms(channel_id)`
need no token: the CSNZ menu (games/csnz/menu/services.qc) calls them and
shows the database's servers after "This computer". Joining a room connects
to its address.

## Behaviour

- On a player's first spawn the local files load as before, then
  `get_profile` answers and its sections replace them. Sections the database
  doesn't have yet are uploaded from the local data, which migrates existing
  saves.
- Saves stay local until that answer comes, so a failed load never
  overwrites stored data with defaults. The profile is asked for again with
  backoff (4 s, up to 2 min).
- Calls that get no answer or a 5xx are retried 6 times with backoff. A 4xx
  is printed and dropped.
- `record_match` runs at every match end. With `cs_noStats` (bot modes and
  Item Battle) it records the match but doesn't add to `mode_stats`.
  zombi.dat (mode 8) has no progression or stats, so it sends nothing.

## Tests

- `cargo test --lib` checks the save-file formats round trip.
- `tests/uri/run.sh` checks the engine side: POST bodies, methods, the token
  only for `stdb_url`, and callbacks reaching addprogs modules.
- The QC side was tested end to end against a mock of this module's HTTP
  surface: new-player upload, stored-profile apply, match record and
  heartbeat.
- Live on Maincloud (2026-10-29, SpacetimeDB 2.13.0, database bulwark-fu58e):
  publish, init (the publisher became the operator), save_progression,
  save_inventory, get_profile, server_heartbeat, record_match and
  server_shutdown over HTTP all work, and an anonymous call is refused.
  Observed answer shapes: a procedure's `Result<String, String>` is HTTP 200
  with `[0,"..."]` (Ok) or `[1,"..."]` (Err). A reducer's `Err` is HTTP 530
  with the message as plain text. The QC handles both.

## Offline server (the real client)

The real CSNZ client's offline mode (`CSOLauncher -offline`, in sokky's local
csnz-offline project) hosts its own server, csnzserver.dll, which keeps its
data in a SQLite file (`OfflineData/UserDatabase.db3`). With an
`OfflineData/stdb.cfg` (`url=`, `database=`, `token=`, `account=`) that server
uploads the whole database to `offline_stage`/`offline_commit` when it stops,
and before it opens the file it downloads the account's snapshot with
`offline_pull` if that is newer than the last one the machine synced (after
backing up the local file). A snapshot is chunk 0 (schema and
`user_version`) plus row chunks of about 256 KiB; staging then committing
means a broken upload never replaces the stored snapshot. Tested 2026-10-29 on
Maincloud under Wine: upload, an up-to-date start, and a restore that matched
the original in all 46 tables.
