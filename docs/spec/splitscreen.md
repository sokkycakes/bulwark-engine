# Split screen and guests

Up to four local players (seats) share one client and one network
connection. Each seat is a full player on the server, with its own view,
input devices, stats and userinfo. Seat 0 is the player who connected; seats
1 to 3 join and leave at any time, as guests of seat 0 (Xbox 360 style,
named `sokky (1)` by default). Offline play is the same path over loopback.

Seats are numbered from 0 in the protocol and the code, and from 1 in
commands players type (`seat_remove 2`, `p2 +attack`), as in FTE and Quake 3.

Status (2026-11-06): seats on the wire, per-seat client and client game
state, per-device input and press Start to join work, tested with Nuclide
base on a listen server and a remote dedicated server with 2 and 4 seats.
Open work is listed at the end.

## Wire protocol

Protocol version 2 is unchanged; these messages are appended:

| Message | Body | Meaning |
|---|---|---|
| `svc_seat` | byte seat | The rest of this message is for that seat, until the next `svc_seat`. Each reliable message and each datagram starts at seat 0 |
| `svc_seatinfo` | byte seat, byte slot (255 none), varuint entity | Seat's player slot and entity, or the seat is gone (left, refused, server full). Sent again after each `svc_serverdata` |
| `clc_seat` | byte seat | The rest of this message is from that seat |

String commands from seat 0 (the connection):

- `seat_join <seat> "<userinfo>" [guest]`: start a seat. The server answers
  with `svc_seatinfo`. A repeat of a seat that exists is answered again.
- `seat_leave <seat>`: the seat's player leaves; the others stay.

The serverinfo key `sv_maxseats` (cvar, default 4) says how many seats a
connection may have; 1 turns split screen off. A client doesn't ask for a
seat the server won't allow.

## Server

- A seat is a `client_t` with `parent` (its connection's slot + 1) and
  `seat`. It never uses its own netchan; its reliable messages go through
  the connection's behind `svc_seat` (`seat_reliable`), and it is never
  matched to incoming packets.
- The server adds `*seat`, `*parent` (the connection's slot) and, for
  guests, `*guest 1` and `*sponsor` to the seat's userinfo.
- Things sent to everyone (prints, `bprint`, chat, sounds, particles,
  `MSG_BROADCAST`, `MSG_ALL`, non-`ONE` multicasts, voice) go once per
  connection. A PVS test passes if any seat of the connection may see the
  point. `MSG_ONE` and `MULTICAST_ONE` to a seat's player reach that seat.
- Each packet carries the connection's entity section, then one per seat
  behind `svc_seat` (a `svc_snapshot` each, with the seat's last command
  number). The room left after the unreliable extras is shared out evenly.
- Every entity is in exactly one section of a connection's packet: a seat's
  own player and what it owns in that seat's; anything else in the section
  of the seat that already has it, or else the first seat whose
  `SendEntity` takes it. Nuclide's `ncPlayer::SendEntity` sends its owner
  fields no one else gets, and `Predict_EntityUpdate` decides at creation
  whether an entity is the local player, so this keeps both right.
- The connection's acks and losses go to every seat's entity history.
- A seat spawns when it joins a spawned connection, or when its connection
  says `begin` after a map change. Dropping the connection drops its seats.
- Recordings (`SV_MVD*`) see seats as players.

## Client

- `cl_seats[4]` hold each seat's state. The active seat's scalars
  (`playerslot`, `playerent`, command frames, `cmd_start`, `viewangles`)
  live in `cl`; its arrays (stats, the command ring) stay in the seat and
  `cl` points at them. `CL_SetSeat` swaps. Seat 0 is active except while a
  seat's part of a server message is parsed, a seat's command is built or
  sent, a seat's console command runs, or the client game has picked a seat.
- Each client entity remembers which seat's section it came in. A removal
  from another seat's section is ignored; an update from a new seat sets the
  entity up again (`isnew`) as that seat sees it.
- Commands are built for every playing seat at the same rate and sent in
  one packet: seat 0's `clc_move`, the voice, then each seat's behind
  `clc_seat`.
- Seat states: free, joining (asked; asked again every 2 s), loading (has a
  slot), playing. Seats survive disconnects and map changes and rejoin the
  next server.
- `CL_SeatsChanged` runs `live_guests` with the guests' names when the
  online party code is built in (thread "Design an Xbox Live-style lobby").

## Client game

- `numclientseats` is the highest playing seat + 1 (0 while loading).
- `setproperty(VF_ACTIVESEAT, n)` makes seat n active and sets
  `player_localentnum`, `player_localnum`, `clientcommandframe`,
  `servercommandframe` and `view_angles` from it; `getstat*`,
  `getinputstate` and `VF_CL_VIEWANGLES` follow. `getproperty` returns it.
- The engine activates a seat before its entity section, its
  `CSQC_Input_Frame`, its devices' `CSQC_InputEvent`s and its console
  commands, and returns to seat 0 after `CSQC_UpdateView`.
- `SetListener` is taken from seat 0 only, for now.
- Nuclide needed one fix: `CSQC_UpdateView` read `player_localentnum`
  before switching seats, so each view found the previous seat's player.
  Base's HUD drew the health row at the top of the screen for every seat.

## Input

- Devices: 0 is the keyboard and mouse, 1..4 the SDL controller slots.
  Seat 0 has the keyboard, mouse and first controller.
- A controller nobody has joins as a guest with Start (`cl_seat_autojoin`,
  default 1). Out of a game it drives the menus like seat 0's.
- A seat's device runs its bindings as `p<seat> <binding>`; `p1`..`p4` (and
  `+p1`..`-p4`) set `in_cmdseat` around the command, so buttons, impulses,
  `rotate`, client game commands and commands forwarded to the server are
  that player's. Buttons, impulses, stick axes and turns are kept per seat.
- The console and the engine's menus belong to seat 0.
- A guest's controller unplugging keeps the seat; the same slot picks it up.
- `joy_virtual [count]` attaches up to four pretend controllers and
  `joy_virtual_set [pad] axis|button n value` drives one.

## Commands and cvars

| Name | |
|---|---|
| `seat_add [guest\|<name>] [device]` | Another local player |
| `seat_remove <seat>` | A local player leaves |
| `seat_list` | The seats, their slots, entities and devices |
| `seat_device <seat> <device>` | Give a device to a seat |
| `cl_seat_autojoin` | Start on a free controller adds a guest (1) |
| `sv_maxseats` | Seats per connection the server allows (4; serverinfo) |

## Test recipe

```sh
Xvfb :99 &
DISPLAY=:99 bulwark -basedir <nuclide> -game base +map ai +wait 200 \
  +seat_add guest +seat_add guest +seat_add guest +wait 200 \
  +p2 rotate 90 "+p2 +back" +wait 100 "+p2 -back" +screenshot four +quit
# press Start to join and move with virtual pads
DISPLAY=:99 bulwark ... +map ai +joy_virtual 2 +wait 200 \
  "+joy_virtual_set 1 button 6 1" +wait 5 "+joy_virtual_set 1 button 6 0" +wait 150 \
  "+joy_virtual_set 1 axis 1 -1" +wait 80 +screenshot pads +quit
```

Measured on `ai` (loopback UDP to `bulwark-ded`): one seat 7.3 KB/s down,
two seats 11.3 KB/s down.

## Open

- One listener per seat in the mixer (each sound from the nearest seat).
- Profiles (a second signed-in player) and per-seat sensitivity, invert and
  FOV; guests use the global settings now.
- Engine overlays (centre print without a game, net graph) per seat.
- Sending identical entity bytes once per connection instead of per seat.
- The LAN lobby's (`cl_lobby.c`) seat list and the online party's
  `live_guests` once that code is merged.
- Nuclide's co-op and TF HUDs have only run with one seat.
