/* Bulwark engine - start-up and shutdown that differ per platform.
 * ISC licence, see LICENSE. */
#ifndef BW_PLATFORM_H
#define BW_PLATFORM_H

#if defined(__3DS__) || defined(__NDS__)
/* brings up the system services, and on consoles (no command line) reads
 * the arguments from a file in the base directory: argv is replaced */
void Plat_Init(int *argc, char ***argv);
const char *Plat_DefaultBaseDir(void);
void Plat_Shutdown(void);
#elif defined(__EMSCRIPTEN__)
/* the browser (src/platform/web/sys_web.c and tools/web/shell): the game's
 * files are at /game, Bulwark's own data at /bulwark/data, and what the
 * game writes goes to /user, which the page keeps in IndexedDB */
void Plat_Init(int *argc, char ***argv);
static inline const char *Plat_DefaultBaseDir(void) { return "/game"; }
void Plat_Shutdown(void);
/* the game quit: the page shows that it ended */
void Plat_Quit(void);
/* the files under /user changed: the page saves them soon */
void Plat_UserFilesChanged(void);
#else
static inline void Plat_Init(int *argc, char ***argv) { (void)argc, (void)argv; }
static inline const char *Plat_DefaultBaseDir(void) { return "."; }
static inline void Plat_Shutdown(void) {}
#endif

#endif
