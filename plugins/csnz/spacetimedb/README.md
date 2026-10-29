# csnz SpacetimeDB module

The persistent backend for CSNZ servers on Bulwark. Gameplay stays on the
Bulwark server. The CSNZ rules QC (`games/csnz/src/rules/common/csnz_stdb.qc`)
writes player data, the server list and match history here over
SpacetimeDB's HTTP API. Every save also goes to the local files, so a server
keeps working when the database is down.

| Table | What it holds | Visibility |
|---|---|---|
| `player` | key (today the player's name), display name, first and last seen | private |
| `progression` | level, xp, rank, kill/death/win/match totals, playtime, money | private |
| `inventory_item`, `loadout` | owned items, equipped character and weapons | private |
| `achievement`, `achievement_counter` | the 30 achievements and their counters | private |
| `mode_stats` | per player and CSO mode: matches, wins, kills, deaths | private |
| `game_server` | live servers (heartbeat every 30 s, dropped after 120 s) | public |
| `match_record`, `match_player` | every finished match and its players | public |
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
- **Not yet tested against a real SpacetimeDB server.** Unconfirmed: the
  exact JSON shape of a procedure's HTTP answer (the QC accepts `"..."` and
  `{"Ok": "..."}`), and whether `init`'s sender is the publisher.
