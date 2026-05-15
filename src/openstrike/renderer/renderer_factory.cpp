#include "openstrike/renderer/renderer_factory.hpp"

#include "openstrike/renderer/null_renderer.hpp"

#if defined(_WIN32)
#include "openstrike/renderer/dx12_renderer.hpp"
#endif

#if defined(__APPLE__)
#include "openstrike/renderer/metal_renderer.hpp"
#endif

namespace openstrike
{
std::unique_ptr<IRenderer> create_renderer(const RuntimeConfig& config)
{
    if (config.mode == AppMode::DedicatedServer || config.renderer_backend == RendererBackend::Null)
    {
        return std::make_unique<NullRenderer>();
    }

#if defined(_WIN32)
    if (config.renderer_backend == RendererBackend::Auto || config.renderer_backend == RendererBackend::D3D12)
    {
        return std::make_unique<Dx12Renderer>();
    }
#endif

#if defined(__APPLE__)
    if (config.renderer_backend == RendererBackend::Auto || config.renderer_backend == RendererBackend::Metal)
    {
        return std::make_unique<MetalRenderer>();
    }
#endif

    return std::make_unique<NullRenderer>();
}
}
