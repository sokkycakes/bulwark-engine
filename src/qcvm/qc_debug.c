/* Bulwark engine - QuakeC debugging commands, for each of the server game
 * (ssqc), client game (csqc) and menu (menuqc):
 *   profile_<vm> [n]       start profiling; again to print the top n and stop
 *   coredump_<vm>          write the stack, globals and entities to <vm>core.txt
 *   extensionlist_<vm>     the extensions checkextension says yes to
 *   breakpoint_<vm> <fn>   print the stack whenever fn is entered
 *   watchpoint_<vm> <var>  report each change to a global or "entnum.field"
 *   poke_<vm> <var> [val]  show or set a global or "entnum.field"
 *   pr_dumpplatform [vm]   write the builtins and extensions to <vm>platform.txt
 * breakpoint, watchpoint and sv_enableprofile without a suffix mean the server.
 * ISC licence, see LICENSE. */
#include "qcvm.h"
#include "qc_internal.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct qc_watch_s {
	char name[96];
	int ent; /* -1 for a global */
	uint32_t ofs;
	int words;
	uint32_t type;
	qcval_t last[3];
} qc_watch_t;

static cvar_t *coreonerror[3];

static const char *const vm_suffix[] = {"ssqc", "csqc", "menuqc"};

static qcvm_t *find_vm(qcvm_kind_t kind)
{
	for (int i = 0; i < QC_VMCount(); i++)
		if (QC_VMAt(i) && QC_VMAt(i)->kind == kind)
			return QC_VMAt(i);
	return NULL;
}

/* the VM a command is for, from its suffix */
static qcvm_t *cmd_vm(const char *cmd, qcvm_kind_t *kind)
{
	qcvm_kind_t k = QCVM_SERVER;
	qcvm_t *vm;
	if (strstr(cmd, "csqc"))
		k = QCVM_CLIENT;
	else if (strstr(cmd, "menu"))
		k = QCVM_MENU;
	if (kind)
		*kind = k;
	vm = find_vm(k);
	if (!vm)
		Con_Printf("%s isn't running\n", vm_suffix[k]);
	return vm;
}

void QC_DebugUpdateHooks(qcvm_t *vm)
{
	vm->debug_hooks = vm->profiling || vm->breakpoint[0] || vm->watch;
}

/* --- values by name ---------------------------------------------------- */

static int type_words(uint32_t type) { return (type & ~DEF_SAVEGLOBAL) == ev_vector ? 3 : 1; }

static void value_string(qcvm_t *vm, uint32_t type, const qcval_t *v, char *out, size_t outsize)
{
	switch (type & ~DEF_SAVEGLOBAL) {
	case ev_string: snprintf(out, outsize, "\"%s\"", QC_GetString(vm, v->i)); break;
	case ev_float: snprintf(out, outsize, "%g", v->f); break;
	case ev_vector: snprintf(out, outsize, "'%g %g %g'", v[0].f, v[1].f, v[2].f); break;
	case ev_entity: snprintf(out, outsize, "entity %d", v->i); break;
	case ev_function: snprintf(out, outsize, "%s()", v->i ? QC_FunctionName(vm, v->i) : "null"); break;
	default: snprintf(out, outsize, "%d", v->i); break;
	}
}

/* "name" is a global; "12.health" is entity 12's health */
static qcval_t *resolve(qcvm_t *vm, const char *name, uint32_t *type, int *ent, uint32_t *ofs)
{
	const char *dot = strchr(name, '.');
	if (dot && dot > name && strspn(name, "0123456789") == (size_t)(dot - name)) {
		int e = atoi(name);
		qcdef_t *d = QC_FindField(vm, dot + 1);
		if (!d || e < 0 || e >= vm->num_edicts)
			return NULL;
		*type = d->type;
		*ent = e;
		*ofs = d->ofs;
		return QC_EdictFields(vm, e) + d->ofs;
	} else {
		qcdef_t *d = QC_FindGlobal(vm, name);
		if (!d)
			return NULL;
		*type = d->type;
		*ent = -1;
		*ofs = d->ofs;
		return vm->globals + d->ofs;
	}
}

static qcval_t *watch_ptr(qcvm_t *vm, qc_watch_t *w)
{
	if (w->ent < 0)
		return vm->progs[0].globals + w->ofs;
	if (w->ent >= vm->num_edicts)
		return NULL;
	return QC_EdictFields(vm, w->ent) + w->ofs;
}

/* --- interpreter hooks ------------------------------------------------- */

void QC_DebugFunctionEntered(qcvm_t *vm, qcfunc_t *f)
{
	if (vm->profiling)
		f->prof_calls++;
	if (vm->breakpoint[0] && f->name && !strcmp(f->name, vm->breakpoint)) {
		Con_Printf("^3%s: breakpoint at %s (%s), self %d\n", vm->name, f->name, f->file ? f->file : "?",
		           QC_GInt(vm, vm->gofs.self));
		QC_StackTrace(vm);
	}
}

void QC_DebugStatement(qcvm_t *vm, int s)
{
	if (vm->profiling && vm->xfunction > 0)
		vm->functions[vm->xfunction].prof_statements++;
	if (vm->watch) {
		qc_watch_t *w = vm->watch;
		qcval_t *v = watch_ptr(vm, w);
		if (v && memcmp(v, w->last, sizeof(qcval_t) * w->words)) {
			char before[256], after[256];
			qcfunc_t *f = &vm->functions[vm->xfunction];
			value_string(vm, w->type, w->last, before, sizeof(before));
			value_string(vm, w->type, v, after, sizeof(after));
			/* the statement before this one made the change */
			Con_Printf("^3%s: %s changed from %s to %s in %s (%s) before statement %d\n", vm->name, w->name, before,
			           after, f->name ? f->name : "?", f->file ? f->file : "?", s);
			memcpy(w->last, v, sizeof(qcval_t) * w->words);
		}
	}
}

/* --- profile ----------------------------------------------------------- */

typedef struct {
	qcfunc_t *f;
	const char *progs;
} profrow_t;

static int prof_cmp(const void *a, const void *b)
{
	const qcfunc_t *x = ((const profrow_t *)a)->f, *y = ((const profrow_t *)b)->f;
	/* builtins rank by time, at a rough ten nanoseconds a statement */
	double cx = x->builtin ? x->prof_time * 1e8 : (double)x->prof_statements;
	double cy = y->builtin ? y->prof_time * 1e8 : (double)y->prof_statements;
	return cx < cy ? 1 : cx > cy ? -1 : 0;
}

static void profile_reset(qcvm_t *vm)
{
	for (int p = 0; p < vm->numprogs; p++)
		for (int i = 0; i < vm->progs[p].numfunctions; i++) {
			qcfunc_t *f = &vm->progs[p].functions[i];
			f->prof_statements = f->prof_calls = 0;
			f->prof_time = 0;
		}
}

static void profile_print(qcvm_t *vm, int top)
{
	int n = 0, cap = 0;
	profrow_t *rows = NULL;
	uint64_t total = 0;
	for (int p = 0; p < vm->numprogs; p++)
		for (int i = 1; i < vm->progs[p].numfunctions; i++) {
			qcfunc_t *f = &vm->progs[p].functions[i];
			if (!f->prof_calls && !f->prof_statements)
				continue;
			if (n == cap) {
				cap = cap ? cap * 2 : 256;
				rows = Mem_Realloc(rows, sizeof(*rows) * (size_t)cap);
			}
			rows[n].f = f;
			rows[n++].progs = vm->progs[p].name;
			total += f->prof_statements;
		}
	qsort(rows, (size_t)n, sizeof(*rows), prof_cmp);
	Con_Printf("%s profile over %.1f seconds: %llu statements in %d functions\n", vm->name,
	           Sys_Seconds() - vm->profile_start, (unsigned long long)total, n);
	Con_Printf("  %12s %6s %10s  %s\n", "statements", "%", "calls", "function");
	for (int i = 0; i < n && i < top; i++) {
		qcfunc_t *f = rows[i].f;
		if (f->builtin)
			Con_Printf("  %9.3f ms %6s %10llu  %s (builtin)\n", f->prof_time * 1000, "",
			           (unsigned long long)f->prof_calls, f->name);
		else
			Con_Printf("  %12llu %5.1f%% %10llu  %s (%s)\n", (unsigned long long)f->prof_statements,
			           total ? 100.0 * (double)f->prof_statements / (double)total : 0.0,
			           (unsigned long long)f->prof_calls, f->name, f->file ? f->file : "?");
	}
	Mem_Free(rows);
}

static void Profile_f(int argc, const char **argv)
{
	qcvm_t *vm = cmd_vm(argv[0], NULL);
	int top = argc > 1 ? atoi(argv[1]) : 30;
	if (!vm)
		return;
	if (!vm->profiling) {
		profile_reset(vm);
		vm->profiling = true;
		vm->profile_start = Sys_Seconds();
		Con_Printf("profiling %s; %s again to see the results\n", vm->name, argv[0]);
	} else {
		profile_print(vm, top > 0 ? top : 30);
		vm->profiling = false;
	}
	QC_DebugUpdateHooks(vm);
}

/* --- coredump ---------------------------------------------------------- */

static void write_frame(FILE *f, qcvm_t *vm, int progs, int fn, int s)
{
	qcprogs_t *p = &vm->progs[progs];
	if (fn > 0 && fn < p->numfunctions)
		fprintf(f, "  %s (%s) statement %d\n", p->functions[fn].name, p->functions[fn].file, s);
}

void QC_CoreDump(qcvm_t *vm, const char *why)
{
	char rel[64], path[1024], buf[512];
	int k = vm->kind <= QCVM_MENU ? (int)vm->kind : 0;
	FILE *f;
	/* errors only dump when asked to */
	if (why && (vm->kind > QCVM_MENU || !coreonerror[k] || !coreonerror[k]->ivalue))
		return;
	snprintf(rel, sizeof(rel), "%score.txt", vm_suffix[k]);
	if (!FS_WritePath(rel, path, sizeof(path)) || !(f = fopen(path, "w"))) {
		Con_Printf("can't write %s\n", rel);
		return;
	}
	fprintf(f, "%s core dump%s%s\n\nstack:\n", vm->name, why ? ": " : "", why ? why : "");
	write_frame(f, vm, vm->curprogs, vm->xfunction, vm->xstatement);
	for (int i = vm->depth - 1; i >= 0; i--)
		write_frame(f, vm, vm->stack[i].progs, vm->stack[i].f, vm->stack[i].s);
	fprintf(f, "\nglobals:\n");
	for (int i = 0; i < vm->numglobaldefs; i++) {
		qcdef_t *d = &vm->globaldefs[i];
		uint32_t t = d->type & ~DEF_SAVEGLOBAL;
		if (!d->name || !*d->name || t == ev_void || t == ev_field || t > ev_integer)
			continue;
		value_string(vm, d->type, vm->globals + d->ofs, buf, sizeof(buf));
		fprintf(f, "  %-32s %s\n", d->name, buf);
	}
	fprintf(f, "\nentities:\n");
	for (int e = 0; e < vm->num_edicts; e++) {
		if (vm->edicts[e].free)
			continue;
		fprintf(f, "entity %d\n", e);
		for (int i = 1; i < vm->numfielddefs; i++) {
			qcdef_t *d = &vm->fielddefs[i];
			size_t n = d->name ? strlen(d->name) : 0;
			if (!n || !strncmp(d->name, "::", 2) || (n > 2 && d->name[n - 2] == '_' && strchr("xyz", d->name[n - 1])))
				continue;
			QC_FieldToString(vm, e, d, buf, sizeof(buf));
			if (!buf[0] || !strcmp(buf, "0") || !strcmp(buf, "0 0 0"))
				continue;
			fprintf(f, "  %-32s %s\n", d->name, buf);
		}
	}
	fclose(f);
	Con_Printf("wrote %s\n", rel);
}

static void Coredump_f(int argc, const char **argv)
{
	qcvm_t *vm = cmd_vm(argv[0], NULL);
	if (vm)
		QC_CoreDump(vm, NULL);
}

/* --- extensions and platform ------------------------------------------- */

static void Extensionlist_f(int argc, const char **argv)
{
	int n = 0;
	if (!cmd_vm(argv[0], NULL))
		return;
	for (int i = 0; qc_supported_extensions[i]; i++, n++)
		Con_Printf("  %s\n", qc_supported_extensions[i]);
	Con_Printf("%d extensions\n", n);
}

static void Dumpplatform_f(int argc, const char **argv)
{
	const char *which = argc > 1 ? argv[1] : "ssqc";
	qcvm_t *vm = cmd_vm(which, NULL);
	char rel[64], path[1024];
	int n = 0;
	FILE *f;
	if (!vm)
		return;
	snprintf(rel, sizeof(rel), "%splatform.txt", strstr(which, "csqc") ? "csqc" : strstr(which, "menu") ? "menuqc" : "ssqc");
	if (!FS_WritePath(rel, path, sizeof(path)) || !(f = fopen(path, "w"))) {
		Con_Printf("can't write %s\n", rel);
		return;
	}
	fprintf(f, "// %s: the extensions and builtins this engine has\n\n", vm->name);
	for (int i = 0; qc_supported_extensions[i]; i++)
		fprintf(f, "// extension %s\n", qc_supported_extensions[i]);
	fprintf(f, "\n");
	for (int t = 0; vm->tables && vm->tables[t]; t++)
		for (const qc_builtin_t *b = vm->tables[t]; b->name; b++, n++)
			fprintf(f, "builtin %s\n", b->name);
	fclose(f);
	Con_Printf("wrote %s (%d builtins)\n", rel, n);
}

/* --- breakpoints, watchpoints and poke --------------------------------- */

static void Breakpoint_f(int argc, const char **argv)
{
	qcvm_t *vm = cmd_vm(argv[0], NULL);
	if (!vm)
		return;
	if (argc < 2) {
		if (vm->breakpoint[0])
			Con_Printf("breakpoint cleared (%s)\n", vm->breakpoint);
		vm->breakpoint[0] = 0;
	} else if (!QC_FindFunction(vm, argv[1])) {
		Con_Printf("%s has no function %s\n", vm->name, argv[1]);
		return;
	} else {
		Str_Copy(vm->breakpoint, argv[1], sizeof(vm->breakpoint));
		Con_Printf("breakpoint on %s; %s with no name clears it\n", argv[1], argv[0]);
	}
	QC_DebugUpdateHooks(vm);
}

static void Watchpoint_f(int argc, const char **argv)
{
	qcvm_t *vm = cmd_vm(argv[0], NULL);
	qc_watch_t *w;
	qcval_t *v;
	if (!vm)
		return;
	Mem_Free(vm->watch);
	vm->watch = NULL;
	QC_DebugUpdateHooks(vm);
	if (argc < 2) {
		Con_Printf("watchpoint cleared\n");
		return;
	}
	w = Mem_Alloc(sizeof(*w));
	v = resolve(vm, argv[1], &w->type, &w->ent, &w->ofs);
	if (!v) {
		Con_Printf("%s has no global or entity field \"%s\"\n", vm->name, argv[1]);
		Mem_Free(w);
		return;
	}
	Str_Copy(w->name, argv[1], sizeof(w->name));
	w->words = type_words(w->type);
	memcpy(w->last, v, sizeof(qcval_t) * w->words);
	vm->watch = w;
	QC_DebugUpdateHooks(vm);
	Con_Printf("watching %s\n", argv[1]);
}

static void Poke_f(int argc, const char **argv)
{
	qcvm_t *vm = cmd_vm(argv[0], NULL);
	uint32_t type, ofs;
	int ent;
	qcval_t *v;
	char buf[512];
	if (!vm)
		return;
	if (argc < 2) {
		Con_Printf("usage: %s <global or entnum.field> [value]\n", argv[0]);
		return;
	}
	if (!(v = resolve(vm, argv[1], &type, &ent, &ofs))) {
		Con_Printf("%s has no global or entity field \"%s\"\n", vm->name, argv[1]);
		return;
	}
	if (argc > 2) {
		const char *s = argv[2];
		switch (type & ~DEF_SAVEGLOBAL) {
		case ev_string: v->i = QC_NewString(vm, s); break;
		case ev_float: v->f = (float)atof(s); break;
		case ev_vector: sscanf(*s == '\'' ? s + 1 : s, "%f %f %f", &v[0].f, &v[1].f, &v[2].f); break;
		case ev_function: v->i = QC_FindFunction(vm, s); break;
		default: v->i = (int32_t)strtol(s, NULL, 0); break;
		}
	}
	/* a watch on the same value shouldn't report the poke as the game's doing */
	if (vm->watch && vm->watch->ent == ent && vm->watch->ofs == ofs)
		memcpy(vm->watch->last, v, sizeof(qcval_t) * vm->watch->words);
	value_string(vm, type, v, buf, sizeof(buf));
	Con_Printf("%s = %s\n", argv[1], buf);
}

void QC_InitDebugCommands(void)
{
	static const char *const descs[3] = {"the server game", "the client game", "the menu"};
	coreonerror[0] = Cvar_Get("pr_ssqc_coreonerror", "0", CVAR_ARCHIVE, "Write ssqccore.txt when the server game has an error");
	coreonerror[1] = Cvar_Get("pr_csqc_coreonerror", "0", CVAR_ARCHIVE, "Write csqccore.txt when the client game has an error");
	coreonerror[2] = Cvar_Get("pr_menuqc_coreonerror", "0", CVAR_ARCHIVE, "Write menuqccore.txt when the menu has an error");
	for (int k = 0; k < 3; k++) {
		char name[64], desc[160];
		snprintf(name, sizeof(name), "profile_%s", vm_suffix[k]);
		snprintf(desc, sizeof(desc), "%s [n]: profile %s; again to show the top n functions", name, descs[k]);
		Cmd_AddD(Mem_Strdup(name), Profile_f, Mem_Strdup(desc));
		snprintf(name, sizeof(name), "coredump_%s", vm_suffix[k]);
		snprintf(desc, sizeof(desc), "Write the stack, globals and entities of %s to %score.txt", descs[k], vm_suffix[k]);
		Cmd_AddD(Mem_Strdup(name), Coredump_f, Mem_Strdup(desc));
		snprintf(name, sizeof(name), "extensionlist_%s", vm_suffix[k]);
		snprintf(desc, sizeof(desc), "The QC extensions %s can use", descs[k]);
		Cmd_AddD(Mem_Strdup(name), Extensionlist_f, Mem_Strdup(desc));
		snprintf(name, sizeof(name), "breakpoint_%s", vm_suffix[k]);
		snprintf(desc, sizeof(desc), "%s [function]: print the stack each time %s enters the function", name, descs[k]);
		Cmd_AddD(Mem_Strdup(name), Breakpoint_f, Mem_Strdup(desc));
		snprintf(name, sizeof(name), "watchpoint_%s", vm_suffix[k]);
		snprintf(desc, sizeof(desc), "%s [global or entnum.field]: report each change in %s", name, descs[k]);
		Cmd_AddD(Mem_Strdup(name), Watchpoint_f, Mem_Strdup(desc));
		snprintf(name, sizeof(name), "poke_%s", vm_suffix[k]);
		snprintf(desc, sizeof(desc), "%s <global or entnum.field> [value]: show or set a value in %s", name, descs[k]);
		Cmd_AddD(Mem_Strdup(name), Poke_f, Mem_Strdup(desc));
	}
	Cmd_AddD("breakpoint", Breakpoint_f, "breakpoint [function]: breakpoint_ssqc");
	Cmd_AddD("watchpoint", Watchpoint_f, "watchpoint [global or entnum.field]: watchpoint_ssqc");
	Cmd_AddD("sv_enableprofile", Profile_f, "profile_ssqc");
	Cmd_AddD("pr_dumpplatform", Dumpplatform_f, "pr_dumpplatform [ssqc|csqc|menuqc]: write the builtins and extensions to a file");
}
