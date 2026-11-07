/* Bulwark engine - replays a dedicated server's logged link commands
 * through the DS's weapon code (src/platform/nds/weapon_nds.c) and compares
 * each result with what the server's game got. ISC licence, see LICENSE.
 *
 *   cc -O2 -std=c99 -Wall -ffp-contract=off -Isrc/platform/nds -o weptest \
 *      tools/nds/weptest.c src/platform/nds/weapon_nds.c src/platform/nds/pmove_nds.c -lm
 *
 *   weptest <ded.log> [-slot n] [-chain] [-v]
 *
 * The log is the ded's console output with sv_link_log 1: the LINKPM winfo,
 * wpre, cmd and wpost lines (docs/spec/link.md, "sv_link_log"). Each logged
 * command runs once from the server's own state before it ("step" mode), or
 * with -chain from the first state on, keeping its own; a step is a match
 * when Weapon_Matches says so. The weapon's melee test has no hulls here
 * (traces miss). Exit status 1 when any step differs.
 *
 * The logs carry the game's data (decl values, model timings): keep them out
 * of git. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weapon_nds.h"

typedef struct {
	wstate_t s;
	float land, health;
	int waterlevel;
} lw_t;

static int parse_w(const char *p, lw_t *o)
{
	wstate_t *s = &o->s;
	int w, ver, st, fi, fl, clip, cs, seq, n, used, nammo;
	float f[17];
	unsigned vf, gf;
	memset(o, 0, sizeof(*o));
	if (sscanf(p, "%d %d %d %d %d %d %d %d%n", &w, &ver, &st, &fi, &fl, &clip, &cs, &seq, &n) != 8)
		return 0;
	p += n;
	for (int i = 0; i < 17; i++) {
		if (sscanf(p, " %a%n", &f[i], &used) != 1)
			return 0;
		p += used;
	}
	if (sscanf(p, " %u %u %d%n", &vf, &gf, &nammo, &used) != 3 || nammo > W_AMMO)
		return 0;
	p += used;
	s->edict = (uint16_t)w, s->infover = (uint16_t)ver, s->state = (uint8_t)st, s->fi = (uint8_t)fi, s->flags = (uint8_t)fl;
	s->clip = (int16_t)clip, s->clipsize = (int16_t)cs, s->animseq = (int16_t)seq;
	s->burst = f[0], s->fiv = f[1], s->overheat = f[2], s->overheatlen = f[3], s->reloadstep = f[4], s->spin = f[5];
	s->atk = f[6], s->idle = f[7], s->rel = f[8], s->animtime = f[9];
	for (int k = 0; k < 3; k++)
		s->punch[k] = f[10 + k], s->punchvel[k] = f[13 + k];
	s->firescale = f[16];
	s->vflags = vf, s->gflags = gf, s->nammo = (uint8_t)nammo;
	for (int i = 0; i < nammo; i++) {
		int t, c;
		if (sscanf(p, " %d %d%n", &t, &c, &used) != 2)
			return 0;
		p += used;
		s->ammotype[i] = (int8_t)t, s->ammo[i] = (int16_t)c;
	}
	return sscanf(p, " %a %a %d", &o->land, &o->health, &o->waterlevel) == 3;
}

static void dump(const char *tag, const wstate_t *s)
{
	printf("  %s st %d fi %d fl %d clip %d/%d seq %d t %.4f atk %.4f idle %.4f rel %.4f burst %g punch %.3f %.3f %.3f "
	       "pv %.3f %.3f %.3f vf %x gf %x",
	       tag, s->state, s->fi, s->flags, s->clip, s->clipsize, s->animseq, s->animtime, s->atk, s->idle, s->rel,
	       s->burst, s->punch[0], s->punch[1], s->punch[2], s->punchvel[0], s->punchvel[1], s->punchvel[2],
	       s->vflags & W_VFL_BITS, s->gflags & W_GF_BITS);
	for (int i = 0; i < s->nammo; i++)
		printf(" a%d=%d", s->ammotype[i], s->ammo[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	int want = -1, chain = 0, verbose = 0;
	static winfo_t info;
	static char line[1 << 16];
	bool haveinfo = false, havepre = false, havecmd = false, haveown = false;
	lw_t pre, post;
	wstate_t own;
	pmcmd_t cmd;
	int steps = 0, bad = 0, skipped = 0, fired = 0;
	FILE *f;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-slot") && i + 1 < argc)
			want = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-chain"))
			chain = 1;
		else if (!strcmp(argv[i], "-v"))
			verbose = 1;
		else
			path = argv[i];
	}
	if (!path || !(f = fopen(path, "r"))) {
		fprintf(stderr, "usage: weptest <ded.log> [-slot n] [-chain] [-v]\n");
		return 2;
	}
	while (fgets(line, sizeof(line), f)) {
		char *p = strstr(line, "LINKPM "), what[16];
		int slot, n;
		if (!p || sscanf(p, "LINKPM %d %15s %n", &slot, what, &n) != 2 || (want >= 0 && slot != want))
			continue;
		p += n;
		if (!strcmp(what, "winfo")) {
			unsigned e, v, len = 0;
			static uint8_t body[4096];
			int used;
			if (sscanf(p, "%u %u %n", &e, &v, &used) != 2)
				continue;
			p += used;
			while (p[0] && p[1] && p[0] != '\n' && len < sizeof(body)) {
				unsigned b;
				if (sscanf(p, "%2x", &b) != 1)
					break;
				body[len++] = (uint8_t)b, p += 2;
			}
			info.edict = (uint16_t)e, info.version = (uint16_t)v;
			haveinfo = Weapon_ParseInfo(&info, body, (int)len);
			printf("winfo edict %u version %u: %s, %d fireinfos, %d sequences, %d events\n", e, v,
			       haveinfo ? "parsed" : "DOESN'T PARSE", info.nfi, info.nseq, info.nev);
		} else if (!strcmp(what, "wpre")) {
			havepre = parse_w(p, &pre) != 0, havecmd = false;
		} else if (!strcmp(what, "cmd")) {
			unsigned seq, buttons;
			int k0, pitch, yaw, m0, m1, m2, k13, imp;
			double msec;
			if (sscanf(p, "%u %d %d %d %d %d %d %u %d %la %d", &seq, &k0, &pitch, &yaw, &m0, &m1, &m2, &buttons, &k13,
			           &msec, &imp) != 11)
				continue;
			memset(&cmd, 0, sizeof(cmd));
			cmd.seq = seq, cmd.msec = (uint8_t)k0, cmd.pitch = (int16_t)pitch, cmd.yaw = (int16_t)yaw;
			cmd.move[0] = (int16_t)m0, cmd.move[1] = (int16_t)m1, cmd.move[2] = (int16_t)m2;
			cmd.buttons = (uint16_t)buttons, cmd.impulse = (uint8_t)imp;
			havecmd = fabs(msec - (float)k0 / 1000.0f) < 1e-6; /* (seconds as run) a command the speedhack budget cut short runs shorter: left out */
			if (!havecmd)
				skipped++;
		} else if (!strcmp(what, "wpost") && havepre && !havecmd && parse_w(p, &post)) {
			own = post.s, haveown = true; /* the server ran it short: carry on from its result */
			havepre = false;
		} else if (!strcmp(what, "wpost") && havepre && havecmd && parse_w(p, &post)) {
			wenv_t env;
			wstate_t s;
			memset(&env, 0, sizeof(env));
			env.seq = cmd.seq, env.health = post.health, env.waterlevel = post.waterlevel;
			env.landed = post.land >= 0, env.downforce = post.land;
			if (!chain || !haveown || own.edict != pre.s.edict)
				own = pre.s;
			s = own;
			Weapon_Run(&s, haveinfo && Weapon_Usable(&pre.s, &info) ? &info : NULL, &cmd, &env);
			/* the version the server names comes with its state, not from the command */
			s.infover = post.s.infover;
			steps++;
			if (cmd.buttons & 1)
				fired++;
			if (!Weapon_Matches(&s, &post.s)) {
				bad++;
				if (verbose || bad <= 20) {
					printf("step %d seq %u buttons %x: differs\n", steps, cmd.seq, cmd.buttons);
					dump("pre ", &pre.s);
					dump("dev ", &s);
					dump("srv ", &post.s);
				}
				if (chain)
					s = post.s; /* a correction, as the device takes one */
			} else if (verbose) {
				printf("step %d seq %u buttons %x: ok\n", steps, cmd.seq, cmd.buttons);
				dump("srv ", &post.s);
			}
			own = s, haveown = true;
			havepre = havecmd = false;
		}
	}
	fclose(f);
	printf("%d steps (%d with attack held), %d differ, %d cut short and left out\n", steps, fired, bad, skipped);
	return bad ? 1 : 0;
}
