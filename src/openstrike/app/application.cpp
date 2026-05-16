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
namespace
{
void configure_application_logging(const RuntimeConfig& config)
{
    if (!config.quiet)
    {
        return;
    }

    Logger::instance().set_min_level(LogLevel::Warning);
}

std::unique_ptr<Engine> create_application_engine(const RuntimeConfig& config)
{
    auto renderer = create_renderer(config);
    auto engine = std::make_unique<Engine>(std::move(renderer));

    engine->add_module(std::make_unique<GameSimulation>());

    if (config.mode == AppMode::DedicatedServer)
    {
        engine->add_module(std::make_unique<ServerModule>());
    }
    else
    {
        engine->add_module(std::make_unique<AudioModule>());
        engine->add_module(std::make_unique<ClientModule>());
    }

    if (config.mode == AppMode::Editor)
    {
        engine->add_module(std::make_unique<EditorModule>());
    }

    return engine;
}
}

Application::Application(RuntimeConfig config)
    : config_(std::move(config))
{
    configure_application_logging(config_);
    engine_ = create_application_engine(config_);
}

Application::~Application() = default;

int Application::run()
{
    const EngineStats stats = engine_->run(config_);
    log_info("stopped after {} frames and {} ticks", stats.frame_count, stats.tick_count);
    return 0;
}

const RuntimeConfig& Application::config() const noexcept
{
    return config_;
}

int run_application(const RuntimeConfig& config)
{
    Application application(config);
    return application.run();
}
}
