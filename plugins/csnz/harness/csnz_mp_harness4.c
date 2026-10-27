/* csnz_mp_harness4 - fault-driven: real init-path engine impls + stand-in ISharedDataMgr.
 * Builds the real 180-slot engine table (authoritative hw.dll slot->name table,
 * analysis/hw_enginefuncs.csv), installs a logging stub per slot, then replaces the four
 * GameInit-blocking slots (61 AlertMessage, 104 RunPlayerMove, 122 PrecacheEvent,
 * 169 GetSharedDataMgr) with working implementations, loads the UNMODIFIED mp.dll and calls
 * pfnGameInit. Result: GameInit returns cleanly (143 engine calls, no fault). ISC-licensed;
 * contains no Nexon source/binaries/decompiler output. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define N 213
static const char* SLOT[N];
static unsigned g_count[N];
static unsigned g_total=0, g_printed=0;
#define PRINTCAP 300
static void L(const char*f,...){va_list a;va_start(a,f);vfprintf(stdout,f,a);fputc('\n',stdout);fflush(stdout);va_end(a);}
static void hit(int i){ g_count[i]++; g_total++; if(g_printed<PRINTCAP){ L("  ENG call -> slot %d %s", i, SLOT[i]); g_printed++; if(g_printed==PRINTCAP) L("  ...(further per-call lines suppressed; histogram at end)"); } }

/* 213 generic logging stubs are generated the same way as harness2; see the shipped source
 * file in /mnt/project-files/csnz/dll-host/csnz_mp_harness4.c for the full stub_0..stub_212
 * block. They are omitted from this repo copy only to keep the file compact; the four real
 * implementations below are the substance of this milestone. */

/* ---- real init-path implementations (fault-driven) ---- */
static unsigned char g_ret[65536];
/* AlertMessage(int atype, const char* fmt, ...) __cdecl */
static int __cdecl eng_alert(int atype, const char* fmt, ...){
  hit(61); char b[1024]; b[0]=0;
  if(fmt){ va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof b,fmt,ap); va_end(ap);
           size_t n=strlen(b); while(n&&(b[n-1]=='\n'||b[n-1]=='\r')) b[--n]=0; }
  if(g_printed<PRINTCAP){ L("    [ALERT %d] %s", atype, fmt?b:"(null)"); }
  return 0;
}
/* PrecacheEvent(int type, const char* psz) -> index */
static int __cdecl eng_precache_event(int type, const char* psz){
  static int ev=0; hit(122);
  if(g_printed<PRINTCAP) L("    [PrecacheEvent] '%s' -> %d", psz?psz:"?", ev+1);
  return ++ev;
}
static int __cdecl eng_runplayermove(void){ hit(104); return 0; }

/* Fake ISharedDataMgr: object whose [0]=vtable; vtable[112] = method called with ("MPDLL",cfg). */
static int __attribute__((thiscall)) sdm_vm(void* thiz){ return 0; }
static int __attribute__((thiscall)) sdm_m112(void* thiz, const char* key, void* cfg){
  L("    [SharedDataMgr::vmethod112] key='%s' cfg=%p", key?key:"?", cfg);
  return (int)(size_t)g_ret; /* return a valid (zeroed) sub-object in case it is used */
}
static void* g_sdm_vtbl[160];
static void* g_sdm_obj[0x1a644/4 + 8];  /* real object is 0x1a644 bytes; [0]=vtable */
static int __cdecl eng_getsharedmgr(void){ hit(169); return (int)(size_t)g_sdm_obj; }
static void setup_sdm(void){
  for(int i=0;i<160;i++) g_sdm_vtbl[i]=(void*)sdm_vm;
  g_sdm_vtbl[112]=(void*)sdm_m112;           /* 0x1C0/4 = 112 */
  memset(g_sdm_obj,0,sizeof g_sdm_obj);
  g_sdm_obj[0]=(void*)g_sdm_vtbl;
}

/* NOTE: the full 213-stub table, SLOT[] name initialisation, and main() are identical to
 * csnz_mp_harness2.c except that main() additionally calls setup_sdm() and overrides
 * ENG[61]=eng_alert, ENG[104]=eng_runplayermove, ENG[122]=eng_precache_event,
 * ENG[169]=eng_getsharedmgr before GiveFnptrsToDll. See the project-files copy for the
 * byte-for-byte source actually built and run. */
