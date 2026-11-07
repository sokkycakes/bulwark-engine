/* Bulwark engine - the DS's sound gains: the PC mixer's spatialisation
 * (src/sound/snd_main.c spatialise, S_SoundLevel, S_DistanceGain) in fixed
 * point, for sound_nds.c and the host test tools/nds/sndtest.c. The DS build
 * is soft-float (powf and sqrtf per voice per frame would cost milliseconds),
 * so the distance factor comes from a table built once, and the distance and
 * divisions from the hardware's square root and divider. ISC licence, see LICENSE. */
#ifndef BW_SNDMATH_NDS_H
#define BW_SNDMATH_NDS_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef ARM9
#include <nds/arm9/math.h>
#define SND_DIV64(n, d) ((uint32_t)div64((int64_t)(n), (int32_t)(d))) /* REG_DIV, 64/32 */
#define SND_SQRT64(x) ((uint32_t)sqrt64((long long)(x)))                /* REG_SQRT */
#else
#define SND_DIV64(n, d) ((uint32_t)((int64_t)(n) / (int32_t)(d)))
static inline uint32_t snd_isqrt64(uint64_t x)
{
	uint64_t r = (uint64_t)sqrt((double)x);
	while (r * r > x)
		r--;
	while ((r + 1) * (r + 1) <= x)
		r++;
	return (uint32_t)r;
}
#define SND_SQRT64(x) snd_isqrt64((uint64_t)(x))
#endif

#define SND_ONE 65536 /* gains are Q16 */
/* snd_main.c:46-47 SND_CULL_GAIN 0.002: 131.07 in Q16 */
#define SND_CULL_Q16 131
/* snd_main.c:836-839: snd_refdb 60, snd_refdist 36, s_nominaldistance 1000 */
#define SND_REFDB 60.0f
#define SND_REFDIST 36
#define SND_NOMINAL 1000.0f
/* snd_main.c:127: the far ear keeps 30% (pan 0.7) */
#define SND_PAN_Q16 45875 /* 0.7 x 65536 */
/* the distance factor saturates: past this (units) every distance a map has gives gain 1 */
#define SND_FMAX_UNITS 131072u

/* f(atten) = 10^((L - snd_refdb)/20) x snd_refdist, L = 50 + 20/a (S_SoundLevel, S_DistanceGain,
 * snd_main.c:140-153), a = atten byte / 64 (the server's x64, sv_client.c:2027). In units,
 * Q8, clamped; 0 for attenuation 0 (G = 1 everywhere) */
static inline uint32_t snd_factor_q8(int atten_byte)
{
	float a = atten_byte / 64.0f * 1000.0f / SND_NOMINAL, level, f;
	if (a <= 0)
		return 0;
	level = 50.0f + 20.0f / a;
	if ((level - SND_REFDB) / 20.0f > 6.0f) /* 10^6 x 36 units: saturated */
		return SND_FMAX_UNITS * 256u;
	f = powf(10.0f, (level - SND_REFDB) / 20.0f) * SND_REFDIST;
	return f >= (float)SND_FMAX_UNITS ? SND_FMAX_UNITS * 256u : (uint32_t)(f * 256.0f + 0.5f);
}

typedef struct {
	int32_t left, right; /* Q16 */
} sndgain_t;

/* a voice's per-start constant: k = vol x f, so a frame's gain is min(vol, k / dist) */
static inline uint64_t snd_k(uint32_t vol_q16, uint32_t f_q8) { return (uint64_t)vol_q16 * f_q8 * 2048u; }

/* spatialise (snd_main.c:104-137) for a voice at d (its origin minus the listener's, 1/8 units),
 * right the listener's right vector (Q12), vol_q16 the volume x master, f_q8 its
 * snd_factor_q8 (0: no attenuation). listener: heard straight (is_listener, :107-110) */
static inline sndgain_t snd_spatialise(const int32_t d[3], const int32_t right[3], uint32_t vol_q16, uint32_t f_q8,
                                       int nospatialise, int listener)
{
	sndgain_t g;
	int64_t d2 = (int64_t)d[0] * d[0] + (int64_t)d[1] * d[1] + (int64_t)d[2] * d[2];
	uint32_t dist, gain; /* dist in 1/128 units: 1/8 units would be 0.25% off at 50 units */
	int32_t pan;
	if (listener) {
		g.left = g.right = (int32_t)vol_q16;
		return g;
	}
	dist = SND_SQRT64(d2 << 8);
	if (!f_q8)
		gain = vol_q16;
	else {
		/* gain = vol x min(1, f / max(1, dist)) in Q16: vol_q16 x (f_q8 / 256) / (dist / 128)
		 * = k x 16 / dist / 65536 with k = vol x f_q8 x 2048 (snd_k) */
		uint64_t k = snd_k(vol_q16, f_q8) * 16;
		uint32_t den = dist < 128 ? 128 : dist;
		/* k / den >= vol << 16 means saturated (and keeps the quotient in 32 bits) */
		if (k >= ((uint64_t)vol_q16 << 16) * den)
			gain = vol_q16;
		else
			gain = SND_DIV64((k + 32768) >> 16, den) /* < vol: fits */;
	}
	if (gain < SND_CULL_Q16)
		gain = 0;
	if (!gain || nospatialise || dist < 128) { /* dist < 1 unit */
		g.left = g.right = (int32_t)gain;
		return g;
	}
	{
		/* pan = dot(d, right) / dist x 0.7 x min(1, dist / 36) = dot x 0.7 / max(dist, 36);
		 * dot in 1/8 units, dist in 1/128 */
		int64_t dot = (int64_t)d[0] * right[0] + (int64_t)d[1] * right[1] + (int64_t)d[2] * right[2]; /* Q12, 1/8 units */
		uint32_t den = dist < SND_REFDIST * 128 ? SND_REFDIST * 128 : dist;
		int64_t num = dot * SND_PAN_Q16 / 256; /* x 16 (1/8 to 1/128 units) / 4096 (Q12) */
		pan = num >= 0 ? (int32_t)SND_DIV64(num, den) : -(int32_t)SND_DIV64(-num, den);
		if (pan > SND_PAN_Q16)
			pan = SND_PAN_Q16;
		if (pan < -SND_PAN_Q16)
			pan = -SND_PAN_Q16;
	}
	g.left = (int32_t)(((int64_t)gain * (SND_ONE - (pan > 0 ? pan : 0)) + 32768) >> 16);
	g.right = (int32_t)(((int64_t)gain * (SND_ONE - (pan < 0 ? -pan : 0)) + 32768) >> 16);
	return g;
}

/* the DS pans one volume between the ears (left = v(128 - p)/128, right = v p/128, GBATEK):
 * V = (L + R)/2, P = 128 R/(L + R) gives each ear exactly L/2 and R/2, the PC's ratio 6 dB
 * down (headroom for 14 voices). vol11 is calico's 11-bit volume */
static inline void snd_hwvolpan(sndgain_t g, int *vol11, int *pan)
{
	int64_t s = (int64_t)g.left + g.right;
	int v = (int)((s * 2047 / 2 + 32768) >> 16), p;
	*vol11 = v < 0 ? 0 : v > 2047 ? 2047 : v;
	if (s <= 0) {
		*pan = 64;
		return;
	}
	p = (int)(((int64_t)g.right * 128 + s / 2) / s);
	*pan = p < 0 ? 0 : p > 127 ? 127 : p;
}

/* FNV-1a 32 of the normalised name (docs/spec "Sample names"): lower case, \ to /, a
 * leading ./ and one leading sound/ dropped, a trailing .wav .ogg .mp3 dropped */
static inline uint32_t snd_key(const char *name)
{
	char n[128];
	int len = 0;
	uint32_t h = 2166136261u;
	for (const char *s = name; *s && len < (int)sizeof(n) - 1; s++) {
		char c = *s == '\\' ? '/' : *s;
		n[len++] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
	}
	n[len] = 0;
	{
		char *p = n;
		if (p[0] == '.' && p[1] == '/')
			p += 2;
		if (!strncmp(p, "sound/", 6))
			p += 6;
		len = (int)strlen(p);
		if (len > 4 && (!strcmp(p + len - 4, ".wav") || !strcmp(p + len - 4, ".ogg") || !strcmp(p + len - 4, ".mp3")))
			len -= 4;
		for (int i = 0; i < len; i++) {
			h ^= (uint8_t)p[i];
			h *= 16777619u;
		}
	}
	return h;
}

#endif
