# Bots

The bot framework in `qc/bots/` is game-side QuakeC. It was written from a
behaviour description (an action stack, components and events, in the style
of Valve's NextBot as a design reference) without reading any NextBot code,
any port of it, or FTEQW's engine. This page records what the framework does
where that description left room, and what was learned by probing Bulwark and
FTEQCC. Usage is in [qc/bots/README.md](../../qc/bots/README.md).

## Shape

- A bot is a `BWBot` entity with four components: Locomotion, Body, Vision
  and Intention. Each comes from a factory method the bot class can override.
- Locomotion and the Body's head aim run every server frame from the bot's
  `.customphysics`. Vision and behaviour run at `bot_think_rate` (10 Hz); each
  bot's first think is offset by a random fraction of the interval.
- Events go to Locomotion, Body, Vision and Intention in that order; the
  Intention hands them to the actions. The payload of an event is saved per
  nesting level, so a handler that sends another event doesn't spoil the
  outer one.

## Action stack: choices made here

- Every level of behaviour is a stack; the top action is active, the rest are
  suspended. An action's child stack starts on its first update.
- `Update` runs the parent before its child stack, so a parent's change ends
  its children before they run again.
- An event visits the innermost active action, then the actions it is
  suspended over, then the parent level the same way. The first action that
  answers with anything but Continue stops it. Suspended actions' own child
  stacks don't get events.
- Requests from events are queued on the action that made them. At the next
  update of that level the strongest one wins (ties: the higher action). If
  it came from a suspended action, the actions above it end first, and the
  request then applies to it, so a Suspend request from below pushes the new
  action directly on top of the action that asked.
- `OnStart` and `OnResume` may also return a result; it is applied at once.
  `OnSuspend` may return Done, which ends the action instead of keeping it.
  More than 16 changes in one step are refused and logged as an error.
- The stack string lists each level innermost first, `<<` between an action
  and what it is suspended over, and ` in ` between a level and its parent:
  `Pounce<<HunterAttack in Behaviour`.
- Killing a bot (`bw_notify_killed`, or `bw_damage` reaching 0 health)
  ends the whole behaviour and starts the bot's `DeadAction` (by default
  `BWDead`, which lies down and sustains every event).
- `MoveTo` keeps following a partial path when told "no path exists" for
  it, and fails on any other failure.

## Locomotion

Origin is at the feet; the standing box is 26 wide and 68 tall, crouching 32,
sitting 40, lying 16 (the last two are guesses). Defaults: run 150, walk 75,
acceleration 500, turning 250°/s, step 18, jump 180, slope limit 0.7 (normal
z), side friction 10/s, braking at twice the acceleration when moving
backwards, governor tapering over the last quarter of the desired speed.
Approach directions are averaged by weight and held for 1.5 think intervals
(at least 0.15 s). Climbs are a jump to 12 units above the ledge with air
control of 0.6 × run speed towards it; gap jumps are ballistic to 24 units
above the higher end, capped at 600 u/s across. Stuck: wanting to move but
staying within 100 units for longer than 100 units would take at a tenth of
the desired speed (6.7 s at run speed).

## Paths and following

- Waypoint files (`data/<map>.way`) are as described: the node count, then
  per node `x y z radius linkcount` followed by `linkcount` lines of
  `othernode cost flags`. Positions are 24 above the floor, and paths use the
  floor below them. Radius is read but not used.
- A* runs in QC over the whole graph with a linear open list (graphs are
  small). The start and end waypoints are the nearest of the eight closest
  that can be seen from the position, else the nearest.
- Up to two waypoints at each end are dropped when the one beyond them can be
  walked to directly.
- Each pair of points is probed in 16 unit steps with the bot's box raised by
  a step: something taller in the way that can be stood on within jump height
  becomes a climb; ground that falls away becomes a gap jump if ground at the
  same height (within a step) turns up within 200 units, otherwise a drop.
- Ground points count as reached within 16 units, or when passed in the
  path's direction within 120 units and 40 in height. Skipping ahead uses
  `bot_lookahead`. Speed drops by up to 60% for the sharpest corner in the
  next 150 units. Following gives up after 3 s without getting 8 units
  closer, after a second of being stuck, or when the bot ends up more than
  60 units below the path.
- Avoidance uses two line feelers at knee height on the box's sides, 40 units
  plus 0.4 s of travel long, and ignores a chased subject.

## Vision and the Body

- Candidates are entities with `FL_CLIENT` or `FL_MONSTER`, culled with
  `checkpvs` from the eye, then the field of view (90°), then a sight line
  (`MOVE_NOMONSTERS`) to the middle of the box or the eye.
- An actor the game tells the bot about (`AddKnownEntity`) counts as known
  for 10 s from the telling; it is "aware" at once. Seen actors are aware
  after the reaction time (0 by default).
- The head turns at up to 540°/s, slowing inside 30° of the target, with a
  0.15 s ramp; "on target" is within 11°. A tracked entity is followed at 8
  per second lag and 0.1 s of lead. The model leans a third of the head's
  pitch.
- An activity with no matching model sequence (or no model) completes on the
  next frame. Model animation events aren't read yet (the engine has no
  `processmodelevents`); a game can send `BWE_ANIM_EVENT` itself.

## Learned by probing

- Bulwark has no autocvar support; the framework reads cvars with `cvar()`.
- Bulwark's `random()` is not seeded from the clock, so a headless run is
  repeatable. The tests take a `bottest_seed` to vary runs.
- `.customphysics` replaces the movetype and is followed by the entity's
  think, as its doc comment says.
- On BSP29/BSP30 maps a box trace uses the map's fixed hull whose bottom lines
  up with the box's. The bot's 26 × 68 box therefore collides as Half-Life's
  32 × 32 × 72 standing hull, and crouched (32 tall) as the 36 tall crouch
  hull. A box under 3 units wide uses the point hull; anything up to 32 wide
  uses the standing hull, so ground and feeler probes use `traceline`.
  The test map is a BSP30 map for this reason.
- FTEQCC: inside a class method a bare field name means `this.field` and
  `self` means `this`, so code that needs the field itself
  (`findflags(e, flags, ...)`) or the global `self` lives in plain
  functions. `float *a, *b` makes `b` a `float **`. Class-typed global arrays
  are refused (an `entity` array works). `1e9` is not a valid literal.
  Fields that are arrays work in classes.
- The framework compiles inside Nuclide's server progs (with its
  `fteextensions.qc`) without warnings.

## Tests

`tests/bots/run.sh` compiles `tests/bots` and runs each scenario on
`tests/data/maps/botmap.bsp` with `bulwark-ded`. The map
(`tools/mkq1map_test.py --bots`) is a 2048 × 2048 room with a 32 thick,
128 tall wall from y −400 to the north wall, a 400 × 500 ledge 48 high, four
16 unit steps (32 deep) up to a 64 high landing, and two 64 high platforms
with a 96 unit gap; `tests/data/botmap.way` holds 22 waypoints.

- **basic**: a MoveTo bot goes from the north-west corner round the wall onto
  the ledge (must succeed within 50 s; about 26 s), another climbs the stairs,
  one jumps the gap, one drops off the ledge; a roaming bot (team 1) and a
  chaser (team 2) that must see it with a sighting event. A catch is reported
  but not required.
- **l4d**: three survivors (team 1) shoot visible infected within 800 units
  for 4 damage every 0.5 s and flee to the waypoint farthest from a threat
  within 350, repathing every second; a Hunter (team 3, 250 hp, speed 250)
  crouches 0.6 s and pounces at 700 u/s, pinning on landing within 48 units
  or touching (8 damage every 0.5 s), waiting 1.5 s after a miss; a Tank
  (team 3, 1500 hp, speed 210, acceleration 800) chases leading its target
  and punches within 70 after a 0.4 s wind-up (24 damage, 450 u/s away and
  300 up, 1.5 s cooldown). A pass needs a pounce, a punch and a fleeing
  survivor; in 8 seeded runs the infected won every time, with pounces
  landing from up to about 600 units.
