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
| `lobby_join <address\|room code> [password]` | Join. Something with a dot or colon, or `localhost`, is an address (port `lobby_port` unless given); anything else is a room code, looked for on this network (below). The password is for a lobby that has one. |
| `lobby_ready [1\|0]` | Ready (or not). |
| `lobby_start` / `lobby_cancel` | Host: start now / stop the countdown. |
| `lobby_set ruleset <id>` / `lobby_set map <id>` / `lobby_limits <minutes> <frags>` | Host: the game's settings. |
| `lobby_set servertype <listen\|dedicated>` / `maxplayers <1-16>` / `bots <n\|fill\|default>` / `public <0\|1>` / `hostname <text>` / `password <text>` | Host: the server the lobby starts (below). An empty name or password clears it. |
| `ded_stop` / `ded_status` | Stop / ask about the dedicated server this game started. |
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
| `lobby_hostname` / `lobby_password` / `lobby_bots` / `lobby_public` / `lobby_servertype` | (empty) / (empty) / (empty: default) / 0 / listen | what a new lobby's server settings start from |
| `ded_exe` | (empty) | the dedicated server program; empty: `bulwark-ded` beside this program, else this program with `-dedicated` |
| `ded_port` | 27500 | the first UDP port tried for it (the first free one of the next 32 is used) |

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
- Joins are refused when full, after the countdown has started, with
  another protocol version, or without the lobby's password (below).
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

## Hosting options

The host also picks the server the lobby starts. `getlobby`'s `settings`
carries them (`servertype` LISTEN SERVER or DEDICATED SERVER, `hostname`,
`maxplayers`, `bots`: a number or -1 for fill, `botsDefault`: true when the
lobby leaves bots to the rules (then `bots` is 0), `public`, `password`: whether
there is one, and `passwordText` on the host's own page only), and `hosting`
says what this engine can host: `serverTypes` (`listen`, and `dedicated`
where programs can be started), `maxSeats`, `online` and `onlineConfigured`
(Bulwark Live), `dedicatedRunning`. The page changes them with
`setserver:<listen|dedicated>:<max>:<bots|fill>:<public 0|1>:<name hex>:<password hex>`
(UTF-8 as hex, empty for none; an empty or `default` bots field is the
rules' own bots); a refusal answers
`{"ok":false,"error":"<why>"}`. Refused: fewer seats than players in the
lobby, more bots than free seats, a name or password with `"`, `\` or `;`
or over 63 bytes, Dedicated where programs can't be started or no server
program is found (without /proc, as on macOS, only `ded_exe` names one; the
page isn't offered `dedicated` then). A change stops
the countdown like any other.

- **Password.** A lobby with a password turns a `join` without the right
  `pass` away with `reject` and `needpass: true` ("This lobby needs a
  password" / "Wrong password"). The guest's `getlobby` then has
  `needPassword` and `joinTarget` while it is IDLE or CLOSING, the lobby
  page stays open and asks, and `joinpass:<hex>` joins again with it
  (`close` gives up). The `start` message carries the password, which the
  guest sets as its `password` userinfo before it connects; the server runs
  with `sv_password` set to it. The lobby's password stays while the guest
  may still go back to that game: after a connection timeout, `reconnect`
  still gets in (as in FTE, where the userinfo simply stays), and the
  title's backdrop map in between doesn't change that (a local client never
  needs a password). The guest's own `password` comes back as soon as the
  lobby's game is over for it, whatever comes next: it disconnects from that
  server (`disconnect`, Leave, or the server drops it or shuts down), it
  connects to any other server (the address differs from the one it
  connected to after `start`), it starts a local game of its own other than
  the backdrop, or the start fails. So the lobby's password is never sent
  to another server.
- **Starting** adds `hostname` (the host's name, else "<host>'s server"),
  `sv_password`, `sv_public` and bots to the commands above. Bots use the
  game's own quota, Nuclide's `bot_minClients`: the lobby's players plus the
  count (at most `maxplayers`), `maxplayers` for fill, 0 for none. The
  default (empty Bots) sets neither cvar, so the rules seat their own bots
  as they do for a console `map`. A ruleset
  marked `seatbots` in `scripts/lobby.txt` (a fourth word on its line; built
  in: FREEZETAG and ARCHSTILETTO) fills its own seats, and a quota would
  fight it: it gets `bot_minClients -1` and `arena_bots` 1 (any bots) or 0.
  On a listen server these are the host's own cvars: the lobby keeps what
  `hostname`, `sv_password`, `sv_public`, `bot_minClients`, `arena_bots`,
  `maxplayers`, `sv_playerslots`, `timelimit`, `fraglimit`, `g_gametype`,
  `deathmatch`, `coop` and `sv_background` were, and every cvar the mode's
  start commands set (`set`/`seta`/`sets`/`setu <cvar>`, or `<cvar>
  <value>`), and puts them back once that game's server stops
  (`disconnect`, `killserver`), or when the start fails, so a later console
  `map` doesn't inherit them. A cvar that didn't exist before goes back to
  its default. The values kept are those from before the first lobby game,
  however many follow before the server stops.
  A player joining a server that bots fill takes a bot's slot
  (`sv_botsyield`, server side).
- **Dedicated** (`src/client/cl_dedlaunch.c`): instead of `map`, the host
  stops its title map, writes the same commands, a random `rcon_password`
  and `map` to `lobby_dedicated.cfg` in its game directory, and starts the
  dedicated server program in the background: `-basedir`, every `-game`,
  `-userdir` and `-enginedata` as this client has them, `+set sv_port <the
  first free port from ded_port>` and `+exec lobby_dedicated.cfg`, its
  standard input from /dev/null, its output in `dedicated.log` beside the
  config, in a session of its own. The host then sets `password`,
  `rcon_password` and `rcon_address` (so `rcon` reaches the server, also
  after a timeout drops the host out of it). All three go back to what they
  were once the lobby's game is over for the host: it disconnects from that
  server or the server drops it, the server stops (`ded_stop`, or it exits),
  the host connects to any other server, the start fails, or the host
  starts a local game of its own. `rcon_password` is also the cvar a listen
  server checks, so even the title's backdrop map takes the rcon settings
  back (the password stays there, as for a guest). The host
  connects to 127.0.0.1 on that port straight away (not through the command
  buffer, which a script's `wait` holds), again every 12 s while it isn't in.
  Guests get that port in `start`. If the program exits first, the lobby
  closes with "The dedicated server stopped"; if the game doesn't start in
  30 s the lobby stops the server it launched. The server keeps running when
  the host leaves, as a dedicated server does; `ded_stop` stops it.
- **The hand-off.** From Start until this client is in the lobby's game
  (or the start fails, or 90 s pass), the session can already be over
  (every guest has the address) while the title map still runs and the
  game loads. The lobby page stays open meanwhile, showing STARTING and
  "Starting the game..." with only Leave to press; the title page does not
  come back over it. Leave (`lobby_close`) then abandons the start for this
  client: it disconnects from the game, stops its own listen server (or a
  dedicated server no guest has heard of yet), puts back the cvars the
  lobby set, and only then is the title page back. A dedicated server the
  guests already have keeps running, as above.
- **Online without Bulwark Live.** An `online` lobby's host whose Live isn't
  connected gets `warning` ("Online services are not connected in this
  build: this room code works on this network only."), and a room code not
  found on this network says the same instead of only "not found".

## Wire format

UDP datagrams, each the text `BWLB1 ` followed by one JSON object whose
`t` member is its type, and `sid`, the session's number (random per lobby).
Guests send from a port of their own; the host listens on `lobby_port`.

| `t` | From | Members | |
|---|---|---|---|
| `discover` | anyone | | broadcast to `lobby_port` and sent to 127.0.0.1; a host in WAITING answers with `info` |
| `info` | host | `proto`, `name`, `map`, `code`, `players`, `max` | becomes a `gethostcache` row |
| `join` | guest | `proto` (1), `name`, `pass` (when the guest has one) | resent every 0.5 s, 20 times at most |
| `accept` | host | `seat`, `tok` | the guest's seat and token |
| `reject` | host | `reason`, `needpass` (true when the lobby's password is missing or wrong) | |
| `intent` | guest | `tok`, `seq`, `kind` (`ready`, `unready`, `chat` with `text`, `leave`, `ping` with seq 0) | resent every 0.25 s until a snapshot's `ack` covers its `seq`; pings once a second |
| `snap` | host | `rev`, `phase`, `status`, `map`, `code`, `network`, `max`, `remaining` (countdown seconds, -1 for none), `ack`, `seat`, `settings` (with the hosting options but never `passwordText`), `players`, `chat` (the last 10 lines) | on every change and once a second; answers each intent |
| `close` | host | `reason` | the host closed the lobby |
| `start` | host | `nonce`, `port`, `map`, `pass` (the server's password, or empty) | resent each frame until acknowledged |
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
