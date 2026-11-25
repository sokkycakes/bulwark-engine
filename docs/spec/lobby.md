# The pre-game lobby

A lobby is a party that exists before any game server: players gather, the
host picks the map and rules, everyone readies up, and when the countdown
ends the host starts a listen server and every guest connects to it. Games
reach it through the console commands below and, for HTML-style menus,
through the WebCore bridge (`getlobby`, `getlobbychat`, `lobby_action:*`,
`hostcache_refresh`, `gethostcache`; see [webcore.md](webcore.md) sections
3 and 9, which describe what Stiletto's pages expect). The code is
`src/client/cl_lobby.c`.

Nothing here comes from another engine. The behaviour follows
[webcore.md](webcore.md) section 9, written from Stiletto's own pages, QC,
configs and design notes; the wire format below is Bulwark's own, since the
reference engine's was never documented.

## Commands and cvars

| Command | |
|---|---|
| `lobby_create <lan\|online\|offline>` (`lobby_create_lan`, ...) | Host. `lan` and `online` listen on `lobby_port`; `online` also makes a six-character room code. `offline` has no network (tests). |
| `lobby_join <address\|room code>` | Join. Something with a dot or colon, or `localhost`, is an address (port `lobby_port` unless given); anything else is a room code, looked for on this network (below). |
| `lobby_ready [1\|0]` | Ready (or not). |
| `lobby_start` / `lobby_cancel` | Host: start now / stop the countdown. |
| `lobby_set ruleset <id>` / `lobby_set map <id>` / `lobby_limits <minutes> <frags>` | Host: the game's settings. |
| `lobby_chat_hex <hex>` | Say something: UTF-8 bytes as hex, at most 240. |
| `lobby_close` | Leave, or close the lobby for everyone when hosting. |
| `lobby_snapshot` / `lobby_chat_snapshot` | Print the state (and write `lobby_snapshot.json`) / the chat. |

| Cvar | Default | |
|---|---|---|
| `lobby_port` | 27501 | the host's UDP port |
| `lobby_readytime` | 5 | countdown seconds once everyone is ready |
| `lobby_maxplayers` | 16 | seats |
| `lobby_defaultmap` | (empty) | when set, the map every new lobby picks; empty picks the ruleset's own map (below) |
| `lobby_defaultruleset` | DUEL | the rules a new lobby picks, when they can run (below) |
| `lobby_snapshot` | | the state as JSON (read-only); also written to `lobby_snapshot.json` in the working directory while `developer` is on |

The legacy mirrors `lobby_room_code`, `lobby_host_name`, `lobby_state_str`,
`lobby_player_count`, `lobby_max_players` and `lobby_player1_name` ..
`lobby_player4_name` (with a `[READY] ` prefix) are kept for menus written
against them. MenuQC can add rulesets with `lobby_registermode(id, name,
start_cmds)` and `lobby_addteam(...)`.

## Maps and rulesets

The game names what its host can pick in `scripts/lobby.txt`, one per line
(`//` comments):

```
map <id> "<title>"
ruleset <ID> "<title>" [<map the ruleset picks>]
```

- A listed map is offered only when `maps/<id>.bsp` (or `.scene`) is in the
  game. With no file, or no listed map present, every `maps/*.bsp` is offered
  under its own name.
- With no `ruleset` lines the built-in rulesets are offered: DUEL "Duel",
  DEATHMATCH "Deathmatch", TEAMDM "Team Deathmatch", DOMINATION
  "Domination", FREEZETAG "Freeze Tag", ARCHSTILETTO "Archstiletto".
  Rulesets MenuQC registers are added to either list.
- A ruleset runs from `progs/<id in lower case>.dat` (Nuclide). When that
  file isn't in the game the ruleset is still listed but can't be picked
  (`available: false`; `lobby_set ruleset` and `setruleset:` refuse it), since
  a Nuclide server without its rules progs spawns nobody. A game with none of
  these progs (not Nuclide) runs every ruleset alike.
- A new lobby picks `lobby_defaultruleset`, or the first listed ruleset that
  can run when it can't, and then `lobby_defaultmap` when set, else that
  ruleset's map, else the first listed map. Until the host picks a map,
  switching ruleset also switches to the new ruleset's map (Duel picks
  `duel_alley` in Stiletto).
- The lists are read when a lobby is created (and again when the search path
  changes), and `getlobby` carries them as `maps` (`[{id, name}]`) and
  `rulesets` (`[{id, name, map, available}]`). The lobby page shows them and
  falls back to its own lists when they are missing.

## Rules

- Phases: IDLE, CREATING, JOINING, WAITING, COUNTDOWN, STARTING, INGAME,
  CLOSING. CLOSING shows its reason for 1.5 s, then IDLE.
- The host owns everything; guests only ask (ready, unready, chat, leave).
  A guest is known by its address and the token the host gave it.
- When every occupied seat is ready (the host included), half a second
  after the last join or leave, the host counts down `lobby_readytime`
  seconds. An unready, a join or leave, or a change of map, rules or limits
  stops the countdown and clears everyone's ready. The host's Start skips
  the wait whether or not everyone is ready (Stiletto's own tests start
  after a cancel cleared everyone).
- Joins are refused when full, after the countdown has started, or with
  another protocol version.
- Starting, the host runs, ahead of anything waiting in the command
  buffer: `sv_background 0`, `coop 0`, `deathmatch 1`, `maxplayers` and
  `sv_playerslots` set to the seats, `g_gametype` set to the ruleset in
  lower case (Nuclide loads `progs/<g_gametype>.dat`), `timelimit` and
  `fraglimit` when set, a registered mode's start commands, then
  `map <map>`. Once its server is up and it is in the game, it tells each
  guest the game port; a guest connects to the host's address it already
  knows, on that port (dropping a local title map first).
- A guest silent for 10 s loses its seat; a host silent for 10 s closes the
  lobby on the guest's side.

## Wire format

UDP datagrams, each the text `BWLB1 ` followed by one JSON object whose
`t` member is its type, and `sid`, the session's number (random per lobby).
Guests send from a port of their own; the host listens on `lobby_port`.

| `t` | From | Members | |
|---|---|---|---|
| `discover` | anyone | | broadcast to `lobby_port` and sent to 127.0.0.1; a host in WAITING answers with `info` |
| `info` | host | `proto`, `name`, `map`, `code`, `players`, `max` | becomes a `gethostcache` row |
| `join` | guest | `proto` (1), `name` | resent every 0.5 s, 20 times at most |
| `accept` | host | `seat`, `tok` | the guest's seat and token |
| `reject` | host | `reason` | |
| `intent` | guest | `tok`, `seq`, `kind` (`ready`, `unready`, `chat` with `text`, `leave`, `ping` with seq 0) | resent every 0.25 s until a snapshot's `ack` covers its `seq`; pings once a second |
| `snap` | host | `rev`, `phase`, `status`, `map`, `code`, `network`, `max`, `remaining` (countdown seconds, -1 for none), `ack`, `seat`, `settings`, `players`, `chat` (the last 10 lines) | on every change and once a second; answers each intent |
| `close` | host | `reason` | the host closed the lobby |
| `start` | host | `nonce`, `port`, `map` | resent each frame until acknowledged |
| `startack` | guest | `tok`, `nonce` | a repeated `start` is acknowledged but connects only once |

The countdown travels as time remaining, never as a clock reading, so the
two machines' clocks are never compared.

## Room codes

Online lobbies get a code like `A41B9C`. A code is first resolved by asking
this network (`discover`, 0.3 s) for a lobby advertising it. When Bulwark
Live is connected (`live_connect`, see [live.md](live.md)), an online
lobby's code is its Live party's code, and a code not found on the LAN is
joined over the internet through Live (punched or relayed). Without Live,
codes work on one machine and across a LAN only.
