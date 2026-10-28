/* prophunt: Hide and Seek (VoxelPropHunt, mode 49) on Studio maps.
 * Original code (ISC).
 *
 * OBSERVED (CSO_PropHunt_*, CSO_MaploadingTip_VoxelPropHunt,
 * CSO_OverlayTutor_PropHunt_Play01..13):
 *   - teams: Object Team and Seeker Team; Objects survive until time runs
 *     out, the Seeker finds every Object before then
 *   - "Seeker will be selected soon"; "%d seconds until the Seeker appears"
 *   - Transformation Machine: pick an object within the time limit or get a
 *     random one; 3 re-rolls per game
 *   - larger objects have more HP and less speed; objects take no fall damage
 *   - Seeker: basic shotgun and a smoke grenade (objects cough in smoke); the
 *     Seeker loses HP for missing; the Seeker does not revive in the round
 *   - movement: spin 90 degrees, lock position, peep, return to body
 *   - timeline Start -> 2nd Transformation -> End Transformation -> Burning
 *     Time -> End; the 2nd Transformation is taken with E or happens at
 *     random at the end of its window
 *   - Burning Time: Seeker speed up, ammo refilled, no HP loss
 *   - compliments (thumbs up) from dead players, one per player per round,
 *     a Golden Thumbs Up at 10 in a round
 *   - results: longest-lived object, objects found
 * DESIGN: every duration and amount below, one Seeker per five players,
 *   object = a Studio block type (shown to the client through userinfo
 *   csnz_prop; drawing it is client work, BLOCKERS.md), how a miss is told
 *   from a hit (shells used with no hit within 0.3 s).
 * BLOCKED: smoke grenade and coughing (no smoke weapon), peep camera
 *   (client), fall damage immunity (Nuclide does not say what hurt). */

#define PH_REROLLS 3
var float autocvar_ph_select_time = 15;
var float autocvar_ph_hide_time = 20;
var float autocvar_ph_second_at = 0.5;		/* share of the round */
var float autocvar_ph_second_window = 20;
var float autocvar_ph_burning = 60;		/* last seconds of the round */
var float autocvar_ph_miss_damage = 5;

.float ph_offer;		/* object type offered by the machine */
.float ph_prop;			/* object type taken */
.float ph_rerolls;		/* left this game */
.float ph_rerollsSet;
.float ph_selectEnd;
.float ph_second;		/* 2nd transformation done this round */
.float ph_locked;
.vector ph_lockAt;
.float ph_rot;
.float ph_aliveSince, ph_diedAt;
.float ph_thumbs;		/* compliments received this round */
.float ph_gaveThumbs;
.float ph_ammo;			/* seeker: shells last frame */
.float ph_shotAt;		/* seeker: when unmatched shots were fired */
.float ph_shots;
.float ph_lastHit;
.float ph_burning;

float ph_liveAt;
float ph_secondStart, ph_secondEnd, ph_burnAt;
int ph_shellType;
bool ph_secondSaid;

float
PH_RandomProp(void)
{
	int n = 0;
	for (int t = 1; t < VX_MAXTYPE; t++)
		if (vx_placeable[t] && (vx_entity[t] == "" || vx_entity[t] == "NONE"))
			n++;
	if (!n)
		return vx_firstType;
	int pick = (int)floor(random() * n);
	for (int u = 1; u < VX_MAXTYPE; u++)
		if (vx_placeable[u] && (vx_entity[u] == "" || vx_entity[u] == "NONE") && pick-- == 0)
			return u;
	return vx_firstType;
}

string
PH_SizeClass(float t)
{
	float cells = VX_GroupCount(t);
	if (cells >= 4)
		return "csm_prop_large";
	if (cells >= 2)
		return "csm_prop_medium";
	return "csm_prop_small";
}

void
PH_Offer(entity p)
{
	p.ph_offer = PH_RandomProp();
	userinfo.SetString(p, "csnz_propoffer", voxel_typename(p.ph_offer));
	CS_Tell(p, sprintf("%s: %s (cmd ph select / cmd ph reroll, %g left)",
		CS_Text("CSO_PropHunt_SelectUI_Title", "Transformation Machine"), voxel_typename(p.ph_offer), p.ph_rerolls));
}

/* becomes the object; keeps the share of health it had */
void
PH_Transform(entity p, float t)
{
	float share = p.max_health > 0 && p.ph_prop ? p.health / p.max_health : 1;
	vector at = p.origin, ang = p.v_angle;
	p.ph_prop = t;
	p.ph_selectEnd = 0;
	ents.ChangeToClass(p, PH_SizeClass(t));
	CS_MoveToSpot(p, at, ang);
	ents.Input(p, "SetHealth", ftos(max(1, ceil(p.max_health * share))), world);
	userinfo.SetString(p, "csnz_prop", voxel_typename(t));
	userinfo.SetString(p, "csnz_propoffer", "");
	printf("studio %.1f: %s is now %s (%s)\n", time, p.netname, voxel_typename(t), PH_SizeClass(t));
}

void
PH_Equip(entity p)
{
	p.ph_locked = 0;
	p.ph_second = 0;
	p.ph_rot = 0;
	p.ph_prop = 0;
	p.ph_burning = 0;
	userinfo.SetString(p, "csnz_prop", "");
	if (p.team == CS_TEAM1) {
		ents.ChangeToClass(p, "csm_seeker");
		p.ph_ammo = -1;
		p.ph_shots = 0;
	} else {
		ents.ChangeToClass(p, "csm_prop_medium");
		/* joined after the Seeker was chosen: the machine still offers */
		if (cs_round > 0 && (cs_state == CS_FREEZE || cs_state == CS_LIVE) && !p.ph_selectEnd) {
			p.ph_selectEnd = time + autocvar_ph_select_time;
			p.ph_aliveSince = time;
			if (p.ph_rerolls <= 0 && !p.ph_rerollsSet) {
				p.ph_rerolls = PH_REROLLS;
				p.ph_rerollsSet = 1;
			}
			PH_Offer(p);
		}
	}
}

float
PH_PickTeam(entity p)
{
	return CS_TEAM2;	/* the Seekers are chosen when a round starts */
}

/* "Seeker will be selected soon": one per five players (DESIGN) */
void
PH_RoundStart(void)
{
	int n = CS_Players();
	int seekers = max(1, floor(n / 5));
	int chosen = 0;

	for (entity p = world; (p = next.Player(p));) {
		if (p.team == CS_TEAM1)
			CS_SetTeam(p, CS_TEAM2);
		p.ph_thumbs = p.ph_gaveThumbs = 0;
		p.ph_diedAt = 0;
		userinfo.SetInteger(p, "csnz_thumbs", 0);
		if (!p.ph_rerollsSet) {
			p.ph_rerolls = PH_REROLLS;
			p.ph_rerollsSet = 1;
		}
	}
	/* a forced seeker for tests */
	string forced = cvars.GetString("ph_test_seeker");
	for (entity f = world; (f = next.Player(f));)
		if (forced != "" && f.netname == forced) {
			CS_SetTeam(f, CS_TEAM1);
			chosen++;
		}
	while (chosen < seekers && chosen < n - 1) {
		int pick = (int)floor(random() * n), i = 0;
		for (entity q = world; (q = next.Player(q)); i++)
			if (i == pick && q.team != CS_TEAM1) {
				CS_SetTeam(q, CS_TEAM1);
				chosen++;
			}
	}
	for (entity r = world; (r = next.Player(r));) {
		r.ph_selectEnd = -1;	/* PH_Equip: no late-join offer, this loop offers */
		CS_PutInGame(r);
		r.ph_aliveSince = time;
		if (r.team == CS_TEAM2) {
			r.ph_selectEnd = time + autocvar_ph_select_time;
			PH_Offer(r);
		} else {
			CS_Tell(r, sprintf(CS_Text("CSO_PropHunt_Msg4", "You are the Seeker. Check the map before the match starts in %d seconds."), autocvar_ph_hide_time));
			printf("studio %.1f: %s is the Seeker\n", time, r.netname);
		}
	}
}

void
PH_Live(void)
{
	ph_liveAt = time;
	ph_secondSaid = false;
	float len = cs_roundTime > 0 ? cs_roundTime : 240;
	ph_secondStart = time + len * autocvar_ph_second_at;
	ph_secondEnd = ph_secondStart + autocvar_ph_second_window;
	ph_burnAt = time + max(len - autocvar_ph_burning, ph_secondEnd - time);
	CS_Banner(sprintf(CS_Text("CSO_PropHunt_Msg3", "%d seconds until the Seeker appears."), autocvar_ph_hide_time));
}

void
PH_Second(entity p)
{
	if (p.team != CS_TEAM2 || p.health <= 0 || p.ph_second || time < ph_secondStart || time > ph_secondEnd)
		return;
	p.ph_second = 1;
	PH_Transform(p, PH_RandomProp());
}

void
PH_Frame(void)
{
	bool hiding = time < ph_liveAt + autocvar_ph_hide_time;
	bool burning = time >= ph_burnAt;

	if (time >= ph_secondStart && !ph_secondSaid) {
		ph_secondSaid = true;
		CS_Banner(sprintf(CS_Text("CSO_PropHunt_Msg6", "Perform your 2nd Transformation using the E key. You'll transform in %d seconds regardless."), autocvar_ph_second_window));
	}

	for (entity p = world; (p = next.Player(p));) {
		if (!CS_Playing(p))
			continue;
		if (p.team == CS_TEAM2) {
			/* the machine picks for those who did not choose (OBSERVED) */
			if (p.ph_selectEnd > 0 && time >= p.ph_selectEnd)
				PH_Transform(p, p.ph_offer);
			/* end of the 2nd transformation window: random (OBSERVED) */
			if (!p.ph_second && time >= ph_secondEnd && time < ph_secondEnd + 60) {
				p.ph_second = 1;
				PH_Transform(p, PH_RandomProp());
			}
			if (p.ph_locked) {
				p.velocity = [0, 0, 0];
				if (vlen(p.origin - p.ph_lockAt) > 1)
					setorigin(p, p.ph_lockAt);
			}
			continue;
		}

		/* Seeker */
		if (hiding) {
			p.velocity = [0, 0, p.velocity[2]];
			continue;
		}
		if (burning && !p.ph_burning) {
			float hp = p.health;
			vector at = p.origin, ang = p.v_angle;
			p.ph_burning = 1;
			ents.ChangeToClass(p, "csm_seeker_burning");
			CS_MoveToSpot(p, at, ang);
			ents.Input(p, "SetHealth", ftos(hp), world);
			p.ph_ammo = -1;
			CS_Banner(CS_Text("CSO_PropHunt_Burningtime_Desc", "The Seeker gets a final chance. Find all the Objects."));
		}
		/* shots: shells used; a miss costs HP outside Burning Time */
		float shells = actor.GetReserveAmmo(p, ph_shellType);
		if (p.ph_burning && shells < 20)
			ents.Input(p, "GiveAmmo", "ammo_shells 20", world);
		if (p.ph_ammo >= 0 && shells < p.ph_ammo) {
			p.ph_shots += p.ph_ammo - shells;
			if (!p.ph_shotAt)
				p.ph_shotAt = time;
		}
		p.ph_ammo = shells;
		if (p.ph_shotAt && time >= p.ph_shotAt + 0.3) {
			if (p.ph_lastHit < p.ph_shotAt && !p.ph_burning) {
				printf("studio %.1f: %s missed (%g shells)\n", time, p.netname, p.ph_shots);
				VE_Hurt(p, autocvar_ph_miss_damage * p.ph_shots, world);
			}
			p.ph_shotAt = 0;
			p.ph_shots = 0;
		}
	}
}

void
PH_Damaged(entity victim, entity attacker)
{
	if (attacker.team == CS_TEAM1 && victim.team == CS_TEAM2)
		attacker.ph_lastHit = time;
}

void
PH_Killed(entity p, entity inflictor, entity attacker, string weapon)
{
	if (attacker.team == CS_TEAM1 && p.team == CS_TEAM2) {
		attacker.ph_lastHit = time;
		attacker.cs_score++;	/* Objects Found */
	}
	p.ph_diedAt = time;
	userinfo.SetString(p, "csnz_prop", "");
}

float
PH_CheckRound(void)
{
	if (CS_Alive(CS_TEAM2) == 0)
		return CS_TEAM1;
	if (CS_Alive(CS_TEAM1) == 0)
		return CS_TEAM2;
	return 0;
}

void
PH_RoundEnded(float winner)
{
	CS_Banner(winner == CS_TEAM1 ? CS_Text("CSO_PropHunt_Result_SeekerWin", "Seeker Team Wins") : CS_Text("CSO_PropHunt_Result_HiderWin", "Object Team Wins"));
	entity best = world;
	float bestTime = -1;
	for (entity p = world; (p = next.Player(p));) {
		if (p.team != CS_TEAM2)
			continue;
		float lived = (p.ph_diedAt ? p.ph_diedAt : time) - p.ph_aliveSince;
		if (lived > bestTime) {
			bestTime = lived;
			best = p;
		}
	}
	if (best)
		printf("studio %.1f: %s: %s (%.0f s%s)\n", time, CS_Text("CSO_PropHunt_Result_AliveTime", "Longest-Lived Object"),
			best.netname, bestTime, best.health > 0 ? ", survived" : "");
}

bool
PH_Command(entity p, string cmd)
{
	if (!p)
		return false;
	tokenize(cmd);
	if (argv(0) == "csnz_use") {
		PH_Second(p);
		return p.team == CS_TEAM2;
	}
	if (argv(0) != "ph")
		return false;
	switch (argv(1)) {
	case "select":
		if (p.team == CS_TEAM2 && p.ph_selectEnd > 0 && p.health > 0)
			PH_Transform(p, p.ph_offer);
		break;
	case "reroll":
		if (p.team == CS_TEAM2 && p.ph_selectEnd > 0 && p.ph_rerolls > 0) {
			p.ph_rerolls--;
			PH_Offer(p);
		} else {
			CS_Tell(p, "No re-rolls left.");
		}
		break;
	case "rotate":
		p.ph_rot = mod(p.ph_rot + 1, 4);
		userinfo.SetInteger(p, "csnz_proprot", (int)p.ph_rot);
		break;
	case "lock":
		if (p.team == CS_TEAM2 && p.health > 0) {
			p.ph_locked = !p.ph_locked;
			p.ph_lockAt = p.origin;
			userinfo.SetBool(p, "csnz_proplock", p.ph_locked);
		}
		break;
	case "second":
		PH_Second(p);
		break;
	case "thumbs":
		/* dead players compliment once per round (OBSERVED) */
		if (p.health > 0 && p.classname != "spectator") {
			CS_Tell(p, "Compliments are given while dead.");
			break;
		}
		if (p.ph_gaveThumbs) {
			CS_Tell(p, "You already gave a compliment this round.");
			break;
		}
		for (entity q = world; (q = next.Player(q));)
			if (q.netname == argv(2) && q != p) {
				p.ph_gaveThumbs = 1;
				q.ph_thumbs++;
				userinfo.SetInteger(q, "csnz_thumbs", (int)q.ph_thumbs);
				printf("studio %.1f: %s complimented %s (%g)\n", time, p.netname, q.netname, q.ph_thumbs);
				if (q.ph_thumbs == 10)
					CS_Banner(sprintf("%s earned a Golden Thumbs Up", q.netname));
			}
		break;
	default:
		return false;
	}
	return true;
}

void
CSM_PropHunt(void)
{
	cs_modeTitle = "CSO_GameMode_VoxelPropHunt";
	cs_modeFallback = "Hide and Seek";
	cs_teams = true;
	cs_rounds = true;
	cs_respawnDelay = -1;
	cs_oneSideStart = true;
	cs_minPlayers = 2;
	cs_roundTimeWinsTeam2 = true;
	cs_winLimitFallback = 5;
	cs_roundTimeFallback = 4;
	cs_timeLimitFallback = 30;
	cs_noBuy = true;
	cs_team1Name = "Seekers";
	cs_team2Name = "Objects";
	ph_shellType = ammo.NumForName("ammo_shells");
	M_Equip = PH_Equip;
	M_PickTeam = PH_PickTeam;
	M_RoundStart = PH_RoundStart;
	M_Live = PH_Live;
	M_Frame = PH_Frame;
	M_Damaged = PH_Damaged;
	M_Killed = PH_Killed;
	M_CheckRound = PH_CheckRound;
	M_RoundEnded = PH_RoundEnded;
	M_Command = PH_Command;
	CSM_DefaultBots(6);
}
