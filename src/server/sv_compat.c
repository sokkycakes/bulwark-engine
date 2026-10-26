/* Bulwark engine - server commands from FTE and QuakeWorld servers: private
 * messages, bestweapon, the 6dof and spiderpig cheats, timers, forced
 * impulses, and the "status" and "ping" queries that server browsers and
 * the packet command send.
 * ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../net/net.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define F sv.vm->fofs
#define G sv.vm->gofs

/* --- bestweapon ------------------------------------------------------------------ */

/* impulse -> the items bit that says the player has the weapon, and the
 * ammo field it needs (Quake's layout, which register_bestweapon changes) */
typedef struct {
	int impulse;
	int itembit;
	char ammo[32];
} bestweapon_t;

static bestweapon_t bestweapons[32] = {
	{1, 4096, ""}, {2, 1, "ammo_shells"}, {3, 2, "ammo_shells"}, {4, 4, "ammo_nails"},
	{5, 8, "ammo_nails"}, {6, 16, "ammo_rockets"}, {7, 32, "ammo_rockets"}, {8, 64, "ammo_cells"},
};
static int numbestweapons = 8;

static float efield(int e, const char *name)
{
	qcdef_t *d = QC_FindField(sv.vm, name);
	return d ? QC_EdictFields(sv.vm, e)[d->ofs].f : 0;
}

static bool has_weapon(int e, int impulse)
{
	for (int i = 0; i < numbestweapons; i++)
		if (bestweapons[i].impulse == impulse) {
			const bestweapon_t *b = &bestweapons[i];
			if (!((int)efield(e, "items") & b->itembit))
				return false;
			return !b->ammo[0] || efield(e, b->ammo) > 0;
		}
	return true; /* unknown impulses are the game's to judge */
}

/* register_bestweapon <impulse> <items bit> [ammo field]: how bestweapon
 * knows the mod's weapons */
static void RegisterBestweapon_f(int argc, const char **argv)
{
	int imp;
	bestweapon_t *b = NULL;
	if (argc < 3) {
		Con_Printf("Unknown arg %s\nregister_bestweapon <impulse> <items bit> [ammo field]\n", argc > 1 ? argv[1] : "");
		return;
	}
	imp = atoi(argv[1]);
	for (int i = 0; i < numbestweapons; i++)
		if (bestweapons[i].impulse == imp)
			b = &bestweapons[i];
	if (!b && numbestweapons < (int)BW_ARRAY_COUNT(bestweapons))
		b = &bestweapons[numbestweapons++];
	if (!b)
		return;
	b->impulse = imp;
	b->itembit = atoi(argv[2]);
	Str_Copy(b->ammo, argc > 3 ? argv[3] : "", sizeof(b->ammo));
}

/* --- client commands --------------------------------------------------------------- */

static bool cheats_ok(client_t *c)
{
	if (Cvar_Value("sv_cheats") || svs.maxclients == 1)
		return true;
	SV_ClientPrint(c, 2, "Cheats are off on this server (sv_cheats 0).\n");
	return false;
}

client_t *SV_FindClient(const char *s)
{
	int n = atoi(s);
	if (n > 0 && n <= svs.maxclients && svs.clients[n - 1].state != CS_FREE)
		return &svs.clients[n - 1];
	for (int i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].state != CS_FREE && !Str_ICmp(svs.clients[i].name, s))
			return &svs.clients[i];
	return NULL;
}

/* the commands below, when the game passes them back; false if not one */
bool SV_ClientCompatCommand(client_t *c, int argc, const char **argv)
{
	const char *cmd = argv[0];
	if (!Str_ICmp(cmd, "sayone")) {
		client_t *to;
		char text[1024] = "";
		if (argc < 3 || !(to = SV_FindClient(argv[1]))) {
			SV_ClientPrint(c, 2, argc < 3 ? "usage: sayone <player> <text>\n" : "no such player\n");
			return true;
		}
		for (int i = 2; i < argc; i++) {
			Str_Cat(text, argv[i], sizeof(text));
			Str_Cat(text, i + 1 < argc ? " " : "", sizeof(text));
		}
		SV_ClientPrint(to, 3, va("%s (private): %s\n", c->name, text));
		if (to != c)
			SV_ClientPrint(c, 3, va("to %s: %s\n", to->name, text));
		return true;
	}
	if (!Str_ICmp(cmd, "bestweapon")) {
		for (int i = 1; i < argc; i++) {
			int imp = atoi(argv[i]);
			if (imp > 0 && has_weapon(c->edict, imp)) {
				c->forced_impulse = imp;
				break;
			}
		}
		return true;
	}
	if (!Str_ICmp(cmd, "6dof")) {
		if (cheats_ok(c)) {
			c->sixdof = !c->sixdof;
			QC_SetEFloat(sv.vm, c->edict, F.movetype, c->sixdof ? (float)MOVETYPE_FLY : (float)MOVETYPE_WALK);
			/* the client lifts its pitch limit and lets the view roll */
			SV_StuffText(c, va("set cl_6dof %d\n", c->sixdof));
			SV_ClientPrint(c, 2, va("6dof %s\n", c->sixdof ? "ON" : "OFF"));
		}
		return true;
	}
	if (!Str_ICmp(cmd, "spiderpig")) {
		if (cheats_ok(c)) {
			c->spiderpig = !c->spiderpig;
			SV_ClientPrint(c, 2, va("spiderpig %s\n", c->spiderpig ? "ON" : "OFF"));
		}
		return true;
	}
	return false;
}

/* spiderpig: walking into a wall climbs it */
void SV_CompatPostThink(client_t *c)
{
	qcval_t *f;
	float start[3], end[3], fwd[3], yaw[3] = {0};
	svtrace_t tr;
	if (!c->spiderpig || c->lastucmd.movevalues[0] <= 0)
		return;
	f = QC_EdictFields(sv.vm, c->edict);
	yaw[1] = c->lastucmd.angles[1];
	AngleVectors(yaw, fwd, NULL, NULL);
	for (int i = 0; i < 3; i++) {
		start[i] = f[F.origin + i].f;
		end[i] = start[i] + fwd[i] * 24;
	}
	SV_Move(start, &f[F.mins].f, &f[F.maxs].f, end, MOVE_NOMONSTERS, c->edict, &tr);
	if (tr.fraction < 1 && fabsf(tr.normal[2]) < 0.7f) {
		f[F.velocity + 2].f = BW_MAX(f[F.velocity + 2].f, 200.0f);
		if (F.flags >= 0)
			f[F.flags].f = (float)((int)f[F.flags].f & ~FL_ONGROUND);
	}
}

/* --- console commands ----------------------------------------------------------------- */

typedef struct {
	bool active;
	int count; /* runs left; negative for forever */
	double interval, next;
	char command[512];
} svtimer_t;
static svtimer_t timers[32];

/* sv_settimer <count> <interval> <command>: run a command repeatedly on the
 * server's clock (count 0 removes timers running that command) */
static void SetTimer_f(int argc, const char **argv)
{
	int count;
	if (argc < 4) {
		Con_Printf("sv_settimer <count> <interval> <command>\n");
		return;
	}
	count = atoi(argv[1]);
	for (int i = 0; i < (int)BW_ARRAY_COUNT(timers); i++)
		if (timers[i].active && !strcmp(timers[i].command, argv[3]) && count == 0)
			timers[i].active = false;
	if (count == 0)
		return;
	for (int i = 0; i < (int)BW_ARRAY_COUNT(timers); i++)
		if (!timers[i].active) {
			timers[i].active = true;
			timers[i].count = count;
			timers[i].interval = BW_MAX(atof(argv[2]), 0.01);
			timers[i].next = sv.time + timers[i].interval;
			Str_Copy(timers[i].command, argv[3], sizeof(timers[i].command));
			return;
		}
	Con_Printf("too many timers\n");
}

void SV_CompatFrame(void)
{
	for (int i = 0; i < (int)BW_ARRAY_COUNT(timers); i++) {
		svtimer_t *t = &timers[i];
		if (!t->active || sv.time < t->next)
			continue;
		Cbuf_AddText(va("%s\n", t->command));
		t->next += t->interval;
		if (t->count > 0 && --t->count == 0)
			t->active = false;
	}
}

/* sv_impulse <player> <impulse>: the player's next move carries an impulse */
static void SvImpulse_f(int argc, const char **argv)
{
	client_t *c;
	if (argc < 3) {
		Con_Printf("sv_impulse <player> <impulse>\n");
		return;
	}
	if (!sv.active || !(c = SV_FindClient(argv[1]))) {
		Con_Printf("no player %s\n", argv[1]);
		return;
	}
	c->forced_impulse = atoi(argv[2]);
}

/* sv_gamedir [name]: the game directory reported to clients, without
 * changing where the server reads files */
static void SvGamedir_f(int argc, const char **argv)
{
	char cur[256];
	if (argc < 2) {
		SV_InfoGet(sv.serverinfo, "*gamedir", cur, sizeof(cur));
		Con_Printf("Current gamedir: %s\n", cur[0] ? cur : FS_GameDir());
		return;
	}
	SV_SetServerinfoKey("*gamedir", argv[1]);
}

/* sv_meminfo: what the server holds */
static void SvMeminfo_f(int argc, const char **argv)
{
	size_t strings = 0;
	if (!sv.active) {
		Con_Printf("no map running\n");
		return;
	}
	if (argc > 1 && !Str_ICmp(argv[1], "all"))
		for (int i = 1; i < sv.nummodels; i++)
			Con_Printf("  model %3d %s\n", i, sv.models[i]);
	Con_Printf("%d models, %d sounds, %d effects precached\n", sv.nummodels - 1, sv.numsounds - 1, sv.numparticles);
	strings = (size_t)QC_LiveStrings();
	Con_Printf("QC: %d of %d entities, %u KB arena, %zu live strings\n", sv.vm->num_edicts, sv.vm->max_edicts,
	           (unsigned)(sv.vm->memsize / 1024), strings);
}

/* --- connectionless queries -------------------------------------------------------------- */

/* "status": serverinfo and the players, as QuakeWorld browsers expect;
 * "ping": an acknowledgement */
bool SV_CompatConnectionless(const netadr_t *from, int argc, const char **argv)
{
	if (!strcmp(argv[0], "ping")) {
		NET_OutOfBandPrint(NS_SERVER, from, "l");
		return true;
	}
	if (!strcmp(argv[0], "status")) {
		strbuf_t sb = {0};
		SB_AppendStr(&sb, "print\n");
		SB_AppendStr(&sb, sv.serverinfo);
		SB_AppendStr(&sb, "\n");
		for (int i = 0; i < svs.maxclients; i++) {
			client_t *c = &svs.clients[i];
			float frags = 0;
			if (c->state == CS_FREE)
				continue;
			if (sv.vm)
				frags = efield(c->edict, "frags");
			SB_AppendStr(&sb, va("%d %d %d %d \"%s\" \"\" 0 0\n", i + 1, (int)frags,
			                     (int)((host_time - c->connect_time) / 60), (int)(c->chan.rtt * 1000), c->name));
		}
		NET_OutOfBandPrint(NS_SERVER, from, "%s", sb.data);
		SB_Free(&sb);
		return true;
	}
	return false;
}

void SV_InitCompat(void)
{
	Cmd_AddD("register_bestweapon", RegisterBestweapon_f, "register_bestweapon <impulse> <items bit> [ammo field]: teach bestweapon a weapon");
	Cmd_AddD("sv_settimer", SetTimer_f, "sv_settimer <count> <interval> <command>: repeat a command (count -1 forever, 0 removes)");
	Cmd_AddD("sv_impulse", SvImpulse_f, "sv_impulse <player> <impulse>: give a player's next move an impulse");
	Cmd_AddD("sv_gamedir", SvGamedir_f, "sv_gamedir [name]: the game directory reported to clients");
	Cmd_AddD("sv_meminfo", SvMeminfo_f, "sv_meminfo [all]: the server's precache counts and QC memory (all lists the models)");
}
