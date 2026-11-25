# Local Play (split screen on one machine)

Local Play is Stiletto's split screen mode: up to four players on one screen,
each with their own controller. It is built on the engine's seats
(splitscreen.md) and the set flow (setflow.md); it adds a page to set a game
up, per-player cards over the set view, and a HUD per player.

## The flow

1. **Title menu → Local Play** (`menu_webcore local`, page
   `data/web/local/index.html`, layout `layout/web/local.xml`).
2. **Join.** The first player has the keyboard, mouse and first controller.
   Start or A on a controller nobody has adds a guest seat; Start on the
   first player's own controller splits it off as a seat of its own (for a
   keyboard player and a pad player). B on a guest's controller leaves.
   Seats stay between games.
3. **Settings** (Up / Down choose, Left / Right change; any seated player can
   drive them):
   - **Mode:** *Versus* is a first-to-N set (setflow.md) for two players, or
     one player against a bot. *Free for all* is Stiletto's deathmatch for
     two or more players, bots included.
   - **Map** from `local_maps`.
   - **Set** (versus: first to 1..5) or **Bots** (free for all: 0..6).
   - **Screen:** how two or three players share the screen
     (`cl_splitscreen2`, `cl_splitscreen3`); four always get quarters.
   - Start is off with a reason shown when the choice can't run (versus with
     three players, free for all alone without bots).
4. **Start** hosts a private listen server (`sv_public 0`, `g_nomotd 1`) on
   the map, then adds the bots.
5. **Heroes.** In versus each player picks and locks on a card over their own
   view, with the usual blind pick and loser's counterpick. In free for all
   each player picks once on the same cards, in place of Nuclide's hero
   menu (which only the first player could use).
6. **The end.** In versus each player votes on their card (Rematch, New
   Hero, Leave); any Leave ends the set. Free for all ends when the host
   leaves from the pause menu. Either way the engine comes back to Local
   Play with the same players. Local games are never reported to Live.

## Input

- Key events from a controller carry the device (0 keyboard and mouse,
  controller slot + 1) and its seat (-1 for a controller nobody has) to a
  Panorama view: `Bulwark.On("key", e)` gets `e.device` and `e.seat`.
- A view opened with `UI_INPUT | UI_SEATS` gets a guest's keys during a
  game too, so each player drives their own card; without `UI_SEATS` a
  guest's keys stay in the game. The left stick moves menus per controller.
- Steam Input controllers take controller slots like SDL ones, so they join
  and play the same way: the Menu action set's `key_gp_menu` is Start and
  `key_gp_a` / `key_gp_dpad_*` drive the cards (checked with
  `tests/steaminput`'s stand-in Steam).

## HUD

Stiletto's HUD page (`webcore_hud`) is drawn once per seat: for seat 1 and
up the engine opens a copy of the page and answers its `cvar:` and
`gethud` queries with that seat's `webcore_hud*` values, kept while the
client game draws each seat.

## Bridge

`fte_query("getlocal")` returns the page's state:

```json
{"seats":[{"seat":0,"on":true,"state":"ready","name":"player",
  "devices":"Keyboard + Pad 1","device":3}, ...],
 "maps":[...],"mode":"versus","map":"duel_alley","firstto":2,"bots":0,
 "players":2,"split":"horizontal","split3":"top","problem":""}
```

`fte_query("local:<verb>")` answers `{"ok":true|false}`:
`join:<device>`, `leave:<seat>`, `set:<mode|map|firstto|bots|split|split3>:<value>`,
`start`, `back`.

`getflow` (setflow.md) adds `local` (cards per player), `localplay`, `pick`
(a free for all waiting for heroes) and `seats`: per player its seat, set
slot, view rectangle (fractions of the screen), hero, name and `done`.
`flow:p<n>:<verb>` runs a flow verb as player n.

## Commands and cvars

| Name | |
|---|---|
| `menu_webcore local` | Open Local Play |
| `local_start [key value]...` | Start with the given settings (`mode`, `map`, `firstto`, `bots`, `split`, `split3`), for tests |
| `local_mode` | `versus` or `ffa` |
| `local_map`, `local_maps` | The map, and the maps to choose from |
| `local_firstto` | Games to win a versus set (2) |
| `local_bots` | Bots in free for all (0) |
| `local_bot_profiles` | Bot profiles added in turn |

## Test recipe

```sh
# two virtual pads: pad 2 joins with Start, a versus set starts
DISPLAY=:99 bulwark -basedir $B -game base -game stbuilt +joy_virtual 2 \
  +menu_webcore local +wait 200 "+joy_virtual_set 1 button 6 1" +wait 5 \
  "+joy_virtual_set 1 button 6 0" +wait 100 +local_start
# three players, free for all with a bot
... +seat_add guest 2 +seat_add guest 3 \
  "+local_start mode ffa map lqdm1 bots 1 split3 top"
```

## Open

- Free for all has no end screen of its own; the host leaves from the pause
  menu.
- Team modes and co-op aren't offered.
- Sound comes from the first player's position only (splitscreen.md).
