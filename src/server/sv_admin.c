/* Bulwark engine - server moderation from FTE: penalties (mute, stealthmute,
 * cuff, cripple), chat flood protection, the address log behind "identify",
 * player, rcon and frag logs, and rankings (ranked names with passwords,
 * rights and kept scores).
 * ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../net/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define F sv.vm->fofs

static cvar_t *sv_floodprotect, *sv_floodprotect_messages, *sv_floodprotect_interval, *sv_floodprotect_silencetime,
	*sv_floodprotect_sendmessage;
static cvar_t *log_enable_players, *log_name_players, *log_enable_rcon, *log_name_rcon, *log_dir, *log_timestamps;
static cvar_t *rank_filename, *rank_autoadd, *rank_needlogin, *rank_parms_first, *rank_parms_last, *sv_cmdlikercon;

/* a player's address without the port, which is what identifies them */
static const char *base_ip(const netadr_t *a)
{
	static char s[64];
	char *colon;
	Str_Copy(s, NET_AdrToString(a), sizeof(s));
	if (s[0] == '[') {
		char *end = strchr(s, ']');
		if (end)
			end[1] = 0;
	} else if ((colon = strrchr(s, ':')) && strchr(s, ':') == colon) {
		*colon = 0;
	}
	return s;
}

static const char *client_ip(const client_t *c) { return c->bot ? "bot" : base_ip(&c->chan.remote); }

static int num_connected(void)
{
	int n = 0;
	for (int i = 0; i < svs.maxclients; i++)
		n += svs.clients[i].state != CS_FREE;
	return n;
}

/* --- penalties ------------------------------------------------------------------------------- */

static const char *const penalty_names[PEN_KINDS] = {"mute", "stealthmute", "cuff", "cripple"};
static const char *const penalty_told[PEN_KINDS] = {"muted", NULL, "cuffed", "crippled"};

/* by address, so reconnecting doesn't lift them; bots by name */
typedef struct {
	char ip[64];
	int kind;
	double expires; /* 0: until the server stops */
	char reason[128];
} penalty_t;
static penalty_t *penalties;
static int numpenalties;

static void expire_penalties(void)
{
	double now = Sys_Seconds();
	for (int i = 0; i < numpenalties;)
		if (penalties[i].expires && penalties[i].expires < now)
			penalties[i] = penalties[--numpenalties];
		else
			i++;
}

static const char *penalty_key(const client_t *c) { return c->bot ? c->name : client_ip(c); }

int SV_Penalties(const client_t *c)
{
	int flags = 0;
	if (!numpenalties)
		return 0;
	expire_penalties();
	for (int i = 0; i < numpenalties; i++)
		if (!strcmp(penalties[i].ip, penalty_key(c)))
			flags |= 1 << penalties[i].kind;
	return flags;
}

/* mute, stealthmute, cuff, cripple <player> [minutes] [reason]; 0 minutes
 * lifts it, none keeps it until the server stops */
static void Penalty_f(int argc, const char **argv)
{
	int kind = 0;
	client_t *c;
	double minutes = 0;
	for (int i = 0; i < PEN_KINDS; i++)
		if (!Str_ICmp(argv[0], penalty_names[i]))
			kind = i;
	if (argc < 2) {
		int n = 0;
		if (!num_connected()) {
			Con_Printf("%s: no clients\n", argv[0]);
			return;
		}
		expire_penalties();
		for (int i = 0; i < numpenalties; i++)
			if (penalties[i].kind == kind) {
				Con_Printf("%-20s %s %s\n", penalties[i].ip,
				           penalties[i].expires ? va("%.1f min", (penalties[i].expires - Sys_Seconds()) / 60) : "until restart",
				           penalties[i].reason);
				n++;
			}
		Con_Printf("%d with %s; usage: %s <player> [minutes, 0 lifts] [reason]\n", n, argv[0], argv[0]);
		return;
	}
	if (!(c = SV_FindClient(argv[1]))) {
		Con_Printf("%s: no player %s\n", argv[0], argv[1]);
		return;
	}
	if (argc > 2)
		minutes = atof(argv[2]);
	for (int i = 0; i < numpenalties; i++)
		if (penalties[i].kind == kind && !strcmp(penalties[i].ip, penalty_key(c))) {
			penalties[i--] = penalties[--numpenalties];
		}
	if (argc > 2 && minutes <= 0) {
		Con_Printf("%s: lifted from %s\n", argv[0], c->name);
		if (penalty_told[kind])
			SV_ClientPrint(c, 2, va("You are no longer %s.\n", penalty_told[kind]));
		return;
	}
	penalties = Mem_Realloc(penalties, sizeof(penalty_t) * (size_t)(numpenalties + 1));
	{
		penalty_t *p = &penalties[numpenalties++];
		memset(p, 0, sizeof(*p));
		Str_Copy(p->ip, penalty_key(c), sizeof(p->ip));
		p->kind = kind;
		p->expires = minutes > 0 ? Sys_Seconds() + minutes * 60 : 0;
		Str_Copy(p->reason, argc > 3 ? argv[3] : "", sizeof(p->reason));
	}
	Con_Printf("%s: %s (%s)%s\n", argv[0], c->name, penalty_key(c), minutes > 0 ? va(" for %g minutes", minutes) : "");
	if (penalty_told[kind])
		SV_ClientPrint(c, 2, va("You have been %s%s%s.\n", penalty_told[kind], argc > 3 ? ": " : "", argc > 3 ? argv[3] : ""));
}

/* cuffed players can't attack, crippled ones can't move */
void SV_AdminFilterMove(client_t *c, usercmd_t *cmd)
{
	int p = SV_Penalties(c);
	if (p & PEN_CUFF)
		cmd->buttons &= ~(1u | 4u);
	if (p & PEN_CRIPPLE) {
		cmd->movevalues[0] = cmd->movevalues[1] = cmd->movevalues[2] = 0;
		cmd->buttons &= ~2u;
	}
}

/* --- chat ------------------------------------------------------------------------------------- */

/* say and say_team, before the game sees them: muted players are told so,
 * flooders are silenced, and stealthmuted players' chat goes only to them */
int SV_AdminFilterChat(client_t *c)
{
	double now = Sys_Seconds();
	int p = SV_Penalties(c);
	if (p & PEN_MUTE) {
		SV_ClientPrint(c, 2, "You are muted.\n");
		return CHAT_DROP;
	}
	if (sv_floodprotect->ivalue && sv_floodprotect_messages->ivalue > 0) {
		int n = BW_MIN(sv_floodprotect_messages->ivalue, (int)BW_ARRAY_COUNT(c->chat_times));
		if (c->silenced_until > now) {
			if (sv_floodprotect_sendmessage->string[0])
				SV_ClientPrint(c, 2, va("%s\n", sv_floodprotect_sendmessage->string));
			else
				SV_ClientPrint(c, 2, va("You can't talk for %d more seconds\n", (int)(c->silenced_until - now + 0.999)));
			return CHAT_DROP;
		}
		/* the nth message back was too recent */
		{
			double nth = c->chat_times[(c->chat_head + BW_ARRAY_COUNT(c->chat_times) - (size_t)n) % BW_ARRAY_COUNT(c->chat_times)];
			c->chat_times[c->chat_head] = now;
			c->chat_head = (c->chat_head + 1) % (int)BW_ARRAY_COUNT(c->chat_times);
			if (nth > 0 && now - nth < sv_floodprotect_interval->value) {
				c->silenced_until = now + sv_floodprotect_silencetime->value;
				SV_ClientPrint(c, 2, va("FloodProt: You can't talk for %g seconds.\n", sv_floodprotect_silencetime->value));
				return CHAT_DROP;
			}
		}
	}
	return (p & PEN_STEALTH) ? CHAT_ONLY_SENDER : CHAT_PASS;
}

/* floodprot [messages seconds silence] */
static void Floodprot_f(int argc, const char **argv)
{
	if (argc < 4) {
		Con_Printf("Current flood protection settings: \n");
		if (!sv_floodprotect->ivalue || sv_floodprotect_messages->ivalue <= 0)
			Con_Printf("Flood protection is off.\n");
		else
			Con_Printf("After %d msgs for %g seconds, silence for %g seconds\n", sv_floodprotect_messages->ivalue,
			           sv_floodprotect_interval->value, sv_floodprotect_silencetime->value);
		if (argc > 1)
			Con_Printf("floodprot <messages> <seconds> <silence seconds> (messages 0 turns it off)\n");
		return;
	}
	Cvar_Set("sv_floodprotect_messages", argv[1]);
	Cvar_Set("sv_floodprotect_interval", argv[2]);
	Cvar_Set("sv_floodprotect_silencetime", argv[3]);
	Cvar_Set("sv_floodprotect", atoi(argv[1]) > 0 ? "1" : "0");
}

/* --- logs --------------------------------------------------------------------------------------- */

static void log_line(const char *name, const char *text)
{
	char rel[256], path[1024];
	FILE *f;
	snprintf(rel, sizeof(rel), "%s%s%s.log", log_dir->string, log_dir->string[0] ? "/" : "", name);
	if (!FS_WritePath(rel, path, sizeof(path)))
		return;
	FS_CreatePath(path);
	if (!(f = fopen(path, "ab")))
		return;
	if (log_timestamps->ivalue) {
		char stamp[64];
		time_t t = time(NULL);
		strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S ", localtime(&t));
		fputs(stamp, f);
	}
	fputs(text, f);
	fclose(f);
}

void SV_LogRcon(const netadr_t *from, const char *command)
{
	if (log_enable_rcon->ivalue)
		log_line(log_name_rcon->string, va("%s: %s\n", NET_AdrToString(from), command));
}

static void log_toggle(cvar_t *enable, cvar_t *name, const char *what)
{
	Cvar_Set(enable->name, enable->ivalue ? "0" : "1");
	if (enable->ivalue)
		Con_Printf("Logging %s to %s/%s%s%s.log\n", what, FS_GameDir(), log_dir->string, log_dir->string[0] ? "/" : "", name->string);
	else
		Con_Printf("Logging %s disabled.\n", what);
}

static void Logplayers_f(int argc, const char **argv) { log_toggle(log_enable_players, log_name_players, "players"); }
static void Logrcon_f(int argc, const char **argv) { log_toggle(log_enable_rcon, log_name_rcon, "rcon"); }

/* fraglogfile: QuakeWorld's frag log, "\killer\victim\" lines from the
 * game's logfrag calls, into the first free frag_<n>.log */
static FILE *fraglog;

static void Fraglogfile_f(int argc, const char **argv)
{
	char rel[64], path[1024];
	if (fraglog) {
		fclose(fraglog);
		fraglog = NULL;
		Con_Printf("Frag file logging off.\n");
		return;
	}
	for (int i = 0; i < 1000; i++) {
		snprintf(rel, sizeof(rel), "frag_%d.log", i);
		if (FS_WhichPath(rel))
			continue;
		if (FS_WritePath(rel, path, sizeof(path)) && (fraglog = fopen(path, "wb"))) {
			Con_Printf("Logging frags to %s.\n", rel);
			return;
		}
		break;
	}
	Con_Printf("Couldn't open a frag log.\n");
}

void SV_LogFrag(int killer, int victim)
{
	client_t *k = SV_ClientForEdict(killer), *v = SV_ClientForEdict(victim);
	if (!fraglog || !k || !v)
		return;
	fprintf(fraglog, "\\%s\\%s\\\n", k->name, v->name);
	fflush(fraglog);
}

/* --- address log ---------------------------------------------------------------------------------- */

typedef struct {
	char ip[64];
	char name[64];
} iplog_t;
static iplog_t *iplog;
static int numiplog;

static void iplog_add(const char *ip, const char *name)
{
	for (int i = 0; i < numiplog; i++)
		if (!strcmp(iplog[i].ip, ip) && !strcmp(iplog[i].name, name))
			return;
	iplog = Mem_Realloc(iplog, sizeof(iplog_t) * (size_t)(numiplog + 1));
	Str_Copy(iplog[numiplog].ip, ip, sizeof(iplog[0].ip));
	Str_Copy(iplog[numiplog].name, name, sizeof(iplog[0].name));
	numiplog++;
}

/* identify <player or address>: the other names that address has used */
static void Identify_f(int argc, const char **argv)
{
	const char *ip;
	client_t *c;
	int n = 0;
	if (argc < 2) {
		Con_Printf("identify <player or address>: the names an address has used\n");
		return;
	}
	ip = (c = SV_FindClient(argv[1])) ? client_ip(c) : argv[1];
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].state != CS_FREE && !svs.clients[i].bot)
			iplog_add(client_ip(&svs.clients[i]), svs.clients[i].name);
	for (int i = 0; i < numiplog; i++)
		if (!strcmp(iplog[i].ip, ip)) {
			Con_Printf("%s%s", n ? ", " : va("%s has been seen as: ", ip), iplog[i].name);
			n++;
		}
	Con_Printf(n ? "\n" : "%s isn't in the address log\n", ip);
}

/* ipdump [file]: the address log, as lines ipmerge reads back */
static void Ipdump_f(int argc, const char **argv)
{
	const char *name = argc > 1 ? argv[1] : "iplog.txt";
	char path[1024];
	FILE *f;
	if (!numiplog) {
		Con_Printf("nothing to write\n");
		return;
	}
	if (!FS_WritePath(name, path, sizeof(path)) || !(f = fopen(path, "wb"))) {
		Con_Printf("couldn't write %s\n", name);
		return;
	}
	for (int i = 0; i < numiplog; i++)
		fprintf(f, "%s \"%s\"\n", iplog[i].ip, iplog[i].name);
	fclose(f);
	Con_Printf("wrote %d entries to %s\n", numiplog, name);
}

static void Ipmerge_f(int argc, const char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";
	size_t len;
	char *data = name[0] ? FS_LoadFile(name, &len) : NULL;
	int before = numiplog;
	if (!data) {
		Con_Printf("unable to read iplog \"%s\" for merging\n", name);
		return;
	}
	for (char *line = data; line && *line;) {
		char *next = strchr(line, '\n');
		cmd_tokens_t *t = Mem_Alloc(sizeof(*t));
		if (next)
			*next++ = 0;
		Cmd_TokenizeInto(t, line);
		if (t->argc >= 2)
			iplog_add(t->argv[0], t->argv[1]);
		Mem_Free(t);
		line = next;
	}
	Mem_Free(data);
	Con_Printf("merged %d new entries from %s\n", numiplog - before, name);
}

/* --- rankings ------------------------------------------------------------------------------------- */

typedef struct {
	char name[64];
	char pwd[64];
	int rights;
	float score, deaths;
	double timeonserver;
	long created;
	float parms[MAX_SPAWN_PARMS];
} rank_t;
static rank_t *ranks;
static int numranks;
static bool ranks_loaded;
static char ranks_file[256];

static bool load_ranks(void)
{
	size_t len;
	char *data;
	if (!rank_filename->string[0])
		return false;
	if (ranks_loaded && !strcmp(ranks_file, rank_filename->string))
		return true;
	Mem_Free(ranks);
	ranks = NULL;
	numranks = 0;
	Str_Copy(ranks_file, rank_filename->string, sizeof(ranks_file));
	ranks_loaded = true;
	if (!(data = FS_LoadFile(ranks_file, &len)))
		return true; /* a new file */
	for (char *line = data; line && *line;) {
		char *next = strchr(line, '\n');
		cmd_tokens_t *t = Mem_Alloc(sizeof(*t));
		if (next)
			*next++ = 0;
		Cmd_TokenizeInto(t, line);
		if (t->argc >= 7) {
			rank_t *r;
			ranks = Mem_Realloc(ranks, sizeof(rank_t) * (size_t)(numranks + 1));
			r = &ranks[numranks++];
			memset(r, 0, sizeof(*r));
			Str_Copy(r->name, t->argv[0], sizeof(r->name));
			Str_Copy(r->pwd, t->argv[1], sizeof(r->pwd));
			r->rights = atoi(t->argv[2]);
			r->score = (float)atof(t->argv[3]);
			r->deaths = (float)atof(t->argv[4]);
			r->timeonserver = atof(t->argv[5]);
			r->created = atol(t->argv[6]);
			for (int i = 7; i < t->argc && i - 7 < MAX_SPAWN_PARMS; i++)
				r->parms[i - 7] = (float)atof(t->argv[i]);
		}
		Mem_Free(t);
		line = next;
	}
	Mem_Free(data);
	return true;
}

/* one line each: "name" "password" rights score deaths seconds created parms... */
static void save_ranks(void)
{
	char path[1024];
	FILE *f;
	if (!rank_filename->string[0] || !FS_WritePath(rank_filename->string, path, sizeof(path)) || !(f = fopen(path, "wb")))
		return;
	for (int i = 0; i < numranks; i++) {
		rank_t *r = &ranks[i];
		fprintf(f, "%s %s %d %g %g %.0f %ld", Str_NeedsEscape(r->name) ? va("\\\"%s\"", r->name) : va("\"%s\"", r->name),
		        va("\"%s\"", r->pwd), r->rights, r->score, r->deaths, r->timeonserver, r->created);
		for (int p = 0; p < MAX_SPAWN_PARMS; p++)
			fprintf(f, " %g", r->parms[p]);
		fputc('\n', f);
	}
	fclose(f);
}

static bool rank_open(void)
{
	if (load_ranks())
		return true;
	Con_Printf("Failed to open rankings file.\n");
	return false;
}

static bool contains_nocase(const char *hay, const char *needle)
{
	for (size_t n = strlen(needle); *hay; hay++)
		if (!Str_NICmp(hay, needle, n))
			return true;
	return !*needle;
}

static int find_rank(const char *name)
{
	for (int i = 0; i < numranks; i++)
		if (!strcmp(ranks[i].name, name))
			return i;
	return -1;
}

static int add_rank(const char *name, const char *pwd, int rights)
{
	rank_t *r;
	ranks = Mem_Realloc(ranks, sizeof(rank_t) * (size_t)(numranks + 1));
	r = &ranks[numranks];
	memset(r, 0, sizeof(*r));
	Str_Copy(r->name, name, sizeof(r->name));
	Str_Copy(r->pwd, pwd ? pwd : "", sizeof(r->pwd));
	r->rights = BW_CLAMP(rights, 0, RESTRICT_MAX);
	r->created = (long)time(NULL);
	return numranks++;
}

/* the ranking number someone typed, from 1 */
static rank_t *rank_arg(const char *s)
{
	int n = atoi(s);
	if (n < 1 || n > numranks) {
		Con_Printf("no ranking %s (see ranklist)\n", s);
		return NULL;
	}
	return &ranks[n - 1];
}

/* rankadd/adduser <name> [password] [rights] */
static void Rankadd_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("%s: <name> [pwd] [rights]\n", argv[0]);
		return;
	}
	if (!rank_open())
		return;
	if (find_rank(argv[1]) >= 0) {
		Con_Printf("%s already has a ranking\n", argv[1]);
		return;
	}
	Con_Printf("%s is ranking %d\n", argv[1],
	           add_rank(argv[1], argc > 2 ? argv[2] : "", argc > 3 ? BW_MIN(atoi(argv[3]), Cmd_ExecLevel()) : 0) + 1);
	save_ranks();
}

static void Rankremove_f(int argc, const char **argv)
{
	rank_t *r;
	if (argc < 2) {
		Con_Printf("Removes a ranking entry.\nUse ranklist to find the entry number.");
		return;
	}
	if (!rank_open() || !(r = rank_arg(argv[1])))
		return;
	Con_Printf("removed %s\n", r->name);
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->rank == (int)(r - ranks) + 1)
			c->rank = 0;
		else if (c->rank > (int)(r - ranks) + 1)
			c->rank--;
	}
	memmove(r, r + 1, sizeof(*r) * (size_t)(numranks - (r - ranks) - 1));
	numranks--;
	save_ranks();
}

static void print_rank(int i)
{
	rank_t *r = &ranks[i];
	Con_Printf("%4d %-24s %2d %6g %6g %5.0fm%s\n", i + 1, r->name, r->rights, r->score, r->deaths, r->timeonserver / 60,
	           r->pwd[0] ? " (password)" : "");
}

static void Ranklist_f(int argc, const char **argv)
{
	if (!rank_open())
		return;
	Con_Printf(" num name                     rt  score deaths  time\n");
	for (int i = 0; i < numranks; i++)
		print_rank(i);
	Con_Printf("%d rankings in %s\n", numranks, rank_filename->string);
}

static void Rankfind_f(int argc, const char **argv)
{
	int n = 0;
	if (!rank_open())
		return;
	if (argc < 2) {
		Con_Printf("rankfind <name>\n");
		return;
	}
	for (int i = 0; i < numranks; i++)
		if (contains_nocase(ranks[i].name, argv[1])) {
			print_rank(i);
			n++;
		}
	if (!n)
		Con_Printf("no rankings match %s\n", argv[1]);
}

static void Rankrconlevel_f(int argc, const char **argv)
{
	rank_t *r;
	if (!rank_open())
		return;
	if (argc < 3) {
		Con_Printf("rankrconlevel <ranking> <rights 0-%d>\n", RESTRICT_MAX);
		return;
	}
	if (!(r = rank_arg(argv[1])))
		return;
	r->rights = BW_CLAMP(atoi(argv[2]), 0, BW_MIN(RESTRICT_MAX, Cmd_ExecLevel()));
	Con_Printf("%s has rights %d\n", r->name, r->rights);
	save_ranks();
}

static void Setpass_f(int argc, const char **argv)
{
	rank_t *r;
	if (!rank_open())
		return;
	if (argc < 2) {
		Con_Printf("setpass <ranking> [password]\n");
		return;
	}
	if (!(r = rank_arg(argv[1])))
		return;
	Str_Copy(r->pwd, argc > 2 ? argv[2] : "", sizeof(r->pwd));
	Con_Printf("%s's password %s\n", r->name, r->pwd[0] ? "set" : "cleared");
	save_ranks();
}

/* the connected ranked players' scores and time go into their rankings */
static void refresh_rank(client_t *c)
{
	rank_t *r;
	if (!c->rank || c->rank > numranks || !sv.vm)
		return;
	r = &ranks[c->rank - 1];
	{
		qcdef_t *frags = QC_FindField(sv.vm, "frags");
		float now = frags ? QC_EdictFields(sv.vm, c->edict)[frags->ofs].f : 0;
		r->score += now - c->rank_frags;
		c->rank_frags = now;
	}
	r->timeonserver += host_time - c->rank_since;
	c->rank_since = host_time;
	for (int p = rank_parms_first->ivalue; p <= rank_parms_last->ivalue && p < MAX_SPAWN_PARMS; p++)
		if (p >= 0 && c->have_parms)
			r->parms[p] = c->spawn_parms[p];
}

static void Rankrefresh_f(int argc, const char **argv)
{
	if (!rank_open())
		return;
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].state != CS_FREE)
			refresh_rank(&svs.clients[i]);
	save_ranks();
}

static int by_score(const void *a, const void *b)
{
	const rank_t *x = &ranks[*(const int *)a], *y = &ranks[*(const int *)b];
	return x->score < y->score ? 1 : x->score > y->score ? -1 : 0;
}

static void topten(client_t *to)
{
	int order[10], n = 0;
	int *all;
	if (!load_ranks()) {
		if (to)
			SV_ClientPrint(to, 2, "This server keeps no rankings.\n");
		else
			Con_Printf("Failed to open rankings file.\n");
		return;
	}
	all = Mem_Alloc(sizeof(int) * (size_t)(numranks + 1));
	for (int i = 0; i < numranks; i++)
		all[i] = i;
	qsort(all, (size_t)numranks, sizeof(int), by_score);
	n = BW_MIN(numranks, 10);
	memcpy(order, all, sizeof(int) * (size_t)n);
	Mem_Free(all);
	for (int i = 0; i < n; i++) {
		const char *line = va("%2d %-24s %6g\n", i + 1, ranks[order[i]].name, ranks[order[i]].score);
		if (to)
			SV_ClientPrint(to, 2, line);
		else
			Con_Print(line);
	}
	if (!n) {
		if (to)
			SV_ClientPrint(to, 2, "No rankings yet.\n");
		else
			Con_Printf("No rankings yet.\n");
	}
}

static void Ranktopten_f(int argc, const char **argv) { topten(NULL); }

/* the rights rcon gets for "<ranked name>:<password>", -1 if it isn't one */
int SV_RankRconLevel(const char *login)
{
	char name[64];
	const char *colon = strrchr(login, ':');
	int r;
	if (!colon || !load_ranks())
		return -1;
	Str_Copy(name, login, BW_MIN(sizeof(name), (size_t)(colon - login) + 1));
	r = find_rank(name);
	if (r < 0 || !ranks[r].pwd[0] || strcmp(ranks[r].pwd, colon + 1) || ranks[r].rights <= 0)
		return -1;
	return ranks[r].rights;
}

/* --- connecting and leaving ------------------------------------------------------------------------ */

/* before a connection is accepted: ranked names need their password (the
 * _pwd userinfo key), and rank_needlogin turns away unranked names */
bool SV_AdminCheckConnect(const char *userinfo, char *why, size_t whysize)
{
	char name[64], pwd[64];
	int r;
	if (!load_ranks())
		return true;
	SV_InfoGet(userinfo, "name", name, sizeof(name));
	SV_InfoGet(userinfo, "_pwd", pwd, sizeof(pwd));
	r = find_rank(name);
	if (r >= 0 && ranks[r].pwd[0] && strcmp(ranks[r].pwd, pwd)) {
		Str_Copy(why, "Invalid password for that ranked name (setinfo _pwd)", whysize);
		return false;
	}
	if (r < 0 && rank_needlogin->ivalue && !rank_autoadd->ivalue) {
		Str_Copy(why, "This server needs a ranked name", whysize);
		return false;
	}
	return true;
}

void SV_AdminConnected(client_t *c)
{
	if (!c->bot)
		iplog_add(client_ip(c), c->name);
	if (log_enable_players->ivalue)
		log_line(log_name_players->string, va("connect \"%s\" %s\n", c->name, client_ip(c)));
	c->rank = 0;
	if (load_ranks()) {
		int r = find_rank(c->name);
		if (r < 0 && rank_autoadd->ivalue && !c->bot) {
			r = add_rank(c->name, "", 0);
			save_ranks();
		}
		if (r >= 0) {
			c->rank = r + 1;
			c->rank_since = host_time;
			c->rank_frags = 0;
			/* the kept parms carry over into the game */
			if (rank_parms_last->ivalue >= rank_parms_first->ivalue && !c->have_parms) {
				for (int p = BW_MAX(0, rank_parms_first->ivalue); p <= rank_parms_last->ivalue && p < MAX_SPAWN_PARMS; p++)
					c->spawn_parms[p] = ranks[r].parms[p];
			}
		}
	}
}

void SV_AdminDisconnected(client_t *c)
{
	if (log_enable_players->ivalue)
		log_line(log_name_players->string, va("disconnect \"%s\" %s %.0fs\n", c->name, client_ip(c), host_time - c->connect_time));
	if (c->rank && load_ranks()) {
		refresh_rank(c);
		save_ranks();
	}
	c->rank = 0;
}

/* a name change: logged, and checked like a connection */
void SV_AdminRenamed(client_t *c, const char *oldname)
{
	if (!c->bot)
		iplog_add(client_ip(c), c->name);
	if (log_enable_players->ivalue)
		log_line(log_name_players->string, va("rename \"%s\" \"%s\" %s\n", oldname, c->name, client_ip(c)));
}

/* --- client commands ---------------------------------------------------------------------------- */

/* "topten", and with sv_cmdlikercon, console commands from ranked players
 * with rights, run at those rights and answered to them */
bool SV_AdminClientCommand(client_t *c, int argc, const char **argv, const char *line)
{
	if (!Str_ICmp(argv[0], "topten")) {
		topten(c);
		return true;
	}
	if (sv_cmdlikercon->ivalue && c->rank && c->rank <= numranks && ranks[c->rank - 1].rights > 0) {
		strbuf_t out = {0};
		Con_Printf("%s (rights %d): %s\n", c->name, ranks[c->rank - 1].rights, line);
		Con_Redirect(&out);
		Cmd_ExecuteStringAtLevel(line, ranks[c->rank - 1].rights);
		Con_Redirect(NULL);
		for (size_t ofs = 0; ofs < out.len; ofs += 1000)
			SV_ClientPrint(c, 2, va("%.*s", (int)BW_MIN(out.len - ofs, (size_t)1000), out.data + ofs));
		SB_Free(&out);
		return true;
	}
	return false;
}

/* --- other console commands ------------------------------------------------------------------------- */

/* renameclient <slot> <name> */
static void Renameclient_f(int argc, const char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 0;
	client_t *c = n > 0 && n <= svs.maxclients && svs.clients[n - 1].state != CS_FREE ? &svs.clients[n - 1] : NULL;
	char old[64];
	if (!c) {
		Con_Printf("Couldn't find user number %s\n", argc > 1 ? argv[1] : "");
		return;
	}
	if (argc < 3) {
		Con_Printf("renameclient <user number> <new name>\n");
		return;
	}
	Str_Copy(old, c->name, sizeof(old));
	SV_SetPlayerKey(c, "name", argv[2], true);
	SV_StuffText(c, va("setinfo name \"%s\"\n", argv[2]));
	Con_Printf("%s is now %s\n", old, c->name);
}

/* banname <name> [minutes] [reason]: ban, by name */
static void Banname_f(int argc, const char **argv)
{
	client_t *c;
	if (argc < 2) {
		Con_Printf(num_connected() ? "banname <name> [minutes] [reason]\n" : "banname: no clients\n");
		return;
	}
	for (int i = 0; i < svs.maxclients; i++) {
		c = &svs.clients[i];
		if (c->state != CS_FREE && !Str_ICmp(c->name, argv[1])) {
			Cmd_ExecuteString(va("banid %s %d kick", argc > 2 ? argv[2] : "0", i + 1));
			return;
		}
	}
	Con_Printf("banname: no player named %s\n", argv[1]);
}

void SV_InitAdmin(void)
{
	sv_floodprotect = Cvar_Get("sv_floodprotect", "1", 0, "Silence players who chat too fast");
	sv_floodprotect_messages = Cvar_Get("sv_floodprotect_messages", "4", 0, "Messages allowed within sv_floodprotect_interval");
	sv_floodprotect_interval = Cvar_Get("sv_floodprotect_interval", "4", 0, "Seconds sv_floodprotect_messages is counted over");
	sv_floodprotect_silencetime = Cvar_Get("sv_floodprotect_silencetime", "10", 0, "Seconds a flooder is silenced");
	sv_floodprotect_sendmessage = Cvar_Get("sv_floodprotect_sendmessage", "", 0, "What a silenced player is told (empty: how long is left)");
	log_dir = Cvar_Get("log_dir", "", 0, "Folder in the game directory for the player and rcon logs");
	log_timestamps = Cvar_Get("log_timestamps", "1", 0, "Start log lines with the date and time");
	log_enable_players = Cvar_Get("log_enable_players", "0", 0, "Log connections, renames and disconnections (logplayers)");
	log_name_players = Cvar_Get("log_name_players", "players", 0, "Name of the player log, without .log");
	log_enable_rcon = Cvar_Get("log_enable_rcon", "1", 0, "Log rcon commands (logrcon)");
	log_name_rcon = Cvar_Get("log_name_rcon", "rcon", 0, "Name of the rcon log, without .log");
	rank_filename = Cvar_Get("rank_filename", "", 0, "Rankings file in the game directory; empty keeps no rankings");
	rank_autoadd = Cvar_Get("rank_autoadd", "1", 0, "Give new names a ranking when they join");
	rank_needlogin = Cvar_Get("rank_needlogin", "0", 0, "Only let ranked names join");
	rank_parms_first = Cvar_Get("rank_parms_first", "0", 0, "First spawn parm a ranking keeps");
	rank_parms_last = Cvar_Get("rank_parms_last", "31", 0, "Last spawn parm a ranking keeps");
	sv_cmdlikercon = Cvar_Get("sv_cmdlikercon", "0", 0, "Ranked players with rights can run console commands at those rights");
	for (int i = 0; i < PEN_KINDS; i++)
		Cmd_AddD(penalty_names[i], Penalty_f,
		         i == PEN_KIND_MUTE    ? "mute <player> [minutes] [reason]: no chat (0 minutes lifts it)"
		         : i == PEN_KIND_STEALTH ? "stealthmute <player> [minutes]: their chat only reaches themselves, and they aren't told"
		         : i == PEN_KIND_CUFF  ? "cuff <player> [minutes] [reason]: can't attack (0 minutes lifts it)"
		                               : "cripple <player> [minutes] [reason]: can't move (0 minutes lifts it)");
	Cmd_AddD("floodprot", Floodprot_f, "floodprot [messages seconds silence]: chat flood protection");
	Cmd_AddD("logplayers", Logplayers_f, "Toggle the player log");
	Cmd_AddD("logrcon", Logrcon_f, "Toggle the rcon log");
	Cmd_AddD("fraglogfile", Fraglogfile_f, "Toggle QuakeWorld's frag log (frag_<n>.log)");
	Cmd_AddD("identify", Identify_f, "identify <player or address>: the names an address has used");
	Cmd_AddD("ipdump", Ipdump_f, "ipdump [file]: write the address log (iplog.txt)");
	Cmd_AddD("ipmerge", Ipmerge_f, "ipmerge <file>: read an address log in");
	Cmd_AddD("rankadd", Rankadd_f, "rankadd <name> [password] [rights]: add a ranking");
	Cmd_AddD("adduser", Rankadd_f, "adduser <name> [password] [rights]: add a ranking");
	Cmd_AddD("rankremove", Rankremove_f, "rankremove <ranking>: remove a ranking");
	Cmd_AddD("ranklist", Ranklist_f, "List the rankings");
	Cmd_AddD("rankfind", Rankfind_f, "rankfind <name>: rankings whose names contain the text");
	Cmd_AddD("rankrconlevel", Rankrconlevel_f, "rankrconlevel <ranking> <rights>: a ranking's rcon rights");
	Cmd_AddD("rankrefresh", Rankrefresh_f, "Save the connected players' scores into their rankings");
	Cmd_AddD("ranktopten", Ranktopten_f, "The ten best scores in the rankings");
	Cmd_AddD("setpass", Setpass_f, "setpass <ranking> [password]: set or clear a ranking's password");
	Cmd_AddD("renameclient", Renameclient_f, "renameclient <user number> <name>: rename a player");
	Cmd_AddD("banname", Banname_f, "banname <name> [minutes] [reason]: ban a player by name");
}
