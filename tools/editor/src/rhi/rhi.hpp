#pragma once

#include "rhi_types.hpp"
#include <vector>

struct SDL_Window;

class Rhi
{
public:
    virtual ~Rhi() = default;

    // --- Lifecycle ---
    virtual bool Init(SDL_Window* window) = 0;
    virtual void Shutdown() = 0;

    // --- ImGui integration ---
    virtual void ImGuiInit(SDL_Window* window) = 0;
    virtual void ImGuiShutdown() = 0;
    virtual void ImGuiNewFrame() = 0;
    virtual void ImGuiRender() = 0;
    virtual void ImGuiUpdatePlatformWindows() = 0;

    // --- Frame ---
    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;

    // --- Buffers ---
    virtual RhiBuffer CreateBuffer(RhiBufferUsage usage, const void* data, size_t sizeBytes) = 0;
    virtual void UpdateBuffer(RhiBuffer buf, const void* data, size_t sizeBytes) = 0;
    virtual void DestroyBuffer(RhiBuffer buf) = 0;

    // --- Vertex layouts ---
    virtual RhiVertexLayout CreateVertexLayout(
        RhiBuffer vertexBuffer,
        const std::vector<RhiVertexAttrib>& attribs,
        RhiBuffer indexBuffer = RHI_NULL_BUFFER) = 0;
    virtual void DestroyVertexLayout(RhiVertexLayout vl) = 0;

    // --- Textures ---
    virtual RhiTexture CreateTexture2D(int width, int height, const void* rgbaPixels,
                                       bool generateMipmaps, bool linearFilter = true) = 0;
    virtual void DestroyTexture(RhiTexture tex) = 0;
    virtual ImTextureID GetImGuiTextureID(RhiTexture tex) = 0;

    // --- Framebuffers ---
    virtual RhiFramebuffer CreateFramebuffer(int width, int height) = 0;
    virtual void DestroyFramebuffer(RhiFramebuffer fb) = 0;
    virtual void BindFramebuffer(RhiFramebuffer fb) = 0;
    virtual void UnbindFramebuffer() = 0;
    virtual ImTextureID GetFramebufferImGuiTexture(RhiFramebuffer fb) = 0;
    virtual void ReadPixel(RhiFramebuffer fb, int x, int y, unsigned char rgba[4]) = 0;
    virtual void ReadFramebufferPixels(RhiFramebuffer fb, int x, int y, int w, int h, void* outRGBA) = 0;

    // --- Shaders ---
    virtual RhiShader CreateSceneShader() = 0;
    virtual RhiShader CreatePickShader() = 0;
    virtual void DestroyShader(RhiShader shader) = 0;
    virtual void BindShader(RhiShader shader) = 0;

    // --- Uniforms ---
    virtual void SetUniformMat4(int location, const float* mat4x4) = 0;
    virtual void SetUniformVec4(int location, float x, float y, float z, float w) = 0;
    virtual void SetUniformInt(int location, int value) = 0;

    // Uniform locations for the scene shader
    virtual int GetSceneUniformMVP() = 0;
    virtual int GetSceneUniformColor() = 0;
    virtual int GetSceneUniformUseLighting() = 0;
    virtual int GetSceneUniformUseVertexColor() = 0;
    virtual int GetSceneUniformUseTexture() = 0;
    virtual int GetSceneUniformTexSampler() = 0;
    // Uniform locations for the pick shader
    virtual int GetPickUniformMVP() = 0;

    // --- Render state ---
    virtual void SetViewport(int x, int y, int w, int h) = 0;
    virtual void Clear(float r, float g, float b, float a) = 0;
    virtual void SetDepthTest(bool enable) = 0;
    virtual void SetCullFace(bool enable) = 0;
    virtual void SetBlend(bool enable) = 0;
    virtual void SetPolygonOffsetFill(bool enable, float factor = 1.0f, float units = 1.0f) = 0;
    virtual void SetLineWidth(float width) = 0;

    // --- Texture binding ---
    virtual void BindTexture(RhiTexture tex, int slot = 0) = 0;
    virtual void UnbindTexture(int slot = 0) = 0;

    // --- Drawing ---
    virtual void BindVertexLayout(RhiVertexLayout vl) = 0;
    virtual void Draw(RhiPrimitive prim, int first, int count) = 0;
    virtual void DrawIndexed(RhiPrimitive prim, int indexCount) = 0;

    // --- Factory ---
    static Rhi* Create(bool useVulkan);
};
