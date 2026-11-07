# Bulwark Live: rendezvous, relay and matchmaking

Bulwark Live lets players find each other over the internet: room codes and
invites for the pre-game lobby ([lobby.md](lobby.md)), a public address for
each player, UDP hole punching between players, a relay when punching fails,
and matchmaking queues. It never runs gameplay. Games stay on a player's
listen server (or a dedicated server), as today.

It comes in two forms that follow the same rules:

| Form | What runs | Where the state lives |
|---|---|---|
| Standalone | `bwlive-server` (one UDP port, plus an optional HTTP status page) | in the server process |
| SpacetimeDB | the `bwlive` module on a SpacetimeDB database, plus `bwlive-server --relay-only` for the UDP parts | in the database |

SpacetimeDB can't send or receive raw UDP, so address reflection, punching
help and relaying always need the small UDP server. In SpacetimeDB form that
server keeps no parties, queues or matches; the module does.

The rules both forms share (room codes, party limits, the matcher) are one
Rust crate, `bwlive-core`, so they can't drift apart. Everything is in
`services/live/`. Original code, ISC licence. The design was informed by
public descriptions of Xbox 360-era lobbies (Halo 3, Halo: Reach, Left 4
Dead) and by Reach Live (RecompiledHaloReach, MIT); no code was taken from
them.

## Terms

- **Player:** one running game, known to the server by a random 64-bit
  `token` it picks at start (kept in `live_identity.txt` so it survives
  restarts) and, while connected, a 32-bit `player id` the server assigns.
- **Party:** players who stay together between matches. It has a six-letter
  **room code**, a **leader** (the one who creates it, then the longest
  present member), a size limit, and an opaque `info` string that the game
  owns (Stiletto puts its lobby summary there as JSON). The party's lobby
  host is the leader; the game decides who hosts a match.
- **Guests:** split-screen players on the same console, Xbox 360 style. A
  player's `hello` names up to three guests, so one console holds up to four
  local players. A guest has no token or id of their own: they ride on their
  console's connection, address and party, and are always in the same party
  (so the same team) as their console. Guests count toward a party's size
  and a match's seats. A match names each seat by console `id` and `slot`
  (0 the console's own player, 1-3 its guests). Sending `hello` with a
  different guest list (someone picked up or put down a controller) updates
  the party and restarts any search. Introductions and punching are per
  console.
- **Ticket:** a party waiting in a playlist's queue.
- **Playlist:** a queue with its rules (team size, teams, how the search
  widens). Defined in `playlists.json` (standalone) or the `playlist` table.
- **Match:** a set of parties the matcher put together, with a chosen host.

## Room codes

Six characters from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` (no `I`, `O`, `0`,
`1`), shown and typed in any case. A code stays the party's for its whole
life and is freed when the party ends.

## Wire format (UDP)

Every datagram starts with the four bytes `BWLV`, a version byte (`1`) and a
type byte. Integers are big-endian. Types used below:

- `u8`, `u16`, `u32`, `u64`: unsigned integers.
- `str`: `u8` length, then that many bytes of UTF-8 (at most 255).
- `addr`: `u8` family (`4` or `6`), the address (4 or 16 bytes), `u16` port.
- `addrs`: `u8` count, then that many `addr`s.
- `strs`: `u8` count, then that many `str`s (guest names, at most 3).

A datagram is at most 1200 bytes, so it survives any path MTU, except
`relay`, `forward` and `data`, which may be up to 1472 bytes to carry a full
1400-byte game packet. Unknown types
are ignored, and a datagram shorter than its type needs is dropped.

So the server can't be used to amplify traffic at someone else, `hello` is
padded with zero bytes to at least 64 bytes (shorter ones are ignored), and
the server answers nothing from an address that hasn't sent `hello`. A
client that stops hearing from the server simply keeps sending `hello`.

### Client to server

| Type | Name | Body | Answer |
|---|---|---|---|
| 1 | `hello` | `u64` token, `str` game, `str` name, `addrs` LAN endpoints, `strs` guests | `welcome`, every 5 s; it is also the keepalive |
| 2 | `bye` | | none; the player is dropped at once |
| 3 | `party_create` | `u8` max members (2-16), `u8` flags (bit 0: joinable by code), `str` info | `party` |
| 4 | `party_join` | `str` code | `party`, or `error` |
| 5 | `party_leave` | | `party_gone` |
| 6 | `party_set` | `u8` flags, `str` info (leader only) | `party` to every member |
| 7 | `invite` | `u32` player id | `invited` to that player |
| 8 | `queue` | `str` playlist, `u32` seniority, optional `str` preferred map (leader only; a map the playlist doesn't play counts as none, and a new match plays the first seated party's preference in a mode that has it) | `queue_status`, then `match` |
| 9 | `queue_cancel` | | `queue_status` with stage 255 |
| 10 | `relay` | `u32` to player id, payload | the server sends `forward` to them |
| 11 | `status` | `str` status line for friends lists | none |
| 12 | `list` | | `players` |
| 13 | `match_left` | `u64` match id | none; the party stops being linked to that match (and frees its seats in a drop-in match) |
| 14 | `pong` | `u32` nonce from `ping` | none; the server measures round-trip time |
| 15 | `playlists` | | `playlist_info` |

### Server to client

| Type | Name | Body |
|---|---|---|
| 64 | `welcome` | `u32` player id, `addr` public endpoint as the server saw it, `u16` hello interval in seconds |
| 65 | `error` | `u8` code (below), `str` text |
| 66 | `party` | `str` code, `u32` leader id, `u8` max, `u8` flags, `str` info, `u8` member count, then per member: `u32` id, `str` name, `addr` public, `addrs` LAN, `strs` guests |
| 67 | `party_gone` | `u8` reason: 0 left, 1 kicked, 2 timed out, 3 server restart |
| 68 | `invited` | `u32` from id, `str` from name, `str` code |
| 69 | `introduce` | `u32` peer id, `str` name, `addr` public, `addrs` LAN, `u64` nonce |
| 70 | `forward` | `u32` from player id, payload |
| 71 | `queue_status` | `str` playlist, `u8` stage (255 = not queued), `u16` seconds waited, `u16` players searching this playlist |
| 72 | `match` | `u64` match id, `str` playlist, `u32` host player id, `u8` count, then per seat: `u32` id, `u8` slot, `u8` team, `str` name; then `str` mode, `str` map, `u8` count, then per setting: `str` name, `str` value (all empty for a playlist without modes) |
| 73 | `players` | `u8` count, then per player: `u32` id, `str` name, `str` status, `str` party code if joinable else empty |
| 74 | `ping` | `u32` nonce; sent after each `welcome`, answered at once with `pong` |
| 75 | `playlist_info` | the rest of the datagram is JSON: `[{id, name, description, players, min_players, teams, drop_in, searching, online, modes: [{id, name, maps}]}]` (`online` = searching plus seated in its running drop-in matches) for the caller's game; maps are dropped if it wouldn't fit one datagram |

Error codes: 1 not registered (reserved: the server stays silent instead), 2 wrong version, 3 server
full, 4 no such room code, 5 party full, 6 not the leader, 7 no such
playlist, 8 rate limited, 9 bad request.

### Player to player

| Type | Name | Body |
|---|---|---|
| 96 | `punch` | `u32` from id, `u32` to id, `u64` nonce |
| 97 | `punch_ack` | `u32` from id, `u32` to id, `u64` nonce |
| 98 | `data` | `u32` from id, payload |

A `relay`, `forward` or `data` payload starts with two bytes, the source and
destination **virtual port**, so one UDP socket carries several streams: 1 is
the pre-game lobby, 2 a game client, 3 a game server.

## Connecting two players

The server sends `introduce` to both players when one joins the other's
party, when a match puts them together, and (with `relay`) whenever the
first relayed packet reaches someone who hasn't been introduced yet. Both
then send `punch` to the other's public endpoint and each LAN endpoint,
every 300 ms for 6 s, from the same socket they talk to the server on. The
first `punch` that arrives is answered with `punch_ack`, and the path it came
in on becomes the direct path. Until there is one, or if there never is,
traffic goes through the server with `relay`. A failed attempt may be
retried after 30 s. (These timings match what Reach Live found workable.)

The relay is limited per player (default 64 KB/s with a 4 s burst), and
counts are shown on the status page.

## Matchmaking

The matcher runs once a second. For each playlist it takes the waiting
tickets and puts parties together into matches of `teams × team_size`
players, never splitting a party. Who may be paired with whom widens with
time, in **stages** defined per playlist:

```json
{
  "id": "duel",
  "name": "Duel",
  "team_size": 1,
  "teams": 2,
  "stages": [
    { "after": 0,  "seniority_ratio": 1.5, "max_rtt_sum": 150 },
    { "after": 15, "seniority_ratio": 3.0, "max_rtt_sum": 250 },
    { "after": 40, "seniority_ratio": 0,   "max_rtt_sum": 400 },
    { "after": 60, "seniority_ratio": 0,   "max_rtt_sum": 0 }
  ],
  "avoid_rematch_minutes": 10
}
```

- `after`: seconds a ticket has waited before this stage applies.
- `seniority_ratio`: two parties may meet if the larger seniority is at most
  this many times the smaller (both counted as at least 1). `0` means any.
  Seniority is what the game reports with `queue`. Stiletto sends the
  number of days with a completed set (see the Stiletto ranked design); a
  2v2 pair sends its mean.
- `max_rtt_sum`: the two parties' round-trip times to the server, added, in
  ms, must be at most this. A party's time is its slowest member's, measured
  with `ping`/`pong` (in SpacetimeDB form the client times its own
  `hello` to `welcome` round trip to the relay and reports it with
  `set_rtt`). It is a rough stand-in for their ping to each
  other (the server can't measure that). `0` means any.
- `avoid_rematch_minutes`: the same two players aren't paired again within
  this time unless one of them has waited past the last stage.

A pair of tickets is allowed when both tickets' current stages allow it (the
stricter of the two applies). Among allowed groups the matcher prefers the
ticket that has waited longest, then the closest seniority. With
`min_players` set below `teams × team_size`, a ticket that has waited past
the last stage starts with fewer players (for games that fill seats with
bots).

### Game modes in the hopper

A playlist may list **modes**, as Reach's and Gears 3's hoppers do (Firefight
and its variants under one playlist, Horde under another). Each match picks
one mode, weighted by `weight`, then one of its `maps` at random, and never
the previous match's mode and map on that playlist while anything else is
left. `match` carries the pick; the host sets each of the mode's `settings`
as a console variable and then loads the map. A co-op playlist is one team
(`"teams": 1`) and needs no seniority, so its stages only widen ping:

```json
{
  "id": "firefight", "name": "Firefight", "game": "coop",
  "team_size": 4, "teams": 1, "min_players": 2,
  "stages": [
    { "after": 0,  "max_rtt_sum": 150 },
    { "after": 20, "max_rtt_sum": 300 },
    { "after": 45 }
  ],
  "modes": [
    { "id": "firefight", "name": "Firefight", "maps": ["coop_arena"], "weight": 3,
      "settings": { "coop_waves": "1", "coop_survival": "0", "coop_waveDelay": "10" } },
    { "id": "grunt_rush", "name": "Grunt Rush", "maps": ["coop_arena"],
      "settings": { "coop_waves": "1", "coop_waveMonsters": "monster_human_grunt" } }
  ]
}
```

- `drop_in`: see Drop-in below.
- `modes`: at most 16. `id` is letters, digits, `_` and `-`; `name` is
  for menus. `weight` defaults to 1; 0 takes a mode out of the rotation.
- `maps`: at most 32 names of letters, digits, `_ - . /` (no `..`). An
  empty list means the host's own default map.
- `settings`: console variable names (letters, digits, `_`) and values
  (printable ASCII without `" ; \`), at most 360 bytes encoded, so a
  16-seat `match` still fits a datagram. Names starting with `live_` or
  `rcon` or holding `password` are refused. The engine checks all of this
  again before applying anything.

### Drop-in

With `"drop_in": true` (one team only) a playlist's matches stay open after
they start. Each second, before any new match is made, waiting tickets go
into running matches that have room: the fullest match first, then the
lowest ping, within the ticket's current stage's `max_rtt_sum` (seniority
and rematches don't count). The newcomers get `match` for the running match
and connect to its host; everyone already in gets the same `match` again
with the new seats (the engine prints "the match now has N players"). A
ticket that finds no room waits for a match of its own, which with
`min_players` 1 starts alone once the last stage is reached.

A party is in a match until it sends `match_left` (the engine does that 10 s
after its game stops running: a client left the host, or the host's server
shut down), leaves its party, queues again or times out. Its seats are then
freed and the others told. Once the host's party leaves, the match takes
nobody else.

The built-in playlists are Stiletto's `duel` and `2v2` (game `stiletto`)
and the co-op mod's, all game `coop`:

- `campaign`: Half-Life chapters, no waves, 4 players.
- `firefight`: Firefight and Grunt Rush on `coop_arena`, 4 players, the
  team sharing 7 lives with one more per cleared wave (`coop_respawnRule
  lives`).
- `horde`: up to 8 players, drop-in, starting alone after 8 s. Horde brings
  the downed back at the end of each wave (`coop_respawnRule wave`); Horde:
  Lives shares 5 lives instead. Bots fill the team to 4 and make room for
  each player who drops in (`coop_survivorFill 4`).

In-game votes (`callvote map`, `nextmap`, `map_restart`, `skill`) work in
any of them, as on any co-op server.

The host of a match is the leader of the longest-waiting party. (A later
version may prefer the member with the best direct connections, once clients
report them.) The game is told with `match`; the host starts its listen server and
the others connect to it the usual way, over the punched path or the relay.

After a match, parties stay together. For a "runback" the game simply queues
the same party again, or its leader keeps the lobby and starts directly.

## The SpacetimeDB module

The module `bwlive` (in `services/live/stdb`) holds the same state as
tables and runs the same matcher in a scheduled reducer every second.
Clients call reducers over SpacetimeDB's HTTP API. All tables are private
(they hold addresses); a client reads what concerns it with the procedures
`state()` (its party, invites, queue and match as JSON), `players()` and
`list_playlists()`, polled about once a second while something is going on.

| Reducer | Same as |
|---|---|
| `hello(game, name, relay_id, public, lan, guests)` | `hello`; the caller's SpacetimeDB identity takes the place of `token`; `relay_id` and `public` come from the relay server's `welcome` |
| `bye()` | `bye` |
| `party_create(max, flags, info)`, `party_join(code)`, `party_leave()`, `party_set(flags, info)` | the party messages |
| `invite(player)` | `invite` |
| `queue(playlist, seniority, map)`, `queue_cancel()` | the queue messages |
| `set_status(status)` | `status` |
| `set_rtt(ms)` | the relay server's measurement, reported by the client |
| `match_left(match_id)` | `match_left` |
| `add_playlist(json)`, `remove_playlist(id)`, `add_operator(identity, note)` | operators only (the publisher is the first) |

A player is the SpacetimeDB identity calling the reducer. Tables: `player`,
`party`, `party_member`, `party_invite`, `ticket`, `game_match`, `match_seat`,
`playlist` and `recent_pair`. Players time out 20 s after their last
`hello`. Punching uses the addresses in `party_member` and `match_seat`;
relaying goes through `bwlive-server --relay-only` by `relay_id`.

## The engine client

`src/net/live.c` is the client. It opens one UDP socket for everything:
the server, punches, direct traffic and the relay.

| Command | Does |
|---|---|
| `live_connect [host[:port]]` | registers with a server (`live_server`, port 27950) |
| `live_disconnect` | sends `bye` |
| `live_party [max] [open] [info]`, `live_join <code>`, `live_leave`, `live_open <0\|1> [info]` | the party messages |
| `live_invite <id>`, `live_list` | invites and the player list |
| `live_queue <playlist> [seniority] [map]`, `live_cancel` | matchmaking |
| `live_guests <name>... \| -` | the console's split-screen guests (sends `hello` at once) |
| `live_setstatus <text>` | the friends-list status line |
| `live_playlists` | asks for the game's playlists again (also asked once after the first `welcome`) |
| `live_status`, `live_peers` | what the client knows, and which peers are direct or relayed |

Cvars:

- `live_game` names the game. Players only meet players of the same game.
- `live_onconnect` holds commands to run each time the client (re)connects.
- `live_hostcmd` holds commands to run when a match picks this game to host.
  When it is empty and the match names a map, the host runs `map <map>`.
  Either way the mode's settings are applied first (each is printed as
  `live: <name> "<value>"`). `$live_match_map` works inside it.
- With `live_autoconnect` (1), a client that isn't hosting runs
  `connect live:<host>`.
- `live_punch` (1) tries direct paths. Set it to 0 to relay everything.
- `live_port` is the local UDP port.
- These are read-only:
  - `live_state`: everything as JSON, for menus and QC. Besides the party,
    queue and match it carries `guests`, `playlists` (the last
    `playlist_info`, or `[]`), and `error` with `error_seq`, which goes up
    by one with each `error` so a menu shows each one once.
  - `live_party`: the room code
  - `live_match_host`, `live_match_id`, `live_match_playlist`,
    `live_match_mode` and `live_match_map`: the last match

Game traffic uses the address type `live:<id>[:<vport>]`, `NA_LIVE` in
`net.h`. Without a port it means the peer's game server (vport 3). So
`connect live:12345` reaches a listen server behind a NAT, and the server
answers to `live:<id>:2`. A server needs no change: its packets to a
`live:` address go out through the Live socket.

The pre-game lobby ([lobby.md](lobby.md)) uses Live when it is connected:

- An `online` lobby takes its Live party's room code.
- `lobby_join <code>` falls back to `live_join` when the code isn't on the
  LAN, then joins the party leader's lobby over vport 1.
- When the game starts, guests connect to `live:<host>`.
- Lobby messages longer than one datagram are sent in pieces of 1200 bytes.
  Each piece starts with a `u16` message number, a `u8` index and a `u8`
  count, and a message is used only once all its pieces have arrived.

This client is IPv4 only, for now.

Tests:

- `tests/live/run.sh build bwlive-server`: a dedicated host and a client are
  matched on `duel`, and the client joins the host's game. With
  `LIVE_PUNCH=0` everything goes through the relay.
- `tests/live/lobby.sh build bwlive-server`: an online lobby is found by
  room code over Live and started.
- `tests/live/coop.sh build bwlive-server`: a one-team playlist with a game
  mode; the host applies the mode's settings, loads its map without a
  `live_hostcmd`, and the client joins.
- `tests/live/dropin.sh build bwlive-server`: a host starts a drop-in match
  alone, a client drops in later, and when the host kicks it the client
  reports leaving and its seat is freed.

All four need `FTEQCC`.
