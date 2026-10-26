/* Bulwark engine - sound samples and the music stream. WAV is read from
 * the RIFF layout described in Microsoft's multimedia file format notes;
 * Ogg Vorbis is decoded with stb_vorbis (public domain). ISC licence, see
 * LICENSE. */
#include "snd_local.h"
#include "../common/fs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "../thirdparty/stb_vorbis.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* --- sample cache ------------------------------------------------------------ */

#define SFX_HASH 256
static sfx_t *sfxhash[SFX_HASH];

static unsigned sfx_hashname(const char *s)
{
	unsigned h = 5381;
	for (; *s; s++) {
		char c = *s >= 'A' && *s <= 'Z' ? *s + 32 : *s;
		h = h * 33 + (unsigned char)(c == '\\' ? '/' : c);
	}
	return h % SFX_HASH;
}

sfx_t *S_FindSfx(const char *name, bool load)
{
	unsigned h;
	sfx_t *s;
	if (!name || !name[0])
		return NULL;
	if (name[0] == '*') /* player sounds of the old protocols are not used */
		name++;
	h = sfx_hashname(name);
	for (s = sfxhash[h]; s; s = s->next)
		if (!Str_ICmp(s->name, name))
			break;
	if (!s) {
		s = Mem_Alloc(sizeof(*s));
		Str_Copy(s->name, name, sizeof(s->name));
		s->loopstart = -1;
		s->next = sfxhash[h];
		sfxhash[h] = s;
	}
	if (load && !s->loaded && !s->failed)
		S_LoadSfx(s);
	return s;
}

void S_FreeAllSfx(void)
{
	for (int i = 0; i < SFX_HASH; i++) {
		while (sfxhash[i]) {
			sfx_t *n = sfxhash[i]->next;
			Mem_Free(sfxhash[i]->data);
			Mem_Free(sfxhash[i]);
			sfxhash[i] = n;
		}
	}
}

/* --- WAV ----------------------------------------------------------------------- */

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

typedef struct {
	int16_t *data;
	int frames, channels, rate, loopstart;
} pcm_t;

/* RIFF/WAVE with 8 or 16 bit integer PCM. A "cue " chunk marks where a
 * sample loops from, as level designers set them for ambient loops. */
static bool decode_wav(const uint8_t *buf, size_t len, pcm_t *out)
{
	const uint8_t *p = buf + 12, *end = buf + len, *pcm = NULL;
	uint32_t pcmlen = 0;
	int fmt = 0, channels = 0, rate = 0, bits = 0, loop = -1;
	if (len < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4))
		return false;
	while (p + 8 <= end) {
		uint32_t size = le32(p + 4);
		const uint8_t *body = p + 8;
		if (size > (uint32_t)(end - body))
			size = (uint32_t)(end - body);
		if (!memcmp(p, "fmt ", 4) && size >= 16) {
			fmt = le16(body);
			channels = le16(body + 2);
			rate = (int)le32(body + 4);
			bits = le16(body + 14);
		} else if (!memcmp(p, "data", 4)) {
			pcm = body;
			pcmlen = size;
		} else if (!memcmp(p, "cue ", 4) && size >= 28 && le32(body) > 0) {
			/* first cue point: id, position, chunk id, chunk start, block start, sample offset */
			loop = (int)le32(body + 4 + 20);
		}
		p = body + size + (size & 1);
	}
	if (fmt != 1 || !pcm || (bits != 8 && bits != 16) || channels < 1 || channels > 2 || rate <= 0)
		return false;
	out->channels = channels;
	out->rate = rate;
	out->frames = (int)(pcmlen / (uint32_t)(bits / 8 * channels));
	out->data = Mem_Alloc((size_t)out->frames * channels * sizeof(int16_t) + 2);
	for (int i = 0; i < out->frames * channels; i++)
		out->data[i] = bits == 8 ? (int16_t)((pcm[i] - 128) << 8) : (int16_t)le16(pcm + i * 2);
	out->loopstart = loop >= 0 && loop < out->frames ? loop : -1;
	return true;
}

static bool decode_ogg(const uint8_t *buf, size_t len, pcm_t *out)
{
	short *data = NULL;
	int channels = 0, rate = 0;
	int frames = stb_vorbis_decode_memory(buf, (int)len, &channels, &rate, &data);
	if (frames <= 0 || !data)
		return false;
	if (channels > 2) {
		/* keep the front pair */
		for (int i = 0; i < frames; i++) {
			data[i * 2] = data[i * channels];
			data[i * 2 + 1] = data[i * channels + 1];
		}
		channels = 2;
	}
	out->data = Mem_Alloc((size_t)frames * channels * sizeof(int16_t) + 2);
	memcpy(out->data, data, (size_t)frames * channels * sizeof(int16_t));
	free(data); /* stb_vorbis allocated it with malloc */
	out->frames = frames;
	out->channels = channels;
	out->rate = rate;
	out->loopstart = -1;
	return true;
}

static bool decode_file(const char *path, pcm_t *out)
{
	size_t len;
	uint8_t *buf = (uint8_t *)FS_LoadFile(path, &len);
	bool ok;
	if (!buf)
		return false;
	ok = len >= 4 && !memcmp(buf, "OggS", 4) ? decode_ogg(buf, len, out) : decode_wav(buf, len, out);
	if (!ok)
		Con_DPrintf("sound %s: not a PCM wav or ogg vorbis file\n", path);
	Mem_Free(buf);
	return ok;
}

/* sample names are relative to sound/, and an .ogg may stand in for a .wav */
static bool find_and_decode(const char *name, pcm_t *out)
{
	char path[192], base[160];
	const char *ext;
	snprintf(path, sizeof(path), "sound/%s", name);
	if (decode_file(path, out) || decode_file(name, out))
		return true;
	Str_Copy(base, name, sizeof(base));
	ext = strrchr(base, '.');
	if (ext && !strchr(ext, '/'))
		base[ext - base] = 0;
	snprintf(path, sizeof(path), "sound/%s.ogg", base);
	if (decode_file(path, out))
		return true;
	snprintf(path, sizeof(path), "sound/%s.wav", base);
	return decode_file(path, out);
}

bool S_LoadSfx(sfx_t *sfx)
{
	pcm_t pcm;
	if (sfx->loaded)
		return true;
	if (sfx->failed || !find_and_decode(sfx->name, &pcm)) {
		if (!sfx->failed)
			Con_DPrintf("sound %s: not found\n", sfx->name);
		sfx->failed = true;
		return false;
	}
	sfx->frames = pcm.frames;
	sfx->channels = pcm.channels;
	sfx->rate = pcm.rate;
	sfx->loopstart = pcm.loopstart;
	sfx->data = pcm.data; /* published last: the mixer checks loaded */
	sfx->loaded = true;
	return true;
}

/* --- music -------------------------------------------------------------------- */

/* One stream: Ogg files are decoded as they play, WAV files are held whole.
 * Touched by the mixer thread, so the caller holds the device lock. */
static struct {
	bool active, loop;
	uint8_t *file;          /* the compressed file, for stb_vorbis */
	stb_vorbis *vorbis;
	pcm_t wav;              /* or a whole decoded WAV */
	int channels, rate;
	short buf[4096 * 2];    /* decoded frames waiting to be mixed */
	int buffered, bufpos;
	double frac;            /* resampling position between buf frames */
	int wavpos;
} mus;

void S_CloseMusic(void)
{
	if (mus.vorbis)
		stb_vorbis_close(mus.vorbis);
	Mem_Free(mus.file);
	Mem_Free(mus.wav.data);
	memset(&mus, 0, sizeof(mus));
}

bool S_OpenMusic(const char *path, bool loop)
{
	size_t len;
	uint8_t *file = (uint8_t *)FS_LoadFile(path, &len);
	S_CloseMusic();
	if (!file)
		return false;
	if (len >= 4 && !memcmp(file, "OggS", 4)) {
		int err = 0;
		stb_vorbis_info info;
		mus.vorbis = stb_vorbis_open_memory(file, (int)len, &err, NULL);
		if (!mus.vorbis) {
			Mem_Free(file);
			return false;
		}
		info = stb_vorbis_get_info(mus.vorbis);
		mus.file = file;
		mus.channels = info.channels;
		mus.rate = (int)info.sample_rate;
	} else {
		bool ok = decode_wav(file, len, &mus.wav);
		Mem_Free(file);
		if (!ok)
			return false;
		mus.channels = mus.wav.channels;
		mus.rate = mus.wav.rate;
	}
	mus.loop = loop;
	mus.active = true;
	return true;
}

bool S_MusicActive(void) { return mus.active; }

/* seconds into the current track */
double S_MusicTime(void)
{
	if (!mus.active || mus.rate <= 0)
		return 0;
	int left = mus.buffered - mus.bufpos;
	if (mus.vorbis)
		return (double)(stb_vorbis_get_sample_offset(mus.vorbis) - left) / mus.rate;
	return (double)(mus.wavpos - left) / mus.rate;
}

/* moves the current track by seconds (either way) */
void S_SeekMusic(double seconds)
{
	if (!mus.active)
		return;
	double t = S_MusicTime() + seconds;
	if (t < 0)
		t = 0;
	unsigned frame = (unsigned)(t * mus.rate);
	if (mus.vorbis) {
		unsigned total = stb_vorbis_stream_length_in_samples(mus.vorbis);
		if (frame >= total) {
			/* past the end: the track is over */
			if (!mus.loop) {
				S_CloseMusic();
				return;
			}
			frame = 0;
		}
		stb_vorbis_seek(mus.vorbis, frame);
	} else {
		if ((int)frame >= mus.wav.frames)
			frame = mus.loop ? 0 : (unsigned)mus.wav.frames;
		mus.wavpos = (int)frame;
	}
	mus.buffered = mus.bufpos = 0;
	mus.frac = 0;
}

/* refills the decode buffer; false at the end of a stream that doesn't loop */
static bool refill(void)
{
	int got;
	mus.bufpos = 0;
	if (mus.vorbis) {
		got = stb_vorbis_get_samples_short_interleaved(mus.vorbis, 2, mus.buf, (int)BW_ARRAY_COUNT(mus.buf));
		if (got <= 0 && mus.loop) {
			stb_vorbis_seek_start(mus.vorbis);
			got = stb_vorbis_get_samples_short_interleaved(mus.vorbis, 2, mus.buf, (int)BW_ARRAY_COUNT(mus.buf));
		}
	} else {
		int want = (int)BW_ARRAY_COUNT(mus.buf) / 2;
		if (mus.wavpos >= mus.wav.frames && mus.loop)
			mus.wavpos = 0;
		got = BW_MIN(want, mus.wav.frames - mus.wavpos);
		for (int i = 0; i < got; i++) {
			const int16_t *f = mus.wav.data + (size_t)(mus.wavpos + i) * mus.wav.channels;
			mus.buf[i * 2] = f[0];
			mus.buf[i * 2 + 1] = f[mus.wav.channels > 1 ? 1 : 0];
		}
		mus.wavpos += BW_MAX(got, 0);
	}
	mus.buffered = BW_MAX(got, 0);
	return mus.buffered > 0;
}

void S_MixMusic(float *out, int frames, int rate, float volume)
{
	double step;
	if (!mus.active || volume <= 0)
		return;
	step = (double)mus.rate / rate;
	for (int i = 0; i < frames; i++) {
		int a, b;
		float t;
		/* the last frame of each block only serves as an interpolation end */
		while (mus.bufpos + 1 >= mus.buffered) {
			if (!refill()) {
				S_CloseMusic();
				return;
			}
		}
		a = mus.bufpos * 2;
		b = a + 2;
		t = (float)mus.frac;
		out[i * 2] += (mus.buf[a] + (mus.buf[b] - mus.buf[a]) * t) * volume;
		out[i * 2 + 1] += (mus.buf[a + 1] + (mus.buf[b + 1] - mus.buf[a + 1]) * t) * volume;
		mus.frac += step;
		while (mus.frac >= 1.0) {
			mus.frac -= 1.0;
			mus.bufpos++;
		}
	}
}

void S_ForEachSfx(void (*fn)(const sfx_t *sfx, void *ctx), void *ctx)
{
	for (int i = 0; i < SFX_HASH; i++)
		for (sfx_t *s = sfxhash[i]; s; s = s->next)
			fn(s, ctx);
}
