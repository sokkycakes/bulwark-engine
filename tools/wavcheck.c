/* wavcheck - runs the engine's WAV decoder on files and prints what it
 * made of them: channels, rate, frames, loop start. Compiles snd_mem.c into
 * itself to reach the decoder without exporting it.
 *
 *   wavcheck file.wav ...
 *
 * Exit status 1 if any file was rejected. ISC licence, see LICENSE. */
#include "../src/sound/snd_mem.c"

static uint8_t *readall(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
	if (n < 0 || fread(b, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(b);
		return NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return b;
}

int main(int argc, char **argv)
{
	int status = 0;
	for (int i = 1; i < argc; i++) {
		size_t len;
		uint8_t *b = readall(argv[i], &len);
		pcm_t pcm = {0};
		if (!b || !decode_wav(b, len, &pcm)) {
			printf("REJECT %s\n", argv[i]);
			status = 1;
		} else {
			int64_t sum = 0;
			for (int k = 0; k < pcm.frames * pcm.channels; k++)
				sum += pcm.data[k] < 0 ? -pcm.data[k] : pcm.data[k];
			printf("ok %s channels %d rate %d frames %d loop %d meanabs %lld\n", argv[i], pcm.channels, pcm.rate,
			       pcm.frames, pcm.loopstart,
			       (long long)(pcm.frames ? sum / ((int64_t)pcm.frames * pcm.channels) : 0));
			Mem_Free(pcm.data);
		}
		free(b);
	}
	return status;
}
