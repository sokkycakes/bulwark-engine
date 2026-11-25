/* Bulwark engine - builtins that keep per-module state: files, directory
 * searches, hash tables and JSON trees. ISC licence, see LICENSE. */
#include "qc_internal.h"
#include "../common/fs.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>

/* --- hash tables ----------------------------------------------------- */

typedef struct qc_hashentry_s {
	char *key;
	qcval_t value[3];
	int type;
	struct qc_hashentry_s *next; /* bucket chain, in insertion order */
} qc_hashentry_t;

typedef struct qc_hashtab_s {
	qc_hashentry_t **buckets;
	int numbuckets;
	int count;
	int defaulttype;
} qc_hashtab_t;

#define HASH_REPLACE 256
#define HASH_ADD 512
#define EV_TYPEMASK 255

static uint32_t key_hash(const char *s)
{
	uint32_t h = 5381;
	for (; *s; s++)
		h = h * 33 + (uint8_t)*s;
	return h;
}

static qc_hashtab_t *hashtab_new(int size, int deftype)
{
	qc_hashtab_t *t = Mem_Alloc(sizeof(*t));
	int n = 16;
	while (n < size && n < (1 << 20))
		n *= 2;
	t->numbuckets = n;
	t->buckets = Mem_Alloc(sizeof(qc_hashentry_t *) * n);
	t->defaulttype = deftype;
	return t;
}

static void hashtab_free(qc_hashtab_t *t)
{
	if (!t)
		return;
	for (int i = 0; i < t->numbuckets; i++) {
		qc_hashentry_t *e = t->buckets[i];
		while (e) {
			qc_hashentry_t *n = e->next;
			Mem_Free(e->key);
			Mem_Free(e);
			e = n;
		}
	}
	Mem_Free(t->buckets);
	Mem_Free(t);
}

static qc_hashtab_t *get_tab(qcvm_t *vm, int h)
{
	if (h < 0 || h >= vm->extra->numhashtabs || !vm->extra->hashtabs[h])
		QC_RunError(vm, "invalid hash table %d", h);
	return vm->extra->hashtabs[h];
}

static void bi_hash_createtab(qcvm_t *vm)
{
	struct qc_extra_s *x = vm->extra;
	int deftype = vm->argc >= 2 ? (int)QC_PFLOAT(vm, 1) : 0;
	int slot;
	/* table 0 is the persistent table and is never handed out */
	for (slot = 1; slot < x->numhashtabs; slot++)
		if (!x->hashtabs[slot])
			break;
	if (slot >= x->numhashtabs) {
		x->numhashtabs = slot + 1;
		x->hashtabs = Mem_Realloc(x->hashtabs, sizeof(*x->hashtabs) * x->numhashtabs);
	}
	x->hashtabs[slot] = hashtab_new((int)QC_PFLOAT(vm, 0), deftype);
	QC_ReturnFloat(vm, (float)slot);
}

static void bi_hash_destroytab(qcvm_t *vm)
{
	int h = (int)QC_PFLOAT(vm, 0);
	if (h <= 0 || h >= vm->extra->numhashtabs)
		return;
	hashtab_free(vm->extra->hashtabs[h]);
	vm->extra->hashtabs[h] = NULL;
}

/* values are stored as raw words; the GC scans them, so string handles stay valid */
static void bi_hash_add(qcvm_t *vm)
{
	qc_hashtab_t *t = get_tab(vm, (int)QC_PFLOAT(vm, 0));
	const char *key = QC_PSTR(vm, 1);
	int flags = vm->argc >= 4 ? (int)QC_PFLOAT(vm, 3) : 0;
	int type = flags & EV_TYPEMASK;
	uint32_t b = key_hash(key) & (t->numbuckets - 1);
	qc_hashentry_t *e, **tail;
	if (!type)
		type = t->defaulttype;
	if (!(flags & HASH_ADD)) {
		for (e = t->buckets[b]; e; e = e->next)
			if (!strcmp(e->key, key)) {
				if (flags & HASH_REPLACE) {
					memcpy(e->value, QC_Parm(vm, 2), sizeof(e->value));
					e->type = type;
				}
				return; /* without either flag an existing key is left alone */
			}
	}
	e = Mem_Alloc(sizeof(*e));
	e->key = Mem_Strdup(key);
	memcpy(e->value, QC_Parm(vm, 2), sizeof(e->value));
	e->type = type;
	for (tail = &t->buckets[b]; *tail; tail = &(*tail)->next)
		;
	*tail = e;
	t->count++;
}

static void bi_hash_get(qcvm_t *vm)
{
	qc_hashtab_t *t = get_tab(vm, (int)QC_PFLOAT(vm, 0));
	const char *key = QC_PSTR(vm, 1);
	int reqtype = vm->argc >= 4 ? (int)QC_PFLOAT(vm, 3) : 0;
	int index = vm->argc >= 5 ? (int)QC_PFLOAT(vm, 4) : 0;
	qcval_t def[3];
	memcpy(def, QC_Parm(vm, 2), sizeof(def));
	if (vm->argc < 3)
		memset(def, 0, sizeof(def));
	for (qc_hashentry_t *e = t->buckets[key_hash(key) & (t->numbuckets - 1)]; e; e = e->next) {
		if (strcmp(e->key, key) || (reqtype && e->type != reqtype))
			continue;
		if (index-- > 0)
			continue;
		memcpy(QC_RETURN(vm), e->value, sizeof(e->value));
		return;
	}
	memcpy(QC_RETURN(vm), def, sizeof(def));
}

static void bi_hash_delete(qcvm_t *vm)
{
	qc_hashtab_t *t = get_tab(vm, (int)QC_PFLOAT(vm, 0));
	const char *key = QC_PSTR(vm, 1);
	qc_hashentry_t **l = &t->buckets[key_hash(key) & (t->numbuckets - 1)];
	memset(QC_RETURN(vm), 0, sizeof(qcval_t) * 3);
	for (; *l; l = &(*l)->next) {
		if (!strcmp((*l)->key, key)) {
			qc_hashentry_t *e = *l;
			memcpy(QC_RETURN(vm), e->value, sizeof(e->value));
			*l = e->next;
			Mem_Free(e->key);
			Mem_Free(e);
			t->count--;
			return;
		}
	}
}

static void bi_hash_getkey(qcvm_t *vm)
{
	qc_hashtab_t *t = get_tab(vm, (int)QC_PFLOAT(vm, 0));
	int idx = (int)QC_PFLOAT(vm, 1);
	for (int b = 0; b < t->numbuckets; b++)
		for (qc_hashentry_t *e = t->buckets[b]; e; e = e->next)
			if (idx-- == 0) {
				QC_ReturnString(vm, e->key);
				return;
			}
	QC_ReturnInt(vm, 0);
}

void QC_MarkHashTables(qcvm_t *vm, void (*mark)(uint32_t value))
{
	for (int i = 0; i < vm->extra->numhashtabs; i++) {
		qc_hashtab_t *t = vm->extra->hashtabs[i];
		if (!t)
			continue;
		for (int b = 0; b < t->numbuckets; b++)
			for (qc_hashentry_t *e = t->buckets[b]; e; e = e->next) {
				mark(e->value[0].u);
				mark(e->value[1].u);
				mark(e->value[2].u);
			}
	}
}

/* --- files ----------------------------------------------------------- */

#define FILE_READ 0
#define FILE_APPEND 1
#define FILE_WRITE 2
#define FILE_READNL 4

static qc_file_t *get_file(qcvm_t *vm, int h)
{
	if (h < 0 || h >= QC_MAX_FILES || !vm->extra->files[h].inuse)
		QC_RunError(vm, "invalid file handle %d", h);
	return &vm->extra->files[h];
}

static int alloc_file(qcvm_t *vm)
{
	for (int i = 0; i < QC_MAX_FILES; i++)
		if (!vm->extra->files[i].inuse)
			return i;
	return -1;
}

/* writes are sandboxed into data/ inside the writable game directory */
static bool data_path(const char *name, char *out, size_t outsize)
{
	char rel[1024];
	if (!strncmp(name, "data/", 5))
		Str_Copy(rel, name, sizeof(rel));
	else
		snprintf(rel, sizeof(rel), "data/%s", name);
	return FS_WritePath(rel, out, outsize);
}

static char *read_any(const char *name, size_t *len)
{
	char ospath[2048];
	char *buf;
	/* written files win over game data, so data/ is checked first */
	if (data_path(name, ospath, sizeof(ospath))) {
		FILE *f = fopen(ospath, "rb");
		if (f) {
			long n;
			fseek(f, 0, SEEK_END);
			n = ftell(f);
			fseek(f, 0, SEEK_SET);
			buf = Mem_Alloc((size_t)(n > 0 ? n : 0) + 1);
			if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)
				n = 0;
			fclose(f);
			buf[n > 0 ? n : 0] = 0;
			*len = (size_t)(n > 0 ? n : 0);
			return buf;
		}
	}
	return FS_LoadFile(name, len);
}

static void bi_fopen(qcvm_t *vm)
{
	const char *name = QC_PSTR(vm, 0);
	int mode = (int)QC_PFLOAT(vm, 1);
	int h = alloc_file(vm);
	qc_file_t *f;
	if (h < 0) {
		Con_Printf("^3fopen: too many open files\n");
		QC_ReturnFloat(vm, -1);
		return;
	}
	f = &vm->extra->files[h];
	memset(f, 0, sizeof(*f));
	if (mode == FILE_READ || mode == FILE_READNL) {
		f->data = read_any(name, &f->len);
		if (!f->data) {
			QC_ReturnFloat(vm, -1);
			return;
		}
		f->write = false;
		f->inuse = true;
		if (mode == FILE_READNL)
			f->pos = 0;
	} else if (mode == FILE_WRITE || mode == FILE_APPEND) {
		char ospath[2048];
		if (!data_path(name, ospath, sizeof(ospath)) || !FS_CreatePath(ospath) ||
		    !(f->fp = fopen(ospath, mode == FILE_WRITE ? "wb" : "ab"))) {
			QC_ReturnFloat(vm, -1);
			return;
		}
		f->write = true;
		f->inuse = true;
	} else {
		/* TODO: FILE_MMAP_* and FILE_STREAM */
		Con_Printf("^3fopen: mode %d not supported\n", mode);
		QC_ReturnFloat(vm, -1);
		return;
	}
	QC_ReturnFloat(vm, (float)h);
}

static void close_file(qc_file_t *f)
{
	if (f->fp)
		fclose(f->fp);
	Mem_Free(f->data);
	memset(f, 0, sizeof(*f));
}

/* handles the engine opens for the game, as savegames hand a file to
 * SV_PerformSave and SV_PerformLoad: a file being written, or a copy of the
 * text to read */
int QC_OpenWriteHandle(qcvm_t *vm, FILE *fp)
{
	int h = alloc_file(vm);
	if (h < 0)
		return -1;
	memset(&vm->extra->files[h], 0, sizeof(qc_file_t));
	vm->extra->files[h].fp = fp;
	vm->extra->files[h].write = true;
	vm->extra->files[h].inuse = true;
	return h;
}

int QC_OpenReadHandle(qcvm_t *vm, const char *data, size_t len)
{
	int h = alloc_file(vm);
	qc_file_t *f;
	if (h < 0)
		return -1;
	f = &vm->extra->files[h];
	memset(f, 0, sizeof(*f));
	f->data = Mem_Alloc(len + 1);
	memcpy(f->data, data, len);
	f->data[len] = 0;
	f->len = len;
	f->inuse = true;
	return h;
}

/* closes a handle the game was meant to close but didn't */
void QC_CloseHandle(qcvm_t *vm, int h)
{
	if (h >= 0 && h < QC_MAX_FILES && vm->extra->files[h].inuse)
		close_file(&vm->extra->files[h]);
}

static void bi_fclose(qcvm_t *vm)
{
	int h = (int)QC_PFLOAT(vm, 0);
	if (h < 0 || h >= QC_MAX_FILES || !vm->extra->files[h].inuse) {
		Con_Printf("^3fclose: invalid handle %d\n", h);
		return;
	}
	close_file(&vm->extra->files[h]);
}

static void bi_fgets(qcvm_t *vm)
{
	qc_file_t *f = get_file(vm, (int)QC_PFLOAT(vm, 0));
	size_t start, end;
	if (f->write || f->pos >= f->len) {
		QC_ReturnInt(vm, 0);
		return;
	}
	start = f->pos;
	end = start;
	while (end < f->len && f->data[end] != '\n')
		end++;
	f->pos = end < f->len ? end + 1 : end;
	if (end > start && f->data[end - 1] == '\r')
		end--;
	vm->globals[OFS_RETURN].i = QC_NewStringLen(vm, f->data + start, end - start);
}

static void bi_fputs(qcvm_t *vm)
{
	qc_file_t *f = get_file(vm, (int)QC_PFLOAT(vm, 0));
	if (!f->write)
		return;
	for (int i = 1; i < vm->argc; i++)
		fputs(QC_PSTR(vm, i), f->fp);
}

static void bi_fread(qcvm_t *vm)
{
	qc_file_t *f = get_file(vm, (int)QC_PFLOAT(vm, 0));
	uint32_t ptr = (uint32_t)QC_PINT(vm, 1);
	int size = QC_PINT(vm, 2);
	if (vm->argc >= 4)
		ptr += (uint32_t)QC_PINT(vm, 3);
	if (f->write || size <= 0) {
		QC_ReturnInt(vm, 0);
		return;
	}
	if ((size_t)size > f->len - f->pos)
		size = (int)(f->len - f->pos);
	if (size > 0)
		memcpy(QC_Pointer(vm, ptr, (size_t)size), f->data + f->pos, (size_t)size);
	f->pos += (size_t)size;
	QC_ReturnInt(vm, size);
}

static void bi_fwrite(qcvm_t *vm)
{
	qc_file_t *f = get_file(vm, (int)QC_PFLOAT(vm, 0));
	uint32_t ptr = (uint32_t)QC_PINT(vm, 1);
	int size = QC_PINT(vm, 2);
	if (vm->argc >= 4)
		ptr += (uint32_t)QC_PINT(vm, 3);
	if (!f->write || size <= 0) {
		QC_ReturnInt(vm, 0);
		return;
	}
	QC_ReturnInt(vm, (int)fwrite(QC_Pointer(vm, ptr, (size_t)size), 1, (size_t)size, f->fp));
}

static void bi_fseek(qcvm_t *vm)
{
	qc_file_t *f = get_file(vm, (int)QC_PFLOAT(vm, 0));
	long prior = f->write ? ftell(f->fp) : (long)f->pos;
	if (vm->argc >= 2) {
		int np = QC_PINT(vm, 1);
		if (f->write)
			fseek(f->fp, np, SEEK_SET);
		else
			f->pos = np < 0 ? 0 : (size_t)np > f->len ? f->len : (size_t)np;
	}
	QC_ReturnInt(vm, (int32_t)prior);
}

static void bi_fsize(qcvm_t *vm)
{
	qc_file_t *f = get_file(vm, (int)QC_PFLOAT(vm, 0));
	if (f->write) {
		long cur = ftell(f->fp), end;
		fseek(f->fp, 0, SEEK_END);
		end = ftell(f->fp);
		fseek(f->fp, cur, SEEK_SET);
		QC_ReturnInt(vm, (int32_t)end);
	} else {
		QC_ReturnInt(vm, (int32_t)f->len);
	}
}

static void bi_fexists(qcvm_t *vm)
{
	char ospath[2048];
	struct stat st;
	const char *name = QC_PSTR(vm, 0);
	if (data_path(name, ospath, sizeof(ospath)) && !stat(ospath, &st)) {
		QC_ReturnFloat(vm, 1);
		return;
	}
	QC_ReturnFloat(vm, FS_FileExists(name) ? 1 : 0);
}

static void bi_fremove(qcvm_t *vm)
{
	char ospath[2048];
	if (!data_path(QC_PSTR(vm, 0), ospath, sizeof(ospath))) {
		QC_ReturnFloat(vm, -1);
		return;
	}
	QC_ReturnFloat(vm, remove(ospath) ? -1 : 0);
}

static void bi_whichpack(qcvm_t *vm)
{
	const char *p = FS_WhichPath(QC_PSTR(vm, 0));
	if (p)
		QC_ReturnString(vm, p);
	else
		QC_ReturnInt(vm, 0);
}

/* --- searches -------------------------------------------------------- */

typedef struct {
	qc_search_t *s;
	int cap;
} search_ctx_t;

static void search_add(const char *name, const char *searchpath, void *ctx)
{
	search_ctx_t *c = ctx;
	(void)searchpath;
	if (c->s->count >= c->cap) {
		c->cap = c->cap ? c->cap * 2 : 32;
		c->s->names = Mem_Realloc(c->s->names, sizeof(char *) * c->cap);
	}
	c->s->names[c->s->count++] = Mem_Strdup(name);
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void bi_search_begin(qcvm_t *vm)
{
	const char *pattern = QC_PSTR(vm, 0);
	int flags = (int)QC_PFLOAT(vm, 1);
	search_ctx_t ctx = {0};
	int h;
	for (h = 0; h < QC_MAX_SEARCHES; h++)
		if (!vm->extra->searches[h].inuse)
			break;
	if (h >= QC_MAX_SEARCHES) {
		QC_ReturnFloat(vm, -1);
		return;
	}
	ctx.s = &vm->extra->searches[h];
	memset(ctx.s, 0, sizeof(*ctx.s));
	ctx.s->inuse = true;
	if (flags & 16) {
		/* SB_MULTISEARCH: patterns separated by colons */
		char one[1024];
		for (const char *p = pattern; *p;) {
			const char *e = strchr(p, ':');
			size_t n = e ? (size_t)(e - p) : strlen(p);
			if (n && n < sizeof(one)) {
				memcpy(one, p, n);
				one[n] = 0;
				FS_List(one, (flags & 1) != 0, search_add, &ctx);
			}
			p += n + (e ? 1 : 0);
		}
	} else {
		FS_List(pattern, (flags & 1) != 0, search_add, &ctx);
	}
	qsort(ctx.s->names, (size_t)ctx.s->count, sizeof(char *), cmp_names);
	/* a file several patterns found is listed once */
	int kept = 0;
	for (int i = 0; i < ctx.s->count; i++) {
		if (kept && !strcmp(ctx.s->names[kept - 1], ctx.s->names[i]))
			Mem_Free(ctx.s->names[i]);
		else
			ctx.s->names[kept++] = ctx.s->names[i];
	}
	ctx.s->count = kept;
	QC_ReturnFloat(vm, (float)h);
}

static qc_search_t *get_search(qcvm_t *vm, int h)
{
	if (h < 0 || h >= QC_MAX_SEARCHES || !vm->extra->searches[h].inuse)
		return NULL;
	return &vm->extra->searches[h];
}

static void free_search(qc_search_t *s)
{
	for (int i = 0; i < s->count; i++)
		Mem_Free(s->names[i]);
	Mem_Free(s->names);
	memset(s, 0, sizeof(*s));
}

static void bi_search_end(qcvm_t *vm)
{
	qc_search_t *s = get_search(vm, (int)QC_PFLOAT(vm, 0));
	if (s)
		free_search(s);
}

static void bi_search_getsize(qcvm_t *vm)
{
	qc_search_t *s = get_search(vm, (int)QC_PFLOAT(vm, 0));
	QC_ReturnFloat(vm, s ? (float)s->count : -1);
}

static const char *search_name(qcvm_t *vm)
{
	qc_search_t *s = get_search(vm, (int)QC_PFLOAT(vm, 0));
	int i = (int)QC_PFLOAT(vm, 1);
	return (s && i >= 0 && i < s->count) ? s->names[i] : NULL;
}

static void bi_search_getfilename(qcvm_t *vm)
{
	const char *n = search_name(vm);
	if (n)
		QC_ReturnString(vm, n);
	else
		QC_ReturnInt(vm, 0);
}

static void bi_search_getpackagename(qcvm_t *vm)
{
	const char *n = search_name(vm);
	const char *p = n ? FS_WhichPath(n) : NULL;
	if (p)
		QC_ReturnString(vm, p);
	else
		QC_ReturnInt(vm, 0);
}

static void bi_search_getfilesize(qcvm_t *vm)
{
	const char *n = search_name(vm);
	size_t len = 0;
	char *buf = n ? FS_LoadFile(n, &len) : NULL;
	Mem_Free(buf);
	QC_ReturnFloat(vm, (float)len);
}

static void bi_search_fopen(qcvm_t *vm)
{
	const char *n = search_name(vm);
	int h;
	qc_file_t *f;
	if (!n || (h = alloc_file(vm)) < 0) {
		QC_ReturnFloat(vm, -1);
		return;
	}
	f = &vm->extra->files[h];
	memset(f, 0, sizeof(*f));
	f->data = FS_LoadFile(n, &f->len);
	if (!f->data) {
		QC_ReturnFloat(vm, -1);
		return;
	}
	f->inuse = true;
	QC_ReturnFloat(vm, (float)h);
}

/* --- JSON ------------------------------------------------------------ */

enum { JSON_STRING, JSON_NUMBER, JSON_OBJECT, JSON_ARRAY, JSON_TRUE, JSON_FALSE, JSON_NULL };

typedef struct qc_json_s {
	int type;
	char *name;
	char *str;
	double num;
	int *children;
	int numchildren;
	bool root;
} qc_json_t;

static int json_new(qcvm_t *vm, int type)
{
	struct qc_extra_s *x = vm->extra;
	int i;
	for (i = 1; i < x->numjson; i++)
		if (!x->json[i])
			break;
	if (i >= x->numjson) {
		x->numjson = i + 1;
		x->json = Mem_Realloc(x->json, sizeof(*x->json) * x->numjson);
	}
	x->json[i] = Mem_Alloc(sizeof(qc_json_t));
	x->json[i]->type = type;
	return i;
}

static void json_free_node(qcvm_t *vm, int n)
{
	qc_json_t *j;
	if (n <= 0 || n >= vm->extra->numjson || !(j = vm->extra->json[n]))
		return;
	for (int i = 0; i < j->numchildren; i++)
		json_free_node(vm, j->children[i]);
	Mem_Free(j->children);
	Mem_Free(j->name);
	Mem_Free(j->str);
	Mem_Free(j);
	vm->extra->json[n] = NULL;
}

static const char *json_ws(const char *p)
{
	while (*p && isspace((unsigned char)*p))
		p++;
	return p;
}

static char *json_parse_string(const char **pp)
{
	const char *p = *pp + 1;
	strbuf_t sb = {0};
	while (*p && *p != '"') {
		if (*p == '\\' && p[1]) {
			p++;
			switch (*p) {
			case 'n': SB_Append(&sb, "\n", 1); break;
			case 't': SB_Append(&sb, "\t", 1); break;
			case 'r': SB_Append(&sb, "\r", 1); break;
			case 'b': SB_Append(&sb, "\b", 1); break;
			case 'f': SB_Append(&sb, "\f", 1); break;
			case 'u': {
				unsigned cp = 0;
				for (int i = 1; i <= 4 && isxdigit((unsigned char)p[i]); i++)
					cp = cp * 16 + (unsigned)(isdigit((unsigned char)p[i]) ? p[i] - '0' : (tolower((unsigned char)p[i]) - 'a' + 10));
				/* encode the code point as UTF-8 (surrogate pairs are left as-is) */
				if (cp < 0x80) {
					char c = (char)cp;
					if (c)
						SB_Append(&sb, &c, 1);
				} else if (cp < 0x800) {
					char c[2] = {(char)(0xc0 | (cp >> 6)), (char)(0x80 | (cp & 63))};
					SB_Append(&sb, c, 2);
				} else {
					char c[3] = {(char)(0xe0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 63)), (char)(0x80 | (cp & 63))};
					SB_Append(&sb, c, 3);
				}
				p += 4;
				break;
			}
			default: SB_Append(&sb, p, 1); break;
			}
			p++;
		} else {
			SB_Append(&sb, p++, 1);
		}
	}
	if (*p == '"')
		p++;
	*pp = p;
	if (!sb.data)
		SB_Append(&sb, "", 0);
	return sb.data;
}

static int json_parse_value(qcvm_t *vm, const char **pp, int depth)
{
	const char *p = json_ws(*pp);
	int n;
	if (depth > 64)
		return 0;
	if (*p == '{' || *p == '[') {
		bool obj = *p == '{';
		char close = obj ? '}' : ']';
		n = json_new(vm, obj ? JSON_OBJECT : JSON_ARRAY);
		p = json_ws(p + 1);
		while (*p && *p != close) {
			char *name = NULL;
			int child;
			qc_json_t *j;
			if (obj) {
				if (*p != '"')
					goto fail;
				name = json_parse_string(&p);
				p = json_ws(p);
				if (*p != ':') {
					Mem_Free(name);
					goto fail;
				}
				p++;
			}
			child = json_parse_value(vm, &p, depth + 1);
			if (!child) {
				Mem_Free(name);
				goto fail;
			}
			vm->extra->json[child]->name = name;
			j = vm->extra->json[n];
			j->children = Mem_Realloc(j->children, sizeof(int) * (j->numchildren + 1));
			j->children[j->numchildren++] = child;
			p = json_ws(p);
			if (*p == ',')
				p = json_ws(p + 1);
			else if (*p != close)
				goto fail;
		}
		if (*p != close)
			goto fail;
		*pp = p + 1;
		return n;
	fail:
		json_free_node(vm, n);
		return 0;
	}
	if (*p == '"') {
		n = json_new(vm, JSON_STRING);
		vm->extra->json[n]->str = json_parse_string(&p);
		*pp = p;
		return n;
	}
	if (!strncmp(p, "true", 4)) {
		*pp = p + 4;
		return json_new(vm, JSON_TRUE);
	}
	if (!strncmp(p, "false", 5)) {
		*pp = p + 5;
		return json_new(vm, JSON_FALSE);
	}
	if (!strncmp(p, "null", 4)) {
		*pp = p + 4;
		return json_new(vm, JSON_NULL);
	}
	if (*p == '-' || isdigit((unsigned char)*p)) {
		char *end;
		double d = strtod(p, &end);
		n = json_new(vm, JSON_NUMBER);
		vm->extra->json[n]->num = d;
		*pp = end;
		return n;
	}
	return 0;
}

static qc_json_t *get_json(qcvm_t *vm, int n)
{
	if (n <= 0 || n >= vm->extra->numjson)
		return NULL;
	return vm->extra->json[n];
}

static void bi_json_parse(qcvm_t *vm)
{
	const char *p = QC_PSTR(vm, 0);
	int n = json_parse_value(vm, &p, 0);
	if (n)
		vm->extra->json[n]->root = true;
	QC_ReturnInt(vm, n);
}

static void bi_json_free(qcvm_t *vm) { json_free_node(vm, QC_PINT(vm, 0)); }

static void bi_json_get_value_type(qcvm_t *vm)
{
	qc_json_t *j = get_json(vm, QC_PINT(vm, 0));
	QC_ReturnInt(vm, j ? j->type : JSON_NULL);
}

static void bi_json_get_integer(qcvm_t *vm)
{
	qc_json_t *j = get_json(vm, QC_PINT(vm, 0));
	QC_ReturnInt(vm, j && j->type == JSON_NUMBER ? (int32_t)j->num : j && j->type == JSON_TRUE ? 1 : 0);
}

static void bi_json_get_float(qcvm_t *vm)
{
	qc_json_t *j = get_json(vm, QC_PINT(vm, 0));
	QC_ReturnFloat(vm, j && j->type == JSON_NUMBER ? (float)j->num : j && j->type == JSON_TRUE ? 1 : 0);
}

static void bi_json_get_string(qcvm_t *vm)
{
	qc_json_t *j = get_json(vm, QC_PINT(vm, 0));
	if (j && j->type == JSON_STRING)
		QC_ReturnString(vm, j->str);
	else
		QC_ReturnInt(vm, 0);
}

static void bi_json_get_name(qcvm_t *vm)
{
	qc_json_t *j = get_json(vm, QC_PINT(vm, 0));
	if (j && j->name)
		QC_ReturnString(vm, j->name);
	else
		QC_ReturnInt(vm, 0);
}

static void bi_json_get_length(qcvm_t *vm)
{
	qc_json_t *j = get_json(vm, QC_PINT(vm, 0));
	QC_ReturnInt(vm, j && (j->type == JSON_ARRAY || j->type == JSON_OBJECT) ? j->numchildren : 0);
}

static void bi_json_get_child_at_index(qcvm_t *vm)
{
	qc_json_t *j = get_json(vm, QC_PINT(vm, 0));
	int i = QC_PINT(vm, 1);
	QC_ReturnInt(vm, j && i >= 0 && i < j->numchildren ? j->children[i] : 0);
}

static void bi_json_find_object_child(qcvm_t *vm)
{
	qc_json_t *j = get_json(vm, QC_PINT(vm, 0));
	const char *name = QC_PSTR(vm, 1);
	if (j && j->type == JSON_OBJECT)
		for (int i = 0; i < j->numchildren; i++) {
			qc_json_t *c = get_json(vm, j->children[i]);
			if (c && c->name && !strcmp(c->name, name)) {
				QC_ReturnInt(vm, j->children[i]);
				return;
			}
		}
	QC_ReturnInt(vm, 0);
}

/* --- setup ----------------------------------------------------------- */

void QC_ExtraInit(qcvm_t *vm)
{
	struct qc_extra_s *x = vm->extra;
	x->numhashtabs = 1;
	x->hashtabs = Mem_Alloc(sizeof(*x->hashtabs));
	x->hashtabs[0] = hashtab_new(64, 0); /* the persistent table */
	x->numjson = 1;
	x->json = Mem_Alloc(sizeof(*x->json));
}

/* --- string buffers (DP_QC_STRINGBUFFERS) ------------------------------ */

static void strbuf_free(qc_strbuf_t *b)
{
	for (int i = 0; i < b->count; i++)
		Mem_Free(b->strs[i]);
	Mem_Free(b->strs);
	memset(b, 0, sizeof(*b));
}

static qc_strbuf_t *get_strbuf(qcvm_t *vm, float h)
{
	int i = (int)h;
	if (i < 0 || i >= vm->extra->numstrbufs || !vm->extra->strbufs[i].inuse)
		return NULL;
	return &vm->extra->strbufs[i];
}

static void strbuf_set(qc_strbuf_t *b, int idx, const char *s)
{
	if (idx < 0 || idx >= (1 << 20))
		return;
	if (idx >= b->cap) {
		int cap = b->cap ? b->cap : 16;
		while (cap <= idx)
			cap *= 2;
		b->strs = Mem_Realloc(b->strs, sizeof(char *) * (size_t)cap);
		memset(b->strs + b->cap, 0, sizeof(char *) * (size_t)(cap - b->cap));
		b->cap = cap;
	}
	Mem_Free(b->strs[idx]);
	b->strs[idx] = s ? Mem_Strdup(s) : NULL;
	if (idx >= b->count)
		b->count = idx + 1;
	while (b->count > 0 && !b->strs[b->count - 1])
		b->count--;
}

static void bi_buf_create(qcvm_t *vm)
{
	struct qc_extra_s *x = vm->extra;
	int i;
	for (i = 0; i < x->numstrbufs; i++)
		if (!x->strbufs[i].inuse)
			break;
	if (i == x->numstrbufs) {
		x->strbufs = Mem_Realloc(x->strbufs, sizeof(*x->strbufs) * (size_t)(x->numstrbufs + 1));
		memset(&x->strbufs[i], 0, sizeof(*x->strbufs));
		x->numstrbufs++;
	}
	x->strbufs[i].inuse = true;
	QC_ReturnFloat(vm, (float)i);
}

static void bi_buf_del(qcvm_t *vm)
{
	qc_strbuf_t *b = get_strbuf(vm, QC_PFLOAT(vm, 0));
	if (b)
		strbuf_free(b);
}

static void bi_buf_getsize(qcvm_t *vm)
{
	qc_strbuf_t *b = get_strbuf(vm, QC_PFLOAT(vm, 0));
	QC_ReturnFloat(vm, b ? (float)b->count : 0);
}

static void bi_buf_copy(qcvm_t *vm)
{
	qc_strbuf_t *from = get_strbuf(vm, QC_PFLOAT(vm, 0));
	qc_strbuf_t *to = get_strbuf(vm, QC_PFLOAT(vm, 1));
	if (!from || !to || from == to)
		return;
	for (int i = 0; i < to->count; i++)
		Mem_Free(to->strs[i]);
	to->count = 0;
	for (int i = 0; i < from->count; i++)
		strbuf_set(to, i, from->strs[i]);
}

static void bi_buf_implode(qcvm_t *vm)
{
	qc_strbuf_t *b = get_strbuf(vm, QC_PFLOAT(vm, 0));
	const char *glue = QC_PSTR(vm, 1);
	strbuf_t sb = {0};
	bool first = true;
	if (b) {
		for (int i = 0; i < b->count; i++) {
			if (!b->strs[i])
				continue;
			if (!first)
				SB_AppendStr(&sb, glue);
			SB_AppendStr(&sb, b->strs[i]);
			first = false;
		}
	}
	QC_ReturnString(vm, sb.data ? sb.data : "");
	SB_Free(&sb);
}

static void bi_bufstr_get(qcvm_t *vm)
{
	qc_strbuf_t *b = get_strbuf(vm, QC_PFLOAT(vm, 0));
	int idx = (int)QC_PFLOAT(vm, 1);
	if (!b || idx < 0 || idx >= b->count || !b->strs[idx]) {
		QC_ReturnString(vm, "");
		return;
	}
	QC_ReturnString(vm, b->strs[idx]);
}

static void bi_bufstr_set(qcvm_t *vm)
{
	qc_strbuf_t *b = get_strbuf(vm, QC_PFLOAT(vm, 0));
	if (b)
		strbuf_set(b, (int)QC_PFLOAT(vm, 1), QC_PSTR(vm, 2));
}

/* order 0: first free slot; otherwise the end */
static void bi_bufstr_add(qcvm_t *vm)
{
	qc_strbuf_t *b = get_strbuf(vm, QC_PFLOAT(vm, 0));
	int idx;
	if (!b) {
		QC_ReturnFloat(vm, -1);
		return;
	}
	idx = b->count;
	if (QC_PFLOAT(vm, 2) == 0)
		for (int i = 0; i < b->count; i++)
			if (!b->strs[i]) {
				idx = i;
				break;
			}
	strbuf_set(b, idx, QC_PSTR(vm, 1));
	QC_ReturnFloat(vm, (float)idx);
}

static void bi_bufstr_free(qcvm_t *vm)
{
	qc_strbuf_t *b = get_strbuf(vm, QC_PFLOAT(vm, 0));
	if (b)
		strbuf_set(b, (int)QC_PFLOAT(vm, 1), NULL);
}

static int strbuf_cmp_prefix;
static int strbuf_cmp(const void *a, const void *b)
{
	const char *x = *(char *const *)a, *y = *(char *const *)b;
	if (!x || !y)
		return !x - !y;
	return strbuf_cmp_prefix > 0 ? strncmp(x, y, (size_t)strbuf_cmp_prefix) : strcmp(x, y);
}

static void bi_buf_sort(qcvm_t *vm)
{
	qc_strbuf_t *b = get_strbuf(vm, QC_PFLOAT(vm, 0));
	if (!b || b->count < 2)
		return;
	strbuf_cmp_prefix = (int)QC_PFLOAT(vm, 1);
	qsort(b->strs, (size_t)b->count, sizeof(char *), strbuf_cmp);
	if (QC_PFLOAT(vm, 2) != 0)
		for (int i = 0, j = b->count - 1; i < j; i++, j--) {
			char *t = b->strs[i];
			b->strs[i] = b->strs[j];
			b->strs[j] = t;
		}
	while (b->count > 0 && !b->strs[b->count - 1])
		b->count--;
}

void QC_ExtraFree(qcvm_t *vm)
{
	struct qc_extra_s *x = vm->extra;
	for (int i = 0; i < x->numhashtabs; i++)
		hashtab_free(x->hashtabs[i]);
	Mem_Free(x->hashtabs);
	for (int i = 1; i < x->numjson; i++)
		if (x->json[i] && x->json[i]->root)
			json_free_node(vm, i);
	for (int i = 1; i < x->numjson; i++)
		json_free_node(vm, i);
	Mem_Free(x->json);
	for (int i = 0; i < QC_MAX_FILES; i++)
		if (x->files[i].inuse)
			close_file(&x->files[i]);
	for (int i = 0; i < QC_MAX_SEARCHES; i++)
		if (x->searches[i].inuse)
			free_search(&x->searches[i]);
	for (int i = 0; i < x->numstrbufs; i++)
		strbuf_free(&x->strbufs[i]);
	Mem_Free(x->strbufs);
	QC_FreeTokens(vm);
}

/* --- FTE_MULTIPROGS ------------------------------------------------------- */

static void bi_addprogs(qcvm_t *vm)
{
	const char *name = QC_PSTR(vm, 0);
	size_t len;
	void *data = FS_LoadFile(name, &len);
	int n = -1;
	if (data) {
		n = QC_AddProgs(vm, name, data, len);
		Mem_Free(data);
	} else {
		Con_DPrintf("addprogs: %s not found\n", name);
	}
	QC_ReturnFloat(vm, (float)n);
}

static void bi_externvalue(qcvm_t *vm)
{
	const char *name = QC_PSTR(vm, 1);
	int pn = QC_ResolveProgs(vm, QC_PFLOAT(vm, 0), name, false);
	qcdef_t *d;
	int f;
	memset(QC_RETURN(vm), 0, sizeof(qcval_t) * 3);
	if (pn < 0 && QC_PFLOAT(vm, 0) == -2)
		pn = QC_ResolveProgs(vm, -2, name, true);
	if (pn < 0)
		return;
	d = QC_FindGlobalIn(vm, pn, name);
	if (d) {
		int words = (d->type & ~DEF_SAVEGLOBAL) == ev_vector ? 3 : 1;
		memcpy(QC_RETURN(vm), QC_ProgsGlobals(vm, pn) + d->ofs, sizeof(qcval_t) * words);
	} else if ((f = QC_FindFunctionIn(vm, pn, name))) {
		QC_ReturnInt(vm, f);
	}
}

static void bi_externset(qcvm_t *vm)
{
	const char *name = QC_PSTR(vm, 2);
	int pn = QC_ResolveProgs(vm, QC_PFLOAT(vm, 0), name, false);
	qcdef_t *d;
	if (pn < 0 || !(d = QC_FindGlobalIn(vm, pn, name)))
		return;
	memcpy(QC_ProgsGlobals(vm, pn) + d->ofs, QC_Parm(vm, 1),
	       sizeof(qcval_t) * ((d->type & ~DEF_SAVEGLOBAL) == ev_vector ? 3 : 1));
}

static void bi_externcall(qcvm_t *vm)
{
	const char *name = QC_PSTR(vm, 1);
	int pn = QC_ResolveProgs(vm, QC_PFLOAT(vm, 0), name, true);
	int f, argc = vm->argc;
	if (pn < 0 || !(f = QC_FindFunctionIn(vm, pn, name))) {
		QC_ReturnInt(vm, 0);
		return;
	}
	/* shift the remaining arguments down by two */
	for (int i = 2; i < argc && i < MAX_PARMS; i++)
		memcpy(QC_Parm(vm, i - 2), QC_Parm(vm, i), sizeof(qcval_t) * 3);
	vm->argc = argc - 2;
	QC_Execute(vm, f);
}

const qc_builtin_t qc_builtins_extra[] = {
	{"addprogs", bi_addprogs}, {"externvalue", bi_externvalue}, {"externset", bi_externset},
	{"externcall", bi_externcall},
	{"hash_createtab", bi_hash_createtab}, {"hash_destroytab", bi_hash_destroytab},
	{"hash_add", bi_hash_add}, {"hash_get", bi_hash_get}, {"hash_delete", bi_hash_delete},
	{"hash_getkey", bi_hash_getkey},
	{"fopen", bi_fopen}, {"fclose", bi_fclose}, {"fgets", bi_fgets}, {"fputs", bi_fputs},
	{"fread", bi_fread}, {"fwrite", bi_fwrite}, {"fseek", bi_fseek}, {"fsize", bi_fsize},
	{"fexists", bi_fexists}, {"fremove", bi_fremove}, {"whichpack", bi_whichpack},
	{"search_begin", bi_search_begin}, {"search_end", bi_search_end},
	{"search_getsize", bi_search_getsize}, {"search_getfilename", bi_search_getfilename},
	{"search_getpackagename", bi_search_getpackagename}, {"search_getfilesize", bi_search_getfilesize},
	{"search_fopen", bi_search_fopen},
	{"buf_create", bi_buf_create}, {"buf_del", bi_buf_del}, {"buf_getsize", bi_buf_getsize},
	{"buf_copy", bi_buf_copy}, {"buf_sort", bi_buf_sort}, {"buf_implode", bi_buf_implode},
	{"bufstr_get", bi_bufstr_get}, {"bufstr_set", bi_bufstr_set}, {"bufstr_add", bi_bufstr_add},
	{"bufstr_free", bi_bufstr_free},
	{"json_parse", bi_json_parse}, {"json_free", bi_json_free},
	{"json_get_value_type", bi_json_get_value_type}, {"json_get_integer", bi_json_get_integer},
	{"json_get_float", bi_json_get_float}, {"json_get_string", bi_json_get_string},
	{"json_get_name", bi_json_get_name}, {"json_get_length", bi_json_get_length},
	{"json_get_child_at_index", bi_json_get_child_at_index},
	{"json_find_object_child", bi_json_find_object_child},
	{NULL, NULL}};
