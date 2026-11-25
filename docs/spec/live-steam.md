# Bulwark Live on Steamworks

`bwlive-steam` (in `services/live/steam`) runs Bulwark Live on Steam. It is
a Bulwark Live server for one computer: the game connects to it on
localhost with the usual `live_connect`, and it does what `bwlive-server`
would do with Steam's services instead of a server of ours. Nothing in the
engine or in game code changes, and the menus, `live_state`, `live_queue`,
`connect live:<id>`, set reports and fighter cards work as described in
[live.md](live.md).

```
 game (Bulwark)  --BWLV on 127.0.0.1:27950-->  bwlive-steam  --Steamworks-->  Steam
```

It is meant for testing against Steam with the Spacewar app id (480), which
any Steam account can run without a store page or partner settings.

## What maps to what

| Bulwark Live | On Steam |
|---|---|
| player id | the Steam account id (the low 32 bits of the SteamID64) |
| `hello`, `welcome` | answered by the bridge; the name is the game's, or the Steam persona name |
| party | a lobby with `bw_kind=party`. Joinable by code (`flags` bit 0): public lobby; otherwise friends-only |
| room code | made from the lobby's id (six characters, the usual alphabet), stored as `bw_code` |
| `party_join <code>` | the lobby from an invite, else a lobby search on `bw_kind`, `bw_code` and `bw_game` |
| `invite`, `invited` | `InviteUserToLobby`, `LobbyInvite_t`; Steam's own "Join game" (`GameLobbyJoinRequested_t`, or rich presence `connect`) joins too |
| guests | lobby member data `guests` (one name a line); they count toward `bw_max` |
| leader | the lobby's owner; Steam hands ownership on when the owner leaves |
| `list` | Steam friends whose rich presence `bw_game` is this game, with `status` and `bw_code` |
| `status` | rich presence `status` |
| ticket (`queue`) | a public lobby with `bw_kind=ticket`, `bw_game`, `bw_pl` (playlist), `bw_t` (Unix time it started), `bw_sen` (seniority), `bw_free` (open seats), `bw_open`, `bw_ids` (consoles in it) |
| `match` | the ticket owner writes it as JSON into the ticket's `bw_match`; the party leader copies it to the party lobby's `bw_match` for the other members |
| `queue_status` for party members | the leader writes `playlist\|stage\|waited\|searching` into the party's `bw_queue` |
| `relay`, `forward` (live:<id> traffic) | `ISteamNetworkingMessages`, channel 0, unreliable without Nagle, the payload as it is (virtual ports first). Steam does NAT traversal and relaying, so `introduce` and punching are never used |
| `set_report` | the host's bridge checks it like the server does and sends it to every console in the match (channel 1, reliable). Each bridge applies its own seats only |
| fighter records | each player's `Records` (bwlive-core), in Steam Cloud as `bwlive_records.json`, with a local copy (`bwlive-steam-records-<account>.json`) |
| `card` | your own from your record; other players' cards arrive from their bridges when a match starts and after each set (channel 1); anyone else's comes from the leaderboard |
| leaderboard | `bwlive_<game>_sets`, descending, score = sets won, details = sets, completed, wins, losses, quits, quits behind, disconnects, runbacks, forfeit wins, active days, the form strip (two bits a result, W L Q D, newest last) and its length. Heroes are only in the player's own record |
| Spacewar's stats | `NumGames`, `NumWins`, `NumLosses` follow the record where the app has them |
| `match_left` | leaves the ticket lobby, which frees the console's seats in a drop-in match |

### Matchmaking

The rules are bwlive-core's, run by the players instead of a server:

- Every 3 seconds a waiting leader searches for tickets of the same game and
  playlist that are open and have `bw_free` at least its party's size. The
  distance filter follows the current stage's `max_rtt_sum` (up to 150 ms:
  Steam's default distance, up to 300: far, more or 0: worldwide), since
  Steam doesn't give round-trip times before players meet.
- A found ticket is kept if `matcher::compatible` allows it with both
  tickets' current stages (seniority ratio, recent opponents from `bw_ids`).
  Drop-in matches with room come first, fullest first; then the ticket that
  started first.
- Without one, the leader opens a ticket of its own and keeps searching. Its
  own ticket only gives way to an older one, and only while nobody else has
  joined it, so two tickets can't keep swapping.
- The ticket's owner forms the match once the parties fill
  `teams × team_size` (or `min_players` after the last stage), with
  `matcher::pack` for the teams and `Playlist::pick` for the mode and map.
  The owner hosts, as the longest-waiting party's leader does on the server.
  A non-drop-in ticket then closes (`bw_open=0`, not joinable).
- A drop-in ticket stays open (`bw_drop=1`) and its owner rewrites the seats
  as consoles join and leave; once the host leaves, the next owner closes it.
- A party that ends up in a ticket the owner didn't seat it in (two parties
  joined at once) sees itself missing from `bw_match` and searches again.

Seniority is the mean of the party's records (lobby member data `sen`), as
the server works it out; the number the game sends with `queue` is not used.

### Sessions

A Steam networking session is accepted from anyone in the party or ticket
lobby or in the match's seats, and from anyone this console has sent to.
A request from anyone else is held for 15 seconds (the match that names them
may still be on its way) and then closed. `--accept-all` accepts everyone.

## Running it

```
bwlive-steam [--bind 127.0.0.1:27950] [--steam-lib libsteam_api.so] [--app-id 480]
             [--game stiletto] [--playlists playlists.json] [--records FILE]
             [--tunnel] [--game-port 27500] [--tunnel-port 27960]
             [--on-match-host CMD] [--on-match-join CMD] [--accept-all]
```

- It needs Steam running and logged in, and `libsteam_api.so` (Linux),
  `steam_api64.dll` (Windows) or `libsteam_api.dylib` (macOS) from the
  Steamworks SDK's `redistributable_bin`. The library is opened at run
  time, so building needs no SDK.
- `steam_appid.txt` is written in the working directory with `--app-id`
  (480, Spacewar) when it isn't there, as the Steam API wants for a game
  started outside Steam.
- The playlists are bwlive-core's built-in ones unless `--playlists` names a
  file. Every player must use the same file.
- Commands on stdin (`help`): `party`, `join`, `leave`, `open`, `invite`,
  `list`, `queue`, `cancel`, `left`, `card`, `status`, `game`, `quit`.
  They act as if the game had sent them, so the bridge is usable without a
  game attached.

### Engines without a Live client (stock FTEQW)

With `--tunnel`, plain UDP game traffic goes over Steam on channel 2:

- The host's bridge forwards each remote player's packets to the local game
  server on `--game-port` (27500) from a socket of their own, so the server
  sees one address per player, and sends the replies back.
- A guest's bridge listens on `127.0.0.1:--tunnel-port` (27960) and forwards
  to the match's host. The game connects there: `connect 127.0.0.1:27960`.
- Matchmaking is driven from the bridge's stdin (`queue duel`), and
  `--on-match-host` / `--on-match-join` run a shell command when a match is
  made, with `BW_MATCH_ID`, `BW_HOST`, `BW_PLAYLIST`, `BW_MODE`, `BW_MAP`,
  `BW_SETTINGS` (as `+set name "value"` arguments), `BW_CONNECT` and
  `BW_GAME_PORT` in the environment. For example:

  ```
  bwlive-steam --game stiletto --tunnel \
    --on-match-host 'fteqw $BW_SETTINGS +map $BW_MAP' \
    --on-match-join 'fteqw +connect $BW_CONNECT'
  ```

- Stock FTEQW finds Stiletto through a manifest, `default.fmf` in its
  basedir (with `-game base` alone it can't tell which game it is and loads
  nothing). `GAME` must not be `quake`, or the client won't connect without
  Quake's own data:

  ```
  FTEMANIFEST 1
  GAME stiletto
  NAME "Stiletto Proto"
  GAMEDIR base
  GAMEDIR stbuilt
  ```

Set reports and fighter cards need the engine's Live client, so a game on
stock FTEQW plays matches found on Steam but keeps no records.

## How it was checked

There's no Steam client in the cloud, so the bridge is tested against
`steam-mock` (`services/live/steam-mock`): a stand-in `libsteam_api.so`
exporting the same flat functions with the same callback layouts, and
`steam-mock-hub`, which holds the shared state (users, lobbies, messages,
leaderboards, cloud files) for every process that loads it. It copies the
Steam behaviour the bridge relies on: only the owner sets lobby data, each
member sets its own member data and everyone hears `LobbyDataUpdate`;
outsiders see a lobby's data but not its members; searches return public,
joinable, non-full lobbies; ownership passes on; messages from strangers
raise a session request and wait for it to be accepted. It is not Steam and
never talks to Valve; its accounts come from `STEAM_MOCK_ACCOUNT` and
`STEAM_MOCK_NAME`, the hub from `STEAM_MOCK_HUB`.

- `tests/live/steam.sh build services/live/target/release`: a dedicated
  host and a headless client, each with its own bridge, are paired on duel
  through lobby search, and the client reaches the host's game over Steam
  networking. Needs `FTEQCC`.
- `tests/live/steam_stiletto.sh build services/live/target/release BASEDIR`:
  Stiletto Proto on Bulwark, two headless players: matched, the host loads
  `duel_alley` with the playlist's settings, the guest joins, a set is
  played, the host's rules report it, both records change and both players
  have each other's fighter card.
- `tests/live/steam_party.py services/live/target/release`: parties and
  room codes, invites into a friends-only party, guests, a full party, 2v2
  with a party on one team, cancelling, Horde drop-in and leaving.
- `tests/live/steam_fteqw.sh services/live/target/release path/to/fteqw-gl
  BASEDIR`: two stock FTEQW clients (under Xvfb) with `--tunnel` and the
  match hooks: matched on duel, the host's FTEQW runs `duel_alley`, the
  guest's connects to its bridge's tunnel and joins the host's server, and
  everything between them (including the downloads) crosses Steam.

The struct offsets the bridge reads (`steam.rs`) were taken from the
Steamworks SDK 1.62 headers by compiling a probe against them on Linux
(x86-64, pack 4): `LobbyCreated_t` 12 bytes (lobby at 4), `LobbyEnter_t` 20
(response at 16), `LobbyDataUpdate_t` 20, `LobbyChatUpdate_t` 28 (change at
24), `LobbyInvite_t` 24, `GameLobbyJoinRequested_t` 16,
`SteamNetworkingIdentity` 136 (SteamID at 8), `SteamNetworkingMessage_t`
216 (size at 8, peer identity at 16, channel at 192), `LeaderboardEntry_t`
28 (details count at 16), `SteamAPICallCompleted_t` 16, `CallbackMsg_t` 20
(param at 8, size at 16). Windows uses pack 8, which moves a 64-bit field
that follows a 32-bit one (`LobbyCreated_t`, `UserStatsReceived_t`); the
bridge accounts for that, but it has only been run on Linux.

What only a real Steam client can show: lobby search latency and rate
limits, the distance filter, Steam Datagram Relay paths and their ping,
overlay invites, and Steam Cloud quotas.
