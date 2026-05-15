#pragma once

#if defined(_WIN32)

#include <RmlUi/Core/RenderInterface.h>

#include <cstdint>
#include <memory>

struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace openstrike
{
class RmlDx12RenderInterface final : public Rml::RenderInterface
{
public:
    RmlDx12RenderInterface();
    ~RmlDx12RenderInterface() override;

    RmlDx12RenderInterface(const RmlDx12RenderInterface&) = delete;
    RmlDx12RenderInterface& operator=(const RmlDx12RenderInterface&) = delete;

    [[nodiscard]] bool initialize(ID3D12Device* device, ID3D12CommandQueue* command_queue);
    void shutdown();

    void begin_frame(ID3D12GraphicsCommandList* command_list, std::uint32_t frame_index, std::uint32_t width, std::uint32_t height);
    void end_frame();

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

#endif
