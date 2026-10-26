# Sound and room effects

## Mixer

`src/sound/snd_main.c` is a software mixer. Channel 0 of an entity mixes any
number of samples, and an entity's other channels replace each other, as
fteextensions.qc documents for `sound()`. Distance and side panning are
worked out on the main thread, and SDL pulls mixed frames from its own
thread. Music is streamed separately and mixed in last, so room effects
don't apply to it.

## Room effects (`src/sound/snd_dsp.c`)

### The game's reverb slots

These follow the interface fteextensions.qc describes (FTE_CSQC_REVERB):

- `setup_reverb(slot, reverbinfo_t *info, sizeof(reverbinfo_t))` fills a
  slot. The struct holds 26 floats and one int: density, diffusion, gain,
  gain HF, gain LF, decay time, decay HF and LF ratios, reflections gain and
  delay, reflections pan (vector), late gain and delay, late pan (vector),
  echo time and depth, modulation time and depth, air absorption, HF and LF
  reference, room rolloff, and the decay HF limit (int). These are the EFX
  reverb properties, in EFX units: linear gains, seconds and Hz.
- Slot 0 is no effect. Slot 1 is underwater; a game may reconfigure it.
- `SetListener(origin, forward, right, up, slot)` picks the slot. Nuclide
  fills slot 12 from its `env_soundscape` and `env_sound` entities (the
  latter through its `gs_*` EFX decls) and passes 12 here.

### Which effect plays

The checks run in this order; the first match wins:

1. `dsp_off 1` turns everything off.
2. The listener is in water, slime or lava (and `dsp_water` is non-zero),
   or the game picked slot 1. This plays slot 1 if the game set it,
   otherwise room type `dsp_water` (default 14). The direct sound is
   muffled as well.
3. `room_type 1..28` forces a GoldSrc room type. 0 means no effect, and the
   default of -1 leaves the choice to the game.
4. The game's slot, when the game has set one.
5. `dsp_automatic 1` (the default) guesses a room from traces. Every 0.5 s
   it casts nine rays (eight around the listener and one up) and measures
   the mean wall distance, the ceiling height, and how many rays reach sky
   or nothing. These set the decay time, density, reflection and late
   delays (at 13500 units/s), and in large open spaces a slap-back echo.
   The guess is only replaced when the room is clearly different (decay
   off by more than 25%, density by more than 0.25, or the echo appears or
   goes), because each change fades the tail.

Changing effects fades the wet signal out over 80 ms, clears the tail and
fades the new one in. `dsp_volume` scales the wet level. `dsp_show 1`
prints each change, and `dsp_info` prints the active effect.

### GoldSrc room types

There are 29 built-in presets matching GoldSrc's room type names: 0 normal
(none), 1 generic, then small, medium and large versions of metal (2-4),
tunnel (5-7), chamber (8-10), bright (11-13), water (14-16), concrete
(17-19), big (20-22), cavern (23-25) and weirdo (26-28). The values are
EFX parameters chosen here from each name's description, not measured from
GoldSrc.

### The effect

The effect is a fresh software design in the Schroeder/Moorer style:

- **Input:** the mono sum, low-passed so gain HF is reached at the HF
  reference, feeds a 0.45 s pre-delay line.
- **Early reflections:** six taps starting at the reflections delay and
  spread over 0 to 24 ms (scaled by density), alternated left and right,
  panned by the reflections pan's x.
- **Late reverb:** read at reflections delay plus late delay, with that read
  point moved by modulation depth × 2 ms at 1/modulation time Hz. It feeds
  eight parallel feedback combs per ear; the right ear's are 27 samples
  longer. Comb lengths are 1103 to 1601 samples at 44.1 kHz, scaled by
  0.35 + 0.65 × density. Each comb's feedback gives a 60 dB fall over the
  decay time. A one-pole low-pass in the loop makes the highs fall over
  decay × HF ratio instead; when decay HF limit is set, the ratio is
  capped at 1. Four series allpasses (gain 0.25 + 0.45 × diffusion) follow.
- **Tail level:** the tail is scaled by lategain × gain ÷ √(Σ 1/(1 − fb²)),
  so its energy doesn't grow with the decay time. With the generic preset,
  the tail's energy sits about 12 dB under an impulse's.
- **Echo:** a feedback delay at the echo time (0.075 to 0.25 s), with
  feedback 0.55 × depth, feeding the tail and heard directly.
- **Not modelled:** gain LF and decay LF ratio, late pan, air absorption
  and room rolloff are read but have no effect.

Checked offline by feeding an impulse through `S_ApplyDSP`: each preset's
measured RT60 comes out within about 0.3 s of its decay time, the
difference being pre-delay and measuring windows, and the generic tail sits
at -12 dB.

## Player movement

`PM_Move` lifts a box that starts inside the world (for example a spawn
point set into the floor) up to 18 units, or moves it up to 4 units
sideways, before moving it. Without this, a trace that starts partly in
solid could pass through the floor.
