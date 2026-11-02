/* uri_get/uri_post acceptance test (see run.sh): a mock web API answers
 * with what it was sent, and each request's callback prints it. Request 4
 * comes from an addprogs module, whose own URI_Get_Callback must get it. */
float g_sent, g_done;

void worldspawn(void)
{
	registercvar("uritest_base", "");
	registercvar("uritest_other", "");
}

void info_player_start(void) { }

void URI_Get_Callback(float id, float code, string body, float len)
{
	uri_line("main", id, code, body);
	g_got++;
}

void StartFrame(void)
{
	string base = cvar_string("uritest_base");
	if (g_done)
		return;
	if (!g_sent) {
		if (time < 0.5)
			return;
		g_sent = time;
		/* 1: a reducer call: POST, JSON, the token added by the engine */
		uri_get(strcat(base, "/v1/database/csnz/call/player_seen"), 1, "application/json", "[\"sokky\"]");
		/* 2: a GET under stdb_url also carries the token */
		uri_get(strcat(base, "/v1/database/csnz/schema?version=9"), 2);
		/* 3: another host never gets it */
		uri_get(strcat(cvar_string("uritest_other"), "/v1/database/csnz/call/player_seen"), 3, "application/json", "[]");
		/* 4: from the addprogs module (its init() sends it) */
		addprogs("uriadd.dat");
		/* 5: an error status still reaches the callback with its body */
		uri_get(strcat(base, "/err"), 5, "application/json", "[]");
		/* 6: the method extension */
		uri_get(strcat(base, "/v1/database/csnz"), 6, "PUT application/json", "{}");
		return;
	}
	if (g_got >= 5 || time - g_sent > 10) {
		print(sprintf("DONE main got %d\n", g_got));
		g_done = 1;
		localcmd("quit\n");
	}
}
