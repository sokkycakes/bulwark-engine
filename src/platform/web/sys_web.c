/* Bulwark engine - the browser: start-up, saving the player's files, and
 * quitting. The page around the engine (tools/web/shell) mounts the file
 * systems before main runs; this side tells it when to save and when the
 * game ended. ISC licence, see LICENSE. */
#include <emscripten.h>

#include "../platform.h"

/* clang-format off */
EM_JS(void, web_user_changed, (void), {
	if (Module.bulwarkUserChanged) Module.bulwarkUserChanged();
});
EM_JS(void, web_quit, (void), {
	if (Module.bulwarkQuit) Module.bulwarkQuit();
});
/* clang-format on */

void Plat_Init(int *argc, char ***argv) { (void)argc, (void)argv; }

void Plat_Shutdown(void) { web_user_changed(); }

void Plat_Quit(void) { web_quit(); }

void Plat_UserFilesChanged(void) { web_user_changed(); }
