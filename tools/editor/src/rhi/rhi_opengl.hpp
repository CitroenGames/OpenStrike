#pragma once

#include "rhi.hpp"
#include <SDL3/SDL.h>

class RhiOpenGL : public Rhi
{
public:
    bool Init(SDL_Window* window) override;
    void Shutdown() override;

    void ImGuiInit(SDL_Window* window) override;
    void ImGuiShutdown() override;
    void ImGuiNewFrame() override;
    void ImGuiRender() override;
    void ImGuiUpdatePlatformWindows() override;

    void BeginFrame() override;
    void EndFrame() override;

    RhiBuffer CreateBuffer(RhiBufferUsage usage, const void* data, size_t sizeBytes) override;
    void UpdateBuffer(RhiBuffer buf, const void* data, size_t sizeBytes) override;
    void DestroyBuffer(RhiBuffer buf) override;

    RhiVertexLayout CreateVertexLayout(
        RhiBuffer vertexBuffer,
        const std::vector<RhiVertexAttrib>& attribs,
        RhiBuffer indexBuffer = RHI_NULL_BUFFER) override;
    void DestroyVertexLayout(RhiVertexLayout vl) override;

    RhiTexture CreateTexture2D(int width, int height, const void* rgbaPixels,
                                bool generateMipmaps, bool linearFilter = true) override;
    void DestroyTexture(RhiTexture tex) override;
    ImTextureID GetImGuiTextureID(RhiTexture tex) override;

    RhiFramebuffer CreateFramebuffer(int width, int height) override;
    void DestroyFramebuffer(RhiFramebuffer fb) override;
    void BindFramebuffer(RhiFramebuffer fb) override;
    void UnbindFramebuffer() override;
    ImTextureID GetFramebufferImGuiTexture(RhiFramebuffer fb) override;
    void ReadPixel(RhiFramebuffer fb, int x, int y, unsigned char rgba[4]) override;
    void ReadFramebufferPixels(RhiFramebuffer fb, int x, int y, int w, int h, void* outRGBA) override;

    RhiShader CreateSceneShader() override;
    RhiShader CreatePickShader() override;
    void DestroyShader(RhiShader shader) override;
    void BindShader(RhiShader shader) override;

    void SetUniformMat4(int location, const float* mat4x4) override;
    void SetUniformVec4(int location, float x, float y, float z, float w) override;
    void SetUniformInt(int location, int value) override;

    int GetSceneUniformMVP() override           { return m_sceneLocMVP; }
    int GetSceneUniformColor() override         { return m_sceneLocColor; }
    int GetSceneUniformUseLighting() override   { return m_sceneLocUseLighting; }
    int GetSceneUniformUseVertexColor() override { return m_sceneLocUseVertexColor; }
    int GetSceneUniformUseTexture() override    { return m_sceneLocUseTexture; }
    int GetSceneUniformTexSampler() override    { return m_sceneLocTexSampler; }
    int GetPickUniformMVP() override            { return m_pickLocMVP; }

    void SetViewport(int x, int y, int w, int h) override;
    void Clear(float r, float g, float b, float a) override;
    void SetDepthTest(bool enable) override;
    void SetCullFace(bool enable) override;
    void SetBlend(bool enable) override;
    void SetPolygonOffsetFill(bool enable, float factor = 1.0f, float units = 1.0f) override;
    void SetLineWidth(float width) override;

    void BindTexture(RhiTexture tex, int slot = 0) override;
    void UnbindTexture(int slot = 0) override;

    void BindVertexLayout(RhiVertexLayout vl) override;
    void Draw(RhiPrimitive prim, int first, int count) override;
    void DrawIndexed(RhiPrimitive prim, int indexCount) override;

private:
    SDL_Window*  m_window    = nullptr;
    SDL_GLContext m_glContext = nullptr;

    // Cached uniform locations from scene/pick shader creation
    int m_sceneLocMVP            = -1;
    int m_sceneLocColor          = -1;
    int m_sceneLocUseLighting    = -1;
    int m_sceneLocUseVertexColor = -1;
    int m_sceneLocUseTexture     = -1;
    int m_sceneLocTexSampler     = -1;
    int m_pickLocMVP             = -1;
};
