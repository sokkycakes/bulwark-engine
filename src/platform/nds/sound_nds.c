/* Bulwark engine - the DS link client's sound (sound_nds.h). The PC's model, cut to the DS:
 *
 * - voices: the PC's 96 channels (src/sound/snd_main.c: 128 less the 32 ambientsound ones,
 *   :16-17), started, replaced and stolen by its rules (pick_channel :155-182), spatialised
 *   by its formulas (spatialise :104-137, S_DistanceGain :140-153) in fixed point
 *   (sndmath_nds.h), every frame;
 * - the hardware's 16 channels go to the loudest voices: a loop that loses its channel goes
 *   on silently and starts again from its loop point when it gets one back, a one-shot that
 *   loses it ends, and a one-shot quieter than every busy channel isn't started;
 * - samples: from packs (<base>/sound/NAME.dsp, the format of docs/platforms/nds.md "Sound
 *   packs"), looked up by key one 512-byte directory page at a time, cached whole in one arena
 *   (sndcache= KB), read in slices within sndreadms= a frame, and played while they load.
 *
 * Every memory block but the code and a few KB of state is taken after a map has loaded and
 * given back before the next loads (Snd_MapUnload / Snd_MapLoaded). ISC licence, see LICENSE. */
#include <dirent.h>
#include <stdarg.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <nds.h>
#include <nds/debug.h>

#include "sndmath_nds.h"
#include "sound_nds.h"

void *sbrk(ptrdiff_t);

#define S_VOICES 96     /* snd_main.c:16-17: MAX_CHANNELS 128 less NUM_AMBIENT 32 */
#define S_HW 16
#define S_KEYS 512      /* keys resolved (or missed) this map, least recently used replaced */
#define S_KHASH 256
#define S_BLOCKS 192    /* samples in the cache */
#define S_PACKS 8
#define S_PAGE 512      /* a directory page: 21 entries of 24 bytes */
#define S_PAGEENTS 21
#define S_SLICE 4096    /* one read of sample data */
#define S_START_BYTES 2048 /* a voice starts once this much of its sample is in (or all of it) */
#define S_AMB 64
#define S_SENT 4
#define S_SENTWORDS 32
#define S_PRECACHE 512
#define S_KEPT 16       /* PRECACHE's names the device keeps (first 0xf000), after the map's list */
#define S_SHADERS 32
#define S_CMDS 20       /* sound commands a frame (calico's 32 credits) */
#define S_LATE_MS 150   /* a one-shot not started this long after it came is dropped */

#define CHAN_VOICE 2
#define CHAN_BODY 4
#define CHAN_LOOP 5
/* the PC's flags byte (fteextensions.qc SOUNDFLAG_*, snd_main.c:19-24) */
#define SF_FORCELOOP 2
#define SF_NOSPATIALISE 4
#define SF_ABSVOLUME 16
#define SF_FOLLOW 64
#define SF_NOREPLACE 128
/* SOUND records' lflags (docs/spec/link.md) */
#define LF_LISTENER 1
#define LF_AT 2
#define LF_STOP 4
#define LF_CLIENTSIDE 8
#define LF_KNOWN 0x0f /* the bits a record may have: the rest are dropped */
#define VIEW_ENT 0xffff /* the view model: follows the camera */

enum { K_FREE, K_PENDING, K_FOUND, K_MISS };
enum { V_FREE, V_DELAY, V_WAIT, V_PLAY };
enum { SRC_SND, SRC_LOOP, SRC_AMB, SRC_SENT, SRC_LOCAL, SRC_VM, SRC_COUNT };
static const char *const srcname[SRC_COUNT] = {"snd", "loop", "amb", "sent", "local", "vm"};
enum { FMT_PCM8, FMT_PCM16, FMT_ADPCM, FMT_STREAM };

typedef struct {
	FILE *f;
	char name[48];
	uint32_t count, dir_off, index_count;
	uint32_t *index; /* the first key of each directory page */
} spack_t;

typedef struct {
	uint32_t key;
	uint8_t state, pack, fmt, flags;
	uint16_t rate;
	int8_t trypack; /* K_PENDING: the next pack to look in */
	uint32_t frames, bytes, offset;
	int32_t loopstart;
	int16_t block;
	uint32_t lastuse;
} skey_t;

typedef struct {
	uint32_t off, size; /* in the arena */
	int16_t ki;
	uint32_t loaded;    /* data bytes in */
	uint32_t datapos;   /* where the data starts in the pack, 0 before the header is read */
	uint32_t lastuse;
	uint8_t pins;
} sblock_t;

typedef struct {
	uint8_t src, lflags, sflags, chan, vol, atten, pitch;
	uint16_t ent, seq, at;
	uint32_t key;
	int16_t ki;
	int8_t hw;
	bool loop, underrun;
	bool ownclock;       /* V_DELAY until t_due (ticks), not the drawn clock: a SoundDef's start delay */
	int8_t amb;          /* its AMBIENT entry, -1 none */
	uint8_t thing;       /* where its entity was in the things list last frame (a hint) */
	int32_t org[3];      /* 1/8 units */
	uint32_t vol_q16, f_q8;
	sndgain_t g;
	int32_t loud;
	uint32_t t_arrive, t_start, t_due;
	uint32_t skip; /* the loader passed it over this frame (S.use) */
} svoice_t;

typedef struct {
	int16_t v, blk;
	uint16_t vol, timer;
	uint8_t pan;
	bool draining;
	uint32_t t_start, t_end, t_stop; /* t_end 0: loops */
} shw_t;

typedef struct {
	uint16_t ent;
	uint32_t key;
	uint8_t vol, atten, pitch, flags;
	int16_t org[3];
	int8_t voice;
	uint32_t pendkey; /* ambient_generic: a new sample, switched to once it is in */
} samb_t;

typedef struct {
	bool on;
	uint16_t ent;
	uint8_t atten, n, next;
	uint32_t keys[S_SENTWORDS];
	uint32_t t_next;
	bool waiting; /* the next word is due; it starts once it is in */
} ssent_t;

typedef struct {
	uint8_t nsamp, vol, pmin, pmax, atten, flags, chan, dist;
	int16_t offset_ms;
	uint32_t keys[8];
} sshader_t;

sndstats_t Snd_Stats;

/* what sound keeps on the heap from a map's load to the next (with the voices, keys and cache) */
static struct sheap_s {
	sblock_t blk[S_BLOCKS];
	samb_t amb[S_AMB];
	ssent_t sent[S_SENT];
	sshader_t shader[S_SHADERS];
	struct {
		int8_t pack;
		uint32_t page;
		uint8_t d[S_PAGE];
	} pagecache[2];
	uint32_t f_q8[256]; /* snd_factor_q8 by attenuation byte */
	samb_t ambnew[S_AMB]; /* handle_ambient's new set (off the stack: DTCM is small) */
	bool ambstart[S_AMB];
	int16_t khead[S_KHASH], knext[S_KEYS];
} *H;

static struct {
	bool on, inited, ready;
	uint8_t log;
	uint32_t master_q16, bgm_q16;
	int cachekb, readms;
	/* after a map loads */
	uint8_t *arena;
	uint32_t arenasize;
	svoice_t *v;
	skey_t *k;
	/* the precache list: pkey by the server's index (S_PRECACHE on: the kept names), each
	 * looked up once (plook goes through them), and pord, what was found, smallest first
	 * (u64: bytes << 16 | index), which the preload reads from pnext on */
	uint32_t *pkey;
	uint64_t *pord;
	int npk, plook, npord, pnext;
	uint32_t preload_bytes;
	int nblk, nkeys, khand;
	uint8_t vst[S_VOICES]; /* each voice's V_ state, apart: the frame's passes read only these for free ones */
	int vtop;              /* every voice from here on is free: the passes stop here */
	spack_t pack[S_PACKS];
	int npacks;
	int pagenext;
	shw_t hw[S_HW];
	uint32_t use; /* the LRU clock: frames */
	uint32_t lastframe, frameint; /* ticks: the last Snd_Frame, and the time since the one before */
	uint32_t readest;             /* ticks a read takes: the longest lately (decays each frame) */
	/* the listener */
	int32_t lorg[3], lright[3];
	bool havelistener;
	int selfent;
	const sndthing_t *things;
	int nthings;
	uint32_t clock;
	bool haveclock;
	/* SOUND's seq window */
	bool haveseq;
	uint16_t seqtop;
	uint64_t seqbits;
	/* AMBIENT */
	int namb;
	uint16_t ambver;
	int nshader;
	uint32_t wsel_until; /* HLWeaponSelect m_flHUDWeaponSelectTime, ticks */
	int cmdrr;           /* volume updates go round */
	uint32_t laststat;
	uint32_t statframes, statcpu, statcpumax, statreadmax, statcmdmax, statactive, sec[5], statreadsum;
	uint32_t rnd;
} S;

/* --- small things -------------------------------------------------------------------------- */

static inline uint32_t ticks(void) { return (uint32_t)tickGetCount(); }
static inline uint32_t ms_ticks(uint32_t ms) { return (uint32_t)((uint64_t)ms * TICK_FREQ / 1000); }
static inline uint32_t ticks_ms(uint32_t t) { return (uint32_t)((uint64_t)t * 1000 / TICK_FREQ); }
static uint32_t rd16(const uint8_t *d) { return d[0] | d[1] << 8; }
static uint32_t rd32(const uint8_t *d) { return rd16(d) | rd16(d + 2) << 16; }
static uint32_t rnd(void) { return S.rnd = S.rnd * 1103515245u + 12345u, S.rnd >> 16; }

uint32_t Snd_Key(const char *name) { return snd_key(name); }
bool Snd_Enabled(void) { return S.on && S.ready; } /* (INPUT's caps: sound is wanted only while it can play) */

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	static char line[384]; /* not on the stack: that is DTCM, which melonDS's debug port doesn't read */
	va_list ap;
	if (!S.log)
		return;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	nocashMessage(line);
}

/* the sound commands, timed: calico blocks when the ARM7's queue is full */
static uint32_t cmd_t0;
static int cmds;
static void cmd_begin(void) { cmd_t0 = ticks(); }
static void cmd_end(void)
{
	uint32_t t = ticks() - cmd_t0;
	cmds++;
	if (t > S.statcmdmax)
		S.statcmdmax = t;
}

static void defaults(void);

bool Snd_Config(const char *key, const char *val)
{
	defaults(); /* the settings read before Snd_Init */
	if (!strcmp(key, "volume"))
		S.master_q16 = (uint32_t)(BW_NDS_CLAMP(strtof(val, NULL), 0.0f, 1.0f) * 65536);
	else if (!strcmp(key, "bgmvolume"))
		S.bgm_q16 = (uint32_t)(BW_NDS_CLAMP(strtof(val, NULL), 0.0f, 1.0f) * 65536);
	else if (!strcmp(key, "sndcache"))
		S.cachekb = BW_NDS_CLAMP(atoi(val), 64, 512);
	else if (!strcmp(key, "sndreadms"))
		S.readms = BW_NDS_CLAMP(atoi(val), 1, 16);
	else if (!strcmp(key, "snd_log"))
		S.log = (uint8_t)BW_NDS_CLAMP(atoi(val), 0, 2); /* 1: each sound and the stats, 2: the stats */
	else if (!strcmp(key, "sound"))
		S.on = atoi(val) != 0;
	else
		return false;
	return true;
}

/* --- packs and keys ------------------------------------------------------------------------- */

static int cmp_name(const void *a, const void *b) { return strcasecmp(((const spack_t *)a)->name, ((const spack_t *)b)->name); }

static void open_packs(void)
{
	spack_t found[S_PACKS];
	int n = 0;
	char path[300];
	for (int b = 0; b < NDS_NUMBASEDIRS && n < S_PACKS; b++) {
		DIR *d;
		struct dirent *e;
		snprintf(path, sizeof(path), "%ssound", nds_basedirs[b]);
		if (!(d = opendir(path)))
			continue;
		while ((e = readdir(d)) && n < S_PACKS) {
			size_t l = strlen(e->d_name);
			if (l < 5 || l >= sizeof(found[0].name) || strcasecmp(e->d_name + l - 4, ".dsp"))
				continue;
			memset(&found[n], 0, sizeof(found[n]));
			snprintf(found[n].name, sizeof(found[n].name), "%s", e->d_name);
			/* the folder goes in f's place for now: the base dir index */
			found[n].count = (uint32_t)b;
			n++;
		}
		closedir(d);
	}
	qsort(found, (size_t)n, sizeof(found[0]), cmp_name);
	S.npacks = 0;
	for (int i = 0; i < n; i++) {
		spack_t *p = &S.pack[S.npacks];
		uint8_t h[32];
		FILE *f;
		snprintf(path, sizeof(path), "%ssound/%s", nds_basedirs[found[i].count], found[i].name);
		if (!(f = fopen(path, "rb")))
			continue;
		setvbuf(f, NULL, _IONBF, 0); /* our reads are whole pages and slices */
		if (fread(h, 1, 32, f) != 32 || memcmp(h, "BWSP", 4) || rd16(h + 4) != 1) {
			fclose(f);
			continue;
		}
		memset(p, 0, sizeof(*p));
		snprintf(p->name, sizeof(p->name), "%s", found[i].name);
		p->f = f;
		p->count = rd32(h + 8), p->dir_off = rd32(h + 12), p->index_count = rd32(h + 20);
		if (p->index_count != (p->count + S_PAGEENTS - 1) / S_PAGEENTS || !(p->index = malloc(p->index_count * 4 + 4)) ||
		    fseek(f, (long)rd32(h + 16), SEEK_SET) || fread(p->index, 4, p->index_count, f) != p->index_count) {
			free(p->index);
			fclose(f);
			continue;
		}
		S.npacks++;
		logf_("SNDPACK %s keys=%u\n", path, (unsigned)p->count);
	}
}

static void close_packs(void)
{
	for (int i = 0; i < S.npacks; i++) {
		if (S.pack[i].f)
			fclose(S.pack[i].f);
		free(S.pack[i].index);
	}
	memset(S.pack, 0, sizeof(S.pack));
	S.npacks = 0;
}

/* one timed read; false on failure */
static bool pread_(spack_t *p, uint32_t pos, void *buf, uint32_t n)
{
	uint32_t t0 = ticks(), t;
	bool ok = !fseek(p->f, (long)pos, SEEK_SET) && fread(buf, 1, n, p->f) == n;
	t = ticks() - t0;
	if (t > S.readest)
		S.readest = t;
	Snd_Stats.reads++, Snd_Stats.readbytes += n;
	S.statreadsum += t;
	if (t > S.statreadmax)
		S.statreadmax = t;
	if (t > Snd_Stats.read_ticks_max)
		Snd_Stats.read_ticks_max = t;
	return ok;
}

/* the key table is hashed (a scan of all 512 is ~18 KB of memory, past the 4 KB data cache) */
#define KHASH(key) ((key) ^ (key) >> 9 ^ (key) >> 18) & (S_KHASH - 1)
static int key_find(uint32_t key)
{
	for (int i = H->khead[KHASH(key)]; i >= 0; i = H->knext[i])
		if (S.k[i].key == key)
			return i;
	return -1;
}

static void key_unlink(int ki)
{
	int16_t *pp = &H->khead[KHASH(S.k[ki].key)];
	while (*pp >= 0 && *pp != ki)
		pp = &H->knext[*pp];
	if (*pp == ki)
		*pp = H->knext[ki];
}

static bool key_busy(int ki)
{
	if (S.k[ki].block >= 0 && H->blk[S.k[ki].block].pins)
		return true;
	for (int i = 0; i < S.vtop; i++)
		if (S.vst[i] != V_FREE && S.v[i].ki == ki)
			return true;
	return false;
}

static void block_free(int b);

/* the key's entry, made (pending a lookup) if it has none; -1 when the table is full of busy ones */
static int key_get(uint32_t key)
{
	int ki = key_find(key), old = -1;
	if (ki >= 0) {
		S.k[ki].lastuse = S.use;
		return ki;
	}
	if (S.nkeys < S_KEYS)
		old = S.nkeys++; /* the table fills in order; then the least recently used of the next 16 goes
		                  * (a clock hand: scanning all 512 costs ~0.4 ms in melonDS) */
	else
		for (int n = 0; n < S_KEYS && (n < 16 || old < 0); n++) {
			int i = S.khand;
			S.khand = (S.khand + 1) % S_KEYS;
			if ((old < 0 || S.k[i].lastuse < S.k[old].lastuse) && !key_busy(i))
				old = i;
		}
	if (old < 0)
		return -1;
	if (S.k[old].state != K_FREE) {
		if (S.k[old].block >= 0)
			block_free(S.k[old].block);
		key_unlink(old);
	}
	memset(&S.k[old], 0, sizeof(S.k[old]));
	H->knext[old] = H->khead[KHASH(key)];
	H->khead[KHASH(key)] = (int16_t)old;
	S.k[old].key = key;
	S.k[old].state = S.npacks ? K_PENDING : K_MISS;
	S.k[old].trypack = (int8_t)(S.npacks - 1);
	S.k[old].block = -1;
	S.k[old].lastuse = S.use;
	return old;
}

/* a key in a directory page of nent entries: fills k and returns true */
static bool page_search(const uint8_t *d, int nent, skey_t *k, int pack)
{
	int lo = 0, hi = nent - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		const uint8_t *e = d + mid * 24;
		uint32_t mk = rd32(e);
		if (mk == k->key) {
			k->pack = (uint8_t)pack;
			k->offset = rd32(e + 4), k->fmt = e[8], k->flags = e[9], k->rate = (uint16_t)rd16(e + 10);
			k->frames = rd32(e + 12), k->loopstart = (int32_t)rd32(e + 16), k->bytes = rd32(e + 20);
			k->state = K_FOUND;
			return true;
		}
		if (mk < k->key)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

/* a lookup, the pack whose name sorts last first, until it reads a page (true) or ends
 * without a read (found in a cached page, or in no pack's page range) */
static bool key_lookup_step(int ki)
{
	skey_t *k = &S.k[ki];
	while (k->state == K_PENDING) {
		spack_t *p;
		int lo = 0, hi, page, nent, c;
		uint8_t *d = NULL;
		bool read = false;
		if (k->trypack < 0) {
			k->state = K_MISS;
			return false;
		}
		p = &S.pack[k->trypack];
		/* the last page whose first key is <= key */
		hi = (int)p->index_count - 1;
		if (hi < 0 || k->key < p->index[0]) {
			k->trypack--;
			continue;
		}
		while (lo < hi) {
			int mid = (lo + hi + 1) / 2;
			if (p->index[mid] <= k->key)
				lo = mid;
			else
				hi = mid - 1;
		}
		page = lo;
		nent = (int)BW_NDS_MIN((uint32_t)S_PAGEENTS, p->count - (uint32_t)page * S_PAGEENTS);
		for (c = 0; c < 2; c++)
			if (H->pagecache[c].pack == k->trypack && H->pagecache[c].page == (uint32_t)page)
				d = H->pagecache[c].d;
		if (!d) {
			c = S.pagenext, S.pagenext ^= 1;
			H->pagecache[c].pack = -1;
			read = true;
			if (pread_(p, p->dir_off + (uint32_t)page * S_PAGE, H->pagecache[c].d, (uint32_t)nent * 24)) {
				H->pagecache[c].pack = k->trypack, H->pagecache[c].page = (uint32_t)page;
				d = H->pagecache[c].d;
			}
		}
		if (!d || !page_search(d, nent, k, k->trypack)) {
			k->trypack--;
			if (k->trypack < 0)
				k->state = K_MISS;
		}
		if (read)
			return true;
	}
	return false;
}

/* --- the cache ------------------------------------------------------------------------------ */

static void block_free(int b)
{
	if (b < 0 || b >= S.nblk)
		return;
	if (H->blk[b].ki >= 0)
		S.k[H->blk[b].ki].block = -1;
	memmove(&H->blk[b], &H->blk[b + 1], sizeof(H->blk[0]) * (size_t)(S.nblk - b - 1));
	S.nblk--;
	/* the indexes after it moved down */
	for (int i = 0; i < S.nblk; i++)
		if (H->blk[i].ki >= 0)
			S.k[H->blk[i].ki].block = (int16_t)i;
	for (int h = 0; h < S_HW; h++)
		if (S.hw[h].blk > b)
			S.hw[h].blk--;
		else if (S.hw[h].blk == b)
			S.hw[h].blk = -1; /* (never: a channel's block is pinned) */
}

static uint32_t pinned_bytes(void)
{
	uint32_t n = 0;
	for (int i = 0; i < S.nblk; i++)
		if (H->blk[i].pins)
			n += H->blk[i].size;
	return n;
}

static uint32_t used_bytes(void)
{
	uint32_t n = 0;
	for (int i = 0; i < S.nblk; i++)
		n += H->blk[i].size;
	return n;
}

static int block_insert(uint32_t off, uint32_t size, int ki)
{
	int i = 0;
	if (S.nblk >= S_BLOCKS)
		return -1;
	while (i < S.nblk && H->blk[i].off < off)
		i++;
	memmove(&H->blk[i + 1], &H->blk[i], sizeof(H->blk[0]) * (size_t)(S.nblk - i));
	S.nblk++;
	memset(&H->blk[i], 0, sizeof(H->blk[i]));
	H->blk[i].off = off, H->blk[i].size = size, H->blk[i].ki = (int16_t)ki, H->blk[i].lastuse = S.use;
	for (int j = 0; j < S.nblk; j++)
		if (H->blk[j].ki >= 0)
			S.k[H->blk[j].ki].block = (int16_t)j;
	for (int h = 0; h < S_HW; h++)
		if (S.hw[h].blk >= i)
			S.hw[h].blk++;
	return i;
}

/* room for size bytes: a gap, or else the contiguous run of unpinned blocks whose newest use is
 * oldest (evict = false: gaps only, for preloads). Returns the block, -1 if nothing fits */
static int block_alloc(uint32_t size, int ki, bool evict)
{
	uint32_t prev = 0;
	int best = -1, bestj = -1;
	uint32_t bestscore = 0;
	size = (size + 31) & ~31u;
	if (size > S.arenasize)
		return -1;
	for (int i = 0; i <= S.nblk; i++) {
		uint32_t next = i < S.nblk ? H->blk[i].off : S.arenasize;
		if (next - prev >= size && S.nblk < S_BLOCKS)
			return block_insert(prev, size, ki);
		if (i < S.nblk)
			prev = H->blk[i].off + H->blk[i].size;
	}
	if (!evict)
		return -1;
	for (int i = 0; i < S.nblk; i++) {
		uint32_t start = i ? H->blk[i - 1].off + H->blk[i - 1].size : 0, newest = 0;
		for (int j = i; j < S.nblk && !H->blk[j].pins; j++) {
			uint32_t end = j + 1 < S.nblk ? H->blk[j + 1].off : S.arenasize;
			newest = BW_NDS_MAX(newest, H->blk[j].lastuse);
			if (end - start >= size) {
				if (best < 0 || newest < bestscore || (newest == bestscore && j - i < bestj - best))
					best = i, bestj = j, bestscore = newest;
				break;
			}
		}
	}
	if (best < 0)
		return -1;
	{
		uint32_t start = best ? H->blk[best - 1].off + H->blk[best - 1].size : 0;
		for (int j = bestj; j >= best; j--)
			block_free(j);
		return block_insert(start, size, ki);
	}
}

static bool key_ready(int ki)
{
	const skey_t *k = &S.k[ki];
	return k->state == K_FOUND && k->block >= 0 && H->blk[k->block].datapos &&
	       H->blk[k->block].loaded >= BW_NDS_MIN(k->bytes, (uint32_t)S_START_BYTES);
}

static bool key_complete(int ki)
{
	const skey_t *k = &S.k[ki];
	return k->state == K_FOUND && k->block >= 0 && H->blk[k->block].datapos && H->blk[k->block].loaded >= k->bytes;
}

/* one read for this key: a lookup, the header and first slice, or the next slice. False when
 * there is nothing to read (in, missing, too big, or no room) */
static bool load_step(int ki, bool evict, bool loop)
{
	skey_t *k = &S.k[ki];
	sblock_t *b;
	spack_t *p;
	if (k->state == K_PENDING)
		return key_lookup_step(ki) || k->state == K_FOUND; /* found in a cached page: go on (no read) */
	if (k->state != K_FOUND)
		return false;
	if (k->fmt == FMT_STREAM || k->bytes > S.arenasize / 2)
		return false; /* streams: not yet */
	if (k->block < 0) {
		int nb;
		if (loop && pinned_bytes() + k->bytes > S.arenasize / 2)
			return false;
		if ((nb = block_alloc(k->bytes, ki, evict)) < 0)
			return false;
		(void)nb;
	}
	b = &H->blk[k->block];
	p = &S.pack[k->pack];
	if (!b->datapos) {
		/* the sample's header: u32 key, u8 fmt, u8 flags, u16 rate, u32 frames, s32 loopstart,
		 * u32 bytes, u8 namelen, name, padded to 4, then the data */
		uint8_t h[24 + 256];
		uint32_t n = BW_NDS_MIN((uint32_t)sizeof(h), 21u + 255u + k->bytes), namelen, dp;
		if (!pread_(p, k->offset, h, BW_NDS_MIN(n, 21u)) || rd32(h) != k->key) {
			block_free(k->block);
			k->state = K_MISS;
			return true;
		}
		namelen = h[20];
		dp = k->offset + ((21 + namelen + 3) & ~3u);
		if (S.log == 1 && namelen && pread_(p, k->offset + 21, h + 21, namelen)) {
			h[21 + namelen] = 0;
			logf_("SNDLOAD key=%08x %s fmt=%d rate=%d frames=%u bytes=%u loop=%d\n", (unsigned)k->key, (char *)h + 21, k->fmt,
			      k->rate, (unsigned)k->frames, (unsigned)k->bytes, (int)k->loopstart);
		}
		b->datapos = dp;
	}
	if (b->loaded < k->bytes) {
		uint32_t n = BW_NDS_MIN((uint32_t)S_SLICE, k->bytes - b->loaded);
		uint8_t *dst = S.arena + b->off + b->loaded;
		if (!pread_(p, b->datapos + b->loaded, dst, n)) {
			if (!b->pins)
				block_free(k->block);
			k->state = K_MISS;
			return true;
		}
		DC_FlushRange(dst, n);
		b->loaded += n;
		return true;
	}
	return false;
}

/* --- hardware channels ---------------------------------------------------------------------- */

static void hw_release(int h, bool stop)
{
	shw_t *c = &S.hw[h];
	if (c->v >= 0)
		S.v[c->v].hw = -1;
	c->v = -1;
	if (stop) {
		cmd_begin();
		soundStop(1u << h);
		cmd_end();
	}
	/* the block stays pinned until the channel is seen to be off */
	c->draining = true;
	c->t_stop = ticks();
}

static void voice_log(const svoice_t *v, const char *what, int hw, int vol, int pan, uint32_t late)
{
	if (S.log == 1)
		logf_("SND %u %s %s seq=%u key=%08x ent=%u ch=%u hw=%d vol=%d pan=%d late=%u\n", (unsigned)ticks_ms(ticks()), what,
		      srcname[v->src], v->seq, (unsigned)v->key, v->ent, v->chan, hw, vol, pan, (unsigned)late);
}

/* every voice that ends goes through here: its AMBIENT entry lets go of it, so that the entry
 * never holds a voice index another sound has since been given */
static void voice_gone(int vi)
{
	svoice_t *v = &S.v[vi];
	if (v->amb >= 0 && v->amb < S.namb && H->amb[v->amb].voice == vi)
		H->amb[v->amb].voice = -1;
	v->amb = -1;
	S.vst[vi] = V_FREE;
}

static void voice_free(int vi, const char *why)
{
	svoice_t *v = &S.v[vi];
	if (S.vst[v - S.v] == V_FREE)
		return;
	if (why)
		voice_log(v, why, v->hw, 0, 0, 0);
	if (v->hw >= 0)
		hw_release(v->hw, true);
	voice_gone(vi);
}

static int sample_timer(const skey_t *k, int pitch)
{
	uint32_t hz = (uint32_t)k->rate * (uint32_t)(pitch ? pitch : 100) / 100;
	/* the timer is 16 bits: SOUND_CLOCK / 65535 is 256 Hz (below, it wraps to a high pitch) */
	return (int)soundTimerFromHz(hz < 256 ? 256 : hz);
}

static void hw_unpin(int h);

/* program channel h for voice vi from its start */
static void hw_start(int h, int vi)
{
	svoice_t *v = &S.v[vi];
	skey_t *k = &S.k[v->ki];
	sblock_t *b = &H->blk[k->block];
	shw_t *c = &S.hw[h];
	int vol, pan, timer = sample_timer(k, v->pitch);
	unsigned words = k->bytes / 4, pnt = 0, len = words;
	SoundFmt fmt = k->fmt == FMT_PCM8 ? SoundFmt_Pcm8 : k->fmt == FMT_PCM16 ? SoundFmt_Pcm16 : SoundFmt_ImaAdpcm;
	SoundMode mode = SoundMode_OneShot;
	if (v->loop) {
		/* the loop point in words; ADPCM's counts the header word */
		int32_t ls = k->loopstart >= 0 ? k->loopstart : 0;
		pnt = k->fmt == FMT_PCM8 ? (unsigned)ls / 4 : k->fmt == FMT_PCM16 ? (unsigned)ls / 2 : (unsigned)ls / 8 + 1;
		if (pnt >= words)
			pnt = 0;
		len = words - pnt;
		mode = SoundMode_Repeat;
	}
	snd_hwvolpan(v->g, &vol, &pan);
	if (c->blk >= 0)
		hw_unpin(h);
	if (b->pins < 255)
		b->pins++;
	c->blk = k->block, c->v = (int16_t)vi;
	c->vol = (uint16_t)vol, c->pan = (uint8_t)pan, c->timer = (uint16_t)timer;
	c->t_start = ticks();
	c->t_end = v->loop ? 0 : c->t_start + (uint32_t)((uint64_t)k->frames * (uint32_t)timer / 32) + 1;
	v->hw = (int8_t)h;
	cmd_begin();
	soundPreparePcm((unsigned)h | SOUND_START, (unsigned)vol, (unsigned)pan, (unsigned)timer, mode, fmt, S.arena + b->off, pnt,
	                len);
	cmd_end();
	b->lastuse = S.use;
}

static void hw_unpin(int h)
{
	shw_t *c = &S.hw[h];
	if (H && c->blk >= 0 && c->blk < S.nblk && H->blk[c->blk].pins)
		H->blk[c->blk].pins--;
	c->blk = -1;
	c->draining = false;
}

/* a channel for a voice as loud as loud: a free one, or the quietest one held if quieter.
 * -1 if none */
static int hw_pick(int32_t loud)
{
	int q = -1;
	for (int h = 0; h < S_HW; h++) {
		if (S.hw[h].draining)
			continue;
		if (S.hw[h].v < 0)
			return h;
		if (q < 0 || S.v[S.hw[h].v].loud < S.v[S.hw[q].v].loud)
			q = h;
	}
	if (q >= 0 && S.v[S.hw[q].v].loud < loud) {
		int vi = S.hw[q].v;
		svoice_t *v = &S.v[vi];
		Snd_Stats.stolen++;
		voice_log(v, "steal", q, 0, 0, 0);
		if (v->loop) { /* goes on silently; back from its loop point when it gets a channel */
			hw_release(q, true);
		} else {
			hw_release(q, true);
			voice_gone(vi);
		}
		/* free now for the new voice: the stop was queued before the start */
		hw_unpin(q);
		return q;
	}
	return -1;
}

/* --- voices ---------------------------------------------------------------------------------- */

static bool thing_origin(int ent, int32_t *org, uint8_t *hint)
{
	int i = hint ? *hint : 0;
	if (ent == VIEW_ENT || (ent && ent == S.selfent)) {
		memcpy(org, S.lorg, sizeof(S.lorg));
		return true;
	}
	if (i >= S.nthings || S.things[i].ent != ent)
		for (i = 0; i < S.nthings && S.things[i].ent != ent; i++)
			;
	if (i >= S.nthings)
		return false;
	for (int c = 0; c < 3; c++)
		org[c] = S.things[i].org[c] * 8;
	if (hint)
		*hint = (uint8_t)BW_NDS_MIN(i, 255);
	return true;
}

static void voice_gain(svoice_t *v)
{
	int32_t d[3];
	/* (heard straight: where it is doesn't matter) */
	if ((v->sflags & SF_FOLLOW) && v->ent && !(v->sflags & SF_NOSPATIALISE) && !(v->lflags & LF_LISTENER))
		thing_origin(v->ent, v->org, &v->thing);
	for (int c = 0; c < 3; c++)
		d[c] = v->org[c] - S.lorg[c];
	v->g = snd_spatialise(d, S.lright, v->vol_q16, v->f_q8, v->sflags & SF_NOSPATIALISE, v->lflags & LF_LISTENER);
	v->loud = BW_NDS_MAX(v->g.left, v->g.right);
}

static bool voice_isloop(const svoice_t *v)
{
	return v->loop || (v->sflags & SF_FORCELOOP) || v->src == SRC_AMB || v->src == SRC_LOOP;
}

/* how far into its sample a voice is (frames), for stealing as the PC does: by pos */
static uint32_t voice_pos(const svoice_t *v)
{
	const skey_t *k;
	uint32_t f;
	if (S.vst[v - S.v] != V_PLAY || v->ki < 0)
		return 0;
	k = &S.k[v->ki];
	f = SND_DIV64((uint64_t)(ticks() - v->t_start) * 32, sample_timer(k, v->pitch));
	if (v->loop && k->frames) {
		uint32_t ls = k->loopstart >= 0 ? (uint32_t)k->loopstart : 0;
		if (f >= k->frames && k->frames > ls)
			f = ls + (f - ls) % (k->frames - ls);
	}
	return f;
}

/* pick_channel (snd_main.c:155-182) at the moment a sound comes: the entity's numbered channel
 * is replaced (or with NOREPLACE the new one dropped), else a free voice, else the one furthest
 * into its sample that isn't the listener's. -1: dropped */
static int voice_pick(int ent, int chan, bool clientside, int sflags)
{
	int best = -1;
	uint32_t bestpos = 0;
	if (chan != 0)
		for (int i = 0; i < S.vtop; i++) {
			svoice_t *v = &S.v[i];
			if (S.vst[v - S.v] != V_FREE && v->ent == ent && v->chan == chan && !(v->lflags & LF_CLIENTSIDE) == !clientside) {
				if (sflags & SF_NOREPLACE)
					return -1;
				voice_free(i, "replace");
				return i;
			}
		}
	for (int i = 0; i < S_VOICES; i++) {
		svoice_t *v = &S.v[i];
		uint32_t pos;
		if (S.vst[v - S.v] == V_FREE)
			return i;
		if (v->lflags & LF_LISTENER)
			continue;
		pos = voice_pos(v);
		if (best < 0 || pos > bestpos)
			best = i, bestpos = pos;
	}
	if (best >= 0) {
		Snd_Stats.stolen++;
		voice_free(best, "steal");
	}
	return best;
}

static void stop_chan(int ent, int chan, bool clientside)
{
	if (!chan)
		return; /* null.wav on CHAN_AUTO only mixes silence */
	for (int i = 0; i < S.vtop; i++) {
		svoice_t *v = &S.v[i];
		if (S.vst[v - S.v] != V_FREE && v->ent == ent && v->chan == chan && !(v->lflags & LF_CLIENTSIDE) == !clientside)
			voice_free(i, "stop");
	}
}

typedef struct {
	uint16_t seq, ent, at;
	uint8_t lflags, sflags, chan, vol, atten, pitch;
	uint32_t key;
	int16_t org[3];
} srec_t;

/* a sound that came (or the DS makes): a voice waiting for its sample */
static int voice_new(const srec_t *r, int src)
{
	int vi;
	svoice_t *v;
	bool cs = (r->lflags & LF_CLIENTSIDE) != 0;
	if (!S.ready)
		return -1;
	Snd_Stats.recs++;
	if (r->lflags & LF_STOP) {
		stop_chan(r->ent, r->chan, cs);
		return -1;
	}
	if ((vi = voice_pick(r->ent, r->chan, cs, r->sflags)) < 0)
		return -1;
	v = &S.v[vi];
	memset(v, 0, sizeof(*v));
	S.vtop = BW_NDS_MAX(S.vtop, vi + 1);
	S.vst[v - S.v] = (r->lflags & LF_AT) && S.haveclock ? V_DELAY : V_WAIT;
	v->src = (uint8_t)src, v->lflags = r->lflags, v->sflags = r->sflags, v->chan = r->chan;
	v->vol = r->vol, v->atten = r->atten, v->pitch = r->pitch;
	v->ent = r->ent, v->seq = r->seq, v->at = r->at, v->key = r->key;
	v->hw = -1, v->amb = -1;
	v->vol_q16 = (uint32_t)(((uint64_t)r->vol * ((r->sflags & SF_ABSVOLUME) ? SND_ONE : S.master_q16) + 127) / 255);
	v->f_q8 = H->f_q8[r->atten];
	for (int c = 0; c < 3; c++)
		v->org[c] = r->org[c] * 8;
	v->t_arrive = ticks();
	v->ki = (int16_t)key_get(r->key);
	if (v->ki < 0) {
		voice_gone(vi);
		return -1;
	}
	return vi;
}

/* the sample is in (enough of it): onto a channel */
static void voice_start(int vi)
{
	svoice_t *v = &S.v[vi];
	skey_t *k = &S.k[v->ki];
	int h;
	uint32_t late = ticks_ms(ticks() - v->t_arrive);
	v->loop = k->loopstart >= 0 || (v->sflags & SF_FORCELOOP);
	S.vst[v - S.v] = V_PLAY;
	v->t_start = ticks();
	voice_gain(v);
	Snd_Stats.starts++;
	if (!v->loud) {
		if (!v->loop) { /* inaudible from the start (gain under the cull) */
			voice_log(v, "silent", -1, 0, 0, late);
			Snd_Stats.quiet++;
			voice_gone(vi);
		}
		return;
	}
	if ((h = hw_pick(v->loud)) < 0) {
		if (!v->loop) {
			Snd_Stats.quiet++;
			voice_log(v, "quiet", -1, 0, 0, late);
			voice_gone(vi);
		}
		return;
	}
	hw_start(h, vi);
	voice_log(v, "start", h, S.hw[h].vol, S.hw[h].pan, late);
}

/* --- records ---------------------------------------------------------------------------------- */

/* the precache list's found names in size order: one in (it goes before any it is smaller
 * than; the preload starts again there), one out */
static void pord_remove(int idx)
{
	for (int i = 0; i < S.npord; i++)
		if ((int)(S.pord[i] & 0xffff) == idx) {
			memmove(&S.pord[i], &S.pord[i + 1], sizeof(S.pord[0]) * (size_t)(S.npord - i - 1));
			S.npord--;
			if (S.pnext > i)
				S.pnext--;
			return;
		}
}

static void pord_insert(int idx, uint32_t bytes)
{
	uint64_t e = (uint64_t)bytes << 16 | (uint32_t)idx;
	int lo = 0, hi;
	pord_remove(idx);
	if (S.npord >= S_PRECACHE + S_KEPT)
		return;
	for (hi = S.npord; lo < hi;) {
		int mid = (lo + hi) / 2;
		if (S.pord[mid] < e)
			lo = mid + 1;
		else
			hi = mid;
	}
	memmove(&S.pord[lo + 1], &S.pord[lo], sizeof(S.pord[0]) * (size_t)(S.npord - lo));
	S.pord[lo] = e;
	S.npord++;
	S.pnext = BW_NDS_MIN(S.pnext, lo);
}

static void precache_reset(void)
{
	if (S.pkey)
		memset(S.pkey, 0, sizeof(uint32_t) * (S_PRECACHE + S_KEPT));
	S.npk = S.plook = S.npord = S.pnext = 0;
	S.preload_bytes = 0;
}

/* a SOUND record (docs/spec/link.md): u16 seq, u8 lflags, u8 sflags, u16 ent, u8 chan, u32 key,
 * u8 vol, u8 atten, u8 pitch, s16 org[3], then s16 at with lflags AT. base is its length
 * without at (20; 22 from a server that pads) */
static void parse_rec(const uint8_t *d, srec_t *r)
{
	r->seq = (uint16_t)rd16(d), r->lflags = d[2] & LF_KNOWN, r->sflags = d[3], r->ent = (uint16_t)rd16(d + 4), r->chan = d[6];
	r->key = rd32(d + 7), r->vol = d[11], r->atten = d[12], r->pitch = d[13];
	for (int c = 0; c < 3; c++)
		r->org[c] = (int16_t)rd16(d + 14 + c * 2);
	r->at = 0;
}

static bool walk(const uint8_t *d, int len, int count, int base)
{
	int i = 0;
	for (int n = 0; n < count; n++) {
		if (i + base > len)
			return false;
		i += base + ((d[i + 2] & LF_AT) ? 2 : 0);
	}
	return i == len;
}

static bool seq_new(uint16_t seq)
{
	int16_t d;
	if (!S.haveseq) {
		S.haveseq = true, S.seqtop = seq, S.seqbits = 1;
		return true;
	}
	d = (int16_t)(seq - S.seqtop);
	if (d > 0) {
		S.seqbits = d >= 64 ? 1 : S.seqbits << d | 1;
		S.seqtop = seq;
		return true;
	}
	if (d > -64) {
		uint64_t bit = 1ull << -d;
		if (S.seqbits & bit)
			return false;
		S.seqbits |= bit;
		return true;
	}
	/* 64 or more behind: only the last packet's records come again, and the server numbers
	 * records it then drops (a full queue), so this is a repeat already had, or one so late
	 * it would replace a newer sound (the window resets on a map change: stop_all) */
	return false;
}

static void handle_sound(const uint8_t *d, int len)
{
	int count = d[6], base, i = 0;
	d += 7, len -= 7;
	/* the records' length: 20 bytes as the spec's fields add up, 22 from a server that pads */
	if (!walk(d, len, count, base = 20) && !walk(d, len, count, base = 22))
		return;
	for (int n = 0; n < count; n++) {
		srec_t r;
		parse_rec(d + i, &r);
		if (r.lflags & LF_AT)
			r.at = (uint16_t)rd16(d + i + base);
		i += base + ((r.lflags & LF_AT) ? 2 : 0);
		if (!seq_new(r.seq)) {
			Snd_Stats.dups++;
			continue;
		}
		voice_new(&r, SRC_SND);
	}
}

/* AMBIENT: the whole set of client-side loops (ambient_generic, ncSound): each entry
 * {u16 ent, u32 key, u8 vol, u8 atten, u8 pitch, u8 flags, s16 org[3]} */
static void amb_start(int ai)
{
	samb_t *a = &H->amb[ai];
	srec_t r = {0};
	int vi;
	r.ent = a->ent, r.chan = CHAN_BODY, r.key = a->key, r.vol = a->vol, r.atten = a->atten, r.pitch = a->pitch;
	r.lflags = LF_CLIENTSIDE, r.sflags = a->flags & SF_FORCELOOP;
	memcpy(r.org, a->org, sizeof(r.org));
	a->voice = -1;
	if ((vi = voice_new(&r, SRC_AMB)) >= 0) {
		S.v[vi].amb = (int8_t)ai;
		a->voice = (int8_t)vi;
	}
}

static void handle_ambient(const uint8_t *d, int len)
{
	samb_t *nw = H->ambnew;
	bool *start = H->ambstart;
	int n = d[8];
	memset(nw, 0, sizeof(H->ambnew));
	uint16_t ver = (uint16_t)rd16(d + 6);
	if (ver == S.ambver || len < 9 + n * 16 || n > S_AMB)
		return;
	for (int i = 0; i < n; i++) {
		const uint8_t *e = d + 9 + i * 16;
		samb_t *a = &nw[i];
		a->ent = (uint16_t)rd16(e), a->key = rd32(e + 2), a->vol = e[6], a->atten = e[7], a->pitch = e[8], a->flags = e[9];
		for (int c = 0; c < 3; c++)
			a->org[c] = (int16_t)rd16(e + 10 + c * 2);
		a->voice = -1, a->pendkey = 0;
	}
	/* gone: stopped */
	for (int j = 0; j < S.namb; j++) {
		bool kept = false;
		for (int i = 0; i < n; i++)
			kept |= nw[i].ent == H->amb[j].ent;
		if (!kept && H->amb[j].voice >= 0)
			voice_free(H->amb[j].voice, "ambstop");
	}
	for (int i = 0; i < n; i++) {
		samb_t *a = &nw[i], *o = NULL;
		for (int j = 0; j < S.namb; j++)
			if (H->amb[j].ent == a->ent)
				o = &H->amb[j];
		start[i] = !o; /* new: started below, once the table is in place */
		if (!o)
			continue;
		a->voice = o->voice, a->pendkey = o->pendkey;
		if (a->flags & 128) { /* ncSound: values only when it appears (Sound.qc:198-205) */
			a->key = o->key, a->vol = o->vol, a->atten = o->atten, a->pitch = o->pitch, a->flags = o->flags;
			memcpy(a->org, o->org, sizeof(a->org));
			a->pendkey = 0;
			continue;
		}
		if (a->key == o->key)
			a->pendkey = 0; /* (back to the sample it plays: a switch waiting is called off) */
		if (!memcmp(a, o, offsetof(samb_t, voice)))
			continue; /* (the same entry) */
		/* ambient_generic: soundupdate on every update (ambient_generic.qc:487-491, snd_main.c:909-939) */
		if (a->key != o->key) {
			a->pendkey = a->key, a->key = o->key; /* switched once the new one is in */
		}
		if (a->voice >= 0) {
			svoice_t *v = &S.v[a->voice];
			v->vol = a->vol, v->atten = a->atten;
			v->vol_q16 = (uint32_t)(((uint64_t)a->vol * S.master_q16 + 127) / 255);
			v->f_q8 = H->f_q8[a->atten];
			if (v->pitch != a->pitch) {
				v->pitch = a->pitch;
				if (v->hw >= 0 && S.vst[v - S.v] == V_PLAY) {
					cmd_begin();
					soundChSetTimer((unsigned)v->hw, (unsigned)sample_timer(&S.k[v->ki], v->pitch));
					cmd_end();
				}
			}
			for (int c = 0; c < 3; c++)
				v->org[c] = a->org[c] * 8;
		} else if (!a->pendkey)
			start[i] = true; /* ended or stolen: any change starts it again (bi_soundupdate -> bi_sound) */
	}
	memcpy(H->amb, nw, sizeof(nw[0]) * (size_t)n);
	S.namb = n;
	for (int i = 0; i < n; i++) {
		if (H->amb[i].voice >= 0)
			S.v[H->amb[i].voice].amb = (int8_t)i;
		else if (start[i])
			amb_start(i);
	}
	S.ambver = ver;
}

uint16_t Snd_AmbientAck(void) { return S.ambver; }

void Snd_Packet(int type, const uint8_t *d, int len, uint16_t spawncount)
{
	uint32_t t0 = ticks();
	if (!S.ready || len < 7 || (uint16_t)rd16(d + 4) != spawncount)
		return;
	if (type == 0x91)
		handle_sound(d, len);
	else if (type == 0x92 && len >= 9)
		handle_ambient(d, len);
	Snd_Stats.cpu_ticks += ticks() - t0;
}

/* SENTENCE: u16 ent, u8 atten, u8 n, n x u32 key: words one after another on CHAN_VOICE
 * (TalkMonster.qc:979-1040, 1222-1245) */
static void handle_sentence(const uint8_t *d, int len)
{
	ssent_t *s = NULL;
	int n;
	if (len < 4 || len < 4 + (n = d[3]) * 4)
		return;
	for (int i = 0; i < S_SENT && !s; i++)
		if (H->sent[i].on && H->sent[i].ent == rd16(d))
			s = &H->sent[i];
	for (int i = 0; i < S_SENT && !s; i++)
		if (!H->sent[i].on)
			s = &H->sent[i];
	if (!s)
		s = &H->sent[0];
	memset(s, 0, sizeof(*s));
	s->on = true, s->ent = (uint16_t)rd16(d), s->atten = d[2], s->n = (uint8_t)BW_NDS_MIN(n, S_SENTWORDS);
	for (int i = 0; i < s->n; i++) {
		s->keys[i] = rd32(d + 4 + i * 4);
		key_get(s->keys[i]); /* queued for loading at once */
	}
	s->waiting = true;
}

static void sentence_frame(void)
{
	uint32_t t = ticks();
	for (int i = 0; i < S_SENT; i++) {
		ssent_t *s = &H->sent[i];
		int ki;
		if (!s->on)
			continue;
		if (!s->waiting) {
			if ((int32_t)(t - s->t_next) < 0)
				continue;
			s->waiting = true;
		}
		if (s->next >= s->n) {
			s->on = false;
			continue;
		}
		ki = key_get(s->keys[s->next]);
		if (ki < 0)
			continue;
		if (S.k[ki].state == K_MISS) { /* soundlength 0: the next one at once */
			s->next++;
			continue;
		}
		if (!key_ready(ki))
			continue;
		{
			srec_t r = {0};
			int vi;
			r.ent = s->ent, r.chan = CHAN_VOICE, r.key = s->keys[s->next], r.vol = 255, r.atten = s->atten, r.pitch = 100;
			r.lflags = LF_CLIENTSIDE, r.sflags = SF_FOLLOW;
			{
				int32_t o[3];
				if (!thing_origin(s->ent, o, NULL))
					memcpy(o, S.lorg, sizeof(o));
				for (int c = 0; c < 3; c++)
					r.org[c] = (int16_t)(o[c] / 8);
			}
			if ((vi = voice_new(&r, SRC_SENT)) >= 0)
				voice_start(vi);
			s->t_next = t + (uint32_t)((uint64_t)S.k[ki].frames * TICK_FREQ / BW_NDS_MAX(S.k[ki].rate, 1));
			s->next++;
			s->waiting = false;
		}
	}
}

void Snd_Event(int type, const uint8_t *d, int len)
{
	if (!S.ready)
		return;
	if (type == 12) { /* SOUND, reliably: loops, and what stops or replaces one */
		srec_t r;
		int base = len - ((len >= 3 && (d[2] & LF_AT)) ? 2 : 0);
		if (base != 20 && base != 22)
			return;
		parse_rec(d, &r);
		if (r.lflags & LF_AT)
			r.at = (uint16_t)rd16(d + base);
		voice_new(&r, SRC_LOOP);
	} else if (type == 13)
		handle_sentence(d, len);
	else if (type == 14) { /* MUSIC: no streams yet */
		char name[64];
		int n = BW_NDS_CLAMP(len - 1, 0, 63);
		memcpy(name, d + 1, (size_t)n), name[n] = 0;
		logf_("SNDMUSIC op=%d %s (not played: no music streams on the DS yet)\n", len ? d[0] : -1, name);
	} else if (type == 15 && len >= 3 && S.pkey) { /* PRECACHE: u16 first, u8 n, keys */
		int first = (int)rd16(d), n = d[2];
		/* the kept names (first 0xf000) go after the map's list */
		int base = first >= 0xf000 ? S_PRECACHE + (first - 0xf000) : first, lim = first >= 0xf000 ? S_PRECACHE + S_KEPT : S_PRECACHE;
		for (int i = 0; i < n && 3 + i * 4 + 4 <= len && base + i < lim; i++) {
			uint32_t key = rd32(d + 3 + i * 4);
			int idx = base + i;
			if (S.pkey[idx] == key)
				continue;
			if (S.pkey[idx])
				pord_remove(idx); /* (a slot that changes: sounds precached after the map's are new ones) */
			S.pkey[idx] = key;
			S.npk = BW_NDS_MAX(S.npk, idx + 1);
			S.plook = BW_NDS_MIN(S.plook, idx);
		}
	}
}

static void local_key(uint32_t key, uint16_t ent, int src)
{
	srec_t r = {0};
	int vi;
	r.key = key, r.vol = 255, r.atten = 0, r.pitch = 100, r.lflags = LF_CLIENTSIDE, r.ent = ent;
	for (int c = 0; c < 3; c++)
		r.org[c] = (int16_t)(S.lorg[c] / 8);
	vi = voice_new(&r, src);
	(void)vi;
}

void Snd_Local(uint32_t key) { local_key(key, 0, SRC_LOCAL); }
void Snd_LocalName(const char *name) { Snd_Local(snd_key(name)); }

/* HLWeaponSelect::SelectNext/SelectPrevious (valve/src/hud/WeaponSelect.qc:283-343): opening
 * plays wpn_hudon, a move while open wpn_moveselect; it stays open hlhud_hideTime (3 s, hud.h:10).
 * The DS switches at once (no confirm), so there is no Trigger and no wpn_hudoff */
void Snd_WeaponSelect(void)
{
	uint32_t t = ticks();
	if (!S.ready)
		return;
	Snd_LocalName((int32_t)(S.wsel_until - t) > 0 ? "common/wpn_moveselect.wav" : "common/wpn_hudon.wav");
	S.wsel_until = t + ms_ticks(3000);
}

/* WEAPONINFO's table: u8 'S', u8 n, n x {u8 nsamp, nsamp x u32 key, u8 vol, u8 pitchmin,
 * u8 pitchmax, u8 atten, u8 flags, u8 chan, u8 dist, s16 offset_ms} */
void Snd_WeaponTable(const uint8_t *d, int len)
{
	int i = 2;
	S.nshader = 0;
	if (!H || len < 2 || d[0] != 'S')
		return;
	for (int n = 0; n < d[1] && S.nshader < S_SHADERS; n++) {
		sshader_t *s = &H->shader[S.nshader];
		int ns;
		if (i >= len || (ns = d[i]) < 1 || ns > 8 || i + 1 + ns * 4 + 9 > len)
			break;
		s->nsamp = (uint8_t)ns;
		for (int k = 0; k < ns; k++)
			s->keys[k] = rd32(d + i + 1 + k * 4);
		i += 1 + ns * 4;
		s->vol = d[i], s->pmin = d[i + 1], s->pmax = d[i + 2], s->atten = d[i + 3], s->flags = d[i + 4], s->chan = d[i + 5];
		s->dist = d[i + 6], s->offset_ms = (int16_t)rd16(d + i + 7);
		i += 9;
		S.nshader++;
	}
}

static void play_shader(int si, bool view, int depth)
{
	const sshader_t *s;
	srec_t r = {0};
	int vi, p;
	if (si < 0 || si >= S.nshader || depth > 1)
		return;
	s = &H->shader[si];
	p = s->pmax > s->pmin ? s->pmin + (int)(rnd() % (unsigned)(s->pmax - s->pmin + 1)) : s->pmin;
	r.key = s->keys[s->nsamp > 1 ? rnd() % s->nsamp : 0];
	r.vol = s->vol, r.atten = s->atten, r.pitch = (uint8_t)BW_NDS_CLAMP(p, 1, 255), r.chan = s->chan;
	r.sflags = s->flags, r.lflags = LF_CLIENTSIDE;
	r.ent = view ? VIEW_ENT : 0;
	if (!view) /* the distance shader: chan 5, ATTN_NONE, no FOLLOW (soundDef.qc:450-511) */
		r.chan = CHAN_LOOP, r.atten = 0, r.sflags &= (uint8_t)~SF_FOLLOW;
	for (int c = 0; c < 3; c++)
		r.org[c] = (int16_t)(S.lorg[c] / 8);
	if ((vi = voice_new(&r, SRC_VM)) >= 0 && s->offset_ms < 0) {
		/* a negative offset delays the start (snd_main.c:204); a positive one would skip in: not done */
		S.vst[vi] = V_DELAY;
		S.v[vi].ownclock = true;
		S.v[vi].t_due = ticks() + ms_ticks((uint32_t)BW_NDS_MIN(-s->offset_ms, 2000));
	}
	if (s->dist != 0xff)
		play_shader(s->dist, false, depth + 1);
}

/* the view model's events 1004 (CHAN_BODY), 1008 (CHAN_VOICE), 5004 (a sample or a decl): the
 * option is the shader's index (viewmodel.qc:116-122, RenderableEntity.qc:1088-1146) */
void Snd_ViewEvent(int code, int opt)
{
	if (!S.ready || (code != 1004 && code != 1008 && code != 5004))
		return;
	play_shader(opt, true, 0);
}

/* --- the frame -------------------------------------------------------------------------------- */


/* the reads a frame: what voices wait for, within sndreadms (at least one read: a sound that
 * came is late otherwise), a read only while the time spent and one more read fit; the
 * precache list's preload, which nothing waits for, only in a frame that has read nothing
 * else and then one read. A long frame (over 100 ms: the models load after the map at 1-2
 * frames a second) gets an eighth of its length, up to 60 ms, for all of it */
static void loader(void)
{
	uint32_t t0 = ticks(), budget = ms_ticks((uint32_t)S.readms);
	int reads = 0, r4;
	bool longframe = S.frameint > ms_ticks(100);
	if (longframe)
		budget = BW_NDS_MAX(budget, BW_NDS_MIN(S.frameint / 8, ms_ticks(60)));
#define BUDGET_LEFT() (reads == 0 || ticks() - t0 + S.readest <= budget)
#define PRECACHE_LEFT() (longframe ? BUDGET_LEFT() : reads == r4 && r4 == 0 && ticks() - t0 + S.readest <= budget)
	/* 1: samples a channel is playing while they load */
	for (int h = 0; h < S_HW && BUDGET_LEFT(); h++)
		if (S.hw[h].v >= 0 && S.hw[h].blk >= 0) {
			int ki = H->blk[S.hw[h].blk].ki;
			while (ki >= 0 && !key_complete(ki) && BUDGET_LEFT() && load_step(ki, false, false))
				reads++;
		}
	/* 2: samples voices wait for, in the order they came */
	for (;;) {
		int best = -1, ki;
		bool did = false, loop;
		if (!BUDGET_LEFT())
			break;
		/* one-shots in the order they came, then loops loudest first: the pinned cap (half the
		 * cache) admits the loudest */
		for (int i = 0; i < S.vtop; i++) {
			svoice_t *v = &S.v[i], *o = best >= 0 ? &S.v[best] : NULL;
			bool vl, ol;
			if (!((S.vst[i] == V_WAIT || (S.vst[i] == V_PLAY && v->loop && v->hw < 0)) && v->ki >= 0 && !key_ready(v->ki) &&
			      S.k[v->ki].state != K_MISS && v->skip != S.use))
				continue;
			vl = voice_isloop(v);
			if (vl)
				voice_gain(v);
			ol = o && voice_isloop(o);
			if (!o || (vl != ol ? !vl : vl ? v->loud > o->loud : (int32_t)(v->t_arrive - o->t_arrive) < 0))
				best = i;
		}
		if (best < 0)
			break;
		ki = S.v[best].ki;
		loop = voice_isloop(&S.v[best]);
		while (!key_ready(ki) && BUDGET_LEFT() && load_step(ki, true, loop))
			reads++, did = true;
		if (!did && !key_ready(ki) && S.k[ki].state == K_FOUND) {
			/* no room (pinned), or a stream: a one-shot goes, a loop waits */
			S.v[best].skip = S.use;
			if (S.k[ki].fmt == FMT_STREAM || S.k[ki].bytes > S.arenasize / 2) {
				Snd_Stats.toobig++;
				voice_free(best, "toobig");
			} else if (!loop && S.vst[best] == V_WAIT) {
				Snd_Stats.quiet++;
				voice_free(best, "noroom");
			}
		}
	}
	/* 3: sentence words and ambient switches */
	for (int i = 0; i < S_SENT && BUDGET_LEFT(); i++)
		if (H->sent[i].on)
			for (int w = H->sent[i].next; w < H->sent[i].n && BUDGET_LEFT(); w++) {
				int ki = key_get(H->sent[i].keys[w]);
				while (ki >= 0 && !key_complete(ki) && BUDGET_LEFT() && load_step(ki, true, false))
					reads++;
			}
	for (int i = 0; i < S.namb && BUDGET_LEFT(); i++)
		if (H->amb[i].pendkey) {
			int ki = key_get(H->amb[i].pendkey);
			while (ki >= 0 && !key_complete(ki) && BUDGET_LEFT() && load_step(ki, true, true))
				reads++;
		}
	/* 4: the precache list: each name looked up, what is found kept in size order, and read
	 * smallest first into free room, up to 75% */
	r4 = reads;
	while (S.pkey && S.plook < S.npk && PRECACHE_LEFT()) {
		int ki;
		if (!S.pkey[S.plook]) {
			S.plook++;
			continue;
		}
		ki = key_get(S.pkey[S.plook]);
		if (ki >= 0 && S.k[ki].state == K_PENDING) {
			if (key_lookup_step(ki))
				reads++;
			continue;
		}
		if (ki >= 0 && S.k[ki].state == K_FOUND)
			pord_insert(S.plook, S.k[ki].bytes);
		S.plook++;
	}
	while (S.pkey && S.pnext < S.npord && PRECACHE_LEFT()) {
		int idx = (int)(S.pord[S.pnext] & 0xffff), ki = key_find(S.pkey[idx]);
		if (ki < 0 || S.k[ki].state != K_FOUND || S.k[ki].fmt == FMT_STREAM || S.k[ki].bytes > S.arenasize / 2 ||
		    (S.k[ki].block >= 0 && key_complete(ki))) {
			S.pnext++;
			continue;
		}
		if (S.preload_bytes + S.k[ki].bytes > S.arenasize / 4 * 3) {
			S.pnext = S.npord; /* (smallest first: none after fits) */
			break;
		}
		if (!load_step(ki, false, false)) {
			S.pnext = S.npord; /* no free room left */
			break;
		}
		reads++;
		while (!key_complete(ki) && PRECACHE_LEFT() && load_step(ki, false, false))
			reads++;
		if (key_complete(ki)) {
			S.preload_bytes += S.k[ki].bytes;
			S.pnext++;
		}
	}
#undef BUDGET_LEFT
#undef PRECACHE_LEFT
}

void Snd_Frame(const ndscam_t *cam, int selfent, const sndthing_t *things, int nthings, uint32_t clock_ms, bool have)
{
	uint32_t t0 = ticks(), t, active;
	if (!S.ready)
		return;
	S.use++;
	cmds = 0;
	S.frameint = t0 - S.lastframe, S.lastframe = t0;
	S.readest -= S.readest / 32; /* (a slow read counts for a second or so) */
	/* the listener: the camera the DS draws with (EFX_UpdateListener, SoundScape.qc:853-868); its
	 * right vector from yaw alone ((sin yaw, -cos yaw, 0): AngleVectors with no roll) */
	memcpy(S.lorg, cam->org, sizeof(S.lorg));
	S.lright[0] = sinLerp((s16)cam->yaw), S.lright[1] = -cosLerp((s16)cam->yaw), S.lright[2] = 0;
	S.havelistener = true;
	S.selfent = selfent;
	S.things = things, S.nthings = nthings;
	S.clock = clock_ms, S.haveclock = have;
	while (S.vtop > 0 && S.vst[S.vtop - 1] == V_FREE)
		S.vtop--;
	/* nothing playing, coming or due: only the stats */
	{
		bool idle = S.plook >= S.npk && S.pnext >= S.npord;
		for (int i = 0; i < S.vtop && idle; i++)
			idle = S.vst[i] == V_FREE;
		for (int h = 0; h < S_HW && idle; h++)
			idle = S.hw[h].v < 0 && !S.hw[h].draining;
		for (int i = 0; i < S_SENT && idle; i++)
			idle = !H->sent[i].on;
		for (int i = 0; i < S.namb && idle; i++)
			idle = !H->amb[i].pendkey;
		if (idle)
			goto stats;
	}

	/* the channels: ended, or seen off after a stop */
	{
		uint32_t a0 = ticks();
		active = soundGetActiveChannels();
		S.statactive = BW_NDS_MAX(S.statactive, ticks() - a0);
	}
	t = ticks();
	for (int h = 0; h < S_HW; h++) {
		shw_t *c = &S.hw[h];
		if (c->draining) {
			if (!(active & (1u << h)) && t - c->t_stop > ms_ticks(40))
				hw_unpin(h);
			continue;
		}
		if (c->v < 0)
			continue;
		if ((c->t_end && (int32_t)(t - c->t_end) >= 0) || (!(active & (1u << h)) && t - c->t_start > ms_ticks(60))) {
			int vi = c->v;
			voice_log(&S.v[vi], "end", h, 0, 0, 0);
			hw_release(h, false);
			if (S.v[vi].loop) /* (a loop the hardware stopped: silent, as when stolen) */
				;
			else
				voice_gone(vi);
		} else if (!S.v[c->v].loop && c->blk >= 0 && !S.v[c->v].underrun && H->blk[c->blk].loaded < S.k[S.v[c->v].ki].bytes) {
			/* still loading: played past what is in? (REG_DIV: a 64-bit '/' is a slow library call) */
			svoice_t *v = &S.v[c->v];
			skey_t *k = &S.k[v->ki];
			uint32_t f = SND_DIV64((uint64_t)(t - c->t_start) * 32, c->timer);
			uint32_t bytes = SND_DIV64((uint64_t)f * k->bytes, BW_NDS_MAX(k->frames, 1));
			if (bytes > H->blk[c->blk].loaded) {
				v->underrun = true;
				Snd_Stats.underrun++;
			}
		}
	}
	/* the delayed (AT records: when the drawn clock passes them) */
	for (int i = 0; i < S.vtop; i++) {
		svoice_t *v = &S.v[i];
		if (S.vst[v - S.v] != V_DELAY)
			continue;
		if (v->ownclock ? (int32_t)(t - v->t_due) >= 0
		                : !S.haveclock || (int16_t)((uint16_t)S.clock - v->at) >= 0 || t - v->t_arrive > ms_ticks(2000))
			S.vst[v - S.v] = V_WAIT, v->t_arrive = t;
	}
	S.sec[0] += ticks() - t0;
	sentence_frame();
	{
		uint32_t r0 = ticks();
		loader();
		S.sec[1] += ticks() - r0;
	}
	t = ticks();
	/* waiting voices whose sample is in start; one-shots too late, or whose sample isn't anywhere, go */
	t = ticks();
	for (int i = 0; i < S.vtop; i++) {
		svoice_t *v = &S.v[i];
		if (S.vst[v - S.v] != V_WAIT)
			continue;
		if (S.k[v->ki].state == K_MISS) {
			Snd_Stats.missing++;
			voice_log(v, "missing", -1, 0, 0, 0);
			voice_gone(i);
			continue;
		}
		if (key_ready(v->ki))
			voice_start(i);
		else if (v->src != SRC_SENT && v->src != SRC_AMB && v->src != SRC_LOOP && !(v->sflags & SF_FORCELOOP) &&
		         t - v->t_arrive > ms_ticks(S_LATE_MS)) {
			Snd_Stats.late++;
			voice_log(v, "late", -1, 0, 0, ticks_ms(t - v->t_arrive));
			voice_gone(i);
		}
	}
	/* ambient_generic sample switches whose new sample is in */
	for (int i = 0; i < S.namb; i++)
		if (H->amb[i].pendkey) {
			int ki = key_find(H->amb[i].pendkey);
			if (ki >= 0 && (key_ready(ki) || S.k[ki].state == K_MISS)) {
				if (S.k[ki].state == K_FOUND) {
					if (H->amb[i].voice >= 0)
						voice_free(H->amb[i].voice, "ambswitch");
					H->amb[i].key = H->amb[i].pendkey;
					amb_start(i);
				}
				H->amb[i].pendkey = 0;
			}
		}
	S.sec[2] += ticks() - t;
	t = ticks();
	/* gains; loops without a channel take one if they are louder than one held */
	for (int i = 0; i < S.vtop; i++)
		if (S.vst[i] == V_PLAY)
			voice_gain(&S.v[i]);
	for (;;) {
		int best = -1;
		for (int i = 0; i < S.vtop; i++) {
			svoice_t *v = &S.v[i];
			if (S.vst[v - S.v] == V_PLAY && v->loop && v->hw < 0 && v->loud && key_ready(v->ki) &&
			    (best < 0 || v->loud > S.v[best].loud))
				best = i;
		}
		if (best < 0 || cmds >= S_CMDS)
			break;
		{
			/* a held channel goes only to one louder by a quarter (no swapping back and forth) */
			int h = -1;
			for (int k = 0; k < S_HW && h < 0; k++)
				if (S.hw[k].v < 0 && !S.hw[k].draining)
					h = k;
			if (h < 0)
				h = hw_pick(S.v[best].loud - S.v[best].loud / 5);
			if (h < 0)
				break;
			S.v[best].t_start = ticks();
			hw_start(h, best);
			voice_log(&S.v[best], "resume", h, S.hw[h].vol, S.hw[h].pan, 0);
		}
	}
	/* loops gone silent give their channel back */
	for (int h = 0; h < S_HW; h++)
		if (S.hw[h].v >= 0 && S.v[S.hw[h].v].loop && !S.v[S.hw[h].v].loud) {
			voice_log(&S.v[S.hw[h].v], "silent", h, 0, 0, 0);
			hw_release(h, true);
		}
	S.sec[3] += ticks() - t;
	t = ticks();
	/* volume and pan: only on a change worth hearing, a few a frame, round the channels */
	for (int n = 0; n < S_HW && cmds < S_CMDS; n++) {
		int h = (S.cmdrr + n) % S_HW, vol, pan;
		shw_t *c = &S.hw[h];
		if (c->v < 0)
			continue;
		snd_hwvolpan(S.v[c->v].g, &vol, &pan);
		if (vol != c->vol && (abs(vol - c->vol) * 16 >= c->vol || !vol || !c->vol)) {
			cmd_begin();
			soundChSetVolume((unsigned)h, (unsigned)vol);
			cmd_end();
			c->vol = (uint16_t)vol;
		}
		if (abs(pan - c->pan) >= 2 && cmds < S_CMDS) {
			cmd_begin();
			soundChSetPan((unsigned)h, (unsigned)pan);
			cmd_end();
			c->pan = (uint8_t)pan;
		}
		if (n == S_HW - 1 || cmds >= S_CMDS)
			S.cmdrr = (h + 1) % S_HW;
	}
	S.sec[4] += ticks() - t;
stats:
	/* the stats, each second */
	t = ticks() - t0;
	Snd_Stats.frames++;
	Snd_Stats.cpu_ticks += t;
	Snd_Stats.cpu_ticks_max = BW_NDS_MAX(Snd_Stats.cpu_ticks_max, t);
	S.statframes++, S.statcpu += t, S.statcpumax = BW_NDS_MAX(S.statcpumax, t);
	if (ticks() - S.laststat > TICK_FREQ) {
		int nv = 0, nh = 0;
		/* (a check: an AMBIENT entry's voice is busy and is that entry's) */
		for (int i = 0; i < S.namb && S.log; i++)
			if (H->amb[i].voice >= 0 && (S.vst[H->amb[i].voice] == V_FREE || S.v[H->amb[i].voice].amb != i))
				logf_("SNDBUG amb %d voice %d state %d amb %d\n", i, H->amb[i].voice, S.vst[H->amb[i].voice],
				      S.v[H->amb[i].voice].amb);
		for (int i = 0; i < S_VOICES; i++)
			nv += S.vst[i] != V_FREE;
		for (int h = 0; h < S_HW; h++)
			nh += S.hw[h].v >= 0;
		logf_("SNDSTAT voices=%d hw=%d cache=%u/%u pinned=%u misses=%u loads=%u late=%u quiet=%u stolen=%u dups=%u underrun=%u "
		      "readms=%u.%02u readsum_ms=%u creditms=%u.%02u active_us=%u cpu_us avg=%u max=%u reads=%u readkb=%u sec=%u/%u/%u/%u/%u\n",
		      nv, nh, (unsigned)(used_bytes() / 1024), (unsigned)(S.arenasize / 1024), (unsigned)(pinned_bytes() / 1024),
		      (unsigned)Snd_Stats.missing, (unsigned)Snd_Stats.starts, (unsigned)Snd_Stats.late, (unsigned)Snd_Stats.quiet,
		      (unsigned)Snd_Stats.stolen, (unsigned)Snd_Stats.dups, (unsigned)Snd_Stats.underrun,
		      (unsigned)(ticks_ms(S.statreadmax * 100) / 100), (unsigned)(ticks_ms(S.statreadmax * 100) % 100),
		      (unsigned)ticks_ms(S.statreadsum),
		      (unsigned)(ticks_ms(S.statcmdmax * 100) / 100), (unsigned)(ticks_ms(S.statcmdmax * 100) % 100),
		      (unsigned)((uint64_t)S.statactive * 1000000 / TICK_FREQ),
		      (unsigned)((uint64_t)S.statcpu * 1000000 / TICK_FREQ / BW_NDS_MAX(S.statframes, 1)),
		      (unsigned)((uint64_t)S.statcpumax * 1000000 / TICK_FREQ), (unsigned)Snd_Stats.reads,
		      (unsigned)(Snd_Stats.readbytes / 1024), (unsigned)(S.sec[0] * 1911 / 1000 / BW_NDS_MAX(S.statframes, 1)),
		      (unsigned)(S.sec[1] * 1911 / 1000 / BW_NDS_MAX(S.statframes, 1)), (unsigned)(S.sec[2] * 1911 / 1000 / BW_NDS_MAX(S.statframes, 1)),
		      (unsigned)(S.sec[3] * 1911 / 1000 / BW_NDS_MAX(S.statframes, 1)), (unsigned)(S.sec[4] * 1911 / 1000 / BW_NDS_MAX(S.statframes, 1)));
		memset(S.sec, 0, sizeof(S.sec));
		S.statreadsum = 0;
		S.laststat = ticks();
		S.statframes = S.statcpu = S.statcpumax = S.statreadmax = S.statcmdmax = S.statactive = 0;
	}
}

/* --- setup ------------------------------------------------------------------------------------ */

static void defaults(void)
{
	if (S.inited)
		return;
	S.on = true;
	S.master_q16 = 45875; /* volume 0.7 (snd_main.c:835) */
	S.bgm_q16 = 32768;    /* bgmvolume 0.5 (:836) */
	S.cachekb = 192;
	S.readms = 2;
	S.inited = true;
	S.rnd = 1;
}

void Snd_Init(void)
{
	defaults();
	logf_("SNDINIT on=%d cache=%dKB readms=%d volume=%u\n", S.on, S.cachekb, S.readms, (unsigned)S.master_q16);
	for (int h = 0; h < S_HW; h++)
		S.hw[h].v = S.hw[h].blk = -1;
	if (!S.on)
		return;
	soundInit();
	soundPowerOn();
	soundSetMixerVolume(127);
	soundSetAutoUpdate(true);
	memset(&Snd_Stats, 0, sizeof(Snd_Stats));
}

static void stop_all(void)
{
	if (!S.on)
		return;
	soundStop(0xffff);
	soundSynchronize(); /* the ARM7 reads no more of what is freed next */
	for (int h = 0; h < S_HW; h++) {
		if (S.hw[h].v >= 0 && S.v)
			S.v[S.hw[h].v].hw = -1;
		S.hw[h].v = -1;
		hw_unpin(h);
	}
	for (int i = 0; i < S_VOICES; i++)
		S.vst[i] = V_FREE;
	S.vtop = 0;
	if (H)
		memset(H->sent, 0, sizeof(H->sent));
	S.namb = 0, S.ambver = 0;
	S.haveseq = false;
}

void Snd_MapChange(void)
{
	stop_all();
	precache_reset();
	S.nshader = 0;
}

void Snd_MapUnload(void)
{
	stop_all();
	S.ready = false;
	free(S.arena), S.arena = NULL, S.arenasize = 0;
	free(S.v), S.v = NULL;
	free(S.k), S.k = NULL;
	free(S.pkey), S.pkey = NULL;
	free(S.pord), S.pord = NULL;
	free(H), H = NULL;
	S.nblk = 0;
	precache_reset();
	close_packs();
}

static uint32_t heap_free(void)
{
	struct mallinfo mi = mallinfo();
	uint8_t *top = (uint8_t *)sbrk(0), *lim = getHeapLimit();
	return (uint32_t)(lim - top) + (uint32_t)mi.fordblks;
}

void Snd_MapLoaded(void)
{
	uint32_t want, avail;
	if (!S.on)
		return;
	if (S.ready)
		Snd_MapUnload();
	open_packs();
	if (!S.npacks) {
		logf_("SNDSTAT no sound packs (sound/NAME.dsp): no sound\n");
		return;
	}
	S.v = calloc(S_VOICES, sizeof(svoice_t));
	S.k = calloc(S_KEYS, sizeof(skey_t));
	S.pkey = calloc(S_PRECACHE + S_KEPT, sizeof(uint32_t));
	S.pord = calloc(S_PRECACHE + S_KEPT, sizeof(uint64_t));
	H = calloc(1, sizeof(*H));
	/* the cache, leaving room for the models still to load (they come after the map) */
	want = (uint32_t)S.cachekb * 1024;
	avail = heap_free();
	if (avail < want + 384 * 1024)
		want = avail > (64 + 384) * 1024 ? avail - 384 * 1024 : 0;
	want &= ~1023u;
	S.arena = want ? memalign(32, want) : NULL;
	if (!S.v || !S.k || !S.pkey || !S.pord || !H || !S.arena) {
		logf_("SNDSTAT no memory for sound (free %u KB)\n", (unsigned)(avail / 1024));
		Snd_MapUnload();
		return;
	}
	S.arenasize = want;
	Snd_Stats.cachekb = want / 1024;
	for (int i = 0; i < S_KEYS; i++)
		S.k[i].block = -1;
	memset(S.vst, 0, sizeof(S.vst));
	memset(H->khead, 0xff, sizeof(H->khead));
	S.nkeys = 0;
	for (int a = 0; a < 256; a++)
		H->f_q8[a] = snd_factor_q8(a);
	H->pagecache[0].pack = H->pagecache[1].pack = -1;
	S.ready = true;
	logf_("SNDSTAT ready packs=%d cache=%uKB free_after=%uKB\n", S.npacks, (unsigned)(want / 1024),
	      (unsigned)(heap_free() / 1024));
}

void Snd_Shutdown(void)
{
	Snd_MapUnload();
	if (S.on)
		soundSetAutoUpdate(false);
}
