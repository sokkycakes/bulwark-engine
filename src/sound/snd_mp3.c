/* Bulwark engine - MP3 sounds, decoded with minimp3 (CC0). Kept apart from
 * snd_mem.c, whose stb_vorbis has helpers with the same names. ISC licence,
 * see LICENSE. */
#include "snd_local.h"

#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "../thirdparty/minimp3_ex.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

bool S_DecodeMP3(const uint8_t *buf, size_t len, int16_t **data, int *frames, int *channels, int *rate)
{
	mp3dec_t dec;
	mp3dec_file_info_t info;
	size_t count;
	int keep;
	memset(&info, 0, sizeof(info));
	if (mp3dec_load_buf(&dec, buf, len, &info, NULL, NULL) || !info.buffer || !info.samples || info.channels < 1) {
		free(info.buffer);
		return false;
	}
	count = info.samples / (size_t)info.channels;
	keep = info.channels > 2 ? 2 : info.channels; /* the front pair */
	*data = Mem_Alloc(count * (size_t)keep * sizeof(int16_t) + 2);
	for (size_t i = 0; i < count; i++)
		for (int c = 0; c < keep; c++)
			(*data)[i * (size_t)keep + (size_t)c] = info.buffer[i * (size_t)info.channels + (size_t)c];
	free(info.buffer); /* minimp3 allocated it with malloc */
	*frames = (int)count;
	*channels = keep;
	*rate = info.hz;
	return true;
}

/* --- streamed music ---------------------------------------------------------- */

/* A music track is decoded a block at a time as it plays, like Ogg: opening
 * one only walks the frame headers (to find the length and a seek index). */
struct s_mp3stream_s {
	mp3dec_ex_t dec;
	int channels;
};

s_mp3stream_t *S_MP3Open(const uint8_t *buf, size_t len, int *channels, int *rate)
{
	s_mp3stream_t *s = Mem_Alloc(sizeof(*s));
	if (mp3dec_ex_open_buf(&s->dec, buf, len, MP3D_SEEK_TO_SAMPLE) || s->dec.info.channels < 1 ||
	    s->dec.info.hz <= 0) {
		mp3dec_ex_close(&s->dec);
		Mem_Free(s);
		return NULL;
	}
	s->channels = s->dec.info.channels;
	*channels = s->channels > 2 ? 2 : s->channels;
	*rate = s->dec.info.hz;
	return s;
}

/* up to frames stereo frames into out (frames * 2 shorts); 0 at the end */
int S_MP3Read(s_mp3stream_t *s, int16_t *out, int frames)
{
	int16_t tmp[1152 * 2];
	int done = 0;
	while (done < frames) {
		int want = frames - done;
		size_t got;
		if (want > 1152)
			want = 1152;
		got = mp3dec_ex_read(&s->dec, tmp, (size_t)want * (size_t)s->channels) / (size_t)s->channels;
		if (!got)
			break;
		for (size_t i = 0; i < got; i++) {
			const int16_t *f = tmp + i * (size_t)s->channels;
			out[(done + (int)i) * 2] = f[0];
			out[(done + (int)i) * 2 + 1] = f[s->channels > 1 ? 1 : 0];
		}
		done += (int)got;
	}
	return done;
}

void S_MP3Seek(s_mp3stream_t *s, uint64_t frame) { mp3dec_ex_seek(&s->dec, frame * (uint64_t)s->channels); }
uint64_t S_MP3Tell(const s_mp3stream_t *s) { return s->dec.cur_sample / (uint64_t)s->channels; }
uint64_t S_MP3Length(const s_mp3stream_t *s) { return s->dec.samples / (uint64_t)s->channels; }

void S_MP3Close(s_mp3stream_t *s)
{
	if (!s)
		return;
	mp3dec_ex_close(&s->dec);
	Mem_Free(s);
}
