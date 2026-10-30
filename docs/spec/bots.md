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
- Being held up by another actor (touching one, or a feeler hitting one) is
  never a reason to give up: the no-progress timer pauses, and being stuck
  only counts once no actor has been in the way for 2 s, and only after the
  current path has existed for a second (a fresh path gets a fair try). After 1.5 s of
  being held up, a climb, drop or gap jump moves 56 units to one side if
  there is room, and otherwise the bot steps aside for a moment. A ground
  point with an actor on it counts as reached within 64 units.

## Vision and the Body

- Candidates are entities with `FL_CLIENT` or `FL_MONSTER`, culled with
  `checkpvs` from the eye, then the field of view (90°), then a sight line
  (`MOVE_NOMONSTERS`) to the middle of the box or the eye.
- The default threat comparison picks the closer threat, but the previous
  primary threat wins unless the other is more than 15% closer, so a bot
  doesn't alternate between two threats at about the same range.
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

## Player bots

A player bot is a fake client from `spawnclient()` that plays through player
input, the way Nuclide's botlib drives its bots, so the game's own player
physics, weapons and rules apply to it unchanged. This part is a Bulwark
addition; the behaviour description doesn't cover it.

- `BWBot::CreateLocomotion` gives a bot that is a client (`FL_CLIENT`) a
  `BWPlayerLocomotion`. `BotStart` then leaves the box, solidity, movetype
  and `.customphysics` to the game, and the Body leaves the box and eye
  height alone. Posture becomes the crouch or prone button.
- The engine runs one command per bot client every frame (see
  [builtins.md](builtins.md), "Bots and routes"). The game calls
  `bw_player_input(self)` at the start of `SV_RunClientCommand` (or from
  Nuclide's `ncBot::RunAI`). That runs the bot's frame and think, then fills
  `input_angles` (the Body's view), `input_movevalues`, `input_buttons` and
  `input_impulse` for the physics that follows.
- Move values are the wish velocity against the final view yaw, so a bot can
  look at one thing and run towards another (strafing). The direction is
  re-aimed at the requested goal every frame, not only at each think, and
  the speed eases off within about a sixth of a second of it, because player
  physics at 320 u/s otherwise circles a goal it can't stop on.
- Jumps press the jump button for one frame; the physics decides the height.
  The bot's highest climb is the jump height from `sv_jumpspeed` and gravity,
  less 2, so paths only plan the climbs the physics can make. Climbs and gap
  jumps keep pushing towards their landing while in the air (weak with
  Source-style air control, but enough to move onto a ledge once the feet
  clear its lip). A jump the physics didn't take is dropped after 0.3 s.
- Leaving and landing on the ground are read from `FL_ONGROUND` after the
  physics ran. Player physics reports no touches, so a short box sweep along
  the move direction finds actors in the way (contact events and the
  actor-blocking rules of path following).
- Buttons: `PressButton(bits, seconds)` holds buttons (0 seconds is one input
  frame), `ReleaseButton`, `IsButtonDown`, and `SelectWeapon(impulse)`. The
  bits match Nuclide's `INPUT_BUTTON*`: attack 1, jump 2, prone 4, reload 8,
  secondary attack 16, use 32, sprint 64, crouch 128.
- The game respawns a dead player bot and then calls `BotRespawn()`, which
  starts the behaviour over and forgets what the bot knew.
- Aiming skill lives on the Body: `m_aim_error` (degrees, a wobble that
  moves every 0.4–0.8 s, scaled by range), `m_track_rate` (how fast the aim
  catches a moving subject) and `m_head_rate` (turning speed). `m_aim_pos`
  is the point the head is steering to, error included;
  `IsAimingAt(pos, radius)` tells a game whether to pull the trigger. Firing
  at `m_aim_pos` rather than the true target lets the error show in the
  shot. Reaction time is the Vision's `m_reaction`.
- The Body keeps the view angles itself (`m_view`) and writes them to
  `v_angle` each frame. A game may reset a player's `v_angle` from the
  command it is running, as Nuclide does before every input frame, and the
  Body would otherwise only ever turn one frame's worth from there.
- A game whose players must be its own class (Nuclide's `ncPlayer`/`ncBot`)
  defines `BW_PAWN_BASE` as that class before including the framework, and
  `BWBot` then derives from it. The pawn class's own `IsAlive` is used.
  Nuclide's tftemplate (sokkycakes/nuclide) runs its bots this way, with
  `BW_PAWN_BASE` set to its `tfPlayer`; see its README, "Bots".

## Learned by probing

- The framework reads cvars with `cvar()`, which works with or without
  autocvar support (Bulwark gained autocvars after this was written).
- `random()` is seeded from the clock, so every test run differs;
  `bottest_seed` adds further variation. The scenarios were soaked over
  2000 runs each.
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
  `&&` and `||` evaluate both sides (no short circuit), so guards against a
  null or freed entity are nested `if`s.
  Fields that are arrays work in classes.
- The framework compiles inside Nuclide's server progs (with its
  `fteextensions.qc`) without warnings, also with `BW_PAWN_BASE` set to
  `ncBot`.
- Notifications such as `bw_notify_weapon_fired` can run other bots' vision,
  which calls `makevectors`, so a weapon must keep its own copy of
  `v_forward` before sending one.

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
  survivor. The infected nearly always win, with pounces landing from up to
  about 600 units. The test's Hunter steers its leap a little (400 u/s² of
  air control towards the target) and won't pounce at a target in the air.
- **pnav**: three player bots (fake clients on the engine's player physics,
  `sv_jumpspeed` 289 for a 52 unit jump) go onto the ledge round the wall,
  up the stairs, and off the ledge. All three must succeed within 60 s.
- **tf**: a TF2-style King of the Hill game, all of it test code in
  `tests/bots/tf.qc`. RED and BLU each have a Scout, Soldier, Heavy, Sniper
  and Medic with TF2's health and speeds; the bots send only input. Weapons:
  scattergun (10 × 6, 5° spread), rockets (1100 u/s, 90 splash over 146
  units with knockback, 60% to the shooter), minigun (spin-up 0.87 s, 4 × 9
  every 0.105 s, 110 u/s while spun), sniper rifle (scoped charge 50 to 150
  over 3.3 s, ×3 for a scoped headshot), medigun (24 hp/s, 72 after 10 s
  unhurt, overheal to 150%, uber after 20 s of healing for 8 s). Hitscan
  damage is 150% up close, 100% at 512 and 50% from 1024. The point unlocks
  after 10 s, takes 6 s for one capper (more cap faster, Scouts count
  double, contested points stop), and holding it 60 s wins. Health packs at
  each spawn and mid-way; respawn after 5 s. As in TF2, teammates pass
  through each other and their own team's shots (each team is solid in one
  `dimension_solid` bit and hits the other's).
  The behaviour, in the style of TF2's bots: a main action aims and fires
  at the most dangerous visible enemy every think (class aim points: head
  for the Sniper, the feet ahead of a walker for rockets, the chest
  otherwise), turns to whoever shot it, and keeps the minigun spinning for
  3 s after a fight. Inside it a tactical action breaks off for a health
  pack below 40% health unless a Medic is on it. Inside that, the class
  role: Scout, Soldier and Heavy take the point or spread around it to
  defend, strafing (and the Scout jumping) while fighting there, which
  doesn't count as stuck; the
  Sniper picks a waypoint 600–1400 from the point with a view of it, scopes
  in and moves on when hurt below 60%; the Medic follows the most hurt
  teammate, leaning towards Heavies and Soldiers, and ubers when it or its
  patient is below half health in a fight. Skill (`tf_bot_skill`, default
  1) sets aim error, tracking, turning speed and reaction time.
  A pass needs a capture, 6 kills by at least 3 classes, 300 healed, a
  respawn, and no living bot stuck for 10 s. A match usually ends in a win
  after 80–160 s with several captures and 40–70 kills. RED wins about
  70% of matches: the test map isn't symmetric (BLU's side has the ledge).
  Soaked over 400 seeds (pnav, basic and l4d over 300 each).
