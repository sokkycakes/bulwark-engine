/* Bulwark engine - the C++ exception entry points nxdk leaves out, for the
 * few PanoramaEngine files built with exceptions (package reading and
 * source cooking, see CMakeLists.txt). nxdk has no C++ exception runtime:
 * a throw stops the console with a message instead of unwinding.
 * ISC licence, see LICENSE. */
#include <windows.h>

void __stdcall _CxxThrowException(void *object, void *info)
{
	(void)object, (void)info;
	XBX_Log("C++ exception thrown: nxdk can't unwind it\n");
	XBX_FatalScreen("A C++ exception was thrown (PanoramaEngine).\nThe Xbox build can't catch exceptions.");
}

/* the frame handler of functions with try blocks or cleanups: no C++
 * exception ever reaches it (a throw stops above), so it passes every
 * other exception on (ExceptionContinueSearch) */
int __cdecl __CxxFrameHandler3(void *record, void *frame, void *context, void *dispatcher)
{
	(void)record, (void)frame, (void)context, (void)dispatcher;
	return 1;
}

void __cdecl __std_terminate(void)
{
	XBX_FatalScreen("std::terminate called (C++ runtime).");
}
