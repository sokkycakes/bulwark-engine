/* Bulwark engine - SQL databases for server QC (the sql* builtins), on
 * SQLite through the system's libsqlite3 (public domain), loaded at run
 * time. A database is a file under the game directory's sqlite/ folder,
 * or ":memory:".
 *
 * Queries run on the worker threads. When one finishes, its callback runs
 * on the main thread with every row at once:
 *   callback(serveridx, queryidx, rows, columns, eof = 1, firstrow = 1)
 * and the rows stay readable with sqlreadfield until sqlclosequery or the
 * connection goes. Query type 2 runs without calling back. Row -1 reads a
 * column's name. ISC licence, see LICENSE. */
#include "sv_local.h"
#include "../common/cmd.h"
#include "../common/fs.h"
#include "../common/worker.h"

#include "../common/sys_dl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONNECTIONS 16
#define MAX_QUERIES 64
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_OPEN_READWRITE 0x2
#define SQLITE_OPEN_CREATE 0x4
#define SQLITE_OPEN_FULLMUTEX 0x10000
#define SQLITE_TRANSIENT ((void (*)(void *))-1)

static struct {
	bool tried, ok;
	int (*open_v2)(const char *name, void **db, int flags, const char *vfs);
	int (*close_v2)(void *db);
	int (*prepare_v2)(void *db, const char *sql, int n, void **stmt, const char **tail);
	int (*step)(void *stmt);
	int (*finalize)(void *stmt);
	int (*column_count)(void *stmt);
	const unsigned char *(*column_text)(void *stmt, int i);
	const void *(*column_blob)(void *stmt, int i);
	int (*column_bytes)(void *stmt, int i);
	const char *(*column_name)(void *stmt, int i);
	const char *(*errmsg)(void *db);
	const char *(*libversion)(void);
} lib;

typedef struct {
	bool used, ready;
	int func;           /* the callback, 0 for none */
	int type;
	char *sql;
	int rows, cols;
	char **names;       /* cols */
	char **cells;       /* rows * cols, NUL-terminated copies */
	int *sizes;         /* rows * cols, the bytes in each (for blobs) */
	char error[256];
} query_t;

typedef struct {
	bool used, dead;    /* dead: killed while queries were still running */
	void *db;
	char name[128];
	char error[256];
	int inflight;
	query_t queries[MAX_QUERIES];
} conn_t;

static conn_t *conns; /* MAX_CONNECTIONS, made by the first sqlconnect */

static bool load_sqlite(void)
{
	static const char *const names[] = {"libsqlite3.so.0", "libsqlite3.so", "libsqlite3.dylib", "sqlite3.dll", NULL};
	void *h = NULL;
	if (lib.tried)
		return lib.ok;
	lib.tried = true;
	for (int i = 0; names[i] && !h; i++)
		h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
	if (!h)
		return false;
	lib.ok = true;
#define S(f, n)                                \
	do {                                       \
		*(void **)&lib.f = dlsym(h, n);        \
		lib.ok &= lib.f != NULL;               \
	} while (0)
	S(open_v2, "sqlite3_open_v2");
	S(close_v2, "sqlite3_close_v2");
	S(prepare_v2, "sqlite3_prepare_v2");
	S(step, "sqlite3_step");
	S(finalize, "sqlite3_finalize");
	S(column_count, "sqlite3_column_count");
	S(column_text, "sqlite3_column_text");
	S(column_blob, "sqlite3_column_blob");
	S(column_bytes, "sqlite3_column_bytes");
	S(column_name, "sqlite3_column_name");
	S(errmsg, "sqlite3_errmsg");
	S(libversion, "sqlite3_libversion");
#undef S
	return lib.ok;
}

static void free_query(query_t *q)
{
	for (int i = 0; i < q->rows * q->cols; i++)
		Mem_Free(q->cells[i]);
	for (int i = 0; i < q->cols; i++)
		Mem_Free(q->names[i]);
	Mem_Free(q->cells);
	Mem_Free(q->names);
	Mem_Free(q->sizes);
	Mem_Free(q->sql);
	memset(q, 0, sizeof(*q));
}

static void close_conn(conn_t *c)
{
	if (c->inflight) {
		c->dead = true; /* the last query to finish closes it */
		return;
	}
	for (int i = 0; i < MAX_QUERIES; i++)
		if (c->queries[i].used)
			free_query(&c->queries[i]);
	if (c->db)
		lib.close_v2(c->db);
	memset(c, 0, sizeof(*c));
}

static conn_t *get_conn(float idx)
{
	int i = (int)idx;
	if (!conns || i < 0 || i >= MAX_CONNECTIONS || !conns[i].used || conns[i].dead)
		return NULL;
	return &conns[i];
}

static query_t *get_query(conn_t *c, float idx)
{
	int i = (int)idx;
	if (!c || i < 0 || i >= MAX_QUERIES || !c->queries[i].used || !c->queries[i].ready)
		return NULL;
	return &c->queries[i];
}

/* --- running queries ------------------------------------------------------------------ */

typedef struct {
	conn_t *conn;
	int query;
} job_t;

static void query_work(void *ctx)
{
	job_t *j = ctx;
	query_t *q = &j->conn->queries[j->query];
	void *stmt = NULL;
	const char *sql = q->sql;
	int cap = 0;
	/* several statements run in turn; the last one's rows are the result */
	while (sql && *sql) {
		const char *tail = NULL;
		if (lib.prepare_v2(j->conn->db, sql, -1, &stmt, &tail) != 0) {
			snprintf(q->error, sizeof(q->error), "%s", lib.errmsg(j->conn->db));
			return;
		}
		sql = tail;
		if (!stmt)
			continue; /* whitespace or a comment */
		int cols = lib.column_count(stmt), r;
		if (cols) {
			for (int i = 0; i < q->rows * q->cols; i++)
				Mem_Free(q->cells[i]);
			for (int i = 0; i < q->cols; i++)
				Mem_Free(q->names[i]);
			Mem_Free(q->names);
			q->rows = 0;
			q->cols = cols;
			q->names = Mem_Alloc(sizeof(char *) * (size_t)cols);
			for (int i = 0; i < cols; i++)
				q->names[i] = Mem_Strdup(lib.column_name(stmt, i));
		}
		while ((r = lib.step(stmt)) == SQLITE_ROW) {
			if (!cols)
				continue;
			if ((q->rows + 1) * cols > cap) {
				cap = BW_MAX(cap * 2, (q->rows + 1) * cols);
				q->cells = Mem_Realloc(q->cells, sizeof(char *) * (size_t)cap);
				q->sizes = Mem_Realloc(q->sizes, sizeof(int) * (size_t)cap);
			}
			for (int i = 0; i < cols; i++) {
				const void *b = lib.column_blob(stmt, i);
				int n = lib.column_bytes(stmt, i);
				char *cell = Mem_Alloc((size_t)n + 1);
				if (b && n)
					memcpy(cell, b, (size_t)n);
				cell[n] = 0;
				q->cells[q->rows * cols + i] = cell;
				q->sizes[q->rows * cols + i] = n;
			}
			q->rows++;
		}
		if (r != SQLITE_DONE)
			snprintf(q->error, sizeof(q->error), "%s", lib.errmsg(j->conn->db));
		lib.finalize(stmt);
		if (r != SQLITE_DONE)
			return;
	}
}

static void query_done(void *ctx)
{
	job_t *j = ctx;
	conn_t *c = j->conn;
	query_t *q = &c->queries[j->query];
	int func = q->func;
	c->inflight--;
	q->ready = true;
	if (q->error[0])
		Str_Copy(c->error, q->error, sizeof(c->error));
	Mem_Free(j);
	if (c->dead) {
		close_conn(c);
		return;
	}
	if (func && sv.vm && q->type != 2) {
		qcvm_t *vm = sv.vm;
		QC_Parm(vm, 0)->f = (float)(c - conns);
		QC_Parm(vm, 1)->f = (float)(q - c->queries);
		QC_Parm(vm, 2)->f = (float)q->rows;
		QC_Parm(vm, 3)->f = (float)q->cols;
		QC_Parm(vm, 4)->f = 1;
		QC_Parm(vm, 5)->f = 1;
		vm->argc = 6;
		QC_Execute(vm, func);
	}
}

/* --- builtins ------------------------------------------------------------------------- */

/* sqlconnect(host, user, pass, defaultdb, driver) */
static void bi_sqlconnect(qcvm_t *vm)
{
	const char *db = vm->argc > 3 ? QC_PSTR(vm, 3) : "", *driver = vm->argc > 4 ? QC_PSTR(vm, 4) : "";
	char rel[160], path[1024];
	QC_ReturnFloat(vm, -1);
	if (driver[0] && Str_ICmp(driver, "sqlite")) {
		Con_Printf("sqlconnect: no %s driver (only sqlite)\n", driver);
		return;
	}
	if (!load_sqlite()) {
		Con_Printf("sqlconnect: sqlite isn't installed\n");
		return;
	}
	if (!conns)
		conns = Mem_Alloc(sizeof(conn_t) * MAX_CONNECTIONS);
	if (!db[0])
		db = "default";
	if (!strcmp(db, ":memory:"))
		Str_Copy(path, db, sizeof(path));
	else {
		snprintf(rel, sizeof(rel), "sqlite/%s%s", db, strchr(db, '.') ? "" : ".db");
		if (!FS_SafeName(rel) || !FS_WritePath(rel, path, sizeof(path)) || !FS_CreatePath(path)) {
			Con_Printf("sqlconnect: bad database name %s\n", db);
			return;
		}
	}
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		conn_t *c = &conns[i];
		if (c->used)
			continue;
		if (lib.open_v2(path, &c->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != 0) {
			Con_Printf("sqlconnect: can't open %s: %s\n", db, c->db ? lib.errmsg(c->db) : "out of memory");
			if (c->db)
				lib.close_v2(c->db);
			c->db = NULL;
			return;
		}
		c->used = true;
		Str_Copy(c->name, db, sizeof(c->name));
		QC_ReturnFloat(vm, (float)i);
		return;
	}
	Con_Printf("sqlconnect: too many connections\n");
}

static void bi_sqldisconnect(qcvm_t *vm)
{
	conn_t *c = get_conn(QC_PFLOAT(vm, 0));
	if (c)
		close_conn(c);
}

/* sqlopenquery(serveridx, callback, querytype, query): the query's index, or -1 */
static void bi_sqlopenquery(qcvm_t *vm)
{
	conn_t *c = get_conn(QC_PFLOAT(vm, 0));
	QC_ReturnFloat(vm, -1);
	if (!c)
		return;
	for (int i = 0; i < MAX_QUERIES; i++) {
		query_t *q = &c->queries[i];
		if (q->used)
			continue;
		q->used = true;
		q->func = QC_PINT(vm, 1);
		q->type = (int)QC_PFLOAT(vm, 2);
		q->sql = Mem_Strdup(QC_PSTR(vm, 3));
		job_t *j = Mem_Alloc(sizeof(*j));
		j->conn = c;
		j->query = i;
		c->inflight++;
		QC_ReturnFloat(vm, (float)i);
		Worker_Add(query_work, query_done, j);
		return;
	}
	Str_Copy(c->error, "too many open queries", sizeof(c->error));
}

static void bi_sqlclosequery(qcvm_t *vm)
{
	query_t *q = get_query(get_conn(QC_PFLOAT(vm, 0)), QC_PFLOAT(vm, 1));
	if (q)
		free_query(q);
}

static const char *cell(qcvm_t *vm, int *size)
{
	query_t *q = get_query(get_conn(QC_PFLOAT(vm, 0)), QC_PFLOAT(vm, 1));
	int row = (int)QC_PFLOAT(vm, 2), col = (int)QC_PFLOAT(vm, 3);
	*size = 0;
	if (!q || col < 0 || col >= q->cols || row < -1 || row >= q->rows)
		return NULL;
	if (row == -1) {
		*size = (int)strlen(q->names[col]);
		return q->names[col];
	}
	*size = q->sizes[row * q->cols + col];
	return q->cells[row * q->cols + col];
}

static void bi_sqlreadfield(qcvm_t *vm)
{
	int n;
	const char *s = cell(vm, &n);
	QC_ReturnString(vm, s ? s : "");
}

static void bi_sqlreadfloat(qcvm_t *vm)
{
	int n;
	const char *s = cell(vm, &n);
	QC_ReturnFloat(vm, s ? (float)atof(s) : 0);
}

/* sqlreadblob(serveridx, queryidx, row, column, ptr, maxsize): the blob's full size */
static void bi_sqlreadblob(qcvm_t *vm)
{
	int n, max = QC_PINT(vm, 5);
	const char *s = cell(vm, &n);
	if (s && max > 0 && QC_PINT(vm, 4)) {
		void *dst = QC_Pointer(vm, (uint32_t)QC_PINT(vm, 4), (size_t)BW_MIN(n, max));
		if (dst)
			memcpy(dst, s, (size_t)BW_MIN(n, max));
	}
	QC_ReturnInt(vm, s ? n : 0);
}

static void bi_sqlerror(qcvm_t *vm)
{
	conn_t *c = get_conn(QC_PFLOAT(vm, 0));
	if (c && vm->argc > 1) {
		int i = (int)QC_PFLOAT(vm, 1);
		if (i >= 0 && i < MAX_QUERIES && c->queries[i].used) {
			QC_ReturnString(vm, c->queries[i].error);
			return;
		}
	}
	QC_ReturnString(vm, c ? c->error : "not connected");
}

static void bi_sqlescape(qcvm_t *vm)
{
	const char *s = QC_PSTR(vm, 1);
	strbuf_t sb = {0};
	for (; *s; s++)
		SB_Append(&sb, *s == '\'' ? "''" : s, *s == '\'' ? 2 : 1);
	QC_ReturnString(vm, sb.data ? sb.data : "");
	SB_Free(&sb);
}

static void bi_sqlescapeblob(qcvm_t *vm)
{
	static const char hex[] = "0123456789abcdef";
	int n = QC_PINT(vm, 2);
	const uint8_t *p = n > 0 ? QC_Pointer(vm, (uint32_t)QC_PINT(vm, 1), (size_t)n) : NULL;
	strbuf_t sb = {0};
	SB_AppendStr(&sb, "X'");
	for (int i = 0; p && i < n; i++) {
		char two[2] = {hex[p[i] >> 4], hex[p[i] & 15]};
		SB_Append(&sb, two, 2);
	}
	SB_AppendStr(&sb, "'");
	QC_ReturnString(vm, sb.data);
	SB_Free(&sb);
}

static void bi_sqlversion(qcvm_t *vm)
{
	QC_ReturnString(vm, get_conn(QC_PFLOAT(vm, 0)) ? va("SQLite %s", lib.libversion()) : "");
}

const qc_builtin_t sv_sql_builtins[] = {
	{"sqlconnect", bi_sqlconnect}, {"sqldisconnect", bi_sqldisconnect}, {"sqlopenquery", bi_sqlopenquery},
	{"sqlclosequery", bi_sqlclosequery}, {"sqlreadfield", bi_sqlreadfield}, {"sqlerror", bi_sqlerror},
	{"sqlescape", bi_sqlescape}, {"sqlversion", bi_sqlversion}, {"sqlreadfloat", bi_sqlreadfloat},
	{"sqlreadblob", bi_sqlreadblob}, {"sqlescapeblob", bi_sqlescapeblob},
	{NULL, NULL}};

/* --- commands ---------------------------------------------------------------------------- */

/* the server's code is going away: its callbacks with it */
void SV_SQLShutdown(void)
{
	if (!conns)
		return;
	for (int i = 0; i < MAX_CONNECTIONS; i++)
		if (conns[i].used && !conns[i].dead)
			close_conn(&conns[i]);
}

static void SqlStatus_f(int argc, const char **argv)
{
	int n = 0;
	Con_Printf("mysql: not supported\n");
	Con_Printf("sqlite: %s\n", load_sqlite() ? "loaded" : "not installed");
	for (int i = 0; conns && i < MAX_CONNECTIONS; i++)
		n += conns[i].used && !conns[i].dead;
	Con_Printf("%d connections\n", n);
	for (int i = 0; conns && i < MAX_CONNECTIONS; i++) {
		conn_t *c = &conns[i];
		int open = 0;
		if (!c->used || c->dead)
			continue;
		for (int k = 0; k < MAX_QUERIES; k++)
			open += c->queries[k].used;
		Con_Printf("  %d: sqlite \"%s\", %d queries open, %d running%s%s\n", i, c->name, open, c->inflight,
		           c->error[0] ? ", last error: " : "", c->error);
	}
}

static void SqlKill_f(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("Syntax: sqlkill serverid\n");
		return;
	}
	conn_t *c = get_conn((float)atoi(argv[1]));
	if (!c)
		Con_Printf("no sql connection %s\n", argv[1]);
	else
		close_conn(c);
}

static void SqlKillAll_f(int argc, const char **argv) { SV_SQLShutdown(); }

void SV_InitSQL(void)
{
	Cmd_AddD("sqlstatus", SqlStatus_f, "The database drivers and the server's open connections");
	Cmd_AddD("sqlkill", SqlKill_f, "sqlkill <serverid>: close a database connection");
	Cmd_AddD("sqlkillall", SqlKillAll_f, "Close every database connection");
}
