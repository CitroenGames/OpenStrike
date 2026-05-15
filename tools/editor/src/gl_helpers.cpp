#include "gl_helpers.hpp"
#include <SDL3/SDL.h>

// ---- Framebuffer ----
PFNGLGENFRAMEBUFFERSPROC_        ed_glGenFramebuffers        = nullptr;
PFNGLDELETEFRAMEBUFFERSPROC_     ed_glDeleteFramebuffers     = nullptr;
PFNGLBINDFRAMEBUFFERPROC_        ed_glBindFramebuffer        = nullptr;
PFNGLFRAMEBUFFERTEXTURE2DPROC_   ed_glFramebufferTexture2D   = nullptr;
PFNGLGENRENDERBUFFERSPROC_       ed_glGenRenderbuffers       = nullptr;
PFNGLDELETERENDERBUFFERSPROC_    ed_glDeleteRenderbuffers    = nullptr;
PFNGLBINDRENDERBUFFERPROC_       ed_glBindRenderbuffer       = nullptr;
PFNGLRENDERBUFFERSTORAGEPROC_    ed_glRenderbufferStorage    = nullptr;
PFNGLFRAMEBUFFERRENDERBUFFERPROC_ ed_glFramebufferRenderbuffer = nullptr;
PFNGLCHECKFRAMEBUFFERSTATUSPROC_ ed_glCheckFramebufferStatus = nullptr;

// ---- VAO/VBO ----
PFNGLGENVERTEXARRAYSPROC_        ed_glGenVertexArrays        = nullptr;
PFNGLDELETEVERTEXARRAYSPROC_     ed_glDeleteVertexArrays     = nullptr;
PFNGLBINDVERTEXARRAYPROC_        ed_glBindVertexArray        = nullptr;
PFNGLGENBUFFERSPROC_             ed_glGenBuffers             = nullptr;
PFNGLDELETEBUFFERSPROC_          ed_glDeleteBuffers          = nullptr;
PFNGLBINDBUFFERPROC_             ed_glBindBuffer             = nullptr;
PFNGLBUFFERDATAPROC_             ed_glBufferData             = nullptr;
PFNGLENABLEVERTEXATTRIBARRAYPROC_ ed_glEnableVertexAttribArray = nullptr;
PFNGLDISABLEVERTEXATTRIBARRAYPROC_ ed_glDisableVertexAttribArray = nullptr;
PFNGLVERTEXATTRIBPOINTERPROC_   ed_glVertexAttribPointer    = nullptr;

// ---- Shader ----
PFNGLCREATESHADERPROC_           ed_glCreateShader           = nullptr;
PFNGLDELETESHADERPROC_           ed_glDeleteShader           = nullptr;
PFNGLSHADERSOURCEPROC_           ed_glShaderSource           = nullptr;
PFNGLCOMPILESHADERPROC_          ed_glCompileShader          = nullptr;
PFNGLGETSHADERIVPROC_            ed_glGetShaderiv            = nullptr;
PFNGLGETSHADERINFOLOGPROC_       ed_glGetShaderInfoLog       = nullptr;
PFNGLCREATEPROGRAMPROC_          ed_glCreateProgram          = nullptr;
PFNGLDELETEPROGRAMPROC_          ed_glDeleteProgram          = nullptr;
PFNGLATTACHSHADERPROC_           ed_glAttachShader           = nullptr;
PFNGLLINKPROGRAMPROC_            ed_glLinkProgram            = nullptr;
PFNGLUSEPROGRAMPROC_             ed_glUseProgram             = nullptr;
PFNGLGETPROGRAMIVPROC_           ed_glGetProgramiv           = nullptr;
PFNGLGETPROGRAMINFOLOGPROC_      ed_glGetProgramInfoLog      = nullptr;

// ---- Uniform ----
PFNGLGETUNIFORMLOCATIONPROC_     ed_glGetUniformLocation     = nullptr;
PFNGLUNIFORMMATRIX4FVPROC_       ed_glUniformMatrix4fv       = nullptr;
PFNGLUNIFORM3FPROC_              ed_glUniform3f              = nullptr;
PFNGLUNIFORM4FPROC_              ed_glUniform4f              = nullptr;
PFNGLUNIFORM1IPROC_              ed_glUniform1i              = nullptr;

// ---- Texture ----
PFNGLACTIVETEXTUREPROC_          ed_glActiveTexture          = nullptr;
PFNGLGENERATEMIPMAPPROC_         ed_glGenerateMipmap         = nullptr;

#define ED_LOAD(ptr, type, name) \
    ptr = (type)SDL_GL_GetProcAddress(name); \
    if (!ptr) return false;

bool GL_LoadFunctions()
{
    // Framebuffer
    ED_LOAD(ed_glGenFramebuffers,        PFNGLGENFRAMEBUFFERSPROC_,        "glGenFramebuffers");
    ED_LOAD(ed_glDeleteFramebuffers,     PFNGLDELETEFRAMEBUFFERSPROC_,     "glDeleteFramebuffers");
    ED_LOAD(ed_glBindFramebuffer,        PFNGLBINDFRAMEBUFFERPROC_,        "glBindFramebuffer");
    ED_LOAD(ed_glFramebufferTexture2D,   PFNGLFRAMEBUFFERTEXTURE2DPROC_,   "glFramebufferTexture2D");
    ED_LOAD(ed_glGenRenderbuffers,       PFNGLGENRENDERBUFFERSPROC_,       "glGenRenderbuffers");
    ED_LOAD(ed_glDeleteRenderbuffers,    PFNGLDELETERENDERBUFFERSPROC_,    "glDeleteRenderbuffers");
    ED_LOAD(ed_glBindRenderbuffer,       PFNGLBINDRENDERBUFFERPROC_,       "glBindRenderbuffer");
    ED_LOAD(ed_glRenderbufferStorage,    PFNGLRENDERBUFFERSTORAGEPROC_,    "glRenderbufferStorage");
    ED_LOAD(ed_glFramebufferRenderbuffer,PFNGLFRAMEBUFFERRENDERBUFFERPROC_,"glFramebufferRenderbuffer");
    ED_LOAD(ed_glCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC_, "glCheckFramebufferStatus");

    // VAO/VBO
    ED_LOAD(ed_glGenVertexArrays,        PFNGLGENVERTEXARRAYSPROC_,        "glGenVertexArrays");
    ED_LOAD(ed_glDeleteVertexArrays,     PFNGLDELETEVERTEXARRAYSPROC_,     "glDeleteVertexArrays");
    ED_LOAD(ed_glBindVertexArray,        PFNGLBINDVERTEXARRAYPROC_,        "glBindVertexArray");
    ED_LOAD(ed_glGenBuffers,             PFNGLGENBUFFERSPROC_,             "glGenBuffers");
    ED_LOAD(ed_glDeleteBuffers,          PFNGLDELETEBUFFERSPROC_,          "glDeleteBuffers");
    ED_LOAD(ed_glBindBuffer,             PFNGLBINDBUFFERPROC_,             "glBindBuffer");
    ED_LOAD(ed_glBufferData,             PFNGLBUFFERDATAPROC_,             "glBufferData");
    ED_LOAD(ed_glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC_, "glEnableVertexAttribArray");
    ED_LOAD(ed_glDisableVertexAttribArray, PFNGLDISABLEVERTEXATTRIBARRAYPROC_, "glDisableVertexAttribArray");
    ED_LOAD(ed_glVertexAttribPointer,    PFNGLVERTEXATTRIBPOINTERPROC_,    "glVertexAttribPointer");

    // Shader
    ED_LOAD(ed_glCreateShader,           PFNGLCREATESHADERPROC_,           "glCreateShader");
    ED_LOAD(ed_glDeleteShader,           PFNGLDELETESHADERPROC_,           "glDeleteShader");
    ED_LOAD(ed_glShaderSource,           PFNGLSHADERSOURCEPROC_,           "glShaderSource");
    ED_LOAD(ed_glCompileShader,          PFNGLCOMPILESHADERPROC_,          "glCompileShader");
    ED_LOAD(ed_glGetShaderiv,            PFNGLGETSHADERIVPROC_,            "glGetShaderiv");
    ED_LOAD(ed_glGetShaderInfoLog,       PFNGLGETSHADERINFOLOGPROC_,       "glGetShaderInfoLog");
    ED_LOAD(ed_glCreateProgram,          PFNGLCREATEPROGRAMPROC_,          "glCreateProgram");
    ED_LOAD(ed_glDeleteProgram,          PFNGLDELETEPROGRAMPROC_,          "glDeleteProgram");
    ED_LOAD(ed_glAttachShader,           PFNGLATTACHSHADERPROC_,           "glAttachShader");
    ED_LOAD(ed_glLinkProgram,            PFNGLLINKPROGRAMPROC_,            "glLinkProgram");
    ED_LOAD(ed_glUseProgram,             PFNGLUSEPROGRAMPROC_,             "glUseProgram");
    ED_LOAD(ed_glGetProgramiv,           PFNGLGETPROGRAMIVPROC_,           "glGetProgramiv");
    ED_LOAD(ed_glGetProgramInfoLog,      PFNGLGETPROGRAMINFOLOGPROC_,      "glGetProgramInfoLog");

    // Uniform
    ED_LOAD(ed_glGetUniformLocation,     PFNGLGETUNIFORMLOCATIONPROC_,     "glGetUniformLocation");
    ED_LOAD(ed_glUniformMatrix4fv,       PFNGLUNIFORMMATRIX4FVPROC_,       "glUniformMatrix4fv");
    ED_LOAD(ed_glUniform3f,              PFNGLUNIFORM3FPROC_,              "glUniform3f");
    ED_LOAD(ed_glUniform4f,              PFNGLUNIFORM4FPROC_,              "glUniform4f");
    ED_LOAD(ed_glUniform1i,              PFNGLUNIFORM1IPROC_,              "glUniform1i");

    // Texture
    ED_LOAD(ed_glActiveTexture,          PFNGLACTIVETEXTUREPROC_,          "glActiveTexture");
    ED_LOAD(ed_glGenerateMipmap,         PFNGLGENERATEMIPMAPPROC_,         "glGenerateMipmap");

    return true;
}
