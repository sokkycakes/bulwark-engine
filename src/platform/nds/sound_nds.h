/* Bulwark engine - the DS link client's sound: what the PC client hears in co-op,
 * from the server's SOUND/AMBIENT packets and EVENT records 12-15 (docs/spec/link.md),
 * played on the DS's 16 hardware channels with the PC mixer's gains (src/sound/snd_main.c).
 * Samples come from sound packs on the card (<base>/sound/NAME.dsp, tools/nds/sndpack.py),
 * cached in a fixed budget and read in slices within a time budget each frame.
 * docs/platforms/nds.md "Sound". ISC licence, see LICENSE. */
#ifndef BW_SOUND_NDS_H
#define BW_SOUND_NDS_H

#include <stdbool.h>
#include <stdint.h>

#include "nds.h"

/* an entity the DS draws, for sounds that follow it: its origin (units) */
typedef struct {
	uint16_t ent;
	int16_t org[3];
} sndthing_t;

/* link.txt keys: volume, bgmvolume, sndcache (KB), sndreadms, snd_log, sound (0 off). True if taken */
bool Snd_Config(const char *key, const char *val);
void Snd_Init(void);     /* once, before joining */
void Snd_Shutdown(void); /* leaving: everything stops and is freed */
/* a new map (WELCOME with another spawncount): every sound stops, as cl_cmds.c does */
void Snd_MapChange(void);
void Snd_MapUnload(void); /* before a map loads: the memory sound holds goes */
void Snd_MapLoaded(void); /* after: packs, voices, the cache */
/* SOUND (0x91) and AMBIENT (0x92), from the session on */
void Snd_Packet(int type, const uint8_t *d, int len, uint16_t spawncount);
/* EVENT records 12 SOUND, 13 SENTENCE, 14 MUSIC, 15 PRECACHE */
void Snd_Event(int type, const uint8_t *d, int len);
/* a sound the DS plays by itself, as localsound: no position, full volume */
void Snd_Local(uint32_t key);
void Snd_LocalName(const char *name);
/* HLWeaponSelect's open/move sounds (valve/src/hud/WeaponSelect.qc) on the DS's weapon switch */
void Snd_WeaponSelect(void);
/* WEAPONINFO's sound table (after the sequences) and its view model events (1004, 1008, 5004) */
void Snd_WeaponTable(const uint8_t *d, int len);
void Snd_ViewEvent(int code, int opt);
/* once a frame: the camera drawn, our player's entity, the things drawn and the clock they
 * are drawn at (server ms; have false before the first ENTS) */
void Snd_Frame(const ndscam_t *cam, int selfent, const sndthing_t *things, int nthings, uint32_t clock_ms, bool have);
uint16_t Snd_AmbientAck(void); /* INPUT's 'S' */
bool Snd_Enabled(void);
/* the FNV-1a key of a sample name (docs/spec/link.md "Sample names") */
uint32_t Snd_Key(const char *name);

typedef struct {
	uint32_t recs, dups, starts, missing, late, quiet, stolen, toobig, underrun;
	uint32_t frames, cpu_ticks, cpu_ticks_max; /* Snd_Frame, reads included */
	uint32_t read_ticks_max, reads, readbytes;
	uint32_t cmd_ticks_max;
	uint32_t cachekb;
} sndstats_t;
extern sndstats_t Snd_Stats;

#endif
