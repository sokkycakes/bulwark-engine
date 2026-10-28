/* Number-key weapon selection.

   STOCK-CS: keys 1-4 pick the primary, pistol, knife and grenade buckets
   (hudSlot 0-3 in decls/def/cs_weapons.def). Pressing the key of the bucket
   you are holding cycles to the next weapon in it.
   DESIGN: the weapon switches at once, like CS's hud_fastswitch 1. CSNZ's own
   selection bar has not been captured, so none is drawn. */

#define WS_MAXWEAPONS 64

/* the next weapon in bucket wantSlot after 'after' (or the first one) */
entity
WS_FindInSlot(int wantSlot, entity after)
{
	entity first = weapon.GetFirstWeaponInInventory();
	entity w = first;
	entity found = __NULL__;
	bool passed = (after == __NULL__) ? true : false;

	for (int i = 0; i < WS_MAXWEAPONS && w; i++) {
		if (weapon.GetSlot(w) == wantSlot) {
			if (passed) {
				return (w);
			}
			if (!found) {
				found = w; /* wrap around to the bucket's first */
			}
		}
		if (w == after) {
			passed = true;
		}
		w = weapon.GetNextWeaponRelativeTo(w);
		if (w == first) {
			break;
		}
	}

	return (found);
}

bool
WS_SelectSlot(int wantSlot)
{
	entity current = weapon.GetActiveWeapon();
	entity pick;

	if (current && weapon.GetSlot(current) == wantSlot) {
		pick = WS_FindInSlot(wantSlot, current);
	} else {
		pick = WS_FindInSlot(wantSlot, __NULL__);
	}

	if (!pick || pick == current) {
		return (false);
	}

	weapon.SelectWeapon(pick);
	return (true);
}

bool
HUD_ConsoleCommand(string commandString)
{
	tokenize_console(commandString);

	switch (argv(0)) {
	case "slot1":
	case "slot2":
	case "slot3":
	case "slot4":
	case "slot5":
		WS_SelectSlot(stoi(substring(argv(0), 4, -1)) - 1i);
		break;
	default:
		return (false);
	}

	return (true);
}
