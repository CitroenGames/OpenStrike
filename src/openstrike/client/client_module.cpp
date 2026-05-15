#include "openstrike/client/client_module.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"

namespace openstrike
{
const char* ClientModule::name() const
{
    return "client";
}

void ClientModule::on_start(const RuntimeConfig& config, EngineContext& engine)
{
    log_info("client module started with content root '{}'", config.content_root.string());
    log_info("GAME search path: {}", engine.filesystem.search_path_string("GAME"));
}

void ClientModule::on_frame(const FrameContext& context, EngineContext&)
{
    if (context.frame_index == 0)
    {
        log_info("client first frame tick={} alpha={}", context.tick_index, context.interpolation_alpha);
    }
}
}
