/* Bulwark engine - GL entry points, cached state, GLSL programs, streamed
 * geometry and the matrix helpers. ISC licence, see LICENSE. */
#include "r_local.h"

#include <math.h>
#include <stdio.h>

#define X(ret, name, args) PFN_##name q##name;
BW_GL_FUNCS(X)
#undef X

int GL_LoadFunctions(void *(*getproc)(const char *name))
{
	int missing = 0;
#define X(ret, name, args)                                                                                     \
	q##name = (PFN_##name)getproc(#name);                                                                      \
	if (!q##name) {                                                                                            \
		Con_Printf("OpenGL: missing %s\n", #name);                                                             \
		missing++;                                                                                             \
	}
	BW_GL_FUNCS(X)
#undef X
	return missing == 0;
}

/* --- state cache ----------------------------------------------------------- */

static struct {
	int blend;
	GLenum src, dst;
	int depthtest, depthwrite, cull;
	GLuint tex[8];
	GLenum target[8];
	int unit;
	int prog;
} gs = {-1, 0, 0, -1, -1, -1, {0}, {0}, -1, -1};

void GL_SetBlend(int blend, GLenum src, GLenum dst)
{
	if (blend == BLEND_ALPHA)
		src = GL_SRC_ALPHA, dst = GL_ONE_MINUS_SRC_ALPHA;
	else if (blend == BLEND_ADD)
		src = GL_SRC_ALPHA, dst = GL_ONE;
	else if (blend == BLEND_FILTER)
		src = GL_DST_COLOR, dst = GL_ZERO;
	if (blend == BLEND_OPAQUE) {
		if (gs.blend != BLEND_OPAQUE)
			qglDisable(GL_BLEND);
		gs.blend = BLEND_OPAQUE;
		return;
	}
	if (gs.blend == BLEND_OPAQUE || gs.blend < 0)
		qglEnable(GL_BLEND);
	if (gs.src != src || gs.dst != dst || gs.blend < 0)
		qglBlendFunc(src, dst);
	gs.blend = blend;
	gs.src = src;
	gs.dst = dst;
}

void GL_SetDepth(bool test, bool write)
{
	if (gs.depthtest != (int)test) {
		if (test)
			qglEnable(GL_DEPTH_TEST);
		else
			qglDisable(GL_DEPTH_TEST);
		gs.depthtest = test;
	}
	if (gs.depthwrite != (int)write) {
		qglDepthMask(write ? GL_TRUE : GL_FALSE);
		gs.depthwrite = write;
	}
}

void GL_SetCull(bool cull)
{
	if (gs.cull == (int)cull)
		return;
	if (cull)
		qglEnable(GL_CULL_FACE);
	else
		qglDisable(GL_CULL_FACE);
	gs.cull = cull;
}

void GL_BindTexture(int unit, GLenum target, GLuint tex)
{
	if (gs.tex[unit] == tex && gs.target[unit] == target)
		return;
	if (gs.unit != unit) {
		qglActiveTexture(GL_TEXTURE0 + unit);
		gs.unit = unit;
	}
	qglBindTexture(target, tex);
	gs.tex[unit] = tex;
	gs.target[unit] = target;
}

/* deleting a texture unbinds it, and the next glGenTextures can hand out
 * the same name: forget it here too, or GL_BindTexture would skip binding
 * the new texture and its upload would land in whatever is bound */
void GL_DeleteTextures(int n, const GLuint *tex)
{
	for (int i = 0; i < n; i++)
		for (int u = 0; u < (int)(sizeof(gs.tex) / sizeof(gs.tex[0])); u++)
			if (gs.tex[u] == tex[i])
				gs.tex[u] = 0;
	qglDeleteTextures(n, tex);
}

/* --- programs -------------------------------------------------------------- */

glprog_t r_progs[PROG_COUNT];

static const char *glsl_header = "#version 330 core\n";

static const char *vs_2d =
	"layout(location=0) in vec3 a_pos;\n"
	"layout(location=1) in vec2 a_tc;\n"
	"layout(location=4) in vec4 a_color;\n"
	"uniform mat4 u_mvp;\n"
	"uniform vec4 u_tcmod;\n"
	"out vec2 v_tc; out vec4 v_color;\n"
	"void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); v_tc = a_tc * u_tcmod.zw + u_tcmod.xy; v_color = a_color; }\n";
static const char *fs_2d =
	"in vec2 v_tc; in vec4 v_color;\n"
	"uniform sampler2D u_tex; uniform vec4 u_color; uniform float u_alphatest;\n"
	"out vec4 o_color;\n"
	"void main() { vec4 c = texture(u_tex, v_tc) * v_color * u_color; if (c.a < u_alphatest) discard; o_color = c; }\n";

/* world surfaces: diffuse times lightmap, plus dynamic lights */
static const char *vs_world =
	"layout(location=0) in vec3 a_pos;\n"
	"layout(location=1) in vec2 a_tc;\n"
	"layout(location=2) in vec2 a_lmtc;\n"
	"layout(location=3) in vec3 a_normal;\n"
	"layout(location=4) in vec4 a_color;\n"
	"uniform mat4 u_mvp; uniform mat4 u_model; uniform vec4 u_tcmod;\n"
	"out vec2 v_tc; out vec2 v_lmtc; out vec3 v_pos; out vec3 v_normal; out vec4 v_color;\n"
	"void main() {\n"
	"  gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"  v_tc = a_tc * u_tcmod.zw + u_tcmod.xy; v_lmtc = a_lmtc;\n"
	"  v_pos = (u_model * vec4(a_pos, 1.0)).xyz; v_normal = mat3(u_model) * a_normal; v_color = a_color;\n"
	"}\n";
/* Dynamic lights fall off as 1 - (d/r)^2 and peak at about a third of the
 * colour, times N.L: the shape FTE gives dlights with its default
 * r_shadow_realtime_dlight 1, measured from screenshots of it running. */
#define DLIGHT_SCALE "0.31"
static const char *fs_world =
	"in vec2 v_tc; in vec2 v_lmtc; in vec3 v_pos; in vec3 v_normal; in vec4 v_color;\n"
	"uniform sampler2D u_tex; uniform sampler2D u_lightmap; uniform sampler2D u_fullbright;\n"
	"uniform vec4 u_color; uniform float u_lmscale; uniform float u_alphatest; uniform float u_usefullbright;\n"
	"uniform vec4 u_dlights[8]; uniform vec3 u_dlightcolors[8]; uniform int u_numdlights;\n"
	"uniform vec4 u_fog;\n"
	"uniform vec3 u_vieworg;\n"
	"out vec4 o_color;\n"
	"void main() {\n"
	"  vec4 d = texture(u_tex, v_tc);\n"
	"  if (d.a * u_color.a < u_alphatest) discard;\n"
	"  vec3 light = u_lmscale >= 0.0 ? texture(u_lightmap, v_lmtc).rgb * u_lmscale : v_color.rgb;\n"
	"  vec3 n = normalize(v_normal);\n"
	"  for (int i = 0; i < u_numdlights; i++) {\n"
	"    vec3 l = u_dlights[i].xyz - v_pos; float dist = length(l);\n"
	"    float x = dist / u_dlights[i].w; float a = max(0.0, 1.0 - x * x) * " DLIGHT_SCALE ";\n"
	"    light += u_dlightcolors[i] * a * max(dot(n, l / max(dist, 0.001)), 0.0);\n"
	"  }\n"
	"  vec3 c = d.rgb * light;\n"
	"  if (u_usefullbright > 0.0) c += texture(u_fullbright, v_tc).rgb;\n"
	"  if (u_fog.w > 0.0) { float z = u_fog.w * length(v_pos - u_vieworg); float f = clamp(exp(-z * z), 0.0, 1.0); c = mix(u_fog.rgb, c, f); }\n"
	"  o_color = vec4(c, d.a) * u_color;\n"
	"}\n";

/* models: an ambient and a directional term from the light grid, plus dlights */
static const char *vs_model =
	"layout(location=0) in vec3 a_pos;\n"
	"layout(location=1) in vec2 a_tc;\n"
	"layout(location=3) in vec3 a_normal;\n"
	"uniform mat4 u_mvp; uniform mat4 u_model;\n"
	"out vec2 v_tc; out vec3 v_pos; out vec3 v_normal;\n"
	"void main() {\n"
	"  gl_Position = u_mvp * vec4(a_pos, 1.0); v_tc = a_tc;\n"
	"  v_pos = (u_model * vec4(a_pos, 1.0)).xyz; v_normal = mat3(u_model) * a_normal;\n"
	"}\n";
static const char *fs_model =
	"in vec2 v_tc; in vec3 v_pos; in vec3 v_normal;\n"
	"uniform sampler2D u_tex; uniform sampler2D u_fullbright; uniform float u_usefullbright;\n"
	"uniform vec4 u_color; uniform float u_alphatest;\n"
	"uniform vec3 u_light_ambient; uniform vec3 u_light_diffuse; uniform vec3 u_light_dir;\n"
	"uniform vec4 u_dlights[8]; uniform vec3 u_dlightcolors[8]; uniform int u_numdlights;\n"
	"uniform vec4 u_fog; uniform vec3 u_vieworg;\n"
	"out vec4 o_color;\n"
	"void main() {\n"
	"  vec4 d = texture(u_tex, v_tc);\n"
	"  if (d.a * u_color.a < u_alphatest) discard;\n"
	"  vec3 n = normalize(v_normal);\n"
	"  vec3 light = u_light_ambient + u_light_diffuse * max(dot(n, u_light_dir), 0.0);\n"
	"  for (int i = 0; i < u_numdlights; i++) {\n"
	"    vec3 l = u_dlights[i].xyz - v_pos; float dist = length(l);\n"
	"    float x = dist / u_dlights[i].w; float a = max(0.0, 1.0 - x * x) * " DLIGHT_SCALE ";\n"
	"    light += u_dlightcolors[i] * a * max(dot(n, l / max(dist, 0.001)), 0.0);\n"
	"  }\n"
	"  vec3 c = d.rgb * light;\n"
	"  if (u_usefullbright > 0.0) c += texture(u_fullbright, v_tc).rgb;\n"
	"  if (u_fog.w > 0.0) { float z = u_fog.w * length(v_pos - u_vieworg); float f = clamp(exp(-z * z), 0.0, 1.0); c = mix(u_fog.rgb, c, f); }\n"
	"  o_color = vec4(c, d.a) * u_color;\n"
	"}\n";

static const char *vs_unlit =
	"layout(location=0) in vec3 a_pos;\n"
	"layout(location=1) in vec2 a_tc;\n"
	"layout(location=4) in vec4 a_color;\n"
	"uniform mat4 u_mvp; uniform vec4 u_tcmod;\n"
	"out vec2 v_tc; out vec4 v_color;\n"
	"void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); v_tc = a_tc * u_tcmod.zw + u_tcmod.xy; v_color = a_color; }\n";

/* sky: the view direction looks up a cubemap. Quake's axes are x forward,
 * y left, z up; the faces were uploaded so that this mapping holds */
static const char *vs_sky =
	"layout(location=0) in vec3 a_pos;\n"
	"uniform mat4 u_mvp; uniform vec3 u_vieworg;\n"
	"out vec3 v_dir;\n"
	"void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); v_dir = a_pos - u_vieworg; }\n";
static const char *fs_sky =
	"in vec3 v_dir;\n"
	"uniform samplerCube u_cube; uniform vec4 u_color;\n"
	"out vec4 o_color;\n"
	"void main() { vec3 d = normalize(v_dir); o_color = texture(u_cube, vec3(-d.x, d.z, d.y)) * u_color; }\n";

static GLuint compile(GLenum type, const char *src, const char *what)
{
	GLuint s = qglCreateShader(type);
	const char *srcs[2] = {glsl_header, src};
	GLint ok;
	qglShaderSource(s, 2, srcs, NULL);
	qglCompileShader(s);
	qglGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[4096];
		qglGetShaderInfoLog(s, sizeof(log), NULL, log);
		Con_Printf("^1%s %s shader: %s\n", what, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
		qglDeleteShader(s);
		return 0;
	}
	return s;
}

static bool build(glprog_t *p, const char *vs, const char *fs, const char *what)
{
	GLuint v = compile(GL_VERTEX_SHADER, vs, what), f = compile(GL_FRAGMENT_SHADER, fs, what);
	GLint ok;
	if (!v || !f)
		return false;
	p->prog = qglCreateProgram();
	qglAttachShader(p->prog, v);
	qglAttachShader(p->prog, f);
	qglLinkProgram(p->prog);
	qglDeleteShader(v);
	qglDeleteShader(f);
	qglGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[4096];
		qglGetProgramInfoLog(p->prog, sizeof(log), NULL, log);
		Con_Printf("^1%s program: %s\n", what, log);
		return false;
	}
#define U(x) p->x = qglGetUniformLocation(p->prog, #x)
	U(u_mvp); U(u_model); U(u_tex); U(u_lightmap); U(u_cube); U(u_color); U(u_lmscale); U(u_vieworg);
	U(u_light_ambient); U(u_light_dir); U(u_light_diffuse); U(u_tcmod); U(u_alphatest); U(u_fog); U(u_time);
	U(u_dlights); U(u_numdlights); U(u_dlightcolors); U(u_fullbright); U(u_usefullbright);
#undef U
	qglUseProgram(p->prog);
	if (p->u_tex >= 0)
		qglUniform1i(p->u_tex, 0);
	if (p->u_lightmap >= 0)
		qglUniform1i(p->u_lightmap, 1);
	if (p->u_fullbright >= 0)
		qglUniform1i(p->u_fullbright, 2);
	if (p->u_cube >= 0)
		qglUniform1i(p->u_cube, 3);
	if (p->u_color >= 0)
		qglUniform4f(p->u_color, 1, 1, 1, 1);
	if (p->u_tcmod >= 0)
		qglUniform4f(p->u_tcmod, 0, 0, 1, 1);
	return true;
}

bool R_InitPrograms(void)
{
	bool ok = build(&r_progs[PROG_2D], vs_2d, fs_2d, "2d");
	ok &= build(&r_progs[PROG_WORLD], vs_world, fs_world, "world");
	ok &= build(&r_progs[PROG_MODEL], vs_model, fs_model, "model");
	ok &= build(&r_progs[PROG_UNLIT], vs_unlit, fs_2d, "unlit");
	ok &= build(&r_progs[PROG_SKY], vs_sky, fs_sky, "sky");
	gs.prog = -1;
	return ok;
}

void R_ShutdownPrograms(void)
{
	for (int i = 0; i < PROG_COUNT; i++)
		if (r_progs[i].prog)
			qglDeleteProgram(r_progs[i].prog);
	memset(r_progs, 0, sizeof(r_progs));
}

void R_UseProgram(int p)
{
	if (gs.prog == p)
		return;
	qglUseProgram(r_progs[p].prog);
	gs.prog = p;
}

/* --- streamed geometry ------------------------------------------------------ */

static GLuint stream_vao, stream_vbo, stream_ebo;

void R_SetupVertexAttribs(void)
{
	qglEnableVertexAttribArray(ATTR_POS);
	qglVertexAttribPointer(ATTR_POS, 3, GL_FLOAT, GL_FALSE, sizeof(rvert_t), (void *)offsetof(rvert_t, xyz));
	qglEnableVertexAttribArray(ATTR_TC);
	qglVertexAttribPointer(ATTR_TC, 2, GL_FLOAT, GL_FALSE, sizeof(rvert_t), (void *)offsetof(rvert_t, st));
	qglEnableVertexAttribArray(ATTR_LMTC);
	qglVertexAttribPointer(ATTR_LMTC, 2, GL_FLOAT, GL_FALSE, sizeof(rvert_t), (void *)offsetof(rvert_t, lm));
	qglEnableVertexAttribArray(ATTR_NORMAL);
	qglVertexAttribPointer(ATTR_NORMAL, 3, GL_FLOAT, GL_FALSE, sizeof(rvert_t), (void *)offsetof(rvert_t, normal));
	qglEnableVertexAttribArray(ATTR_COLOR);
	qglVertexAttribPointer(ATTR_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(rvert_t), (void *)offsetof(rvert_t, color));
}

void R_StreamDraw(GLenum mode, const rvert_t *verts, int numverts, const uint16_t *idx, int numidx)
{
	if (!stream_vao) {
		qglGenVertexArrays(1, &stream_vao);
		qglGenBuffers(1, &stream_vbo);
		qglGenBuffers(1, &stream_ebo);
		qglBindVertexArray(stream_vao);
		qglBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
		qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, stream_ebo);
		R_SetupVertexAttribs();
	}
	qglBindVertexArray(stream_vao);
	qglBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
	/* orphan the buffer each time so the driver never waits on it */
	qglBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(rvert_t) * numverts), verts, GL_STREAM_DRAW);
	if (idx) {
		qglBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(uint16_t) * numidx), idx, GL_STREAM_DRAW);
		qglDrawElements(mode, numidx, GL_UNSIGNED_SHORT, NULL);
	} else {
		qglDrawArrays(mode, 0, numverts);
	}
}

void R_ShutdownStream(void)
{
	if (stream_vao) {
		qglDeleteVertexArrays(1, &stream_vao);
		qglDeleteBuffers(1, &stream_vbo);
		qglDeleteBuffers(1, &stream_ebo);
		stream_vao = 0;
	}
	memset(&gs, 0, sizeof(gs));
	gs.blend = gs.depthtest = gs.depthwrite = gs.cull = gs.unit = gs.prog = -1;
}

/* --- maths ------------------------------------------------------------------ */
/* matrices are column-major, as GL expects */

void Mat4_Identity(float *m)
{
	memset(m, 0, sizeof(float) * 16);
	m[0] = m[5] = m[10] = m[15] = 1;
}

void Mat4_Multiply(float *out, const float *a, const float *b)
{
	float r[16];
	for (int c = 0; c < 4; c++)
		for (int rr = 0; rr < 4; rr++)
			r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] + a[2 * 4 + rr] * b[c * 4 + 2] +
			                a[3 * 4 + rr] * b[c * 4 + 3];
	memcpy(out, r, sizeof(r));
}

void Mat4_Perspective(float *m, float fovx, float fovy, float znear, float zfar)
{
	float x = 1.0f / tanf(fovx * (float)M_PI / 360.0f);
	float y = 1.0f / tanf(fovy * (float)M_PI / 360.0f);
	memset(m, 0, sizeof(float) * 16);
	m[0] = x;
	m[5] = y;
	m[11] = -1;
	if (zfar <= 0) {
		/* infinite far plane */
		m[10] = -1;
		m[14] = -2 * znear;
	} else {
		m[10] = -(zfar + znear) / (zfar - znear);
		m[14] = -2 * zfar * znear / (zfar - znear);
	}
}

void Mat4_Ortho(float *m, float l, float r, float b, float t, float n, float f)
{
	memset(m, 0, sizeof(float) * 16);
	m[0] = 2 / (r - l);
	m[5] = 2 / (t - b);
	m[10] = -2 / (f - n);
	m[12] = -(r + l) / (r - l);
	m[13] = -(t + b) / (t - b);
	m[14] = -(f + n) / (f - n);
	m[15] = 1;
}

void Mat4_FromAxis(float *m, const float axis[3][3], const float *origin, float scale)
{
	for (int i = 0; i < 3; i++) {
		m[i] = axis[0][i] * scale;
		m[4 + i] = axis[1][i] * scale;
		m[8 + i] = axis[2][i] * scale;
		m[12 + i] = origin[i];
	}
	m[3] = m[7] = m[11] = 0;
	m[15] = 1;
}

/* axis[0] forward, axis[1] left, axis[2] up */
void AnglesToAxis(const float *angles, float axis[3][3])
{
	float right[3];
	AngleVectors(angles, axis[0], right, axis[2]);
	axis[1][0] = -right[0];
	axis[1][1] = -right[1];
	axis[1][2] = -right[2];
}

/* world to eye: Quake forward (x) looks down GL's -z, left (y) is -x, up is +y */
void Mat4_View(float *m, const float *origin, const float *angles)
{
	float f[3], r[3], u[3];
	AngleVectors(angles, f, r, u);
	m[0] = r[0], m[4] = r[1], m[8] = r[2];
	m[1] = u[0], m[5] = u[1], m[9] = u[2];
	m[2] = -f[0], m[6] = -f[1], m[10] = -f[2];
	m[3] = m[7] = m[11] = 0;
	m[12] = -(r[0] * origin[0] + r[1] * origin[1] + r[2] * origin[2]);
	m[13] = -(u[0] * origin[0] + u[1] * origin[1] + u[2] * origin[2]);
	m[14] = f[0] * origin[0] + f[1] * origin[1] + f[2] * origin[2];
	m[15] = 1;
}

void Mat4_Transform(const float *m, const float *in, float *out)
{
	for (int i = 0; i < 4; i++)
		out[i] = m[i] * in[0] + m[4 + i] * in[1] + m[8 + i] * in[2] + m[12 + i];
}

bool Mat4_Invert(float *out, const float *m)
{
	float inv[16], det;
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (det == 0)
		return false;
	det = 1.0f / det;
	for (int i = 0; i < 16; i++)
		out[i] = inv[i] * det;
	return true;
}

/* a program's GLSL, for r_dumpshaders */
bool R_ProgramSource(int p, const char **name, const char **vs, const char **fs)
{
	static const char *const names[PROG_COUNT] = {"2d", "world", "model", "unlit", "sky"};
	const char *const v[PROG_COUNT] = {vs_2d, vs_world, vs_model, vs_unlit, vs_sky};
	const char *const f[PROG_COUNT] = {fs_2d, fs_world, fs_model, fs_2d, fs_sky};
	if (p < 0 || p >= PROG_COUNT)
		return false;
	*name = names[p];
	*vs = v[p];
	*fs = f[p];
	return true;
}

const char *R_GLSLHeader(void) { return glsl_header; }
