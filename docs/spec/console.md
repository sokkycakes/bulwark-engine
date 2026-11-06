# Console commands

Bulwark has 707 console commands and about 380 cvars (`cmdlist` and
`cvarlist` show them; `help <name>` and `find <text>` explain one). Every
one of FTEQW's 593 console commands exists in Bulwark (checked against
FTE's own `cmdlist`), along with Source's entity commands. The game
modules add their own on top: Nuclide registers about 150 more. This page
covers how the console behaves, the formats the commands write, and where
Bulwark's version of a command differs from FTE's. Behaviour copied from
FTE was checked by running FTE as a black box; nothing here comes from its
source.

## How the console runs a line

1. Commands the game modules registered (menu, then client game, then
   server game) get the line first, so a game can override an engine
   command. `cl_cmd`, `menu_cmd` and `sv` pass a line to one module even
   when it never registered the command.
2. Then aliases, engine commands and cvars, in that order.
3. Anything else goes to the server as a client command. Nuclide handles
   most of its player commands this way, and the engine handles what the
   game passes back with `clientcommand`: the cheats (`god`, `buddha`,
   `noclip`, `notarget`, `fly`, `give`, `setpos`), `pause`, `fullupdate`.
   Cheats work when `sv_cheats` is 1 or the game is single player.

`$name` and `${name}` expand to a cvar's value, except in `alias`, `bind`,
`if`, `set_calc` and `defer`, which keep them for later. `$$` is a literal
`$`. Inside an alias, `$1`..`$9` are its arguments and `$*` all of them.
`wait [n]` delays the rest of the buffer by n frames; `defer <seconds>
<commands>` runs them later.

Quoted text written by sprintf's `%S` uses an escaped form when it holds a
quote, backslash, tab or line break: `\"line one\nline two"`, with `\n`,
`\t`, `\r`, `\"` and `\\` inside. Both `tokenize` and the console read the
form back. Nuclide's saved games depend on this (entities' spawn text has
line breaks in it). FTE writes the same form.

## Typing in the console

Tab completes the word at the cursor. At the start of a command (the
line's start, or after a `;`) that's a command, cvar or alias name. After
a command that takes a known kind of argument it's that kind: maps for
`map`, `changelevel` and `devmap`; config files for `exec` and
`cfg_load`; demos for `playdemo` and `timedemo`; cvars for `set`, `seta`,
`toggle`, `inc`, `reset`, `vstr` and the like; key names for `bind`,
`unbind` and `bindlevel`, then a command for the binding; aliases for
`unalias`; saves for `load` and `save`; sounds for `play`; models for
`modelviewer`; skies for `sky` and `loadsky`.

One match is put in with a space after it. Several are completed as far as
they agree; pressing Tab again then steps through them, and Shift+Tab
steps back. While typing, the matches are listed under the input line with
each cvar's value and description, each command's description and each
key's binding (`con_suggestions 0` turns the list off and prints the
matches when Tab can't go further, as before). After a command or cvar
with nothing to list, the line shows what it is.

## Files the commands write

| What | Where | Notes |
|---|---|---|
| Config | `config.cfg`, or `configs/<name>.cfg` for `cfg_save <name>` | Same layout as FTE, which writes `fte.cfg` and `configs/<name>.cfg`. `cfg_load <name>` falls back to `<name>.cfg` at the top level. Saved on quit when a setting or bind changed (`cfg_save_auto`). |
| Startup | `quake.rc` if the game has one, else `default.cfg` or `default_controls.cfg`; then the config and `autoexec.cfg` unless `quake.rc` already runs them | Nuclide games put their defaults in files only `quake.rc` runs (Rad-Therapy's movement cvars are in `default_cvar.cfg`), as FTE runs it. |
| Bans | `banned_ip.cfg` | `addip`/`banid` lines, run at startup. |
| Saved games | `saves/<name>.sav` | See below. |
| Demos | `<name>.dem`; `easyrecord` uses `demos/<date>-<map>.dem` | See below. |
| Screenshots | `screenshots/` | `r_screenshot_format` picks png, jpg, tga or bmp. |
| Console log | `logfile` / `con_logfile` | `con_timestamp` adds the time to each line. |
| QC dumps | `ssqccore.txt`, `csqccore.txt`, `menuqccore.txt`, `<vm>platform.txt` | From `coredump_*` and `pr_dumpplatform`. |
| Decompiled progs | `decompiled/<progs>.qc` | From `decompile`. |
| Static lights | `maps/<map>.rtlights` | DarkPlaces' format, from `r_editlights_save`. |
| Terrain | `maps/<name>.hmp`, `maps/<name>/sect_<x>_<y>.bwh`, generated `maps/<name>.bsp`, `maps/<name>.map` | See Terrain below. |
| Edited files | wherever `edit` opened them, in the writable game directory | |
| SQL databases | `sqlite/<name>.db` | From the `sqlconnect` builtin. |

### Saved games

A save is text. The engine writes a header, and the game writes the rest:

```
BULWARK SAVEGAME 1
map "ai"
comment "ai, 0:03"
time 3.34722224
maxclients 1
cvar skill "1"
parms 0 <16 spawn parms>
lightstyle 0 "m"
game            <- or "engine"
...the game's data...
```

With `game`, the rest is whatever the game's `SV_PerformSave(fd, entcount,
playerslots)` wrote through the file handle it was given; loading hands
the same text to `SV_PerformLoad`. With `engine` (no hooks, or
`sv_save_engine 1`), the engine writes every saved global and every
entity's fields and puts them back itself.

Loading starts the map again first, so precaches and static entities
exist, and then sets the time to the saved time. A player entity that the
load put back is kept as it is, so loading doesn't drop you at the start.
This is what Quake does: the player doesn't go through `ClientConnect` and
`PutClientInServer` again. With Nuclide, a save made after `setpos` loads
with the player in the same place, whether the game was still running or
the server had been shut down first.

`changelevel` in single player saves `autosave` once the player is in
(`sv_autosave`). `quicksave` and `quickload` use the slot `quick`.

FTE saves to a folder per save (`saves/<name>/info.fsv` and `<map>.lvc`).
The game data inside is the same, but Bulwark doesn't read FTE saves.

### Demos

`BWDM`, a 32-bit version (1), then records of a double time, a byte type
and a 32-bit length followed by that many bytes. The types are 1 (a
reliable server message), 2 (an unreliable one), 3 (view angles) and 4 (a
usercmd). Starting `record` during a game asks the server for a
`fullupdate`, so the demo starts with everything the client needs.

## Terrain

`mod_terrain_create` writes a `.hmp` file in the same layout as FTE's: an
entity list whose worldspawn holds the terrain settings (`_segmentsize`,
the `_min`/`_max` segment bounds, `_defaultgroundheight`,
`_defaultgroundtexture`, `_defaultwaterheight`, `_defaultwatertexture`,
`_seed`). Loading a map that has a `.hmp` builds `maps/<name>.bsp` from it,
a Q3 BSP the renderer and collision already handle: heights come from
section files where they exist, from noise seeded by `_seed` otherwise
(flat when the seed is empty), each 1/16 of a segment is a cell of two
brushes, the ground is vertex lit from `mod_terrain_sundir` and
`mod_terrain_ambient` (with `mod_terrain_shadows`), and water fills up to
the water height. An ordinary `.bsp` of the same name is never replaced.

Section files (`.bwh`, Bulwark's own): `BWH1`, int32 version (1), int32
section x and y, then 17×17 float heights along x then y.
`mod_terrain_save` writes them for the current terrain, and
`mod_terrain_convert` writes the terrain as brushes in a `.map`. FTE's
in-game sculpting (its `terrain_edit` builtin and csaddon) isn't there;
edit heights by writing section files.

## Commands by area

- **Cvars and scripting:** `set`, `seta`, `sets`, `setu`, `toggle`, `inc`,
  `incrementvar`, `multvar`, `reset`, `revert`, `cvarreset`,
  `cvar_resettodefaults_all` (and `_saveonly`, `_nosaveonly`),
  `differences`, `cvarwatch`, `vstr`, `if`, `set_calc`, `seta_calc`,
  `defer`, `alias`, `unalias`, `aliaslist`, `showalias`, `find`,
  `apropos`, `help`.
- **Configs and files:** `exec`, `cfg_save`, `cfg_save_ifmodified`,
  `saveconfig`, `writeconfig`, `host_writeconfig`, `cfg_load`,
  `cfg_reset`, `listconfigs`, `which`, `flocate`, `dir`, `ls`, `maps`,
  `maplist`, `path`, `version`, `logfile`, `condump`.
- **Console and chat:** `clear`, `conclear`, `toggleconsole`, `conclose`,
  `showconsole`, `messagemode`, `messagemode2`, `say`, `say_team`, `echo`,
  `conecho`, `centerprint`, `cprint`.
- **Screen:** `bf`, `v_cshift`, `fadein`, `fadeout`, `sizeup`,
  `sizedown`, `fog`, `screenshot`, `jpeg`, `screenshot_tga`, plus the
  cvars `cl_showfps`, `cl_showpos`, `net_graph`, `con_drawnotify`.
- **Video and input:** `vid_restart`, `vid_reload`, `vid_toggle`,
  `vid_modelist`, `in_restart`, `in_deviceids`, `bind`, `bindtoggle`,
  `in_bind`, `key_findbinding`, `key_listboundkeys`, `centerview`,
  `setang_local`, the `+`/`-` buttons including `+button0`..`+button19`,
  `+strafe` and `+klook`.
- **Sound:** `play`, `play2`, `playvol`, `stopsound`, `soundlist`,
  `soundinfo`, `snd_restart`, `music`, `stopmusic`.
- **Connecting:** `connect`, `reconnect`, `retry`, `disconnect`,
  `pingserver`, `rcon`, `setinfo`, `fullinfo`, `color`, `pause`.
- **Running a server:** `map`, `changelevel`, `changelevel2`, `devmap`,
  `spmap`, `spdevmap`, `gamemap`, `map_restart`, `restart`, `reload`,
  `killserver`, `status`, `users`, `user`, `clients`, `kick`, `kickall`,
  `kickid`, `stuffcmd`, `tell`, `serverinfo`, `localinfo`, `addip`,
  `banip`, `ban`, `banid`, `removeip`, `unban`, `listip`, `writeip`,
  `setpause`, `unpause`, `precaches`, `sv_lightstyle`, `sv_saveentfile`.
- **Entities:** `ent_dump`, `ent_info`, `ent_remove`, `ent_remove_all`,
  `ent_create`, `ent_teleport`, `ent_setname`, `find_ent`,
  `report_entities`, `edicts`, `edict`.
- **Saved games:** `save`, `savegame`, `load`, `loadgame`, `quicksave`,
  `quickload`, `autosave`, `listsaves`, `unsavegame`, `deletesave`.
- **Demos:** `record`, `easyrecord`, `rerecord`, `stop`, `playdemo`,
  `timedemo`, `stopdemo`, `demo_pause`, `demo_resume`,
  `demo_togglepause`, `demo_jump`, `demo_gototime`, `demo_setspeed`,
  `demo_info`, `listdemo`, `demolist`, `rmdemo`, `startdemos`, `demos`.
- **QC debugging:** `profile_ssqc`, `profile_csqc`, `profile_menuqc`
  (statements and calls per function, time per builtin),
  `coredump_*` (and `pr_*_coreonerror`), `breakpoint_*` (prints the stack
  each time a function is entered), `watchpoint_*` (reports each change to
  a global or `entnum.field`), `poke_*` (shows or sets one),
  `extensionlist_*`, `pr_dumpplatform`, `pr_trace`, `sv`, `cl_cmd`,
  `menu_cmd`, `compile`, `applycompile`, `decompile`.
- **Tools and editors:** `edit`, `aliasedit` (a full-screen text editor),
  `modelviewer`, `mapedit`, `r_editlights_*`, `mod_terrain_*`,
  `mod_bspx_*`, `mod_realign`, `mod_showent`, `mod_buildcubemaps`,
  `skel_*`.
- **Menus:** `togglemenu`, `menu_restart`, the engine's own pages
  (`menu_main`, `menu_options`, `menu_video`, `menu_servers` and the rest)
  for games without a menu progs, and the script menus (`conmenu`,
  `menutext`, `menucheck`, `menuslider`, `menuedit`, `menubind`, ...).
- **Voice chat:** `+voip`, `voip`, `cl_voip_mute`, `cl_voip_unmute`.
- **Plugins, SQL, workers:** `plug_load`, `plug_close`, `plug_closeall`,
  `plug_list`, `sqlstatus`, `sqlkill`, `sqlkillall`, `worker_status`,
  `worker_test`.
- **Multi-view demos and QTV:** `mvd*`, `sv_demo*`, `qtv*`, `track`,
  `autotrack`.
- **Downloads and media:** `download`, `dlsize`, `skipdl`, `finishdl`,
  `curl`, `capture*`, `playfilm`, `playvideo`, `cinematic`, `media_*`.

## Who may run what

Every line runs at an execution level from 0 to 50. Commands, cvars and
aliases each need a level (`restrict` shows or changes it, but never above
the level of whoever asks).

| Source | Level |
|---|---|
| The console, configs, the command line, binds the player made, the menu (`menu.dat`) | 50 |
| Text a server sends (`stufftext`), also when a demo plays it back | `cmd_serverlevel`, 29 |
| `localcmd` and `cvar_set` from the client and server games | `cmd_gamecodelevel`, 29 |
| rcon | `rcon_level` or the ranking's rights |

What a lower level makes keeps that level: an alias a server defines runs
at 29 when the player uses it, a bind it makes is written as `bindlevel`,
`vstr` runs a cvar's text no higher than whoever last set it, and `in`
or `defer` commands run at the level that queued them.

These need level 50, so neither a server nor game code reaches them:
loading code (`plug_load`, `plug_close`, `plug_closeall`, `compile`,
`applycompile`, `decompile`); writing or removing files (`wf`, the config
writers, `condump`, the log files, `writeid`, `writeip`, `deletesave`,
`unsavegame`, `rmdemo`, the demo recorders, `edit`, `aliasedit`, the
terrain, light, pin, BSPX and effect exporters, the QC core dumps);
reaching outside the game (`pkg`, `curl`, `download_map_url`, `gamedir`,
`fs_changegame`, `fs_changemod`, `dtls_*`, `rcon`); and the player's own
setup (`quit`, `exit`, `unbindall`, the cvar default resets, `resetcfg`,
`cfg_reset`). So do the cvars `cmd_gamecodelevel`, `cmd_serverlevel`,
`rcon_password`, `rcon_address`, `rcon_level`, `pr_sourcedir`, `log_dir`,
`sv_snapdir` and `cl_downloads`. Below 50, `setinfo`, `setu`, `sets` and
`setfl` can't publish an engine cvar in userinfo or serverinfo, since that
would send its value to the server. `restrict <command> 29` opens one up
for servers the player trusts.

The client won't download files that could run as code: `.cfg`, `.rc`,
`.fmf`, libraries and programs, packages (`.pk3`, `.pak`, `.pk4`, `.zip`,
`.bwp`), `menu.dat` and `progs.dat`.

## Where Bulwark differs from FTE

The commands all exist; these behave differently from FTE's or stop short
of it, and say so when used:

- **`compile`** runs an external `fteqcc` (on the `PATH` or in the base
  directory) on `<pr_sourcedir>/progs.src` on a worker thread; Bulwark has
  no compiler built in. **`applycompile`** saves the game and loads it again
  (or restarts the map when nobody is in it) so new progs take over.
- **`decompile`** writes readable pseudo-QC (one line a statement, with
  labels for jumps), not FTE's reconstructed source.
- **`modelviewer`** shows the model's own skins and frames; separate skin
  and animation files aren't read. **`mapedit`** loads a map with no
  gamecode and flies through it showing entities and their keys; it does
  not edit brushes.
- **Terrain** is turned into a generated BSP (see above) rather than
  FTE's live heightmap, and has no in-game sculpting.
- **SQL** has SQLite only; `mysql` connections report that they aren't
  supported.
- **`dtls_*`**: Bulwark has no DTLS, so there are no certificates to
  manage.
- **Plugins** are Bulwark's own (`bwplug_<name>.so` with `bwplug_init`,
  see `src/common/bwplug.h`), not FTE's plugin API. Plugins can add image,
  material and model formats, and every plugin next to the engine loads at
  startup. The Source formats (VTF, VMT, MDL) come this way, from
  `bwplug_source.so`; see [source-formats.md](source-formats.md).

## Full list

The engine's commands, from `cmdlist`. A `+` command runs while its key is
held and the matching `-` command when it's let go.

| Command | What it does |
|---|---|
| `6dof` | Toggle six-degrees-of-freedom movement: no pitch limit, roll with +rollleft/+rollright (cheat) |
| `__capturedemo_start` | For internal use |
| `__fs_apply` | Internal: finishes a game switch |
| `__snap` | For internal use: send the server a screenshot |
| `addip` | addip <minutes> <ip[/bits]>: ban an address (0 minutes is permanent) |
| `adduser` | adduser <name> [password] [rights]: add a ranking |
| `alias` | alias <name> [commands]: name a list of commands; $1..$9 and $* are its arguments |
| `aliasedit` | aliasedit <alias>: edit an alias's commands, one a line |
| `aliaslevel` | aliaslevel <alias> [level]: the execution level an alias runs at |
| `aliaslist` | aliaslist [pattern]: list the aliases |
| `allskins` | Reload every player's skin |
| `applycompile` | Save the game and load it again with freshly compiled progs |
| `apropos` | apropos <text>: commands and cvars whose name or help holds the text |
| `+attack` / `-attack` | Fire (button 0) |
| `+attack2` / `-attack2` | Secondary fire |
| `autosave` | Save to the "autosave" slot |
| `autotrack` | Follow the best player in a server demo or QTV stream |
| `+back` / `-back` | Move back |
| `ban` | ban <player> [minutes] [reason]: ban a player's address |
| `banid` | banid <minutes> <slot>: ban a player's address |
| `banip` | banip <ip[/bits]> [reason] [seconds]: ban an address |
| `banlist` | List the bans |
| `banname` | banname <name> [minutes] [reason]: ban a player by name |
| `bestweapon` | bestweapon <impulse> [impulse...]: switch to the first of these weapons you have |
| `bf` | The item-pickup screen flash |
| `bind` | bind <key> [command]: run a command on a key |
| `bindlevel` | bindlevel <key> <level> [command]: a binding that runs at a lower execution level |
| `bindlist` | List the key bindings |
| `bindtoggle` | bindtoggle <key> <cvar> [values...]: the key toggles the cvar |
| `breakpoint` | breakpoint [function]: breakpoint_ssqc |
| `breakpoint_csqc` | breakpoint_csqc [function]: print the stack each time the client game enters the function |
| `breakpoint_menuqc` | breakpoint_menuqc [function]: print the stack each time the menu enters the function |
| `breakpoint_ssqc` | breakpoint_ssqc [function]: print the stack each time the server game enters the function |
| `buddha` | Take damage but don't die (cheat) |
| `+button0` / `-button0` | Input button 0 (input_buttons bit value 1) |
| `+button1` / `-button1` | Input button 1 (input_buttons bit value 256) |
| `+button10` / `-button10` | Input button 10 (input_buttons bit value 1024) |
| `+button11` / `-button11` | Input button 11 (input_buttons bit value 2048) |
| `+button12` / `-button12` | Input button 12 (input_buttons bit value 4096) |
| `+button13` / `-button13` | Input button 13 (input_buttons bit value 8192) |
| `+button14` / `-button14` | Input button 14 (input_buttons bit value 16384) |
| `+button15` / `-button15` | Input button 15 (input_buttons bit value 32768) |
| `+button16` / `-button16` | Input button 16 (input_buttons bit value 65536) |
| `+button17` / `-button17` | Input button 17 (input_buttons bit value 131072) |
| `+button18` / `-button18` | Input button 18 (input_buttons bit value 262144) |
| `+button19` / `-button19` | Input button 19 (input_buttons bit value 524288) |
| `+button2` / `-button2` | Input button 2 (input_buttons bit value 2) |
| `+button3` / `-button3` | Input button 3 (input_buttons bit value 4) |
| `+button4` / `-button4` | Input button 4 (input_buttons bit value 8) |
| `+button5` / `-button5` | Input button 5 (input_buttons bit value 16) |
| `+button6` / `-button6` | Input button 6 (input_buttons bit value 32) |
| `+button7` / `-button7` | Input button 7 (input_buttons bit value 64) |
| `+button8` / `-button8` | Input button 8 (input_buttons bit value 128) |
| `+button9` / `-button9` | Input button 9 (input_buttons bit value 512) |
| `cancel` | Stop recording and delete the demo |
| `capture` | capture <filename>: record the screen and sound to a video |
| `capturedemo` | capturedemo foo.dem foo.avi - Captures a named demo to a named video file. |
| `capturepause` | Pauses the video capture, allowing you to avoid capturing uninteresting parts. This is a toggle. |
| `capturestop` | Aborts the current video capture. |
| `cd` | cd <play\|loop\|stop\|info> [track]: CD audio, played as music/track<n> |
| `centerprint` | centerprint <text>: print in the middle of the screen |
| `centerview` | Level the view pitch |
| `cfg_load` | cfg_load [name]: reset to defaults, then run the config |
| `cfg_reset` | Settings and binds back to their defaults |
| `cfg_save` | cfg_save [name]: write settings, binds and aliases to a config |
| `cfg_save_ifmodified` | cfg_save, only if something changed since the last save |
| `changelevel` | changelevel <map>: go to a map, keeping the players' spawn parms |
| `changelevel2` | changelevel2 <map> [landmark]: move on to a map |
| `changing` | Sent by servers before a level change; not for typing |
| `check_maps` | Obsolete in FTE; does nothing |
| `cinematic` | cinematic <filename>: play a film from video/ |
| `cl_addport` | cl_addport [port]: listen on another UDP port; lists the ports without one |
| `cl_cmd` | cl_cmd <command>: run a command of the client game (CSQC_ConsoleCommand) |
| `cl_status` | The connection, map, client game and game directory |
| `cl_transfer` | Connect to a different server, disconnecting from the current server only when the new server replies |
| `cl_voip_mute` | cl_voip_mute <player...>: stop hearing a player's voice |
| `cl_voip_unmute` | cl_voip_unmute <player...>: hear a player's voice again |
| `clear` | Clear the console |
| `clientkick` | clientkick <slot> [reason]: kick a player |
| `clients` | List the players on the server |
| `closemenu` | Close the engine's menus |
| `cmd` | cmd <text>: send a command to the server |
| `cmdlist` | cmdlist [pattern]: list the commands |
| `color` | color <top> [bottom]: player colours |
| `colorize` | colorize <text>: print text with &cRGB colours |
| `colourise` | colourise <text>: print text with &cRGB colours |
| `com_reloadfilter` | Reload the chat word filter (filter.txt) |
| `compile` | compile [file.src]: compile QuakeC with an external fteqcc |
| `conactivate` | Open the console |
| `conclear` | Clear the console text |
| `conclose` | Close the console |
| `condump` | condump <file>: write the console text to a file |
| `conecho` | conecho <text>: print to the console |
| `conecho_center` | conecho_center <text>: print in the middle of the screen |
| `conmenu` | conmenu <callback> |
| `connect` | connect <address>: join a server |
| `connectbr` | Connect to a server using the best route we can find (UDP, else TCP) |
| `connectnq` | Connect to a server, defaulting to port 26000 |
| `connectq2e` | Connect to a server, defaulting to port 5069 |
| `connectqe` | Connect to a server, defaulting to port 26000 |
| `connecttcp` | Connect to a server using the tcp:// prefix |
| `Converts` | a terrain to a .map file of brushes. |
| `coredump_csqc` | Write the stack, globals and entities of the client game to csqccore.txt |
| `coredump_menuqc` | Write the stack, globals and entities of the menu to menuqccore.txt |
| `coredump_ssqc` | Write the stack, globals and entities of the server game to ssqccore.txt |
| `cprint` | cprint <text>: print in the middle of the screen |
| `cripple` | cripple <player> [minutes] [reason]: can't move (0 minutes lifts it) |
| `ctfscores` | Print the team scores |
| `cuff` | cuff <player> [minutes] [reason]: can't attack (0 minutes lifts it) |
| `curl` | For use by xonotic: curl [--pak] [--as <file>] [--for <file>] <url>, --info, --cancel |
| `cvar_lockdefaults` | Make every cvar's current value its default |
| `cvar_purgedefaults` | Put cvar defaults back to the engine's, keeping their values |
| `cvar_resettodefaults_all` | Every cvar back to its default |
| `cvar_resettodefaults_nosaveonly` | Every cvar that isn't archived back to its default |
| `cvar_resettodefaults_saveonly` | Every archived cvar back to its default |
| `cvarlist` | cvarlist [-c -d -F -a -r -h] [pattern]: list the cvars |
| `cvarreset` | cvarreset <cvar or pattern>: back to the default value |
| `cvarwatch` | cvarwatch <cvar>: print each change to it (again to stop) |
| `decompile` | decompile [progs]: write decompiled/<progs>.qc as readable pseudo-QC |
| `defer` | defer <seconds> <commands>: run commands later |
| `deletesave` | deletesave <name>: delete a saved game |
| `demo_gototime` | demo_gototime <seconds>: go to a time in the demo |
| `demo_info` | What demo is playing or recording |
| `demo_jump` | demo_jump <seconds>: go to a time in the demo (+n/-n relative) |
| `demo_jump_end` | Jump to the end of the demo |
| `demo_jump_mark` | Jump to the next //demomark the server sent |
| `demo_nudge` | demo_nudge [frames]: step server frames forward (or back, negative) |
| `demo_pause` | Pause demo playback |
| `demo_resume` | Resume demo playback |
| `demo_setspeed` | demo_setspeed <scale>: playback speed (demo_timescale) |
| `demo_togglepause` | Pause or resume demo playback |
| `demolist` | List the demos |
| `demos` | Back to the startdemos loop |
| `demos_next` | Play the next demo of the startdemos loop |
| `devmap` | devmap <map>: start a map with cheats on |
| `df` | df <file>: print a file |
| `differences` | List the cvars that differ from their defaults |
| `dir` | dir [pattern]: list game files |
| `disconnect` | Leave the game |
| `dlsize` | Show the current download's size and progress |
| `download` | download <file>: get a file from the server |
| `download_map_url` | Obsolete in FTE; does nothing |
| `dsp_info` | The sound effects room (DSP) in use |
| `dtls_importtrust` | DTLS certificate trust (Bulwark has no DTLS) |
| `dtls_untrustall` | DTLS certificate trust (Bulwark has no DTLS) |
| `+duck` / `-duck` | Crouch |
| `easyrecord` | easyrecord [suffix]: record a demo under a dated name |
| `echo` | echo <text>: print to the console |
| `edict` | edict <n>: an entity's position, velocity and angles |
| `edictcount` | Entity counts by class |
| `edicts` | List the server's entities |
| `edit` | edit [filename[:line]]: edit a file |
| `ent_create` | ent_create <classname> [key value...]: spawn where you look |
| `ent_dump` | ent_dump [entity] [all]: an entity's fields (default: under the crosshair) |
| `ent_info` | ent_info [entity]: an entity's fields |
| `ent_remove` | ent_remove [entity]: remove an entity (default: under the crosshair) |
| `ent_remove_all` | ent_remove_all <class or name>: remove all matching entities |
| `ent_setname` | ent_setname <name> [entity]: set a targetname |
| `ent_teleport` | ent_teleport <entity>: move an entity to where you look |
| `envmap` | envmap [name] [size]: screenshot_cubemap |
| `exec` | exec <file>: run a config file |
| `exit` | Quit the game |
| `extensionlist_csqc` | The QC extensions the client game can use |
| `extensionlist_menuqc` | The QC extensions the menu can use |
| `extensionlist_ssqc` | The QC extensions the server game can use |
| `fadein` | fadein [seconds] [r g b]: fade the screen in from a colour |
| `fadeout` | fadeout [seconds] [r g b]: fade the screen out to a colour |
| `filter` | filter [#tag...\|clear]: only show tagged chat with these tags |
| `find` | find <text>: commands and cvars whose name or help holds the text |
| `find_ent` | find_ent <text>: entities by classname or targetname |
| `finishdl` | Stop downloading and carry on with the files that are here |
| `flocate` | flocate <file>: the package or directory a file comes from |
| `floodprot` | floodprot [messages seconds silence]: chat flood protection |
| `flush` | Rescan the game directories for packages |
| `fly` | Toggle flying (cheat) |
| `fog` | fog <density> [r g b]: set the fog (0 turns it off); densities as Quake maps give them (squared exponential, a 64th of the density per unit) |
| `force_centerview` | Level the view pitch |
| `+forward` / `-forward` | Move forward |
| `fps_preset` | fps_preset <fast\|normal\|nice>: set the effect cvars for speed or looks |
| `fraglogfile` | Toggle QuakeWorld's frag log (frag_<n>.log) |
| `fs_changegame` | Switch between different manifests (or games in the base directory) |
| `fs_changemod` | Fetch a mod's files and switch to it, for online installers |
| `fs_flush` | Rescan the game directories for packages |
| `fs_hash` | fs_hash <file>: CRC16, MD5, SHA1 and SHA256 of a file |
| `fs_restart` | Rescan the game directories for packages |
| `fs_showmanifest` | Show the manifest in effect |
| `fullinfo` | fullinfo \key\value...: set several userinfo keys |
| `gamedir` | Show or change the mod directory |
| `gamemap` | gamemap <map>: start a map |
| `give` | give <item or health> [amount]: (cheat) |
| `globalservers` | globalservers [master...]: list the servers the master servers know |
| `god` | Toggle invulnerability (cheat) |
| `heartbeat` | Sends an update or ping to the master servers so the server stays listed |
| `help` | help <name>: describe a command or cvar |
| `hideconsole` | Close the console |
| `host_writeconfig` | host_writeconfig [name]: write the config |
| `hunkprint` | Memory statistics |
| `identify` | identify <player or address>: the names an address has used |
| `if` | if <expression> then <commands> [else <commands>] |
| `ignore` | ignore [name...]: hide a player's chat (no name lists who is ignored) |
| `ignore_id` | ignore_id <player number>: hide a player's chat |
| `ignore_team` | ignore_team <team>: hide a team's chat |
| `ignorelist` | Who is ignored |
| `impulse` | impulse <n>: send an impulse with the next move |
| `in` | in <seconds> <command>: run a command later |
| `in_bind` | in_bind <device> <key> [command]: bind for one input device |
| `in_deviceids` | List the input devices |
| `in_restart` | Reopen the game controllers |
| `inc` | inc <cvar> [amount]: add to a cvar, 1 by default |
| `incrementvar` | incrementvar <cvar> <min> <max> <delta>: step a cvar, wrapping |
| `+infoplaque` / `-infoplaque` | Show the game's details while held |
| `invleft` | Select the inventory item to the left |
| `invnext` | Select the next inventory item |
| `invprev` | Select the previous inventory item |
| `invright` | Select the inventory item to the right |
| `invuse` | Use the selected inventory item |
| `ipdump` | ipdump [file]: write the address log (iplog.txt) |
| `ipmerge` | ipmerge <file>: read an address log in |
| `join` | Join the game from spectating |
| `joy_virtual` | joy_virtual [count]: attach pretend controllers (up to 4) for tests |
| `joy_virtual_set` | joy_virtual_set [pad] axis\|button <n> <value>: move a pretend controller |
| `jpeg` | jpeg [name]: a JPEG screenshot |
| `+jump` / `-jump` | Jump (button 2) |
| `key_findbinding` | key_findbinding <command>: keys bound to a command |
| `key_listboundkeys` | List every bound key |
| `kick` | kick <player> [reason]: drop a player |
| `kickall` | kickall [reason]: kick every player |
| `kickid` | kickid <slot> [reason]: kick a player |
| `kill` | Kill yourself |
| `killserver` | Stop the running server |
| `+klook` / `-klook` | Forward and back look up and down instead |
| `+left` / `-left` | Turn left |
| `listconfigs` | List the .cfg files |
| `listdemo` | listdemo <name>: what a demo holds |
| `listfonts` | The font files and the loaded fonts |
| `listid` | List the bans |
| `listip` | List the bans |
| `listmaps` | listmaps [text]: list the maps |
| `listplayers` | List the players |
| `listsaves` | List the saved games |
| `listskins` | List the skins in skins/ |
| `listskyboxes` | The skyboxes in env/ and gfx/env/ |
| `load` | load <name>: load saves/<name>.sav |
| `loadfont` | loadfont [slot] <file> [sizes]: load a font |
| `loadgame` | loadgame <name>: load a saved game |
| `loadloc` | loadloc <file>: load locs/<file>.loc, the map's place names |
| `loadsky` | loadsky [name]: the map's skybox (DarkPlaces' name for sky) |
| `lobby_cancel` | Stop the lobby's countdown (host) |
| `lobby_chat_hex` | lobby_chat_hex <hex>: say something in the lobby (UTF-8 as hex) |
| `lobby_chat_snapshot` | Print the lobby's chat |
| `lobby_close` | Leave the lobby, or close it when hosting |
| `lobby_create` | lobby_create <lan|online|offline>: host a lobby (see lobby.md) |
| `lobby_create_lan` | Host a lobby on this network |
| `lobby_create_offline` | Host a lobby nobody else can join (tests) |
| `lobby_create_online` | Host a lobby with a room code |
| `lobby_join` | lobby_join <address|room code>: join a lobby |
| `lobby_limits` | lobby_limits <minutes> <frags>: the game's limits (host) |
| `lobby_ready` | lobby_ready [1|0]: say you are ready, or not |
| `lobby_set` | lobby_set <ruleset|map> <value>: change the lobby's game (host) |
| `lobby_snapshot` | Print the lobby's state and write lobby_snapshot.json |
| `lobby_start` | Start the lobby's game now (host) |
| `localinfo` | localinfo [key [value]]: server-side settings |
| `localservers` | List the servers on the local network |
| `logfile` | logfile [name]: start or stop copying the console to a file |
| `logplayers` | Toggle the player log |
| `logrcon` | Toggle the rcon log |
| `+lookdown` / `-lookdown` | Look down |
| `+lookup` / `-lookup` | Look up |
| `ls` | ls [pattern]: list game files |
| `macrolist` | List the $macros |
| `map` | map <name>: start a map |
| `map_restart` | map_restart [seconds]: start the map over |
| `mapedit` | Loads the named map without any gamecode active. |
| `maplist` | maplist [text]: list the maps |
| `maps` | maps [text]: list the maps |
| `me` | me <action>: an emote in chat |
| `media_add` | media_add <track>: add music to the playlist (lists it without one) |
| `media_next` | Play the next track of the playlist |
| `media_remove` | media_remove <track>: take music off the playlist |
| `menu_audio` | Open the engine's Audio and voice menu |
| `menu_cmd` | menu_cmd <command>: run a command of the menu (m_consolecommand) |
| `menu_credits` | Open the engine's Credits menu |
| `menu_demo` | Open the engine's Demos menu |
| `menu_download` | Open the engine's Packages and updates menu |
| `menu_fps` | Open the engine's Performance presets menu |
| `menu_keys` | Open the engine's Keys menu |
| `menu_lighting` | Open the engine's Lighting menu |
| `menu_load` | Open the engine's Load a game menu |
| `menu_loadgame` | Open the engine's Load a game menu |
| `menu_lobby_create` | Create a LAN lobby and show it |
| `menu_main` | Open the engine's Main menu menu |
| `menu_media` | Open the engine's Media player menu |
| `menu_mediafiles` | Open the engine's Add music menu |
| `menu_mods` | Open the engine's Mods menu |
| `menu_multi` | Open the engine's Multiplayer menu |
| `menu_network` | Open the engine's Network menu |
| `menu_newmulti` | Open the engine's Start a server menu |
| `menu_options` | Open the engine's Options menu |
| `menu_particles` | Open the engine's Particles and decals menu |
| `menu_quit` | Open the engine's Quit menu |
| `menu_render` | Open the engine's Rendering menu |
| `menu_restart` | Reload the menu |
| `menu_save` | Open the engine's Save the game menu |
| `menu_servers` | Open the engine's Servers menu |
| `menu_setup` | Open the engine's Player setup menu |
| `menu_single` | Open the engine's Single player menu |
| `menu_slist` | Open the engine's Servers menu |
| `menu_spcheats` | Open the engine's Cheats menu |
| `menu_speakers` | Open the engine's Speakers menu |
| `menu_teamplay` | Open the engine's Teamplay menu |
| `menu_teamplay_ammo_health` | Open the engine's Ammo and health menu |
| `menu_teamplay_armor` | Open the engine's Armour menu |
| `menu_teamplay_items` | Open the engine's Items menu |
| `menu_teamplay_locations` | Open the engine's Locations menu |
| `menu_teamplay_needs` | Open the engine's Needs menu |
| `menu_teamplay_powerups` | Open the engine's Powerups menu |
| `menu_teamplay_status_location_misc` | Open the engine's Status, location and other words menu |
| `menu_teamplay_team_fortress` | Open the engine's Team Fortress menu |
| `menu_teamplay_weapons` | Open the engine's Weapons menu |
| `menu_textures` | Open the engine's Textures menu |
| `menu_video` | Open the engine's Video menu |
| `menu_webcore` | menu_webcore [title|lobby|pause|arena <team|class|hero>|<url>]: open an HTML menu page (see webcore.md) |
| `menu_webcore_lobby` | Open the lobby page |
| `menu_webcore_title` | Open the title page |
| `menubind` | x y caption bindcommand |
| `menubox` | x y width height: a framed box, in characters |
| `menucallback` | Explicitly invoke the active script menu's callback function with the given option set. |
| `menucheck` | x y caption cvarname bitmask |
| `menuclear` | Pop the currently scripted menu. |
| `menucomboi` | x y caption cvarname [caption0] [caption1] ... |
| `menucombos` | x y caption cvarname [caption0] [value0] [caption1] [value1] ... |
| `menuedit` | x y caption cvarname |
| `menueditpriv` | x y caption def |
| `menupic` | x y picname |
| `menupop` | Go back one menu |
| `menuslider` | x y caption cvarname min max |
| `menutext` | x y caption cbcommand |
| `menutextbig` | x y caption cbcommand |
| `messagemode` | Type a chat message to everyone |
| `messagemode2` | Type a chat message to your team |
| `+mlook` / `-mlook` | Accepted for old configs; the mouse always looks |
| `mod` | mod <command>: a command for the menu, client game or server game |
| `mod_batchlist` | The world's draw batches |
| `mod_bspx_injest` | mod_bspx_injest <bsp>: put a map's .lit/.lux files into it as BSPX lumps |
| `mod_bspx_list` | mod_bspx_list [bsp]: a map's BSPX lumps and their sizes |
| `mod_bspx_strip` | mod_bspx_strip <bsp> <lump>: take a BSPX lump out of a map |
| `mod_buildcubemaps` | Draw a cubemap at each env_cubemap into textures/env/ |
| `mod_findcubemaps` | Find each surface's nearest env_cubemap and store it in the map's BSPX lumps |
| `mod_memlist` | The loaded models and roughly what they take |
| `mod_realign` | mod_realign [bsp]: rewrite a map with only its lump alignment changed |
| `mod_showent` | mod_showent [number or text]: the running map's entities, by number or containing text |
| `mod_terrain_convert` | mod_terrain_convert [mapname] [texkill] |
| `mod_terrain_create` | Generates a fresh maps/foo.hmp file. You may wish to edit it with notepad later to customise it. |
| `mod_terrain_reload` | Rebuilds the current terrain map from its .hmp and sections. |
| `mod_terrain_save` | Writes the current terrain's heights out as section files. |
| `mod_texturelist` | mod_texturelist [filter]: the world's materials and surface counts |
| `mod_usetexture` | mod_usetexture <material>: put a material on the world surface in view |
| `modelviewer` | Shows a model on its own: modelviewer <MODELNAME> [SKINFILE] [ANIMATIONFILE] |
| `+movedown` / `-movedown` | Swim or fly down |
| `+moveleft` / `-moveleft` | Strafe left |
| `+moveright` / `-moveright` | Strafe right |
| `+moveup` / `-moveup` | Swim or fly up |
| `msg_trigger` | msg_trigger [name [text [-l level]]]: run alias <name> when a message contains the text |
| `multvar` | multvar <cvar> <min> <max> <factor>: scale a cvar within a range |
| `music` | music <file or track> [-]: play music |
| `music_fforward` | music_fforward [seconds]: skip ahead in the music (10 seconds) |
| `music_next` | Play the next track of the playlist |
| `music_rewind` | music_rewind [seconds]: go back in the music (10 seconds) |
| `mute` | mute <player> [minutes] [reason]: no chat (0 minutes lifts it) |
| `mvdcancel` | Stop the server demo and delete it |
| `mvdlist` | mvdlist [filter]: list the server demos |
| `mvdrecord` | mvdrecord <name>: record every player's view to demos/<name>.mvd |
| `mvdstop` | Stop the server demo |
| `net_ice_show` | Show WebRTC/ICE connections (there are none in Bulwark) |
| `netstats` | Network statistics: latency, loss and traffic |
| `newalias` | newalias <name> <commands>: alias |
| `nextul` | For internal use |
| `noclip` | Toggle flying through walls (cheat) |
| `notarget` | Toggle being ignored by monsters (cheat) |
| `observe` | Spectate |
| `openroute` | Ask the router to forward the server's port (NAT-PMP) |
| `+p` / `-p` | Following input is for the next seat |
| `p` | p <seat> <command>: run a command as one split screen player |
| `+p1` / `-p1` | +pN <button>: press a button for split screen player N |
| `p1` | p1 <command>: run a command for seat 1 |
| `+p2` / `-p2` | +pN <button>: press a button for split screen player N |
| `p2` | p2 <command>: run a command for seat 2 |
| `+p3` / `-p3` | +pN <button>: press a button for split screen player N |
| `p3` | p3 <command>: run a command for seat 3 |
| `+p4` / `-p4` | +pN <button>: press a button for split screen player N |
| `p4` | p4 <command>: run a command for seat 4 |
| `packet` | packet <destination> <contents>: send a connectionless packet (\n for a line break) |
| `pause` | Pause or unpause the game |
| `pin_add` | pin_add <author> <message>: a message shown to players as they join |
| `pin_delete` | pin_delete <number\|all>: remove pinned messages |
| `pin_reload` | Read pinned.txt again |
| `pin_save` | Write the pinned messages to pinned.txt |
| `pingserver` | pingserver <address>: ask a server for its status |
| `pkg` | Package manager: list, show, search, add, rem, del, upgrade, changes, apply, sources... |
| `play` | play <sound>: play a sound |
| `play2` | play2 <sound>...: play sounds without position |
| `playclip` | playclip <filename>: play a film to its end |
| `playdemo` | playdemo <name>: play a demo |
| `playfilm` | playfilm <filename>: play a RoQ or AVI film (escape skips it) |
| `playvideo` | playvideo <filename>: play a film |
| `playvol` | playvol <sound> <volume>: play a sound at a volume |
| `plug_close` | plug_close <name>: unload a plugin |
| `plug_closeall` | Unload every plugin |
| `plug_list` | The loaded plugins, and the ones that could be loaded |
| `plug_load` | plug_load <name\|path>: load a native plugin (bwplug_<name>.so) |
| `pointfile` | pointfile [map\|clear]: show a map compiler's leak trail (maps/<map>.pts or .lin) |
| `poke_csqc` | poke_csqc <global or entnum.field> [value]: show or set a value in the client game |
| `poke_menuqc` | poke_menuqc <global or entnum.field> [value]: show or set a value in the menu |
| `poke_ssqc` | poke_ssqc <global or entnum.field> [value]: show or set a value in the server game |
| `pr_dumpplatform` | pr_dumpplatform [ssqc\|csqc\|menuqc]: write the builtins and extensions to a file |
| `pr_missing` | pr_missing [ssqc|csqc|menuqc]: list the builtins the progs declares that the engine lacks |
| `pr_trace` | pr_trace [text]: print each QC function entered whose name contains the text |
| `precaches` | List the precached models, sounds and effects |
| `profile_csqc` | profile_csqc [n]: profile the client game; again to show the top n functions |
| `profile_menuqc` | profile_menuqc [n]: profile the menu; again to show the top n functions |
| `profile_ssqc` | profile_ssqc [n]: profile the server game; again to show the top n functions |
| `+prone` / `-prone` | Go prone |
| `qtv_status` | Who is watching the QTV stream |
| `qtvdemos` | qtvdemos [host[:port]]: the demos a QTV server has |
| `qtvlist` | qtvlist [host[:port]]: the streams a QTV server has |
| `qtvplay` | qtvplay [stream@]host[:port] [password]: watch a game (or demo:<name>@host) streamed over QTV |
| `qtvreverse` | qtvreverse <host[:port]> [name]: send the game to another server's QTV viewers |
| `quickconnect` | Find a server and join the best one |
| `quickload` | Load the "quick" slot |
| `quicksave` | Save to the "quick" slot |
| `quit` | Quit the game |
| `qwurl` | qwurl qw://server[/join\|/observe\|/qtvplay]: for compat with ezquake |
| `r_beaminfo` | How many beam segments are alive |
| `r_converteffectinfo` | r_converteffectinfo [file]: turn DarkPlaces' effectinfo.txt into particles/effectinfo.cfg |
| `r_decalinfo` | The decals on screen |
| `r_dumpshaders` | Write the GLSL programs to glsl/ |
| `r_editlights_clone` | Duplicate the current light (with a new origin) |
| `r_editlights_copyinfo` | store a copy of all properties (except origin) of the selected light |
| `r_editlights_edit` | Changes named properties on the current light. |
| `r_editlights_editall` | Like r_editlights_edit, but affects all lights instead of just the selected one. |
| `r_editlights_lock` | Blocks changing the current light according the crosshair. |
| `r_editlights_pasteinfo` | apply the stored properties onto the selected light (making it exactly identical except for origin) |
| `r_editlights_reload` | Reload static rtlights. Argument can be rtlights\|statics\|bsp\|none to override the source. |
| `r_editlights_remove` | Removes the current light. |
| `r_editlights_save` | Saves rtlights to maps/FOO.rtlights |
| `r_editlights_spawn` | Spawn a new light with default properties |
| `r_editlights_togglecorona` | Toggles the current light's corona field. |
| `r_editlights_toggleshadow` | Toggles the shadow flag on the current light. |
| `r_effect` | r_effect <name>: start a particle effect where you look |
| `r_exportalleffects` | Write every effect read so far to particles/export.cfg |
| `r_exportbuiltinparticles` | r_exportbuiltinparticles <set>: write one of the engine's effect sets to particles/ |
| `r_imageformats` | The image files and GPU formats the renderer uses |
| `r_imagelist` | r_imagelist [filter]: the loaded images |
| `r_imagelist_wad` | r_imagelist_wad [filter]: the images in the map's WAD files |
| `r_part` | r_part <name> { properties }: define a particle effect |
| `r_partinfo` | r_partinfo [effect]: an effect's definition, or the running effects |
| `r_partlist` | List the particle effects |
| `r_partredirect` | r_partredirect <effect> [replacement]: use one effect in place of another |
| `r_partreload` | Reload the particle effects |
| `r_remapshader` | r_remapshader <from> <to>: draw one material as another (game code or cheats) |
| `r_shaderlist` | r_shaderlist [filter]: the loaded materials |
| `r_showshader` | r_showshader [material]: a material's definition (default: the surface in view) |
| `r_trail` | r_trail <effect> [length]: draw an effect as a trail along the view |
| `rankadd` | rankadd <name> [password] [rights]: add a ranking |
| `rankfind` | rankfind <name>: rankings whose names contain the text |
| `ranklist` | List the rankings |
| `rankrconlevel` | rankrconlevel <ranking> <rights>: a ranking's rcon rights |
| `rankrefresh` | Save the connected players' scores into their rankings |
| `rankremove` | rankremove <ranking>: remove a ranking |
| `ranktopten` | The ten best scores in the rankings |
| `rcon` | rcon <command>: run a command on the server (needs rcon_password) |
| `reconnect` | Connect to the last server again |
| `record` | record <name> [map]: record a demo |
| `register_bestweapon` | register_bestweapon <impulse> <items bit> [ammo field]: teach bestweapon a weapon |
| `+reload` / `-reload` | Reload |
| `reload` | Start the map over |
| `removeid` | removeid <number>: lift a ban |
| `removeip` | removeip <ip or number>: lift a ban |
| `renameclient` | renameclient <user number> <name>: rename a player |
| `report_entities` | Entity counts by class |
| `rerecord` | Record again under the last name, from the start of the map |
| `reset` | reset <cvar or pattern>: back to the default value |
| `resetcfg` | Settings and binds back to their defaults |
| `restart` | Start the map over |
| `restrict` | restrict <command or cvar> [level]: the execution level needed to use it |
| `retry` | Connect again to the last server |
| `revert` | revert <cvar or pattern>: back to the default value |
| `+right` / `-right` | Turn right |
| `rmdemo` | rmdemo <name>: delete a demo |
| `rmdemonum` | rmdemonum <#>: delete a demo by its demolist number |
| `+rollleft` / `-rollleft` | Roll left (with `6dof`) |
| `+rollright` / `-rollright` | Roll right (with `6dof`) |
| `rotate` | rotate <degrees> [seconds]: turn the view |
| `route_reload` | Read the map's waypoint graph (data/<map>.way) again |
| `route_visualise` | route_visualise [0\|1]: show the waypoint graph of a local server |
| `save` | save <name> [comment]: save the game to saves/<name>.sav |
| `saveconfig` | saveconfig [name]: write the config |
| `savegame` | savegame <name> [comment]: save the game |
| `savegame_legacy` | savegame_legacy <name>: save in the classic single player format |
| `say` | say <text>: chat to everyone |
| `say_team` | say_team <text>: chat to your team |
| `sayone` | sayone <player> <text>: a private message |
| `screenshot` | screenshot [name]: save the screen (r_screenshot_format) |
| `screenshot_360` | screenshot_360 [name] [width] [height]: all the way round, equirectangular |
| `screenshot_cubemap` | screenshot_cubemap [name] [size]: six faces from here, as a skybox set |
| `screenshot_jpeg` | screenshot_jpeg [name]: a JPEG screenshot |
| `screenshot_mega` | screenshot_mega [name] [width] [height]: the 3D view drawn bigger than the window |
| `screenshot_stereo` | screenshot_stereo [name] [width] [height]: left and right eye side by side |
| `screenshot_tga` | screenshot_tga [name]: a TGA screenshot |
| `screenshot_vr` | screenshot_vr [name] [width]: stereo 360, left eye over right |
| `seat_add` | seat_add [guest\|<name>] [device]: another local player (split screen) |
| `seat_device` | seat_device <seat> <device>: give a player the keyboard (0) or a controller (1..4) |
| `seat_list` | The local players and their controllers |
| `seat_remove` | seat_remove <seat>: a split screen player leaves |
| `sendcvar` | sendcvar <cvar>: tell the server a cvar's value (servers send this) |
| `serverinfo` | serverinfo [key value]: the server's settings, or set one on a local server |
| `set` | set <cvar> <value> [description]: set or create a cvar |
| `set_calc` | set_calc <cvar> <expression>: set a cvar to a computed value |
| `set_tp` | set_tp <var> <value>: set (ezQuake's name) |
| `seta` | seta <cvar> <value>: set a cvar and save it in the config |
| `seta_calc` | seta_calc <cvar> <expression>: set_calc, and archive the cvar |
| `setang_local` | setang_local <pitch> <yaw> [roll]: set the view angles |
| `setfl` | setfl <var> <value> <a\|u\|s>: set a cvar, archived (a), in userinfo (u) or serverinfo (s) |
| `setinfo` | setinfo [key value]: show or set userinfo |
| `setinfoblob` | setinfoblob <key> <file>: send a file as a userinfo blob |
| `setmaster` | setmaster <address>...: replace the master servers |
| `setpass` | setpass <ranking> [password]: set or clear a ranking's password |
| `setpause` | Pause the game |
| `setpos` | setpos <x> <y> <z> [pitch yaw roll]: move yourself (cheat) |
| `setrenderer` | setrenderer [gl]: the renderers there are; gl restarts video |
| `sets` | sets <cvar> <value>: set a cvar that's also in serverinfo |
| `setu` | setu <cvar> <value>: set a cvar that's also in userinfo |
| `showalias` | showalias <name>: what an alias runs |
| `showconsole` | Open the console |
| `+showdm` / `-showdm` | Show the leaders while held |
| `+showinfo` / `-showinfo` | Show the server info while held |
| `showpic` | showpic <image> <place> <x> <y> <zone> [width] [height] [touchcommand]: pin a picture to the screen |
| `showpic_removeall` | Remove the showpic pictures |
| `+showscores` / `-showscores` | Show the scoreboard while held |
| `+showteamscores` / `-showteamscores` | Show the team scores while held |
| `sizedown` | Smaller view (viewsize) |
| `sizeup` | Bigger view (viewsize) |
| `skel_generateragdoll` | skel_generateragdoll <model>: write a starting .doll ragdoll file for a model |
| `skel_info` | The live skeletal objects |
| `skins` | Reload the skins players use |
| `skipdl` | Give up on the current download |
| `sky` | sky <name>: change the sky |
| `skygroup` | skygroup <skybox> <map...>: maps that use a skybox |
| `skyroomfog` | skyroomfog <density> <red> <green> <blue> <alpha> <depthbias>: fog in the skyroom |
| `snap` | snap <userid>: get a player's screenshot |
| `snapall` | Get every player's screenshot |
| `snd_restart` | Restart the sound system |
| `soundcontrol` | soundcontrol off\|single\|multi\|card0 mono\|stereo\|front\|back |
| `soundinfo` | Sound device and mixer state |
| `soundlist` | List the loaded sounds |
| `spdevmap` | spdevmap <map>: single player with cheats |
| `+speed` / `-speed` | Walk (or run, with cl_run 0) |
| `spiderpig` | Toggle walking up walls (cheat) |
| `spmap` | spmap <map>: start a single player game |
| `+sprint` / `-sprint` | Sprint |
| `sqlkill` | sqlkill <serverid>: close a database connection |
| `sqlkillall` | Close every database connection |
| `sqlstatus` | The database drivers and the server's open connections |
| `startdemos` | startdemos <demo1> [demo2...]: play demos in a loop |
| `status` | The map, time and entity counts |
| `stealthmute` | stealthmute <player> [minutes]: their chat only reaches themselves, and they aren't told |
| `stop` | Stop recording (or playing) a demo |
| `stopdemo` | Stop the demo playing |
| `stopmusic` | Stop the music |
| `stopsound` | Stop all sounds |
| `stopul` | For internal use |
| `+strafe` / `-strafe` | The turn keys strafe instead |
| `stuffcmd` | stuffcmd <player> <text>: run commands on a player's game |
| `sv` | sv <command>: run a command of the server game (ConsoleCmd) |
| `sv_addport` | sv_addport [port]: listen on another UDP port; lists the ports without one |
| `sv_democancel` | Stop the server demo and delete it |
| `sv_demoeasyrecord` | sv_demoeasyrecord [name]: record a server demo named after the game |
| `sv_demoinfo` | sv_demoinfo <#\|*>: show a server demo's info |
| `sv_demoinfoadd` | sv_demoinfoadd <#\|*> <text>: add a line to a server demo's info |
| `sv_demoinforemove` | sv_demoinforemove <#\|*>: delete a server demo's info |
| `sv_demolist` | sv_demolist [filter]: list the server demos |
| `sv_demonumremove` | sv_demonumremove <#>: delete a server demo by its sv_demolist number |
| `sv_demorecord` | sv_demorecord <name>: record every player's view to demos/<name>.mvd |
| `sv_demoremove` | sv_demoremove <name\|*token>: delete server demos |
| `sv_demostop` | Stop the server demo |
| `sv_downloadchunksperframe` | Obsolete in FTE; does nothing |
| `sv_enableprofile` | profile_ssqc |
| `sv_gamedir` | sv_gamedir [name]: the game directory reported to clients |
| `sv_impulse` | sv_impulse <player> <impulse>: give a player's next move an impulse |
| `sv_lightstyle` | sv_lightstyle <index> <pattern>: change a light style |
| `sv_meminfo` | sv_meminfo [all]: the server's precache counts and QC memory (all lists the models) |
| `sv_progsname` | Obsolete in FTE; does nothing |
| `sv_progtype` | Obsolete in FTE; does nothing |
| `sv_saveentfile` | Write the map's entity text to maps/<map>.ent |
| `sv_serverinfo` | sv_serverinfo <key> <value>: publish a server key |
| `sv_settimer` | sv_settimer <count> <interval> <command>: repeat a command (count -1 forever, 0 removes) |
| `sv_speedcheck` | Obsolete in FTE; does nothing |
| `sys_select_timeout` | Obsolete in FTE; does nothing |
| `tcpconnect` | Connect to a server using the tcp:// prefix |
| `tell` | tell <player> <text>: a private message |
| `timedemo` | timedemo <name>: play a demo as fast as possible and report the frame rate |
| `timerefresh` | Draw 128 frames turning around once, and report the frame rate |
| `toggle` | toggle <cvar> [values...]: flip between 0 and 1, or step through the values |
| `toggleconsole` | Open or close the console |
| `togglemenu` | Open or close the menu |
| `topten` | The server's ten best ranked players |
| `tp_pickup` | tp_pickup [items\|default\|clear]: what counts as a pickup |
| `tp_point` | tp_point [items\|default\|clear]: what $point reports |
| `tp_took` | tp_took [items\|default\|clear]: what $took reports picking up |
| `track` | track <player\|off>: whose view a server demo or QTV stream shows |
| `track1` | track1 <player\|off>: whose view seat 1 shows |
| `track2` | track2 <player\|off>: whose view seat 2 shows |
| `track3` | track3 <player\|off>: whose view seat 3 shows |
| `track4` | track4 <player\|off>: whose view seat 4 shows |
| `ui_font` | ui_font <family> <file> [weight]: a font for UI views' CSS font-family |
| `ui_restart` | Reload the menu module (menu.dat; there is no Quake 3 ui.qvm) |
| `unalias` | unalias <name>: remove an alias |
| `unban` | unban <ip or number>: lift a ban |
| `unbind` | unbind <key>: clear a key |
| `unbindall` | Clear every key |
| `unignore` | unignore <name>: show a player's chat again |
| `unignore_id` | unignore_id <player number>: show a player's chat again |
| `unignore_team` | unignore_team <team>: show a team's chat again |
| `unignoreAll` | Clear the ignore list |
| `unignoreAll_team` | Clear the team ignore list |
| `unpause` | Unpause the game |
| `unsavegame` | unsavegame <name>: delete a saved game |
| `+use` / `-use` | Use |
| `user` | user <player>: a player's userinfo |
| `users` | List the players |
| `v_cshift` | v_cshift <r> <g> <b> <amount>: tint the screen (0-255) |
| `version` | Engine version and build date |
| `version_modelformats` | The model and map formats the engine reads |
| `vid_modelist` | List the display's video modes |
| `vid_reload` | Reload the games' renderer resources |
| `vid_restart` | Apply the video mode cvars and reload the games' renderer resources |
| `vid_toggle` | Switch between fullscreen and a window |
| `+voip` / `-voip` | Talk while held |
| `voip` | voip unmuteall\|maxgain [n]\|codecs\|devices |
| `vstr` | vstr <cvar>: run the cvar's value as commands |
| `wait` | wait [frames]: run the rest of the line in a later frame |
| `watchpoint` | watchpoint [global or entnum.field]: watchpoint_ssqc |
| `watchpoint_csqc` | watchpoint_csqc [global or entnum.field]: report each change in the client game |
| `watchpoint_menuqc` | watchpoint_menuqc [global or entnum.field]: report each change in the menu |
| `watchpoint_ssqc` | watchpoint_ssqc [global or entnum.field]: report each change in the server game |
| `waterfog` | waterfog <density> <red> <green> <blue> <alpha> <depthbias>: fog under water |
| `+weaponwheel` / `-weaponwheel` | Hold to pick a weapon with the mouse |
| `webcore_closemenu` | Close the HTML menu page |
| `wf` | wf <file> <text>: write text to a file in the game directory |
| `which` | which <file>: the package or directory a file comes from |
| `worker_status` | How many worker threads there are and how much work waits |
| `worker_test` | Send an empty job round a worker thread and print how long it took |
| `writeconfig` | writeconfig [name]: write the config |
| `writeid` | Save the permanent bans to banned_ip.cfg |
| `writeip` | Save the permanent bans to banned_ip.cfg |
