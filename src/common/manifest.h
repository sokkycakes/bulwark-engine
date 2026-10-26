/* Bulwark engine - game manifests (.fmf files, as fte_manifests.txt
 * describes them): which directories make up the game, its name and the
 * commands around its default.cfg. The gamedir, fs_changegame and
 * fs_showmanifest commands switch and show them. ISC licence, see LICENSE. */
#ifndef BW_MANIFEST_H
#define BW_MANIFEST_H

#include "common.h"

#define MF_MAX_DIRS 16
#define MF_MAX_PACKAGES 64

typedef struct {
	char path[128];   /* gamedir/file the package becomes */
	char crc[16];     /* "-" for none */
	char archived[128]; /* the file inside a zip, ARCHIVEDPACKAGE only */
	char mirrors[4][512];
	int nummirrors;
} mf_package_t;

typedef struct {
	char source[512]; /* where it came from: a file, a url, or "" when made up */
	char game[64], name[128], protocolname[64], mainconfig[64];
	char updateurl[512], downloadsurl[512], install[512], rtcbroker[256];
	char basegame[MF_MAX_DIRS][64];
	int numbasegames;
	char gamedir[MF_MAX_DIRS][64];
	int numgamedirs;
	strbuf_t precmds, postcmds; /* the - and + lines */
	bool disablehomedir;
	mf_package_t packages[MF_MAX_PACKAGES];
	int numpackages;
} manifest_t;

/* parses manifest text; false (with a message) if it isn't one */
bool Mf_Parse(manifest_t *mf, const char *text, const char *source);
void Mf_Free(manifest_t *mf);
const manifest_t *Mf_Active(void);
/* sets up the file system at startup from the -game list: a default.fmf in
 * the base directory, or a <game>.fmf for a single -game, or the list as is */
void Mf_Startup(const char *const *games, int count);
/* adds the startup configs to the command buffer: the - commands,
 * default.cfg, the + commands, the main config and autoexec.cfg */
void Mf_ExecConfigs(void);
/* switches to mf (taking it over) at the end of this frame: drops the game,
 * then remounts, reloads configs and restarts what depends on files */
void Mf_Switch(manifest_t *mf);
/* fetches a manifest from a url for fs_changegame; set by the network code */
typedef void (*mf_fetch_fn)(const char *url);
void Mf_SetFetcher(mf_fetch_fn fn);
/* changes only the mod directories on top of the base games */
void Mf_SwitchGamedir(const char *dir);
/* commands to run once the pending switch is done, before the menu restarts */
void Mf_Then(const char *cmds);
/* the menu's getgamedirinfo: the n-th game directory of the base directory */
const char *Mf_GameDirInfo(int n, int prop);

/* the word filter (filter.txt) for chat when sv_chatfilter is on: text is
 * changed in place; true if anything was replaced */
bool Com_FilterText(char *text, size_t size);
void Mf_Init(void);

#endif
