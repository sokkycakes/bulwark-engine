/* Bulwark engine - video capture: the screen and the game's sound recorded
 * into an AVI file (Motion JPEG pictures, PCM sound) or, with the raw
 * driver, a directory of images and a WAV file. While capturing the game
 * steps exactly one video frame of time per frame and the sound is mixed
 * for exactly that time, so picture and sound stay together however slowly
 * frames are drawn.
 *
 * The AVI layout follows the RIFF AVI format as Microsoft documents it:
 * an 'hdrl' list with the main header and a stream list for each stream,
 * a 'movi' list of '00dc' picture and '01wb' sound chunks, and an 'idx1'
 * index. ISC licence, see LICENSE. */
#include "client.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../render/render.h"
#include "../sound/sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

static cvar_t *capturedriver, *capturecodec, *capturerate, *capturesound, *capturesoundchannels, *capturesoundbits;
static cvar_t *capturedemowidth, *capturedemoheight, *capturemessage, *capturethrottlesize;

#define AVI_LIMIT (1900u << 20) /* AVI 1.0 offsets are 32 bit; stop well short */

static struct {
	bool active, paused, raw, demo;
	char name[256];
	FILE *f;           /* the AVI, or the raw WAV */
	char dir[1024];    /* raw: where the images go */
	char ext[8];
	int width, height;
	double fps;
	int frames;
	/* sound */
	int rate, channels, bits;
	double audio_frac;
	size_t audio_bytes;
	/* AVI bookkeeping */
	long movi_start; /* file offset of the 'movi' fourcc */
	long avih_frames_at, vstrh_len_at, astrh_len_at, riff_size_at, movi_size_at;
	struct {
		char id[4];
		uint32_t ofs, size, flags;
	} *index;
	int numindex, capindex;
	float old_framerate;
	bool had_framerate;
} cap;

/* --- writing ------------------------------------------------------------------------ */

static void put32(FILE *f, uint32_t v)
{
	uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
	fwrite(b, 1, 4, f);
}

static void put16(FILE *f, uint16_t v)
{
	uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
	fwrite(b, 1, 2, f);
}

static void fourcc(FILE *f, const char *cc) { fwrite(cc, 1, 4, f); }

static void patch32(FILE *f, long at, uint32_t v)
{
	long here = ftell(f);
	fseek(f, at, SEEK_SET);
	put32(f, v);
	fseek(f, here, SEEK_SET);
}

static void avi_header(void)
{
	FILE *f = cap.f;
	bool sound = cap.rate > 0;
	int block = cap.channels * cap.bits / 8;
	fourcc(f, "RIFF");
	cap.riff_size_at = ftell(f);
	put32(f, 0);
	fourcc(f, "AVI ");
	fourcc(f, "LIST");
	long hdrl_size_at = ftell(f);
	put32(f, 0);
	fourcc(f, "hdrl");
	/* main header */
	fourcc(f, "avih");
	put32(f, 56);
	put32(f, (uint32_t)(1000000.0 / cap.fps));         /* microseconds per frame */
	put32(f, (uint32_t)(cap.width * cap.height * 3 * cap.fps)); /* max bytes per second */
	put32(f, 0);                                        /* padding granularity */
	put32(f, 0x10);                                     /* has an index */
	cap.avih_frames_at = ftell(f);
	put32(f, 0);                                        /* total frames */
	put32(f, 0);                                        /* initial frames */
	put32(f, sound ? 2 : 1);                            /* streams */
	put32(f, (uint32_t)(cap.width * cap.height * 3));   /* suggested buffer */
	put32(f, (uint32_t)cap.width);
	put32(f, (uint32_t)cap.height);
	for (int i = 0; i < 4; i++)
		put32(f, 0);
	/* the picture stream */
	fourcc(f, "LIST");
	put32(f, 4 + 8 + 56 + 8 + 40);
	fourcc(f, "strl");
	fourcc(f, "strh");
	put32(f, 56);
	fourcc(f, "vids");
	fourcc(f, "MJPG");
	put32(f, 0);       /* flags */
	put16(f, 0);       /* priority */
	put16(f, 0);       /* language */
	put32(f, 0);       /* initial frames */
	put32(f, 1000);    /* scale */
	put32(f, (uint32_t)(cap.fps * 1000 + 0.5)); /* rate: frames per second is rate / scale */
	put32(f, 0);       /* start */
	cap.vstrh_len_at = ftell(f);
	put32(f, 0);       /* length in frames */
	put32(f, (uint32_t)(cap.width * cap.height * 3));
	put32(f, 0xffffffffu); /* quality: default */
	put32(f, 0);       /* sample size: varies */
	put16(f, 0);
	put16(f, 0);
	put16(f, (uint16_t)cap.width);
	put16(f, (uint16_t)cap.height);
	fourcc(f, "strf");
	put32(f, 40);
	put32(f, 40);
	put32(f, (uint32_t)cap.width);
	put32(f, (uint32_t)cap.height);
	put16(f, 1);
	put16(f, 24);
	fourcc(f, "MJPG");
	put32(f, (uint32_t)(cap.width * cap.height * 3));
	put32(f, 0);
	put32(f, 0);
	put32(f, 0);
	put32(f, 0);
	if (sound) {
		fourcc(f, "LIST");
		put32(f, 4 + 8 + 56 + 8 + 18);
		fourcc(f, "strl");
		fourcc(f, "strh");
		put32(f, 56);
		fourcc(f, "auds");
		put32(f, 0);
		put32(f, 0);
		put16(f, 0);
		put16(f, 0);
		put32(f, 0);
		put32(f, (uint32_t)block);              /* scale */
		put32(f, (uint32_t)(cap.rate * block)); /* rate: bytes per second */
		put32(f, 0);
		cap.astrh_len_at = ftell(f);
		put32(f, 0);                            /* length in blocks */
		put32(f, (uint32_t)(cap.rate * block));
		put32(f, 0xffffffffu);
		put32(f, (uint32_t)block);              /* sample size */
		for (int i = 0; i < 4; i++)
			put16(f, 0);
		fourcc(f, "strf");
		put32(f, 18);
		put16(f, 1); /* PCM */
		put16(f, (uint16_t)cap.channels);
		put32(f, (uint32_t)cap.rate);
		put32(f, (uint32_t)(cap.rate * block));
		put16(f, (uint16_t)block);
		put16(f, (uint16_t)cap.bits);
		put16(f, 0);
	}
	patch32(f, hdrl_size_at, (uint32_t)(ftell(f) - hdrl_size_at - 4));
	fourcc(f, "LIST");
	cap.movi_size_at = ftell(f);
	put32(f, 0);
	cap.movi_start = ftell(f);
	fourcc(f, "movi");
}

static void avi_chunk(const char *id, const void *data, size_t len)
{
	if (cap.numindex == cap.capindex) {
		cap.capindex = cap.capindex ? cap.capindex * 2 : 1024;
		cap.index = Mem_Realloc(cap.index, (size_t)cap.capindex * sizeof(*cap.index));
	}
	memcpy(cap.index[cap.numindex].id, id, 4);
	cap.index[cap.numindex].ofs = (uint32_t)(ftell(cap.f) - cap.movi_start);
	cap.index[cap.numindex].size = (uint32_t)len;
	cap.index[cap.numindex].flags = id[2] == 'd' ? 0x10 : 0; /* every picture is a key frame */
	cap.numindex++;
	fourcc(cap.f, id);
	put32(cap.f, (uint32_t)len);
	fwrite(data, 1, len, cap.f);
	if (len & 1)
		fputc(0, cap.f);
}

static void avi_finish(void)
{
	FILE *f = cap.f;
	int block = cap.channels * cap.bits / 8;
	patch32(f, cap.movi_size_at, (uint32_t)(ftell(f) - cap.movi_size_at - 4));
	fourcc(f, "idx1");
	put32(f, (uint32_t)cap.numindex * 16);
	for (int i = 0; i < cap.numindex; i++) {
		fourcc(f, cap.index[i].id);
		put32(f, cap.index[i].flags);
		put32(f, cap.index[i].ofs);
		put32(f, cap.index[i].size);
	}
	patch32(f, cap.riff_size_at, (uint32_t)(ftell(f) - 8));
	patch32(f, cap.avih_frames_at, (uint32_t)cap.frames);
	patch32(f, cap.vstrh_len_at, (uint32_t)cap.frames);
	if (cap.rate > 0)
		patch32(f, cap.astrh_len_at, (uint32_t)(cap.audio_bytes / (size_t)block));
}

static void wav_header(FILE *f, int rate, int channels, int bits, uint32_t bytes)
{
	int block = channels * bits / 8;
	fourcc(f, "RIFF");
	put32(f, 36 + bytes);
	fourcc(f, "WAVE");
	fourcc(f, "fmt ");
	put32(f, 16);
	put16(f, 1);
	put16(f, (uint16_t)channels);
	put32(f, (uint32_t)rate);
	put32(f, (uint32_t)(rate * block));
	put16(f, (uint16_t)block);
	put16(f, (uint16_t)bits);
	fourcc(f, "data");
	put32(f, bytes);
}

/* --- each frame --------------------------------------------------------------------- */

static void stop(const char *why);

static void write_audio(void)
{
	if (cap.rate <= 0)
		return;
	cap.audio_frac += cap.rate / cap.fps;
	int frames = (int)cap.audio_frac;
	cap.audio_frac -= frames;
	if (frames <= 0)
		return;
	int16_t *mix = Mem_Alloc((size_t)frames * 4);
	S_CaptureMix(mix, frames);
	/* to the chosen channels and bits */
	size_t bytes = (size_t)frames * (size_t)cap.channels * (size_t)(cap.bits / 8);
	uint8_t *out = Mem_Alloc(bytes);
	size_t o = 0;
	for (int i = 0; i < frames; i++) {
		int l = mix[i * 2], r = mix[i * 2 + 1];
		int v[2] = {cap.channels == 1 ? (l + r) / 2 : l, r};
		for (int c = 0; c < cap.channels; c++) {
			if (cap.bits == 8)
				out[o++] = (uint8_t)((v[c] >> 8) + 128);
			else {
				out[o++] = (uint8_t)v[c];
				out[o++] = (uint8_t)(v[c] >> 8);
			}
		}
	}
	if (cap.raw)
		fwrite(out, 1, bytes, cap.f);
	else
		avi_chunk("01wb", out, bytes);
	cap.audio_bytes += bytes;
	Mem_Free(out);
	Mem_Free(mix);
}

static void grab(const uint8_t *rgb, int w, int h)
{
	strbuf_t sb = {0};
	if (!cap.active || cap.paused)
		return;
	if (w != cap.width || h != cap.height) {
		stop("the window changed size");
		return;
	}
	if (!R_EncodeImage(cap.raw ? cap.ext : "jpg", rgb, w, h, (int)Cvar_Value("jpeg_quality"), &sb)) {
		stop("can't encode the picture");
		return;
	}
	if (cap.raw) {
		char path[1100];
		snprintf(path, sizeof(path), "%s/%05d.%s", cap.dir, cap.frames, cap.ext);
		FILE *f = fopen(path, "wb");
		if (!f || fwrite(sb.data, 1, sb.len, f) != sb.len) {
			if (f)
				fclose(f);
			SB_Free(&sb);
			stop("can't write the pictures");
			return;
		}
		fclose(f);
	} else {
		avi_chunk("00dc", sb.data, sb.len);
	}
	SB_Free(&sb);
	cap.frames++;
	write_audio();
	if (capturethrottlesize->value > 0 && !(cap.frames % 30)) {
		struct statvfs vfs;
		char where[1024];
		if (FS_WritePath("x", where, sizeof(where)) && strrchr(where, '/'))
			*strrchr(where, '/') = 0;
		if (!statvfs(where, &vfs) && (double)vfs.f_bavail * vfs.f_frsize / (1 << 20) < capturethrottlesize->value) {
			stop("the disk is nearly full (capturethrottlesize)");
			return;
		}
	}
	if (!cap.raw && (uint32_t)ftell(cap.f) > AVI_LIMIT)
		stop("the file reached the AVI size limit");
}

/* each client frame: a captured demo stops at its end */
void CL_CaptureFrame(void)
{
	if (cap.demo && cap.active && !cl.demoplayback && cap.frames > 0)
		stop(NULL); /* the demo ended */
}

/* --- starting and stopping ------------------------------------------------------------- */

static void stop(const char *why)
{
	if (!cap.active)
		return;
	R_SetFrameGrab(NULL);
	S_SetCapture(false);
	if (cap.raw) {
		if (cap.f) {
			fseek(cap.f, 0, SEEK_SET);
			wav_header(cap.f, cap.rate, cap.channels, cap.bits, (uint32_t)cap.audio_bytes);
		}
	} else {
		avi_finish();
	}
	if (cap.f)
		fclose(cap.f);
	cap.f = NULL;
	Mem_Free(cap.index);
	cap.index = NULL;
	cap.numindex = cap.capindex = 0;
	if (cap.had_framerate)
		Cvar_SetValue("host_framerate", cap.old_framerate);
	cap.active = false;
	Con_Printf("%scapture of %s stopped: %d frames (%.1f seconds)\n", why ? "^3" : "", cap.name, cap.frames,
	           cap.frames / cap.fps);
	if (why)
		Con_Printf("^3%s\n", why);
}

static bool start(const char *name)
{
	char rel[300], path[1024];
	if (cap.active) {
		Con_Printf("already capturing %s\n", cap.name);
		return false;
	}
	if (!R_Active()) {
		Con_Printf("capture: nothing is drawn (no window)\n");
		return false;
	}
	memset(&cap, 0, sizeof(cap));
	const char *driver = capturedriver->string;
	cap.raw = !Str_ICmp(driver, "raw");
	if (!cap.raw && driver[0] && Str_ICmp(driver, "avi")) {
		Con_Printf("capture: no driver \"%s\" (use avi or raw)\n", driver);
		return false;
	}
	cap.fps = BW_CLAMP(capturerate->value, 1, 1000);
	R_GetSize(&cap.width, &cap.height);
	{
		/* the real framebuffer size */
		float scale = R_Get2DScale();
		cap.width = (int)(cap.width * scale + 0.5f);
		cap.height = (int)(cap.height * scale + 0.5f);
	}
	if (capturesound->ivalue && S_Rate() > 0) {
		cap.rate = S_Rate();
		cap.channels = capturesoundchannels->ivalue == 1 ? 1 : 2;
		cap.bits = capturesoundbits->ivalue == 8 ? 8 : 16;
	}
	Str_Copy(cap.name, name, sizeof(cap.name));
	if (cap.raw) {
		Str_Copy(cap.ext, capturecodec->string[0] ? capturecodec->string : "tga", sizeof(cap.ext));
		if (Str_ICmp(cap.ext, "tga") && Str_ICmp(cap.ext, "png") && Str_ICmp(cap.ext, "jpg") && Str_ICmp(cap.ext, "bmp")) {
			Con_Printf("capture: the raw driver writes tga, png, jpg or bmp, not %s\n", cap.ext);
			return false;
		}
		snprintf(rel, sizeof(rel), "%s/audio.wav", name);
		if (!FS_WritePath(rel, path, sizeof(path)) || !FS_CreatePath(path)) {
			Con_Printf("capture: can't write %s\n", rel);
			return false;
		}
		Str_Copy(cap.dir, path, sizeof(cap.dir));
		*strrchr(cap.dir, '/') = 0;
		if (cap.rate > 0) {
			cap.f = fopen(path, "wb");
			if (cap.f)
				wav_header(cap.f, cap.rate, cap.channels, cap.bits, 0);
		}
	} else {
		snprintf(rel, sizeof(rel), "%s%s", name, strrchr(name, '.') ? "" : ".avi");
		if (!FS_WritePath(rel, path, sizeof(path)) || !FS_CreatePath(path) || !(cap.f = fopen(path, "wb"))) {
			Con_Printf("capture: can't write %s\n", rel);
			return false;
		}
		Str_Copy(cap.name, rel, sizeof(cap.name));
		avi_header();
	}
	/* the game steps one video frame per drawn frame */
	cvar_t *hf = Cvar_Find("host_framerate");
	if (hf) {
		cap.had_framerate = true;
		cap.old_framerate = hf->value;
		Cvar_SetValue("host_framerate", (float)(1.0 / cap.fps));
	}
	cap.active = true;
	if (cap.rate > 0)
		S_SetCapture(true);
	R_SetFrameGrab(grab);
	Con_Printf("capturing %s at %dx%d, %g frames a second%s\n", cap.name, cap.width, cap.height, cap.fps,
	           cap.rate > 0 ? va(", sound %d Hz", cap.rate) : "");
	return true;
}

static void Capture_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("capture <filename>\n"
		           "Records video output in an avi file (Motion JPEG pictures, PCM sound).\n"
		           "Use capturerate and capturedriver to configure.\n\n"
		           "avi: one .avi file.\n"
		           "raw: Saves the video as a series of image files within the named sub directory, one for each "
		           "frame (capturecodec: tga, png, jpg or bmp). Audio is recorded as a wav file.\n\n"
		           "Current capture settings:\n");
		cvar_t *list[] = {capturedriver, capturecodec, capturedemowidth, capturedemoheight, capturerate,
		                  capturesound, capturesoundchannels, capturesoundbits};
		for (size_t i = 0; i < BW_ARRAY_COUNT(list); i++)
			Con_Printf(" /%s %s\n", list[i]->name, list[i]->string);
		if (cap.active)
			Con_Printf("capturing %s: %d frames%s\n", cap.name, cap.frames, cap.paused ? " (paused)" : "");
		return;
	}
	start(argv[1]);
}

static void Capturestop_f(int argc, const char **argv)
{
	if (!cap.active)
		Con_Printf("not capturing\n");
	stop(NULL);
}

static void Capturepause_f(int argc, const char **argv)
{
	if (!cap.active) {
		Con_Printf("not capturing\n");
		return;
	}
	cap.paused = !cap.paused;
	S_SetCapture(!cap.paused && cap.rate > 0);
	Con_Printf(cap.paused ? "capture paused\n" : "capture resumed\n");
}

/* capturedemo <demo> <output>: plays the demo into a video, stopping at its end */
static void Capturedemo_f(int argc, const char **argv)
{
	if (argc < 3) {
		Con_Printf("capturedemo demoname outputname\n"
		           "captures a demo to video frames, a fixed time step per frame for smooth results\n"
		           "see also: apropos capture\n");
		return;
	}
	if (capturedemowidth->ivalue || capturedemoheight->ivalue)
		Con_Printf("^3capturedemowidth/height: Bulwark captures at the window's size\n");
	Cbuf_InsertText(va("playdemo \"%s\"\n__capturedemo_start \"%s\"\n", argv[1], argv[2]));
}

static void CapturedemoStart_f(int argc, const char **argv)
{
	if (argc < 2 || !cl.demoplayback) {
		Con_Printf("capturedemo: the demo didn't start\n");
		return;
	}
	if (start(argv[1]))
		cap.demo = true;
}

bool CL_Capturing(void) { return cap.active && !cap.paused; }

void CL_InitCapture(void)
{
	capturedriver = Cvar_Get("capturedriver", "", CVAR_ARCHIVE, "The driver to use: avi (the default) or raw");
	capturecodec = Cvar_Get("capturecodec", "tga", CVAR_ARCHIVE,
	                        "the compression/encoding codec to use.\nWith raw capturing, this should be one of tga, png, "
	                        "jpg, bmp (ie: screenshot extensions). AVI files are always Motion JPEG.");
	capturerate = Cvar_Get("capturerate", "30", CVAR_ARCHIVE, "The framerate of the video to capture");
	capturesound = Cvar_Get("capturesound", "1", CVAR_ARCHIVE, "Record the game's sound too");
	capturesoundchannels = Cvar_Get("capturesoundchannels", "2", CVAR_ARCHIVE, "1 for mono, 2 for stereo");
	capturesoundbits = Cvar_Get("capturesoundbits", "16", CVAR_ARCHIVE, "8 or 16 bit sound");
	capturedemowidth = Cvar_Get("capturedemowidth", "0", CVAR_ARCHIVE, "Not used: Bulwark captures at the window's size");
	capturedemoheight = Cvar_Get("capturedemoheight", "0", CVAR_ARCHIVE, "Not used: Bulwark captures at the window's size");
	capturemessage = Cvar_Get("capturemessage", "", 0, "Text drawn at the bottom of the screen while capturing");
	capturethrottlesize = Cvar_Get("capturethrottlesize", "0", 0,
	                               "If set, capturing stops when there is less disk space than this (in MB)");
	Cmd_AddD("capture", Capture_f, "capture <filename>: record the screen and sound to a video");
	Cmd_AddD("capturestop", Capturestop_f, "Aborts the current video capture.");
	Cmd_AddD("capturepause", Capturepause_f,
	         "Pauses the video capture, allowing you to avoid capturing uninteresting parts. This is a toggle.");
	Cmd_AddD("capturedemo", Capturedemo_f, "capturedemo foo.dem foo.avi - Captures a named demo to a named video file.");
	Cmd_AddD("__capturedemo_start", CapturedemoStart_f, "For internal use");
}
