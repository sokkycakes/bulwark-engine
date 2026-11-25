/* Bulwark engine - finished sets to Bulwark Live's fighter records.
 *
 * A game's rules (Stiletto's arena set play) say who sat where and how the
 * set ended with two console commands, queued from QC with localcmd:
 *
 *   live_set_seat <edict> <side> <outcome> <behind> <games won> [hero]
 *   live_set_report <0 complete | 1 forfeit> <first to> <runback 0|1>
 *
 * The engine turns each player's edict into their Live player id (from the
 * address they play from) and, for a player who left (outcome 2), tells a
 * quit from a dropped connection by how the server lost them. Only the
 * host of a Live match sends anything; elsewhere the commands do nothing.
 * docs/spec/live.md, "Fighter records". ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cmd.h"
#include "../net/live.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REPORT 16
#define DROP_RING 16

static live_seat_report_t pending[MAX_REPORT];
static int npending;

/* clients the server let go lately: the rules report a leaver after the
 * server has already freed their slot */
static struct {
	int edict;
	netadr_t adr;
	int slot;
	bool timed_out;
	double at;
} drops[DROP_RING];
static int drop_head;

void SV_LiveNoteDrop(const client_t *c, const char *reason)
{
	if (c->bot)
		return;
	const client_t *conn = c->parent ? &svs.clients[c->parent - 1] : c;
	drops[drop_head].edict = c->edict;
	drops[drop_head].adr = conn->chan.remote;
	drops[drop_head].slot = c->parent ? c->seat : 0;
	drops[drop_head].timed_out = reason && strstr(reason, "timed out");
	drops[drop_head].at = Sys_Seconds();
	drop_head = (drop_head + 1) % DROP_RING;
}

static bool adr_to_id(const netadr_t *a, uint32_t *id)
{
	if (a->type == NA_LIVE) {
		*id = (uint32_t)a->ip[0] << 24 | (uint32_t)a->ip[1] << 16 | (uint32_t)a->ip[2] << 8 | a->ip[3];
		return *id != 0;
	}
	if (a->type == NA_LOOPBACK) {
		/* the host's own player */
		*id = Live_MyId();
		return *id != 0;
	}
	return false;
}

/* the Live id and split-screen slot of the player on 'edict' */
static bool seat_of(int edict, uint32_t *id, int *slot, bool *timed_out)
{
	*timed_out = false;
	for (int i = 0; i < svs.maxclients; i++) {
		client_t *c = &svs.clients[i];
		if (c->state == CS_FREE || c->bot || c->edict != edict)
			continue;
		const client_t *conn = c->parent ? &svs.clients[c->parent - 1] : c;
		*slot = c->parent ? c->seat : 0;
		return adr_to_id(&conn->chan.remote, id);
	}
	double t = Sys_Seconds();
	for (int k = 0; k < DROP_RING; k++) {
		int i = (drop_head - 1 - k + DROP_RING) % DROP_RING;
		if (drops[i].at && drops[i].edict == edict && t - drops[i].at < 30.0) {
			*slot = drops[i].slot;
			*timed_out = drops[i].timed_out;
			return adr_to_id(&drops[i].adr, id);
		}
	}
	return false;
}

static void SetSeat_f(int argc, const char **argv)
{
	if (argc < 6) {
		Con_Printf("usage: live_set_seat <edict> <side> <outcome 0 win|1 loss|2 left> <behind> <games> [hero]\n");
		return;
	}
	if (npending >= MAX_REPORT)
		return;
	live_seat_report_t *s = &pending[npending];
	memset(s, 0, sizeof(*s));
	bool timed_out;
	if (!seat_of(atoi(argv[1]), &s->id, &s->slot, &timed_out)) {
		Con_DPrintf("live_set_seat: edict %s isn't a Live player\n", argv[1]);
		return;
	}
	s->outcome = atoi(argv[3]);
	if (s->outcome < 0 || s->outcome > 2)
		return;
	if (s->outcome == 2 && timed_out)
		s->outcome = 3;
	s->behind = atoi(argv[4]) != 0;
	s->games = atoi(argv[5]);
	if (s->games < 0 || s->games > 255)
		s->games = 0;
	if (argc > 6)
		snprintf(s->hero, sizeof(s->hero), "%s", argv[6]);
	npending++;
}

static void SetReport_f(int argc, const char **argv)
{
	int n = npending;
	npending = 0;
	if (argc < 4) {
		Con_Printf("usage: live_set_report <0 complete|1 forfeit> <first to> <runback 0|1>\n");
		return;
	}
	if (!n)
		return;
	if (Live_ReportSet(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]) != 0, pending, n))
		Con_Printf("live: reported a %s set, %d seats\n", atoi(argv[1]) ? "forfeited" : "finished", n);
}

void SV_InitLiveSets(void)
{
	Cmd_AddD("live_set_seat", SetSeat_f, "Rules: one seat of a finished set, for Bulwark Live records");
	Cmd_AddD("live_set_report", SetReport_f, "Rules: send the finished set to Bulwark Live (the match's host only)");
}
