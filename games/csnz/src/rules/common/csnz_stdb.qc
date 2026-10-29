/* csnz_stdb: SpacetimeDB as the persistent backend of the meta-game.
 *
 * Gameplay stays here; the player data the local files hold (progression,
 * inventory, achievements, per-mode stats), the server list and the match
 * history also go to the csnz module (bulwark-engine
 * plugins/csnz/spacetimedb) over SpacetimeDB's HTTP API:
 *   POST <stdb_url>/v1/database/<csnz_stdb_db>/call/<reducer>, JSON array body
 * The engine adds the server's token (stdb_url / stdb_token cvars, from a
 * cfg kept out of git), so it never passes through here.
 *
 * Write-through: every save still writes the local file, so the server
 * keeps working when the database can't be reached. Calls that get no
 * answer (or a 5xx) are retried with backoff; a refusal is reported and
 * dropped. OBSERVED on Maincloud: a reducer that returns Err (e.g. "not an
 * operator") answers HTTP 530 with the message as the body.
 *
 * On a player's first spawn the local files load as before, then
 * get_profile's answer replaces them section by section. A section the
 * database doesn't have yet is uploaded from what was loaded locally, which
 * migrates existing saves. Until that answer, saves stay local only, so the
 * defaults of a failed load never overwrite stored data; the profile is
 * asked for again with backoff. A save made in the moment between the
 * local load and the answer is replaced by the stored data.
 *
 * Players are keyed by STDB_PlayerKey (their name today, as the local
 * files are).
 *
 * Original code (ISC). */

var float autocvar_csnz_stdb = 0;		/* 1: use SpacetimeDB */
var string autocvar_csnz_stdb_db = "";		/* the database's name or identity */
var string autocvar_csnz_stdb_address = "";	/* host:port players connect to, for the server list */
var float autocvar_csnz_stdb_heartbeat = 30;	/* seconds between server list updates */
var float autocvar_csnz_stdb_debug = 0;		/* print every call and answer */

#define STDB_QUEUE 64
#define STDB_TRIES 6
#define STDB_ID_CALL 100000	/* + queue slot */
#define STDB_ID_UNTRACKED 199999
#define STDB_ID_PROFILE 200000	/* + the player's .stdb_profileReq */

string stdb_qName[STDB_QUEUE];	/* the reducer; "" while the slot is free */
string stdb_qBody[STDB_QUEUE];
float stdb_qTries[STDB_QUEUE];
float stdb_qRetryAt[STDB_QUEUE];	/* 0 while a request is out */
float stdb_profileCount;
float stdb_nextHeartbeat;
bool stdb_announced;

.float stdb_profileReq;	/* the outstanding get_profile request, 0 when none */
.float stdb_profileDone;	/* answered: saves may go out now */
.float stdb_profileRetryAt;	/* unanswered: ask again then */
.float stdb_profileTries;

/* the other systems' parsers (csnz_progression/inventory/achievements.qc) */
void PROG_Defaults(entity p);
void PROG_ParseLine(entity p, string line);
string PROG_Text(entity p);
void INV_ParseLine(entity p, string line);
string INV_Text(entity p);
void INV_Publish(entity p);
void ACH_Reset(entity p);
void ACH_ParseLine(entity p, string line);
string ACH_Text(entity p);

bool
STDB_Enabled(void)
{
	return autocvar_csnz_stdb && autocvar_csnz_stdb_db != "" && cvar_string("stdb_url") != "";
}

string
STDB_PlayerKey(entity p)
{
	return p.netname;
}

/* a JSON string literal for s */
string
STDB_Json(string s)
{
	s = strreplace("\\", "\\\\", s);
	s = strreplace("\"", "\\\"", s);
	s = strreplace("\n", "\\n", s);
	s = strreplace("\r", "", s);
	s = strreplace("\t", "\\t", s);
	return strcat("\"", s, "\"");
}

/* the string in a procedure's answer. OBSERVED on Maincloud (2.13.0,
   2026-10-29): a Result<String, String> comes back as HTTP 200 with the
   sum's tag and value, [0,"..."] for Ok and, by the same encoding,
   [1,"..."] for Err (see STDB_IsErr) */
string
STDB_JsonString(string body)
{
	float start = strstrofs(body, "\"");
	if (start < 0)
		return body;	/* not JSON: take it as it is */

	string out = "";
	float len = strlen(body);
	float from = start + 1;
	for (float i = start + 1; i < len; i++) {
		float c = str2chr(body, i);
		if (c == '"')
			return strcat(out, substring(body, from, i - from));
		if (c != '\\')
			continue;
		out = strcat(out, substring(body, from, i - from));
		i++;
		c = str2chr(body, i);
		if (c == 'n') out = strcat(out, "\n");
		else if (c == 't') out = strcat(out, "\t");
		else if (c == 'r') { }
		else if (c == 'u') {
			/* \uXXXX: only plain ASCII comes back from the module */
			float code = 0;
			for (float h = 1; h <= 4; h++) {
				float d = str2chr(body, i + h);
				if (d >= '0' && d <= '9') d -= '0';
				else if (d >= 'a' && d <= 'f') d -= 'a' - 10;
				else if (d >= 'A' && d <= 'F') d -= 'A' - 10;
				else d = 0;
				code = code * 16 + d;
			}
			if (code > 0 && code < 128)
				out = strcat(out, chr2str(code));
			else
				out = strcat(out, "?");
			i += 4;
		} else out = strcat(out, chr2str(c));	/* \" \\ \/ */
		from = i + 1;
	}
	return strcat(out, substring(body, from, -1));
}

bool
STDB_IsErr(string body)
{
	return substring(strreplace(" ", "", body), 0, 3) == "[1,";
}

string
STDB_URL(string what)
{
	string base = cvar_string("stdb_url");
	if (substring(base, strlen(base) - 1, 1) == "/")
		base = substring(base, 0, strlen(base) - 1);
	return sprintf("%s/v1/database/%s/%s", base, autocvar_csnz_stdb_db, what);
}

void
STDB_Send(float slot)
{
	stdb_qRetryAt[slot] = 0;
	stdb_qTries[slot]++;
	if (autocvar_csnz_stdb_debug)
		print(sprintf("stdb: call %s %s\n", stdb_qName[slot], stdb_qBody[slot]));
	if (!uri_get(STDB_URL(strcat("call/", stdb_qName[slot])), STDB_ID_CALL + slot, "application/json", stdb_qBody[slot])) {
		stdb_qRetryAt[slot] = time + 5;	/* the engine is busy: try again */
	}
}

/* calls a reducer; args is the inside of the JSON argument array */
void
STDB_Call(string reducer, string args)
{
	if (!STDB_Enabled())
		return;
	args = strcat("[", args, "]");
	for (float i = 0; i < STDB_QUEUE; i++) {
		if (stdb_qName[i] != "")
			continue;
		stdb_qName[i] = strzone(reducer);
		stdb_qBody[i] = strzone(args);
		stdb_qTries[i] = 0;
		STDB_Send(i);
		return;
	}
	/* queue full (the database has been away a while): send without retries */
	print(sprintf("stdb: queue full, %s goes out without retries\n", reducer));
	uri_get(STDB_URL(strcat("call/", reducer)), STDB_ID_UNTRACKED, "application/json", args);
}

void
STDB_Free(float slot)
{
	strunzone(stdb_qName[slot]);
	strunzone(stdb_qBody[slot]);
	stdb_qName[slot] = "";
	stdb_qBody[slot] = "";
}

/* a save from csnz_progression/inventory/achievements; held back until the
   player's profile has come in, so defaults never overwrite stored data */
void
STDB_SaveSection(entity p, string reducer, string text)
{
	if (!STDB_Enabled() || clienttype(p) == CLIENTTYPE_BOT)
		return;
	if (!p.stdb_profileDone)
		return;	/* the profile's answer uploads what's missing */
	STDB_Call(reducer, sprintf("%s,%s", STDB_Json(STDB_PlayerKey(p)), STDB_Json(text)));
}

/* --- profiles ------------------------------------------------------------- */

void STDB_AskProfile(entity p);

void
STDB_LoadProfile(entity p)
{
	if (!STDB_Enabled() || clienttype(p) == CLIENTTYPE_BOT)
		return;
	STDB_Call("player_seen", sprintf("%s,%s,%s", STDB_Json(STDB_PlayerKey(p)), STDB_Json(p.netname),
		STDB_Json(autocvar_csnz_stdb_address)));
	p.stdb_profileTries = 0;
	STDB_AskProfile(p);
}

void
STDB_AskProfile(entity p)
{
	stdb_profileCount = (stdb_profileCount % 99999) + 1;
	p.stdb_profileTries++;
	p.stdb_profileRetryAt = 0;
	p.stdb_profileReq = stdb_profileCount;
	p.stdb_profileDone = false;
	if (!uri_get(STDB_URL("call/get_profile"), STDB_ID_PROFILE + p.stdb_profileReq, "application/json",
		sprintf("[%s]", STDB_Json(STDB_PlayerKey(p))))) {
		p.stdb_profileReq = 0;
		p.stdb_profileRetryAt = time + 5;
	}
}

string stdb_lines[512];

void
STDB_ApplyProfile(entity p, string text)
{
	float n = tokenizebyseparator(text, "\n");
	float count = 0;
	for (float i = 0; i < n && count < 512; i++)
		if (argv(i) != "")
			stdb_lines[count++] = strzone(argv(i));

	bool haveProg = false, haveInv = false, haveAch = false;
	string section = "";
	for (float i = 0; i < count; i++) {
		string line = stdb_lines[i];
		if (substring(line, 0, 1) == "[") {
			section = line;
			if (section == "[progression]") {
				haveProg = true;
				PROG_Defaults(p);
			} else if (section == "[inventory]") {
				haveInv = true;
			} else if (section == "[achievements]") {
				haveAch = true;
				ACH_Reset(p);
			}
			continue;
		}
		if (section == "[progression]")
			PROG_ParseLine(p, line);
		else if (section == "[inventory]")
			INV_ParseLine(p, line);
		else if (section == "[achievements]")
			ACH_ParseLine(p, line);
		/* [stats] is the database's own record: the "stats" command reads the local file */
	}
	for (float i = 0; i < count; i++)
		strunzone(stdb_lines[i]);

	p.stdb_profileDone = true;
	print(sprintf("stdb: %s's profile: progression %s, inventory %s, achievements %s\n", p.netname,
		haveProg ? "stored" : "uploaded", haveInv ? "stored" : "uploaded", haveAch ? "stored" : "uploaded"));

	/* what the database lacks comes from the local files (or the defaults) */
	if (!haveProg)
		STDB_SaveSection(p, "save_progression", PROG_Text(p));
	if (!haveInv)
		STDB_SaveSection(p, "save_inventory", INV_Text(p));
	if (!haveAch)
		STDB_SaveSection(p, "save_achievements", ACH_Text(p));
	INV_Publish(p);
}

/* --- matches and the server list -------------------------------------------- */

/* from CS_StatsRecordMatch: winnerTeam 1/2 for team modes, 0 for a draw;
   free-for-all passes its winner */
void
STDB_RecordMatch(float winnerTeam, entity ffaWinner, bool teams, bool countStats)
{
	string players = "";
	if (!STDB_Enabled())
		return;
	for (entity p = world; (p = next.Player(p));) {
		if (clienttype(p) == CLIENTTYPE_BOT)
			continue;
		bool won = teams ? (winnerTeam != 0 && p.team == winnerTeam) : (p == ffaWinner);
		players = strcat(players, sprintf("%s\t%g\t%g\t%g\t%g\n", strreplace("\t", " ", STDB_PlayerKey(p)), p.team,
			p.cs_kills, p.cs_deaths, won ? 1 : 0));
	}
	if (players == "")
		return;
	STDB_Call("record_match", sprintf("%s,%s,%d,%d,%s,%s", STDB_Json(autocvar_csnz_stdb_address), STDB_Json(mapname),
		autocvar_csnz_mode, teams ? winnerTeam : 0, countStats ? "true" : "false", STDB_Json(players)));
}

void
STDB_Heartbeat(void)
{
	float players = 0;
	if (autocvar_csnz_stdb_address == "")
		return;
	for (entity p = world; (p = next.Player(p));)
		if (clienttype(p) != CLIENTTYPE_BOT)
			players++;
	STDB_Call("server_heartbeat", sprintf("%s,%s,%s,%d,%d,%d,%s", STDB_Json(autocvar_csnz_stdb_address),
		STDB_Json(cvar_string("hostname")), STDB_Json(mapname), autocvar_csnz_mode, players,
		cvar("maxplayers") ? cvar("maxplayers") : cvar("sv_playerslots"), STDB_Json("bulwark-csnz")));
}

void
STDB_Frame(void)
{
	if (!STDB_Enabled())
		return;
	if (!stdb_announced) {
		stdb_announced = true;
		print(sprintf("stdb: using database %s at %s\n", autocvar_csnz_stdb_db, cvar_string("stdb_url")));
	}
	for (float i = 0; i < STDB_QUEUE; i++)
		if (stdb_qName[i] != "" && stdb_qRetryAt[i] && time >= stdb_qRetryAt[i])
			STDB_Send(i);
	for (entity p = world; (p = next.Player(p));)
		if (p.stdb_profileRetryAt && time >= p.stdb_profileRetryAt)
			STDB_AskProfile(p);
	if (time >= stdb_nextHeartbeat) {
		stdb_nextHeartbeat = time + max(5, autocvar_csnz_stdb_heartbeat);
		STDB_Heartbeat();
	}
}

/* --- answers ------------------------------------------------------------- */

void
URI_Get_Callback(float id, float code, string body, int len)
{
	if (autocvar_csnz_stdb_debug)
		print(sprintf("stdb: answer %d: %d %s\n", id, code, body));

	if (id >= STDB_ID_PROFILE) {
		float req = id - STDB_ID_PROFILE;
		for (entity p = world; (p = next.Player(p));) {
			if (p.stdb_profileReq != req)
				continue;
			p.stdb_profileReq = 0;
			if (code >= 200 && code < 300 && !STDB_IsErr(body)) {
				STDB_ApplyProfile(p, STDB_JsonString(body));
			} else {
				/* keep the local data, and don't let saves overwrite what
				   may be stored: ask again after 4, 8... up to 120 seconds */
				p.stdb_profileRetryAt = time + min(120, pow(2, p.stdb_profileTries + 1));
				print(sprintf("stdb: no profile for %s yet (%d %s); local data until it comes\n", p.netname, code, body));
			}
			return;
		}
		return;	/* the player left */
	}

	if (id < STDB_ID_CALL || id >= STDB_ID_CALL + STDB_QUEUE) {
		if (id == STDB_ID_UNTRACKED && (code < 200 || code >= 300))
			print(sprintf("stdb: a call failed (%d %s)\n", code, body));
		return;
	}

	float slot = id - STDB_ID_CALL;
	if (stdb_qName[slot] == "")
		return;
	if (code >= 200 && code < 300) {
		STDB_Free(slot);
	} else if ((code == 0 || (code >= 500 && code != 530)) && stdb_qTries[slot] < STDB_TRIES) {
		/* no answer or a server error: back off 2, 4, 8... seconds */
		stdb_qRetryAt[slot] = time + pow(2, stdb_qTries[slot]);
	} else {
		print(sprintf("stdb: %s failed (%d %s)\n", stdb_qName[slot], code, body));
		STDB_Free(slot);
	}
}
