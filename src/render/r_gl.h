/* Bulwark engine - the OpenGL 3.3 core entry points and constants the
 * renderer uses, loaded at runtime through the window layer.
 * ISC licence, see LICENSE. */
#ifndef BW_R_GL_H
#define BW_R_GL_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define BWGLAPI __stdcall
#else
#define BWGLAPI
#endif

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_ZERO 0
#define GL_ONE 1
#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_LINE_LOOP 0x0002
#define GL_LINE_STRIP 0x0003
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_ALWAYS 0x0207
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_RED 0x1903
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_REPEAT 0x2901
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_RGB8 0x8051
#define GL_RGBA8 0x8058
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_CUBE_MAP_SEAMLESS 0x884F
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400

/* return type, name, parameters */
#define BW_GL_FUNCS(X)                                                                                         \
	X(const GLubyte *, glGetString, (GLenum name))                                                             \
	X(GLenum, glGetError, (void))                                                                              \
	X(void, glGetIntegerv, (GLenum pname, GLint *data))                                                        \
	X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                              \
	X(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h))                                               \
	X(void, glClear, (GLbitfield mask))                                                                        \
	X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                        \
	X(void, glEnable, (GLenum cap))                                                                            \
	X(void, glDisable, (GLenum cap))                                                                           \
	X(void, glBlendFunc, (GLenum s, GLenum d))                                                                 \
	X(void, glDepthFunc, (GLenum f))                                                                           \
	X(void, glDepthMask, (GLboolean f))                                                                        \
	X(void, glDepthRange, (double n, double f))                                                                \
	X(void, glCullFace, (GLenum m))                                                                            \
	X(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                                 \
	X(void, glPolygonOffset, (GLfloat factor, GLfloat units))                                                  \
	X(void, glLineWidth, (GLfloat w))                                                                          \
	X(void, glPixelStorei, (GLenum pname, GLint param))                                                        \
	X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p))               \
	X(void, glFinish, (void))                                                                                  \
	X(void, glGenTextures, (GLsizei n, GLuint *t))                                                             \
	X(void, glDeleteTextures, (GLsizei n, const GLuint *t))                                                    \
	X(void, glBindTexture, (GLenum target, GLuint t))                                                          \
	X(void, glActiveTexture, (GLenum unit))                                                                    \
	X(void, glTexImage2D, (GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty,   \
	                       const void *p))                                                                     \
	X(void, glTexSubImage2D, (GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty,  \
	                          const void *p))                                                                  \
	X(void, glTexParameteri, (GLenum t, GLenum p, GLint v))                                                    \
	X(void, glTexParameterf, (GLenum t, GLenum p, GLfloat v))                                                  \
	X(void, glGenerateMipmap, (GLenum t))                                                                      \
	X(void, glGenBuffers, (GLsizei n, GLuint *b))                                                              \
	X(void, glDeleteBuffers, (GLsizei n, const GLuint *b))                                                     \
	X(void, glBindBuffer, (GLenum t, GLuint b))                                                                \
	X(void, glBufferData, (GLenum t, GLsizeiptr size, const void *data, GLenum usage))                         \
	X(void, glBufferSubData, (GLenum t, GLintptr ofs, GLsizeiptr size, const void *data))                      \
	X(void, glGenVertexArrays, (GLsizei n, GLuint *a))                                                         \
	X(void, glDeleteVertexArrays, (GLsizei n, const GLuint *a))                                                \
	X(void, glBindVertexArray, (GLuint a))                                                                     \
	X(void, glEnableVertexAttribArray, (GLuint i))                                                             \
	X(void, glDisableVertexAttribArray, (GLuint i))                                                            \
	X(void, glVertexAttribPointer, (GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride,         \
	                                const void *p))                                                            \
	X(void, glVertexAttrib4f, (GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w))                          \
	X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))                                           \
	X(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void *ofs))                        \
	X(void, glDrawElementsBaseVertex, (GLenum mode, GLsizei count, GLenum type, const void *ofs, GLint base))  \
	X(GLuint, glCreateShader, (GLenum type))                                                                   \
	X(void, glDeleteShader, (GLuint s))                                                                        \
	X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar *const *src, const GLint *len))                 \
	X(void, glCompileShader, (GLuint s))                                                                       \
	X(void, glGetShaderiv, (GLuint s, GLenum p, GLint *v))                                                     \
	X(void, glGetShaderInfoLog, (GLuint s, GLsizei max, GLsizei *len, GLchar *log))                            \
	X(GLuint, glCreateProgram, (void))                                                                         \
	X(void, glDeleteProgram, (GLuint p))                                                                       \
	X(void, glAttachShader, (GLuint p, GLuint s))                                                              \
	X(void, glBindAttribLocation, (GLuint p, GLuint i, const GLchar *name))                                    \
	X(void, glLinkProgram, (GLuint p))                                                                         \
	X(void, glGetProgramiv, (GLuint p, GLenum pn, GLint *v))                                                   \
	X(void, glGetProgramInfoLog, (GLuint p, GLsizei max, GLsizei *len, GLchar *log))                           \
	X(void, glUseProgram, (GLuint p))                                                                          \
	X(GLint, glGetUniformLocation, (GLuint p, const GLchar *name))                                             \
	X(void, glUniform1i, (GLint l, GLint v))                                                                   \
	X(void, glUniform1f, (GLint l, GLfloat v))                                                                 \
	X(void, glUniform2f, (GLint l, GLfloat a, GLfloat b))                                                      \
	X(void, glUniform3f, (GLint l, GLfloat a, GLfloat b, GLfloat c))                                           \
	X(void, glUniform4f, (GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d))                                \
	X(void, glUniform3fv, (GLint l, GLsizei n, const GLfloat *v))                                              \
	X(void, glUniform4fv, (GLint l, GLsizei n, const GLfloat *v))                                              \
	X(void, glUniformMatrix4fv, (GLint l, GLsizei n, GLboolean t, const GLfloat *v))

#define X(ret, name, args) typedef ret(BWGLAPI *PFN_##name) args;
BW_GL_FUNCS(X)
#undef X
#define X(ret, name, args) extern PFN_##name q##name;
BW_GL_FUNCS(X)
#undef X

/* loads every entry point; false (with a message) if one is missing */
int GL_LoadFunctions(void *(*getproc)(const char *name));

#endif
