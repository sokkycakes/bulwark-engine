# Steam Input

Bulwark supports Steam Input natively: the Steam Deck, the Steam Controller
and every pad Steam Input handles drive the game through the game's own
actions, and button prompts match the controller in hand, using Xelu's free
prompts (CC0). Steam stays optional. Without Steam, controllers go through
SDL as before.

Code: `src/client/steaminput.{c,h}` (Steamworks loaded at run time, controller
identity, glyph tables; shared with the playtest launcher),
`src/client/in_steam.c` (actions to engine keys and axes),
`src/client/vid_sdl.c` (SDL pads, prompts). Tests: `tests/steaminput/run.sh`.

## How a run picks its input

| Started | Steam running | What happens |
| --- | --- | --- |
| As a Steam app (`SteamAppId` set) | yes | ISteamInput with that app id |
| As a **non-Steam game** (Steam's `SteamGameId` names a shortcut) | yes | ISteamInput, started as app `steam_input_appid` (480, Spacewar) |
| From a terminal or a desktop icon | yes | the same as a non-Steam game |
| Any of the above, but no `libsteam_api` beside the engine | yes | SDL, reading Steam's virtual gamepad (next section) |
| Anything | no | SDL, reading the devices directly |

Starting ISteamInput needs `libsteam_api.so` (Linux, Steam Deck),
`steam_api64.dll` (Windows) or `libsteam_api.dylib` beside the engine. These
come with the Steamworks SDK (redistributable) and are the same files the
Steamworks bridge (`services/live/steam`) uses. Bulwark never links against
them, so builds without them work the same as before.

As a non-Steam game, Steam names no app, so Bulwark sets `SteamAppId` to
`steam_input_appid` (default 480) before `SteamAPI_Init`, the same as a
`steam_appid.txt` would. A `steam_appid.txt` in the working directory wins.
While the game runs, Steam shows it as playing Spacewar. With
`steam_input_appid 0`, only a real Steam launch or `steam_appid.txt` starts
it.
`si_shutdown` puts `SteamAppId` and `SteamGameId` back the way Steam left
them, so a program started afterwards sees the real launch. The playtest
launcher relies on this before it starts the game.

The playtest launcher (nuclide `Tools/playtest-launcher`) builds copies of
`steaminput.{c,h}` and `steaminput_origins.h`. Change them here and copy them
across.

### Steam's virtual gamepad

Steam gives every game it starts, non-Steam games included, a virtual
gamepad (Valve `28de:11ff`) in place of the real device. It also writes what
each virtual pad really is to the file named by `SteamVirtualGamepadInfo`
(`[slot N]` sections with `VID=`, `PID=`, `name=`, `type=`). SDL 2.30 and
newer read that file and report the real device. With an older SDL, Bulwark
reads the file itself and matches the slot by the device name's "pad N" on
Linux and by the XInput index on Windows. If the file is missing on a Deck
(`SteamDeck=1`), the pad counts as the Deck. So a Deck run as a non-Steam
game shows Deck prompts even without `libsteam_api`.

While ISteamInput has controllers, SDL's pads are ignored, because they are
Steam's copy of the same controllers. `in_deviceids` lists them as unused.

Without Steam, SDL reads Valve's hardware itself. Bulwark turns on SDL's
Steam Controller driver (`SDL_JOYSTICK_HIDAPI_STEAM`) only when Steam isn't
running, since two readers fight over it. The Deck's L4/L5/R4/R5, a
touchpad click and the share button become keys `GP_L4`, `GP_L5`, `GP_R4`,
`GP_R5`, `GP_TOUCHPAD` and `GP_MISC1`. Buttons are named by position on
every pad (`SDL_GAMECONTROLLER_USE_BUTTON_LABELS 0`), so `GP_A` is the
bottom face button, including a Switch pad's B.

## Actions

The game ships `steam_input/steam_input_manifest.vdf`, Valve's Action Manifest,
plus the layout files it names, as loose files in its game directory. Steam
reads them from disk. Bulwark falls back to its own in `data/steam_input/`,
which `tools/steaminput/gen_configs.py` generates. Bulwark gives Steam the
absolute path (`SetInputActionManifestFilePath`), which Valve documents for
running outside a Steam depot and which applies until Steam restarts.

| Action | Engine side |
| --- | --- |
| a digital action `key_<KEY>` | presses that engine key, e.g. `key_gp_a` is `GP_A`, so menus and anything bound to pad keys work unchanged |
| any other digital action | presses key `SI_<ACTION>` (`SI_JUMP`), bound like any key; `steam_input/bindings.txt` gives the default (`jump +jump`) for keys nothing binds |
| analog `move`, `menu_move` | left stick, axes 0 and 1, which also drive menu navigation |
| analog `look` | a stick held over (`k_EInputSourceMode_JoystickMove`) becomes axes 2 and 3. Trackpads, gyro and joystick camera arrive as mouse deltas and turn the view like the mouse, scaled by `sensitivity` and `in_steamlook` |

Action names are unique across sets, because Steam looks actions up by name
alone. Sets switch on their own. `Menu` is active while the console, a menu
or anything else that takes UI input is open, or when no game is running.
Otherwise `InGame` is active. A manifest without those names uses its first
set. Keys that were held in the old set are released before the new set
counts. `steaminput_set <set>|auto` pins a set, and
`steaminput_layer <layer> 0|1` turns on a layer from the manifest's
`action_layers`.

Split screen: each Steam Input controller takes a controller slot (device
slot + 1), the same as an SDL pad, so seats, guests and `p2`... work the same.

The default layouts (`bulwark_<type>.vdf`) cover the Deck (`controller_neptune`), the
original Steam Controller (`controller_steamcontroller_gordon`), Xbox One/Series,
Xbox 360, PS4, PS5, Switch Pro and generic pads:

| | In game | Menus |
| --- | --- | --- |
| A / B / X / Y | jump, duck, reload, use | select, back, X, Y |
| RT / LT | fire, alt fire | page right/left |
| RB / LB | next/previous weapon | next/previous tab |
| left stick (click) | move (sprint) | move through the menu |
| right stick (click) | look (flashlight) | |
| right trackpad (Deck, Steam Controller) | look, as a mouse | |
| d-pad (Steam Controller: left pad) | weapon slots 1-4 | d-pad |
| Menu / View | pause, scores | close menu, view |

The Deck's back buttons and gyro start unbound, and players add them in
Steam's layout editor. These layout files are written in the
`controller_mappings` format that Steam exports. `tests/steaminput/check_manifest.py`
checks them against the manifest, but Steam itself could not be run here.
If Steam shows an action set without bindings, build the layout in Steam's
editor (Steam Input Layout Dev Mode), export it with
`steam://dumpcontrollerconfig?appid=480`, and put the file in its place.

## Prompts

Xelu's prompts live in `data/panorama/images/glyphs/<set>/<button>.png`. They
are imported by `tools/steaminput/import_xelu.py` from the pack at
https://thoseawesomeguys.com/prompts/ (CC0). Sets: `steamdeck` (also the
2026 Steam Controller and Steam Frame pads, which share the Deck's layout and
have no Xelu art of their own), `steamcontroller` (the original),
`xbox`, `xbox360`, `ps4`, `ps5`, `switch`. Buttons are named by position:
`south east west north lb rb lt rt lstick rstick lclick rclick dpad dup
ddown dleft dright start back l4 l5 r4 r5 lpad rpad cpad gyro lgrip rgrip`.

- `in_padglyphs` (read only) names the set of the controller used last.
- `in_glyph <key or command> [device]` prints the prompt for a binding.
- Panorama: `fte_query("glyph:+jump")` or `fte_query("glyph:GP_A 2")` returns
  `"<image>|<label>"` (`{images}/glyphs/steamdeck/south.png|A`), or `""`.
  The co-op HUD and menu use it.

On a Steam Input controller, the prompt comes from what the player has bound
right now (`GetDigitalActionOrigins`). Each Steam origin maps to a set and a
button through `src/client/steaminput_origins.h`, which
`tools/steaminput/gen_origins.py` generates from the order of the SDK's
`EInputActionOrigin` enum. An origin newer than that table falls back to
Steam's own name for it. On an SDL pad, the prompt comes from the pad key
the command is bound to and the pad's set.

## Cvars and commands

| | |
| --- | --- |
| `in_steaminput 1` | 0 uses SDL only; `in_steaminput_restart` applies it |
| `steam_input_appid 480` | the app id for non-Steam-game and terminal launches |
| `in_steamlook 1` | scale of the mouse-like look from trackpads and gyro |
| `in_steaminput_status` | how the game was started, Steam Input's state, the manifest and controllers |
| `steaminput_bindings` | opens Steam's binding panel (needs the overlay) |
| `joy_virtual_dev <vid> <pid> [name]` | a pretend controller that claims to be that device, for tests |

## Tests

`tests/steaminput/run.sh [build dir]` runs the engine in Xvfb against
`tests/steaminput/mock_steam_api.c`. That is a stand-in `libsteam_api` that
loads the manifest the way Steam does, hands out handles only for what the
manifest declares, reports actions as active only while their set is, and
plays a script of connects, presses and origins. It covers:

1. As a non-Steam game with Steam running: the API starts as app 480, the
   manifest loads, a Deck connects, the menu set presses `GP_DPAD_DOWN`, the
   in-game set presses `SI_JUMP`/`SI_ATTACK`, Menu actions stay quiet in
   game, `bindings.txt` defaults apply, prompts are the Deck's A and R2, and
   Steam's virtual gamepad doesn't double the input.
2. Launched as a Steam app: Steam's app id is kept, and the original Steam
   Controller gets its own prompts.
3. Steam not running: a Deck read through SDL gets Deck prompts.
4. A non-Steam game without `libsteam_api`: Steam's virtual gamepads plus
   `SteamVirtualGamepadInfo` give Deck and DualSense prompts.
5. Plain SDL: Xbox and Switch prompts, and `fte_query("glyph:...")`.
6. `in_steaminput 0`.

A real Steam client can't sign in on the build machines, so the stand-in
replaces Steam. The things only real hardware can confirm are listed in the
pull request.
