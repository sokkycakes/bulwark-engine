/* Bulwark engine - UI views on top of PanoramaEngine (MIT, third_party/).
 * This file is the only place that knows about the library: it feeds it
 * files from the engine's file system, hands its draw list to the
 * renderer's 2D API and routes input and script calls. ISC licence, see
 * LICENSE. */
#include "ui.h"

extern "C" {
#include "../common/bmfont.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../client/keys.h"
#include "../render/render.h"
int Key_NumForName(const char *name);
bool Key_IsDown(int key);
const char *Key_NameForNum(int key);
}

#include "ui/panorama/panorama_font_atlas.hpp"
#include "ui/panorama/panorama_paint.hpp"
#include "ui/panorama/panorama_render_backend.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"
#include "ui/panorama/panorama_runtime.hpp"
#include "ui/panorama/panorama_view.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef BW_HAVE_GDIFONT
#include "../client/gdifont.h"
#endif

using namespace panorama;

namespace {

/* every UI file lives under panorama/ in the game dirs, except what a
 * layout names as {game}/<path>: any game file (a game's own scripts and
 * art, used in place) */
std::string ui_path(std::string_view path)
{
	std::string p(path);
	size_t g = p.find("{game}/");
	if (g != std::string::npos)
		return p.substr(g + 7);
	if (p.rfind("panorama/", 0) == 0)
		return p;
	return "panorama/" + p;
}

bool load_bytes(const std::string &path, std::vector<unsigned char> &out)
{
	size_t len = 0;
	char *data = FS_LoadFile(path.c_str(), &len);
	if (!data)
		return false;
	out.assign(data, data + len);
	Mem_Free(data);
	return true;
}

class FsProvider final : public PanoramaResourceProvider {
public:
	bool read(std::string_view path, PanoramaResource &out) const override
	{
		std::string full = ui_path(path);
		if (!load_bytes(full, out.bytes))
			return false;
		out.source = full;
		return true;
	}
};

/* textures the library makes (the glyph atlas) and images it names */
class Backend final : public PanoramaRenderBackend {
public:
	struct Texture {
		int handle, w, h;
		bool premultiplied;
	};

	PanoramaTextureId generate_texture(std::span<const unsigned char> rgba, int w, int h) override
	{
		PanoramaTextureId id = next++;
		char name[64];
		snprintf(name, sizeof(name), "_ui/generated%lu", (unsigned long)id);
		/* the library generates only the glyph atlas, whose texels are
		 * premultiplied (coverage in every channel) */
		textures[id] = {R_UploadImage(name, rgba.data(), w, h), w, h, true};
		return id;
	}
	PanoramaTextureId load_texture(std::string_view source, int &w, int &h) override
	{
		std::string name(source);
		int handle = R_ImageHandle(name.c_str());
		w = h = 0;
		if (!handle)
			return 0;
		R_ImageSize(handle, &w, &h);
		PanoramaTextureId id = next++;
		textures[id] = {handle, w, h, false};
		return id;
	}
	void release_texture(PanoramaTextureId id) override { textures.erase(id); }
	bool update_texture(PanoramaTextureId id, std::span<const unsigned char> rgba, int w, int h) override
	{
		auto it = textures.find(id);
		if (it == textures.end() || it->second.w != w || it->second.h != h)
			return false;
		R_UpdateImage(it->second.handle, 0, 0, w, h, rgba.data());
		return true;
	}
	bool update_texture_regions(PanoramaTextureId id, int tw, int th,
	                            std::span<const PanoramaTextureRegionUpdate> regions) override
	{
		auto it = textures.find(id);
		if (it == textures.end() || it->second.w != tw || it->second.h != th)
			return false;
		for (const PanoramaTextureRegionUpdate &r : regions) {
			size_t tight = (size_t)r.width * 4;
			if (r.row_pitch == 0 || r.row_pitch == tight) {
				R_UpdateImage(it->second.handle, r.x, r.y, r.width, r.height, r.rgba.data());
				continue;
			}
			std::vector<unsigned char> packed(tight * r.height);
			for (int y = 0; y < r.height; y++)
				memcpy(&packed[tight * y], r.rgba.data() + r.row_pitch * y, tight);
			R_UpdateImage(it->second.handle, r.x, r.y, r.width, r.height, packed.data());
		}
		return true;
	}

	/* views draw from their draw lists directly (see draw_view), so compiled
	 * geometry is never used */
	PanoramaCompiledGeometryHandle compile_geometry(std::span<const PanoramaPaintVertex>, std::span<const int>,
	                                                float) override
	{
		return 0;
	}
	void render_geometry(PanoramaCompiledGeometryHandle, PanoramaTextureId, const PanoramaDrawConstants &) override {}
	void release_geometry(PanoramaCompiledGeometryHandle) override {}

	int handle(PanoramaTextureId id) const
	{
		auto it = textures.find(id);
		return it == textures.end() ? 0 : it->second.handle;
	}
	bool premultiplied(PanoramaTextureId id) const
	{
		auto it = textures.find(id);
		return it != textures.end() && it->second.premultiplied;
	}

private:
	std::unordered_map<PanoramaTextureId, Texture> textures;
	PanoramaTextureId next = 1;
};

/* scripts talk to the engine through this object; native code calls
 * Bulwark.__recv to deliver UI_Post events */
const char *bulwark_prelude = R"JS(
var Bulwark = (function () {
	var handlers = {};
	return {
		Send: function (action, arg) { $.__host(String(action), arg === undefined ? "" : String(arg)); },
		Cmd: function (command) { $.__host("cmd", String(command)); },
		Query: function (request) { return fte_query(request); },
		/* the view's size in layout pixels (the height is ui_designheight for a full-screen view) */
		Width: function () { return __bwSize()[0]; },
		Height: function () { return __bwSize()[1]; },
		/* the mouse pointer in the view, in layout pixels */
		MouseX: function () { return __bwSize()[2]; },
		MouseY: function () { return __bwSize()[3]; },
		On: function (event, fn) { (handlers[event] = handlers[event] || []).push(fn); },
		__recv: function (event, value) {
			var list = handlers[event];
			if (!list)
				return;
			for (var i = 0; i < list.length; i++)
				list[i](value);
		}
	};
})();

/* fte_query(request): a synchronous request to the engine. The reply comes
 * back as an attribute of the document root, written while $.__host runs. */
function __bwRoot() {
	var r = $.GetContextPanel();
	while (r && r.GetParent())
		r = r.GetParent();
	return r;
}
function __bwSize() {
	var r = __bwRoot();
	$.__host("__size", "");
	return r ? [parseFloat(r.GetAttributeString("bwwidth", "0")), parseFloat(r.GetAttributeString("bwheight", "0")),
	            parseFloat(r.GetAttributeString("bwmousex", "0")), parseFloat(r.GetAttributeString("bwmousey", "0"))]
	         : [0, 0, 0, 0];
}
var fte_query = (function () {
	var root = null;
	return function (request) {
		if (!root) {
			root = $.GetContextPanel();
			while (root && root.GetParent())
				root = root.GetParent();
		}
		$.__host("__query", String(request));
		return root ? root.GetAttributeString("bwreply", "") : "";
	};
})();
var window = globalThis;
)JS";

/* a JS string literal, so engine text can't escape into code */
std::string js_quote(const char *s)
{
	std::string out = "\"";
	for (; s && *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			out += '\\';
			out += (char)c;
		} else if (c < 0x20) {
			char buf[8];
			snprintf(buf, sizeof(buf), "\\u%04x", c);
			out += buf;
		} else {
			out += (char)c;
		}
	}
	return out + "\"";
}

struct View {
	int num = 0;
	int flags = 0;
	std::string layout;
	PanoramaView pv;
	bool images_resolved = false;
	/* the draw list converted to renderer vertices, rebuilt when it changes */
	struct Batch {
		int image;
		int blend;
		bool clip;
		float cx, cy, cw, ch;
		std::vector<r2dvert_t> verts;
		std::vector<uint16_t> idx;
	};
	std::vector<Batch> batches;
	bool batches_dirty = true;
	/* closed while its own script may still be running: freed next frame */
	bool dead = false;
	/* UI_EMBED views: the size they were last drawn at, in virtual pixels */
	float embed_w = 0, embed_h = 0;
	float drawn_scale = 0; /* virtual pixels per design unit the batches were built for */
	/* the layout's own design height and pixelated look (attributes
	 * bwdesignheight / bwpixelated on a top-level panel); 0 / -1 follow
	 * ui_designheight / ui_pixelated */
	float design = 0;
	int px = -1;
	/* bwexacttext="13": text up to this size keeps its own pixel size */
	float exact = 0;
	float atlas_key = 0; /* the font atlas scale the view was given */
	float atlas_exact = 0;
};

Backend backend;
/* glyphs are baked for one scale per atlas: views at another scale (their
 * own design height, or pixelated) get an atlas of their own */
struct ScaledAtlas {
	float scale;
	std::unique_ptr<PanoramaFontAtlas> atlas;
	bool loaded;
	float exact = 0; /* set_exact_pixel_max */
};
std::vector<ScaledAtlas> atlases;
bool fonts_loaded;
bool fonts_dirty; /* UI_AddFont added a face: reload before the next frame */
std::vector<std::unique_ptr<View>> views;
int next_view = 1;
ui_action_fn action_handler;
ui_query_fn query_handler;
/* fonts registered for CSS font-family (UI_AddFont): game file and font key */
struct ExtraFace {
	std::string path;
	int key;
};
std::vector<ExtraFace> extra_faces;
float mouse_x, mouse_y;
bool mouse_down;
cvar_t *ui_designheight;
cvar_t *ui_pixelated;
/* time spent in UI_Frame and UI_Draw, for ui_stats */
double stat_update, stat_draw;
long stat_frames;

/* design units -> framebuffer pixels; layouts are authored for a fixed
 * height (ui_designheight, 1080 by default) and scale with the window */
float design_height(const View *v)
{
	float design = v && v->design >= 1 ? v->design : ui_designheight ? ui_designheight->value : 1080;
	return design < 1 ? 1080 : design;
}

float ui_scale(const View *v = nullptr)
{
	int w, h;
	R_GetSize(&w, &h);
	return h * R_Get2DScale() / design_height(v);
}

/* pixelated: the view is drawn at its design resolution (one pixel per
 * design unit) and blown up to the window with nearest filtering, so text
 * and art keep a low-resolution look; glyphs are then baked at that
 * resolution too. Embedded views are never pixelated. */
bool pixelated(const View *v)
{
	if (v->flags & UI_EMBED)
		return false;
	return v->px >= 0 ? v->px != 0 : ui_pixelated && ui_pixelated->ivalue;
}
float atlas_scale(const View *v) { return pixelated(v) ? 1.0f : ui_scale(v); }

/* layout pixels per virtual pixel */
float to_layout(const View *v) { return R_Get2DScale() / ui_scale(v); }

/* an embedded view is laid out at the size it is drawn */
void view_viewport(const View &v, float &w, float &h)
{
	float s = to_layout(&v);
	if ((v.flags & UI_EMBED) && v.embed_w > 0 && v.embed_h > 0) {
		w = v.embed_w * s;
		h = v.embed_h * s;
		return;
	}
	int vw, vh;
	R_GetSize(&vw, &vh);
	w = vw * s;
	h = vh * s;
}

int face_weight(const char *name)
{
	std::string n(name);
	std::transform(n.begin(), n.end(), n.begin(), ::tolower);
	if (n.find("black") != std::string::npos || n.find("heavy") != std::string::npos)
		return 900;
	if (n.find("bold") != std::string::npos)
		return 700;
	if (n.find("semibold") != std::string::npos || n.find("demi") != std::string::npos)
		return 600;
	if (n.find("medium") != std::string::npos)
		return 500;
	if (n.find("light") != std::string::npos)
		return 300;
	return 400;
}

/* the file's name without its extension(s) */
std::string file_stem(const char *path)
{
	const char *base = strrchr(path, '/');
	std::string stem(base ? base + 1 : path);
	if (stem.size() > 7 && !Str_ICmp(stem.c_str() + stem.size() - 7, ".pcf.gz"))
		stem.erase(stem.size() - 7);
	else if (stem.find('.') != std::string::npos)
		stem.erase(stem.rfind('.'));
	return stem;
}

bool font_file(const char *name)
{
	const char *dot = strrchr(name, '.');
	return BMF_IsBitmapName(name) ||
	       (dot && (!Str_ICmp(dot, ".ttf") || !Str_ICmp(dot, ".otf") || !Str_ICmp(dot, ".ttc")));
}

/* game files read once per search path: a .gz is inflated */
bool load_font_bytes(const std::string &path, std::vector<unsigned char> &out)
{
	if (!load_bytes(path, out))
		return false;
	if (out.size() > 2 && out[0] == 0x1f && out[1] == 0x8b) {
		size_t n = 0;
		uint8_t *raw = BMF_Gunzip(out.data(), out.size(), &n);
		if (!raw)
			return false;
		out.assign(raw, raw + n);
		Mem_Free(raw);
	}
	return true;
}

/* Pixel fonts are drawn crisp (r_pixelfonts, as for engine text): a BDF or
   PCF file from its strike, a TrueType font on a pixel grid at whole
   multiples of that grid. Worked out once per file and search path. */
std::unordered_map<std::string, float> pixel_cache;
int pixel_cache_gen = -1;

float face_pixel_ppem(const std::string &path)
{
	cvar_t *on = Cvar_Find("r_pixelfonts");
	if (on && !on->value)
		return 0;
	if (BMF_IsBitmapName(path.c_str()))
		return -1;
	if (pixel_cache_gen != FS_Generation()) {
		pixel_cache.clear();
		pixel_cache_gen = FS_Generation();
	}
	auto it = pixel_cache.find(path);
	if (it != pixel_cache.end())
		return it->second;
	std::vector<unsigned char> bytes;
	float ppem = 0;
	if (load_bytes(path, bytes) && bytes.size() <= (2u << 20))
		ppem = BMF_PixelGrid(bytes.data(), bytes.size(), nullptr);
	pixel_cache[path] = ppem;
	return ppem;
}

void add_face(const char *name, const char *searchpath, void *ctx)
{
	auto *faces = static_cast<std::vector<PanoramaFontAtlasFace> *>(ctx);
	const char *base = strrchr(name, '/');
	(void)searchpath;
	base = base ? base + 1 : name;
	for (const PanoramaFontAtlasFace &f : *faces)
		if (f.path == base)
			return;
	float ppem = face_pixel_ppem(std::string("panorama/fonts/") + base);
	faces->push_back({base, face_weight(base), ppem});
	/* each file is also a family of its own name, without the extension */
	faces->push_back({base, panorama_register_font_family(file_stem(base)) * kPanoramaFontFamilyStride + 400, ppem});
}

/* Bulwark's shared fonts, fonts/ in its data directory: every game can name
   them in CSS by family name ("Visitor TT1 BRK") or file name ("visitor1") */
void add_shared_face(const char *name, const char *searchpath, void *ctx)
{
	auto *faces = static_cast<std::vector<PanoramaFontAtlasFace> *>(ctx);
	std::vector<unsigned char> bytes;
	char family[64];
	int weight = 400;
	bool italic = false;
	if (strcmp(searchpath, "(engine)") || !font_file(name) || !load_font_bytes(name, bytes))
		return;
	if (!BMF_Describe(bytes.data(), bytes.size(), family, sizeof(family), &weight, &italic))
		family[0] = 0;
	float ppem = face_pixel_ppem(name);
	weight = BW_CLAMP(weight, 1, 1000);
	std::string stem = file_stem(name);
	faces->push_back({name, panorama_register_font_family(stem) * kPanoramaFontFamilyStride + weight, ppem});
	if (family[0] && Str_ICmp(family, stem.c_str()))
		faces->push_back({name, panorama_register_font_family(family) * kPanoramaFontFamilyStride + weight, ppem});
}

/* every font in panorama/fonts/ joins one family; weights come from the
 * file names (Bold, Medium, ...) */
#ifdef BW_HAVE_GDIFONT
void gdi_clear();
#endif
/* loads one atlas, or every atlas when only is null */
void load_fonts(ScaledAtlas *only = nullptr)
{
	PanoramaFontAtlasLoadOptions opts;
#ifdef BW_HAVE_GDIFONT
	if (!only)
		gdi_clear();
#endif
	FS_List("panorama/fonts/*.ttf", true, add_face, &opts.faces);
	FS_List("panorama/fonts/*.otf", true, add_face, &opts.faces);
	FS_List("panorama/fonts/*.bdf", true, add_face, &opts.faces);
	FS_List("panorama/fonts/*.pcf", true, add_face, &opts.faces);
	FS_List("panorama/fonts/*.pcf.gz", true, add_face, &opts.faces);
	for (const ExtraFace &f : extra_faces)
		opts.faces.push_back({f.path, f.key, face_pixel_ppem(f.path)});
	/* last, so that a game's own faces stay the fallback for unknown families */
	FS_List("fonts/*", true, add_shared_face, &opts.faces);
	if (opts.faces.empty()) {
		Con_Printf("^3ui: no fonts in panorama/fonts/; text will not draw\n");
		fonts_loaded = false;
		return;
	}
	opts.resource_root = "/nonexistent-bulwark-ui-root";
	opts.font_bytes_provider = [](std::string_view face) -> std::optional<std::vector<unsigned char>> {
		std::vector<unsigned char> bytes;
		std::string name(face);
		/* registered faces are named by their game path, the others by file name */
		if (!load_font_bytes(name.find('/') != std::string::npos ? name : "panorama/fonts/" + name, bytes))
			return std::vector<unsigned char>(); /* handled: never look on disk */
		return bytes;
	};
	fonts_loaded = true;
	for (ScaledAtlas &a : atlases) {
		if (only && &a != only)
			continue;
		a.atlas->clear();
		a.loaded = a.atlas->load(opts);
		a.atlas->set_ui_scale(a.scale);
		a.atlas->set_exact_pixel_max(a.exact);
		fonts_loaded = fonts_loaded && a.loaded;
	}
	if (!fonts_loaded)
		Con_Printf("^3ui: couldn't load the fonts in panorama/fonts/\n");
}

/* the atlas baked at this scale (and exact text size), made (and loaded) on
 * first use */
PanoramaFontAtlas *atlas_for(float scale, float exact)
{
	scale = std::clamp(scale, 0.1f, 8.0f);
	for (ScaledAtlas &a : atlases)
		if (fabsf(a.scale - scale) < 1e-4f && a.exact == exact)
			return a.loaded ? a.atlas.get() : nullptr;
	atlases.push_back({scale, std::make_unique<PanoramaFontAtlas>(), false, exact});
	load_fonts(&atlases.back());
	ScaledAtlas &a = atlases.back();
	return a.loaded ? a.atlas.get() : nullptr;
}

/* atlases no open view uses any more */
void drop_unused_atlases()
{
	atlases.erase(std::remove_if(atlases.begin(), atlases.end(),
	                             [](const ScaledAtlas &a) {
		                             for (auto &v : views)
			                             if (!v->dead && v->atlas_key == a.scale && v->atlas_exact == a.exact)
				                             return false;
		                             return true;
	                             }),
	              atlases.end());
}

/* gives the view the atlas for its current scale; true when that changed */
bool update_view_atlas(View &v)
{
	float scale = std::clamp(atlas_scale(&v), 0.1f, 8.0f);
	if (v.atlas_key == scale && v.atlas_exact == v.exact)
		return false;
	PanoramaFontAtlas *a = atlas_for(scale, v.exact);
	v.atlas_key = scale;
	v.atlas_exact = v.exact;
	if (a)
		v.pv.set_font_atlas(a);
	return true;
}

/* url("file://{images}/x.png") and friends -> a game file path */
std::string image_path(std::string src)
{
	if (src.rfind("url(", 0) == 0) {
		src = src.substr(4);
		if (!src.empty() && src.back() == ')')
			src.pop_back();
		while (!src.empty() && (src.front() == '"' || src.front() == '\''))
			src.erase(src.begin());
		while (!src.empty() && (src.back() == '"' || src.back() == '\''))
			src.pop_back();
	}
	if (src.rfind("data:", 0) == 0)
		return src;
	if (src.rfind("file://", 0) == 0)
		src = src.substr(7);
	if (src.rfind("{images}/", 0) == 0)
		return "panorama/images/" + src.substr(9);
	/* any game file, for layouts that use a game's own art */
	if (src.rfind("{game}/", 0) == 0)
		return src.substr(7);
	if (src.rfind("{resources}/", 0) == 0)
		return ui_path(src.substr(12));
	return ui_path(src);
}

/* data:image/...;base64,... (or a percent-encoded data:image/svg+xml,...)
 * decoded into an image named after its hash */
std::string data_url_image(const std::string &url)
{
	size_t comma = url.find(',');
	if (comma == std::string::npos || comma < 7)
		return std::string();
	std::vector<unsigned char> bytes;
	unsigned bits = 0;
	int nbits = 0;
	bool base64 = url.compare(comma - 7, 7, ";base64") == 0;
	for (size_t i = comma + 1; !base64 && i < url.size(); i++) {
		auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
		if (url[i] == '%' && i + 2 < url.size() && hex(url[i + 1]) >= 0 && hex(url[i + 2]) >= 0) {
			bytes.push_back((unsigned char)(hex(url[i + 1]) * 16 + hex(url[i + 2])));
			i += 2;
		} else {
			bytes.push_back((unsigned char)url[i]);
		}
	}
	for (size_t i = comma + 1; base64 && i < url.size(); i++) {
		char c = url[i];
		int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
		        : c == '+' ? 62 : c == '/' ? 63 : -1;
		if (v < 0)
			continue;
		bits = (bits << 6) | (unsigned)v;
		nbits += 6;
		if (nbits >= 8) {
			nbits -= 8;
			bytes.push_back((unsigned char)(bits >> nbits));
		}
	}
	unsigned h = 2166136261u;
	for (unsigned char b : bytes)
		h = (h ^ b) * 16777619u;
	char name[64];
	snprintf(name, sizeof(name), "_ui/data%08x", h);
	if (!R_ImageHandle(name)) {
		int w = 0, ih = 0;
		uint8_t *px = R_DecodeImageFile(bytes.data(), bytes.size(), &w, &ih);
		if (!px)
			return std::string();
		R_UploadImage(name, px, w, ih);
		Mem_Free(px);
	}
	return name;
}

PanoramaTextureId texture_for(const std::string &path, float &w, float &h)
{
	static std::unordered_map<std::string, std::pair<PanoramaTextureId, std::pair<int, int>>> cache;
	auto it = cache.find(path);
	if (it == cache.end()) {
		int iw = 0, ih = 0;
		std::string src = path.rfind("data:", 0) == 0 ? data_url_image(path) : path;
		PanoramaTextureId id = src.empty() ? 0 : backend.load_texture(src, iw, ih);
		if (!id)
			Con_Printf("^3ui: couldn't load image %s\n", path.c_str());
		it = cache.emplace(path, std::make_pair(id, std::make_pair(iw, ih))).first;
	}
	w = (float)it->second.second.first;
	h = (float)it->second.second.second;
	return it->second.first;
}

/* the library leaves image decoding to the host: give each Image panel its
 * src and each panel its background-image */
bool resolve_images(PanoramaNode &node)
{
	bool changed = false;
	auto src = node.attributes.find("src");
	if (src != node.attributes.end() && !src->second.empty() && node.tag_lower == "image") {
		float w, h;
		PanoramaTextureId id = texture_for(image_path(src->second), w, h);
		if (id != node.paint_texture) {
			node.paint_texture = id;
			node.paint_texture_natural_width = w;
			node.paint_texture_natural_height = h;
			changed = true;
		}
	}
	const std::string &bg = node.computed.background_image;
	if (!bg.empty() && bg != "none") {
		float w, h;
		PanoramaTextureId id = texture_for(image_path(bg), w, h);
		if (id != node.background_texture) {
			node.background_texture = id;
			node.background_texture_aspect = h > 0 ? w / h : 0;
			node.background_texture_natural_width = w;
			node.background_texture_natural_height = h;
			changed = true;
		}
	}
	for (auto &child : node.children)
		changed |= resolve_images(*child);
	return changed;
}

View *find_view(int num)
{
	for (auto &v : views)
		if (v->num == num && !v->dead)
			return v.get();
	return nullptr;
}

/* views closed since the last frame go now, outside any of their scripts */
void reap_views()
{
	views.erase(std::remove_if(views.begin(), views.end(), [](const auto &v) { return v->dead; }), views.end());
}

void host_action(View *v, const std::string &action, const std::string &arg)
{
	if (action == "__size") {
		float w, h, s = to_layout(v);
		view_viewport(*v, w, h);
		if (PanoramaNode *root = v->pv.root()) {
			root->attributes["bwwidth"] = std::to_string(w);
			root->attributes["bwheight"] = std::to_string(h);
			/* the pointer, in the same layout pixels */
			root->attributes["bwmousex"] = std::to_string(mouse_x * s);
			root->attributes["bwmousey"] = std::to_string(mouse_y * s);
		}
		return;
	}
	if (action == "__query") {
		strbuf_t reply = {};
		if (query_handler && arg.size() <= 4096)
			query_handler(v->num, arg.c_str(), &reply);
		if (PanoramaNode *root = v->pv.root())
			root->attributes["bwreply"] = reply.data ? std::string(reply.data, reply.len) : std::string();
		SB_Free(&reply);
		return;
	}
	if (action == "cmd") {
		Cbuf_AddText(arg.c_str());
		Cbuf_AddText("\n");
		return;
	}
	if (action_handler)
		action_handler(v->num, action.c_str(), arg.c_str());
}

int blend_for(PanoramaBlendMode m)
{
	switch (m) {
	case PanoramaBlendMode::Additive:
	case PanoramaBlendMode::Screen:
		return R2D_ADD;
	case PanoramaBlendMode::Multiply:
		return R2D_MULTIPLY;
	default:
		return R2D_ALPHA;
	}
}

void build_batches(View &v)
{
	float to_virtual = 1 / to_layout(&v);
	v.batches.clear();
	for (const PanoramaDrawCommand &cmd : v.pv.draw_list().commands) {
		if (cmd.vertices.empty() || cmd.indices.empty())
			continue; /* backdrop blur commands carry no geometry; not supported yet */
		if (cmd.vertices.size() > 65535) {
			Con_DPrintf("ui: skipped a draw with %zu vertices\n", cmd.vertices.size());
			continue;
		}
		View::Batch b;
		b.image = cmd.texture ? backend.handle(cmd.texture) : 0;
		b.blend = blend_for(cmd.blend_mode);
		/* glyphs blended as straight alpha would weigh their coverage twice
		 * and come out thin; browsers draw them at full weight */
		if (b.blend == R2D_ALPHA && cmd.texture && backend.premultiplied(cmd.texture))
			b.blend = R2D_PREMULTIPLIED;
		b.clip = cmd.scissor;
		b.cx = cmd.scissor_x * to_virtual;
		b.cy = cmd.scissor_y * to_virtual;
		b.cw = cmd.scissor_width * to_virtual;
		b.ch = cmd.scissor_height * to_virtual;
		bool premul = b.blend == R2D_MULTIPLY || b.blend == R2D_PREMULTIPLIED;
		bool identity = cmd.constants.is_identity();
		b.verts.reserve(cmd.vertices.size());
		for (const PanoramaPaintVertex &pv : cmd.vertices) {
			PanoramaPaintVertex p = identity ? pv : panorama_apply_draw_constants(pv, cmd.constants);
			r2dvert_t out;
			out.x = p.x * to_virtual;
			out.y = p.y * to_virtual;
			out.s = p.u;
			out.t = p.v;
			float a = premul ? p.color.a / 255.0f : 1.0f;
			out.rgba[0] = (uint8_t)(p.color.r * a);
			out.rgba[1] = (uint8_t)(p.color.g * a);
			out.rgba[2] = (uint8_t)(p.color.b * a);
			out.rgba[3] = p.color.a;
			b.verts.push_back(out);
		}
		b.idx.reserve(cmd.indices.size());
		for (int i : cmd.indices)
			b.idx.push_back((uint16_t)i);
		v.batches.push_back(std::move(b));
	}
	v.batches_dirty = false;
}

PanoramaKey panorama_key(int key)
{
	switch (key) {
	case K_LEFTARROW: return PanoramaKey::ArrowLeft;
	case K_RIGHTARROW: return PanoramaKey::ArrowRight;
	case K_UPARROW: return PanoramaKey::ArrowUp;
	case K_DOWNARROW: return PanoramaKey::ArrowDown;
	case K_HOME: return PanoramaKey::Home;
	case K_END: return PanoramaKey::End;
	case K_BACKSPACE: return PanoramaKey::Backspace;
	case K_DEL: return PanoramaKey::Delete;
	case K_ENTER: case K_KP_ENTER: return PanoramaKey::Enter;
	case K_TAB: return PanoramaKey::Tab;
	case K_ESCAPE: return PanoramaKey::Escape;
	default: return PanoramaKey::Unknown;
	}
}

/* the key names scripts see, after DOM KeyboardEvent.key */
const char *key_name(int key, int unicode, char *buf, size_t size)
{
	switch (key) {
	case K_LEFTARROW: return "ArrowLeft";
	case K_RIGHTARROW: return "ArrowRight";
	case K_UPARROW: return "ArrowUp";
	case K_DOWNARROW: return "ArrowDown";
	case K_ENTER: case K_KP_ENTER: return "Enter";
	case K_ESCAPE: return "Escape";
	case K_TAB: return "Tab";
	case K_BACKSPACE: return "Backspace";
	case K_SPACE: return " ";
	}
	if (unicode >= 32 && unicode < 127) {
		snprintf(buf, size, "%c", unicode);
		return buf;
	}
	if (key >= 32 && key < 127) {
		snprintf(buf, size, "%c", key);
		return buf;
	}
	return nullptr;
}

View *input_view()
{
	for (auto it = views.rbegin(); it != views.rend(); ++it)
		if (((*it)->flags & UI_INPUT) && !((*it)->flags & UI_EMBED) && !(*it)->dead)
			return it->get();
	return nullptr;
}

void cmd_ui_open(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: ui_open <layout> [input]\n");
		return;
	}
	int flags = argc > 2 && !strcmp(argv[2], "input") ? UI_INPUT : 0;
	int v = UI_Open(argv[1], flags);
	if (v)
		Con_Printf("ui: view %d is %s\n", v, argv[1]);
}

void cmd_ui_close(int argc, const char **argv)
{
	if (argc < 2 || !strcmp(argv[1], "all"))
		UI_CloseAll();
	else
		UI_Close(atoi(argv[1]));
}

void cmd_ui_post(int argc, const char **argv)
{
	if (argc < 3) {
		Con_Printf("usage: ui_post <view> <event> [json]\n");
		return;
	}
	UI_Post(atoi(argv[1]), argv[2], argc > 3 ? argv[3] : nullptr);
}

void cmd_ui_eval(int argc, const char **argv)
{
	if (argc < 3) {
		Con_Printf("usage: ui_eval <view> <javascript>\n");
		return;
	}
	std::string js;
	for (int i = 2; i < argc; i++) {
		if (i > 2)
			js += ' ';
		js += argv[i];
	}
	UI_Eval(atoi(argv[1]), js.c_str());
}

/* ui_font <family> <game file> [weight]: a font for CSS font-family */
void cmd_ui_font(int argc, const char **argv)
{
	if (argc < 3) {
		Con_Printf("usage: ui_font <family> <file> [weight]\n");
		return;
	}
	if (!UI_AddFont(argv[1], argv[2], argc > 3 ? atoi(argv[3]) : 400))
		Con_Printf("ui_font: couldn't add %s\n", argv[2]);
}

void cmd_ui_stats(int argc, const char **argv)
{
	if (stat_frames)
		Con_Printf("ui: %ld frames, update %.3f ms, draw %.3f ms per frame\n", stat_frames,
		           stat_update * 1000 / stat_frames, stat_draw * 1000 / stat_frames);
	stat_update = stat_draw = 0;
	stat_frames = 0;
}

void cmd_ui_list(int argc, const char **argv)
{
	for (auto &v : views)
		Con_Printf("%d %s%s\n", v->num, v->layout.c_str(), (v->flags & UI_INPUT) ? " (input)" : "");
}

/* moves the pointer in window pixels to a view-relative click, for tests and
 * scripted runs: ui_click <x> <y> */
void cmd_ui_click(int argc, const char **argv)
{
	if (argc < 3) {
		Con_Printf("usage: ui_click <x> <y>\n");
		return;
	}
	UI_MouseMove((float)atof(argv[1]), (float)atof(argv[2]));
	UI_KeyEvent(K_MOUSE1, true, 0);
	UI_KeyEvent(K_MOUSE1, false, 0);
}

void cmd_ui_key(int argc, const char **argv)
{
	if (argc < 2) {
		Con_Printf("usage: ui_key <keyname>\n");
		return;
	}
	int k = Key_NumForName(argv[1]);
	if (k < 0) {
		Con_Printf("ui_key: unknown key %s\n", argv[1]);
		return;
	}
	UI_KeyEvent(k, true, k < 127 ? k : 0);
	UI_KeyEvent(k, false, 0);
}

#ifdef BW_HAVE_GDIFONT
/* UI text drawn the way the engine draws its TrueType text: through gdifont,
 * which rasterizes like Windows GDI's ExtTextOut (docs/spec/gdi-text.md).
 * ui_gdifont 0 goes back to Panorama's own FreeType rendering; CFF (.otf)
 * faces always stay on FreeType. r_gdifont_aa picks the quality as it does
 * for engine text. */
cvar_t *ui_gdifont;

struct GdiSize
{
	gdifont_t *font = nullptr;
	int ascent = 0;
	std::vector<unsigned char> scratch;
	int sw = 0, sh = 0, pad = 0;
};

struct GdiFace
{
	bool truetype = false;
	std::unordered_map<int, GdiSize> sizes; /* by pixel size * 4 + quality */
};

std::unordered_map<std::string, GdiFace> gdi_faces;

void gdi_clear()
{
	for (auto &f : gdi_faces)
		for (auto &sz : f.second.sizes)
			if (sz.second.font)
				gdifont_free(sz.second.font);
	gdi_faces.clear();
}

/* the CreateFont cell height whose em (ppem) is the CSS pixel size */
gdifont_t *gdi_font_for_ppem(const unsigned char *data, std::size_t len, int ppem, int weight, int quality)
{
	int tall = std::max(1, (int)(ppem * 1.2f + 0.5f));
	gdifont_t *best = nullptr;
	int bestdiff = 1 << 30;
	for (int tries = 0; tries < 6; tries++) {
		gdifont_t *f = gdifont_create_memory(data, len, tall, weight, 0, quality, nullptr);
		if (!f)
			break;
		int got = gdifont_metrics(f)->ppem;
		int diff = std::abs(got - ppem);
		if (diff < bestdiff) {
			if (best)
				gdifont_free(best);
			best = f;
			bestdiff = diff;
		} else {
			gdifont_free(f);
		}
		if (got == ppem)
			break;
		tall += (ppem > got) ? 1 : -1;
		if (tall < 1)
			break;
	}
	return best;
}

bool gdi_rasterize(const std::string &identity, const unsigned char *data, std::size_t len, int pixel_size,
	int weight, char32_t cp, PanoramaRasterGlyph &out)
{
	if (!ui_gdifont || !ui_gdifont->value || !data || !len || pixel_size < 1 || pixel_size > 256)
		return false;
	auto fit = gdi_faces.find(identity);
	if (fit == gdi_faces.end()) {
		GdiFace face;
		int tt = 0, w = 0, it = 0;
		char family[64];
		face.truetype = gdifont_describe(nullptr, data, len, 0, family, sizeof(family), &w, &it, &tt) > 0 && tt;
		fit = gdi_faces.emplace(identity, std::move(face)).first;
	}
	GdiFace &face = fit->second;
	if (!face.truetype)
		return false;
	cvar_t *aa = Cvar_Find("r_gdifont_aa");
	int quality = (aa && !aa->value) ? GDIFONT_NONANTIALIASED : GDIFONT_ANTIALIASED;
	GdiSize &sz = face.sizes[pixel_size * 4 + quality];
	if (!sz.font) {
		sz.font = gdi_font_for_ppem(data, len, pixel_size, weight % kPanoramaFontFamilyStride, quality);
		if (!sz.font)
			return false;
		const gdifont_metrics_t *tm = gdifont_metrics(sz.font);
		sz.ascent = tm->ascent;
		sz.pad = tm->height * 2;
		sz.sw = tm->max_char_width + sz.pad * 2;
		sz.sh = tm->height + sz.pad * 2;
		sz.scratch.assign((std::size_t)sz.sw * sz.sh, 0);
	}
	/* one character as ExtTextOut draws it, pen at the cell's top left,
	   cropped to its ink (as r_gdifont.c does for engine text) */
	unsigned u = (unsigned)cp;
	std::fill(sz.scratch.begin(), sz.scratch.end(), 0);
	int adv = gdifont_text_out(sz.font, sz.pad, sz.pad, &u, 1, sz.scratch.data(), sz.sw, sz.sh, sz.sw);
	int minx = sz.sw, miny = sz.sh, maxx = -1, maxy = -1;
	for (int y = 0; y < sz.sh; y++)
		for (int x = 0; x < sz.sw; x++)
			if (sz.scratch[(std::size_t)y * sz.sw + x]) {
				minx = std::min(minx, x);
				maxx = std::max(maxx, x);
				miny = std::min(miny, y);
				maxy = std::max(maxy, y);
			}
	out.advance = (float)adv;
	if (maxx < 0)
		return true; /* a space */
	out.width = maxx - minx + 1;
	out.height = maxy - miny + 1;
	out.left = minx - sz.pad;
	out.top = sz.ascent - (miny - sz.pad);
	out.coverage.resize((std::size_t)out.width * out.height);
	for (int y = 0; y < out.height; y++)
		std::memcpy(&out.coverage[(std::size_t)y * out.width], &sz.scratch[(std::size_t)(miny + y) * sz.sw + minx],
			(std::size_t)out.width);
	return true;
}
#endif

} // namespace

extern "C" {

void UI_Init(void)
{
	ui_designheight = Cvar_Get("ui_designheight", "1080", CVAR_ARCHIVE,
	                           "screen height, in layout pixels, that UI layouts are authored for");
	ui_pixelated = Cvar_Get("ui_pixelated", "0", CVAR_ARCHIVE,
	                         "1: draw UI views at ui_designheight pixels and scale them up unfiltered (a low-resolution look)");
	set_panorama_render_backend(&backend);
#ifdef BW_HAVE_GDIFONT
	ui_gdifont = Cvar_Get("ui_gdifont", "1", CVAR_ARCHIVE,
	                      "1: draw UI views' TrueType text the way Windows GDI does (as engine text is); 0: FreeType's smoothed text");
	panorama_set_glyph_rasterizer(gdi_rasterize);
#endif
	load_fonts();
	Cmd_Add("ui_open", cmd_ui_open);
	Cmd_Add("ui_close", cmd_ui_close);
	Cmd_Add("ui_post", cmd_ui_post);
	Cmd_Add("ui_eval", cmd_ui_eval);
	Cmd_Add("ui_list", cmd_ui_list);
	Cmd_Add("ui_stats", cmd_ui_stats);
	Cmd_Add("ui_click", cmd_ui_click);
	Cmd_Add("ui_key", cmd_ui_key);
	Cmd_AddD("ui_font", cmd_ui_font, "ui_font <family> <file> [weight]: a font for UI views' CSS font-family");
}

void UI_Shutdown(void)
{
	views.clear();
	for (ScaledAtlas &a : atlases)
		a.atlas->abandon_gpu_resources();
	atlases.clear();
	set_panorama_render_backend(nullptr);
}

void UI_SetActionHandler(ui_action_fn fn) { action_handler = fn; }
void UI_SetQueryHandler(ui_query_fn fn) { query_handler = fn; }

/* a top-level panel can carry bwdesignheight="540" and bwpixelated="1":
 * that view then keeps its own design height and look whatever
 * ui_designheight / ui_pixelated say. True when the layout set either. */
bool read_view_options(View &v)
{
	PanoramaNode *root = v.pv.root();
	if (!root)
		return false;
	bool set = false;
	std::vector<PanoramaNode *> top = {root};
	for (auto &c : root->children)
		top.push_back(c.get());
	for (PanoramaNode *n : top) {
		auto d = n->attributes.find("bwdesignheight");
		if (d != n->attributes.end() && atof(d->second.c_str()) >= 1) {
			v.design = (float)atof(d->second.c_str());
			set = true;
		}
		auto p = n->attributes.find("bwpixelated");
		if (p != n->attributes.end()) {
			v.px = atoi(p->second.c_str()) != 0;
			set = true;
		}
		/* bwexacttext="13": text at or below 13 layout pixels is drawn at
		 * exactly its own pixel size, not scaled with the view, so small
		 * hinted and bitmap-strike text (Tahoma 11, 12) stays crisp */
		auto e = n->attributes.find("bwexacttext");
		if (e != n->attributes.end()) {
			v.exact = std::max(0.0f, (float)atof(e->second.c_str()));
			set = true;
		}
	}
	return set;
}

bool UI_LayoutExists(const char *layout) { return FS_FileExists(ui_path(layout).c_str()); }

int UI_Open(const char *layout, int flags) { return UI_OpenEx(layout, flags, NULL); }

int UI_OpenEx(const char *layout, int flags, const char *bootstrap)
{
	if (fonts_dirty) {
		fonts_dirty = false;
		load_fonts();
	}
	auto v = std::make_unique<View>();
	View *vp = v.get();
	v->num = next_view;
	v->flags = flags;
	v->layout = layout;
	float w, h;
	view_viewport(*v, w, h);
	v->pv.set_viewport(w, h);
	update_view_atlas(*v);
	v->pv.resources().add_provider(std::make_unique<FsProvider>());
	v->pv.runtime().add_bootstrap_script({"bulwark://prelude", bulwark_prelude});
	if (bootstrap && *bootstrap)
		v->pv.runtime().add_bootstrap_script({"bulwark://bootstrap", bootstrap});
	v->pv.runtime().set_host_action_handler(
	    [vp](const std::string &action, const std::string &arg) { host_action(vp, action, arg); });
	PanoramaViewLoadOptions opts;
	opts.document.resource_root = "/nonexistent-bulwark-ui-root";
	if (!v->pv.load(layout, std::move(opts))) {
		Con_Printf("ui: couldn't load %s\n", ui_path(layout).c_str());
		return 0;
	}
	if (read_view_options(*v)) {
		/* the layout has its own design height or look: lay it out again */
		view_viewport(*v, w, h);
		v->pv.set_viewport(w, h);
		if (update_view_atlas(*v)) {
			v->pv.invalidate_style();
			v->pv.invalidate_visual();
		}
	}
	next_view++;
	views.push_back(std::move(v));
	return vp->num;
}

/* a script can close its own view (a page asking the engine to close it):
 * the view is only marked here and freed at the start of the next frame */
void UI_Close(int num)
{
	for (auto &v : views)
		if (v->num == num)
			v->dead = true;
}

void UI_CloseAll(void)
{
	for (auto &v : views)
		v->dead = true;
}

bool UI_IsOpen(int num) { return find_view(num) != nullptr; }

void UI_SetFlags(int num, int flags)
{
	if (View *v = find_view(num)) {
		if ((v->flags ^ flags) & UI_EMBED)
			v->batches_dirty = true;
		v->flags = flags;
	}
}

int UI_Flags(int num)
{
	View *v = find_view(num);
	return v ? v->flags : 0;
}

void UI_ViewSize(int num, float *w, float *h)
{
	View *v = find_view(num);
	float dw = 0, dh = 0;
	if (v) {
		view_viewport(*v, dw, dh);
		float s = 1 / to_layout(v);
		dw *= s;
		dh *= s;
	}
	*w = dw;
	*h = dh;
}

bool UI_AddFont(const char *family, const char *path, int weight)
{
	if (!FS_FileExists(path)) {
		Con_DPrintf("ui: no font %s for %s\n", path, family);
		return false;
	}
	int key = panorama_register_font_family(family) * kPanoramaFontFamilyStride + BW_CLAMP(weight, 1, 1000);
	for (const ExtraFace &f : extra_faces)
		if (f.path == path && f.key == key)
			return true;
	extra_faces.push_back({path, key});
	fonts_dirty = true;
	return true;
}

void UI_DrawAt(int num, float x, float y, float w, float h, const float *rgb, float alpha)
{
	View *v = find_view(num);
	if (!v || !(v->flags & UI_EMBED) || w <= 0 || h <= 0)
		return;
	if (v->embed_w != w || v->embed_h != h) {
		/* laid out at the new size on the next frame */
		v->embed_w = w;
		v->embed_h = h;
		v->batches_dirty = true;
	}
	if (v->batches_dirty)
		build_batches(*v);
	float tint[4] = {rgb ? rgb[0] : 1, rgb ? rgb[1] : 1, rgb ? rgb[2] : 1, alpha};
	std::vector<r2dvert_t> moved;
	for (const View::Batch &b : v->batches) {
		bool premul = b.blend == R2D_MULTIPLY || b.blend == R2D_PREMULTIPLIED;
		moved.assign(b.verts.begin(), b.verts.end());
		for (r2dvert_t &vt : moved) {
			vt.x += x;
			vt.y += y;
			for (int c = 0; c < 4; c++)
				vt.rgba[c] = (uint8_t)BW_CLAMP(vt.rgba[c] * tint[c] * (c < 3 && premul ? alpha : 1), 0, 255);
		}
		if (b.clip) {
			float cx = BW_MAX(b.cx + x, x), cy = BW_MAX(b.cy + y, y);
			float cx2 = BW_MIN(b.cx + x + b.cw, x + w), cy2 = BW_MIN(b.cy + y + b.ch, y + h);
			R_Set2DClip(cx, cy, BW_MAX(0, cx2 - cx), BW_MAX(0, cy2 - cy));
		} else {
			R_Set2DClip(x, y, w, h);
		}
		R_Draw2DTriangles(b.image, moved.data(), (int)moved.size(), b.idx.data(), (int)b.idx.size(), b.blend);
	}
	R_Reset2DClip();
}

void UI_Post(int num, const char *event, const char *json)
{
	View *v = find_view(num);
	if (!v || !v->pv.root())
		return;
	std::string js = "Bulwark.__recv(" + js_quote(event) + ", " +
	                 (json && *json ? "JSON.parse(" + js_quote(json) + ")" : std::string("undefined")) + ");";
	v->pv.runtime().run_source_in_context(js, "bulwark://post", *v->pv.root());
}

void UI_Eval(int num, const char *js)
{
	View *v = find_view(num);
	if (v && v->pv.root())
		v->pv.runtime().run_source_in_context(js, "bulwark://eval", *v->pv.root());
}

bool UI_WantsInput(void) { return input_view() != nullptr; }

/* posts a "key" event: key is the DOM-style name (may be empty), bind the
 * engine's own key name, as the bind command takes it */
static void post_key(View *v, const char *name, int key, bool down)
{
	const char *bind = Key_NameForNum(key);
	std::string json = "{\"key\":" + js_quote(name ? name : "") + ",\"bind\":" + js_quote(bind ? bind : "") +
	                   ",\"down\":" + (down ? "true" : "false") +
	                   ",\"shift\":" + (Key_IsDown(K_LSHIFT) ? "true" : "false") +
	                   ",\"ctrl\":" + (Key_IsDown(K_LCTRL) ? "true" : "false") +
	                   ",\"alt\":" + (Key_IsDown(K_LALT) ? "true" : "false") + "}";
	UI_Post(v->num, "key", json.c_str());
}

bool UI_KeyEvent(int key, bool down, int unicode)
{
	View *v = input_view();
	if (!v)
		return false;
	float s = to_layout(v);
	if (key == K_MOUSE1) {
		mouse_down = down;
		v->pv.update_pointer(mouse_x * s, mouse_y * s, mouse_down);
		post_key(v, "", key, down);
		return true;
	}
	if (key == K_MWHEELUP || key == K_MWHEELDOWN) {
		if (down)
			v->pv.update_wheel(mouse_x * s, mouse_y * s, key == K_MWHEELUP ? 1.0f : -1.0f);
		post_key(v, key == K_MWHEELUP ? "WheelUp" : "WheelDown", key, down);
		return true;
	}
	if (down) {
		PanoramaKey pk = panorama_key(key);
		if (pk != PanoramaKey::Unknown) {
			PanoramaKeyEvent ev;
			ev.key = pk;
			ev.shift = Key_IsDown(K_LSHIFT);
			ev.ctrl = Key_IsDown(K_LCTRL);
			ev.alt = Key_IsDown(K_LALT);
			v->pv.handle_key_down(ev);
		} else if (unicode >= 32) {
			char text[8] = {0};
			text[0] = (char)unicode; /* the client passes Latin-1 for now */
			v->pv.handle_text_input(text);
		}
	}
	char buf[8];
	post_key(v, key_name(key, unicode, buf, sizeof(buf)), key, down);
	return true;
}

void UI_MouseMove(float x, float y)
{
	mouse_x = x;
	mouse_y = y;
	if (View *v = input_view()) {
		float s = to_layout(v);
		v->pv.update_pointer(x * s, y * s, mouse_down);
	}
}

void UI_Frame(double dt)
{
	double start = Sys_Seconds();
	reap_views();
	drop_unused_atlases();
	if (fonts_dirty) {
		fonts_dirty = false;
		load_fonts();
		for (auto &v : views) {
			v->atlas_key = 0;
			update_view_atlas(*v);
			/* font-family names that weren't registered yet resolve now */
			v->pv.invalidate_style();
			v->pv.invalidate_visual();
			v->batches_dirty = true;
		}
	}
	float w, h;
	for (auto &v : views) {
		/* window size or cvars changed: rebake glyphs, rebuild geometry */
		float scale = 1 / to_layout(v.get()) * (pixelated(v.get()) ? -1 : 1);
		if (scale != v->drawn_scale) {
			v->drawn_scale = scale;
			v->batches_dirty = true;
			if (update_view_atlas(*v))
				v->pv.invalidate_visual();
		}
	}
	/* a script can close views (Bulwark.Cmd("ui_close ...") runs later from
	 * the command buffer, so iterating here is safe) */
	for (auto &v : views) {
		view_viewport(*v, w, h);
		if (v->pv.viewport_width() != w || v->pv.viewport_height() != h)
			v->pv.set_viewport(w, h);
		PanoramaViewUpdateResult r = v->pv.update((float)dt);
		if (v->pv.root() && (r.style_changed || !v->images_resolved)) {
			v->images_resolved = true;
			if (resolve_images(*v->pv.root())) {
				v->pv.invalidate_visual();
				r = v->pv.update(0);
				v->batches_dirty = true;
			}
		}
		if (r.visual_changed || r.draw_list_rebuilt)
			v->batches_dirty = true;
	}
	if (!views.empty()) {
		stat_update += Sys_Seconds() - start;
		stat_frames++;
	}
}

void UI_Draw(void)
{
	double start = Sys_Seconds();
	for (auto &v : views) {
		if ((v->flags & UI_EMBED) || v->dead)
			continue;
		if (v->batches_dirty)
			build_batches(*v);
		bool lowres = pixelated(v.get());
		if (lowres) {
			int w, h;
			R_GetSize(&w, &h);
			float s = to_layout(v.get());
			R_BeginPixelated2D((int)lroundf(w * s), (int)lroundf(h * s));
		}
		for (const View::Batch &b : v->batches) {
			if (b.clip)
				R_Set2DClip(b.cx, b.cy, b.cw, b.ch);
			else
				R_Reset2DClip();
			R_Draw2DTriangles(b.image, b.verts.data(), (int)b.verts.size(), b.idx.data(), (int)b.idx.size(),
			                  b.blend);
		}
		R_Reset2DClip();
		if (lowres)
			R_EndPixelated2D();
	}
	if (!views.empty())
		stat_draw += Sys_Seconds() - start;
}

} // extern "C"
