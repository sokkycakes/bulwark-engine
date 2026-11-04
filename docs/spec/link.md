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

A link player takes a normal client slot and edict. The game sees it as a
real player: `clienttype` is `CLIENTTYPE_REAL`, `ClientConnect` and
`PutClientInServer` run, and its input reaches the game through the usual
`input_*` globals in `SV_RunClientCommand`. The `*link` info key is `1`.
The engine sends it no entity snapshots, and between packets it repeats
the last command, as it does for bots.

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
| X | 0x20 (use) |
| Y | 0x08 (reload) |
| R | 0x10 (secondary attack) |

These are Nuclide's `INPUT_BUTTON*` bits. The impulse goes into the move
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
| 0x02 | JOIN | u32 session, u16 device (1 = DS), char name[16] |
| 0x03 | INPUT | u32 session, u16 seq, u16 keys, u8 touch (bit 0 down), u8 tx, u8 ty, s8 move[3], s16 pitch, s16 yaw, u16 screen_ack, then optionally u8 fps, u8 min_fps, u16 peak_polys (the device's last second), u16 load_ms (its last map load, in 10 ms steps), u16 spawncount (the map it has loaded), u16 buttons (the game's buttons, as the device mapped them), u8 impulse, u8 impulse_count |
| 0x04 | LEAVE | u32 session |

Angles use 65536 per turn, and positive pitch looks up.

### Server to device

| Type | Name | Payload |
|---|---|---|
| 0x81 | INFO | char host[32], char map[32], u8 players, u8 maxplayers, u8 link_free |
| 0x82 | WELCOME | u32 session, u8 playernum, u16 spawncount, char map[64] |
| 0x83 | REFUSE | char reason[64] |
| 0x84 | SCREEN | u32 session, u16 version, then draw commands until the end of the packet |
| 0x85 | VIEW | u32 session, u16 seq, s32 origin[3] (1/8 units), s16 pitch, s16 yaw, s16 health, u8 flags, u8 count, then count × {s16 org[3] (units), u8 kind, u16 rgb15} |
| 0x86 | KICK | u32 session, char reason[64] |
| 0x87 | ENTS | u32 session, u16 seq, u16 nummodels, u8 shown[(nummodels + 7) / 8], u8 moved, moved × {u16 model, s16 origin[3] (1/4 units), s16 angles[3]}, u8 things, u8 thing_size (16), things × {s16 org[3] (units, the bottom middle), u8 yaw (256 a turn), u8 half_width, u8 height, u8 kind, u16 entnum, u16 model, u8 frame (the sequence), u8 lift (how far the model's origin sits above the bottom)} |
| 0x88 | MODELS | u32 session, u16 spawncount, u8 count, count × {u16 model, u8 len, char name[len]} |

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
  eases toward each update.
- `things` are the nearest other entities with a model (at most 48 within
  2048 units), including the other players. Kind 1 is a monster
  (`FL_MONSTER`), 2 something solid, 3 something not solid, 4 a dead
  monster. `thing_size` lets a later version add fields. `model` is the
  entity's model index when it is a studio model (`.mdl`), or 1000 plus the
  slot for a player, whose model is `models/player/<m>/<m>.mdl` from their
  `model` userinfo (or `models/player.mdl`); 0 for none. A device that has
  a DS copy of that model (tools/nds/hlmdl.py) draws it playing `frame`'s
  sequence, and otherwise a box.

`MODELS` names the model indexes in `things`, a few at a time with every
fifth `ENTS`, until the device has had them all for this map.

A device ignores an `ENTS` older than the last one it used.

`link_teleport x y z [yaw] [name]` (with `sv_cheats 1`) puts every link
player, or the one named, at a point, for testing a map's triggers and
level changes. `link_god [0|1]` (with `sv_cheats 1`) makes the link players
take no damage, for leaving them in a fight while measuring. `link_stats`
lists the link players with the frame rate their devices report.

### Screen commands

| Byte | Command | Arguments |
|---|---|---|
| 0x01 | CLEAR | u16 rgb15 |
| 0x02 | FILL | u8 x, u8 y, u8 w, u8 h, u16 rgb15 (w and h of 0 mean 256) |
| 0x03 | TEXT | u8 x, u8 y, u16 rgb15, u8 len, char text[len] |

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
