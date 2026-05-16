#include "openstrike/app/openstrike_application.hpp"

#include "openstrike/audio/audio_module.hpp"
#include "openstrike/client/client_module.hpp"
#include "openstrike/editor/editor_module.hpp"
#include "openstrike/game/game_simulation.hpp"
#include "openstrike/server/server_module.hpp"

#include <memory>
#include <utility>

namespace openstrike
{
ApplicationDefinition make_openstrike_application_definition()
{
    ApplicationDefinition definition;
    definition.name = "OpenStrike";
    definition.modules = {
        ApplicationModuleRegistration{
            .modes = ApplicationModuleMode::All,
            .create = []() -> std::unique_ptr<EngineModule> { return std::make_unique<GameSimulation>(); },
        },
        ApplicationModuleRegistration{
            .modes = ApplicationModuleMode::DedicatedServer,
            .create = []() -> std::unique_ptr<EngineModule> { return std::make_unique<ServerModule>(); },
        },
        ApplicationModuleRegistration{
            .modes = ApplicationModuleMode::ClientAndEditor,
            .create = []() -> std::unique_ptr<EngineModule> { return std::make_unique<AudioModule>(); },
        },
        ApplicationModuleRegistration{
            .modes = ApplicationModuleMode::ClientAndEditor,
            .create = []() -> std::unique_ptr<EngineModule> { return std::make_unique<ClientModule>(); },
        },
        ApplicationModuleRegistration{
            .modes = ApplicationModuleMode::Editor,
            .create = []() -> std::unique_ptr<EngineModule> { return std::make_unique<EditorModule>(); },
        },
    };
    return definition;
}

Application::Application(RuntimeConfig config)
    : Application(std::move(config), make_openstrike_application_definition())
{
}

int run_application(const RuntimeConfig& config)
{
    return run_openstrike_application(config);
}

int run_openstrike_application(const RuntimeConfig& config)
{
    Application application(config, make_openstrike_application_definition());
    return application.run();
}
}
