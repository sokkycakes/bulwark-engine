/* Bulwark engine - room effects: the reverb slots that setup_reverb fills
 * and SetListener picks (fteextensions.qc, FTE_CSQC_REVERB), Source-style
 * dsp cvars with built-in presets for the 29 GoldSrc room types, muffling
 * under water, and an automatic room guess from traces around the listener.
 *
 * The reverb is written fresh in software: a pre-delay line, a handful of
 * early reflection taps, then parallel damped feedback combs into series
 * allpasses for the late tail, after the classic Schroeder design. Docs in
 * docs/spec/sound.md. ISC licence, see LICENSE. */
#include "snd_local.h"
#include "../client/client.h"
#include "../common/cmd.h"
#include "../common/cvar.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_SLOTS 64
#define NUM_COMBS 8
#define NUM_ALLPASS 4
#define PREDELAY_SEC 0.45f /* reflections up to 0.3 s, late up to 0.1 s, and modulation */
#define ECHO_SEC 0.3f
#define FADE_SEC 0.08f
/* sets the tail's overall level: the generic room's tail sits about 12 dB
 * under the direct sound */
#define LATE_LEVEL 0.12f
#define SOUND_SPEED 13500.0f /* units per second, taking a unit as an inch */

/* reverbinfo_t as fteextensions.qc lays it out: EFX reverb properties */
typedef struct {
	float density, diffusion, gain, gainhf, gainlf;
	float decay, decayhf, decaylf;
	float reflgain, refldelay, reflpan[3];
	float lategain, latedelay, latepan[3];
	float echotime, echodepth, modtime, moddepth;
	float airabsorption, hfref, lfref, rolloff;
	int32_t decayhflimit;
} reverb_t;

/* --- presets ----------------------------------------------------------------- */

/* shorthand: density, diffusion, gain, gainhf, decay, decayhf, reflgain,
 * refldelay, lategain, latedelay, echotime, echodepth */
#define RV(de, di, g, ghf, dt, dhf, rg, rd, lg, ld, et, ed)                                                         \
	{de, di, g, ghf, 1, dt, dhf, 1, rg, rd, {0, 0, 0}, lg, ld, {0, 0, 0}, et, ed, 0.25f, 0, 0.994f, 5000, 250, 0, 1}

/* GoldSrc's room types (the env_sound roomtype key, room_type cvar), as
 * EFX parameters chosen here by ear from each name's description */
static const reverb_t room_presets[29] = {
	RV(0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0.25f, 0),                          /* 0 normal: off */
	RV(1.00f, 1.0f, 0.32f, 0.89f, 1.49f, 0.83f, 0.05f, 0.007f, 1.26f, 0.011f, 0.25f, 0), /* 1 generic */
	RV(0.30f, 1.0f, 0.32f, 0.79f, 0.70f, 1.20f, 0.30f, 0.004f, 1.40f, 0.006f, 0.25f, 0), /* 2 metal small */
	RV(0.55f, 1.0f, 0.32f, 0.79f, 1.20f, 1.20f, 0.30f, 0.008f, 1.40f, 0.010f, 0.25f, 0), /* 3 metal medium */
	RV(1.00f, 1.0f, 0.32f, 0.79f, 2.10f, 1.10f, 0.30f, 0.015f, 1.40f, 0.020f, 0.25f, 0), /* 4 metal large */
	RV(0.35f, 0.8f, 0.32f, 0.70f, 1.10f, 0.70f, 0.25f, 0.005f, 1.30f, 0.020f, 0.12f, 0.15f), /* 5 tunnel small */
	RV(0.60f, 0.8f, 0.32f, 0.70f, 1.80f, 0.70f, 0.25f, 0.010f, 1.30f, 0.030f, 0.16f, 0.20f), /* 6 tunnel medium */
	RV(1.00f, 0.8f, 0.32f, 0.70f, 2.70f, 0.65f, 0.25f, 0.020f, 1.30f, 0.040f, 0.22f, 0.25f), /* 7 tunnel large */
	RV(0.35f, 1.0f, 0.32f, 0.60f, 1.30f, 0.60f, 0.40f, 0.006f, 1.10f, 0.012f, 0.25f, 0), /* 8 chamber small */
	RV(0.60f, 1.0f, 0.32f, 0.60f, 2.20f, 0.60f, 0.40f, 0.012f, 1.10f, 0.020f, 0.25f, 0), /* 9 chamber medium */
	RV(1.00f, 1.0f, 0.32f, 0.60f, 3.40f, 0.55f, 0.40f, 0.020f, 1.10f, 0.030f, 0.25f, 0), /* 10 chamber large */
	RV(0.30f, 1.0f, 0.32f, 1.00f, 0.90f, 1.40f, 0.50f, 0.003f, 1.50f, 0.006f, 0.25f, 0), /* 11 bright small */
	RV(0.55f, 1.0f, 0.32f, 1.00f, 1.50f, 1.40f, 0.50f, 0.007f, 1.50f, 0.012f, 0.25f, 0), /* 12 bright medium */
	RV(1.00f, 1.0f, 0.32f, 1.00f, 2.40f, 1.30f, 0.50f, 0.012f, 1.50f, 0.020f, 0.25f, 0), /* 13 bright large */
	RV(0.36f, 1.0f, 0.32f, 0.010f, 1.49f, 0.10f, 0.60f, 0.007f, 1.20f, 0.011f, 0.25f, 0), /* 14 water 1 */
	RV(0.50f, 1.0f, 0.32f, 0.020f, 2.00f, 0.20f, 0.60f, 0.010f, 1.60f, 0.015f, 0.25f, 0), /* 15 water 2 */
	RV(0.70f, 1.0f, 0.32f, 0.005f, 3.00f, 0.10f, 0.40f, 0.015f, 2.00f, 0.020f, 0.25f, 0.10f), /* 16 water 3 */
	RV(0.30f, 1.0f, 0.32f, 0.80f, 0.80f, 0.90f, 0.45f, 0.004f, 1.10f, 0.008f, 0.25f, 0), /* 17 concrete small */
	RV(0.55f, 1.0f, 0.32f, 0.80f, 1.40f, 0.85f, 0.45f, 0.009f, 1.10f, 0.014f, 0.25f, 0), /* 18 concrete medium */
	RV(1.00f, 1.0f, 0.32f, 0.80f, 2.20f, 0.80f, 0.45f, 0.016f, 1.10f, 0.022f, 0.25f, 0), /* 19 concrete large */
	RV(0.70f, 0.4f, 0.32f, 0.70f, 1.50f, 0.60f, 0.20f, 0.060f, 1.10f, 0.080f, 0.15f, 0.30f), /* 20 big 1 */
	RV(0.85f, 0.4f, 0.32f, 0.70f, 2.50f, 0.60f, 0.20f, 0.100f, 1.10f, 0.090f, 0.20f, 0.40f), /* 21 big 2 */
	RV(1.00f, 0.4f, 0.32f, 0.70f, 3.50f, 0.60f, 0.20f, 0.150f, 1.10f, 0.100f, 0.25f, 0.50f), /* 22 big 3 */
	RV(0.40f, 1.0f, 0.32f, 0.70f, 1.80f, 0.70f, 0.30f, 0.010f, 1.40f, 0.020f, 0.25f, 0), /* 23 cavern small */
	RV(0.70f, 1.0f, 0.32f, 0.70f, 3.00f, 0.70f, 0.30f, 0.020f, 1.40f, 0.035f, 0.25f, 0), /* 24 cavern medium */
	RV(1.00f, 1.0f, 0.32f, 0.70f, 4.50f, 0.65f, 0.30f, 0.030f, 1.40f, 0.050f, 0.25f, 0.10f), /* 25 cavern large */
	RV(0.50f, 0.3f, 0.32f, 0.60f, 1.00f, 0.50f, 0.10f, 0.050f, 1.80f, 0.060f, 0.10f, 0.80f), /* 26 weirdo 1 */
	RV(0.80f, 0.2f, 0.32f, 0.40f, 2.00f, 0.50f, 0.10f, 0.080f, 1.80f, 0.080f, 0.18f, 0.90f), /* 27 weirdo 2 */
	RV(1.00f, 1.0f, 0.32f, 1.00f, 5.00f, 1.80f, 0.10f, 0.100f, 1.40f, 0.100f, 0.25f, 0.60f), /* 28 weirdo 3 */
};

/* --- state ------------------------------------------------------------------- */

static reverb_t slots[MAX_SLOTS];
static bool slot_set[MAX_SLOTS];
static int listener_slot;     /* from SetListener */
static reverb_t auto_room;    /* dsp_automatic's guess, while it has one */
static bool auto_valid;
static double auto_next;

static cvar_t *dsp_off, *dsp_volume, *room_type, *dsp_water, *dsp_automatic, *dsp_show;

/* what the mixer thread runs, filled in under the lock */
typedef struct {
	reverb_t p;
	bool water; /* muffle the dry signal too */
	bool on;
} target_t;
static target_t want, cur;

/* the mixer's working state */
static struct {
	int rate;
	float *pre;
	int prelen, prepos;
	float *echo;
	int echolen, echopos;
	float *comb[2][NUM_COMBS];
	int comblen[2][NUM_COMBS], combmax[2][NUM_COMBS], combpos[2][NUM_COMBS];
	float combstore[2][NUM_COMBS], combfb[NUM_COMBS], combdamp;
	float *ap[2][NUM_ALLPASS];
	int aplen[2][NUM_ALLPASS], appos[2][NUM_ALLPASS];
	float apgain;
	int taps[6];
	float tapgain[6][2];
	int latetap, echotap;
	float lategain, echofb, echoout;
	float inlp, inlp_a;   /* wet input low-pass for gainhf */
	float drylp[2], drylp_a;
	float modphase, modstep, moddepth;
	float fade;           /* 0..1, the wet level while switching */
	bool switching;
} dsp;

/* comb and allpass lengths at 44.1 kHz; the right channel's are a little
 * longer to keep the tail wide */
static const int comb_base[NUM_COMBS] = {1103, 1171, 1249, 1327, 1399, 1459, 1531, 1601};
static const int ap_base[NUM_ALLPASS] = {547, 433, 337, 229};
#define STEREO_SPREAD 27

/* --- coefficients ------------------------------------------------------------ */

/* a one-pole low-pass y += (1 - a)(x - y) giving gain g at the reference
 * frequency, from the bilinear magnitude of that filter */
static float lowpass_for(float g, float freq, int rate)
{
	float c, a;
	g = BW_CLAMP(g, 0.0001f, 1.0f);
	if (g >= 0.999f)
		return 0;
	c = cosf(2 * (float)M_PI * BW_MIN(freq, rate * 0.45f) / rate);
	/* |H|^2 = (1-a)^2 / (1 - 2a cos w + a^2) = g^2, solved for a */
	{
		float g2 = g * g, A = 1 - g2, B = 2 * (g2 * c - 1), C = 1 - g2;
		float disc = B * B - 4 * A * C;
		if (A < 1e-6f || disc < 0)
			return 0.99f;
		a = (-B - sqrtf(disc)) / (2 * A);
	}
	return BW_CLAMP(a, 0, 0.995f);
}

static void compute(const target_t *t)
{
	const reverb_t *p = &t->p;
	float scale = 0.35f + 0.65f * BW_CLAMP(p->density, 0, 1);
	float decay = BW_CLAMP(p->decay, 0.1f, 20.0f), hfratio = BW_CLAMP(p->decayhf, 0.1f, 2.0f);
	float hfref = p->hfref > 0 ? p->hfref : 5000;
	int r = dsp.rate;
	float ratio, gtot = BW_CLAMP(p->gain, 0, 1);
	if (p->decayhflimit && hfratio > 1)
		hfratio = 1; /* the limit keeps the highs from outlasting the room */
	for (int c = 0; c < NUM_COMBS; c++)
		for (int s = 0; s < 2; s++) {
			int len = (int)((comb_base[c] + s * STEREO_SPREAD) * scale * r / 44100.0f);
			dsp.comblen[s][c] = BW_CLAMP(len, 16, dsp.combmax[s][c]);
		}
	/* feedback for a 60 dB fall over the decay time; the low-pass in the
	 * loop makes the highs fall over decay * hfratio instead */
	{
		float len = (float)dsp.comblen[0][NUM_COMBS / 2] / r;
		float g = powf(10, -3 * len / decay), ghf = powf(10, -3 * len / (decay * hfratio));
		ratio = BW_MIN(1.0f, ghf / g);
		dsp.combdamp = lowpass_for(ratio, hfref, r);
		for (int c = 0; c < NUM_COMBS; c++)
			dsp.combfb[c] = powf(10, -3 * ((float)dsp.comblen[0][c] / r) / decay);
	}
	dsp.apgain = 0.25f + 0.45f * BW_CLAMP(p->diffusion, 0, 1);
	/* early reflections: taps after the reflection delay, spread by size */
	{
		static const float spread_ms[6] = {0, 4.3f, 7.9f, 12.1f, 17.3f, 23.9f};
		float rg = BW_CLAMP(p->reflgain, 0, 3.16f) * gtot * 0.45f;
		for (int i = 0; i < 6; i++) {
			int d = (int)((BW_CLAMP(p->refldelay, 0, 0.3f) + spread_ms[i] * scale / 1000) * r);
			float pan = BW_CLAMP(p->reflpan[0], -1, 1); /* listener space, +x to the right */
			dsp.taps[i] = BW_CLAMP(d, 1, dsp.prelen - 1);
			dsp.tapgain[i][0] = rg * (i & 1 ? 0.6f : 1.0f) * (1 - BW_MAX(0, pan)) / (1 + i * 0.25f);
			dsp.tapgain[i][1] = rg * (i & 1 ? 1.0f : 0.6f) * (1 - BW_MAX(0, -pan)) / (1 + i * 0.25f);
		}
	}
	dsp.latetap = (int)((BW_CLAMP(p->refldelay, 0, 0.3f) + BW_CLAMP(p->latedelay, 0, 0.1f)) * r);
	dsp.moddepth = BW_CLAMP(p->moddepth, 0, 1) * 0.002f * r;
	dsp.latetap = BW_CLAMP(dsp.latetap, 1 + (int)dsp.moddepth, dsp.prelen - 2 - (int)dsp.moddepth);
	dsp.modstep = 2 * (float)M_PI / (BW_CLAMP(p->modtime, 0.04f, 4.0f) * r);
	/* the tail's level: a comb's impulse response holds 1 / (1 - fb^2) of
	 * energy, so the combs are scaled to make the tail's energy follow
	 * lategain * gain alone, whatever the decay time */
	{
		float e = 0;
		for (int c = 0; c < NUM_COMBS; c++)
			e += 1 / BW_MAX(1e-4f, 1 - dsp.combfb[c] * dsp.combfb[c]);
		dsp.lategain = BW_CLAMP(p->lategain, 0, 10) * gtot * LATE_LEVEL / sqrtf(e);
	}
	dsp.echotap = BW_CLAMP((int)(BW_CLAMP(p->echotime, 0.075f, 0.25f) * r), 1, dsp.echolen - 1);
	dsp.echofb = BW_CLAMP(p->echodepth, 0, 1) * 0.55f;
	dsp.lategain *= sqrtf(1 - dsp.echofb * dsp.echofb); /* the echo adds its own energy */
	/* the repeats are heard directly as well, as a slap-back */
	dsp.echoout = BW_CLAMP(p->echodepth, 0, 1) * gtot * 0.8f;
	dsp.inlp_a = lowpass_for(p->gainhf, hfref, r);
	dsp.drylp_a = t->water ? lowpass_for(BW_MAX(p->gainhf, 0.05f), hfref, r) : 0;
}

static void clear_buffers(void)
{
	memset(dsp.pre, 0, sizeof(float) * dsp.prelen);
	memset(dsp.echo, 0, sizeof(float) * dsp.echolen);
	for (int s = 0; s < 2; s++) {
		for (int c = 0; c < NUM_COMBS; c++) {
			memset(dsp.comb[s][c], 0, sizeof(float) * dsp.combmax[s][c]);
			dsp.combstore[s][c] = 0;
		}
		for (int a = 0; a < NUM_ALLPASS; a++)
			memset(dsp.ap[s][a], 0, sizeof(float) * dsp.aplen[s][a]);
	}
	dsp.inlp = 0;
}

/* --- the mixer side ------------------------------------------------------------ */

void S_InitDSP(int rate)
{
	dsp.rate = rate;
	dsp.prelen = (int)(PREDELAY_SEC * rate) + 4;
	dsp.pre = Mem_Alloc(sizeof(float) * dsp.prelen);
	dsp.echolen = (int)(ECHO_SEC * rate) + 4;
	dsp.echo = Mem_Alloc(sizeof(float) * dsp.echolen);
	for (int s = 0; s < 2; s++) {
		for (int c = 0; c < NUM_COMBS; c++) {
			dsp.combmax[s][c] = (int)((comb_base[c] + s * STEREO_SPREAD) * (float)rate / 44100) + 2;
			dsp.comb[s][c] = Mem_Alloc(sizeof(float) * dsp.combmax[s][c]);
		}
		for (int a = 0; a < NUM_ALLPASS; a++) {
			dsp.aplen[s][a] = (int)((ap_base[a] + s * STEREO_SPREAD / 2) * (float)rate / 44100) + 1;
			dsp.ap[s][a] = Mem_Alloc(sizeof(float) * dsp.aplen[s][a]);
		}
	}
	memset(&cur, 0, sizeof(cur));
}

void S_ShutdownDSP(void)
{
	Mem_Free(dsp.pre);
	Mem_Free(dsp.echo);
	for (int s = 0; s < 2; s++) {
		for (int c = 0; c < NUM_COMBS; c++)
			Mem_Free(dsp.comb[s][c]);
		for (int a = 0; a < NUM_ALLPASS; a++)
			Mem_Free(dsp.ap[s][a]);
	}
	memset(&dsp, 0, sizeof(dsp));
}

/* runs over the mixed effects (not the music), in place */
void S_ApplyDSP(float *buf, int frames, float volume)
{
	float fadestep;
	if (!dsp.pre)
		return;
	/* a new room fades the old one out, clears the tail and fades in */
	if (memcmp(&want, &cur, sizeof(want)) && !dsp.switching)
		dsp.switching = true;
	if (!cur.on && !dsp.switching)
		return;
	fadestep = 1.0f / (FADE_SEC * dsp.rate);
	for (int i = 0; i < frames; i++) {
		float l = buf[i * 2], r = buf[i * 2 + 1], in, wet[2] = {0, 0}, late, x;
		int p;
		if (dsp.switching) {
			dsp.fade -= fadestep;
			if (dsp.fade <= 0) {
				dsp.fade = 0;
				dsp.switching = false;
				cur = want;
				if (cur.on) {
					compute(&cur);
					clear_buffers();
				}
				if (!cur.on)
					return;
			}
		} else if (dsp.fade < 1) {
			dsp.fade = BW_MIN(1.0f, dsp.fade + fadestep);
		}
		/* under water the direct sound is muffled as well */
		if (cur.water && dsp.drylp_a > 0) {
			dsp.drylp[0] += (1 - dsp.drylp_a) * (l - dsp.drylp[0]);
			dsp.drylp[1] += (1 - dsp.drylp_a) * (r - dsp.drylp[1]);
			l = l + (dsp.drylp[0] - l) * dsp.fade;
			r = r + (dsp.drylp[1] - r) * dsp.fade;
		}
		in = (buf[i * 2] + buf[i * 2 + 1]) * 0.5f;
		dsp.inlp += (1 - dsp.inlp_a) * (in - dsp.inlp);
		dsp.pre[dsp.prepos] = dsp.inlp;
		/* early reflections */
		for (int t = 0; t < 6; t++) {
			p = dsp.prepos - dsp.taps[t];
			if (p < 0)
				p += dsp.prelen;
			wet[0] += dsp.pre[p] * dsp.tapgain[t][0];
			wet[1] += dsp.pre[p] * dsp.tapgain[t][1];
		}
		/* late reverb input, its delay gently modulated */
		{
			float d = dsp.latetap + dsp.moddepth * sinf(dsp.modphase), fp = dsp.prepos - d, frac;
			int i0;
			if (fp < 0)
				fp += dsp.prelen;
			i0 = (int)fp;
			frac = fp - i0;
			late = dsp.pre[i0] + (dsp.pre[(i0 + 1) % dsp.prelen] - dsp.pre[i0]) * frac;
			dsp.modphase += dsp.modstep;
			if (dsp.modphase > 2 * (float)M_PI)
				dsp.modphase -= 2 * (float)M_PI;
		}
		/* echo: a single feedback delay feeding the tail */
		if (dsp.echofb > 0) {
			p = dsp.echopos - dsp.echotap;
			if (p < 0)
				p += dsp.echolen;
			x = dsp.echo[p];
			dsp.echo[dsp.echopos] = late + x * dsp.echofb;
			late += x;
			wet[0] += x * dsp.echoout;
			wet[1] += x * dsp.echoout;
			if (++dsp.echopos == dsp.echolen)
				dsp.echopos = 0;
		}
		for (int s = 0; s < 2; s++) {
			float sum = 0;
			for (int c = 0; c < NUM_COMBS; c++) {
				float *b = dsp.comb[s][c];
				int len = dsp.comblen[s][c], pos = dsp.combpos[s][c];
				float y = b[pos];
				dsp.combstore[s][c] = y + (dsp.combstore[s][c] - y) * dsp.combdamp;
				b[pos] = late + dsp.combstore[s][c] * dsp.combfb[c];
				if (++pos >= len)
					pos = 0;
				dsp.combpos[s][c] = pos;
				sum += y;
			}
			for (int a = 0; a < NUM_ALLPASS; a++) {
				float *b = dsp.ap[s][a], y = b[dsp.appos[s][a]];
				b[dsp.appos[s][a]] = sum + y * dsp.apgain;
				sum = y - sum * dsp.apgain;
				if (++dsp.appos[s][a] >= dsp.aplen[s][a])
					dsp.appos[s][a] = 0;
			}
			wet[s] += sum * dsp.lategain;
		}
		if (++dsp.prepos == dsp.prelen)
			dsp.prepos = 0;
		buf[i * 2] = l + wet[0] * volume * dsp.fade;
		buf[i * 2 + 1] = r + wet[1] * volume * dsp.fade;
	}
}

/* --- choosing the room ------------------------------------------------------------ */

/* guesses the room from traces: how far the walls are, how high the
 * ceiling, and how much of it is open sky */
static bool guess_room(const float *org, reverb_t *out)
{
	static const float zero[3];
	float dsum = 0, open = 0, height, size, d;
	int n = 0;
	bsptrace_t tr;
	if (!cl.world)
		return false;
	for (int i = 0; i < 9; i++) {
		float dir[3], end[3];
		if (i < 8) {
			float a = i * (float)M_PI / 4;
			dir[0] = cosf(a), dir[1] = sinf(a), dir[2] = 0.15f;
		} else {
			dir[0] = dir[1] = 0, dir[2] = 1;
		}
		for (int k = 0; k < 3; k++)
			end[k] = org[k] + dir[k] * 2048;
		BSP_TraceModel(cl.world, 0, zero, org, zero, zero, end, CONTENTS_SOLID, &tr);
		if (tr.startsolid)
			return false;
		d = tr.fraction * 2048;
		if (tr.fraction == 1)
			open += 1;
		else {
			float past[3];
			for (int k = 0; k < 3; k++)
				past[k] = tr.endpos[k] + dir[k] * 4;
			if ((tr.surfaceflags & 4) || (BSP_PointContents(cl.world, 0, zero, past) & CONTENTS_SKY))
				open += 1; /* sky: Q3's SURF_SKY, or a Q1 sky leaf behind the hit */
		}
		if (i < 8) {
			dsum += d;
			n++;
		} else {
			height = d;
		}
	}
	size = dsum / n;
	open /= 9;
	memset(out, 0, sizeof(*out));
	out->density = BW_CLAMP(size / 900, 0.1f, 1.0f);
	out->diffusion = 1;
	out->gain = 0.32f;
	out->gainhf = 0.85f;
	out->gainlf = 1;
	out->decay = BW_CLAMP(0.25f + (size + height * 0.5f) / 500, 0.3f, 5.0f) * (1 - 0.7f * open);
	out->decayhf = 0.8f;
	out->decaylf = 1;
	out->reflgain = 0.6f * (1 - open);
	out->refldelay = BW_CLAMP(size * 2 / SOUND_SPEED, 0.002f, 0.3f);
	out->lategain = 1.3f * (1 - 0.8f * open);
	out->latedelay = BW_CLAMP(height / SOUND_SPEED, 0.004f, 0.1f);
	/* open air: a distant slap-back instead of a room */
	out->echotime = BW_CLAMP(size * 2 / SOUND_SPEED, 0.075f, 0.25f);
	out->echodepth = open > 0.5f && size > 600 ? 0.3f : 0;
	out->modtime = 0.25f;
	out->airabsorption = 0.994f;
	out->hfref = 5000;
	out->lfref = 250;
	out->decayhflimit = 1;
	return true;
}

/* called once a frame from S_Update with the listener's position */
void S_UpdateDSP(const float *org)
{
	target_t t;
	int rt = room_type->ivalue;
	bool underwater = false;
	memset(&t, 0, sizeof(t));
	if (cl.world && dsp_water->ivalue > 0)
		underwater = (BSP_PointContents(cl.world, 0, (const float[3]){0, 0, 0}, org) &
		              (CONTENTS_WATER | CONTENTS_SLIME | CONTENTS_LAVA)) != 0;
	/* most specific first: under water, a forced room type, the game's
	 * slot, then the automatic guess */
	if (dsp_off->ivalue) {
	} else if (underwater || listener_slot == 1) {
		if (slot_set[1])
			t.p = slots[1];
		else
			t.p = room_presets[BW_CLAMP(dsp_water->ivalue, 1, 28)];
		t.water = true;
		t.on = true;
	} else if (rt > 0 && rt < 29) {
		t.p = room_presets[rt];
		t.on = true;
	} else if (rt < 0 && listener_slot > 1 && listener_slot < MAX_SLOTS && slot_set[listener_slot]) {
		t.p = slots[listener_slot];
		t.on = t.p.gain > 0;
	} else if (rt < 0 && dsp_automatic->ivalue) {
		if (cl.realtime >= auto_next) {
			reverb_t g;
			/* only a clearly different room replaces the last guess, as each
			 * change fades the tail out and in */
			if (guess_room(org, &g) &&
			    (!auto_valid || fabsf(g.decay - auto_room.decay) > auto_room.decay * 0.25f ||
			     fabsf(g.density - auto_room.density) > 0.25f || (g.echodepth > 0) != (auto_room.echodepth > 0))) {
				auto_room = g;
				auto_valid = true;
			}
			auto_next = cl.realtime + 0.5;
		}
		if (auto_valid) {
			t.p = auto_room;
			t.on = true;
		}
	}
	if (dsp_show->ivalue && memcmp(&t, &want, sizeof(t)))
		Con_Printf("dsp: %s, decay %.2f s, density %.2f, reflections %.3f s%s\n", t.on ? "on" : "off", t.p.decay,
		           t.p.density, t.p.refldelay, t.water ? ", under water" : "");
	SNDDMA_Lock();
	want = t;
	SNDDMA_Unlock();
}

void S_SetReverbSlot(int slot) { listener_slot = slot; }

/* --- builtins and commands ----------------------------------------------------------- */

/* setup_reverb(slot, reverbinfo_t *info, sizeof(reverbinfo_t)) */
void S_BI_SetupReverb(qcvm_t *vm)
{
	int slot = (int)QC_PFLOAT(vm, 0), size = QC_PINT(vm, 2);
	reverb_t r;
	if (slot < 1 || slot >= MAX_SLOTS || size <= 0)
		return;
	memset(&r, 0, sizeof(r));
	memcpy(&r, QC_Pointer(vm, (uint32_t)QC_PINT(vm, 1), (size_t)BW_MIN(size, (int)sizeof(r))),
	       (size_t)BW_MIN(size, (int)sizeof(r)));
	slots[slot] = r;
	slot_set[slot] = true;
}

static void DSPInfo_f(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	Con_Printf("listener slot %d, room_type %d, %s\n", listener_slot, room_type->ivalue,
	           cur.on ? (cur.water ? "under water" : "reverb on") : "off");
	if (cur.on)
		Con_Printf("decay %.2f s (hf x%.2f), density %.2f, diffusion %.2f, gain %.2f, hf %.3f\n"
		           "reflections %.2f at %.3f s, late %.2f at %.3f s, echo %.2f at %.3f s\n",
		           cur.p.decay, cur.p.decayhf, cur.p.density, cur.p.diffusion, cur.p.gain, cur.p.gainhf, cur.p.reflgain,
		           cur.p.refldelay, cur.p.lategain, cur.p.latedelay, cur.p.echodepth, cur.p.echotime);
}

void S_InitDSPCvars(void)
{
	dsp_off = Cvar_Get("dsp_off", "0", CVAR_ARCHIVE, "Turn room effects off");
	dsp_volume = Cvar_Get("dsp_volume", "1", CVAR_ARCHIVE, "Level of the room effects");
	room_type = Cvar_Get("room_type", "-1", 0,
	                     "Force a GoldSrc room type (0 to 28, 0 is none); -1 lets the game and dsp_automatic choose");
	dsp_water = Cvar_Get("dsp_water", "14", CVAR_ARCHIVE, "Room type used under water (0 to leave water alone)");
	dsp_automatic = Cvar_Get("dsp_automatic", "1", CVAR_ARCHIVE,
	                         "Guess the room from the walls around the listener when the game sets no reverb");
	dsp_show = Cvar_Get("dsp_show", "0", 0, "Print when the room effect changes");
	Cmd_Add("dsp_info", DSPInfo_f);
	/* slot 1 is under water, as in FTE; the game may reconfigure it */
	slot_set[1] = false;
}

float S_DSPVolume(void) { return dsp_volume ? BW_CLAMP(dsp_volume->value, 0, 4) : 1; }
