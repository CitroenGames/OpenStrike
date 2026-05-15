#include "openstrike/editor/editor_module.hpp"

#include "openstrike/core/log.hpp"

namespace openstrike
{
const char* EditorModule::name() const
{
    return "editor";
}

void EditorModule::on_start(const RuntimeConfig&, EngineContext&)
{
    log_info("editor module started");
}

void EditorModule::on_frame(const FrameContext& context, EngineContext&)
{
    if (context.frame_index == 0)
    {
        log_info("editor shell attached to runtime frame loop");
    }
}
}
