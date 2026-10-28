/* Bulwark engine - the package manager ("pkg"), fs_changemod and fetching
 * manifests from the web for fs_changegame.
 *
 * Package lists come from sources: urls (or local files) the user adds
 * with "pkg addsource", plus the DOWNLOADSURL of the game's manifest. A
 * list is text, a "BWPKG 1" line and then one line per package:
 *   package "name" version "1.2" file "base/foo.pk3" url "http://..."
 *           sha256 "<hex>" size 1234 title "..." desc "..." author "..."
 *           license "..." website "..." depends "other;another"
 * with // comments. "file" is where the package goes, relative to the base
 * directory. Installed packages are remembered in <basedir>/pkg/installed.txt
 * and the user's sources in <basedir>/pkg/sources.txt. Changes are marked
 * first (add, rem, del, upgrade...) and made by "pkg apply".
 * ISC licence, see LICENSE. */
#include "pkg.h"
#include "http.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/digest.h"
#include "../common/fs.h"
#include "../common/manifest.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MARK_NONE, MARK_INSTALL, MARK_REINSTALL, MARK_DISABLE, MARK_PURGE };

typedef struct {
	char name[64], version[32], file[128], url[512], sha256[65], depends[256], category[64];
	char title[128], desc[512], author[128], license[64], website[256];
	long size;
	int source; /* -1: known only from the installed list */
	int mark;
	bool installed, disabled, corrupt;
	char installedver[32];
} pkg_t;

enum { SRC_UNFETCHED, SRC_FETCHING, SRC_OK, SRC_FAILED };

typedef struct {
	char url[512];
	bool implicit, enabled;
	int state;
	http_t *h;
} source_t;

enum { JOB_FILE, JOB_MANIFEST };

typedef struct job_s {
	int kind;
	http_t *h;
	char name[64];       /* package name, or "" for fs_changemod files */
	char tmp[1024], dest[1024], sha256[65];
	struct job_s *next;
} job_t;

static pkg_t *pkgs;
static int numpkgs, cappkgs;
static source_t sources[32];
static int numsources;
static job_t *jobs;
static bool applying;
static strbuf_t after_jobs; /* commands for when the downloads finish (fs_changemod) */
static char after_gamedir[64];
static bool jobs_failed;
static cvar_t *pkg_autoupdate;
static bool announce; /* say what can be upgraded once the lists are in */

static const char *base_path(const char *rel) { return va("%s/%s", FS_BaseDir(), rel); }

static bool is_url(const char *s) { return strstr(s, "://") && Str_NICmp(s, "file://", 7); }
static const char *local_path(const char *s) { return !Str_NICmp(s, "file://", 7) ? s + 7 : s; }

static char *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) {
		fclose(f);
		return NULL;
	}
	char *buf = Mem_Alloc((size_t)n + 1);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		Mem_Free(buf);
		return NULL;
	}
	buf[n] = 0;
	fclose(f);
	if (len)
		*len = (size_t)n;
	return buf;
}

static bool exists(const char *path)
{
	struct stat st;
	return !stat(path, &st);
}

/* numbers compare as numbers, everything else as text */
static int vercmp(const char *a, const char *b)
{
	while (*a || *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
			if (x != y)
				return x < y ? -1 : 1;
			continue;
		}
		if (*a != *b)
			return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
		a++, b++;
	}
	return 0;
}

/* by name, or "name=version" as the menus see packages */
static pkg_t *find(const char *name)
{
	const char *eq = strchr(name, '=');
	size_t len = eq ? (size_t)(eq - name) : strlen(name);
	for (int i = 0; i < numpkgs; i++)
		if (strlen(pkgs[i].name) == len && !Str_NICmp(pkgs[i].name, name, len))
			return &pkgs[i];
	return NULL;
}

static pkg_t *new_pkg(const char *name)
{
	if (numpkgs == cappkgs) {
		cappkgs = cappkgs ? cappkgs * 2 : 64;
		pkgs = Mem_Realloc(pkgs, (size_t)cappkgs * sizeof(*pkgs));
	}
	pkg_t *p = &pkgs[numpkgs++];
	memset(p, 0, sizeof(*p));
	Str_Copy(p->name, name, sizeof(p->name));
	p->source = -1;
	return p;
}

/* --- the installed list and the sources file ------------------------------ */

static void load_installed(void)
{
	char *text = read_file(base_path("pkg/installed.txt"), NULL);
	cmd_tokens_t *t = Mem_Alloc(sizeof(*t));
	for (char *l = text; l && *l;) {
		char *e = strchr(l, '\n');
		if (e)
			*e = 0;
		Cmd_TokenizeInto(t, l);
		if (t->argc >= 3) {
			pkg_t *p = find(t->argv[0]);
			if (!p) {
				p = new_pkg(t->argv[0]);
				Str_Copy(p->version, t->argv[1], sizeof(p->version));
				Str_Copy(p->file, t->argv[2], sizeof(p->file));
			}
			p->installed = true;
			Str_Copy(p->installedver, t->argv[1], sizeof(p->installedver));
			if (!p->file[0])
				Str_Copy(p->file, t->argv[2], sizeof(p->file));
			p->disabled = t->argc > 3 && !strcmp(t->argv[3], "disabled");
			p->corrupt = !exists(base_path(va("%s%s", p->file, p->disabled ? ".disabled" : "")));
		}
		l = e ? e + 1 : NULL;
	}
	Mem_Free(t);
	Mem_Free(text);
}

static void save_installed(void)
{
	const char *path = base_path("pkg/installed.txt");
	FS_CreatePath(path);
	FILE *f = fopen(path, "w");
	if (!f) {
		Con_Printf("pkg: can't write %s\n", path);
		return;
	}
	fprintf(f, "// packages installed by \"pkg\": name, version, file\n");
	for (int i = 0; i < numpkgs; i++)
		if (pkgs[i].installed)
			fprintf(f, "\"%s\" \"%s\" \"%s\"%s\n", pkgs[i].name, pkgs[i].installedver, pkgs[i].file,
			        pkgs[i].disabled ? " disabled" : "");
	fclose(f);
}

static void save_sources(void)
{
	const char *path = base_path("pkg/sources.txt");
	FS_CreatePath(path);
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	for (int i = 0; i < numsources; i++)
		if (!sources[i].implicit)
			fprintf(f, "%s\n", sources[i].url);
		else if (!sources[i].enabled)
			fprintf(f, "-%s\n", sources[i].url);
	fclose(f);
}

static source_t *find_source(const char *url)
{
	for (int i = 0; i < numsources; i++)
		if (!strcmp(sources[i].url, url))
			return &sources[i];
	return NULL;
}

static source_t *add_source(const char *url, bool implicit)
{
	source_t *s = find_source(url);
	if (s || numsources >= (int)BW_ARRAY_COUNT(sources))
		return s;
	s = &sources[numsources++];
	memset(s, 0, sizeof(*s));
	Str_Copy(s->url, url, sizeof(s->url));
	s->implicit = implicit;
	s->enabled = true;
	return s;
}

/* the sources and installed list, read again whenever the game changes */
static char loaded_for[512];

static void forget_all(void)
{
	for (int i = 0; i < numsources; i++)
		if (sources[i].h)
			HTTP_Free(sources[i].h);
	numsources = 0;
	numpkgs = 0;
}

static void ensure_loaded(void)
{
	const manifest_t *mf = Mf_Active();
	char key[sizeof(loaded_for)];
	snprintf(key, sizeof(key), "%.200s|%.300s", FS_BaseDir(), mf->downloadsurl);
	if (!strcmp(key, loaded_for))
		return;
	Str_Copy(loaded_for, key, sizeof(loaded_for));
	forget_all();
	if (mf->downloadsurl[0])
		add_source(mf->downloadsurl, true);
	char *text = read_file(base_path("pkg/sources.txt"), NULL);
	for (char *l = text; l && *l;) {
		char *e = strchr(l, '\n');
		if (e)
			*e = 0;
		while (*l == ' ' || *l == '\t')
			l++;
		size_t n = strlen(l);
		while (n && (l[n - 1] == '\r' || l[n - 1] == ' '))
			l[--n] = 0;
		if (*l == '-') {
			source_t *s = find_source(l + 1);
			if (s)
				s->enabled = false;
		} else if (*l && Str_NICmp(l, "//", 2)) {
			add_source(l, false);
		}
		l = e ? e + 1 : NULL;
	}
	Mem_Free(text);
	load_installed();
}

/* --- reading package lists ---------------------------------------------- */

static void parse_list(int src, const char *text)
{
	cmd_tokens_t *t = Mem_Alloc(sizeof(*t));
	bool first = true;
	int count = 0;
	for (const char *l = text; l && *l;) {
		const char *e = strchr(l, '\n');
		char line[4096];
		size_t n = e ? (size_t)(e - l) : strlen(l);
		if (n >= sizeof(line))
			n = sizeof(line) - 1;
		memcpy(line, l, n);
		line[n] = 0;
		l = e ? e + 1 : NULL;
		Cmd_TokenizeInto(t, line);
		if (!t->argc)
			continue;
		if (first) {
			first = false;
			if (strcmp(t->argv[0], "BWPKG") || t->argc < 2 || atoi(t->argv[1]) != 1) {
				Con_Printf("pkg: %s isn't a package list\n", sources[src].url);
				break;
			}
			continue;
		}
		if (strcmp(t->argv[0], "package") || t->argc < 2)
			continue;
		pkg_t q;
		memset(&q, 0, sizeof(q));
		Str_Copy(q.name, t->argv[1], sizeof(q.name));
		for (int a = 2; a + 1 < t->argc; a += 2) {
			const char *k = t->argv[a], *v = t->argv[a + 1];
#define F(key, field) else if (!strcmp(k, key)) Str_Copy(q.field, v, sizeof(q.field));
			if (!strcmp(k, "size"))
				q.size = atol(v);
			F("version", version) F("category", category) F("file", file) F("url", url) F("sha256", sha256) F("depends", depends)
			F("title", title) F("desc", desc) F("author", author) F("license", license) F("website", website)
#undef F
		}
		if (!q.file[0] || !FS_SafeName(q.file) || !q.url[0]) {
			Con_DPrintf("pkg: %s: package %s has no usable file or url\n", sources[src].url, q.name);
			continue;
		}
		pkg_t *p = find(q.name);
		/* the newest version wins when two sources list one package */
		if (p && p->source >= 0 && vercmp(p->version, q.version) >= 0)
			continue;
		if (!p)
			p = new_pkg(q.name);
		q.installed = p->installed;
		q.disabled = p->disabled;
		q.corrupt = p->corrupt;
		q.mark = p->mark;
		Str_Copy(q.installedver, p->installedver, sizeof(q.installedver));
		if (p->installed && strcmp(p->file, q.file))
			Str_Copy(q.file, p->file, sizeof(q.file)); /* where it already is */
		q.source = src;
		*p = q;
		count++;
	}
	Mem_Free(t);
	Con_DPrintf("pkg: %d packages from %s\n", count, sources[src].url);
}

static void fetch_sources(bool again)
{
	for (int i = 0; i < numsources; i++) {
		source_t *s = &sources[i];
		if (!s->enabled || s->state == SRC_FETCHING || (s->state == SRC_OK && !again))
			continue;
		if (is_url(s->url)) {
			s->h = HTTP_Get(s->url, NULL);
			s->state = s->h ? SRC_FETCHING : SRC_FAILED;
			continue;
		}
		char *text = read_file(local_path(s->url), NULL);
		if (!text)
			text = read_file(base_path(local_path(s->url)), NULL);
		if (text) {
			parse_list(i, text);
			s->state = SRC_OK;
			Mem_Free(text);
		} else {
			Con_Printf("pkg: can't read %s\n", s->url);
			s->state = SRC_FAILED;
		}
	}
}

static int pending_sources(void)
{
	int n = 0;
	for (int i = 0; i < numsources; i++)
		n += sources[i].state == SRC_FETCHING;
	return n;
}

/* --- making changes ------------------------------------------------------ */

static bool sha_matches(const char *path, const char *want)
{
	size_t len;
	char hex[80];
	if (!want[0])
		return true;
	char *data = read_file(path, &len);
	if (!data)
		return false;
	bool ok = Digest_Hex("sha256", data, len, hex, sizeof(hex)) && !Str_ICmp(hex, want);
	Mem_Free(data);
	return ok;
}

static bool copy_file(const char *from, const char *to)
{
	size_t len;
	char *data = read_file(from, &len);
	if (!data)
		return false;
	FILE *f = fopen(to, "wb");
	bool ok = f && fwrite(data, 1, len, f) == len;
	if (f)
		fclose(f);
	Mem_Free(data);
	return ok;
}

static void finish_file(job_t *j, bool ok, const char *error)
{
	pkg_t *p = j->name[0] ? find(j->name) : NULL;
	if (ok && !sha_matches(j->tmp, j->sha256)) {
		ok = false;
		error = "the download doesn't match its sha256";
	}
	if (ok && rename(j->tmp, j->dest)) {
		ok = false;
		error = strerror(errno);
	}
	if (!ok) {
		unlink(j->tmp);
		Con_Printf("^1pkg: %s failed: %s\n", j->name[0] ? j->name : j->dest, error);
		jobs_failed = true;
		return;
	}
	if (p) {
		p->installed = true;
		p->disabled = false;
		p->corrupt = false;
		Str_Copy(p->installedver, p->version, sizeof(p->installedver));
		Con_Printf("pkg: installed %s %s\n", p->name, p->version);
	} else {
		Con_Printf("downloaded %s\n", j->dest);
	}
}

/* starts fetching url into dest (OS paths) */
static void queue_file(const char *name, const char *url, const char *dest, const char *sha256)
{
	job_t *j = Mem_Alloc(sizeof(*j));
	j->kind = JOB_FILE;
	Str_Copy(j->name, name, sizeof(j->name));
	Str_Copy(j->dest, dest, sizeof(j->dest));
	snprintf(j->tmp, sizeof(j->tmp), "%s.tmp", dest);
	Str_Copy(j->sha256, sha256 ? sha256 : "", sizeof(j->sha256));
	FS_CreatePath(dest);
	if (is_url(url)) {
		j->h = HTTP_Get(url, j->tmp);
		if (!j->h) {
			Con_Printf("^1pkg: can't fetch %s\n", url);
			jobs_failed = true;
			Mem_Free(j);
			return;
		}
		j->next = jobs;
		jobs = j;
		return;
	}
	/* a local file: copied at once */
	const char *src = local_path(url);
	bool ok = copy_file(src, j->tmp) || copy_file(base_path(src), j->tmp);
	finish_file(j, ok, "can't read the file");
	Mem_Free(j);
}

static void finish_apply(void)
{
	applying = false;
	save_installed();
	FS_Restart();
	Con_Printf(jobs_failed ? "^3pkg: some changes failed\n" : "pkg: changes applied\n");
}

static void finish_jobs(void)
{
	if (applying)
		finish_apply();
	if (after_gamedir[0] && !jobs_failed && strcmp(after_gamedir, FS_GameDir())) {
		Con_Printf("switching gamedir for %s\n", after_gamedir);
		Mf_SwitchGamedir(after_gamedir);
		if (after_jobs.data)
			Mf_Then(after_jobs.data);
	} else if (after_gamedir[0] || after_jobs.data) {
		FS_Restart();
		if (after_jobs.data && !jobs_failed)
			Cbuf_AddText(after_jobs.data);
	}
	SB_Free(&after_jobs);
	after_gamedir[0] = 0;
	jobs_failed = false;
}

static void apply(void)
{
	int changes = 0;
	jobs_failed = false;
	Con_Printf("Applying package changes\n");
	for (int i = 0; i < numpkgs; i++) {
		pkg_t *p = &pkgs[i];
		const char *path = base_path(p->file);
		const char *off = va("%s.disabled", path);
		switch (p->mark) {
		case MARK_DISABLE:
			if (p->installed && !p->disabled) {
				if (rename(path, off) && errno != ENOENT)
					Con_Printf("^1pkg: can't disable %s: %s\n", p->name, strerror(errno));
				else
					p->disabled = true, Con_Printf("pkg: disabled %s\n", p->name);
			}
			break;
		case MARK_PURGE:
			unlink(path);
			unlink(off);
			if (p->installed)
				Con_Printf("pkg: removed %s\n", p->name);
			p->installed = p->disabled = false;
			p->installedver[0] = 0;
			if (p->source < 0) {
				/* known only because it was installed: forget it */
				*p = pkgs[--numpkgs];
				i--;
				changes++;
				continue;
			}
			break;
		case MARK_INSTALL:
		case MARK_REINSTALL:
			if (p->source < 0) {
				Con_Printf("^1pkg: %s isn't in any package list\n", p->name);
				break;
			}
			/* a disabled copy of the same version needs no download */
			if (p->mark == MARK_INSTALL && p->disabled && !strcmp(p->installedver, p->version) && !rename(off, path)) {
				p->disabled = false;
				Con_Printf("pkg: enabled %s\n", p->name);
				break;
			}
			if (p->mark == MARK_INSTALL && p->installed && !p->disabled && !p->corrupt &&
			    !strcmp(p->installedver, p->version))
				break;
			queue_file(p->name, p->url, path, p->sha256);
			break;
		default:
			continue;
		}
		p->mark = MARK_NONE;
		changes++;
	}
	if (!changes) {
		Con_Printf("pkg: nothing to change\n");
		return;
	}
	applying = true;
	if (!jobs)
		finish_jobs();
}

/* marks name (and what it depends on) for install */
static bool mark_install(const char *name, int mark, int depth)
{
	pkg_t *p = find(name);
	if (!p || p->source < 0)
		return false;
	p->mark = mark;
	if (depth < 8 && p->depends[0]) {
		char deps[256];
		Str_Copy(deps, p->depends, sizeof(deps));
		for (char *d = strtok(deps, ";, "); d; d = strtok(NULL, ";, ")) {
			pkg_t *q = find(d);
			if (q && q->installed && !q->disabled)
				continue;
			char depname[64];
			Str_Copy(depname, d, sizeof(depname));
			if (!mark_install(depname, MARK_INSTALL, depth + 1))
				Con_Printf("^3pkg: %s needs %s, which no list has\n", name, depname);
		}
	}
	return true;
}

/* --- the command --------------------------------------------------------- */

static const char *flags(const pkg_t *p)
{
	static char f[8];
	int n = 0;
	f[n++] = p->installed ? (p->disabled ? 'd' : 'I') : ' ';
	f[n++] = p->mark == MARK_INSTALL ? 'M' : p->mark == MARK_REINSTALL ? 'R' : p->mark == MARK_DISABLE ? '-' :
	         p->mark == MARK_PURGE ? 'X' : ' ';
	f[n++] = p->installed && p->source >= 0 && vercmp(p->installedver, p->version) < 0 ? 'U' : ' ';
	f[n++] = p->corrupt ? '!' : ' ';
	f[n] = 0;
	return f;
}

static void print_sources(void)
{
	int enabled = 0;
	for (int i = 0; i < numsources; i++) {
		const source_t *s = &sources[i];
		const char *state = s->state == SRC_FETCHING ? " (fetching)" : s->state == SRC_FAILED ? " ^1(failed)^7" : "";
		Con_Printf(" %s%s%s%s\n", s->url, s->implicit ? " (implicit)" : "", s->enabled ? "" : " (disabled)", state);
		enabled += s->enabled;
	}
	Con_Printf("<%d sources, %d enabled>\n", numsources, enabled);
}

static void print_pkg_line(const pkg_t *p)
{
	const char *ver = p->installed && strcmp(p->installedver, p->version) && p->source >= 0
	                      ? va("%s -> %s", p->installedver, p->version)
	                      : (p->installed ? p->installedver : p->version);
	Con_Printf(" %s %s (%s)%s%s\n", flags(p), p->name, ver, p->title[0] ? " - " : "", p->title);
}

static void show(const pkg_t *p)
{
	Con_Printf("%s (%s)\n", p->name, p->version);
	if (p->title[0])
		Con_Printf("\ttitle: %s\n", p->title);
	if (p->desc[0])
		Con_Printf("\t%s\n", p->desc);
	if (p->license[0])
		Con_Printf("\tlicense: %s\n", p->license);
	if (p->author[0])
		Con_Printf("\tauthor: %s\n", p->author);
	if (p->website[0])
		Con_Printf("\twebsite: %s\n", p->website);
	if (p->source >= 0)
		Con_Printf("\tsource: %s\n", sources[p->source].url);
	Con_Printf("\tfile: %s\n", p->file);
	if (p->size)
		Con_Printf("\tsize: %ld bytes\n", p->size);
	if (p->depends[0])
		Con_Printf("\tdepends: %s\n", p->depends);
	static const char *const marks[] = {NULL, "package is flagged to be installed", "package is flagged to be re-installed",
	                                    "package is flagged to be disabled", "package is flagged to be purged"};
	if (marks[p->mark])
		Con_Printf("\t%s\n", marks[p->mark]);
	if (p->corrupt)
		Con_Printf("\tpackage is corrupt (its file is missing)\n");
	else if (p->installed && p->disabled)
		Con_Printf("\tpackage is disabled (version %s)\n", p->installedver);
	else if (p->installed)
		Con_Printf("\tpackage is currently installed (version %s)\n", p->installedver);
	else
		Con_Printf("\tpackage is not installed\n");
	if (p->installed && p->source >= 0 && vercmp(p->installedver, p->version) < 0)
		Con_Printf("\tversion %s is available\n", p->version);
}

static bool matches(const pkg_t *p, const char *text)
{
	const char *fields[] = {p->name, p->title, p->desc, p->author};
	for (int i = 0; i < 4; i++)
		for (const char *s = fields[i]; *s; s++)
			if (!Str_NICmp(s, text, strlen(text)))
				return true;
	return false;
}

static void Pkg_f(int argc, const char **argv)
{
	const char *act = argc > 1 ? argv[1] : "list";
	ensure_loaded();
	if (!strcmp(act, "list") || !strcmp(act, "search") || !strcmp(act, "show") || !strcmp(act, "add") ||
	    !strcmp(act, "upgrade") || !strcmp(act, "apply") || !strcmp(act, "reinstall"))
		fetch_sources(false);
	int waiting = pending_sources();

	if (!strcmp(act, "list") || !strcmp(act, "search")) {
		bool search = !strcmp(act, "search");
		if (search && argc < 3) {
			Con_Printf("pkg search <text>\n");
			return;
		}
		if (!search)
			print_sources();
		for (int i = 0; i < numpkgs; i++)
			if (!search || matches(&pkgs[i], argv[2]))
				print_pkg_line(&pkgs[i]);
		if (waiting)
			Con_Printf("<%d list%s still downloading>\n", waiting, waiting == 1 ? "" : "s");
		Con_Printf("<end of list>\n");
	} else if (!strcmp(act, "show")) {
		pkg_t *p = argc > 2 ? find(argv[2]) : NULL;
		if (p)
			show(p);
		else
			Con_Printf("<package not found>\n");
	} else if (!strcmp(act, "add") || !strcmp(act, "rem") || !strcmp(act, "uninstall") || !strcmp(act, "del") ||
	           !strcmp(act, "purge") || !strcmp(act, "reinstall")) {
		int mark = !strcmp(act, "add") ? MARK_INSTALL : !strcmp(act, "reinstall") ? MARK_REINSTALL :
		           (!strcmp(act, "del") || !strcmp(act, "purge")) ? MARK_PURGE : MARK_DISABLE;
		for (int i = 2; i < argc; i++) {
			pkg_t *p = find(argv[i]);
			if (!p) {
				Con_Printf("%s: package %s not known\n", argv[0], argv[i]);
				continue;
			}
			if (mark == MARK_INSTALL || mark == MARK_REINSTALL) {
				if (!mark_install(argv[i], mark, 0))
					Con_Printf("%s: unable to activate/mark %s\n", argv[0], argv[i]);
			} else if (!p->installed && mark == MARK_DISABLE) {
				p->mark = MARK_NONE; /* unmarks a pending install */
			} else
				p->mark = mark;
		}
	} else if (!strcmp(act, "upgrade")) {
		int n = 0;
		for (int i = 0; i < numpkgs; i++) {
			pkg_t *p = &pkgs[i];
			if (p->installed && !p->disabled && p->source >= 0 && vercmp(p->installedver, p->version) < 0)
				p->mark = MARK_REINSTALL, n++;
		}
		if (n)
			Con_Printf("%d packages flagged\n", n);
		else if (waiting)
			Con_Printf("pkg: %d package list%s still downloading\n", waiting, waiting == 1 ? "" : "s");
		else
			Con_Printf("Already using latest versions of all packages\n");
	} else if (!strcmp(act, "revert") || !strcmp(act, "reset")) {
		for (int i = 0; i < numpkgs; i++)
			pkgs[i].mark = MARK_NONE;
	} else if (!strcmp(act, "changes") || !strcmp(act, "listmarked")) {
		int n = 0;
		for (int i = 0; i < numpkgs; i++)
			if (pkgs[i].mark)
				print_pkg_line(&pkgs[i]), n++;
		Con_Printf("<%d change%s>\n", n, n == 1 ? "" : "s");
	} else if (!strcmp(act, "apply")) {
		if (applying)
			Con_Printf("pkg: changes are still being applied\n");
		else if (waiting)
			Con_Printf("pkg: %d package list%s still downloading, try again shortly\n", waiting, waiting == 1 ? "" : "s");
		else
			apply();
	} else if (!strcmp(act, "sources") || !strcmp(act, "listenabledsources")) {
		print_sources();
	} else if (!strcmp(act, "addsource") || !strcmp(act, "remsource")) {
		bool add = !strcmp(act, "addsource");
		if (argc < 3) {
			Con_Printf("pkg %s <url>\n", act);
			return;
		}
		source_t *s = find_source(argv[2]);
		if (add) {
			if (s)
				s->enabled = true;
			else
				s = add_source(argv[2], false);
			if (s)
				fetch_sources(false);
		} else if (s && s->implicit) {
			s->enabled = false;
		} else if (s) {
			int i = (int)(s - sources);
			if (s->h)
				HTTP_Free(s->h);
			/* its packages go, apart from installed ones, which lose their source */
			for (int k = 0; k < numpkgs; k++)
				if (pkgs[k].source == i && !pkgs[k].installed)
					pkgs[k--] = pkgs[--numpkgs];
				else if (pkgs[k].source == i)
					pkgs[k].source = -1;
				else if (pkgs[k].source > i)
					pkgs[k].source--;
			memmove(s, s + 1, (size_t)(numsources - i - 1) * sizeof(*s));
			numsources--;
		} else {
			Con_Printf("pkg: no source %s\n", argv[2]);
			return;
		}
		save_sources();
	} else if (!strcmp(act, "refresh")) {
		fetch_sources(true);
	} else {
		Con_Printf("%s: Unknown action %s\n", argv[0], act);
		Con_Printf("Should be one of list, show, search, upgrade, revert, add, rem, del, changes, apply, sources, "
		           "addsource, remsource\n");
	}
}

/* fs_changemod [gamedir <dir>] [package <file> <url> [<sha256>]]... [map|spmap <map>]
 * the back end of an online mod installer: fetches what's missing, switches
 * to the mod and starts its map */
static void FsChangemod_f(int argc, const char **argv)
{
	char dir[64] = "";
	char map[64] = "";
	if (argc < 2) {
		Con_Printf("fs_changemod [gamedir <dir>] [package <file> <url> [sha256]] [map|spmap <map>]\n");
		return;
	}
	if (jobs) {
		Con_Printf("fs_changemod: still downloading\n");
		return;
	}
	jobs_failed = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "gamedir") && i + 1 < argc) {
			Str_Copy(dir, argv[++i], sizeof(dir));
			if (!FS_SafeName(dir) || strchr(dir, '/')) {
				Con_Printf("fs_changemod: bad gamedir %s\n", dir);
				return;
			}
		} else if (!strcmp(argv[i], "package") && i + 2 < argc) {
			const char *file = argv[i + 1], *url = argv[i + 2];
			const char *sha = i + 3 < argc && strlen(argv[i + 3]) == 64 ? argv[i + 3] : NULL;
			i += sha ? 3 : 2;
			if (!FS_SafeName(file)) {
				Con_Printf("fs_changemod: bad file name %s\n", file);
				continue;
			}
			const char *dest = base_path(file);
			if (!exists(dest))
				queue_file("", url, dest, sha);
		} else if ((!strcmp(argv[i], "map") || !strcmp(argv[i], "spmap")) && i + 1 < argc) {
			Str_Copy(map, argv[++i], sizeof(map));
		} else {
			Con_Printf("unsupported args: %s\n", argv[i]);
			return;
		}
	}
	Str_Copy(after_gamedir, dir, sizeof(after_gamedir));
	SB_Free(&after_jobs);
	if (map[0])
		SB_Printf(&after_jobs, "set deathmatch 0; set coop 0; map \"%s\"\n", map);
	if (!jobs)
		finish_jobs();
}

/* fs_changegame with a url: the manifest comes from the web */
static void fetch_manifest(const char *url)
{
	job_t *j = Mem_Alloc(sizeof(*j));
	j->kind = JOB_MANIFEST;
	Str_Copy(j->dest, url, sizeof(j->dest));
	j->h = HTTP_Get(url, NULL);
	if (!j->h) {
		Con_Printf("fs_changegame: can't fetch %s\n", url);
		Mem_Free(j);
		return;
	}
	j->next = jobs;
	jobs = j;
}

/* after the startup configs: the update check pkg_autoupdate asks for */
static void startup(void)
{
	if (!pkg_autoupdate->ivalue)
		return;
	ensure_loaded();
	fetch_sources(false);
	announce = true;
}

void PKG_Frame(void)
{
	static bool started;
	if (!started) {
		started = true;
		startup();
	}
	for (int i = 0; i < numsources; i++) {
		source_t *s = &sources[i];
		if (s->state != SRC_FETCHING || HTTP_State(s->h) == HTTP_RUNNING)
			continue;
		if (HTTP_State(s->h) == HTTP_DONE && HTTP_Code(s->h) / 100 == 2) {
			size_t len;
			const char *body = HTTP_Body(s->h, &len);
			parse_list(i, body ? body : "");
			s->state = SRC_OK;
		} else {
			Con_Printf("pkg: can't fetch %s: %s\n", s->url, HTTP_Error(s->h));
			s->state = SRC_FAILED;
		}
		HTTP_Free(s->h);
		s->h = NULL;
	}
	if (announce && !pending_sources()) {
		int n = 0;
		announce = false;
		for (int i = 0; i < numpkgs; i++)
			n += pkgs[i].installed && pkgs[i].source >= 0 && vercmp(pkgs[i].installedver, pkgs[i].version) < 0;
		if (n)
			Con_Printf("^3%d package update%s available: pkg upgrade; pkg apply\n", n, n == 1 ? "" : "s");
	}
	if (!jobs)
		return;
	for (job_t **pj = &jobs; *pj;) {
		job_t *j = *pj;
		int st = HTTP_State(j->h);
		if (st == HTTP_RUNNING) {
			pj = &j->next;
			continue;
		}
		bool ok = st == HTTP_DONE && HTTP_Code(j->h) / 100 == 2;
		const char *err = st == HTTP_DONE ? va("HTTP %d", HTTP_Code(j->h)) : HTTP_Error(j->h);
		if (j->kind == JOB_FILE)
			finish_file(j, ok, err);
		else if (!ok)
			Con_Printf("fs_changegame: can't fetch %s: %s\n", j->dest, err);
		else {
			manifest_t mf;
			size_t len;
			const char *body = HTTP_Body(j->h, &len);
			if (Mf_Parse(&mf, body ? body : "", j->dest))
				Mf_Switch(&mf);
		}
		HTTP_Free(j->h);
		*pj = j->next;
		Mem_Free(j);
		if (!jobs) {
			finish_jobs();
			return;
		}
	}
}

/* the menus' getpackagemanagerinfo, by its GPMI_ numbers; NULL past the end */
const char *PKG_Info(int n, int prop)
{
	static char out[128];
	if (n == 0) {
		ensure_loaded();
		fetch_sources(false);
	}
	if (n < 0 || n >= numpkgs)
		return NULL;
	const pkg_t *p = &pkgs[n];
	switch (prop) {
	case 0: snprintf(out, sizeof(out), "%s=%s", p->name, p->version); return out;
	case 1: return p->category;
	case 2: return p->title[0] ? p->title : p->name;
	case 3: return p->version;
	case 4: return p->desc;
	case 5: return p->license;
	case 6: return p->author;
	case 7: return p->website;
	case 8: /* installed state, or the percentage while downloading */
		for (job_t *j = jobs; j; j = j->next)
			if (!strcmp(j->name, p->name)) {
				size_t total, got = HTTP_Progress(j->h, &total);
				if (!total)
					return "pending";
				snprintf(out, sizeof(out), "%d%%", (int)(got * 100 / total));
				return out;
			}
		return p->corrupt ? "corrupt" : !p->installed ? "" : p->disabled ? "present" : "enabled";
	case 9: {
		static const char *const actions[] = {"", "user", "reinstall", "disable", "purge"};
		return p->mark ? actions[p->mark] : (p->installed && !p->disabled ? "user" : "");
	}
	case 10: return p->source >= 0 ? "1" : "";
	case 11: snprintf(out, sizeof(out), "%ld", p->size); return out;
	case 12: {
		const char *slash = strchr(p->file, '/');
		snprintf(out, sizeof(out), "%.*s", slash ? (int)(slash - p->file) : 0, p->file);
		return out;
	}
	default: return "";
	}
}

/* how far the downloads are, for the console's download line */
bool PKG_Progress(char *out, size_t outsize)
{
	size_t got = 0, total = 0;
	int n = 0;
	for (job_t *j = jobs; j; j = j->next) {
		size_t t;
		got += HTTP_Progress(j->h, &t);
		total += t;
		n++;
	}
	if (!n)
		return false;
	if (total)
		snprintf(out, outsize, "%d download%s, %d%%", n, n == 1 ? "" : "s", (int)(got * 100 / total));
	else
		snprintf(out, outsize, "%d download%s, %zu KiB", n, n == 1 ? "" : "s", got / 1024);
	return true;
}

void PKG_Init(void)
{
	pkg_autoupdate = Cvar_Get("pkg_autoupdate", "0", CVAR_ARCHIVE,
	                          "Fetch the package lists at startup and say when updates are available");
	Cmd_AddD("pkg", Pkg_f, "Package manager: list, show, search, add, rem, del, upgrade, changes, apply, sources...");
	Cmd_AddD("fs_changemod", FsChangemod_f, "Fetch a mod's files and switch to it, for online installers");
	Mf_SetFetcher(fetch_manifest);
}

