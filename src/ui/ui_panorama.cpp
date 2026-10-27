/* Bulwark engine - UI views on top of PanoramaEngine (MIT, third_party/).
 * This file is the only place that knows about the library: it feeds it
 * files from the engine's file system, hands its draw list to the
 * renderer's 2D API and routes input and script calls. ISC licence, see
 * LICENSE. */
#include "ui.h"

extern "C" {
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"
#include "../client/keys.h"
#include "../render/render.h"
int Key_NumForName(const char *name);
bool Key_IsDown(int key);
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
	};

	PanoramaTextureId generate_texture(std::span<const unsigned char> rgba, int w, int h) override
	{
		PanoramaTextureId id = next++;
		char name[64];
		snprintf(name, sizeof(name), "_ui/generated%lu", (unsigned long)id);
		textures[id] = {R_UploadImage(name, rgba.data(), w, h), w, h};
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
		textures[id] = {handle, w, h};
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
	return r ? [parseFloat(r.GetAttributeString("bwwidth", "0")), parseFloat(r.GetAttributeString("bwheight", "0"))] : [0, 0];
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
};

Backend backend;
PanoramaFontAtlas fonts;
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
double last_frame_scale;
cvar_t *ui_designheight;
/* time spent in UI_Frame and UI_Draw, for ui_stats */
double stat_update, stat_draw;
long stat_frames;

/* design units -> framebuffer pixels; layouts are authored for a fixed
 * height (ui_designheight, 1080 by default) and scale with the window */
float ui_scale()
{
	int w, h;
	R_GetSize(&w, &h);
	float fbh = h * R_Get2DScale();
	float design = ui_designheight ? ui_designheight->value : 1080;
	if (design < 1)
		design = 1080;
	return fbh / design;
}

void viewport_design(float &w, float &h)
{
	int vw, vh;
	R_GetSize(&vw, &vh);
	float s = R_Get2DScale() / ui_scale();
	w = vw * s;
	h = vh * s;
}

/* an embedded view is laid out at the size it is drawn */
void view_viewport(const View &v, float &w, float &h)
{
	if ((v.flags & UI_EMBED) && v.embed_w > 0 && v.embed_h > 0) {
		float s = R_Get2DScale() / ui_scale();
		w = v.embed_w * s;
		h = v.embed_h * s;
		return;
	}
	viewport_design(w, h);
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

void add_face(const char *name, const char *searchpath, void *ctx)
{
	auto *faces = static_cast<std::vector<PanoramaFontAtlasFace> *>(ctx);
	const char *base = strrchr(name, '/');
	base = base ? base + 1 : name;
	for (const PanoramaFontAtlasFace &f : *faces)
		if (f.path == base)
			return;
	faces->push_back({base, face_weight(base)});
	/* each file is also a family of its own name, without the extension */
	std::string fam(base);
	if (fam.find('.') != std::string::npos)
		fam.erase(fam.rfind('.'));
	faces->push_back({base, panorama_register_font_family(fam) * kPanoramaFontFamilyStride + 400});
}

/* every font in panorama/fonts/ joins one family; weights come from the
 * file names (Bold, Medium, ...) */
void load_fonts()
{
	PanoramaFontAtlasLoadOptions opts;
	FS_List("panorama/fonts/*.ttf", true, add_face, &opts.faces);
	FS_List("panorama/fonts/*.otf", true, add_face, &opts.faces);
	for (const ExtraFace &f : extra_faces)
		opts.faces.push_back({f.path, f.key});
	if (opts.faces.empty()) {
		Con_Printf("^3ui: no fonts in panorama/fonts/; text will not draw\n");
		return;
	}
	opts.resource_root = "/nonexistent-bulwark-ui-root";
	opts.font_bytes_provider = [](std::string_view face) -> std::optional<std::vector<unsigned char>> {
		std::vector<unsigned char> bytes;
		std::string name(face);
		/* registered faces are named by their game path, the others by file name */
		if (!load_bytes(name.find('/') != std::string::npos ? name : "panorama/fonts/" + name, bytes))
			return std::vector<unsigned char>(); /* handled: never look on disk */
		return bytes;
	};
	fonts.clear();
	fonts_loaded = fonts.load(opts);
	if (!fonts_loaded)
		Con_Printf("^3ui: couldn't load the fonts in panorama/fonts/\n");
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
		float w, h;
		view_viewport(*v, w, h);
		if (PanoramaNode *root = v->pv.root()) {
			root->attributes["bwwidth"] = std::to_string(w);
			root->attributes["bwheight"] = std::to_string(h);
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
	float to_virtual = ui_scale() / R_Get2DScale();
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
		b.clip = cmd.scissor;
		b.cx = cmd.scissor_x * to_virtual;
		b.cy = cmd.scissor_y * to_virtual;
		b.cw = cmd.scissor_width * to_virtual;
		b.ch = cmd.scissor_height * to_virtual;
		bool premul = b.blend == R2D_MULTIPLY;
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

} // namespace

extern "C" {

void UI_Init(void)
{
	ui_designheight = Cvar_Get("ui_designheight", "1080", CVAR_ARCHIVE,
	                           "screen height, in layout pixels, that UI layouts are authored for");
	set_panorama_render_backend(&backend);
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
	fonts.abandon_gpu_resources();
	set_panorama_render_backend(nullptr);
}

void UI_SetActionHandler(ui_action_fn fn) { action_handler = fn; }
void UI_SetQueryHandler(ui_query_fn fn) { query_handler = fn; }

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
	viewport_design(w, h);
	v->pv.set_viewport(w, h);
	if (fonts_loaded) {
		fonts.set_ui_scale(ui_scale());
		v->pv.set_font_atlas(&fonts);
	}
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
		float s = ui_scale() / R_Get2DScale();
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
		moved.assign(b.verts.begin(), b.verts.end());
		for (r2dvert_t &vt : moved) {
			vt.x += x;
			vt.y += y;
			for (int c = 0; c < 4; c++)
				vt.rgba[c] = (uint8_t)BW_CLAMP(vt.rgba[c] * tint[c], 0, 255);
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

bool UI_KeyEvent(int key, bool down, int unicode)
{
	View *v = input_view();
	if (!v)
		return false;
	float s = R_Get2DScale() / ui_scale();
	if (key == K_MOUSE1) {
		mouse_down = down;
		v->pv.update_pointer(mouse_x * s, mouse_y * s, mouse_down);
		return true;
	}
	if (key == K_MWHEELUP || key == K_MWHEELDOWN) {
		if (down)
			v->pv.update_wheel(mouse_x * s, mouse_y * s, key == K_MWHEELUP ? 1.0f : -1.0f);
		char json[64];
		snprintf(json, sizeof(json), "{\"key\":\"%s\",\"down\":%s}", key == K_MWHEELUP ? "WheelUp" : "WheelDown",
		         down ? "true" : "false");
		UI_Post(v->num, "key", json);
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
	const char *name = key_name(key, unicode, buf, sizeof(buf));
	if (name) {
		std::string json = "{\"key\":" + js_quote(name) + ",\"down\":" + (down ? "true" : "false") +
		                   ",\"shift\":" + (Key_IsDown(K_LSHIFT) ? "true" : "false") +
		                   ",\"ctrl\":" + (Key_IsDown(K_LCTRL) ? "true" : "false") +
		                   ",\"alt\":" + (Key_IsDown(K_LALT) ? "true" : "false") + "}";
		UI_Post(v->num, "key", json.c_str());
	}
	return true;
}

void UI_MouseMove(float x, float y)
{
	mouse_x = x;
	mouse_y = y;
	if (View *v = input_view()) {
		float s = R_Get2DScale() / ui_scale();
		v->pv.update_pointer(x * s, y * s, mouse_down);
	}
}

void UI_Frame(double dt)
{
	double start = Sys_Seconds();
	reap_views();
	if (fonts_dirty) {
		fonts_dirty = false;
		load_fonts();
		if (fonts_loaded)
			fonts.set_ui_scale(ui_scale());
		for (auto &v : views) {
			if (fonts_loaded)
				v->pv.set_font_atlas(&fonts);
			/* font-family names that weren't registered yet resolve now */
			v->pv.invalidate_style();
			v->pv.invalidate_visual();
			v->batches_dirty = true;
		}
	}
	float w, h;
	viewport_design(w, h);
	double scale = ui_scale();
	if (scale != last_frame_scale) {
		last_frame_scale = scale;
		if (fonts_loaded)
			fonts.set_ui_scale((float)scale);
		for (auto &v : views)
			v->batches_dirty = true;
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
		for (const View::Batch &b : v->batches) {
			if (b.clip)
				R_Set2DClip(b.cx, b.cy, b.cw, b.ch);
			else
				R_Reset2DClip();
			R_Draw2DTriangles(b.image, b.verts.data(), (int)b.verts.size(), b.idx.data(), (int)b.idx.size(),
			                  b.blend);
		}
		R_Reset2DClip();
	}
	if (!views.empty())
		stat_draw += Sys_Seconds() - start;
}

} // extern "C"
