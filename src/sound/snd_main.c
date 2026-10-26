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
static bool media_playing; /* a playlist track is on, so its end moves on */
static bool snd_active;  /* a device is open */
static bool snd_started; /* S_Init ran */
static int snd_rate = 44100;
static float listener_origin[3], listener_right[3] = {0, -1, 0};
static float master_volume = 0.7f, music_volume = 0.5f, dsp_level = 1; /* copies the mixer reads */
static cvar_t *s_volume, *s_bgmvolume, *s_nominaldist, *s_show;
static float *mixbuf;
static int mixbuf_frames;
static float listener_forward[3] = {1, 0, 0};
static bool capturing;           /* the capture mixes; the device plays silence */
static int speaker_mode;         /* soundcontrol: 0 stereo, 1 mono */
static bool speakers_back;       /* soundcontrol cardN back: the speakers face the listener's back */
static bool sound_off;           /* soundcontrol off */

/* raw streams: video soundtracks (0) and voices; each a ring of stereo
 * frames at its own rate */
#define RAW_FRAMES (1 << 17)
typedef struct {
	int16_t *ring;
	unsigned head, tail; /* in frames, wrapping */
	int rate;
	float volume;
	double frac;
} rawstream_t;
static rawstream_t rawstreams[S_MAX_STREAMS];
static float duck = 1; /* game sound scale while a voice talks */

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
	if (speakers_back) {
		/* rear speakers: sides swap, and what's in front is quieter */
		float front = DotProduct(d, listener_forward) / dist;
		pan = -pan;
		gain *= BW_CLAMP(0.65f - 0.35f * front, 0.3f, 1.0f);
	}
	ch->left = gain * (1 - BW_MAX(0.0f, pan));
	ch->right = gain * (1 - BW_MAX(0.0f, -pan));
	if (speaker_mode == 1)
		ch->left = ch->right = (ch->left + ch->right) * 0.5f;
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

static void media_frame(void);

void S_Update(const float *origin, const float *forward, const float *right, const float *up)
{
	(void)up;
	media_frame();
	if (!snd_started)
		return;
	SNDDMA_Lock();
	if (origin)
		memcpy(listener_origin, origin, sizeof(listener_origin));
	if (right)
		memcpy(listener_right, right, sizeof(listener_right));
	if (forward)
		memcpy(listener_forward, forward, sizeof(listener_forward));
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

static void mix_raw(rawstream_t *r, float *out, int frames)
{
	double step = (double)r->rate / snd_rate;
	for (int i = 0; i < frames && r->tail != r->head; i++) {
		const int16_t *a = r->ring + (r->tail % RAW_FRAMES) * 2;
		const int16_t *b = r->tail + 1 != r->head ? r->ring + ((r->tail + 1) % RAW_FRAMES) * 2 : a;
		float t = (float)r->frac;
		out[i * 2] += (a[0] + (b[0] - a[0]) * t) * r->volume;
		out[i * 2 + 1] += (a[1] + (b[1] - a[1]) * t) * r->volume;
		r->frac += step;
		while (r->frac >= 1 && r->tail != r->head) {
			r->frac -= 1;
			r->tail++;
		}
	}
}

static void paint(int16_t *out, int frames);

void S_PaintAudio(int16_t *out, int frames)
{
	/* while capturing, the capture paints; the device hears nothing */
	if (capturing || sound_off) {
		memset(out, 0, (size_t)frames * 4);
		return;
	}
	paint(out, frames);
}

void S_CaptureMix(int16_t *out, int frames)
{
	if (!snd_started || !mixbuf) {
		memset(out, 0, (size_t)frames * 4);
		return;
	}
	SNDDMA_Lock();
	for (int done = 0; done < frames;) {
		int n = BW_MIN(frames - done, mixbuf_frames);
		paint(out + done * 2, n);
		done += n;
	}
	SNDDMA_Unlock();
}

void S_SetCapture(bool on)
{
	SNDDMA_Lock();
	capturing = on;
	SNDDMA_Unlock();
}

int S_Rate(void) { return snd_rate; }

void S_StreamSamples(int stream, const int16_t *frames, int count, int rate, float volume)
{
	if (!snd_started || !mixbuf || stream < 0 || stream >= S_MAX_STREAMS)
		return;
	rawstream_t *r = &rawstreams[stream];
	SNDDMA_Lock();
	if (!r->ring)
		r->ring = Mem_Alloc(sizeof(int16_t) * 2 * RAW_FRAMES);
	if (r->rate != rate || r->head == r->tail)
		r->frac = 0;
	r->rate = rate;
	r->volume = volume;
	for (int i = 0; i < count && r->head - r->tail < RAW_FRAMES - 1; i++, r->head++) {
		r->ring[(r->head % RAW_FRAMES) * 2] = frames[i * 2];
		r->ring[(r->head % RAW_FRAMES) * 2 + 1] = frames[i * 2 + 1];
	}
	SNDDMA_Unlock();
}

void S_StreamClear(int stream)
{
	if (stream < 0 || stream >= S_MAX_STREAMS)
		return;
	SNDDMA_Lock();
	rawstreams[stream].head = rawstreams[stream].tail = 0;
	SNDDMA_Unlock();
}

int S_StreamQueued(int stream)
{
	if (stream < 0 || stream >= S_MAX_STREAMS || !rawstreams[stream].rate || !snd_rate)
		return 0;
	const rawstream_t *r = &rawstreams[stream];
	return (int)((double)(r->head - r->tail) * snd_rate / r->rate);
}

void S_RawSamples(const int16_t *frames, int count, int rate, float volume)
{
	S_StreamSamples(0, frames, count, rate, volume);
}

void S_RawClear(void) { S_StreamClear(0); }
int S_RawQueued(void) { return S_StreamQueued(0); }

void S_SetDucking(float scale)
{
	duck = scale < 0 ? 0 : scale > 1 ? 1 : scale;
}

static void paint(int16_t *out, int frames)
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
	if (duck < 1)
		for (int i = 0; i < frames * 2; i++)
			mixbuf[i] *= duck;
	for (int i = 0; i < S_MAX_STREAMS; i++)
		if (rawstreams[i].ring && rawstreams[i].head != rawstreams[i].tail)
			mix_raw(&rawstreams[i], mixbuf, frames);
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
	media_playing = false; /* this replaces the playlist's track */
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
	media_playing = false;
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

/* playvol <sound> <volume> [<sound> <volume>...] */
static void PlayVol_f(int argc, const char **argv)
{
	for (int i = 1; i + 1 < argc; i += 2)
		start_sound(0, 0, S_FindSfx(argv[i], true), NULL, (float)atof(argv[i + 1]), 0, 100, 0, 0);
}

typedef struct {
	int count;
	size_t bytes;
} sfxstats_t;

static void list_sfx(const sfx_t *sfx, void *ctx)
{
	sfxstats_t *st = ctx;
	size_t bytes = sfx->loaded ? (size_t)sfx->frames * sfx->channels * 2 : 0;
	if (sfx->loaded)
		Con_Printf("%c %6dHz %s %7.2fs %8zu %s\n", sfx->loopstart >= 0 ? 'L' : ' ', sfx->rate,
		           sfx->channels == 2 ? "stereo" : "mono  ", sfx->rate ? (double)sfx->frames / sfx->rate : 0, bytes,
		           sfx->name);
	else
		Con_Printf("  %s %s\n", sfx->failed ? "missing" : "not loaded", sfx->name);
	st->count++;
	st->bytes += bytes;
}

static void SoundList_f(int argc, const char **argv)
{
	sfxstats_t st = {0};
	S_ForEachSfx(list_sfx, &st);
	Con_Printf("%d sounds, %zu bytes of samples\n", st.count, st.bytes);
}

static void SoundInfo_f(int argc, const char **argv)
{
	int live = 0;
	if (!snd_active) {
		Con_Printf("sound is off\n");
		return;
	}
	for (int i = 0; i < MAX_CHANNELS; i++)
		if (channel_live(&channels[i]))
			live++;
	Con_Printf("%d Hz stereo, 16 bit; %d of %d channels playing; volume %g, music %g\n", snd_rate, live, MAX_CHANNELS,
	           s_volume->value, s_bgmvolume->value);
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

/* --- the media player: a playlist of music tracks ------------------------------------ */

static char *playlist[256];
static int numtracks, curtrack = -1;

static bool play_track(int i)
{
	bool ok;
	if (i < 0 || i >= numtracks || !snd_started)
		return false;
	curtrack = i;
	SNDDMA_Lock();
	ok = S_OpenMusic(playlist[i], false);
	SNDDMA_Unlock();
	media_playing = ok;
	Con_Printf(ok ? "playing %s\n" : "^3can't play %s\n", playlist[i]);
	return ok;
}

static void next_track(void)
{
	for (int tries = 0; tries < numtracks; tries++)
		if (play_track((curtrack + 1) % numtracks))
			return;
	media_playing = false;
}

/* the playlist lives in sound/media.m3u */
static void save_playlist(void)
{
	char path[1024];
	FILE *f;
	if (!FS_WritePath("sound/media.m3u", path, sizeof(path)) || !FS_CreatePath(path) || !(f = fopen(path, "w")))
		return;
	fprintf(f, "#EXTM3U\n");
	for (int i = 0; i < numtracks; i++)
		fprintf(f, "%s\n", playlist[i]);
	fclose(f);
}

static void load_playlist(void)
{
	char *text = FS_LoadFile("sound/media.m3u", NULL);
	for (char *l = text; l && *l && numtracks < (int)BW_ARRAY_COUNT(playlist);) {
		char *e = strchr(l, '\n');
		if (e)
			*e = 0;
		size_t n = strlen(l);
		while (n && (l[n - 1] == '\r' || l[n - 1] == ' '))
			l[--n] = 0;
		if (*l && *l != '#')
			playlist[numtracks++] = Mem_Strdup(l);
		l = e ? e + 1 : NULL;
	}
	Mem_Free(text);
}

static void media_frame(void)
{
	static bool loaded;
	if (!loaded) {
		loaded = true;
		load_playlist();
	}
	if (media_playing && snd_started && !S_MusicActive())
		next_track();
}

/* media_add <track>: adds a music file to the playlist */
static void MediaAdd_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("media_add <track>\n");
		for (int i = 0; i < numtracks; i++)
			Con_Printf("%s%d: %s\n", i == curtrack ? "^2" : "", i + 1, playlist[i]);
		return;
	}
	for (int i = 1; i < argc && numtracks < (int)BW_ARRAY_COUNT(playlist); i++)
		playlist[numtracks++] = Mem_Strdup(argv[i]);
	save_playlist();
	if (!media_playing && !S_MusicActive())
		play_track(numtracks - 1);
}

/* media_remove <track>: by name or number */
static void MediaRemove_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("media_remove <track>\n");
		return;
	}
	for (int i = 0; i < numtracks; i++) {
		if (strcmp(playlist[i], argv[1]) && atoi(argv[1]) != i + 1)
			continue;
		Mem_Free(playlist[i]);
		memmove(playlist + i, playlist + i + 1, (size_t)(numtracks - i - 1) * sizeof(playlist[0]));
		numtracks--;
		save_playlist();
		if (curtrack == i) {
			media_playing = false;
			SNDDMA_Lock();
			S_CloseMusic();
			SNDDMA_Unlock();
			curtrack = i - 1;
		} else if (curtrack > i)
			curtrack--;
		return;
	}
	Con_Printf("no track %s\n", argv[1]);
}

/* media_next, music_next: the next track */
static void MediaNext_f(int argc, const char **argv)
{
	if (numtracks)
		next_track();
	else
		Con_Printf("the playlist is empty (media_add <track>)\n");
}

/* music_fforward / music_rewind [seconds]: moves through the playing track */
static void MusicSeek_f(int argc, const char **argv)
{
	double secs = argc > 1 ? atof(argv[1]) : 10;
	if (!snd_started)
		return;
	if (strstr(argv[0], "rewind"))
		secs = -secs;
	SNDDMA_Lock();
	S_SeekMusic(secs);
	SNDDMA_Unlock();
}

/* soundcontrol off|single|multi|cardN mono|stereo|front|back */
static void SoundControl_f(int argc, const char **argv)
{
	const char *a = argc > 1 ? argv[1] : "";
	if (!strcmp(a, "off")) {
		sound_off = true;
	} else if (!strcmp(a, "single")) {
		sound_off = false;
	} else if (!strcmp(a, "multi")) {
		Con_Printf("only one sound device is supported\n");
	} else if (!strncmp(a, "card", 4) && argc > 2) {
		if (atoi(a + 4) != 0) {
			Con_Printf("there is only card0\n");
			return;
		}
		SNDDMA_Lock();
		if (!strcmp(argv[2], "mono"))
			speaker_mode = 1;
		else if (!strcmp(argv[2], "stereo"))
			speaker_mode = 0;
		else if (!strcmp(argv[2], "front"))
			speakers_back = false;
		else if (!strcmp(argv[2], "back"))
			speakers_back = true;
		SNDDMA_Unlock();
	} else {
		Con_Printf("valid commands are: off, single, multi, cardX mono, cardX stereo, cardX front, cardX back\n");
	}
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
	Cmd_AddD("play2", Play_f, "play2 <sound>...: play sounds without position");
	Cmd_AddD("playvol", PlayVol_f, "playvol <sound> <volume>: play a sound at a volume");
	Cmd_AddD("soundlist", SoundList_f, "List the loaded sounds");
	Cmd_AddD("soundinfo", SoundInfo_f, "Sound device and mixer state");
	Cmd_Add("stopsound", StopSound_f);
	Cmd_Add("snd_restart", SndRestart_f);
	Cmd_AddD("media_add", MediaAdd_f, "media_add <track>: add music to the playlist (lists it without one)");
	Cmd_AddD("media_remove", MediaRemove_f, "media_remove <track>: take music off the playlist");
	Cmd_AddD("media_next", MediaNext_f, "Play the next track of the playlist");
	Cmd_AddD("music_next", MediaNext_f, "Play the next track of the playlist");
	Cmd_AddD("music_fforward", MusicSeek_f, "music_fforward [seconds]: skip ahead in the music (10 seconds)");
	Cmd_AddD("music_rewind", MusicSeek_f, "music_rewind [seconds]: go back in the music (10 seconds)");
	Cmd_AddD("soundcontrol", SoundControl_f, "soundcontrol off|single|multi|card0 mono|stereo|front|back");
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
