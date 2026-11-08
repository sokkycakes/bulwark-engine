# Bulwark Link: handhelds as companion players

A Bulwark server (listen or dedicated) can take **link players**. A link
player is a small device, the Nintendo DS first, that joins the game as its
own player over UDP. It sends its buttons, stick and touch input, and it gets
back:

- a **private screen**, which the game draws with QuakeC (the bottom screen
  on the DS: map, inventory, messages, like GBA link play);
- a **view**: where its player is and what is near it, so the device can
  draw the world itself (the top screen on the DS, through its own map
  renderer).

The device runs none of the game's code, so this works with full Nuclide
games on the PC.

## On the server

| Cvar | Default | |
|---|---|---|
| `sv_link_port` | `27901` | UDP port for link players, `0` turns links off |
| `sv_link_max` | `4` | how many player slots link players may take |
| `sv_link_timeout` | `10` | seconds without input before a link player is dropped (after a map change it gets 60 more, while the device loads the map, but never more than 60 s of silence in all) |
| `sv_link_log` | `0` | `1`: print every command a link v2 player runs, with its state before and after, as `LINKPM` lines (see "The movement log") |
| `sv_link_lag` | `0` | milliseconds each link packet is held, each way, before it is handled or sent: tests a device at a given round trip |
| `sv_link_autoaim` | `5` | degrees: a link player's shots bend toward a monster this far from the crosshair, `0` for none (see "Autoaim") |

A link player takes a normal client slot and edict. The game sees it as a
real player: `clienttype` is `CLIENTTYPE_REAL`, `ClientConnect` and
`PutClientInServer` run, and its input reaches the game through the usual
`input_*` globals in `SV_RunClientCommand`. The `*link` info key is `1`.
The engine sends it no entity snapshots. An old (link v1) device sends the
state of its sticks and buttons, and between packets the engine repeats the
last command, as it does for bots. A link v2 device sends numbered commands
instead; each runs once, as a PC client's do (see "Prediction").

### Default input mapping

The device sends movement (-127..127 per axis), view angles, and its raw
keys. A device that maps its own buttons (the DS does) also sends the
game's buttons and a one-shot impulse at the end of `INPUT`, and the server
takes those as they are. Otherwise the server maps the keys:

| Device | usercmd |
|---|---|
| move axes | `movevalues`, ×400/127 forward and side, ×400/127 up |
| angles | `angles` (pitch, yaw), as sent |
| A | 0x01 (attack) |
| B | 0x02 (jump) |
| X | 0x10 (use) |
| Y | 0x08 (reload) |
| R | 0x20 (secondary attack) |

These are Nuclide's `INPUT_*` bits (`src/shared/input/input.h`): attack
0x01, jump 0x02, prone 0x04, reload 0x08, use 0x10, secondary 0x20, sprint
0x40, crouch 0x80. DS builds before link v2 sent use as 0x20 and secondary
as 0x10 in their `buttons`; the server swaps those two back for a device
whose `JOIN` carries no `linkver`. The impulse goes into the move
once, when its count changes (the device repeats it in every packet, so a
lost packet loses nothing).

The game can read the raw state with `link_keys` and `link_touch` and do its
own mapping.

### QuakeC builtins

```c
float(entity player) link_isdevice;     // 1 for a link player
float(entity player) link_keys;         // raw device key bits (DS: KEY_A = 1 ... as libnds)
vector(entity player) link_touch;       // x, y on the private screen; z = 1 while touching
void(entity player, vector rgb) link_clear;
void(entity player, float x, float y, float w, float h, vector rgb) link_fill;
void(entity player, float x, float y, string text, vector rgb) link_text;  // 8x8 font
void(entity player) link_present;       // send what was drawn since link_clear
void(entity player, vector org, float kind, vector rgb) link_marker;     // for the next view update
```

Colours are 0..1 per channel. The DS's private screen is 256×192, and text
is 8×8 per character.

Until the game first calls `link_present` for a player, the engine shows a
default screen: the player's name, health, the map and the player list.

## Packets

All packets are UDP. Every packet starts with the 4 bytes `BWL1` and then
a type byte. Multi-byte values are little endian. A device keeps sending
`INPUT` at 20 Hz or more. That also acknowledges the last screen it got.

### Device to server

| Type | Name | Payload |
|---|---|---|
| 0x01 | DISCOVER | nothing (broadcast to the port) |
| 0x02 | JOIN | u32 session, u16 device (1 = DS), char name[16], then optionally u8 linkver (2) |
| 0x03 | INPUT | u32 session, u16 seq, u16 keys, u8 touch (bit 0 down), u8 tx, u8 ty, s8 move[3], s16 pitch, s16 yaw, u16 screen_ack, then optionally u8 fps, u8 min_fps, u16 peak_polys (the device's last second), u16 load_ms (its last map load, in 10 ms steps), u16 spawncount (the map it has loaded), u16 buttons (the game's buttons, as the device mapped them), u8 impulse, u8 impulse_count, then optionally the commands (link v2, below) |
| 0x04 | LEAVE | u32 session |

Angles use 65536 per turn, and positive pitch looks up.

### Server to device

| Type | Name | Payload |
|---|---|---|
| 0x81 | INFO | char host[32], char map[32], u8 players, u8 maxplayers, u8 link_free |
| 0x82 | WELCOME | u32 session, u8 playernum, u16 spawncount, char map[64], u8 linkver (2 from a v2 server) |
| 0x83 | REFUSE | char reason[64] |
| 0x84 | SCREEN | u32 session, u16 version, then draw commands until the end of the packet |
| 0x85 | VIEW | u32 session, u16 seq, s32 origin[3] (1/8 units), s16 pitch, s16 yaw, s16 health, u8 flags (1 the game turned us, 2 on the ground, 4 crouching), u8 count, then count × {s16 org[3] (units), u8 kind, u16 rgb15}, then s16 velocity[3] (units a second), then `'A'`, u8 cone (tenths of a degree, 0 off), u16 target (edict, 0 none), s16 point[3] (units) (see "Autoaim") |
| 0x86 | KICK | u32 session, char reason[64] |
| 0x87 | ENTS | u32 session, u16 seq, u16 nummodels, u8 shown[(nummodels + 7) / 8], u8 moved, moved × {u16 model, s16 origin[3] (1/4 units), s16 angles[3]}, u8 things, u8 thing_size (19), things × {s16 org[3] (units, the bottom middle), u8 yaw (256 a turn), u8 half_width, u8 height, u8 kind, u16 entnum, u16 model, u8 frame (the sequence), u8 lift (how far the model's origin sits above the bottom), u16 frame1time (1/100 s), u8 alpha (255 opaque)}, then u32 time (ms), u8 count, count × {u8 thing, s16 velocity[3] (units a second)} |
| 0x88 | MODELS | u32 session, u16 spawncount, u8 count, count × {u16 model, u8 len, char name[len]} |
| 0x89 | STATS | u32 session, s16 health, s16 armour, s16 score, u8 len, char weapon[len], u16 viewmodel, u8 sequence, u8 plays |
| 0x8A | FX | u32 session, u8 count, count × {u8 flags, s32 hit[3] (1/8 units), s32 eye[3] (1/8 units)} |
| 0x8B | PSTATE | the player's state after the last command run (link v2, below) |
| 0x8C | PMOVEVARS | the game's movement values (link v2, below) |
| 0x8D | BRUSHINFO | the brush entities that collide (link v2, below) |
| 0x8E | WEAPONINFO | the weapon in hand, as its game code reads it (link v2, below) |
| 0x8F | HUDINFO | what the co-op HUD shows (below) |
| 0x90 | EVENT | prints, chat, centerprints and the game's CSQC events, as records (below) |
| 0x91 | SOUND | one-shot sounds (below, "Sound") |
| 0x92 | AMBIENT | the client-side loops near the player (below, "Sound") |

The server sends `WELCOME` again whenever the map changes (new
`spawncount`), and every 2 s in case one was lost. The device then loads
`maps/<map>.bsp` from its own storage if it has a copy. Without one, it
shows no 3D view. While it loads it reads nothing, and its network stack
keeps every packet that arrives, so the server sends nothing else until the
device's `INPUT` carries the new `spawncount` (a device that leaves the
field out is taken to have loaded at its first packet after the welcome).
Several devices can share one address (behind one router): the server tells
them apart by session and port.

The server resends `SCREEN` every 250 ms until the device's `screen_ack`
matches its version. One screen fits in one packet, up to 1200 bytes of
commands.

The server sends `VIEW` at 20 Hz. Flag bit 0 means the game turned the
player (it set `fixangle`, after a teleporter or a respawn): the device
takes the pitch and yaw in this packet as its own. Otherwise the device
keeps its own angles, and the ones in `VIEW` echo what it sent. Kinds are 0 for other players and 1 to
255 for whatever the game passes to `link_marker`.

The server sends `ENTS` with each `VIEW`, so the device can draw the
entities itself:

- `shown` has a bit for each brush model of the map (`*1` and up): set
  while an entity the game networks uses that model. Triggers and entities
  without a `SendEntity` are left out, so a door that breaks or a wall that
  toggles off disappears.
- `moved` lists the brush models whose entity has a non-zero origin or
  angles: doors, lifts, trains and rotating things, and Half-Life models
  built round an origin brush. The device draws the model translated to
  `origin` and turned by `angles` (yaw about z, then pitch, then roll), and
  draws it between updates (below).
- `things` are the nearest other entities with a model (at most 48 within
  2048 units), including the other players. Kind 1 is a monster
  (`FL_MONSTER`), 2 something solid, 3 something not solid, 4 a dead
  monster or a body (`SOLID_CORPSE`). `thing_size` lets a later version add fields. `model` is the
  entity's model index when it is a studio model (`.mdl`), or 1000 plus the
  slot for a player, whose model is `models/player/<m>/<m>.mdl` from their
  `model` userinfo (or `models/player.mdl`); 0 for none. A player's body
  (Nuclide's body queue: not a client, `SOLID_CORPSE`, no `FL_MONSTER`)
  whose model is no studio model (the co-op heroes' `.md2`s) is sent as
  its player's, 1000 plus the slot: the player who was dead within 160
  units when it appeared (else the nearest with its model), kept while it
  lies there. Its `frame` is the death sequence of `models/player.mdl` the
  game gave it. A device that has
  a DS copy of that model (tools/nds/hlmdl.py) draws it playing `frame`'s
  sequence, and otherwise a box. `frame1time` is how long that sequence
  has played (the game's `frame1time`, which the PC client draws the
  model at; 0xffff when the game has no such field). Past 600 s a looping
  sequence's is sent modulo the most whole laps of it (`Studio_SeqDuration`)
  under 600 s, which poses the model the same; Nuclide only sets it back
  when the sequence changes, so a monster idling for long would otherwise
  reach the field's limit and stop there.
- `time` is the game's time (`sv.time`, the time the PC's snapshots
  carry) for everything in the packet, in milliseconds. Then the velocity
  of the other players among `things` (at most 8, by their place in the
  list), which the PC's client game moves them on by between updates
  (Nuclide's `cg_extrapolateClients`, on by default).
- `alpha` is how opaque the PC draws the thing: Nuclide's render amount
  (`ncRenderableEntity::m_renderAmount`, 0 to 255) when its render mode is
  one that uses it (1, 2, 4 or 5), else 255. The co-op game fades bodies
  out by it before it hides them.
- A device that reads only 16 bytes of a thing (`thing_size` says how far
  to step) and stops after the things sees the packet as before.

The DS draws the things and the moved brush models as the PC client draws
every networked entity (`CSQC_LerpEntity`, `src/client/cl_csqc.c`): a
little behind `time` as it reckons the server's clock, between the two
updates around that moment, `frame1time` included; the other players at
the newest update moved on by their velocity (`docs/platforms/nds.md`,
"Other entities").

`MODELS` names the model indexes in `things`, a few at a time with every
fifth `ENTS`, until the device has had them all for this map.

`STATS` comes with every fourth `ENTS`: the player's `health`,
`armorvalue` and `frags`, and the weapon in hand as its class name without
`weapon_` (from Nuclide's `m_activeWeapon`; empty when there is none, at
most 24 bytes). A field the game doesn't have is sent as 0. Then the weapon's view
model as a model index (named by `MODELS`; Nuclide's `ncWeapon::viewModel`,
0 for none), the sequence it plays (`ncPlayer::weaponAnimSequence`, an
integer field: servers before WEAPONINFO read it as a float and always sent
0), and a
count that goes up each time a sequence starts, the same one again
included (seen as `weaponAnimTime` going back). `STATS` also goes out with
the next `ENTS` whenever that count changes. The DS shows
them above its touch pad.

`FX` carries bullet impacts. The PC client draws these from the game's own
CSQC events, and draws muzzle flashes and tracers itself; a device runs no
CSQC, so the server reads Nuclide's impact multicasts (`SVC_CGAMEPACKET`
with `EV_SURFIMPACT` or `EV_SURFIMPACTID`, which begin with the hit point)
as they go to a device's client, PVS rules included, and passes on the
point. The cause is QC `self` when the multicast is made, or its `owner`
for a projectile. Flags: 2 when there is a shooter, with `eye` its origin
plus `view_ofs`; 1 as well when that shooter is this device's player. Up
to 16 impacts are kept between server frames and sent once a frame. The
device draws a puff at each hit; for a shot also a muzzle flash and a
tracer, unless the hit is within 72 units (a melee hit), and one flash for
a volley of pellets.

A device ignores an `ENTS` older than the last one it used.

### Autoaim

A link player aims with a stylus or buttons, so, as Half-Life did for players
on a pad, its shots bend toward a monster near the crosshair. Before each of
its commands runs, the server looks along the command's angles from the
player's eye (`origin + view_ofs`):

- Nothing happens while `sv_link_autoaim` is 0 or the device's `INPUT` says
  `'A'` without bit 0, or when the straight line (2048 units) already meets
  a monster (`takedamage` 2), a friend in the way or a player.
- Otherwise the target is the monster nearest the crosshair whose middle
  (`(absmin + absmax) / 2`) is within `sv_link_autoaim` degrees of it (1.25
  times that for the target of the command before, so it doesn't flicker at
  the edge), at most 2048 units away, with nothing between (a `MOVE_NORMAL`
  line from the eye, at most 4 tried): `takedamage` 2 (Nuclide's
  `ncSurfacePropEntity::EnableAimAssist`, every monster), `health` above 0,
  not a player, and not a friend: Nuclide's `m_iAlliance` (the def's `team`)
  not 0 (`MAL_FRIEND`) or 4 (`MAL_NEUTRAL`).
- The shot goes to the target's middle. Nuclide's weapons fire from the
  right hand's XR input (`ncWeapon::FiredWeaponAttack`: `m_xrInputRight`'s
  `GetOrigin` and `GetAngles`, handed to each hitscan or projectile of the
  fireInfo), which without XR is the eye turned by `input_right_angles`
  (taken while `input_right_status` has `XR_STATUS_ANG`). For a link player
  the server sets those two for the command: the turn from the angles of the
  command before (Nuclide's XR space holds them: `Physics_Run` sets it after
  the movement) to the target, or none, and clears them afterwards, so no
  other player's command sees them (the engine sets them for nobody else).
- The player's view, `v_angle` and the command's angles stay as the device
  sent them: the movement and the device's prediction don't change. Melee
  and thrown attacks (`v_angle`, `GetViewAngle`) don't bend, as in
  Half-Life.

With `developer 1` the server logs `link: <name> fires with autoaim on edict
<n> (<classname>), <d> degrees off the crosshair` when a link player starts
firing or its target changes while firing.

`VIEW`'s `'A'` carries the cone and what the last command's shots bend
toward. The DS draws its crosshair on that target, as Half-Life moved its
crosshair onto the autoaim target: at the middle of the target as the DS
draws it (`ENTS`), or at `point` when it is not among the things, while it
is in front within 1.5 times the cone; otherwise in the middle of the screen.
Its own tracers already go to the target, since they go to the server's
impacts (`FX`). An old device reads `VIEW` only up to the velocity and sends
no `'A'`: it gets autoaim, with its crosshair in the middle.

`link_teleport x y z [yaw] [name]` (with `sv_cheats 1`) puts every link
player, or the one named, at a point, for testing a map's triggers and
level changes. `link_god [0|1]` (with `sv_cheats 1`) makes the link players
take no damage, for leaving them in a fight while measuring. `link_stats`
lists the link players with the frame rate their devices report.

## Prediction (link v2)

A link v2 device predicts its own movement, as the PC client does: it runs
a C port of the game's player movement (Nuclide's `PMoveCustom` and
`ncPlayer::Physics_Run`) on each command as it makes it, and when the
server's state for a command comes back it starts from that state and runs
again the commands the server has not run yet.

- The device makes a command every vblank (16 or 17 ms, `msec` in whole
  milliseconds that add up to real time at the DS's 59.8261 Hz), so two a
  frame at 30 fps (below 30 fps one command covers two vblanks, so a frame
  never runs more than two), and sends them at most 30 times a second, each `INPUT`
  with every command it made on this map that the server has not
  acknowledged, up to the newest 8. The PC makes 14 ms commands (72 Hz);
  33 ms commands would jump 2.5 units lower (39.6 against 42.0 on
  coop_arena), 17 ms ones jump 41.7 (`tools/nds/pmtest -resample`).
- The server runs each command **once**, when the packet arrives, through
  the same code as a PC client's move: the movement budget, `forced_impulse`
  and the admin filters apply, `servertime_ms` is 0 (no lag rewind). It
  never repeats a v2 player's last command and the bot loop leaves the
  player alone: while the device sends nothing (loading a map) the player
  stands still.
- A command already run (a resend) is skipped. A sequence that never arrived
  is lost for good and counted in PSTATE's `gaps`. Once a command has run,
  an `INPUT` whose `newest` is more than 1024 past it is a bad packet and
  is dropped whole.
- While the client is not in the game, or the device's `spawncount` is not
  the server's map, commands run nothing and are acknowledged anyway, so
  commands made for another map or before this spawn never run. Sequences
  keep counting across maps.
- PSTATE's `ack` is the last command run or dropped.
- The device compares its own state for `ack` with PSTATE's, every field
  the movement reads (position within 1/32 unit, velocity within 0.5 units
  a second, the clocks and stamina within 1/1000, the rest exactly); when
  one differs it starts from PSTATE's state and runs again the commands
  after `ack`, 16 a frame at most (a longer backlog goes on over the next
  frames while the view stays on the old prediction), and keeps PSTATE's
  state as its own for `ack`. With more than 63 commands after `ack` it takes PSTATE's state
  as it is. A PSTATE made with a `varsver` or `brushver` the device doesn't
  have yet is skipped; it goes on predicting with what it has.

A link is v2 from its first `INPUT` with a valid command list; an old device
and an old server see nothing new (an old server reads only the v1 fields,
which a v2 device fills from its newest command; an old device ignores the
WELCOME byte and the new packets, which only go to v2 links).

### INPUT commands

After the 32 bytes of the v1 fields (all present):

| Offset | Type | Field |
|---|---|---|
| 32 | u8 | `'C'` |
| 33 | u8 | version, 2 |
| 34 | u32 | `newest`: the sequence of the last command (the first command after a join is 1) |
| 38 | u8 | `count`, 1..8 |
| 39 | u8 | `cmdsize`, 14 |
| 40 | u16 | `varsack`: the PMOVEVARS version the device applied for its map, 0 for none |
| 42 | u16 | `brushack`: the BRUSHINFO version it has complete for its map, 0 for none |
| 44 | count × 14 | commands, oldest first: command `i` has sequence `newest - (count - 1) + i` |
| 44 + 14 × count | u8, u16, u16 | optional: `'W'`, the WEAPONINFO version the device has (0 none yet), and its edict; a device that sends it takes WEAPONINFO |
| then | u8, u16 | optional, after `'W'` when that is there: `'H'`, the HUDINFO version the device has (0 none yet); a device that sends it draws the HUD and takes HUDINFO |
| then | u8, u16 | optional (after `'W'` and `'H'` when they are there, or at 32 without commands): `'E'`, the last EVENT record the device has taken in order (0 none); a device that sends it takes EVENT |
| then | u8, u16, u8 | optional, after `'E'`: `'S'`, the AMBIENT version the device has (0 none), caps (bit 0: it plays sound); a device that sends it with bit 0 takes SOUND, AMBIENT, EVENT records 12-15 and WEAPONINFO's sound events |
| then | u8, u8 | optional, after `'S'`: `'L'`, the count of the device's last-weapon presses (the PC's `weaplast`, Q), mod 256. Each change after the first one heard switches the player to `m_lastWeapon` while they still own it, as the next command's `input_cursor_entitynumber` (the PC's client sends its weapon choice the same way), or else sends impulse 12 (the previous weapon, `GetLastWeapon`'s own fallback) |
| then | u8, u8 | optional, last, after `'L'`: `'A'`, flags: bit 0 the device's autoaim is on (`autoaim=` in the DS's `link.txt`). Without it the device is taken to want autoaim (see "Autoaim") |

A command: u8 `msec` (1..100), s16 `pitch` (65536 a turn, up positive),
s16 `yaw`, s16 `move[3]` (forward, side, up, in the game's
`input_movevalues` units), u16 `buttons` (Nuclide's `INPUT_*`), u8 `impulse`
(0 for none). The list is valid when `count` is 1..8, `cmdsize` is 14, the
packet holds all of it and `newest >= count`; otherwise the packet is taken
as v1. With a valid list the server ignores the v1 `move`, `pitch`, `yaw`,
`buttons` and `impulse` fields; `keys`, `touch`, `screen_ack`, the frame
rate, `load_ms` and `spawncount` are used as before. Offsets here count from
the session, after the magic and type.

### PSTATE

Sent after the `VIEW` of a send pass to a v2 device that has the map, when a
command ran since the last one (so at the device's packet rate, at most 30 a
second), or with a `VIEW` when none went for 0.1 s (at least 10 a second). The state is the player's at send time, after the
server frame; movers and boxes are read at the same moment.

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | session |
| 4 | u32 | `ack`: the last command run or dropped, 0 for none |
| 8 | u16 | `pseq`, +1 each PSTATE |
| 10 | u8 | `status`: 1 PREDICT, 4 AUTOJUMP, 8 LONGJUMP, 16 HLRULES |
| 11 | u8 | `clipped`: commands the movement budget cut short (wraps) |
| 12 | u16 | `gaps`: commands that never arrived (wraps) |
| 14 | u16 | spawncount |
| 16 | u16 | `varsver`: the PMOVEVARS version now |
| 18 | u16 | `brushver`: the BRUSHINFO version now |
| 20 | u8 | `fixseq`: +1 each time the game turned the player |
| 21 | u8 | `nmovers`, 0..8 |
| 22 | s16 | `fixpitch` (65536 a turn, up positive) |
| 24 | s16 | `fixyaw` |
| 26 | f32×3 | `origin` |
| 38 | f32×3 | `velocity` |
| 50 | f32×3 | `m_pmoveBaseVelocity` |
| 62 | f32×3 | `ncPlayer::m_pmoveHookVelocity` (a point) |
| 74 | f32×3 | `view_ofs` |
| 86 | f32×3 | `mins` |
| 98 | f32×3 | `maxs` |
| 110 | u32 | `flags` |
| 114 | u32 | `m_nuclideFlags` |
| 118 | u32 | `gflags` |
| 122 | u8 | `movetype` |
| 123 | u8 | `solid` |
| 124 | s8 | `waterlevel` |
| 125 | s8 | `watertype` (Quake contents) |
| 126 | u16 | `groundentity` as an edict number, 0 for the world or none |
| 128 | f32 | `gravity` |
| 132 | f32 | `friction` |
| 136 | f32 | `maxspeed` |
| 140 | f32 | `ncPlayer::m_timeSinceJump` |
| 144 | f32 | `ncPlayer::m_timeSinceTeleport` |
| 148 | f32 | `ncActor::m_currentStamina` |
| 152 | f32 | `ncPlayer::m_pmoveSpeedTarget` |
| 156 | f32 | the weapon's `PlayerSpeedMod()`, worked out from the three below |
| 160 | f32 | the weapon's `m_fiSpeedMod` (1 with no weapon) |
| 164 | f32 | `m_fiSpeedModFiring` (0 with no weapon) |
| 168 | f32 | `m_fiSpeedModZoomed` (0 with no weapon) |
| 172 | f32 | `ncPlayer::viewzoom` |
| 176 | s16 | health |
| 178 | u8 | `nboxes`, 0..16 |
| 179 | u8 | 1 (`MOVERANGLES`: the movers' angles and boxes close the packet), 0 from an older server |
| 180 | nmovers × 26 | u16 edict, f32 origin[3], f32 velocity[3] |
| | nboxes × 27 | u16 edict, f32 mins[3], f32 maxs[3], u8 solid (2, 3 or 4) |
| | 84 + 3 × nammo | the weapon block (below), from a server that has WEAPONINFO |
| | nmovers × 36 | with `MOVERANGLES`: f32 angles[3], f32 absmin[3], f32 absmax[3] of each mover |
| | 2 | with `MOVERANGLES`: u8 nmovers, `'R'` (the device finds the block from the end) |

Floats are IEEE binary32, little endian, as the server has them. Fields the
game lacks read 0 (the server says once per join which ones are missing).

- **PREDICT**: the device may predict. Set while: the link is v2; PMOVEVARS
  went out for this map; the game has `origin velocity mins maxs flags
  m_nuclideFlags movetype view_ofs`; health is above 0; `movetype` is not
  `MOVETYPE_NONE`; the player is not a fake spectator nor in a vehicle
  (`VFL_FAKESPEC`, `VFL_INVEHICLE`); its `hitcontentsmaski` is 0; neither
  `g_pmoveVars` nor its `m_pmoveVars` is the world; and Nuclide's
  `SV_RunClientCommand` would reach the movement (`_isPlayer` is 1,
  `g_mapEntitiesLoaded` set, `g_menuBackgroundMapMode` not; a game without
  them is not held back).
- **AUTOJUMP**: the player's `autojump` userinfo is `1`. **LONGJUMP**: its
  item list holds `item_longjump`. **HLRULES**: the game has
  `hlPlayer::Physics_Jump` (Half-Life's jump and sprint rules).
- **fixseq**: the game turned the player: it set `fixangle` (pitch 0, yaw
  from `angles`), sent its `EV_ANGLE` event to this player (Nuclide's
  `Client_FixAngle`, teleporters and respawns), or the player entered the
  game (its `angles`; the `EV_ANGLE` of a first spawn reaches nobody, as the
  client is not in the game yet). The device takes `fixpitch`/`fixyaw`
  when `fixseq` changes and runs again the commands after `ack` from the
  server's state, as for any state (the server runs them with the angles
  they were made with); the fields persist,
  so a lost PSTATE loses nothing. `VIEW` flag 1 stays for old devices.
- **movers**: brush entities of the current BRUSHINFO list within 256 units
  of the player (`absmin`..`absmax`) that move or turn, or are somewhere
  else or turned otherwise than that list says: the ground entity first,
  then the nearest, at most 8. A turned one's `absmin`/`absmax` are the ones
  the server culls it with (`sv_world.c` keeps the box it was last linked
  with: a `func_tracktrain` turned along its path keeps its spawn box).
- **boxes**: what the server's `MOVE_NORMAL` trace for the player tests as
  a box: entities with `solid` 2 or 3, or 4 without a brush model, that the
  owner and `dimension_solid`/`dimension_hit` rules let through and whose
  contents are in the player's movement mask; `origin + mins .. origin +
  maxs`; the nearest 16 within 512 units, by edict. ENTS is not for
  collision.

### PMOVEVARS

The values Nuclide's movement reads: `g_pmoveVars` (what `pmove_custom.qc`
reads) and the player's own `m_pmoveVars` (what `player_pmove.qc` reads;
Nuclide links it to the player's decl, so a game's classes can set their own
speeds). Each is the entity's `ncPMoveVars::<name>` field.

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | session |
| 4 | u16 | spawncount |
| 6 | u16 | version (+1 when a value changes, and with each map; never 0) |
| 8 | u8 | count, 37 |
| 9 | u8 | 0 |
| 10 | f32 × 37 | `g_pmoveVars` |
| 158 | f32 × 37 | the player's `m_pmoveVars` |

Order: `g_gravity pm_accelerate pm_airaccelerate pm_airstepsize
pm_boxcenter pm_boxwidth pm_crouchheight pm_crouchspeed pm_crouchviewheight
pm_edgefriction pm_friction pm_gravity pm_jumpheight pm_airjumps
pm_backspeed pm_groundspeedcap pm_jumpspeedcap pm_maxviewpitch
pm_minviewpitch pm_noclipaccelerate pm_noclipspeed pm_normalheight
pm_normalviewheight pm_nospeedcap pm_proneheight pm_pronespeed
pm_proneviewheight pm_runspeed pm_runfiring pm_stamina pm_staminarate
pm_staminathreshold pm_stepsize pm_stopspeed pm_walkspeed
pm_wateraccelerate pm_waterjumpheight` (Nuclide's `pmove.h`). When an
entity is 0 the world's fields are sent, as QC would read them, and PREDICT
is off; a game without `g_pmoveVars` gets the cvars of those names, and
PREDICT is off.

Sent once the device has the map, every 2 s, at once when a value changes
(the server compares every send pass), and again 0.2 s after the last one
while the device's `varsack` is behind.

### BRUSHINFO

Which brush entities collide, and how; ENTS can't tell (it leaves out what
the game doesn't network, and shows a `func_illusionary`).

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | session |
| 4 | u16 | spawncount |
| 6 | u16 | version (+1 when the list changes, and with each map; never 0) |
| 8 | u16 | entries in this version |
| 10 | u16 | the index of this packet's first entry |
| 12 | u8 | count: as many as fit in 1380 bytes |
| 13 | count × 18 | u16 edict, u16 model (the `n` of `*n`), u8 solid (4 or 22), s8 skin, f32 origin[3] |
| | count × 12 | f32 angles[3] of each entry: a turned brush is traced in its own frame (`sv_world.c` `brush_frame`) |
| | 1 | `nturned`: the entries with angles not all 0 |
| | nturned × 25 | u8 entry (in this packet), f32 absmin[3], f32 absmax[3] |
| | 2 | `ntouch`: the touches in this version |
| | 1 | the touches in this packet |
| | each × 44 | u16 edict, u8 kind (1 `trigger_push`, 2 `func_conveyor`), u8 flags, f32 speed, f32 dir[3], f32 absmin[3], f32 absmax[3] |

Every brush entity (model `*1` and up) with `solid` 4 (`SOLID_BSP`) or 22
(`SOLID_BSPTRIGGER`) that the server's traces test for this player (owner
and dimension rules), by edict, up to 512. `skin` is the entity's negative
`.skin` when it gives the whole model Quake contents (-2 solid, -3 water, -4
slime, -5 lava, -6 sky, -8 clip, -16 ladder), else 0. A `func_ladder` is two
entries with the same model: the ladder and a solid twin. All the packets of
a version go out together. Sent once the device has the map, every 2 s, at
once when the list changes (compared every 0.25 s: an entry added, gone or
changed, or a brush entity at rest whose origin or angles are not the ones in
the list, or a turned one whose box is not; a moving one doesn't count,
PSTATE's movers carry it), and again 0.2 s after the last one while
`brushack` is behind. A device that reads 18 bytes an entry stops before the
angles.

The touches are what touching an entity does to the player's movement,
which Nuclide runs inside the movement: each `trigger_push` that
`touchtriggers` would run (`solid` 1 or 22 with a `touch`; Nuclide makes
them 22) and each `func_conveyor` of the list (`PMoveCustom_DoTouch`), up to
64, by edict. `speed` and `dir` are the entity's `m_flSpeed` and
`m_vecMoveDir`; flags: 1 `TP_ONCE`, 2 the push has a target (its velocity
is the server's), 4 a straight up or down push (`m_pushAngle` ±90 0 0), 8 a
conveyor's `SF_CONVEYOR_VISUAL`. They come after the brushes, in the packets
that have room, and in packets with no entries after them when needed; the
version is complete when all entries and all `ntouch` touches have come. A
change in them is a new version.

The device drops a PSTATE, PMOVEVARS or BRUSHINFO whose spawncount is not
its map's.

### Weapon prediction

The device also predicts the weapon in hand, as the PC client does: after
each command's movement, Nuclide runs `ncPlayer::Physics_InputPostMove`
(the attack, reload and idle timers, the view model's time and the punch
angle, `src/shared/physics/player_pmove.qc`) and then
`ncPlayer::ProcessInput` (`src/shared/game/Player.qc`), whose weapon part
calls `ncWeapon`'s `PrimaryAttack`, `SecondaryAttack`, `Reload` and
`Release` (`src/shared/game/Weapon.qc`); `Physics_Fall` adds a punch on a
hard landing. These run on the client for its own player and pick the view
model's sequence, the muzzle flash (the view model's studio events), the
punch and the ammo the HUD shows. A device runs a C port of them
(`src/platform/nds/weapon_nds.c`) on each command; the values they read
come from the server, so nothing is coded per weapon.

**WEAPONINFO** is the weapon decl as `ncWeapon::UpdateFireInfoCache` reads
it, and its view model:

| Field | |
|---|---|
| u32 session, u16 spawncount, u16 edict, u16 version | the weapon entity and its version (> 0) |
| u16 viewmodel | model index (`ncWeapon::viewModel`) |
| u8 flags | 1 `altAlternates`, 2 `removeOnEmpty`, 4 `powerAmmo`, 8 `altMode`, 16 `buggyIdleAnim`, 32 has a `def_altFireInfo` |
| s8 infinite | `g_infiniteAmmo` |
| s8 primammo, secammo | `m_primaryAmmoType`, `m_secondaryAmmoType` (-1 none) |
| f32 primed_fuse | |
| f32 × 4 | the `falldamage` decl's `minFallDistance`, `minLandDistance`, `punchAngleScale`, `punchSpringScale` |
| u8 nfi | 1, or 2 with a secondary fireInfo |
| nfi × fireInfo | primary, then secondary |
| u8 nseq | the view model's sequences, up to 64; for a view model that isn't a studio model (a Quake `.mdl` with a `.framegroups` file, a skeletal model) its frame groups, which `frameduration` times as count / fps, with no events |
| nseq × {f32 duration, f32 fps, u16 frames, u8 loop, u8 nev, nev × {u16 frame, u16 event, u8 option}} | `frameduration` as the game gets it; the muzzle flash events (5001, 5011, 5021, 5031), the option as a number |

A fireInfo: u16 flags (1 `ammoRequired`, 2 `semiAuto`, 4 `fireUnderwater`,
8 `act_loop`, 16 `cocks`, 32 `drawAfterRelease`, 64 `reloadTimed`, 128 has
`def_onFire`, 256 has `def_onRelease`, 512 `detonateOnFire`, 1024
`serverFire`), s8 `ammoType` (`ammoNumForName`), s16 `ammoPerShot` (1 when
unset), s16 `depleteAmmo`, then 25 f32: `punchAngle` (3), `punchSpring` (3),
`punchKick`, `fireRate`, `chargeTime`, `overheatPerShot`, `overheatLength`,
`burstCount`, `burstTime`, `knockback`, `knockbackRelease`, `testDistance`,
`spinUpTime`, `reloadTime` (-1 when unset: the animation's length),
`reloadStartTime`, `zoomFov` / 90, `failRate`, the `def_onFire`'s
`sweepAngle` and `range`, `regenRate`, `depleteRate`; then u8 count and
that many activities {u8 activity, u8 n, n × s16 sequence}. Activities are
numbered `fire fireLast fireFailed fireStart fireStop reload reloadEmpty
reloadStart reloadEnd draw drawEmpty idle idleEmpty cock loop delay release
cooking detonate modeOn modeOff` (0..20); one with a list (`act_fire
"9,10"`) has its entries (up to 32), which `GetSubDefAct` picks among by
`input_sequence`. It picks among all of them, so a device that holds
fewer (the DS: 8) leaves that weapon to the server.

The server reads it with the game's own functions (`ncIO::GetSubDefString`,
`GetSubDefAct`, `ammoNumForName`, `EntityDef_GetKeyValue`) whenever the
weapon in hand changes and once a second, and gives it a new version when it
differs (or the edict holds a weapon of another class). It sends it only to a
device whose INPUT carries the `'W'` trailer (version 0 while it has none),
every 0.25 s until the device's INPUT acknowledges that version for that
edict, and after 8 sends every 2 s. A game without these functions or fields,
or a body over 1200 bytes, gets no WEAPONINFO, and the device shows the
server's.

**The weapon block** follows PSTATE's boxes:

| Offset | Type | Field |
|---|---|---|
| 0 | u8, u8 | `'W'`, 1 |
| 2 | u16 | the weapon in hand (`m_activeWeapon`), 0 none |
| 4 | u16 | the WEAPONINFO version that describes it now, 0 none yet |
| 6 | u8 | `m_weaponState` |
| 7 | u8 | `m_weaponLastFireInfo`: 0 primary, 1 secondary, 255 another |
| 8 | u8 | 1 `m_weaponIsFiring`, 2 `m_fiWillRelease` |
| 9 | u8 | `nammo`, 0..4 |
| 10 | s16 × 2 | `m_weaponClip`, `m_weaponClipSize` |
| 14 | f32 × 9 | `m_weaponBurstCount`, `m_weaponFireInfoValue`, `m_weaponOverheating`, `m_fiOverheatLength`, `m_reloadStepLength`, `m_weaponSpin`, the player's `m_timeUntilNextAttack`, `m_timeUntilNextIdle`, `m_timeUntilReloaded` |
| 50 | s16 | `m_weaponAnimSequence` |
| 52 | f32 | `m_weaponAnimTime` |
| 56 | f32 × 6 | `m_punchAngle`, `m_punchVelocity` |
| 80 | f32 | the `*fireScale` userinfo, 1 without |
| 84 | nammo × {u8 type, s16 count} | `ncActor::m_ammoTypes` of the primary, secondary and fireInfo types |

The device keeps a weapon state for each command, as for the movement:
its prediction for `ack` must match this block (the timers, the anim time
and the clocks within 1/1000, the punch within 0.01 degree, the rest
exactly) and the movement both, or it runs again the commands after `ack`
from PSTATE's state and this block together. `VFL_FIRING`,
`VFL_PRIMEDFUSE`, `VFL_HOLDING` (`m_nuclideFlags`) and `GF_SEMI_TOGGLED`
(`gflags`) are the weapon code's; the movement reads them from the weapon
state. What only the server does is not predicted: projectiles, damage,
sounds, the clip a reload fills (`_ReloadFinished` is a think), a weapon
switch, timed ammo regen; the next PSTATE brings it.

### HUDINFO

The PC's co-op HUD (`coop/src/hud/jtf.qc` `Jtf_Post` and its Panorama
layout, `coop/panorama/layout/coop/hud.xml` and `scripts/coop/hud.js`)
reads infokeys the rules set for each player
(`coop/src/rules/hud_info.qc`), serverinfo keys, the weapon in hand, the
other players' userinfo, the friendly NPCs near it
(`src/client/nameplates.qc`) and the obituaries (`EV_OBITUARY`,
`HUD_Obituary`). A link player is a bot to the networking and gets none of
it, so the server puts it in one packet for each device whose INPUT carries
the `'H'` trailer:

| Field | |
|---|---|
| u32 session, u16 version | the version (> 0) is new whenever the body differs |
| u8 self | the player's slot (its edict - 1) |
| u8 nkeys, nkeys × {u8 id, u8 len, char text[len]} | the keys that are set (below) |
| u8 nplayers, nplayers × {u8 slot, s16 health, u8 len, char name[len], u8 len, char mate[len], u8 cadence, u8 len, char team[len]} | every client with a name, this one too: health 0 while not spawned, `*mate`, `*cadence`, `*team` |
| u8 nnpc, nnpc × {u16 entnum, s16 health, s16 max_health, u8 len, char netname[len]} | up to 8 monsters with `FL_MONSTER`, health over 0 and `m_iAlliance` `MAL_FRIEND` (0) within 2000 units |
| u8 w, u8 h, then if w: u16 rgb15, u8 mask[(w × h + 7) / 8] | the weapon's crosshair sprite at its size in a 640-wide HUD, a bit for each pixel drawn (row by row, bit 0 first), in its most common colour; 0 × 0 none |
| u8 obitseq, u8 n, n × 4 × {u8 len, char text[len]} | the newest obituaries, oldest first, each the target, the attacker, the weapon's `inv_name` and the means; `obitseq` counts them all (mod 256) |

Key ids: 0 `*herohud`, 1 `*respawn`, 2 `*revive`, 3 `*prompt`, 4
`*promptAt`, 5 `*usable`, 6 `*stats`, 7 `*xpm`, 8 `*cadence`, 9
`*cadenceN` (the player's infokeys); 16 `coop_wave`, 17 `coop_left`, 18
`coop_notice`, 19 `coop_feed`, 20 `coop_engageCost`, 21 `nameplates`, 22
`targetid` (serverinfo); 32 the weapon's `ncWeapon::m_weaponTitle`, 33 and
34 the `inv_name` of `*herohud`'s static and dynamic weapons (fields 8 and
9), 35 `"1"` when the weapon needs no ammo (`!fiAmmoRequired`). A key over
255 bytes is cut; keys stop before the body passes 900 bytes, and the body
is at most 1300.

The crosshair is the one `HUD_WeaponSwitched` (`valve/src/hud/hud.qc`)
loads: the weapon decl's `crosshair` key, or `<class>.crosshair`; a name
`a.b` is entry `b` of `sprites/a.txt`, one without a dot an entry of
`sprites/hud.txt`, at resolution 640, as `InterfaceSpriteSheet.qc` reads
them. The sprite is Half-Life's (`IDSP` version 2); a pixel counts by its
format: alphatest not index 255, index-alpha (colour 255) index 128 and
up, additive a colour whose channels add to 192 or more, normal every
pixel. Over
32 × 32, or a missing file, sends none.

The server builds the body on each `VIEW` (20 a second) and sends a new
version at once, then every 0.25 s until the device's INPUT acknowledges
it. The first `'H'` sets the player's `phud` userinfo to 1, as the PC's
Panorama HUD does (`jtf.qc` `Jtf_SetPhud`), so the rules leave out the
centerprints and text lines that HUD replaces. Obituaries are taken from
the multicasts that reach the device (`EV_OBITUARY`, 25); the weapon decl
they name is read for its `inv_name` when the body is built, not in the
multicast.

### The movement log

With `sv_link_log 1` the server prints, for every command a v2 player runs,
lines a host tool (`tools/nds/pmtest`) replays the movement from. Floats are
C99 hex (`%a`), so the bits are exact; `<slot>` is the link's slot.

```
LINKPM <slot> map <mapname> <spawncount>                          (a new map, and when the log starts)
LINKPM <slot> vars <74 floats: g_pmoveVars, then m_pmoveVars>       (when they change, and when the log starts)
LINKPM <slot> brush <n> {<edict> <model> <solid> <skin> <ox> <oy> <oz>}...   (when the list changes)
LINKPM <slot> brushpos <n> {<edict> <ox> <oy> <oz>}...             (entries of the last brush line somewhere else now)
LINKPM <slot> brushang <n> {<edict> <pitch> <yaw> <roll>}...        (the turned entries, the rest 0; while any are turned, and once after)
LINKPM <slot> brushabs <n> {<edict> <absmin xyz> <absmax xyz>}...   (the turned entries' boxes, after brushang)
LINKPM <slot> touch <n> {<edict> <kind> <flags> <speed> <dir xyz> <absmin xyz> <absmax xyz>}...   (BRUSHINFO's touches, when they change)
LINKPM <slot> boxes <n> {<edict> <x0> <y0> <z0> <x1> <y1> <z1> <solid>}...   (as PSTATE's, up to 64)
LINKPM <slot> pre <state>
LINKPM <slot> cmd <seq> <msec> <pitch> <yaw> <fwd> <side> <up> <buttons> <impulse> <ran_msec> <ran_impulse>
LINKPM <slot> post <state>
```

```
LINKPM <slot> winfo <edict> <version> <WEAPONINFO body in hex>     (each new version)
LINKPM <slot> wpre <weapon>
LINKPM <slot> wpost <weapon>
```

`brush` to `pre` come just before the command runs, `cmd` and `post` just
after; `wpre` after `pre`, `wpost` after `post`. `<weapon>` is the weapon
block's fields: `edict version state fireinfo flags clip clipsize animseq`,
17 floats (`burst fiv overheat overheatlen reloadstep spin atk idle rel
animtime punch[3] punchvel[3] firescale`), `m_nuclideFlags gflags nammo`,
nammo × `type count`, then the downforce if this command landed (-1 if
not; 0 in `wpre`), health and waterlevel. `tools/nds/weptest` replays
them through the DS's weapon code. In `cmd`, `msec`, `pitch`, `yaw` and `impulse` are as the device sent
them; `fwd side up buttons` are what ran (after the admin filters),
`ran_msec` the time that ran (after the movement budget) and `ran_impulse`
the impulse that ran (`forced_impulse` included). `<state>` is PSTATE's
fields from `origin` to `health`: 21 floats (`origin` to `maxs`), `flags
m_nuclideFlags gflags` as unsigned, `movetype solid waterlevel watertype
groundentity` as integers, 13 floats (`gravity` to `viewzoom`, then
health), then the status bits 2-4 shifted down (1 autojump, 2 long jump, 4
HL rules).

## Events

A PC client shows more than the world: prints and chat, centerprints, and
what the game sends its CSQC as `SVC_CGAMEPACKET` events (Nuclide's
`src/client/event.qc`, `Event_Parse`). A link player is a bot to the
engine, so none of it reaches it as messages. The engine hands it to
`sv_link.c` instead: `sprint`, `bprint` and the server's `say` (svc_print),
`centerprint`, the game's multicasts, `MSG_ALL` and `MSG_BROADCAST` writes,
and its `MSG_ONE` writes, which are read from the client's QC reliable buffer
(nothing else empties it for a bot). Once a frame, outside the game's code,
those become records, numbered from 1 for each device. `EVENT` carries the
ones the device has not acknowledged (INPUT's `'E'`), oldest first, as many
as fit, once the oldest of them has waited 0.2 s since it last went out.
New records go out the frame they are made in an `EVENT` of their own,
with the top bit of `count` set: the records before its `first` are still on
their way, and a device that lacks one of them drops that packet and waits
for the resend.
The server keeps 64; when a device falls further behind, the oldest go, and
the next `EVENT` starts past the device's last: the device takes on from
there. `sv_link_log 1` prints each record made as a `LINKEV` line, and
`link_stats` counts them.

`EVENT`: u32 session, u16 `first` (the first record's number), u8 `count`
(bit 7: new records only, as above),
then `count` records of u8 type, u8 length, payload. Strings run to the end
of the record, with the game's colour codes (`^1`, `^xRGB`) for the device to
draw; titles.txt escapes become what `Titles_ParseFunString` makes of them
(`\y` `^3`, `\n` a new line), and a tab a space.

| Type | Record | From | Payload |
|---|---|---|---|
| 1 | PRINT | svc_print; `EV_CHAT`, `EV_CHAT_TEAM` (as `Util_ChatFormat` words them), `EV_HUDHINT` ("Hint: ", as a chat line) | u8 level (3 chat), text without its last new line |
| 2 | CENTER | `centerprint` | text |
| 3 | SHAKE | `EV_SHAKE` (none for a player whose class is `spectator`) | f32 duration, amplitude, frequency |
| 4 | FADE | `EV_FADE`; serverinfo `startdark` when the device has the map (WorldSpawn_Init's Fade_StartDark: black, 5 s, 4 s held, fading from) | u8 r, g, b, alpha (0-255), f32 duration, hold, u8 flags (1 fade from, 2 modulate) |
| 5 | DAMAGE | `EV_DAMAGE` | s16 origin[3] (units: the attacker's, or ours when the attacker is at the world's origin), s16 damage, u32 damage flags |
| 6 | TEXT | `EV_TEXT`, `EV_TEXT_STRING` (game_text); `EV_MESSAGE` (env_message) and serverinfo `chaptertitle` when the device has the map (WorldSpawn_Init): the name uppercased and looked up in titles.txt (then `titles_<game>.txt`) as `src/client/titles.qc` reads it, that entry's text, place, effect, colours and times (fx time 0, which GameMessage_Setup leaves as the queue's slot had it), or the name itself with GameMessage_Setup's defaults; channel 255, the queue | u8 channel, f32 x, y, u8 effect, u8 colour1[3], colour2[3], f32 fade in, fade out, hold, fx time, text |
| 7 | FEED | `EV_OBITUARY` for a player's death (as `coop/src/hud/jtf.qc HUD_Obituary` passes it to the Panorama feed: no attacker when the means of death says `self`, the weapon decl's `inv_name`); serverinfo `coop_feed` changes (team events) | u8 about us, u8 length, attacker (or the victim, with weapon "died"), u8 length, weapon, victim |
| 8 | PICKUP | `EV_PICKUP` | the item's class (`EntityDef_NameFromNetID`) |
| 9 | TEXTAGAIN | a TEXT on channel 0-4 the same as that channel's last | u8 channel: show it again from its start |
| 10 | CENTERAGAIN | a `centerprint` the same as the last (the co-op rules send "Respawn in N seconds" every frame while down); while one has not gone out yet, more make none | nothing: the centerprint's 3 s start again |
| 11 | FLASH | `EV_PICKUP` of an item whose entityDef has `pickupFlash 1` (EV_Pickup's `bf`) | nothing: the engine's gold pickup flash |
| 12 | SOUND | a sound that starts a loop or ends one the device has ("Sound") | a SOUND record with `seq` 0 |
| 13 | SENTENCE | `EV_SENTENCE` ("Sound") | u16 entity, u8 attenuation (×64), u8 n, n × u32 key |
| 14 | MUSIC | `EV_MUSIC*`, serverinfo `bgm`, a `music`/`stopmusic` stuffcmd ("Sound") | u8 op (0 stop, 1 once, 2 loop), the name as the PC's `music` command takes it |
| 15 | PRECACHE | the map's sound precache list ("Sound") | u16 first, u8 n, n × u32 key |

A device that also sends `'H'` gets no FEED records: the obituaries and
`coop_feed` reach it in `HUDINFO`, whose feed it draws.

`EV_ANGLE` from a `MSG_ONE` write turns the player as the multicast does.
The sound events (`EV_MUSICTRACK`, `EV_MUSICLOOP`, `EV_MUSIC_ONCE`,
`EV_MUSIC_LOOP`, `EV_MUSIC_STOP`, `EV_SPEAK`, `EV_SENTENCE`) are read to
their end for every device and make records only for one that takes sound.
`EV_HITNOTIFY` (the PC does nothing with it) and events of other kinds
(intermission, models) make no records; an event whose
length the server doesn't know ends that message (`link_stats` counts
these as stops; `sv_link_log 1` prints each). The game's `MSG_ONE` and
`MSG_ALL` writes are taken as they are written, so they keep their order
against prints and multicasts. When the server drops records (a full ring),
or the map changes, the next game_text and centerprint go out in full.

### Screen commands

| Byte | Command | Arguments |
|---|---|---|
| 0x01 | CLEAR | u16 rgb15 |
| 0x02 | FILL | u8 x, u8 y, u8 w, u8 h, u16 rgb15 (w and h of 0 mean 256) |
| 0x03 | TEXT | u8 x, u8 y, u16 rgb15, u8 len, char text[len] |

## Sound

A PC client hears the server's `sound()` and `pointsound()` as `svc_sound`
(`src/server/sv_client.c` `write_sound`), which the engine sends to every
spawned client with no PVS test, and never to a bot. For a link player the
same call makes a record here instead (`SV_LinkSound`). The PC's client game
also makes sounds by itself from what it is sent; a device runs no QuakeC, so
the server works those out as the client game would and sends them as
`CLIENTSIDE` records:

| Path | What the PC's client game does | Sent as |
|---|---|---|
| impacts | `EV_SURFIMPACT`/`EV_SURFIMPACTID` (`surfaceproperties.qc` `SurfData_Impact_Parse`): the surface near the point on entity number `e - 1` (world for 0 and 1), its material's `SURFDATA_SND_BULLETIMPACT` SoundDef, `SoundDef_PlayAt` (pitch 100: the client's `pointsound` drops it; the decl's `distshader` too, at attenuation 0) | SOUND, entity 0 |
| speech | `EV_SPEAK`: `sound(t, CHAN_VOICE, sample, 1, ATTN_NORM, pitch)` | SOUND on the entity |
| sentences | `EV_SENTENCE`: the client's `sound/sentences.txt` table (`sentences.qc`), each word + `.wav`, the last path seen (`vox` to start); only for a `speaker`, `ambient_generic` (attenuation its radius) or talking monster | EVENT 13 |
| env_message | `EV_MESSAGE`'s sound, `CHAN_ITEM` | SOUND, entity 0, at the player's eye |
| music | `EV_MUSIC_ONCE`/`EV_MUSIC_LOOP` (a `*M:` model name; "once" loops too, as the PC's `music X 0`), `EV_MUSIC_STOP`, serverinfo `bgm` (`sound/bgm/<x>.mp3`); the CD tracks (`EV_MUSICTRACK`, `EV_MUSICLOOP`) only when the game's `menu.dat` has `menu_musictrack`, which co-op's lacks (the PC stays silent) | EVENT 14 |
| stuffcmd | `play`, `play2`, `playvol` (a local sound at the listener), `music`, `stopmusic` | SOUND entity 0 at the eye; EVENT 14 |
| stuffcmd | `selectItem <weapon>` (the PC's client switches through its next command's `input_cursor_entitynumber`; the co-op heroes use it after a floor gun is taken) | the server finds the weapon the player owns by classname and sets it as the next link command's cursor entity |
| view model | `HandleAnimEvent` 1004, 1008, 5004 on the view model | WEAPONINFO's `'S'` table |
| model events | 1004, 1008, 5004 of the other studio entities, played as the client draws them | SOUND with `AT` |
| loops | looping `ambient_generic` (not a SoundDef one) and `ncSound` entities | AMBIENT |

**Sample names** are a key: FNV-1a 32 of the name in lower case, `\` as
`/`, without a leading `./` or one leading `sound/`, and without a trailing
`.wav`, `.ogg` or `.mp3`. `misc/talk.wav` is `0x7c72dd3c`,
`common/null.wav` `0x7e3eafd0`, `Sound\Weapons\Glock1.WAV` `0xf61c39de`.

**SOUND**: u32 session, u16 spawncount, u8 count, then count records of 20
bytes (22 with `AT`):

| Field | |
|---|---|
| u16 seq | +1 each record for this device; 0 in an EVENT 12 |
| u8 lflags | 1 LISTENER (the device's own player, a server sound: heard straight), 2 `AT` follows, 4 STOP (the sample is `common/null`: stop entity and channel), 8 CLIENTSIDE (made for the PC client game's own sounds: a client entity's channel), bits 4-6 (0x70) DELAY: made this many 4 ms after the packet's first new record (0-7; 0 in a repeat, with `AT` and in EVENT 12), 128 ATTEN16 (the attenuation byte is ×16: only CLIENTSIDE records, when ×64 would pass 255) |
| u8 sflags | the engine's sound flags as `svc_sound` has them (2 FORCELOOP, 4 NOSPACIALISE, 16 ABSVOLUME, 32 NOREVERB, 64 FOLLOW, 128 NOREPLACE) |
| u16 entity | 0 none |
| u8 channel | |
| u32 key | the sample |
| u8 volume, attenuation, pitch | ×255, ×64 (×16 with ATTEN16), percent (0 means 100), as `svc_sound` |
| s16 origin[3] | units (the entity's origin and the middle of its box, as `svc_sound`) |
| s16 at | with `AT`: the server time (ms, the low 16 bits of the `ENTS` clock) at which the PC would start it, less the SoundDef's start offset |

A SOUND packet goes at most every 1/36 s while records wait, so it can
carry two server frames' records; the DELAY bits let the device start each
as late after the first as it was made, which is when the PC would. Each
record goes in two consecutive packets (the device drops a seq it has had,
and one 64 or more behind the newest): a packet holds the previous one's
records first, then its new ones in the order they were made. At most 40 a
packet: the farthest from the player go first (`link_stats` counts them).
Two records on one entity's channel (not 0) in the same server frame: only
the later goes (with NOREPLACE, the earlier), which is all the PC hears of
them (`pick_channel` replaces the first before it is mixed). A sound that loops (the FORCELOOP flag, or a WAV with a `cue `
chunk anywhere in the file, which the PC loops), and a stop or start on the
entity and channel (not 0) of a loop the device was sent, go as EVENT 12
instead, which is reliable and in order; records on the same entity and
channel still waiting to go unreliably (or to be repeated) are dropped,
since they would reach the device after it and replace it, where the PC
hears them before. The server keeps the last 32 loops
per device; a device joining late misses loops started before, as a PC does.

**AMBIENT**: u32 session, u16 spawncount, u16 version, u8 count, count ×
{u16 entity, u32 key, u8 volume (×255), u8 attenuation (×64, or ×16 with
flag 1), u8 pitch, u8 flags (1 ATTEN16: the attenuation is ×16, set when ×64
would pass 255; 2 FORCELOOP, 128 an `ncSound`, else an `ambient_generic`), s16
origin[3]}: the whole set, the 64 nearest the player. An `ambient_generic`
(or an entityDef whose `spawnclass` is one) is in it while it loops and is on
and is not a SoundDef one (`ambient_generic::SendEntity`); its radius is the
whole byte the client gets. An `ncSound` while it loops and the player is
within its radius (attenuation 1000 / radius). Read at the VIEW rate; a new
version when it changes, sent then and every 0.25 s until INPUT's `'S'`
acknowledges it, then every 2 s.

**EVENT 15 PRECACHE**: once the device has the map, the sound precache list
in order (`first` its index, up to 54 keys a record), and sounds precached
later as they come; then one record with `first` 0xf000: the names the device
keeps itself (`misc/talk`, `common/null`, `common/wpn_hudon`,
`common/wpn_hudoff`, `common/wpn_moveselect`).

**WEAPONINFO** for a device that plays sound: each sequence's event list also
holds its 1004, 1008 and 5004 events (after the muzzle flashes, which keep
their places; at most 64 events in all), whose `option` is an index into a
table after the sequences: u8 `'S'`, u8 n, n × {u8 nsamples (1..8), nsamples ×
u32 key, u8 volume (×255), u8 pitch min, pitch max (percent), u8 attenuation
(×64), u8 flags (as SOUND's sflags), u8 channel, u8 distshader (another
entry's index, 0xff none), s16 offset (ms)}, then u8 release[2]: the entry of
the primary and secondary fireInfo's `snd_release` SoundDef (0xff none). 1004
and 1008 play their file on channel 4 and 2 when it exists; 5004 its file on
channel 0, else its SoundDef on channel 4 (a footstep one is silent on a view
model). A body over 1200 bytes goes without its sound events.

`sv_link_log 1` prints a `LINKSND <slot> <seq> <path> <name> entity channel
volume attenuation pitch flags x y z [at]` line for each record (`flags` is
sflags + 256 × lflags; seq 0: an EVENT, AMBIENT or WEAPONINFO entry),
`SVSND <index> <name> entity channel` for each `svc_sound` the engine writes
while a device takes sound (what a PC gets), and `LINKSNDUSED <map> <name>`
for each sound the map precaches and each sample of the SoundDefs the server
read. `tools/nds/linktest_sound.py` is a fake device that checks them.

The DS plays these records with `src/platform/nds/sound_nds.c`, from packs made
by `tools/nds/sndpack.py`; how, where it differs from the PC, and what was
measured are in `docs/platforms/nds.md`, "Sound packs" and "Sound: what the
PC client hears". Matching a device's `SND` log (`snd_log=1`) to `LINKSND` by
seq accounts for every unreliable record; `LINKSNDGONE <slot> <seq>
sameframe|overtaken` names one the server dropped after logging it, and
`LINKSNDSAME` a NOREPLACE one it never numbered.

## On the DS

The DS build (`bulwark_nds.nds`) offers *[Join a game over WiFi]* at the top
of its menu. It joins
WiFi in one of two ways:

- with the network saved in the DS's own settings (Nintendo WFC settings);
- with `link.txt` in a base folder, such as `/bulwark/link.txt` on the SD card:

  ```
  ssid=MyNetwork
  key=passphrase   (WPA/WPA2 passphrase, or a WEP key in hex)
  security=wpa2    (open, wep, wpa or wpa2)
  server=192.168.1.20:27901   (optional: skips discovery)
  name=Player      (optional)
  autojoin=1       (optional: join the first server that answers, at start-up)
  ```

Then it broadcasts `DISCOVER`, lists the servers that answer, and joins the
one you pick. The top screen draws the map from the player's position
(Quake-format maps only, for now). The bottom screen shows the private
screen. Touches on it go to the game.

DS controls while linked:

| Button | Action |
|---|---|
| D-pad up / down | move |
| D-pad left / right | turn |
| L + D-pad | strafe, and look up / down |
| A, B, X, Y, R | game buttons (see the mapping above) |
| touch | goes to the game's private screen |
| START | leave |
