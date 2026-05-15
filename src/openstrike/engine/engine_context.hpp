#pragma once

#include "openstrike/core/console.hpp"
#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/engine/runtime_config.hpp"
#include "openstrike/world/world.hpp"

namespace openstrike
{
class EngineContext
{
public:
    ConsoleVariables variables;
    CommandRegistry commands;
    CommandBuffer command_buffer;
    ContentFileSystem filesystem;
    WorldManager world;
    bool quit_requested = false;

    [[nodiscard]] ConsoleCommandContext console_context();
    void request_quit();
};

void configure_engine_context(EngineContext& context, const RuntimeConfig& config);
}
