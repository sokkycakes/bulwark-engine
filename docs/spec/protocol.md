# Network protocol

Bulwark's own protocol, version 1 (`PROTOCOL_VERSION` in
`src/net/protocol.h`). It is not wire compatible with QuakeWorld, NetQuake or
FTE; the message numbers and layouts below are Bulwark's. All numbers are
little-endian. `varuint` is an unsigned LEB128 value (7 bits a byte, low
first). `string` is NUL terminated. `coord` and `angle16` are as `MSG_*` in
`src/net/msg.c` write them.

## Transports

- **UDP**: a dual-stack IPv6 socket where the system has one, otherwise IPv4.
- **Loopback** for the listen server's own client.
- **TCP** for `tcp://host:port` addresses (`connecttcp`, `tcpconnect`).
  Each packet travels as a 2-byte big-endian length then the packet, so
  everything above the transport is the same as over UDP. A server opens
  its TCP port with `sv_port_tcp`. Streams buffer at most 256 KB of
  unsent data before the connection is dropped.

## Connectionless packets

Four bytes of `0xff`, then text, as in Quake:

1. client: `getchallenge`
2. server: `challenge <n>`
3. client: `connect <protocol> <qport> <challenge> "<userinfo>"`
4. server: `accept <spawncount>` or `reject <reason>`

Server queries (`getinfo`, `getstatus`) and master server traffic use the
same framing, in the Quake III / DarkPlaces text forms so ordinary master
servers and browsers can list Bulwark servers.

## Channel

Every packet after `accept` starts with:

| field | size |
|---|---|
| sequence | int32 |
| last sequence received from the peer | int32 |
| 32 bits: which of the 32 sequences before that arrived | int32 |
| next reliable fragment expected from the peer | int32 |
| qport (client to server only) | int16 |
| flags (bit 0: reliable fragments follow) | byte |

With the reliable flag, a byte count of fragments follows, then each
fragment as int32 fragment sequence, uint16 length and the bytes. Reliable
data is one ordered stream cut into fragments of up to 1024 bytes, with up
to 256 in flight; a fragment is resent after 1.5 round trips plus 30 ms
without an ack. The rest of the packet is unreliable message data.

The ack bits tell the sender exactly which of its packets were lost, which
the server uses to resend lost entity state instead of waiting for a full
update (the same idea as Source's).

## Server to client

| # | message | body |
|---|---|---|
| 0 | `svc_bad` | |
| 1 | `svc_nop` | |
| 2 | `svc_disconnect` | string reason |
| 3 | `svc_print` | byte level, string |
| 4 | `svc_centerprint` | string |
| 5 | `svc_stufftext` | string, run as console text at `cmd_serverlevel` (see console.md, "Who may run what") |
| 6 | `svc_serverdata` | see below |
| 7 | `svc_precache` | byte kind (0 model, 1 sound, 2 particle effect), varuint index, string name |
| 8 | `svc_lightstyle` | byte index, string |
| 9 | `svc_serverinfo` | string key, string value |
| 10 | `svc_playerinfo` | byte slot, string key, string value |
| 11 | `svc_signon` | byte stage |
| 12 | `svc_snapshot` | see below |
| 13 | `svc_qcmessage` | varuint length, bytes the server game wrote with `MSG_ONE`/`MSG_ALL`/`MSG_MULTICAST`; a first byte of 83 goes to `CSQC_Parse_Event` |
| 14 | `svc_setpause` | byte paused |
| 15 | `svc_sound` | entity, byte channel, varuint sound index, byte volume ×255, byte attenuation ×64, byte pitch, byte flags, coord ×3 origin |
| 16 | `svc_setangle` | angle16 ×3 |
| 17 | `svc_download` | see Downloads |
| 18 | `svc_pointparticles` | varuint effect, coord ×3 origin, float ×3 direction, float count |
| 19 | `svc_trailparticles` | varuint effect, entity, coord ×3 start, coord ×3 end |
| 20 | `svc_playerblob` | byte slot, string key, varuint length, bytes |
| 21 | `svc_stats` | varuint count, then per stat byte index, byte type (2 float, 1 string, 8 integer) and the value |
| 22 | `svc_voice` | byte sender slot, then a `clc_voice` body |
| 23 | `svc_voxel` | byte subcommand, see voxel.md |
| 24 | `svc_flash` | coord ×3 origin, float radius, float lifetime, float ×3 colour: a light that shrinks to nothing over its lifetime (`te_customflash`) |

### Prints and player names

A print sent to every player (QC `bprint`, chat, `say` from the server
console, "game paused") goes out as `svc_print`. A dedicated server also
prints its own copy on its console. A listen server keeps its own copy out
of the console while the host's loopback client is in the game, since that
client prints the `svc_print` into the same console: the host sees each line
once, as on FTE. As on FTE, the server copy still goes to stdout and the
console log, so a listen host's terminal and log may show a line twice (the
server's copy and the client's).

A player's name lives in the `name` user info key. Every change to it, the
player's own (`setinfo name`, the `name` command) or the game's
(`forceinfokey`, as Nuclide names its bots after their profiles), also
rewrites the player entity's `.netname`, which is what the game reads for
the HUD, obituaries and its serverinfo keys. A rename the player asked for
is announced once they are in the game ("X changed their name to Y"), as
FTE does; a rename by the game is not. As on FTE, a player who has a name
may not change it while muted or stealthmuted ("Muted players may not change
their names", not shown to a stealthmuted player), and a seventh rename
within five seconds of the first is refused and mutes them for five minutes
("Muted for name spam"; not counted while paused). Renames by the game are
never refused.

### serverdata

varuint protocol, int32 spawn count, float tick rate, byte max clients,
byte the client's slot, entity the client's player entity, string map name,
string world model path (`maps/<name>.bsp`), string client game progs name,
uint16 its CRC, int32 its size, string serverinfo.

### snapshot

int32 server tick, double server time, int32 the last client command the
server ran. Then entity updates until a varuint 0: a varuint of
`(entity + 1) << 1 | remove`. A removal has nothing more. An update has a
varuint length and that many bytes written by the entity's `SendEntity`,
which the client game reads in `CSQC_Ent_Update`.

## Client to server

| # | message | body |
|---|---|---|
| 0 | `clc_bad` | |
| 1 | `clc_nop` | |
| 2 | `clc_disconnect` | |
| 3 | `clc_move` | see below |
| 4 | `clc_stringcmd` | string |
| 5 | `clc_event` | string function, then typed arguments (`sendevent`) |
| 6 | `clc_userblob` | string key, varuint length, bytes (`setinfoblob`) |
| 7 | `clc_upload` | see Uploads |
| 8 | `clc_voice` | see Voice |

### move

byte count (1 to 9), int32 the server time the client was looking at in
milliseconds (for lag compensation), double client time, then `count`
commands, oldest first (the newest last), each a delta from the one before it (the first
from zeros): int32 sequence, byte milliseconds, byte field bits, then by
bit: 1 angles (angle ×3), 2 movement (short ×3), 4 buttons (varuint),
8 impulse (byte, otherwise 0), 16 weapon (varuint), 32 cursor (float ×2
screen position, float ×3 start, float ×3 end, entity). The extra commands
repeat earlier ones so a lost packet loses no input; the server skips any
it has already run. The client keeps numbering its commands across a level
change, so the first `clc_move` a client (or a split screen seat) sends
after it spawns on a level still carries commands from the previous level
or from before the spawn: the server runs only that packet's newest
command and takes its sequence as the last one run, as FTE discards old
moves across a level change. Replaying them would rerun the old map's
movement, buttons, impulses and cursor entity on the new one.

## Downloads

`svc_download` is a byte op then:

- 0 START: string name, int32 size
- 1 DATA: int32 offset, int16 length, bytes
- 2 DONE: string SHA-256 of the file, in hex
- 3 REFUSED: string name, string reason

The client asks with the `download <name>` string command. Files go to the
writable game directory under their own name once the hash matches;
`sv_allowdownload`, `sv_allowdownload_packages` and
`sv_allowdownload_csprogs` say what a server hands out. The client refuses
names that could run as code (configs, libraries, programs, packages,
`menu.dat`, `progs.dat`).

## Uploads

`clc_upload` carries `snap` screenshots and other files the server asks for:
a byte op then 0 START (string name, int32 size), 1 DATA (int32 offset,
int16 length, bytes), 2 WINDOW (the client waits for a `nextul` command
before sending more) or 3 DONE.

## Voice

A `clc_voice` body is: byte codec, byte generation, varuint sequence, byte
frame count, then each frame as int16 length and the encoded bytes. Every
frame is 20 ms. The codec numbers are FTE's `cl_voip_codec` values: 0 Speex
narrowband at 11025 Hz, 1 raw 16-bit PCM at 11025 Hz, 2 Opus at 48000 Hz,
3, 4 and 5 Speex at 8000, 16000 and 32000 Hz, 6 G.711 A-law and 7 G.711
µ-law at 8000 Hz. Opus and Speex are used when their libraries are present
(they are loaded at run time); raw and G.711 always work.

The generation changes when the speaker starts talking again, so the
receiver can reset its decoder. Voice travels in the move packet. The
server forwards it as `svc_voice` to every other player in the game,
skipping listeners who sent `vignore <slot> 1` and players who are muted
(`sv_voip_echo 1` sends it back to the speaker too). `sv_voip 0` turns
relaying off. Voice is kept out of
server-side demos unless `sv_voip_record` is 1.
