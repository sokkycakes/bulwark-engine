# Server browser

Stiletto's server browser on Bulwark: a Panorama page that lists the servers
the master servers know, the ones on the LAN, favourites and history, and
joins one. It is a row in the online hub ("Server Browser", above Back) and
opens with `menu_webcore servers`.

| Piece | Where |
|---|---|
| Page | `data/panorama/{layout,scripts,styles}/web/server-browser.*` |
| Engine side of the page's requests | `src/client/cl_master.c`, "the server browser page's requests" |
| What servers answer | `src/server/sv_master.c` (`build_info`, `getstatus`) |
| Route, `menu_webcore servers` | `src/client/cl_webcore.c` (`ROUTE_SERVERS`) |
| Master server | `services/master/bwmaster.py` |
| Tests | `tests/serverbrowser/master_test.py` (master alone), `tests/serverbrowser/run.py` (end to end) |

## Protocol

The DarkPlaces master protocol, as dpmaster's documentation describes it.
Bulwark already spoke it (cl_master.c, sv_master.c); FTEQW and DarkPlaces
speak it too.

- A public server (`sv_public 1`) sends `heartbeat DarkPlaces` to each
  `sv_master<n>` every 300 s and after a map change. The master answers
  `getinfo <challenge>`; the server's `infoResponse` must echo the challenge
  before the master lists it.
- The client sends `getservers <game> <protocol> empty full` and
  `getserversExt ... ipv6` to each `net_master<n>`. `<game>` is
  `com_protocolname`, which a game's manifest sets with `PROTOCOLNAME`
  (Stiletto's `base.fmf`: `StilettoSwift`). The master answers with 6-byte
  IPv4 entries (`\` + ip + port) and 18-byte IPv6 entries (`/` + ip + port),
  ending `\EOT`.
- The client then asks each server `getstatus <challenge>`. The time to the
  `statusResponse` is the ping. A LAN refresh broadcasts `getinfo` to UDP
  27500-27509 (a second server on a machine takes the next free port).

### What a server says

`infoResponse` and the first line of `statusResponse`:

| Key | Value |
|---|---|
| `gamename` | `com_protocolname` |
| `modname` | the write game dir |
| `protocol` | Bulwark's protocol version; the browser greys out others |
| `hostname`, `mapname`/`map`, `clients`, `bots`, `sv_maxclients`/`maxclients` | |
| `engine` | `BULWARK` |
| `password` | 1 when `sv_password` is set |
| `g_gametype` | from serverinfo, else the `g_gametype` cvar (Stiletto's mode) |
| `arena_state`, `arena_set`, `sv_region` | from serverinfo when set |

`statusResponse` also carries the rest of the public serverinfo (keys not
starting with `_`, at most about 1 KB) after those keys, then one line per
player: `frags ping "name"` (ping 0 for bots).

## The page's requests

Through the webcore bridge (`fte_query`). `sb_query "<request>"` prints the
reply in the console, for tests.

| Request | Reply |
|---|---|
| `sb_refresh[:internet\|lan\|favorites\|history]` | `ok`. Forgets that list's servers and asks again: the masters, a LAN broadcast, or each saved address. No tab means all four. |
| `sb_list` | `{game, protocol, masters, masters_answered, pending, refreshing, connected, server, other_games, servers:[row...]}`. Only servers that answered, and only `com_protocolname`'s game (`sb_list:all` includes the rest; `other_games` counts them). |
| `sb_details:<addr>` | a row plus `ok`, `answered`, `player_list:[{name, frags, ping, bot}]`, `rules:{...}`. Asks the server again if its status is over 5 s old. |
| `sb_ping:<addr>` | `ok` and asks again |
| `sb_connect:<addr>[ <password>]` | `{ok}`. Sets the `password` userinfo cvar (empty when none) and runs `connect`. |
| `sb_favorite:<addr>` | toggles; `{ok, favorite, addr}` |
| `sb_add:<addr>` | adds a favourite |
| `sb_clearhistory` | `ok` |

A row: `addr, name, map, game, mod, gametype, state, engine, players` (humans),
`bots, max, ping, protocol, password, compatible, internet, lan, favorite, history`.

Addresses must be 1-64 characters from `[A-Za-z0-9._:-[]]` and resolve to
an IPv4 or IPv6 address; they are stored as `ip:port`.

Cvars: `sb_favorites` and `sb_history` (archived, space-separated `ip:port`,
history newest first), `sb_history_max` (30), `password` (userinfo).
Every `connect` to an IP address goes into the history, whatever started it.

### When a join fails

After `sb_connect` the engine watches the connection. If it ends before the
game is up (refused, no answer), it prints `server browser: couldn't join
<addr> (<reason>)` and opens
`server-browser/index.html?failed=<addr>&reason=<reason>`; the page shows
the reason in words ("wrong or missing password", "the server is full").
Once the game is up the page closes itself.

## The page

- Tabs: Internet, LAN, Favorites, History. History is sorted newest first
  unless another sort is picked.
- Filters (a row of chips above the list): Mode (All or a mode the list
  has), hide empty, hide full, hide locked, Sort (ping, players, name, map).
  Servers on another protocol always sort last and can't be joined.
- The list shows 11 rows and scrolls with the selection; the right panel
  shows the selected server's mode, map, players with scores, Stiletto's
  arena state and flags.
- Join: a locked server asks for its password first (typed text shows as
  `*`); a full or other-version server says so instead of trying.
- Connect to address: a dialog that joins any `host[:port]`.

Controls; the hint bar shows whichever was used last:

| Action | Controller | Keyboard | Mouse |
|---|---|---|---|
| Move | D-pad / left stick | arrows, W / S | wheel |
| Page | LT / RT | PgUp / PgDn, Home / End | |
| Join | A | Enter, Space | click the selected row |
| Back (hub, or pause in a game) | B | Esc | Back hint |
| Tabs | LB / RB | Q / E, Left / Right | click a tab |
| Filters | Up from the first row, Left / Right, A | same with Enter | click a chip |
| Refresh | X | R, F5 | Refresh hint |
| Favourite | Y | F | Favorite hint |
| Sort | View | O | Sort chip |
| Address | Menu | C | Address hint |

Controller keys reach the page as the engine's key names (`e.bind` is
`GP_A`, `GP_DPAD_UP`...); `ui_key GP_A` sends the same event, which is how
the tests drive it.

## Running a master

    python3 services/master/bwmaster.py --port 27950 [--bind6 ::] [--http 127.0.0.1:27951]

Servers: `sv_public 1`, `sv_master1 <host>:27950`. Players: `net_master1
<host>:27950`. With no `sv_master<n>` / `net_master<n>` set, Bulwark uses the
public DarkPlaces masters (`master.frag-net.com`, `dpmaster.deathmask.net`,
`dpmaster.tchr.no`), which list any game name.

The master drops a server it hasn't heard from in `--timeout` seconds
(900), holds `--max-servers` (4096), at most `--max-per-ip` (32) from one
address, and `--http` serves its list as JSON for checking (keep it local).

## Findings

- **FTEQW dedicated servers** (this project's `fteqw-bulwark`, `make sv-rel`,
  run with Stiletto) send `heartbeat DarkPlaces` but answer `getinfo` and
  `getstatus` with an empty info string, so a DarkPlaces master can't list
  them and the browser can't show them. They answer QuakeWorld `status`
  with `n\<serverinfo>`. Found by probing the running server, not by
  reading its code. Bulwark and FTEQW can't join each other's servers
  anyway (different network protocols), so the browser lists Bulwark servers.
- FTEQW also heartbeats its built-in public masters even when `sv_master1`
  is set; block UDP 27950 outbound when testing it.
