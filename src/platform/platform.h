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
#else
static inline void Plat_Init(int *argc, char ***argv) { (void)argc, (void)argv; }
static inline const char *Plat_DefaultBaseDir(void) { return "."; }
static inline void Plat_Shutdown(void) {}
#endif

#endif
