/* srcconv: runs the Source formats plugin outside the engine, to check it
 * against real files.
 *   srcconv <plugin.so> vtf <file.vtf> <out.png>
 *   srcconv <plugin.so> vmt <root> <materials/x.vmt>
 *   srcconv <plugin.so> mdl <root> <models/x.mdl> <out.iqm>
 * <root> stands in for the game directory. ISC licence, see LICENSE. */
#include "../src/common/bwplug.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../src/thirdparty/stb_image_write.h"

static const char *root = ".";
static bwplug_image_fn image_fn;
static bwplug_material_fn material_fn;
static bwplug_model_fn model_fn;

static void p_print(const char *t) { fputs(t, stdout); }
static void p_add_command(const char *n, bwplug_cmd_fn fn, const char *d) {}
static void p_add_text(const char *t) {}
static const char *p_cvar_string(const char *n)
{
	/* SRCCONV_<name>=value sets a cvar, e.g. SRCCONV_src_boneboxes=1 */
	char env[128];
	snprintf(env, sizeof(env), "SRCCONV_%s", n);
	return getenv(env) ? getenv(env) : "";
}
static void p_cvar_set(const char *n, const char *v) {}
static void p_cvar_register(const char *n, const char *v, const char *d) {}
static const char *p_gamedir(void) { return root; }
static void *p_alloc(size_t n) { return calloc(1, n ? n : 1); }
static void p_free(void *p) { free(p); }

static void *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *d;
	long n;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = calloc(1, (size_t)n + 1);
	if (fread(d, 1, (size_t)n, f) != (size_t)n) {
		free(d);
		d = NULL;
	}
	fclose(f);
	if (len)
		*len = (size_t)n;
	return d;
}

static void *p_load_file(const char *path, size_t *len)
{
	char full[1024];
	snprintf(full, sizeof(full), "%s/%s", root, path);
	return read_file(full, len);
}

static int p_file_exists(const char *path)
{
	size_t n;
	void *d = p_load_file(path, &n);
	free(d);
	return d != NULL;
}

static void p_reg_image(const char *e, bwplug_image_fn fn) { image_fn = fn; }
static void p_reg_material(const char *e, bwplug_material_fn fn) { material_fn = fn; }
static void p_reg_model(const char *e, bwplug_model_fn fn) { model_fn = fn; }

static const bwplug_api_t api = {BWPLUG_VERSION, p_print,       p_add_command, p_add_text,     p_cvar_string,
                                 p_cvar_set,     p_cvar_register, p_gamedir,   p_alloc,        p_free,
                                 p_load_file,    p_file_exists, p_reg_image,   p_reg_material, p_reg_model};

int main(int argc, char **argv)
{
	void *h;
	int (*init)(const bwplug_api_t *);
	size_t len;
	void *d;
	if (argc < 4) {
		fprintf(stderr, "usage: srcconv <plugin.so> vtf <in> <out.png> | vmt <root> <path> | mdl <root> <path> <out.iqm>\n");
		return 2;
	}
	if (!(h = dlopen(argv[1], RTLD_NOW)) || !(*(void **)&init = dlsym(h, "bwplug_init")) || !init(&api)) {
		fprintf(stderr, "can't load %s: %s\n", argv[1], dlerror());
		return 1;
	}
	if (!strcmp(argv[2], "vtf") && argc >= 5) {
		int w, hh;
		unsigned char *px;
		if (!(d = read_file(argv[3], &len)) || !(px = image_fn(d, len, &w, &hh)))
			return 1;
		printf("%dx%d\n", w, hh);
		return stbi_write_png(argv[4], w, hh, 4, px, w * 4) ? 0 : 1;
	}
	if (!strcmp(argv[2], "vmt") && argc >= 5) {
		bwplug_material_t m = {0};
		root = argv[3];
		if (!(d = p_load_file(argv[4], &len)) || !material_fn(argv[4], d, len, &m))
			return 1;
		printf("diffuse %s\nnormal %s\nflags %d\nalpha %g\n", m.diffuse, m.normal, m.flags, m.alpha);
		return 0;
	}
	if (!strcmp(argv[2], "mdl") && argc >= 6) {
		void *q;
		size_t qlen;
		FILE *f;
		root = argv[3];
		if (!(d = p_load_file(argv[4], &len)) || !(q = model_fn(argv[4], d, len, &qlen)))
			return 1;
		if (!(f = fopen(argv[5], "wb")) || fwrite(q, 1, qlen, f) != qlen)
			return 1;
		fclose(f);
		printf("%zu bytes\n", qlen);
		return 0;
	}
	fprintf(stderr, "bad arguments\n");
	return 2;
}
