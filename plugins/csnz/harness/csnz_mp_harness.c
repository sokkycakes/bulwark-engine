/* csnz_mp_harness — smallest viable execution milestone for CSNZ mp.dll.
 *
 * Loads the UNMODIFIED Nexon mp.dll (build 0.0.18845.1), lets the PE loader run its
 * relocations/imports/TLS callbacks/DllMain + MSVC static initializers, supplies a
 * compatibility enginefuncs_t (213 slots) and a globalvars_t, then performs the first
 * verified call into Nexon's own machine code: GiveFnptrsToDll. Nothing in mp.dll is
 * modified or patched; we only *supply* the interfaces the engine normally would.
 *
 * Binary-verified contract (disassembled from mp.dll, image base 0x10000000):
 *   GiveFnptrsToDll    @RVA 0x1490720  __stdcall(enginefuncs_t*, globalvars_t*)
 *                        -> rep movs of 0xD5 (213) dwords = 852 bytes from arg1; stores arg2 as gpGlobals.
 *   GetEntityAPI       @RVA 0x13E86E0  __cdecl(DLL_FUNCTIONS*, int version)
 *                        -> requires version == 0x8C (140); copies 0x4A (74) dwords = 296 bytes;
 *                           then registers entity classes (may call back through our engine table).
 *   GetNewDLLFunctions @RVA 0x13E87F0  __cdecl(NEW_DLL_FUNCTIONS*, int* piVersion)
 *                        -> requires *piVersion == 1; copies 0x14 (20) bytes = 5 pointers.
 *
 * Build: i686-w64-mingw32-gcc -O2 -municode -o csnz_mp_harness.exe csnz_mp_harness.c
 * Run  : wine csnz_mp_harness.exe   (cwd must be the Bin/ folder so sibling runtime DLLs resolve)
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define ENG_SLOTS_USED   213   /* 0xD5  — dwords GiveFnptrsToDll copies  */
#define ENG_SLOTS_ALLOC  256   /* over-allocate so the copy can never overrun our buffer */
#define DLLFUNCS_DWORDS  74    /* 0x4A */
#define IFACE_VERSION    140   /* 0x8C */

static int g_engcalls = 0;     /* how many times mp.dll called back into our stubs */

/* Generic __cdecl engine-function stub. GoldSrc enginefuncs_t members are __cdecl.
 * GiveFnptrsToDll only COPIES these pointers (it never calls them), so for the core
 * milestone they are inert; they exist so the optional GetEntityAPI pass, which can
 * call back during entity-class registration, lands on valid code instead of garbage. */
static int __cdecl eng_stub(void) { ++g_engcalls; return 0; }

typedef void (__stdcall *GiveFn_t)(void *pengfuncs, void *pGlobals);
typedef int  (__cdecl   *GetEntityAPI_t)(void *pFunctionTable, int interfaceVersion);
typedef int  (__cdecl   *GetNewDLLFunctions_t)(void *pFunctionTable, int *piVersion);

static void L(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stdout, fmt, ap); fputc('\n', stdout); fflush(stdout);
    va_end(ap);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    L("[FAULT] exception 0x%08lx at eip=0x%p (engine-stub callbacks so far: %d)",
      ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress, g_engcalls);
    fflush(stdout); return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv) {
    const char *dll = (argc > 1) ? argv[1] : "mp.dll";
    int do_entityapi = 0;
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "--entityapi")) do_entityapi = 1;

    setvbuf(stdout, NULL, _IONBF, 0);
    SetUnhandledExceptionFilter(crash_filter);

    L("=== csnz_mp_harness ===");
    L("step 1: LoadLibraryA(\"%s\")  (runs relocations, imports, TLS callbacks, DllMain, static init)", dll);
    HMODULE h = LoadLibraryA(dll);
    if (!h) { L("  FAIL: LoadLibraryA error=%lu", GetLastError()); return 2; }
    L("  OK: mp.dll mapped at %p  (its init machine code has now executed)", (void*)h);

    GiveFn_t             GiveFnptrsToDll    = (GiveFn_t)            GetProcAddress(h, "GiveFnptrsToDll");
    GetEntityAPI_t       GetEntityAPI       = (GetEntityAPI_t)      GetProcAddress(h, "GetEntityAPI");
    GetNewDLLFunctions_t GetNewDLLFunctions = (GetNewDLLFunctions_t)GetProcAddress(h, "GetNewDLLFunctions");
    L("step 2: resolved exports  GiveFnptrsToDll=%p  GetEntityAPI=%p  GetNewDLLFunctions=%p",
      (void*)GiveFnptrsToDll, (void*)GetEntityAPI, (void*)GetNewDLLFunctions);
    if (!GiveFnptrsToDll) { L("  FAIL: GiveFnptrsToDll not exported"); return 3; }

    /* Compatibility engine table: 213 slots required, 256 allocated. Every slot -> logging stub. */
    static void *engfuncs[ENG_SLOTS_ALLOC];
    for (int i = 0; i < ENG_SLOTS_ALLOC; ++i) engfuncs[i] = (void*)eng_stub;
    /* globalvars_t: GiveFnptrsToDll stores only the pointer; give it a generous zeroed block. */
    static unsigned char globals[8192];
    memset(globals, 0, sizeof globals);

    L("step 3: THE VERIFIED CALL -> GiveFnptrsToDll(engfuncs=%p [%d slots], globals=%p)",
      (void*)engfuncs, ENG_SLOTS_USED, (void*)globals);
    GiveFnptrsToDll(engfuncs, globals);
    L("  OK: GiveFnptrsToDll returned cleanly. Nexon's rep-movs of %d dwords executed; "
      "engine table + gpGlobals now installed inside mp.dll.", ENG_SLOTS_USED);

    if (GetNewDLLFunctions) {
        void *newfuncs[8]; memset(newfuncs, 0, sizeof newfuncs);
        int ver = 1;
        int r = GetNewDLLFunctions(newfuncs, &ver);
        L("step 4: GetNewDLLFunctions(&v=1) => %d (ver now %d); slot0=%p slot4=%p",
          r, ver, newfuncs[0], newfuncs[4]);
    }

    if (do_entityapi && GetEntityAPI) {
        static unsigned char dllfuncs[DLLFUNCS_DWORDS*4 + 64]; memset(dllfuncs, 0, sizeof dllfuncs);
        L("step 5: GetEntityAPI(table, %d)  (registers entity classes; may call our stubs)", IFACE_VERSION);
        int r = GetEntityAPI(dllfuncs, IFACE_VERSION);
        L("  GetEntityAPI => %d; first slot=%p; engine-stub callbacks during registration: %d",
          r, *(void**)dllfuncs, g_engcalls);
    }

    L("=== done: original mp.dll machine code was loaded, initialized, and called ===");
    return 0;
}
