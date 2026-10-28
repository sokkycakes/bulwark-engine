/* Bulwark engine - voice chat. The microphone is read every frame, cleaned
 * up (gain, a noise gate), and while the player talks (+voip, voice
 * activation, or always, by cl_voip_send) it is encoded in 20ms frames and
 * sent in clc_voice next to the moves. Other players' voices come back as
 * svc_voice and play on a sound stream each.
 *
 * Codecs, numbered as cl_voip_codec: Opus and Speex through the system's
 * libopus and libspeex when they're installed (both BSD licensed, loaded at
 * run time), raw 16-bit PCM, and G.711 A-law and mu-law, written here from
 * the ITU description. ISC licence, see LICENSE. */
#include "client.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../net/protocol.h"
#include "../sound/sound.h"

#include "../common/sys_dl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SPEAKERS 256
#define MAX_FRAME 2048    /* samples in one codec frame, at most */
#define MAX_PACKET_FRAMES 6

static cvar_t *cl_voip_send, *cl_voip_codec, *cl_voip_bitrate, *cl_voip_autogain, *cl_voip_noisefilter;
static cvar_t *cl_voip_micamp, *cl_voip_ducking, *cl_voip_play, *cl_voip_showmeter, *cl_voip_capturingvol;
static cvar_t *cl_voip_vad_delay, *cl_voip_vad_threshhold, *cl_voip_test, *cl_voip_capturedevice;
static cvar_t *cl_voip_capturedevice_opts;

/* --- the libraries ---------------------------------------------------------------- */

static struct {
	bool tried, ok;
	void *(*encoder_create)(int32_t fs, int channels, int application, int *error);
	int32_t (*encode)(void *enc, const int16_t *pcm, int frame_size, unsigned char *data, int32_t max);
	int (*encoder_ctl)(void *enc, int request, ...);
	void (*encoder_destroy)(void *enc);
	void *(*decoder_create)(int32_t fs, int channels, int *error);
	int (*decode)(void *dec, const unsigned char *data, int32_t len, int16_t *pcm, int frame_size, int fec);
	void (*decoder_destroy)(void *dec);
} opus;

static struct {
	bool tried, ok;
	const void *(*lib_get_mode)(int mode);
	void *(*encoder_init)(const void *mode);
	void *(*decoder_init)(const void *mode);
	int (*encoder_ctl)(void *state, int request, void *ptr);
	int (*decoder_ctl)(void *state, int request, void *ptr);
	void (*encoder_destroy)(void *state);
	void (*decoder_destroy)(void *state);
	int (*encode_int)(void *state, int16_t *in, void *bits);
	int (*decode_int)(void *state, void *bits, int16_t *out);
	void (*bits_init)(void *bits);
	void (*bits_reset)(void *bits);
	int (*bits_write)(void *bits, char *bytes, int max);
	void (*bits_read_from)(void *bits, const char *bytes, int len);
	void (*bits_destroy)(void *bits);
} speex;

/* SpeexBits, opaque: speex_bits_init fills it in */
typedef union {
	char pad[128];
	void *align;
} speexbits_t;

#define OPUS_APPLICATION_VOIP 2048
#define OPUS_SET_BITRATE_REQUEST 4002
#define SPEEX_GET_FRAME_SIZE 3
#define SPEEX_SET_QUALITY 4
#define SPEEX_SET_SAMPLING_RATE 24

static void *open_lib(const char *const *names)
{
	for (int i = 0; names[i]; i++) {
		void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
		if (h)
			return h;
	}
	return NULL;
}

#define SYM(lib, h, field, name)                                   \
	do {                                                           \
		*(void **)&lib.field = dlsym(h, name);                     \
		lib.ok &= lib.field != NULL;                               \
	} while (0)

static bool load_opus(void)
{
	static const char *const names[] = {"libopus.so.0", "libopus.so", "libopus.dylib", "libopus-0.dll", NULL};
	if (opus.tried)
		return opus.ok;
	opus.tried = true;
	void *h = open_lib(names);
	if (!h)
		return false;
	opus.ok = true;
	SYM(opus, h, encoder_create, "opus_encoder_create");
	SYM(opus, h, encode, "opus_encode");
	SYM(opus, h, encoder_ctl, "opus_encoder_ctl");
	SYM(opus, h, encoder_destroy, "opus_encoder_destroy");
	SYM(opus, h, decoder_create, "opus_decoder_create");
	SYM(opus, h, decode, "opus_decode");
	SYM(opus, h, decoder_destroy, "opus_decoder_destroy");
	return opus.ok;
}

static bool load_speex(void)
{
	static const char *const names[] = {"libspeex.so.1", "libspeex.so", "libspeex.dylib", "libspeex-1.dll", NULL};
	if (speex.tried)
		return speex.ok;
	speex.tried = true;
	void *h = open_lib(names);
	if (!h)
		return false;
	speex.ok = true;
	SYM(speex, h, lib_get_mode, "speex_lib_get_mode");
	SYM(speex, h, encoder_init, "speex_encoder_init");
	SYM(speex, h, decoder_init, "speex_decoder_init");
	SYM(speex, h, encoder_ctl, "speex_encoder_ctl");
	SYM(speex, h, decoder_ctl, "speex_decoder_ctl");
	SYM(speex, h, encoder_destroy, "speex_encoder_destroy");
	SYM(speex, h, decoder_destroy, "speex_decoder_destroy");
	SYM(speex, h, encode_int, "speex_encode_int");
	SYM(speex, h, decode_int, "speex_decode_int");
	SYM(speex, h, bits_init, "speex_bits_init");
	SYM(speex, h, bits_reset, "speex_bits_reset");
	SYM(speex, h, bits_write, "speex_bits_write");
	SYM(speex, h, bits_read_from, "speex_bits_read_from");
	SYM(speex, h, bits_destroy, "speex_bits_destroy");
	return speex.ok;
}

/* --- G.711 ----------------------------------------------------------------------------- */

static uint8_t ulaw_encode(int16_t pcm)
{
	int s = pcm, sign = 0, exp = 7;
	if (s < 0) {
		s = -s;
		sign = 0x80;
	}
	s = BW_MIN(s, 32635) + 0x84;
	for (int mask = 0x4000; !(s & mask) && exp > 0; mask >>= 1)
		exp--;
	return (uint8_t)~(sign | exp << 4 | ((s >> (exp + 3)) & 0x0f));
}

static int16_t ulaw_decode(uint8_t u)
{
	u = (uint8_t)~u;
	int s = ((((u & 0x0f) << 3) + 0x84) << ((u >> 4) & 7)) - 0x84;
	return (int16_t)(u & 0x80 ? -s : s);
}

static uint8_t alaw_encode(int16_t pcm)
{
	int s = pcm >> 3, mask = 0xd5, seg = 0, a;
	if (s < 0) {
		mask = 0x55;
		s = -s - 1;
	}
	while (seg < 8 && s > (0x1f << seg | ((1 << seg) - 1)))
		seg++;
	if (seg >= 8)
		a = 0x7f;
	else
		a = seg << 4 | ((seg < 2 ? s >> 1 : s >> seg) & 0x0f);
	return (uint8_t)(a ^ mask);
}

static int16_t alaw_decode(uint8_t a)
{
	a ^= 0x55;
	int t = (a & 0x0f) << 4, seg = (a & 0x70) >> 4;
	if (seg == 0)
		t += 8;
	else
		t = (t + 0x108) << (seg - 1);
	return (int16_t)(a & 0x80 ? t : -t);
}

/* --- codecs ------------------------------------------------------------------------- */

static const char *const codec_names[] = {"speex@11025", "raw", "opus@48000", "speex@8000", "speex@16000", "speex@32000",
                                          "pcma@8000", "pcmu@8000"};

static bool is_speex(int codec)
{
	return codec == VOICE_SPEEX11 || codec == VOICE_SPEEX8 || codec == VOICE_SPEEX16 || codec == VOICE_SPEEX32;
}

static int codec_rate(int codec)
{
	switch (codec) {
	case VOICE_SPEEX11: case VOICE_RAW: return 11025;
	case VOICE_OPUS: return 48000;
	case VOICE_SPEEX16: return 16000;
	case VOICE_SPEEX32: return 32000;
	default: return 8000;
	}
}

static int speex_mode(int codec) { return codec == VOICE_SPEEX16 ? 1 : codec == VOICE_SPEEX32 ? 2 : 0; }

/* samples in a frame: 20ms for all of them */
static int codec_frame(int codec)
{
	switch (codec) {
	case VOICE_OPUS: return 960;
	case VOICE_RAW: return 220;
	case VOICE_SPEEX16: return 320;
	case VOICE_SPEEX32: return 640;
	default: return 160;
	}
}

/* false and why when this build can't use it */
static bool codec_usable(int codec, const char **why)
{
	*why = NULL;
	if (codec == VOICE_OPUS && !load_opus())
		*why = "Unable to use opus codec - not installed";
	else if (is_speex(codec) && !load_speex())
		*why = "Unable to use speex codec - not installed";
	else if (codec < 0 || codec > VOICE_PCMU)
		*why = "Unable to use that codec - not implemented";
	return !*why;
}

typedef struct {
	int codec;
	void *state;
	speexbits_t bits;
} coder_t;

static void coder_free(coder_t *c, bool enc)
{
	if (c->state) {
		if (c->codec == VOICE_OPUS)
			(enc ? opus.encoder_destroy : opus.decoder_destroy)(c->state);
		else if (is_speex(c->codec)) {
			(enc ? speex.encoder_destroy : speex.decoder_destroy)(c->state);
			speex.bits_destroy(&c->bits);
		}
	}
	c->state = NULL;
	c->codec = -1;
}

static bool coder_init(coder_t *c, int codec, bool enc)
{
	const char *why;
	coder_free(c, enc);
	if (!codec_usable(codec, &why))
		return false;
	c->codec = codec;
	if (codec == VOICE_OPUS) {
		int err = 0;
		c->state = enc ? opus.encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err) : opus.decoder_create(48000, 1, &err);
		if (!c->state)
			return false;
		if (enc && cl_voip_bitrate->ivalue > 0)
			opus.encoder_ctl(c->state, OPUS_SET_BITRATE_REQUEST, (int32_t)cl_voip_bitrate->ivalue);
	} else if (is_speex(codec)) {
		const void *mode = speex.lib_get_mode(speex_mode(codec));
		int rate = codec_rate(codec), q = 8;
		c->state = enc ? speex.encoder_init(mode) : speex.decoder_init(mode);
		if (!c->state)
			return false;
		speex.bits_init(&c->bits);
		if (enc) {
			speex.encoder_ctl(c->state, SPEEX_SET_SAMPLING_RATE, &rate);
			speex.encoder_ctl(c->state, SPEEX_SET_QUALITY, &q);
		} else
			speex.decoder_ctl(c->state, SPEEX_SET_SAMPLING_RATE, &rate);
	}
	return true;
}

/* one frame in, bytes out; 0 on a failure */
static int encode_frame(coder_t *c, int16_t *pcm, uint8_t *out, int max)
{
	int n = codec_frame(c->codec);
	switch (c->codec) {
	case VOICE_OPUS: {
		int32_t r = opus.encode(c->state, pcm, n, out, max);
		if (r < 0)
			Con_Printf("Opus encoding error: %d\n", (int)r);
		return r < 0 ? 0 : (int)r;
	}
	case VOICE_RAW:
		if (n * 2 > max)
			return 0;
		for (int i = 0; i < n; i++) {
			out[i * 2] = (uint8_t)(pcm[i] & 0xff);
			out[i * 2 + 1] = (uint8_t)((uint16_t)pcm[i] >> 8);
		}
		return n * 2;
	case VOICE_PCMA:
	case VOICE_PCMU:
		if (n > max)
			return 0;
		for (int i = 0; i < n; i++)
			out[i] = c->codec == VOICE_PCMA ? alaw_encode(pcm[i]) : ulaw_encode(pcm[i]);
		return n;
	default:
		speex.bits_reset(&c->bits);
		speex.encode_int(c->state, pcm, &c->bits);
		return speex.bits_write(&c->bits, (char *)out, max);
	}
}

/* bytes in (NULL to make up a lost frame), samples out */
static int decode_frame(coder_t *c, const uint8_t *in, int len, int16_t *pcm)
{
	int n = codec_frame(c->codec);
	switch (c->codec) {
	case VOICE_OPUS: {
		int r = opus.decode(c->state, in, in ? len : 0, pcm, MAX_FRAME, 0);
		if (r < 0)
			Con_DPrintf("Opus decoding error %d\n", r);
		return r < 0 ? 0 : r;
	}
	case VOICE_RAW:
		if (!in)
			break;
		n = BW_MIN(len / 2, MAX_FRAME);
		for (int i = 0; i < n; i++)
			pcm[i] = (int16_t)(in[i * 2] | in[i * 2 + 1] << 8);
		return n;
	case VOICE_PCMA:
	case VOICE_PCMU:
		if (!in)
			break;
		n = BW_MIN(len, MAX_FRAME);
		for (int i = 0; i < n; i++)
			pcm[i] = c->codec == VOICE_PCMA ? alaw_decode(in[i]) : ulaw_decode(in[i]);
		return n;
	default:
		if (in)
			speex.bits_read_from(&c->bits, (const char *)in, len);
		speex.decode_int(c->state, in ? &c->bits : NULL, pcm);
		return n;
	}
	memset(pcm, 0, sizeof(int16_t) * (size_t)n);
	return n;
}

/* --- sending ----------------------------------------------------------------------------- */

static struct {
	bool key;          /* +voip held */
	bool micopen;
	int miccodec;      /* the codec the mic was opened for */
	char micdevice[128];
	coder_t enc;
	int16_t acc[MAX_FRAME * 4];
	int accn;
	float gain;        /* autogain's current gain */
	float maxgain;     /* "voip maxgain" */
	float level;       /* 0-1, the last frame's loudness */
	double lastloud;   /* realtime voice activation last fired */
	bool sending;
	uint8_t gen;
	uint32_t seq;
	uint8_t packet[1400];
	int packetlen, packetframes;
	double meter_time;
} tx = {.miccodec = -1, .gain = 1, .maxgain = 10, .enc = {.codec = -1}};

static int send_codec(void)
{
	const char *why;
	if (cl_voip_codec->string[0]) {
		int c = cl_voip_codec->ivalue;
		return codec_usable(c, &why) ? c : -1;
	}
	return load_opus() ? VOICE_OPUS : VOICE_PCMU;
}

static void mic_close(void)
{
	if (tx.micopen)
		S_MicClose();
	tx.micopen = false;
	tx.accn = 0;
}

static void play_frame(int stream, const int16_t *pcm, int n, int rate, float volume)
{
	int16_t st[MAX_FRAME * 2];
	n = BW_MIN(n, MAX_FRAME);
	for (int i = 0; i < n; i++)
		st[i * 2] = st[i * 2 + 1] = pcm[i];
	S_StreamSamples(stream, st, n, rate, volume);
}

/* gain, gate and loudness for one frame; true when it is loud enough to send */
static bool process_frame(int16_t *pcm, int n)
{
	float amp = cl_voip_micamp->value * tx.gain;
	double sum = 0;
	for (int i = 0; i < n; i++) {
		float v = pcm[i] * amp;
		pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		sum += (double)pcm[i] * pcm[i];
	}
	float rms = (float)sqrt(sum / n);
	tx.level = BW_MIN(rms / 8192.0f, 1.0f);
	if (cl_voip_autogain->ivalue && rms > 64) {
		/* steer towards a speaking level of about a quarter of full scale */
		float want = 8192.0f / (rms / tx.gain);
		tx.gain += (BW_MIN(want, tx.maxgain) - tx.gain) * 0.05f;
	} else if (!cl_voip_autogain->ivalue)
		tx.gain = 1;
	if (cl_voip_noisefilter->ivalue && rms < 200) {
		memset(pcm, 0, sizeof(int16_t) * (size_t)n); /* below the noise floor */
		tx.level = 0;
	}
	/* the threshold is a percentage of the speaking level */
	return tx.level * 100 > cl_voip_vad_threshhold->value;
}

static void flush_packet(void)
{
	tx.packetlen = tx.packetframes = 0;
}

void CL_VoipWrite(msg_t *m)
{
	if (!tx.packetframes)
		return;
	if (cl.state == ca_active && !(cl_voip_send->ivalue & 4) && m->maxsize - m->cursize > (size_t)tx.packetlen + 16) {
		MSG_WriteByte(m, clc_voice);
		MSG_WriteByte(m, tx.enc.codec);
		MSG_WriteByte(m, tx.gen);
		MSG_WriteVarUInt(m, tx.seq - (uint32_t)tx.packetframes);
		MSG_WriteByte(m, tx.packetframes);
		MSG_WriteData(m, tx.packet, (size_t)tx.packetlen);
	}
	flush_packet();
}

static void capture_frame(void)
{
	int codec = send_codec(), frame;
	bool want = cl_voip_test->ivalue || (cl.state == ca_active && (tx.key || cl_voip_send->ivalue & 3));
	if (!want || codec < 0) {
		if (tx.micopen)
			mic_close();
		tx.sending = false;
		tx.level = 0;
		return;
	}
	if (!tx.micopen || tx.miccodec != codec || strcmp(tx.micdevice, cl_voip_capturedevice->string)) {
		mic_close();
		Str_Copy(tx.micdevice, cl_voip_capturedevice->string, sizeof(tx.micdevice));
		tx.miccodec = codec;
		if (!S_MicOpen(tx.micdevice, codec_rate(codec))) {
			static bool told;
			if (!told)
				Con_Printf("^&C0No microphone detected\n");
			told = true;
			Cvar_Set("cl_voip_send", va("%d", cl_voip_send->ivalue & 4));
			tx.key = false;
			return;
		}
		tx.micopen = true;
	}
	if (tx.enc.codec != codec && !coder_init(&tx.enc, codec, true))
		return;
	frame = codec_frame(codec);
	tx.accn += S_MicRead(tx.acc + tx.accn, (int)BW_ARRAY_COUNT(tx.acc) - tx.accn);
	while (tx.accn >= frame) {
		int16_t pcm[MAX_FRAME];
		memcpy(pcm, tx.acc, sizeof(int16_t) * (size_t)frame);
		tx.accn -= frame;
		memmove(tx.acc, tx.acc + frame, sizeof(int16_t) * (size_t)tx.accn);
		if (process_frame(pcm, frame))
			tx.lastloud = cl.realtime;
		bool send = tx.key || (cl_voip_send->ivalue & 2) ||
		            ((cl_voip_send->ivalue & 1) && cl.realtime - tx.lastloud < cl_voip_vad_delay->value);
		if (!send) {
			tx.sending = false;
			continue;
		}
		if (!tx.sending) {
			tx.gen++; /* a new stretch of talking: the decoders start fresh */
			tx.sending = true;
		}
		if (cl_voip_test->ivalue)
			play_frame(1 + cl.playerslot % (S_MAX_STREAMS - 1), pcm, frame, codec_rate(codec),
			           cl_voip_play->value);
		uint8_t bytes[1024];
		int len = encode_frame(&tx.enc, pcm, bytes, (int)sizeof(bytes));
		tx.seq++;
		if (len <= 0)
			continue;
		if (tx.packetframes >= MAX_PACKET_FRAMES || tx.packetlen + len + 2 > (int)sizeof(tx.packet))
			flush_packet(); /* the moves fell behind: drop the oldest audio */
		tx.packet[tx.packetlen++] = (uint8_t)(len & 0xff);
		tx.packet[tx.packetlen++] = (uint8_t)(len >> 8);
		memcpy(tx.packet + tx.packetlen, bytes, (size_t)len);
		tx.packetlen += len;
		tx.packetframes++;
	}
}

/* --- receiving --------------------------------------------------------------------------- */

typedef struct {
	coder_t dec;
	uint8_t gen;
	uint32_t nextseq;
	double lastheard;
	bool muted;
} speaker_t;

static speaker_t *speakers;

static speaker_t *speaker(int slot)
{
	if (!speakers) {
		speakers = Mem_Alloc(sizeof(*speakers) * MAX_SPEAKERS);
		for (int i = 0; i < MAX_SPEAKERS; i++)
			speakers[i].dec.codec = -1;
	}
	return &speakers[slot & (MAX_SPEAKERS - 1)];
}

void CL_ParseVoice(msg_t *m)
{
	int slot = MSG_ReadByte(m), codec = MSG_ReadByte(m), gen = MSG_ReadByte(m);
	uint32_t seq = MSG_ReadVarUInt(m);
	int frames = MSG_ReadByte(m);
	speaker_t *s = speaker(slot);
	bool play = !m->badread && cl_voip_play->value > 0 && !s->muted && !CL_DemoMultiview();
	if (play && (s->dec.codec != codec || s->gen != gen)) {
		play = coder_init(&s->dec, codec, false);
		s->gen = (uint8_t)gen;
		s->nextseq = seq;
	}
	for (int i = 0; i < frames && !m->badread; i++) {
		int len = MSG_ReadShort(m) & 0xffff;
		if (m->readpos + (size_t)len > m->cursize) {
			m->badread = true;
			return;
		}
		const uint8_t *data = m->data + m->readpos;
		m->readpos += (size_t)len;
		if (!play)
			continue;
		int16_t pcm[MAX_FRAME];
		int stream = 1 + slot % (S_MAX_STREAMS - 1), rate = codec_rate(codec);
		uint32_t at = seq + (uint32_t)i;
		if (at < s->nextseq)
			continue; /* already played */
		/* fill a short gap from lost packets; a long one just restarts */
		for (uint32_t lost = s->nextseq; at - lost <= 3 && lost < at; lost++)
			play_frame(stream, pcm, decode_frame(&s->dec, NULL, 0, pcm), rate, cl_voip_play->value);
		play_frame(stream, pcm, decode_frame(&s->dec, data, len, pcm), rate, cl_voip_play->value);
		s->nextseq = at + 1;
		s->lastheard = cl.realtime;
	}
}

/* the voipspeaking, voiploudness and vignored player keys; NULL for other keys */
const char *CL_VoipPlayerKey(int slot, const char *key, char *out, size_t size)
{
	if (slot < 0 || slot >= MAX_SPEAKERS)
		return NULL;
	if (!strcmp(key, "voipspeaking")) {
		bool on = slot == cl.playerslot ? tx.sending : speakers && cl.realtime - speaker(slot)->lastheard < 0.25;
		snprintf(out, size, "%d", on ? 1 : 0);
	} else if (!strcmp(key, "voiploudness"))
		snprintf(out, size, "%g", slot == cl.playerslot ? tx.level : 0.0f);
	else if (!strcmp(key, "vignored"))
		snprintf(out, size, "%d", speakers && speaker(slot)->muted ? 1 : 0);
	else
		return NULL;
	return out;
}

/* the meter's level for the renderer, or -1 for no meter */
float CL_VoipMeter(void)
{
	int show = cl_voip_showmeter->ivalue;
	if (!show || !tx.micopen)
		return -1;
	if (show == 1 && !tx.sending)
		return -1;
	return tx.level;
}

void CL_VoipFrame(void)
{
	capture_frame();
	float duck = 1;
	if (speakers)
		for (int i = 0; i < MAX_SPEAKERS; i++)
			if (i != cl.playerslot && speakers[i].lastheard && cl.realtime - speakers[i].lastheard < 0.25) {
				duck = BW_MIN(duck, cl_voip_ducking->value);
				break;
			}
	if (tx.sending)
		duck = BW_MIN(duck, cl_voip_capturingvol->value);
	S_SetDucking(duck);
}

void CL_VoipDisconnected(void)
{
	tx.key = false;
	tx.sending = false;
	flush_packet();
	if (speakers)
		for (int i = 0; i < MAX_SPEAKERS; i++) {
			coder_free(&speakers[i].dec, false);
			speakers[i].lastheard = 0;
			speakers[i].muted = false;
		}
	for (int i = 1; i < S_MAX_STREAMS; i++)
		S_StreamClear(i);
}

/* --- commands ------------------------------------------------------------------------- */

static void VoipDown_f(int argc, const char **argv) { tx.key = true; }
static void VoipUp_f(int argc, const char **argv) { tx.key = false; }

void CL_VoipPrintMutes(void)
{
	char name[64];
	int n = 0;
	Con_Printf("User Mute List: ");
	for (int i = 0; speakers && i < cl.maxclients && i < MAX_SPEAKERS; i++)
		if (speakers[i].muted) {
			CL_PlayerKey(i, "name", name, sizeof(name));
			Con_Printf("%s\"%s\"", n++ ? ", " : "", name[0] ? name : va("#%d", i + 1));
		}
	Con_Printf("%s\n\n", n ? "" : "empty");
}

/* a player by name or number, as ignore takes them; -1 when nobody matches */
static int find_player(const char *who)
{
	char name[64];
	for (int i = 0; i < cl.maxclients; i++)
		if (CL_PlayerKey(i, "name", name, sizeof(name))[0] && !Str_ICmp(name, who))
			return i;
	int n = atoi(who);
	if (n > 0 && n <= cl.maxclients && CL_PlayerKey(n - 1, "name", name, sizeof(name))[0])
		return n - 1;
	return -1;
}

static void set_mute(int slot, bool muted)
{
	speaker(slot)->muted = muted;
	if (cl.state >= ca_loading)
		CL_SendStringCmd(va("vignore %d %d", slot, muted ? 1 : 0));
}

static void Mute_f(int argc, const char **argv)
{
	bool mute = !Str_ICmp(argv[0], "cl_voip_mute");
	if (argc < 2) {
		/* as ignore does: the lists */
		Cmd_ExecuteString("ignorelist");
		return;
	}
	for (int i = 1; i < argc; i++) {
		int slot = find_player(argv[i]);
		if (slot < 0) {
			Con_Printf("no player %s\n", argv[i]);
			continue;
		}
		set_mute(slot, mute);
		Con_Printf("%s %s\n", mute ? "muted" : "unmuted", argv[i]);
	}
}

static void Voip_f(int argc, const char **argv)
{
	const char *p = argc > 1 ? argv[1] : "";
	if (!strcmp(p, "unmuteall")) {
		for (int i = 0; speakers && i < MAX_SPEAKERS; i++)
			if (speakers[i].muted)
				set_mute(i, false);
	} else if (!strcmp(p, "maxgain")) {
		if (argc > 2)
			tx.maxgain = BW_MAX((float)atof(argv[2]), 1.0f);
		Con_Printf("autogain goes up to %g times\n", tx.maxgain);
	} else if (!strcmp(p, "codecs")) {
		for (int i = 0; i <= VOICE_PCMU; i++) {
			const char *why;
			Con_Printf("%d: %s%s\n", i, codec_names[i], codec_usable(i, &why) ? "" : " (not installed)");
		}
	} else if (!strcmp(p, "devices")) {
		char names[16][128];
		int n = S_MicDevices(names, 16);
		if (!n)
			Con_Printf("^&C0No microphone detected\n");
		for (int i = 0; i < n; i++)
			Con_Printf("%s\n", names[i]);
	} else
		Con_Printf("unrecognised parameter \"%s\"\n", p);
}

void CL_InitVoip(void)
{
	cl_voip_send = Cvar_Get("cl_voip_send", "0", 0,
	                        "Sends voice-over-ip data to the server whenever it is set.\n0: only send voice if +voip is pressed.\n"
	                        "1: voice activation.\n2: constantly send.\n+4: Do not send to game, only to rtp sessions.");
	cl_voip_codec = Cvar_Get("cl_voip_codec", "", CVAR_ARCHIVE,
	                         "0: speex(@11khz). 1: raw. 2: opus. 3: speex(@8khz). 4: speex(@16). 5:speex(@32). 6: pcma. 7: pcmu.");
	cl_voip_bitrate = Cvar_Get("cl_voip_bitrate", "3000", CVAR_ARCHIVE,
	                           "For codecs with non-specific bitrates, this specifies the target bitrate to use.");
	cl_voip_autogain = Cvar_Get("cl_voip_autogain", "0", CVAR_ARCHIVE,
	                            "Attempts to normalize your voice levels to a standard level.");
	cl_voip_noisefilter = Cvar_Get("cl_voip_noisefilter", "1", CVAR_ARCHIVE, "Enable the use of the noise cancelation filter.");
	cl_voip_micamp = Cvar_Get("cl_voip_micamp", "2", CVAR_ARCHIVE, "Amplifies your microphone when using voip.");
	cl_voip_ducking = Cvar_Get("cl_voip_ducking", "0.5", CVAR_ARCHIVE,
	                           "Scales game audio by this much when someone is talking to you.");
	cl_voip_play = Cvar_Get("cl_voip_play", "1", CVAR_ARCHIVE, "Enables voip playback. Value is a volume scaler.");
	cl_voip_showmeter = Cvar_Get("cl_voip_showmeter", "1", CVAR_ARCHIVE,
	                             "Shows your speech volume above the standard hud. 0=hide, 1=show when transmitting, 2=ignore "
	                             "voice-activation disable");
	cl_voip_capturingvol = Cvar_Get("cl_voip_capturingvol", "0.5", CVAR_ARCHIVE,
	                                "Volume multiplier applied while capturing, to avoid your audio from being heard by others.");
	cl_voip_vad_delay = Cvar_Get("cl_voip_vad_delay", "0.3", CVAR_ARCHIVE,
	                             "Keeps sending voice data for this many seconds after voice activation would normally stop");
	cl_voip_vad_threshhold = Cvar_Get("cl_voip_vad_threshhold", "15", CVAR_ARCHIVE,
	                                  "This is the threshhold for voice-activation-detection when sending voip data");
	cl_voip_test = Cvar_Get("cl_voip_test", "0", 0, "If 1, enables you to hear your own voice directly, bypassing the server.");
	cl_voip_capturedevice = Cvar_Get("cl_voip_capturedevice", "", CVAR_ARCHIVE, "The microphone to use, empty for the default");
	cl_voip_capturedevice_opts = Cvar_Get("_cl_voip_capturedevice_opts", "", 0,
	                                      "The possible audio capture devices, in \"value\" \"description\" pairs, for gamecode to read.");
	{
		char names[16][128];
		strbuf_t sb = {0};
		int n = S_MicDevices(names, 16);
		SB_AppendStr(&sb, "\"\" \"Default\"");
		for (int i = 0; i < n; i++)
			SB_Printf(&sb, " \"%s\" \"%s\"", names[i], names[i]);
		Cvar_Set(cl_voip_capturedevice_opts->name, sb.data ? sb.data : "");
		SB_Free(&sb);
	}
	Cmd_AddD("+voip", VoipDown_f, "Talk while held");
	Cmd_AddD("-voip", VoipUp_f, "Stop talking");
	Cmd_AddD("cl_voip_mute", Mute_f, "cl_voip_mute <player...>: stop hearing a player's voice");
	Cmd_AddD("cl_voip_unmute", Mute_f, "cl_voip_unmute <player...>: hear a player's voice again");
	Cmd_AddD("voip", Voip_f, "voip unmuteall|maxgain [n]|codecs|devices");
}
