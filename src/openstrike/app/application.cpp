#include "openstrike/app/application.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine.hpp"
#include "openstrike/engine/module.hpp"
#include "openstrike/renderer/renderer_factory.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace openstrike
{
namespace
{
[[nodiscard]] ApplicationModuleMode mode_flag(AppMode mode) noexcept
{
    switch (mode)
    {
    case AppMode::Client:
        return ApplicationModuleMode::Client;
    case AppMode::DedicatedServer:
        return ApplicationModuleMode::DedicatedServer;
    case AppMode::Editor:
        return ApplicationModuleMode::Editor;
    }

    return ApplicationModuleMode::Client;
}

void configure_application_logging(const RuntimeConfig& config)
{
    if (!config.quiet)
    {
        return;
    }

    Logger::instance().set_min_level(LogLevel::Warning);
}

std::unique_ptr<Engine> create_application_engine(const RuntimeConfig& config, const ApplicationDefinition& definition)
{
    auto renderer = create_renderer(config);
    auto engine = std::make_unique<Engine>(std::move(renderer));

    for (const ApplicationModuleRegistration& module : definition.modules)
    {
        if (!application_module_runs_in_mode(module.modes, config.mode))
        {
            continue;
        }

        if (!module.create)
        {
            throw std::invalid_argument("application module registration has no factory");
        }

        std::unique_ptr<EngineModule> instance = module.create();
        if (!instance)
        {
            throw std::invalid_argument("application module factory returned null");
        }

        engine->add_module(std::move(instance));
    }

    return engine;
}
}

bool application_module_runs_in_mode(ApplicationModuleMode modes, AppMode mode) noexcept
{
    const auto mask = static_cast<std::uint8_t>(modes);
    const auto flag = static_cast<std::uint8_t>(mode_flag(mode));
    return (mask & flag) != 0U;
}

Application::Application(RuntimeConfig config, ApplicationDefinition definition)
    : config_(std::move(config))
    , definition_(std::move(definition))
{
    if (!definition_.name.empty())
    {
        config_.application_name = definition_.name;
    }

    configure_application_logging(config_);
    engine_ = create_application_engine(config_, definition_);
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

int run_application(const RuntimeConfig& config, ApplicationDefinition definition)
{
    Application application(config, std::move(definition));
    return application.run();
}
}
