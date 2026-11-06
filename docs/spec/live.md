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

A datagram is at most 1200 bytes, so it survives any path MTU. Unknown types
are ignored, and a datagram shorter than its type needs is dropped.

So the server can't be used to amplify traffic at someone else, `hello` is
padded with zero bytes to at least 64 bytes (shorter ones are ignored), and
the server answers nothing from an address that hasn't sent `hello`. A
client that stops hearing from the server simply keeps sending `hello`.

### Client to server

| Type | Name | Body | Answer |
|---|---|---|---|
| 1 | `hello` | `u64` token, `str` game, `str` name, `addrs` LAN endpoints | `welcome`, every 5 s; it is also the keepalive |
| 2 | `bye` | | none; the player is dropped at once |
| 3 | `party_create` | `u8` max members (2-16), `u8` flags (bit 0: joinable by code), `str` info | `party` |
| 4 | `party_join` | `str` code | `party`, or `error` |
| 5 | `party_leave` | | `party_gone` |
| 6 | `party_set` | `u8` flags, `str` info (leader only) | `party` to every member |
| 7 | `invite` | `u32` player id | `invited` to that player |
| 8 | `queue` | `str` playlist, `u32` seniority (leader only) | `queue_status`, then `match` |
| 9 | `queue_cancel` | | `queue_status` with stage 255 |
| 10 | `relay` | `u32` to player id, payload | the server sends `forward` to them |
| 11 | `status` | `str` status line for friends lists | none |
| 12 | `list` | | `players` |
| 13 | `match_left` | `u64` match id | none; the party stops being linked to that match |
| 14 | `pong` | `u32` nonce from `ping` | none; the server measures round-trip time |

### Server to client

| Type | Name | Body |
|---|---|---|
| 64 | `welcome` | `u32` player id, `addr` public endpoint as the server saw it, `u16` hello interval in seconds |
| 65 | `error` | `u8` code (below), `str` text |
| 66 | `party` | `str` code, `u32` leader id, `u8` max, `u8` flags, `str` info, `u8` member count, then per member: `u32` id, `str` name, `addr` public, `addrs` LAN |
| 67 | `party_gone` | `u8` reason: 0 left, 1 kicked, 2 timed out, 3 server restart |
| 68 | `invited` | `u32` from id, `str` from name, `str` code |
| 69 | `introduce` | `u32` peer id, `str` name, `addr` public, `addrs` LAN, `u64` nonce |
| 70 | `forward` | `u32` from player id, payload |
| 71 | `queue_status` | `str` playlist, `u8` stage (255 = not queued), `u16` seconds waited, `u16` players searching this playlist |
| 72 | `match` | `u64` match id, `str` playlist, `u32` host player id, `u8` count, then per player: `u32` id, `u8` team, `str` name |
| 73 | `players` | `u8` count, then per player: `u32` id, `str` name, `str` status, `str` party code if joinable else empty |
| 74 | `ping` | `u32` nonce; sent after each `welcome`, answered at once with `pong` |

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

The host of a match is the leader of the longest-waiting party. (A later
version may prefer the member with the best direct connections, once clients
report them.) The game is told with `match`; the host starts its listen server and
the others connect to it the usual way, over the punched path or the relay.

After a match, parties stay together. For a "runback" the game simply queues
the same party again, or its leader keeps the lobby and starts directly.

## The SpacetimeDB module

The module `bwlive` (in `services/live/stdb`) holds the same state as
tables and runs the same matcher in a scheduled reducer every second.
Clients call reducers over SpacetimeDB's HTTP API and read their rows with
SQL or a subscription.

| Reducer | Same as |
|---|---|
| `hello(token, game, name, relay_id, public, lan)` | `hello`; `relay_id` and `public` come from the relay server's `welcome` |
| `bye()` | `bye` |
| `party_create(max, flags, info)`, `party_join(code)`, `party_leave()`, `party_set(flags, info)` | the party messages |
| `invite(player)` | `invite` |
| `queue(playlist, seniority)`, `queue_cancel()` | the queue messages |
| `set_status(status)` | `status` |
| `set_rtt(ms)` | the relay server's measurement, reported by the client |
| `add_playlist(json)`, `remove_playlist(id)` | operators only |

A player is the SpacetimeDB identity calling the reducer. Tables: `player`,
`party`, `party_member`, `invite`, `ticket`, `game_match`, `match_seat`,
`playlist` and `recent_pair`. Players time out 20 s after their last
`hello`. Punching uses the addresses in `party_member` and `match_seat`;
relaying goes through `bwlive-server --relay-only` by `relay_id`.
