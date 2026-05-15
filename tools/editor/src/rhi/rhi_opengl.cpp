#include "rhi_opengl.hpp"
#include "../gl_helpers.hpp"

#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>

// ---------- Framebuffer internal data ----------

struct GLFramebufferData
{
    uint32_t fbo         = 0;
    uint32_t colorTex    = 0;
    uint32_t depthRBO    = 0;
    int      width       = 0;
    int      height      = 0;
};

static RhiFramebuffer PackFB(GLFramebufferData* p) { return (RhiFramebuffer)(uintptr_t)p; }
static GLFramebufferData* UnpackFB(RhiFramebuffer h) { return (GLFramebufferData*)(uintptr_t)h; }

// ---------- Lifecycle ----------

bool RhiOpenGL::Init(SDL_Window* window)
{
    m_window = window;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    m_glContext = SDL_GL_CreateContext(window);
    if (!m_glContext)
    {
        fprintf(stderr, "RhiOpenGL: SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_MakeCurrent(window, m_glContext);
    SDL_GL_SetSwapInterval(1);

    if (!GL_LoadFunctions())
    {
        fprintf(stderr, "RhiOpenGL: GL_LoadFunctions failed\n");
        return false;
    }

    return true;
}

void RhiOpenGL::Shutdown()
{
    if (m_glContext)
    {
        SDL_GL_DestroyContext(m_glContext);
        m_glContext = nullptr;
    }
}

// ---------- ImGui ----------

void RhiOpenGL::ImGuiInit(SDL_Window* window)
{
    ImGui_ImplSDL3_InitForOpenGL(window, m_glContext);
    ImGui_ImplOpenGL3_Init("#version 330");
}

void RhiOpenGL::ImGuiShutdown()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
}

void RhiOpenGL::ImGuiNewFrame()
{
    ImGui_ImplOpenGL3_NewFrame();
}

void RhiOpenGL::ImGuiRender()
{
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void RhiOpenGL::ImGuiUpdatePlatformWindows()
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        SDL_Window* backup = SDL_GL_GetCurrentWindow();
        SDL_GLContext backupCtx = SDL_GL_GetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        SDL_GL_MakeCurrent(backup, backupCtx);
    }
}

// ---------- Frame ----------

void RhiOpenGL::BeginFrame()
{
    // No-op for OpenGL (no swapchain acquire)
}

void RhiOpenGL::EndFrame()
{
    SDL_GL_SwapWindow(m_window);
}

// ---------- Buffers ----------

RhiBuffer RhiOpenGL::CreateBuffer(RhiBufferUsage usage, const void* data, size_t sizeBytes)
{
    uint32_t vbo = 0;
    ed_glGenBuffers(1, &vbo);

    GLenum target = (usage == RhiBufferUsage::Index) ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    ed_glBindBuffer(target, vbo);
    ed_glBufferData(target, (ptrdiff_t)sizeBytes, data, GL_STATIC_DRAW);
    ed_glBindBuffer(target, 0);

    return (RhiBuffer)vbo;
}

void RhiOpenGL::UpdateBuffer(RhiBuffer buf, const void* data, size_t sizeBytes)
{
    uint32_t vbo = (uint32_t)buf;
    ed_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    ed_glBufferData(GL_ARRAY_BUFFER, (ptrdiff_t)sizeBytes, data, GL_DYNAMIC_DRAW);
    ed_glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void RhiOpenGL::DestroyBuffer(RhiBuffer buf)
{
    if (!buf) return;
    uint32_t vbo = (uint32_t)buf;
    ed_glDeleteBuffers(1, &vbo);
}

// ---------- Vertex layouts (VAO) ----------

RhiVertexLayout RhiOpenGL::CreateVertexLayout(
    RhiBuffer vertexBuffer,
    const std::vector<RhiVertexAttrib>& attribs,
    RhiBuffer indexBuffer)
{
    uint32_t vao = 0;
    ed_glGenVertexArrays(1, &vao);
    ed_glBindVertexArray(vao);

    ed_glBindBuffer(GL_ARRAY_BUFFER, (uint32_t)vertexBuffer);

    for (const auto& a : attribs)
    {
        ed_glEnableVertexAttribArray((uint32_t)a.location);
        ed_glVertexAttribPointer(
            (uint32_t)a.location,
            a.components,
            GL_FLOAT,
            GL_FALSE,
            a.strideBytes,
            (const void*)(intptr_t)a.offsetBytes);
    }

    if (indexBuffer != RHI_NULL_BUFFER)
        ed_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (uint32_t)indexBuffer);

    ed_glBindVertexArray(0);
    return (RhiVertexLayout)vao;
}

void RhiOpenGL::DestroyVertexLayout(RhiVertexLayout vl)
{
    if (!vl) return;
    uint32_t vao = (uint32_t)vl;
    ed_glDeleteVertexArrays(1, &vao);
}

// ---------- Textures ----------

RhiTexture RhiOpenGL::CreateTexture2D(int width, int height, const void* rgbaPixels,
                                       bool generateMipmaps, bool linearFilter)
{
    uint32_t tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);

    if (linearFilter)
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, generateMipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    else
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    if (generateMipmaps)
        ed_glGenerateMipmap(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, 0);
    return (RhiTexture)tex;
}

void RhiOpenGL::DestroyTexture(RhiTexture tex)
{
    if (!tex) return;
    uint32_t t = (uint32_t)tex;
    glDeleteTextures(1, &t);
}

ImTextureID RhiOpenGL::GetImGuiTextureID(RhiTexture tex)
{
    return (ImTextureID)(intptr_t)(uint32_t)tex;
}

// ---------- Framebuffers ----------

RhiFramebuffer RhiOpenGL::CreateFramebuffer(int width, int height)
{
    auto* fb = new GLFramebufferData();
    fb->width = width;
    fb->height = height;

    ed_glGenFramebuffers(1, &fb->fbo);
    ed_glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);

    glGenTextures(1, &fb->colorTex);
    glBindTexture(GL_TEXTURE_2D, fb->colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ed_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb->colorTex, 0);

    ed_glGenRenderbuffers(1, &fb->depthRBO);
    ed_glBindRenderbuffer(GL_RENDERBUFFER, fb->depthRBO);
    ed_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    ed_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fb->depthRBO);

    ed_glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return PackFB(fb);
}

void RhiOpenGL::DestroyFramebuffer(RhiFramebuffer handle)
{
    if (!handle) return;
    GLFramebufferData* fb = UnpackFB(handle);
    if (fb->fbo)       ed_glDeleteFramebuffers(1, &fb->fbo);
    if (fb->colorTex)  glDeleteTextures(1, &fb->colorTex);
    if (fb->depthRBO)  ed_glDeleteRenderbuffers(1, &fb->depthRBO);
    delete fb;
}

void RhiOpenGL::BindFramebuffer(RhiFramebuffer handle)
{
    GLFramebufferData* fb = UnpackFB(handle);
    ed_glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
}

void RhiOpenGL::UnbindFramebuffer()
{
    ed_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

ImTextureID RhiOpenGL::GetFramebufferImGuiTexture(RhiFramebuffer handle)
{
    GLFramebufferData* fb = UnpackFB(handle);
    return (ImTextureID)(intptr_t)fb->colorTex;
}

void RhiOpenGL::ReadPixel(RhiFramebuffer handle, int x, int y, unsigned char rgba[4])
{
    GLFramebufferData* fb = UnpackFB(handle);
    ed_glBindFramebuffer(GL_READ_FRAMEBUFFER, fb->fbo);
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    ed_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

void RhiOpenGL::ReadFramebufferPixels(RhiFramebuffer handle, int x, int y, int w, int h, void* outRGBA)
{
    GLFramebufferData* fb = UnpackFB(handle);
    ed_glBindFramebuffer(GL_READ_FRAMEBUFFER, fb->fbo);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, outRGBA);
    ed_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

// ---------- Shaders ----------

static const char* kSceneVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
uniform mat4 uMVP;
out vec3 vNormal;
out vec2 vUV;
void main()
{
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNormal = aNormal;
    vUV = aUV;
}
)";

static const char* kSceneFragmentShader = R"(
#version 330 core
in vec3 vNormal;
in vec2 vUV;
uniform vec4 uColor;
uniform int uUseLighting;
uniform int uUseVertexColor;
uniform int uUseTexture;
uniform sampler2D uTexture;
out vec4 FragColor;
void main()
{
    if (uUseVertexColor != 0)
    {
        FragColor = vec4(vNormal, 1.0);
    }
    else if (uUseLighting != 0)
    {
        vec3 lightDir = normalize(vec3(0.3, 0.8, 0.5));
        float ndl = max(dot(normalize(vNormal), lightDir), 0.0);
        float ambient = 0.25;
        float lit = ambient + (1.0 - ambient) * ndl;
        vec3 color = uColor.rgb * lit;
        if (uUseTexture != 0)
        {
            color *= texture(uTexture, vUV).rgb;
        }
        FragColor = vec4(color, uColor.a);
    }
    else
    {
        FragColor = uColor;
    }
}
)";

static const char* kPickVertexShader = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 3) in float aPickId;
uniform mat4 uMVP;
flat out float vPickId;
void main()
{
    gl_Position = uMVP * vec4(aPos, 1.0);
    vPickId = aPickId;
}
)";

static const char* kPickFragmentShader = R"(
#version 330 core
flat in float vPickId;
out vec4 FragColor;
void main()
{
    int id = int(vPickId + 0.5);
    FragColor = vec4(float(id & 0xFF) / 255.0, float((id >> 8) & 0xFF) / 255.0, 0.0, 1.0);
}
)";

static uint32_t CompileGLShader(uint32_t type, const char* source)
{
    uint32_t shader = ed_glCreateShader(type);
    ed_glShaderSource(shader, 1, &source, nullptr);
    ed_glCompileShader(shader);

    GLint status = 0;
    ed_glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status)
    {
        char log[512];
        ed_glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        fprintf(stderr, "RhiOpenGL shader compile error: %s\n", log);
        ed_glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static uint32_t LinkGLProgram(uint32_t vs, uint32_t fs)
{
    uint32_t prog = ed_glCreateProgram();
    ed_glAttachShader(prog, vs);
    ed_glAttachShader(prog, fs);
    ed_glLinkProgram(prog);

    GLint status = 0;
    ed_glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status)
    {
        char log[512];
        ed_glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "RhiOpenGL shader link error: %s\n", log);
    }

    ed_glDeleteShader(vs);
    ed_glDeleteShader(fs);
    return prog;
}

RhiShader RhiOpenGL::CreateSceneShader()
{
    uint32_t vs = CompileGLShader(GL_VERTEX_SHADER, kSceneVertexShader);
    uint32_t fs = CompileGLShader(GL_FRAGMENT_SHADER, kSceneFragmentShader);
    if (!vs || !fs) return RHI_NULL_SHADER;

    uint32_t prog = LinkGLProgram(vs, fs);

    m_sceneLocMVP            = ed_glGetUniformLocation(prog, "uMVP");
    m_sceneLocColor          = ed_glGetUniformLocation(prog, "uColor");
    m_sceneLocUseLighting    = ed_glGetUniformLocation(prog, "uUseLighting");
    m_sceneLocUseVertexColor = ed_glGetUniformLocation(prog, "uUseVertexColor");
    m_sceneLocUseTexture     = ed_glGetUniformLocation(prog, "uUseTexture");
    m_sceneLocTexSampler     = ed_glGetUniformLocation(prog, "uTexture");

    return (RhiShader)prog;
}

RhiShader RhiOpenGL::CreatePickShader()
{
    uint32_t vs = CompileGLShader(GL_VERTEX_SHADER, kPickVertexShader);
    uint32_t fs = CompileGLShader(GL_FRAGMENT_SHADER, kPickFragmentShader);
    if (!vs || !fs) return RHI_NULL_SHADER;

    uint32_t prog = LinkGLProgram(vs, fs);
    m_pickLocMVP = ed_glGetUniformLocation(prog, "uMVP");

    return (RhiShader)prog;
}

void RhiOpenGL::DestroyShader(RhiShader shader)
{
    if (!shader) return;
    ed_glDeleteProgram((uint32_t)shader);
}

void RhiOpenGL::BindShader(RhiShader shader)
{
    ed_glUseProgram((uint32_t)shader);
}

// ---------- Uniforms ----------

void RhiOpenGL::SetUniformMat4(int location, const float* mat4x4)
{
    if (location >= 0)
        ed_glUniformMatrix4fv(location, 1, GL_FALSE, mat4x4);
}

void RhiOpenGL::SetUniformVec4(int location, float x, float y, float z, float w)
{
    if (location >= 0)
        ed_glUniform4f(location, x, y, z, w);
}

void RhiOpenGL::SetUniformInt(int location, int value)
{
    if (location >= 0)
        ed_glUniform1i(location, value);
}

// ---------- Render state ----------

void RhiOpenGL::SetViewport(int x, int y, int w, int h)
{
    glViewport(x, y, w, h);
}

void RhiOpenGL::Clear(float r, float g, float b, float a)
{
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void RhiOpenGL::SetDepthTest(bool enable)
{
    if (enable) glEnable(GL_DEPTH_TEST);
    else        glDisable(GL_DEPTH_TEST);
}

void RhiOpenGL::SetCullFace(bool enable)
{
    if (enable) glEnable(GL_CULL_FACE);
    else        glDisable(GL_CULL_FACE);
}

void RhiOpenGL::SetBlend(bool enable)
{
    if (enable) glEnable(GL_BLEND);
    else        glDisable(GL_BLEND);
}

void RhiOpenGL::SetPolygonOffsetFill(bool enable, float factor, float units)
{
    if (enable)
    {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(factor, units);
    }
    else
    {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

void RhiOpenGL::SetLineWidth(float width)
{
    glLineWidth(width);
}

// ---------- Texture binding ----------

void RhiOpenGL::BindTexture(RhiTexture tex, int slot)
{
    ed_glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, (uint32_t)tex);
}

void RhiOpenGL::UnbindTexture(int slot)
{
    ed_glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ---------- Drawing ----------

void RhiOpenGL::BindVertexLayout(RhiVertexLayout vl)
{
    ed_glBindVertexArray((uint32_t)vl);
}

static GLenum ToGLPrimitive(RhiPrimitive prim)
{
    switch (prim)
    {
    case RhiPrimitive::Triangles: return GL_TRIANGLES;
    case RhiPrimitive::Lines:     return GL_LINES;
    }
    return GL_TRIANGLES;
}

void RhiOpenGL::Draw(RhiPrimitive prim, int first, int count)
{
    glDrawArrays(ToGLPrimitive(prim), first, count);
}

void RhiOpenGL::DrawIndexed(RhiPrimitive prim, int indexCount)
{
    glDrawElements(ToGLPrimitive(prim), indexCount, GL_UNSIGNED_INT, nullptr);
}

// ---------- Factory ----------

Rhi* Rhi::Create(bool useVulkan)
{
    (void)useVulkan;
    return new RhiOpenGL();
}
