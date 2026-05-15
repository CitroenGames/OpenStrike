#pragma once

#include <SDL3/SDL_opengl.h>

// ---- GL constants ----
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER                  0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER             0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER             0x8CA9
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0            0x8CE0
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT     0x821A
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8             0x88F0
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER                 0x8D41
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE         0x8CD5
#endif
#ifndef GL_RGBA8
#define GL_RGBA8                        0x8058
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE                0x812F
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL            0x813D
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0                     0x84C0
#endif

// Buffer/VAO constants
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER                 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER         0x8893
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW                  0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW                 0x88E8
#endif

// Shader constants
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER                0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER              0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS               0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS                  0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH              0x8B84
#endif

// Polygon offset
#ifndef GL_POLYGON_OFFSET_FILL
#define GL_POLYGON_OFFSET_FILL          0x8037
#endif
#ifndef GL_POLYGON_OFFSET_LINE
#define GL_POLYGON_OFFSET_LINE          0x2A02
#endif

// ---- GLchar typedef ----
#ifndef GL_VERSION_2_0
typedef char GLchar;
#endif

// ---- Framebuffer function pointer typedefs ----
typedef void   (APIENTRY *PFNGLGENFRAMEBUFFERSPROC_)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFNGLDELETEFRAMEBUFFERSPROC_)(GLsizei, const GLuint*);
typedef void   (APIENTRY *PFNGLBINDFRAMEBUFFERPROC_)(GLenum, GLuint);
typedef void   (APIENTRY *PFNGLFRAMEBUFFERTEXTURE2DPROC_)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void   (APIENTRY *PFNGLGENRENDERBUFFERSPROC_)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFNGLDELETERENDERBUFFERSPROC_)(GLsizei, const GLuint*);
typedef void   (APIENTRY *PFNGLBINDRENDERBUFFERPROC_)(GLenum, GLuint);
typedef void   (APIENTRY *PFNGLRENDERBUFFERSTORAGEPROC_)(GLenum, GLenum, GLsizei, GLsizei);
typedef void   (APIENTRY *PFNGLFRAMEBUFFERRENDERBUFFERPROC_)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *PFNGLCHECKFRAMEBUFFERSTATUSPROC_)(GLenum);

// ---- VAO/VBO function pointer typedefs ----
typedef void   (APIENTRY *PFNGLGENVERTEXARRAYSPROC_)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFNGLDELETEVERTEXARRAYSPROC_)(GLsizei, const GLuint*);
typedef void   (APIENTRY *PFNGLBINDVERTEXARRAYPROC_)(GLuint);
typedef void   (APIENTRY *PFNGLGENBUFFERSPROC_)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFNGLDELETEBUFFERSPROC_)(GLsizei, const GLuint*);
typedef void   (APIENTRY *PFNGLBINDBUFFERPROC_)(GLenum, GLuint);
typedef void   (APIENTRY *PFNGLBUFFERDATAPROC_)(GLenum, ptrdiff_t, const void*, GLenum);
typedef void   (APIENTRY *PFNGLENABLEVERTEXATTRIBARRAYPROC_)(GLuint);
typedef void   (APIENTRY *PFNGLDISABLEVERTEXATTRIBARRAYPROC_)(GLuint);
typedef void   (APIENTRY *PFNGLVERTEXATTRIBPOINTERPROC_)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);

// ---- Shader function pointer typedefs ----
typedef GLuint (APIENTRY *PFNGLCREATESHADERPROC_)(GLenum);
typedef void   (APIENTRY *PFNGLDELETESHADERPROC_)(GLuint);
typedef void   (APIENTRY *PFNGLSHADERSOURCEPROC_)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void   (APIENTRY *PFNGLCOMPILESHADERPROC_)(GLuint);
typedef void   (APIENTRY *PFNGLGETSHADERIVPROC_)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY *PFNGLGETSHADERINFOLOGPROC_)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLuint (APIENTRY *PFNGLCREATEPROGRAMPROC_)();
typedef void   (APIENTRY *PFNGLDELETEPROGRAMPROC_)(GLuint);
typedef void   (APIENTRY *PFNGLATTACHSHADERPROC_)(GLuint, GLuint);
typedef void   (APIENTRY *PFNGLLINKPROGRAMPROC_)(GLuint);
typedef void   (APIENTRY *PFNGLUSEPROGRAMPROC_)(GLuint);
typedef void   (APIENTRY *PFNGLGETPROGRAMIVPROC_)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY *PFNGLGETPROGRAMINFOLOGPROC_)(GLuint, GLsizei, GLsizei*, GLchar*);

// ---- Uniform function pointer typedefs ----
typedef GLint  (APIENTRY *PFNGLGETUNIFORMLOCATIONPROC_)(GLuint, const GLchar*);
typedef void   (APIENTRY *PFNGLUNIFORMMATRIX4FVPROC_)(GLint, GLsizei, GLboolean, const GLfloat*);
typedef void   (APIENTRY *PFNGLUNIFORM3FPROC_)(GLint, GLfloat, GLfloat, GLfloat);
typedef void   (APIENTRY *PFNGLUNIFORM4FPROC_)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (APIENTRY *PFNGLUNIFORM1IPROC_)(GLint, GLint);

// ---- Texture function pointer typedefs ----
typedef void   (APIENTRY *PFNGLACTIVETEXTUREPROC_)(GLenum);
typedef void   (APIENTRY *PFNGLGENERATEMIPMAPPROC_)(GLenum);

// ---- Framebuffer function pointers ----
extern PFNGLGENFRAMEBUFFERSPROC_        ed_glGenFramebuffers;
extern PFNGLDELETEFRAMEBUFFERSPROC_     ed_glDeleteFramebuffers;
extern PFNGLBINDFRAMEBUFFERPROC_        ed_glBindFramebuffer;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC_   ed_glFramebufferTexture2D;
extern PFNGLGENRENDERBUFFERSPROC_       ed_glGenRenderbuffers;
extern PFNGLDELETERENDERBUFFERSPROC_    ed_glDeleteRenderbuffers;
extern PFNGLBINDRENDERBUFFERPROC_       ed_glBindRenderbuffer;
extern PFNGLRENDERBUFFERSTORAGEPROC_    ed_glRenderbufferStorage;
extern PFNGLFRAMEBUFFERRENDERBUFFERPROC_ ed_glFramebufferRenderbuffer;
extern PFNGLCHECKFRAMEBUFFERSTATUSPROC_ ed_glCheckFramebufferStatus;

// ---- VAO/VBO function pointers ----
extern PFNGLGENVERTEXARRAYSPROC_        ed_glGenVertexArrays;
extern PFNGLDELETEVERTEXARRAYSPROC_     ed_glDeleteVertexArrays;
extern PFNGLBINDVERTEXARRAYPROC_        ed_glBindVertexArray;
extern PFNGLGENBUFFERSPROC_             ed_glGenBuffers;
extern PFNGLDELETEBUFFERSPROC_          ed_glDeleteBuffers;
extern PFNGLBINDBUFFERPROC_             ed_glBindBuffer;
extern PFNGLBUFFERDATAPROC_             ed_glBufferData;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC_ ed_glEnableVertexAttribArray;
extern PFNGLDISABLEVERTEXATTRIBARRAYPROC_ ed_glDisableVertexAttribArray;
extern PFNGLVERTEXATTRIBPOINTERPROC_    ed_glVertexAttribPointer;

// ---- Shader function pointers ----
extern PFNGLCREATESHADERPROC_           ed_glCreateShader;
extern PFNGLDELETESHADERPROC_           ed_glDeleteShader;
extern PFNGLSHADERSOURCEPROC_           ed_glShaderSource;
extern PFNGLCOMPILESHADERPROC_          ed_glCompileShader;
extern PFNGLGETSHADERIVPROC_            ed_glGetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC_       ed_glGetShaderInfoLog;
extern PFNGLCREATEPROGRAMPROC_          ed_glCreateProgram;
extern PFNGLDELETEPROGRAMPROC_          ed_glDeleteProgram;
extern PFNGLATTACHSHADERPROC_           ed_glAttachShader;
extern PFNGLLINKPROGRAMPROC_            ed_glLinkProgram;
extern PFNGLUSEPROGRAMPROC_             ed_glUseProgram;
extern PFNGLGETPROGRAMIVPROC_           ed_glGetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC_      ed_glGetProgramInfoLog;

// ---- Uniform function pointers ----
extern PFNGLGETUNIFORMLOCATIONPROC_     ed_glGetUniformLocation;
extern PFNGLUNIFORMMATRIX4FVPROC_       ed_glUniformMatrix4fv;
extern PFNGLUNIFORM3FPROC_              ed_glUniform3f;
extern PFNGLUNIFORM4FPROC_              ed_glUniform4f;
extern PFNGLUNIFORM1IPROC_              ed_glUniform1i;

// ---- Texture function pointers ----
extern PFNGLACTIVETEXTUREPROC_          ed_glActiveTexture;
extern PFNGLGENERATEMIPMAPPROC_         ed_glGenerateMipmap;

bool GL_LoadFunctions();
