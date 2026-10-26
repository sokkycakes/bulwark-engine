# Console commands

Bulwark has 336 console commands and about 180 engine cvars (`cmdlist` and
`cvarlist` show them; `help <name>` and `find <text>` explain one). The game
modules add their own on top: Nuclide registers about 150 more. This page
covers how the console behaves, the formats the commands write, and what
from FTEQW and Source is left out. Behaviour copied from FTE was checked by
running FTE as a black box; nothing here comes from its source.

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

## Files the commands write

| What | Where | Notes |
|---|---|---|
| Config | `config.cfg`, or `configs/<name>.cfg` for `cfg_save <name>` | Same layout as FTE, which writes `fte.cfg` and `configs/<name>.cfg`. `cfg_load <name>` falls back to `<name>.cfg` at the top level. Saved on quit when a setting or bind changed (`cfg_save_auto`). |
| Bans | `banned_ip.cfg` | `addip`/`banid` lines, run at startup. |
| Saved games | `saves/<name>.sav` | See below. |
| Demos | `<name>.dem`; `easyrecord` uses `demos/<date>-<map>.dem` | See below. |
| Screenshots | `screenshots/` | `r_screenshot_format` picks png, jpg, tga or bmp. |
| Console log | `logfile` / `con_logfile` | `con_timestamp` adds the time to each line. |
| QC dumps | `ssqccore.txt`, `csqccore.txt`, `menuqccore.txt`, `<vm>platform.txt` | From `coredump_*` and `pr_dumpplatform`. |

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
  `menu_cmd`.

## Left out

These FTE and Source commands belong to systems Bulwark doesn't have.
Each would need that system first, not just the command.

- **FTE's built-in menus:** the `menu_*` pages, `menubind`, `menuedit`,
  `conmenu`, `closemenu` and similar. Bulwark runs the game's own menu
  progs instead.
- **QuakeWorld TV, MVD and server-side demos:** `qtv*`, `mvd*`,
  `sv_demo*`.
- **Voice chat:** `+voip`, `voip`, `cl_voip_mute`.
- **Plugins, SQL, the rank system, workers:** `plug_*`, `sql*`, `rank*`,
  `topten`, `worker_*`.
- **QuakeWorld teamplay and chat filtering:** `tp_*`, `set_tp`,
  `macrolist`, `loadloc`, `msg_trigger`, `ignore*`, `mute`, `cuff`,
  `cripple`, `floodprot`, `skins`, `allskins`, `+p1`..`+p4` (split-screen).
- **Master servers and other transports:** `heartbeat`, `setmaster`,
  `localservers`, `globalservers`, `connectnq`, `connectqe`, `connectbr`,
  `tcpconnect`, `qwurl`, `dtls_*`, `net_ice_show`, `cl_transfer`.
- **Downloads:** `download`, `dlsize`, `skipdl`, `finishdl`, `stopul`,
  `nextul`, `curl`, `download_map_url`. Bulwark has no download protocol
  yet; players need the content already.
- **Video and media capture and playback:** `capture*`, `playvideo`,
  `playfilm`, `cinematic`, `media_*`, `cd`. Nuclide calls `playvideo` for
  intro videos, which do nothing on Bulwark.
- **FTE's editors and model tools:** `r_editlights_*`, `mod_terrain_*`,
  `mapedit`, `modelviewer`, `mod_bspx_*`, `mod_buildcubemaps`,
  `skel_generateragdoll`, `compile`, `decompile`, `edit`.
- **Switching game directories while running:** `gamedir`,
  `fs_changegame`, `fs_changemod`, `sv_gamedir`, `fs_restart`, `pkg`.
  Bulwark sets the game directories on the command line.
- **Renderer listings and special screenshots:** `r_imagelist`,
  `r_shaderlist`, `mod_texturelist`, `listfonts`, `r_remapshader`,
  `pointfile`, `timerefresh`, `screenshot_360`, `screenshot_mega`,
  `screenshot_cubemap`. These could be added on the current renderer if
  needed.

## Full list

The engine's commands, from `cmdlist`. A `+` command runs while its key is
held and the matching `-` command when it's let go.

| Command | What it does |
|---|---|
| `addip` | addip <minutes> <ip[/bits]>: ban an address (0 minutes is permanent) |
| `alias` | alias <name> [commands]: name a list of commands; $1..$9 and $* are its arguments |
| `aliaslist` | aliaslist [pattern]: list the aliases |
| `apropos` | apropos <text>: commands and cvars whose name or help holds the text |
| `+attack` / `-attack` | Fire (button 0) |
| `+attack2` / `-attack2` | Secondary fire |
| `autosave` | Save to the "autosave" slot |
| `+back` / `-back` | Move back |
| `ban` | ban <player> [minutes] [reason]: ban a player's address |
| `banid` | banid <minutes> <slot>: ban a player's address |
| `banip` | banip <ip[/bits]> [reason] [seconds]: ban an address |
| `banlist` | List the bans |
| `bf` | The item-pickup screen flash |
| `bind` | bind <key> [command]: run a command on a key |
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
| `centerprint` | centerprint <text>: print in the middle of the screen |
| `centerview` | Level the view pitch |
| `cfg_load` | cfg_load [name]: reset to defaults, then run the config |
| `cfg_reset` | Settings and binds back to their defaults |
| `cfg_save` | cfg_save [name]: write settings, binds and aliases to a config |
| `cfg_save_ifmodified` | cfg_save, only if something changed since the last save |
| `changelevel` | changelevel <map>: go to a map, keeping the players' spawn parms |
| `changelevel2` | changelevel2 <map> [landmark]: move on to a map |
| `cl_cmd` | cl_cmd <command>: run a command of the client game (CSQC_ConsoleCommand) |
| `clear` | Clear the console |
| `clientkick` | clientkick <slot> [reason]: kick a player |
| `clients` | List the players on the server |
| `cmd` | cmd <text>: send a command to the server |
| `cmdlist` | cmdlist [pattern]: list the commands |
| `color` | color <top> [bottom]: player colours |
| `conactivate` | Open the console |
| `conclear` | Clear the console text |
| `conclose` | Close the console |
| `condump` | condump <file>: write the console text to a file |
| `conecho` | conecho <text>: print to the console |
| `conecho_center` | conecho_center <text>: print in the middle of the screen |
| `connect` | connect <address>: join a server |
| `coredump_csqc` | Write the stack, globals and entities of the client game to csqccore.txt |
| `coredump_menuqc` | Write the stack, globals and entities of the menu to menuqccore.txt |
| `coredump_ssqc` | Write the stack, globals and entities of the server game to ssqccore.txt |
| `cprint` | cprint <text>: print in the middle of the screen |
| `cvar_resettodefaults_all` | Every cvar back to its default |
| `cvar_resettodefaults_nosaveonly` | Every cvar that isn't archived back to its default |
| `cvar_resettodefaults_saveonly` | Every archived cvar back to its default |
| `cvarlist` | cvarlist [-c -d -F -a -r -h] [pattern]: list the cvars |
| `cvarreset` | cvarreset <cvar or pattern>: back to the default value |
| `cvarwatch` | cvarwatch <cvar>: print each change to it (again to stop) |
| `defer` | defer <seconds> <commands>: run commands later |
| `deletesave` | deletesave <name>: delete a saved game |
| `demo_gototime` | demo_gototime <seconds>: go to a time in the demo |
| `demo_info` | What demo is playing or recording |
| `demo_jump` | demo_jump <seconds>: go to a time in the demo (+n/-n relative) |
| `demo_pause` | Pause demo playback |
| `demo_resume` | Resume demo playback |
| `demo_setspeed` | demo_setspeed <scale>: playback speed (demo_timescale) |
| `demo_togglepause` | Pause or resume demo playback |
| `demolist` | List the demos |
| `demos` | Back to the startdemos loop |
| `demos_next` | Play the next demo of the startdemos loop |
| `devmap` | devmap <map>: start a map with cheats on |
| `differences` | List the cvars that differ from their defaults |
| `dir` | dir [pattern]: list game files |
| `disconnect` | Leave the game |
| `dsp_info` | The sound effects room (DSP) in use |
| `+duck` / `-duck` | Crouch |
| `easyrecord` | easyrecord [suffix]: record a demo under a dated name |
| `echo` | echo <text>: print to the console |
| `edict` | edict <n>: an entity's position, velocity and angles |
| `edictcount` | Entity counts by class |
| `edicts` | List the server's entities |
| `ent_create` | ent_create <classname> [key value...]: spawn where you look |
| `ent_dump` | ent_dump [entity] [all]: an entity's fields (default: under the crosshair) |
| `ent_info` | ent_info [entity]: an entity's fields |
| `ent_remove` | ent_remove [entity]: remove an entity (default: under the crosshair) |
| `ent_remove_all` | ent_remove_all <class or name>: remove all matching entities |
| `ent_setname` | ent_setname <name> [entity]: set a targetname |
| `ent_teleport` | ent_teleport <entity>: move an entity to where you look |
| `exec` | exec <file>: run a config file |
| `exit` | Quit the game |
| `extensionlist_csqc` | The QC extensions the client game can use |
| `extensionlist_menuqc` | The QC extensions the menu can use |
| `extensionlist_ssqc` | The QC extensions the server game can use |
| `fadein` | fadein [seconds] [r g b]: fade the screen in from a colour |
| `fadeout` | fadeout [seconds] [r g b]: fade the screen out to a colour |
| `find` | find <text>: commands and cvars whose name or help holds the text |
| `find_ent` | find_ent <text>: entities by classname or targetname |
| `flocate` | flocate <file>: the package or directory a file comes from |
| `fly` | Toggle flying (cheat) |
| `fog` | fog <density> [r g b]: set the fog (0 turns it off) |
| `force_centerview` | Level the view pitch |
| `+forward` / `-forward` | Move forward |
| `fullinfo` | fullinfo \key\value...: set several userinfo keys |
| `gamemap` | gamemap <map>: start a map |
| `give` | give <item or health> [amount]: (cheat) |
| `god` | Toggle invulnerability (cheat) |
| `help` | help <name>: describe a command or cvar |
| `hideconsole` | Close the console |
| `host_writeconfig` | host_writeconfig [name]: write the config |
| `hunkprint` | Memory statistics |
| `if` | if <expression> then <commands> [else <commands>] |
| `impulse` | impulse <n>: send an impulse with the next move |
| `in_bind` | in_bind <device> <key> [command]: bind for one input device |
| `in_deviceids` | List the input devices |
| `in_restart` | Reopen the game controllers |
| `inc` | inc <cvar> [amount]: add to a cvar, 1 by default |
| `incrementvar` | incrementvar <cvar> <min> <max> <delta>: step a cvar, wrapping |
| `join` | Join the game from spectating |
| `joy_virtual` | Show the on-screen controller |
| `joy_virtual_set` | Set up the on-screen controller |
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
| `listid` | List the bans |
| `listip` | List the bans |
| `listmaps` | listmaps [text]: list the maps |
| `listplayers` | List the players |
| `listsaves` | List the saved games |
| `load` | load <name>: load saves/<name>.sav |
| `loadgame` | loadgame <name>: load a saved game |
| `localinfo` | localinfo [key [value]]: server-side settings |
| `logfile` | logfile [name]: start or stop copying the console to a file |
| `+lookdown` / `-lookdown` | Look down |
| `+lookup` / `-lookup` | Look up |
| `ls` | ls [pattern]: list game files |
| `map` | map <name>: start a map |
| `map_restart` | map_restart [seconds]: start the map over |
| `maplist` | maplist [text]: list the maps |
| `maps` | maps [text]: list the maps |
| `menu_cmd` | menu_cmd <command>: run a command of the menu (m_consolecommand) |
| `menu_restart` | Reload the menu |
| `messagemode` | Type a chat message to everyone |
| `messagemode2` | Type a chat message to your team |
| `+mlook` / `-mlook` | Accepted for old configs; the mouse always looks |
| `+movedown` / `-movedown` | Swim or fly down |
| `+moveleft` / `-moveleft` | Strafe left |
| `+moveright` / `-moveright` | Strafe right |
| `+moveup` / `-moveup` | Swim or fly up |
| `multvar` | multvar <cvar> <min> <max> <factor>: scale a cvar within a range |
| `music` | music <file or track> [-]: play music |
| `netstats` | Network statistics: latency, loss and traffic |
| `noclip` | Toggle flying through walls (cheat) |
| `notarget` | Toggle being ignored by monsters (cheat) |
| `observe` | Spectate |
| `path` | The search path for game files |
| `pause` | Pause or unpause the game |
| `pingserver` | pingserver <address>: ask a server for its status |
| `play` | play <sound>: play a sound |
| `play2` | play2 <sound>...: play sounds without position |
| `playdemo` | playdemo <name>: play a demo |
| `playvol` | playvol <sound> <volume>: play a sound at a volume |
| `poke_csqc` | poke_csqc <global or entnum.field> [value]: show or set a value in the client game |
| `poke_menuqc` | poke_menuqc <global or entnum.field> [value]: show or set a value in the menu |
| `poke_ssqc` | poke_ssqc <global or entnum.field> [value]: show or set a value in the server game |
| `pr_dumpplatform` | pr_dumpplatform [ssqc/csqc/menuqc]: write the builtins and extensions to a file |
| `pr_trace` | pr_trace [text]: print each QC function entered whose name contains the text |
| `precaches` | List the precached models, sounds and effects |
| `profile_csqc` | profile_csqc [n]: profile the client game; again to show the top n functions |
| `profile_menuqc` | profile_menuqc [n]: profile the menu; again to show the top n functions |
| `profile_ssqc` | profile_ssqc [n]: profile the server game; again to show the top n functions |
| `+prone` / `-prone` | Go prone |
| `quickload` | Load the "quick" slot |
| `quicksave` | Save to the "quick" slot |
| `quit` | Quit the game |
| `r_decalinfo` | The decals on screen |
| `r_effect` | r_effect <name>: start a particle effect where you look |
| `r_partlist` | List the particle effects |
| `r_partreload` | Reload the particle effects |
| `rcon` | rcon <command>: run a command on the server (needs rcon_password) |
| `reconnect` | Connect to the last server again |
| `record` | record <name> [map]: record a demo |
| `+reload` / `-reload` | Reload |
| `reload` | Start the map over |
| `removeid` | removeid <number>: lift a ban |
| `removeip` | removeip <ip or number>: lift a ban |
| `report_entities` | Entity counts by class |
| `rerecord` | Record again under the last name, from the start of the map |
| `reset` | reset <cvar or pattern>: back to the default value |
| `resetcfg` | Settings and binds back to their defaults |
| `restart` | Start the map over |
| `retry` | Connect again to the last server |
| `revert` | revert <cvar or pattern>: back to the default value |
| `+right` / `-right` | Turn right |
| `rmdemo` | rmdemo <name>: delete a demo |
| `save` | save <name> [comment]: save the game to saves/<name>.sav |
| `saveconfig` | saveconfig [name]: write the config |
| `savegame` | savegame <name> [comment]: save the game |
| `say` | say <text>: chat to everyone |
| `say_team` | say_team <text>: chat to your team |
| `screenshot` | screenshot [name]: save the screen (r_screenshot_format) |
| `screenshot_jpeg` | screenshot_jpeg [name]: a JPEG screenshot |
| `screenshot_tga` | screenshot_tga [name]: a TGA screenshot |
| `serverinfo` | serverinfo [key value]: the server's settings, or set one on a local server |
| `set` | set <cvar> <value> [description]: set or create a cvar |
| `set_calc` | set_calc <cvar> <expression>: set a cvar to a computed value |
| `seta` | seta <cvar> <value>: set a cvar and save it in the config |
| `seta_calc` | seta_calc <cvar> <expression>: set_calc, and archive the cvar |
| `setang_local` | setang_local <pitch> <yaw> [roll]: set the view angles |
| `setinfo` | setinfo [key value]: show or set userinfo |
| `setinfoblob` | setinfoblob <key> <file>: send a file as a userinfo blob |
| `setpause` | Pause the game |
| `setpos` | setpos <x> <y> <z> [pitch yaw roll]: move yourself (cheat) |
| `sets` | sets <cvar> <value>: set a cvar that's also in serverinfo |
| `setu` | setu <cvar> <value>: set a cvar that's also in userinfo |
| `showalias` | showalias <name>: what an alias runs |
| `showconsole` | Open the console |
| `sizedown` | Smaller view (viewsize) |
| `sizeup` | Bigger view (viewsize) |
| `sky` | sky <name>: change the sky |
| `snd_restart` | Restart the sound system |
| `soundinfo` | Sound device and mixer state |
| `soundlist` | List the loaded sounds |
| `spdevmap` | spdevmap <map>: single player with cheats |
| `+speed` / `-speed` | Walk (or run, with cl_run 0) |
| `spmap` | spmap <map>: start a single player game |
| `+sprint` / `-sprint` | Sprint |
| `startdemos` | startdemos <demo1> [demo2...]: play demos in a loop |
| `status` | The map, time and entity counts |
| `stop` | Stop recording (or playing) a demo |
| `stopdemo` | Stop the demo playing |
| `stopmusic` | Stop the music |
| `stopsound` | Stop all sounds |
| `+strafe` / `-strafe` | The turn keys strafe instead |
| `stuffcmd` | stuffcmd <player> <text>: run commands on a player's game |
| `sv` | sv <command>: run a command of the server game (ConsoleCmd) |
| `sv_enableprofile` | profile_ssqc |
| `sv_lightstyle` | sv_lightstyle <index> <pattern>: change a light style |
| `sv_saveentfile` | Write the map's entity text to maps/<map>.ent |
| `sv_serverinfo` | sv_serverinfo <key> <value>: publish a server key |
| `tell` | tell <player> <text>: a private message |
| `timedemo` | timedemo <name>: play a demo as fast as possible and report the frame rate |
| `toggle` | toggle <cvar> [values...]: flip between 0 and 1, or step through the values |
| `toggleconsole` | Open or close the console |
| `togglemenu` | Open or close the menu |
| `unalias` | unalias <name>: remove an alias |
| `unban` | unban <ip or number>: lift a ban |
| `unbind` | unbind <key>: clear a key |
| `unbindall` | Clear every key |
| `unpause` | Unpause the game |
| `unsavegame` | unsavegame <name>: delete a saved game |
| `+use` / `-use` | Use |
| `user` | user <player>: a player's userinfo |
| `users` | List the players |
| `v_cshift` | v_cshift <r> <g> <b> <amount>: tint the screen (0-255) |
| `version` | Engine version and build date |
| `vid_modelist` | List the display's video modes |
| `vid_reload` | Reload the games' renderer resources |
| `vid_restart` | Apply the video mode cvars and reload the games' renderer resources |
| `vid_toggle` | Switch between fullscreen and a window |
| `vstr` | vstr <cvar>: run the cvar's value as commands |
| `wait` | wait [frames]: run the rest of the line in a later frame |
| `watchpoint` | watchpoint [global or entnum.field]: watchpoint_ssqc |
| `watchpoint_csqc` | watchpoint_csqc [global or entnum.field]: report each change in the client game |
| `watchpoint_menuqc` | watchpoint_menuqc [global or entnum.field]: report each change in the menu |
| `watchpoint_ssqc` | watchpoint_ssqc [global or entnum.field]: report each change in the server game |
| `which` | which <file>: the package or directory a file comes from |
| `writeconfig` | writeconfig [name]: write the config |
| `writeid` | Save the permanent bans to banned_ip.cfg |
| `writeip` | Save the permanent bans to banned_ip.cfg |
