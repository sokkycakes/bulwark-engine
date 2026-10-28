/* Vote kick in the match (handoff M3, the vote menu's rules).

   SOURCE-CONFIRMED (client.dll): the scoreboard's VoteKickButton sends
   "votekick" / "votekick2" with a "playerindex" (FUN_029f8e90,
   FUN_029fc550), and the HUD's commands include "vote_yes" and "vote_no"
   (FUN_02a52dd0). The vote window (CIGVoteMenu) shows the proposer, the
   user to kick and the reason, and takes F3 (yes) and F4 (no).
   SOURCE-CONFIRMED (mp.dll FUN_113ef8c0): the game server only prints
   #CSO_VoteResultYes2 when a kicked player leaves; the vote itself is run
   by the lobby server, which isn't part of this build.
   OBSERVED (cso_na_en.txt): four reasons (CSO_VoteReason0-3); a kick vote
   can be called 180 seconds after entering the match, a limited number of
   times per game, and only once per player (CSO_VoteKick_Btn_Tooltip1-3);
   "The player will be kicked if several players vote for the kick".

   DESIGN, because the lobby's rules are not in this build: the vote runs
   in the match for csnz_vote_time seconds (30). Every human except the
   target may vote, and the proposer counts as a yes. It passes when more
   than half of them vote yes, and fails as soon as that can no longer
   happen. A passed vote drops the target (bots too). csnz_votekick_wait
   (180, OBSERVED) and csnz_votekick_limit (3 per game, DESIGN) apply to
   the proposer. The proposer can withdraw with vote_cancel. The state is
   published as serverinfo csnz_vote_* for the HUD. */

void(entity client) dropclient = #453;	/* not in fteextensions' SSQC list; Bulwark has it */

var float autocvar_csnz_vote_time = 30;
var float autocvar_csnz_votekick_wait = 180;
var float autocvar_csnz_votekick_limit = 3;

entity vk_proposer;
entity vk_target;
float vk_reason;
float vk_end;
.float vk_ballot;		/* 0 none, 1 yes, 2 no */
.float vk_joinTime;
.float vk_calls;
.string vk_calledOn;		/* netnames this player has called a vote on */

string
VK_ReasonText(float r)
{
	switch (r) {
	case 1: return CS_Text("CSO_VoteReason1", "Unsportsmanlike conduct");
	case 2: return CS_Text("CSO_VoteReason2", "Profanity or vulgar language");
	case 3: return CS_Text("CSO_VoteReason3", "Absent from game for too long");
	}
	return CS_Text("CSO_VoteReason0", "Use of illegal program");
}

void
VK_Publish(void)
{
	float yes = 0, no = 0;

	for (entity p = world; (p = next.Player(p));) {
		if (p.vk_ballot == 1)
			yes++;
		else if (p.vk_ballot == 2)
			no++;
	}

	serverinfo.SetFloat("csnz_vote_end", vk_end);
	serverinfo.SetString("csnz_vote_by", vk_proposer ? vk_proposer.netname : "");
	serverinfo.SetString("csnz_vote_target", vk_target ? vk_target.netname : "");
	serverinfo.SetInteger("csnz_vote_targetnum", vk_target ? num_for_edict(vk_target) : 0i);
	serverinfo.SetInteger("csnz_vote_reason", (int)vk_reason);	/* the HUD shows its own CSO_VoteReason text */
	serverinfo.SetInteger("csnz_vote_yes", (int)yes);
	serverinfo.SetInteger("csnz_vote_no", (int)no);
	for (entity p = world; (p = next.Player(p));)
		userinfo.SetInteger(p, "csnz_vote_ballot", (int)p.vk_ballot);
}

void
VK_Stop(void)
{
	vk_proposer = vk_target = world;
	vk_end = 0;
	for (entity p = world; (p = next.Player(p));)
		p.vk_ballot = 0;
	VK_Publish();
}

/* the humans who may vote */
float
VK_Voters(void)
{
	float n = 0;

	for (entity p = world; (p = next.Player(p));)
		if (clienttype(p) != CLIENTTYPE_BOT && p != vk_target)
			n++;
	return n;
}

void
VK_Check(bool timeUp)
{
	float yes = 0, no = 0, voters = VK_Voters();

	if (!vk_target)
		return;

	for (entity p = world; (p = next.Player(p));) {
		if (p.vk_ballot == 1)
			yes++;
		else if (p.vk_ballot == 2)
			no++;
	}

	if (yes * 2 > voters) {
		entity t = vk_target;
		string name = t.netname;

		CS_Banner(sprintf(CS_Text("CSO_VoteResultYes", "%s has been kicked as a result of a vote."), name));
		VK_Stop();
		dropclient(t);
		return;
	}

	/* fails when a majority can no longer be reached, or time is up */
	if (timeUp || (voters - no) * 2 <= voters) {
		CS_Banner(sprintf(CS_Text("CSO_VoteResultNo", "%s has not been kicked as a result of a vote."), vk_target.netname));
		VK_Stop();
		return;
	}

	VK_Publish();
}

/* votekick <entity number or name> [reason 0-3] */
void
VK_Start(entity p, string who, float reason)
{
	entity t = world;

	if (vk_target) {
		sprint(p, PRINT_HIGH, "A vote is already underway.\n");
		return;
	}

	for (entity q = world; (q = next.Player(q));)
		if (q.netname == who || ftos(num_for_edict(q)) == who)
			t = q;

	if (!t || t == p) {
		sprint(p, PRINT_HIGH, "votekick: no such player.\n");
		return;
	}
	if (time - p.vk_joinTime < autocvar_csnz_votekick_wait) {
		sprint(p, PRINT_HIGH, sprintf(strreplace("%d", "%i", CS_Text("CSO_VoteKick_Btn_Tooltip1", "You can call a vote %d seconds after entering.")),
			(int)ceil(autocvar_csnz_votekick_wait - (time - p.vk_joinTime))));
		sprint(p, PRINT_HIGH, "\n");
		return;
	}
	if (p.vk_calls >= autocvar_csnz_votekick_limit) {
		sprint(p, PRINT_HIGH, "You can't call any more kick votes this game.\n");
		return;
	}
	if (strstrofs(strcat("\n", p.vk_calledOn, "\n"), strcat("\n", t.netname, "\n")) >= 0) {
		sprint(p, PRINT_HIGH, CS_Text("CSO_VoteKick_Btn_Tooltip3", "You've already called for a vote to kick this player."));
		sprint(p, PRINT_HIGH, "\n");
		return;
	}

	p.vk_calls++;
	p.vk_calledOn = strzone(strcat(p.vk_calledOn, "\n", t.netname));

	for (entity q = world; (q = next.Player(q));)
		q.vk_ballot = 0;
	vk_proposer = p;
	vk_target = t;
	vk_reason = reason;
	vk_end = time + autocvar_csnz_vote_time;
	p.vk_ballot = 1;
	printf("csnz %.1f: %s calls a kick vote on %s (%s)\n", time, p.netname, t.netname, VK_ReasonText(reason));
	VK_Check(false);
}

bool
VK_Command(entity p, string cmd)
{
	tokenize(cmd);

	switch (argv(0)) {
	case "votekick":
	case "votekick2":
		VK_Start(p, argv(1), stof(argv(2)));
		return (true);
	case "vote_yes":
	case "vote_no":
		if (!vk_target || p == vk_target || clienttype(p) == CLIENTTYPE_BOT)
			return (true);
		p.vk_ballot = (argv(0) == "vote_yes") ? 1 : 2;
		VK_Check(false);
		return (true);
	case "vote_cancel":
		if (vk_target && p == vk_proposer) {
			CS_Banner(sprintf(CS_Text("CSO_VoteResultNo", "%s has not been kicked as a result of a vote."), vk_target.netname));
			VK_Stop();
		}
		return (true);
	}
	return (false);
}

void
VK_Frame(void)
{
	if (vk_target && time >= vk_end)
		VK_Check(true);
}

void
VK_PlayerSpawned(entity p)
{
	if (!p.vk_joinTime)
		p.vk_joinTime = time;
}

void
VK_Disconnect(entity p)
{
	p.vk_joinTime = p.vk_calls = p.vk_ballot = 0;
	p.vk_calledOn = "";

	if (p == vk_target) {
		/* OBSERVED CSO_VoteStop: the target left during the vote */
		CS_Banner(CS_Text("CSO_VoteStop", "The player to be voted on has left the room."));
		VK_Stop();
	} else if (p == vk_proposer || vk_target) {
		if (p == vk_proposer)
			vk_proposer = world;
		VK_Check(false);
	}
}
