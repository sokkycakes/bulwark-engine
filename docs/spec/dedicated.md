# Dedicated server

`bulwark-ded` runs a server with no window, sound or client. The game's own
binary does the same with `-dedicated` (as FTE's does), so a tester can
host with what they already have:

```sh
bulwark-ded -basedir . -game base
bulwark -dedicated -basedir . -game base            # the same server
bulwark -dedicated 16 -basedir . -game base -port 27600 +set g_gametype duel +map duel_alley
```

The host is `src/server/sv_ded.c`; `src/main_ded.c` only calls it, and
`src/main.c` hands over to it when `-dedicated` is anywhere on the command
line. What FTE does was found by running `fteqw-sv64` as a black box with
the same game files and typing the same commands (see "Checked against FTE"
below); nothing comes from its source.

## Command line

| Option | Meaning |
| --- | --- |
| `-basedir dir`, `-game dir`, `-userdir dir`, `-enginedata dir` | as for the client |
| `-port n` (or `-udpport n`) | the UDP port, the same as `+set sv_port n` |
| `-dedicated [n]` | accepted by both binaries; a number after it sets `maxplayers`, as in Quake |
| `-frames n` | run n server frames and exit (tests), printing `status` |
| `-developer` | developer messages |
| `+command args` | console commands, one per `+` |

The client's `-width`, `-height`, `-window`, `-fullscreen` and `-headless`
are accepted and ignored, so one launcher line can carry `-dedicated` on
its own.

## Start-up order

1. `-port` and `-dedicated n` (as `set` lines).
2. The game's configs, exactly as the client runs them: the manifest's
   commands, `quake.rc` (Stiletto's execs `default_controls.cfg`,
   `default_cvar.cfg`, `default_video.cfg`, `default_mayhem.cfg`, `fte.cfg`,
   `config.cfg`, `autoexec.cfg`), the saved config and `autoexec.cfg`. This
   is what gives the server the game's hostname, slot count
   (`sv_playerslots`), bot, vote and movement defaults.
3. `server.cfg`, when the game has one.
4. `-port` and `-dedicated n` again, then the `+` commands.
5. The first map.

A `map` (or `changelevel`, `devmap`, `spmap`, `gamemap`) in steps 1 to 3 is
held, not run: the last one held starts after the command line, so
`+set g_gametype freezetag` still applies to the map `server.cfg` names.
When the command line starts a map itself, the held one is dropped with a
note ("server.cfg: "map duel_alley" skipped, the command line starts a
map"), so only one map loads. FTE runs `server.cfg` before the command
line and loads both maps. With no map anywhere the server says how to
start one and waits.

## The typed console

Lines typed on the terminal (or piped into stdin) run as console commands
between frames. POSIX polls file descriptor 0 without blocking; Windows
reads stdin on a thread, since a console handle can't be polled for whole
lines. End of input (stdin closed, or `/dev/null`) stops reading and the
server keeps running; `quit` or `exit` stops it, and so does Ctrl+C or
SIGTERM (the players are told "server shut down"; a second Ctrl+C exits at
once). A server put in the
background of a shell doesn't read the terminal (that would stop it with
SIGTTIN).

Commands run in this order, as on the client (see console.md), except for
the last step:

1. Commands the server game registered.
2. Aliases, engine commands, cvars.
3. Anything else goes to the server game's `ConsoleCmd`, as typed. So
   Nuclide's `addBot <profile>`, `kickAllBots`, `way*`, `listEntityDef` and
   the rest work without the `sv` prefix. If the game doesn't take the line,
   "unknown command" is printed.

Commands only a client has (`bind`, `unbind`, `unbindall`, `bindlevel`,
`in_bind`, `setinfo`, `menu_restart`, `menu_cmd`, `cl_cmd`, `togglemenu`,
`toggleconsole`, `vid_restart`, `snd_restart`, `r_restart`, `cfg_load`,
`cfg_save`, `showscores`, `messagemode`, `messagemode2`, `headmodel`,
`team_model`, `team_headmodel`) exist on a dedicated server and do nothing,
so the shared configs run without a screen of "unknown command". FTE prints
"Unknown command" for them.

The terminal gets plain text: `^0`..`^9`, `^xRGB`, `^&FB` and the
one-letter modes (`^b ^s ^h ^m ^d ^r ^a`) are dropped, `^^` prints a caret,
and a link `^[text\key\value...^]` prints only its text. Nuclide's log lines
(`^[\img\gfx/icon16/server...^] ^7...precaching`) come out as
` ...precaching`, as FTE prints them.

## Server commands worth knowing

| Command | What it does |
| --- | --- |
| `status` | map, time and entities, then `host <hostname>, port <n>, <p> players, <b> bots, <s> spectators, <slots> slots[, password set]` and one line per client (slot, name, state, frags, address or `bot`, `(spectator)`) |
| `serverinfo` | the server's info keys, one per line (`Server info settings:` first, as in QuakeWorld) |
| `serverinfo <key> <value>` | sets a key and sends it to the clients (`sv_serverinfo` is the same); `*` keys are the engine's |
| `map <name>`, `changelevel <name>` | new map; `g_gametype <mode>` before it picks Stiletto's rules. A name with no `maps/<name>.scene`, `.bsp`, `.vxmap` or `.ftew` prints `Can't find "maps/<name>.bsp"` and the current game goes on untouched (no spawn parms saved). A map that is found but fails to load ends the game and drops everyone with "server shut down" |
| `kick <slot or name>` | drops a player ("kicked") |
| `say <text>` | prints `console: <text>` to everyone |
| `clients` | the client list with rates |
| `quit`, `exit` | stop |

## Passwords and spectators

- `sv_password` or, on a dedicated server, FTE's `password`: players must
  join with userinfo `password` set to it (`setinfo password x` or
  `password x` on the client). `none` means no password. A listen server
  reads only `sv_password`, because on a client `password` is the userinfo
  value it sends to other servers (the server browser sets it), and a host
  who once joined a locked server would lock their own game.
- A client that joins with userinfo `spectator` set (and not `0`) is a
  QuakeWorld spectator. When `spectator_password` is set, the `spectator`
  value must be that password ("reject server needs a spectator password").
  At most `maxspectators` (default 8, in serverinfo) at once ("reject server
  is full of spectators"). Spectators use player slots. They get
  `*spectator 1` in their userinfo, and the game's `SpectatorConnect`,
  `SpectatorThink` and `SpectatorDisconnect` run for them instead of
  `ClientConnect`/`PutClientInServer`, `PlayerPreThink`/`PlayerPostThink`
  and `ClientDisconnect` (when the progs has them; otherwise they join as
  players). Nuclide makes them free-flying spectators.
- The `password` and `spectator` values are taken off a client's userinfo
  once it has joined, so other players are never sent them. A connected
  client's later `setinfo password ...`, `setinfo spectator ...` or
  `setinfo *<key> ...` is ignored: the join values are read only at
  connect, and `*` keys belong to the server.
- A reconnect from the same address and qport takes over its old slot, so
  a spectator reconnecting while the server is at `maxspectators` isn't
  refused (its own old slot doesn't count).

## Slots

`maxplayers`, `maxclients` and `sv_playerslots` are one setting under
three names. When several were set since the last map, the one set last
wins (the configs set all three, and `+set maxplayers 2` after them must
count), and all three are given its value when the next map starts. A slot
count of 1 is single player (loopback only); a dedicated server should keep
2 or more.

## Checked against FTE

With Stiletto's files, `fteqw-sv64` (stdin) and `bulwark-ded` were given the
same lines:

| Line | FTE | Bulwark |
| --- | --- | --- |
| (start-up) | runs quake.rc: hostname "Stiletto Swift", sv_playerslots 8 | the same |
| `serverinfo` | `Server info settings:` then key/value lines | the same layout |
| `addBot` | Nuclide's "Usage: Addbot <botname> ..." (QC) | the same |
| `addBot Marco` | a bot joins | a bot joins |
| `foo_unknown` | Unknown command "foo_unknown" | unknown command "foo_unknown" |
| `bind x y` | Unknown command "bind" | nothing (client command) |
| `password abc` | sets the server password | the same |
| `maxspectators` | 8 | 8 |
| `server.cfg` with a map, plus `+map` | both maps load | one map loads |
| `map nosuchmap` while a game runs | Can't find "maps/nosuchmap.bsp"; the game goes on | the same |
