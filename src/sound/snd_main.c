/* Bulwark engine - sound: channels, spatialisation, the mixer, music and
 * the client game's sound builtins. The channel model follows what the
 * fteextensions.qc documentation describes for sound(): channel 0 mixes
 * any number of samples, other channels of an entity replace each other.
 * ISC licence, see LICENSE. */
#include "snd_local.h"
#include "../client/client.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_CHANNELS 128
#define NUM_AMBIENT 32   /* the first channels hold ambientsound loops */

/* the flags documented as SOUNDFLAG_* in fteextensions.qc */
#define SNDF_FORCELOOP 2
#define SNDF_NOSPATIALISE 4
#define SNDF_ABSVOLUME 16
#define SNDF_FOLLOW 64
#define SNDF_NOREPLACE 128
/* engine-side: the entity number belongs to the client game, not the server */
#define SNDF_CSQCENT (1 << 20)

typedef struct {
	sfx_t *sfx;
	int ent, entchannel, flags;
	float origin[3];
	float volume, attenuation;
	float pitch;            /* 1 is normal speed */
	double pos;             /* in sample frames; negative while delayed */
	float left, right;      /* gains worked out by spatialise() */
	bool ambient;
} channel_t;

static channel_t channels[MAX_CHANNELS];
static bool snd_active;  /* a device is open */
static bool snd_started; /* S_Init ran */
static int snd_rate = 44100;
static float listener_origin[3], listener_right[3] = {0, -1, 0};
static float master_volume = 0.7f, music_volume = 0.5f, dsp_level = 1; /* copies the mixer reads */
static cvar_t *s_volume, *s_bgmvolume, *s_nominaldist, *s_show;
static float *mixbuf;
static int mixbuf_frames;

/* --- channels ----------------------------------------------------------------- */

static bool channel_live(const channel_t *ch) { return ch->sfx != NULL; }

/* the listener's own sounds are heard straight, as in Quake and Source */
static bool is_listener(const channel_t *ch)
{
	if (ch->flags & SNDF_CSQCENT)
		return false; /* the client game spatialises its own entities as it likes */
	return ch->ent == cl.playerent && ch->ent > 0;
}

static bool entity_origin(const channel_t *ch, float *out)
{
	qcvm_t *vm = cl.csqc;
	int e = ch->ent;
	if (e <= 0)
		return false;
	if (!(ch->flags & SNDF_CSQCENT)) {
		if (!cl.ents || e >= cl.numents)
			return false;
		e = cl.ents[e].csqc_ent;
	}
	if (!vm || e <= 0 || e >= vm->num_edicts || vm->edicts[e].free || vm->fofs.origin < 0)
		return false;
	memcpy(out, &QC_EdictFields(vm, e)[vm->fofs.origin].f, sizeof(float) * 3);
	return true;
}

/* gains from distance and direction. Attenuation 1 fades a sound out over
 * s_nominaldist units; 0 plays it everywhere at full volume. */
static void spatialise(channel_t *ch)
{
	float vol = ch->volume * ((ch->flags & SNDF_ABSVOLUME) ? 1.0f : master_volume);
	float d[3], dist, gain, pan;
	if ((ch->flags & SNDF_FOLLOW) && !ch->ambient)
		entity_origin(ch, ch->origin);
	if (is_listener(ch) || ch->attenuation <= 0) {
		ch->left = ch->right = vol;
		return;
	}
	for (int i = 0; i < 3; i++)
		d[i] = ch->origin[i] - listener_origin[i];
	dist = sqrtf(DotProduct(d, d));
	gain = vol * BW_MAX(0.0f, 1.0f - dist * ch->attenuation / BW_MAX(1.0f, s_nominaldist->value));
	if (gain <= 0 || (ch->flags & SNDF_NOSPATIALISE) || dist < 1) {
		ch->left = ch->right = gain;
		return;
	}
	/* the far ear loses up to 70% for a sound fully to one side */
	pan = DotProduct(d, listener_right) / dist * 0.7f;
	ch->left = gain * (1 - BW_MAX(0.0f, pan));
	ch->right = gain * (1 - BW_MAX(0.0f, -pan));
}

static channel_t *pick_channel(int ent, int entchannel, int flags)
{
	channel_t *best = NULL;
	double bestpos = -1;
	/* an entity's numbered channel replaces what played on it before */
	if (entchannel != 0) {
		for (int i = NUM_AMBIENT; i < MAX_CHANNELS; i++) {
			channel_t *ch = &channels[i];
			if (channel_live(ch) && ch->ent == ent && ch->entchannel == entchannel &&
			    (ch->flags & SNDF_CSQCENT) == (flags & SNDF_CSQCENT)) {
				if (flags & SNDF_NOREPLACE)
					return NULL;
				return ch;
			}
		}
	}
	for (int i = NUM_AMBIENT; i < MAX_CHANNELS; i++) {
		channel_t *ch = &channels[i];
		if (!channel_live(ch))
			return ch;
		/* otherwise steal the sound that has played longest, but not the listener's */
		if (!is_listener(ch) && ch->pos > bestpos) {
			best = ch;
			bestpos = ch->pos;
		}
	}
	return best;
}

static void start_sound(int ent, int entchannel, sfx_t *sfx, const float *origin, float vol, float atten, float pitch,
                        int flags, float timeofs)
{
	channel_t *ch;
	if (!snd_started || !sfx || (!sfx->loaded && !S_LoadSfx(sfx)))
		return;
	SNDDMA_Lock();
	ch = pick_channel(ent, entchannel, flags);
	if (ch) {
		memset(ch, 0, sizeof(*ch));
		ch->sfx = sfx;
		ch->ent = ent;
		ch->entchannel = entchannel;
		ch->flags = flags;
		if (origin)
			memcpy(ch->origin, origin, sizeof(ch->origin));
		else if (!entity_origin(ch, ch->origin))
			memcpy(ch->origin, listener_origin, sizeof(ch->origin));
		ch->volume = vol;
		ch->attenuation = atten;
		ch->pitch = pitch > 0 ? pitch / 100.0f : 1.0f;
		ch->pos = timeofs * sfx->rate; /* negative offsets delay the start */
		spatialise(ch);
	}
	SNDDMA_Unlock();
}

void S_StartSound(int ent, int channel, const char *name, const float *origin, float volume, float attenuation,
                  float pitch, int flags)
{
	start_sound(ent, channel, S_FindSfx(name, true), origin, volume, attenuation, pitch, flags, 0);
}

static void start_ambient(const float *origin, sfx_t *sfx, float vol, float atten)
{
	if (!snd_started || !sfx || (!sfx->loaded && !S_LoadSfx(sfx)))
		return;
	SNDDMA_Lock();
	for (int i = 0; i < NUM_AMBIENT; i++) {
		channel_t *ch = &channels[i];
		if (channel_live(ch))
			continue;
		memset(ch, 0, sizeof(*ch));
		ch->sfx = sfx;
		ch->ambient = true;
		ch->flags = SNDF_FORCELOOP;
		memcpy(ch->origin, origin, sizeof(ch->origin));
		ch->volume = vol;
		ch->attenuation = atten;
		ch->pitch = 1;
		spatialise(ch);
		break;
	}
	SNDDMA_Unlock();
}

static void stop_channel(int ent, int entchannel, int flags)
{
	SNDDMA_Lock();
	for (int i = NUM_AMBIENT; i < MAX_CHANNELS; i++) {
		channel_t *ch = &channels[i];
		if (channel_live(ch) && ch->ent == ent && (entchannel < 0 || ch->entchannel == entchannel) &&
		    (ch->flags & SNDF_CSQCENT) == (flags & SNDF_CSQCENT))
			ch->sfx = NULL;
	}
	SNDDMA_Unlock();
}

static channel_t *find_channel(int ent, int entchannel, int flags)
{
	for (int i = NUM_AMBIENT; i < MAX_CHANNELS; i++) {
		channel_t *ch = &channels[i];
		if (channel_live(ch) && ch->ent == ent && ch->entchannel == entchannel &&
		    (ch->flags & SNDF_CSQCENT) == (flags & SNDF_CSQCENT))
			return ch;
	}
	return NULL;
}

void S_StopAllSounds(void)
{
	if (!snd_started)
		return;
	SNDDMA_Lock();
	memset(channels, 0, sizeof(channels));
	SNDDMA_Unlock();
}

bool S_PrecacheSound(const char *name)
{
	sfx_t *s = S_FindSfx(name, snd_started);
	return s && !s->failed;
}

void S_Update(const float *origin, const float *forward, const float *right, const float *up)
{
	(void)forward;
	(void)up;
	if (!snd_started)
		return;
	SNDDMA_Lock();
	if (origin)
		memcpy(listener_origin, origin, sizeof(listener_origin));
	if (right)
		memcpy(listener_right, right, sizeof(listener_right));
	master_volume = BW_MAX(0.0f, s_volume->value);
	dsp_level = S_DSPVolume();
	music_volume = BW_MAX(0.0f, s_bgmvolume->value);
	for (int i = 0; i < MAX_CHANNELS; i++)
		if (channel_live(&channels[i]))
			spatialise(&channels[i]);
	SNDDMA_Unlock();
	S_UpdateDSP(listener_origin);
	if (s_show->ivalue) {
		int n = 0;
		for (int i = 0; i < MAX_CHANNELS; i++)
			if (channel_live(&channels[i]))
				n++;
		Con_Printf("%d sounds playing\n", n);
	}
}

/* --- mixing --------------------------------------------------------------------- */

static void mix_channel(channel_t *ch, float *out, int frames)
{
	sfx_t *s = ch->sfx;
	double step = (double)s->rate / snd_rate * ch->pitch;
	bool loop = s->loopstart >= 0 || (ch->flags & SNDF_FORCELOOP);
	int loopstart = s->loopstart >= 0 ? s->loopstart : 0;
	for (int i = 0; i < frames; i++) {
		int f;
		float t, l, r;
		if (ch->pos < 0) {
			ch->pos += step;
			continue;
		}
		f = (int)ch->pos;
		if (f >= s->frames) {
			if (!loop || s->frames <= loopstart) {
				ch->sfx = NULL;
				return;
			}
			ch->pos -= s->frames - loopstart;
			f = (int)ch->pos;
		}
		t = (float)(ch->pos - f);
		if (s->channels == 2) {
			const int16_t *a = s->data + f * 2;
			const int16_t *b = f + 1 < s->frames ? a + 2 : a;
			l = a[0] + (b[0] - a[0]) * t;
			r = a[1] + (b[1] - a[1]) * t;
		} else {
			const int16_t *a = s->data + f;
			const int16_t *b = f + 1 < s->frames ? a + 1 : a;
			l = r = a[0] + (b[0] - a[0]) * t;
		}
		out[i * 2] += l * ch->left;
		out[i * 2 + 1] += r * ch->right;
		ch->pos += step;
	}
}

void S_PaintAudio(int16_t *out, int frames)
{
	if (frames > mixbuf_frames) {
		memset(out, 0, (size_t)frames * 4);
		return;
	}
	memset(mixbuf, 0, sizeof(float) * 2 * (size_t)frames);
	for (int i = 0; i < MAX_CHANNELS; i++) {
		channel_t *ch = &channels[i];
		if (channel_live(ch) && ch->sfx->loaded && (ch->left > 0 || ch->right > 0 || ch->pos < 0))
			mix_channel(ch, mixbuf, frames);
		else if (channel_live(ch) && ch->sfx->loaded)
			ch->pos += (double)ch->sfx->rate / snd_rate * ch->pitch * frames; /* silent, keep time */
	}
	S_ApplyDSP(mixbuf, frames, dsp_level); /* music stays out of the room */
	S_MixMusic(mixbuf, frames, snd_rate, music_volume);
	for (int i = 0; i < frames * 2; i++) {
		float v = mixbuf[i];
		out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
}

/* --- music ------------------------------------------------------------------------ */

/* "music <name> [-]": a path, or a CD track number. A trailing "-" plays it
 * once; otherwise it loops. A missing extension, or one this engine can't
 * decode (such as .flac), falls back to .ogg then .wav. */
static void Music_f(int argc, const char **argv)
{
	char base[160], path[192];
	static const char *exts[] = {"", ".ogg", ".wav"};
	bool loop = !(argc > 2 && !strcmp(argv[2], "-"));
	char *dot;
	if (argc < 2) {
		Con_Printf("usage: music <file or track number> [-]\n");
		return;
	}
	if (atoi(argv[1]) > 0 && strspn(argv[1], "0123456789") == strlen(argv[1]))
		snprintf(base, sizeof(base), "music/track%02d", atoi(argv[1]));
	else
		Str_Copy(base, argv[1], sizeof(base));
	if (!snd_started)
		return;
	for (size_t i = 0; i < BW_ARRAY_COUNT(exts); i++) {
		bool ok;
		snprintf(path, sizeof(path), "%s%s", base, exts[i]);
		SNDDMA_Lock();
		ok = S_OpenMusic(path, loop);
		SNDDMA_Unlock();
		if (ok)
			return;
		if (i == 0 && (dot = strrchr(base, '.')) && !strchr(dot, '/'))
			*dot = 0; /* try the other formats under the same name */
	}
	Con_DPrintf("music: can't play %s\n", argv[1]);
}

static void StopMusic_f(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	if (!snd_started)
		return;
	SNDDMA_Lock();
	S_CloseMusic();
	SNDDMA_Unlock();
}

static void Play_f(int argc, const char **argv)
{
	for (int i = 1; i < argc; i++)
		start_sound(0, 0, S_FindSfx(argv[i], true), NULL, 1, 0, 100, 0, 0);
}

static void StopSound_f(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	S_StopAllSounds();
}

static void SndRestart_f(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	S_StopAllSounds();
	/* samples are reloaded from disk the next time they are played */
	SNDDMA_Lock();
	S_FreeAllSfx();
	SNDDMA_Unlock();
}

/* --- setup -------------------------------------------------------------------------- */

void S_Init(bool headless)
{
	s_volume = Cvar_Get("volume", "0.7", CVAR_ARCHIVE, "Sound effect volume");
	s_bgmvolume = Cvar_Get("bgmvolume", "0.5", CVAR_ARCHIVE, "Music volume");
	s_nominaldist = Cvar_Get("snd_nominaldistance", "1000", 0, "Distance over which an attenuation 1 sound fades out");
	s_show = Cvar_Get("snd_show", "0", 0, "Print how many sounds are playing");
	Cmd_Add("music", Music_f);
	Cmd_Add("stopmusic", StopMusic_f);
	Cmd_Add("play", Play_f);
	Cmd_Add("stopsound", StopSound_f);
	Cmd_Add("snd_restart", SndRestart_f);
	S_InitDSPCvars();
	if (headless)
		return;
	mixbuf_frames = 8192;
	mixbuf = Mem_Alloc(sizeof(float) * 2 * (size_t)mixbuf_frames);
	snd_started = true; /* before the device starts pulling frames */
	snd_active = SNDDMA_Init(&snd_rate);
	if (!snd_active) {
		Con_Printf("no sound device: sound is off\n");
		snd_started = false;
		return;
	}
	SNDDMA_Lock();
	S_InitDSP(snd_rate);
	SNDDMA_Unlock();
}

void S_Shutdown(void)
{
	if (snd_active)
		SNDDMA_Shutdown();
	S_ShutdownDSP();
	snd_active = snd_started = false;
	S_CloseMusic();
	S_FreeAllSfx();
	memset(channels, 0, sizeof(channels));
	Mem_Free(mixbuf);
	mixbuf = NULL;
}

/* --- builtins ----------------------------------------------------------------------- */

/* the sound builtins in csprogs and menu.dat; the server's networked ones
 * live in the server */

static int argflags(qcvm_t *vm, int n) { return vm->argc > n ? (int)QC_PFLOAT(vm, n) : 0; }

static void bi_precache_sound(qcvm_t *vm)
{
	S_PrecacheSound(QC_PSTR(vm, 0));
	vm->globals[OFS_RETURN] = *QC_Parm(vm, 0); /* returns its argument */
}

static void bi_sound(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0);
	float pitch = vm->argc > 5 ? QC_PFLOAT(vm, 5) : 100;
	float timeofs = vm->argc > 7 ? QC_PFLOAT(vm, 7) : 0;
	start_sound(e, (int)QC_PFLOAT(vm, 1), S_FindSfx(QC_PSTR(vm, 2), true), NULL, QC_PFLOAT(vm, 3), QC_PFLOAT(vm, 4),
	            pitch, argflags(vm, 6) | SNDF_CSQCENT, timeofs);
}

static void bi_soundupdate(qcvm_t *vm)
{
	int e = QC_PENT(vm, 0), chan = (int)QC_PFLOAT(vm, 1);
	const char *name = QC_PSTR(vm, 2);
	float vol = QC_PFLOAT(vm, 3);
	channel_t *ch;
	if (vol < 0) {
		stop_channel(e, chan, SNDF_CSQCENT);
		QC_ReturnFloat(vm, 1);
		return;
	}
	SNDDMA_Lock();
	ch = find_channel(e, chan, SNDF_CSQCENT);
	if (ch) {
		sfx_t *s = name[0] ? S_FindSfx(name, false) : NULL;
		if (s && s->loaded && s != ch->sfx) {
			ch->sfx = s;
			ch->pos = 0;
		}
		ch->volume = vol;
		ch->attenuation = QC_PFLOAT(vm, 4);
		ch->pitch = QC_PFLOAT(vm, 5) > 0 ? QC_PFLOAT(vm, 5) / 100 : 1;
		ch->flags = argflags(vm, 6) | SNDF_CSQCENT;
		ch->pos += QC_PFLOAT(vm, 7) * ch->sfx->rate;
		spatialise(ch);
	}
	SNDDMA_Unlock();
	QC_ReturnFloat(vm, ch ? 1 : 0);
	if (!ch && name[0]) /* nothing to update: start it */
		bi_sound(vm);
}

static void bi_stopsound(qcvm_t *vm) { stop_channel(QC_PENT(vm, 0), (int)QC_PFLOAT(vm, 1), SNDF_CSQCENT); }

static void bi_pointsound(qcvm_t *vm)
{
	start_sound(0, 0, S_FindSfx(QC_PSTR(vm, 1), true), QC_PVEC(vm, 0), QC_PFLOAT(vm, 2), QC_PFLOAT(vm, 3), 100,
	            SNDF_CSQCENT, 0);
}

static void bi_localsound(qcvm_t *vm)
{
	float vol = vm->argc > 2 ? QC_PFLOAT(vm, 2) : 1;
	start_sound(0, 0, S_FindSfx(QC_PSTR(vm, 0), true), NULL, vol, 0, 100, SNDF_CSQCENT, 0);
}

static void bi_ambientsound(qcvm_t *vm)
{
	start_ambient(QC_PVEC(vm, 0), S_FindSfx(QC_PSTR(vm, 1), true), QC_PFLOAT(vm, 2), QC_PFLOAT(vm, 3));
}

/* SetListener(origin, forward, right, up, reverbslot) */
static void bi_setlistener(qcvm_t *vm)
{
	S_SetReverbSlot(vm->argc > 4 ? (int)QC_PFLOAT(vm, 4) : 0);
	S_Update(QC_PVEC(vm, 0), QC_PVEC(vm, 1), QC_PVEC(vm, 2), QC_PVEC(vm, 3));
}

static void bi_getsoundtime(qcvm_t *vm)
{
	channel_t *ch;
	float t = -1;
	SNDDMA_Lock();
	ch = find_channel(QC_PENT(vm, 0), (int)QC_PFLOAT(vm, 1), SNDF_CSQCENT);
	if (ch && ch->sfx->rate > 0)
		t = (float)(ch->pos / ch->sfx->rate);
	SNDDMA_Unlock();
	QC_ReturnFloat(vm, t);
}

static void bi_soundlength(qcvm_t *vm)
{
	sfx_t *s = S_FindSfx(QC_PSTR(vm, 0), true);
	if (s && !s->loaded)
		S_LoadSfx(s); /* lengths are known even without a device */
	QC_ReturnFloat(vm, s && s->loaded && s->rate > 0 ? (float)s->frames / s->rate : 0);
}

const qc_builtin_t s_builtins[] = {
	{"precache_sound", bi_precache_sound}, {"precache_sound2", bi_precache_sound}, {"sound", bi_sound},
	{"soundupdate", bi_soundupdate}, {"stopsound", bi_stopsound}, {"pointsound", bi_pointsound},
	{"pointsound_proper", bi_pointsound}, {"localsound", bi_localsound}, {"ambientsound", bi_ambientsound},
	{"SetListener", bi_setlistener}, {"getsoundtime", bi_getsoundtime}, {"soundlength", bi_soundlength},
	{"setup_reverb", S_BI_SetupReverb}, {NULL, NULL}};
