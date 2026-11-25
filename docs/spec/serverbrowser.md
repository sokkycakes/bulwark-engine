# Server browser

Stiletto's server browser on Bulwark: a Panorama page that lists the servers
the master servers know, the ones on the LAN, favourites and history, and
joins one. It is "Find Servers" on the title menu (title-menu-v2, between
Join Lobby and Settings) and opens with `menu_webcore servers`.

| Piece | Where |
|---|---|
| Page | `data/panorama/{layout,scripts,styles}/web/server-browser.*` |
| Engine side of the page's requests | `src/client/cl_master.c`, "the server browser page's requests" |
| What servers answer | `src/server/sv_master.c` (`build_info`, `getstatus`) |
| Route, `menu_webcore servers` | `src/client/cl_webcore.c` (`ROUTE_SERVERS`) |
| Title menu item | `data/panorama/{layout,scripts,styles}/web/title-menu-v2.*` |
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

## The page

Top to bottom, on the hub's look (#2a2a2a, red wash, Stiletto's fonts):

- **Title band:** FIND SERVERS, and at the right what is listed (source,
  master servers that answered, how many servers match).
- **Toolbar:** the filter button, a search field (matches server name, map,
  mode and address; every typed word must match), refresh.
- **Sort row:** SORT BY: Name, Map, Players, Latency (the default), Last
  Played (newest join first; the default on History). Servers on another
  protocol always sort last and can't be joined.
- **The list:** one column of 8 two-line rows that scrolls with the
  selection. Line 1 is the server name; line 2 is tags: mode, Stiletto's
  arena state, Password, Favorite, LAN, You're here, Different version. On
  the right: the bot count with a bot icon, the map name over its picture,
  and a last column with latency bars and ping over players `n/max` (red
  when full).
- **Bottom band:** Close, Address (a dialog that joins any `host[:port]`),
  the smaller actions (Refresh, Favorite, Filters, source), Connect.
- **Filter menu** (from the filter button): Show (Internet, LAN, Favorites,
  History), Mode (All or a mode the list has), Empty / Full / Locked servers
  (Show or Hide).
- Connect on a locked server asks for its password first (typed text shows
  as `*`); a full or other-version server says so instead of trying. A
  server that turns a join away for its password (one joined by address, or
  whose lock the list didn't know) brings the page back with the password
  dialog open for that address.
- Opened without a `tab`, the page starts on Internet; when no master server
  answers (none is deployed for Stiletto's testers yet, or there is no
  internet) it moves to LAN after the refresh, or after 4 s, and says why
  ("online services are not connected"). Asking for a tab keeps it.

Controls. The controller moves between four rows, top to bottom: toolbar,
sort row, list, bottom band. Up from the first server goes to the sort row,
Down from the last to the bottom band, Left / Right move along a row.

| Action | Controller | Keyboard | Mouse |
|---|---|---|---|
| Move | D-pad / left stick | arrows, W / S | wheel scrolls the list |
| Page | LT / RT | PgUp / PgDn, Home / End | |
| Connect | A on a server, or the Connect button | Enter, Space | click the selected row, Connect |
| Close (title, or pause in a game) | B | Esc | Close |
| Search | A on the search field (needs a keyboard to type); A, B or Down leaves it | `/` | click the field |
| Sort | A on a sort label | | click a label |
| Filter menu | View, or A on the filter button; Up / Down, Left / Right or A changes, B closes | O | filter button |
| Source (Internet, LAN, Favorites, History) | LB / RB | Q / E | Show in the filter menu |
| Refresh | X | R, F5 | refresh button |
| Favourite | Y | F | Favorite hint |
| Address | Menu | C | Address |

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
