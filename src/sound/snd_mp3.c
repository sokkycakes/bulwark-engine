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
