/* Bulwark engine - QuakeC to x86 machine code, for the interpreter to call.
 * ISC licence, see LICENSE.
 *
 * A function that has run a few times is translated statement by
 * statement into native code that works on the same globals and entity
 * fields, with the same checks. Anything unusual is handed back: a
 * statement the translator has no code for runs through the interpreter
 * (QC_JitStep), a failed check reruns its statement there so the error is
 * the interpreter's own. A call to another compiled function goes straight
 * to that function's entry, which makes the frame itself; other calls go
 * through QC_JitCall. Native code returns -1 once the function has
 * returned, or the statement the interpreter is to carry on from in the
 * same frame.
 *
 * pr_jit 0 turns it off; profiling, breakpoints and watchpoints use the
 * interpreter while they are on. */
#ifndef BW_QC_JIT_H
#define BW_QC_JIT_H

#include "qcvm.h"
#include "../common/cvar.h"

/* QC_JIT itself is decided in qcvm.h */

#ifdef QC_JIT
#ifdef __i386__
#define QC_JIT_CC __attribute__((cdecl))
#else
#define QC_JIT_CC
#endif
typedef int (QC_JIT_CC *qc_jitfn_t)(qcvm_t *vm);

/* qc_jit.c */
extern cvar_t *qc_jit_cvar;
void QC_JitInit(void);
/* translates f (of the running progs); NULL if it can't be */
qc_jitfn_t QC_JitCompile(qcvm_t *vm, qcfunc_t *f);
void QC_JitFree(qcvm_t *vm);

/* qc_exec.c, called from native code */
int QC_JIT_CC QC_JitStep(qcvm_t *vm, int s);
/* the same for a statement the loader fused with the ones after it, run
 * whole: returns the statement it carries on at */
int QC_JIT_CC QC_JitStepFused(qcvm_t *vm, int s);
int QC_JIT_CC QC_JitCall(qcvm_t *vm, int s);
int QC_JIT_CC QC_JitStrEq(qcvm_t *vm, int s);
int QC_JIT_CC QC_JitStrEmpty(qcvm_t *vm, int s);
/* the interpreter finishes a function native code handed back at statement s */
int QC_JIT_CC QC_JitFinish(qcvm_t *vm, int s);
/* after a builtin called from native code: back to progs caller, autocvars synced */
int QC_JIT_CC QC_JitBuiltinDone(qcvm_t *vm, int caller);

/* calls before a function is compiled: most that run only now and then
 * never are, which keeps the code (and memory) to what runs hot */
#define QC_JIT_AFTER 64
/* native calls nested deeper than this are interpreted, which takes no
 * more of the C stack for each */
#define QC_JIT_NEST 128
#endif

#endif
