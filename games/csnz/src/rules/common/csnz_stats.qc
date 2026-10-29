/* csnz_stats: per-player match records kept across matches and server
   restarts, in data/csnz/stats.txt. One line per player and mode:
     name <TAB> mode <TAB> matches <TAB> wins <TAB> kills <TAB> deaths
   OBSERVED: bot modes and Team Deathmatch Item Battle do not accumulate
   kill/death points ("Kill/Death points are not accumulated in Bot Team
   Deathmatch Mode"), so those modes set cs_noStats. Bots are never
   recorded. The previous file is kept as stats.bak before each write. */

#define CS_STATS_FILE "csnz/stats.txt"
#define CS_STATS_BAK "csnz/stats.bak"

bool cs_noStats;
string cs_statLines[512];
int cs_statCount;

void
CS_StatsLoad(void)
{
	filestream f;
	string s;

	for (int i = 0; i < cs_statCount; i++)
		strunzone(cs_statLines[i]);
	cs_statCount = 0;

	f = fopen(CS_STATS_FILE, FILE_READ);
	if (f < 0)
		return;
	while (cs_statCount < 512) {
		s = fgets(f);
		if not (s)	/* end of file; "" is a blank line */
			break;
		if (tokenizebyseparator(s, "\t") != 6)
			continue;	/* damaged line: dropped */
		cs_statLines[cs_statCount++] = strzone(s);
	}
	fclose(f);
}

bool
CS_StatsSave(void)
{
	filestream f;
	filestream old = fopen(CS_STATS_FILE, FILE_READ);

	/* keep the last good file */
	if (old >= 0) {
		filestream bak = fopen(CS_STATS_BAK, FILE_WRITE);
		string s;
		while (1) {
			s = fgets(old);
			if not (s)	/* end of file; "" is a blank line */
				break;
			if (bak >= 0)
				fputs(bak, strcat(s, "\n"));
		}
		fclose(old);
		if (bak >= 0)
			fclose(bak);
	}

	f = fopen(CS_STATS_FILE, FILE_WRITE);
	if (f < 0) {
		print("csnz: cannot write data/" CS_STATS_FILE "\n");
		return false;
	}
	for (int i = 0; i < cs_statCount; i++)
		fputs(f, strcat(cs_statLines[i], "\n"));
	fclose(f);
	return true;
}

/* adds one match to a player's record for the current mode */
void
CS_StatsAdd(string name, float mode, bool won, float kills, float deaths)
{
	int i;
	name = strreplace("\t", " ", name);

	for (i = 0; i < cs_statCount; i++) {
		tokenizebyseparator(cs_statLines[i], "\t");
		if (argv(0) == name && stof(argv(1)) == mode)
			break;
	}

	float matches = 0, wins = 0, k = 0, d = 0;
	if (i < cs_statCount) {
		matches = stof(argv(2));
		wins = stof(argv(3));
		k = stof(argv(4));
		d = stof(argv(5));
		strunzone(cs_statLines[i]);
	} else if (cs_statCount < 512) {
		cs_statCount++;
	} else {
		return;
	}

	matches++;
	if (won)
		wins++;
	cs_statLines[i] = strzone(sprintf("%s\t%g\t%g\t%g\t%g\t%g", name, mode, matches, wins, k + kills, d + deaths));
}

/* winnerTeam: 1/2 for team modes, 0 for a draw; FFA passes the winner */
void
CS_StatsRecordMatch(float winnerTeam, entity ffaWinner)
{
	int n = 0;

	STDB_RecordMatch(winnerTeam, ffaWinner, cs_teams, !cs_noStats);
	if (cs_noStats) {
		print("csnz: this mode does not record statistics\n");
		return;
	}

	CS_StatsLoad();
	for (entity p = world; (p = next.Player(p));) {
		if (clienttype(p) == CLIENTTYPE_BOT)
			continue;
		bool won = cs_teams ? (winnerTeam != 0 && p.team == winnerTeam) : (p == ffaWinner);
		CS_StatsAdd(p.netname, autocvar_csnz_mode, won, p.cs_kills, p.cs_deaths);
		n++;
	}
	if (n)	/* && does not short-circuit: no save when nobody played */
		if (CS_StatsSave())
			printf("csnz: recorded %i players in data/%s\n", n, CS_STATS_FILE);
}

/* "stats" console/client command: prints a player's records */
void
CS_StatsPrint(entity to, string name)
{
	CS_StatsLoad();
	for (int i = 0; i < cs_statCount; i++) {
		tokenizebyseparator(cs_statLines[i], "\t");
		if (name != "" && argv(0) != name)
			continue;
		string line = sprintf("%s mode %s: %s matches, %s wins, %s kills, %s deaths\n", argv(0), argv(1), argv(2), argv(3), argv(4), argv(5));
		if (to)
			sprint(to, PRINT_HIGH, line);
		else
			print(line);
	}
}

/* test hook: cs_test_stats 1 records a match for "tester" twice, adds a
   damaged line, reloads and prints the record (serialisation check) */
void
CS_StatsTest(void)
{
	if (!cvars.GetBool("cs_test_stats"))
		return;
	cvars.SetString("cs_test_stats", "0");

	CS_StatsLoad();
	CS_StatsAdd("tester", autocvar_csnz_mode, true, 5, 2);
	CS_StatsAdd("tester", autocvar_csnz_mode, false, 1, 3);
	CS_StatsSave();

	filestream f = fopen(CS_STATS_FILE, FILE_APPEND);
	if (f >= 0) {
		fputs(f, "damaged line without tabs\n");
		fclose(f);
	}

	CS_StatsLoad();
	for (int i = 0; i < cs_statCount; i++) {
		tokenizebyseparator(cs_statLines[i], "\t");
		if (argv(0) == "tester" && stof(argv(1)) == autocvar_csnz_mode)
			printf("csnz: stats tester matches %s wins %s kills %s deaths %s (%i lines kept)\n", argv(2), argv(3), argv(4), argv(5), cs_statCount);
	}
}
