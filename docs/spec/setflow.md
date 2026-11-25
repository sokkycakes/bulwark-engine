# The online match flow (Stiletto Swift sets)

How a player finds an opponent, plays a set and moves on, the way fighting
games do it (SF6, Tekken 8, Guilty Gear Strive): search from a hub, an
opponent card, a blind hero pick, a set of games with a counterpick for the
loser, then rematch or leave. The rules live in the game (Stiletto's arena
director, `base/src/rules/arena.qc` in the nuclide repo); the screens and
the glue to Bulwark Live live in the engine.

## The flow

1. **Title**: Find Match opens the online hub (`menu_webcore online`).
2. **Online hub** (`layout/web/online.xml`): connects to Bulwark Live,
   shows your fighter card, and offers Casual Duel (first to 2), 2v2 (not
   ready: Stiletto has no 2v2 rules yet), Private Room (an online lobby with
   a room code), Training, the after-a-set choice and Back.
3. **Searching**: the ribbon shows the time; Esc cancels. Training (T) loads
   a local map (`setflow_training_map`, duel_alley) and keeps searching, with
   the ribbon over the game.
4. **Opponent found**: both fighter cards, VS, for `live_match_delay` (3) s.
   No decline. Then the host loads the map and the other player connects.
5. **Hero select** (SELECT): a blind pick with a 20 s timer. Neither side sees
   the other's hero until both lock in (or the timer locks them: anyone
   without a hero gets a random one). Then the reveal, 2.5 s.
6. **Games**: each game is best of 3 rounds, a game ends early on a majority.
   The set is first to 2 games. The set strip at the top shows both names,
   games won and the game number.
7. **Between games**: the game's result, then a 10 s select where only the
   loser may change hero (the counterpick). The winner's hero stays locked.
8. **Set end** (SET END): the result, each game's rounds, and the vote, 15 s:
   Rematch, Rematch with another hero, or Leave. The opponent's vote shows as
   it comes in. Both rematch: a runback, straight into a new set (with a
   fresh select for whoever asked for a new hero). A leave or the timer
   closes the set.
9. **Leaving**: both players see why ("Rematch declined", "No rematch: time
   ran out", ...) and leave the server 2.5 s later. `set_after` decides what
   comes next: `search` (the default) searches the same playlist again from
   the hub; `menu` just opens the hub. Live doesn't pair two players who just
   played again for 10 minutes, unless one of them has searched for a
   minute (the duel playlist's last stage) and nobody else turned up.
10. **Someone leaves a running set** (pause menu: Forfeit Set, or the game
    closing): the other side wins by forfeit. The set end screen says who
    left and whether they were behind. The leaver gets a quit on their card,
    or a disconnect if they timed out. Leaving a set yourself goes back to
    the hub without searching again. The host leaving can't be recorded
    (see live.md, Fighter records).

Every finished set is reported to Live by the host and shows on both fighter
cards: sets, wins and losses, finished-set percentage, quits (and quits
while behind), disconnects, runbacks, forfeit wins, the last 10 results, the
heroes played most, and active days (the matchmaking seniority).

## The game's side (Stiletto)

`arena_setFirstTo` (0) turns set play on; a Live duel's mode sets it to 2
before the map loads. The arena director publishes everything in
serverinfo (keys documented in `rules/arena.h`): `arena_state`, `arena_set`
("firstTo game wins1 wins2 runbacks result winnerSide counterSide"),
`arena_lock`, `arena_vote`, `arena_ents`, `arena_heroN`, `arena_deadline`,
`arena_set_log`, `arena_set_left`, `arena_set_closed` and `arena_set_serial`
(goes up when a set closes, which tells clients to leave). Client commands:
`selecthero <id>`, `lockhero`, `set_rematch`, `set_rematch_hero`,
`set_leave`. A finished set is reported with `live_set_seat` and
`live_set_report` console commands, which an engine without Live ignores, so
the same progs run on FTEQW (sets work there; nothing is recorded).

## The engine's side

`src/client/cl_setflow.c`:

- opens `layout/web/setflow.xml` over a set game (and over a training map
  while searching), and gives it input only while the player has something
  to choose (an unlocked hero, an unvoted set end);
- the page bridge: `getflow` (JSON: the arena keys, your slot from
  `arena_ents` and your entity number, your hero, Live's state, the notice,
  `set_after`, seconds since the last match formed) and `flow:<verb>`:
  `connect`, `game:<id>` (sets `live_game` if empty), `queue[:playlist]`,
  `cancel`, `training`, `pick:<hero>`, `lock:<hero>`, `rematch`,
  `rematch_hero`, `leave`, `after:search|menu`, `hub`, `card:<id>`,
  `dismiss`;
- leaves the server when `arena_set_serial` goes up, then opens the hub and
  searches again if `set_after` is `search`; after an unexpected end (the
  host left, the connection dropped) it opens the hub with a notice;
- resets `arena_setFirstTo` to 0 after a set, so local games aren't sets.

The pause menu turns its host menu, team and vote items off in a set and
calls Leave "Forfeit Set" while the set runs.

`src/server/sv_livesets.c` turns the game's `live_set_seat` /
`live_set_report` into a Live `set_report` (edicts to Live ids; a player who
dropped is reported from a short list of recent drops, with "timed out" as a
disconnect).

`setflow_ui.py` in `tests/live` runs two windowed clients on their own Xvfb
displays through all of this with key presses only (xdotool), and takes a
screenshot of each screen.

On one screen (Local Play, localplay.md) every player in the set picks and
votes on a card over their own view; `getflow` lists them under `seats` and
`flow:p<n>:<verb>` acts for player n. Local sets skip Live.
