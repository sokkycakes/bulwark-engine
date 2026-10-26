# Bot framework

Game-side QuakeC for bots that move, see and decide for themselves: an action
stack for behaviour, four replaceable components, events, waypoint paths with
climbing, drops and gap jumps, and chasing. It is written for Bulwark (ISC)
from the behaviour description in [docs/spec/bots.md](../../docs/spec/bots.md),
and needs only builtins Bulwark already has. It also compiles inside Nuclide's
server progs.

## Using it

Add `bots.qc` to the game's `progs.src` after the game's own definitions
(builtins, constants, system fields; Nuclide's `fteextensions.qc` or
`tests/bots/defs.qc` both work):

```
defs.qc
path/to/qc/bots/bots.qc
mygame.qc
```

Then:

- Call `bw_init()` once, from `worldspawn`. It registers the cvars and
  console commands.
- Pass console lines on: `float ConsoleCmd(string line) { return bw_console_command(line); }`
  (return the game's own result when that returns FALSE).
- Report combat and sound with `bw_notify_injured`, `bw_notify_killed`,
  `bw_notify_sound(source, pos, range)` and `bw_notify_weapon_fired`.
  `bw_damage(victim, inflictor, attacker, amount)` applies damage to a bot and
  sends the notifications for you.
- Waypoints come from `data/<mapname>.way` (format in the spec doc).

A bot is a class derived from `BWBot`:

```c
class Grunt : BWBot {
	virtual BWAction InitialAction(void) { return spawn(GruntPatrol); };
};

Grunt g = spawn(Grunt, netname: "grunt", team: 2);
g.origin = '0 0 0';	/* the origin is at the feet */
g.BotStart();
```

Override `CreateLocomotion`, `CreateBody`, `CreateVision` or
`CreateIntention` to swap a component, `IsEnemy`/`IsFriend` to change teams,
and `DeadAction` for what happens on death. Tuning lives on the components,
for example `bot.m_loco.m_run_speed = 250`. The bot moves itself through
`.customphysics`; `.think` still runs as usual. Remove bots with
`bot.BotRemove()`.

## Actions

```c
class GruntPatrol : BWAction {
	virtual string GetName(void) { return "Patrol"; };
	virtual float Update(float interval)
	{
		BWKnown k = m_bot.m_vision.GetPrimaryKnownThreat(TRUE);
		if (k)
			return SuspendFor(spawn(GruntAttack, m_target: k.m_subject), "saw an enemy");
		return SuspendFor(bw_moveto(somewhere, __NULL__, __NULL__), "patrolling");
	};
	virtual float OnInjured(entity inflictor, entity attacker, float damage)
	{
		return TrySuspendFor(bw_backup(attacker.origin), BWP_IMPORTANT, "hurt");
	};
};
```

- `Update` returns `Continue()`, `ChangeTo(a, why)`, `SuspendFor(a, why)` or
  `Done(why)`, applied at once.
- Event handlers return `BWR_CONTINUE` to pass the event on, or
  `TryChangeTo`, `TrySuspendFor`, `TryDone` or `TryToSustain` with a priority
  (`BWP_TRY`, `BWP_IMPORTANT`, `BWP_CRITICAL`). Their requests run at the
  start of that action's next update.
- `InitialChild()` gives an action a child that runs inside it.
- `Own(obj)` frees a path or helper entity when the action ends.
- Contextual queries (`ShouldHurry`, `SelectMoreDangerousThreat`, ...) are
  answered by overriding them on an action.

Ready-made: `bw_moveto(goal, onSuccess, onFail)`, `bw_backup(from)`, and
`BWDead`.

Paths: `BWPathFollower` (`Compute(bot, goal, forbiddenflags)`, then
`Follow(bot)` every update) and `BWChasePath` (`Chase(bot, subject)`, with
`m_lead`, `m_direct` and `m_lifetime`).

## Console and cvars

| Command | |
| --- | --- |
| `bot_debug <cat>...` | behavior, look, path, animation, locomotion, vision, hearing, events, errors, `*`, or none |
| `bot_debug_filter [entnum\|name]...` | limit debug output to some bots; no arguments clears it |
| `bot_status` | each bot's position, health and action stack |
| `bot_command <text>` | a command-string event to every bot |
| `bot_delete_all [team]` | remove bots |
| `bot_move_to_cursor` | an approach command to where the first player looks |

| Cvar | Default | |
| --- | --- | --- |
| `bot_think_rate` | 10 | full thinks per second |
| `bot_stop` | 0 | freeze every bot |
| `bot_blind` | 0 | bots see nothing |
| `bot_allow_climb` | 1 | climbs in new paths |
| `bot_allow_gap_jump` | 1 | gap jumps in new paths |
| `bot_allow_avoid` | 1 | feeler steering round obstacles |
| `bot_lookahead` | 300 | how far path following skips ahead |
| `bot_actors_block` | 1 | bots collide with other actors (read when a bot starts) |
| `bot_debug_draw` | 0 | print paths and known entities with the debug output |

## Tests

`tests/bots/run.sh build/bulwark-ded` (or `ctest`, with `FTEQCC` set) runs
the headless scenarios on `tests/data/maps/botmap.bsp`, made by
`tools/mkq1map_test.py --bots`. See the spec doc for what they check.
