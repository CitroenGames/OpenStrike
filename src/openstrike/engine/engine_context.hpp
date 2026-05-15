#pragma once

#include "openstrike/audio/audio_system.hpp"
#include "openstrike/core/console.hpp"
#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/engine/hud_state.hpp"
#include "openstrike/engine/input.hpp"
#include "openstrike/engine/runtime_config.hpp"
#include "openstrike/network/network_system.hpp"
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
    NetworkSystem network;
    AudioSystem audio;
    InputState input;
    HudState hud;
    CameraState camera;
    bool quit_requested = false;

    [[nodiscard]] ConsoleCommandContext console_context();
    void request_quit();
};

void configure_engine_context(EngineContext& context, const RuntimeConfig& config);
}
