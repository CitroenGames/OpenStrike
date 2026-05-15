#include "openstrike/app/application.hpp"

#include "openstrike/audio/audio_module.hpp"
#include "openstrike/client/client_module.hpp"
#include "openstrike/core/log.hpp"
#include "openstrike/editor/editor_module.hpp"
#include "openstrike/engine/engine.hpp"
#include "openstrike/game/game_simulation.hpp"
#include "openstrike/renderer/renderer_factory.hpp"
#include "openstrike/server/server_module.hpp"

#include <memory>
#include <utility>

namespace openstrike
{
int run_application(const RuntimeConfig& config)
{
    if (config.quiet)
    {
        Logger::instance().set_min_level(LogLevel::Warning);
    }

    auto renderer = create_renderer(config);
    Engine engine(std::move(renderer));

    engine.add_module(std::make_unique<GameSimulation>());

    if (config.mode == AppMode::DedicatedServer)
    {
        engine.add_module(std::make_unique<ServerModule>());
    }
    else
    {
        engine.add_module(std::make_unique<AudioModule>());
        engine.add_module(std::make_unique<ClientModule>());
    }

    if (config.mode == AppMode::Editor)
    {
        engine.add_module(std::make_unique<EditorModule>());
    }

    const EngineStats stats = engine.run(config);
    log_info("stopped after {} frames and {} ticks", stats.frame_count, stats.tick_count);
    return 0;
}
}
