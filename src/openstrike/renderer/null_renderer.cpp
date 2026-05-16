#include "openstrike/renderer/null_renderer.hpp"

#include "openstrike/core/log.hpp"

namespace openstrike
{
bool NullRenderer::initialize(const RuntimeConfig&)
{
    log_info("null renderer initialized");
    return true;
}

void NullRenderer::render(const RenderFrame& frame)
{
    const FrameContext& context = frame.timing;
    if (context.frame_index == 0)
    {
        log_info("null renderer first frame tick={} alpha={}", context.tick_index, context.interpolation_alpha);
    }
}

void NullRenderer::shutdown()
{
    log_info("null renderer shutdown");
}
}
