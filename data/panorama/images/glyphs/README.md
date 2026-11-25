# Controller prompts

Button glyphs by Nicolae "Xelu" Berbece, from Xelu's Free Controller & Key
Prompts, https://thoseawesomeguys.com/prompts/, released into the public
domain under CC0 1.0 (see LICENSE).

One folder per controller family, one file per button, named by position
(`south` is A on Xbox and the Deck, Cross on PlayStation, B on Switch):

| Folder | Controllers |
| --- | --- |
| `steamdeck` | Steam Deck, and Valve's newer Deck-layout pads (2026 Steam Controller, Steam Frame) |
| `steamcontroller` | the original Steam Controller |
| `xbox` | Xbox Series and One, and anything unknown |
| `xbox360` | Xbox 360 |
| `ps4`, `ps5` | DualShock 4 (and 3), DualSense |
| `switch` | Switch Pro, Joy-Cons, Switch 2 |

The engine's `in_padglyphs` cvar names the folder for the controller used
last, and `in_glyph <command>` / `fte_query("glyph:+jump")` give the file for
a binding. Regenerate with `tools/steaminput/import_xelu.py <pack>`; which
Xelu file is which button lives in `src/client/steaminput.c`.
See docs/spec/steam-input.md.
