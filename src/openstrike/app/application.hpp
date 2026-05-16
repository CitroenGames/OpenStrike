#pragma once

#include "openstrike/engine/runtime_config.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace openstrike
{
class Engine;
class EngineModule;

enum class ApplicationModuleMode : std::uint8_t
{
    Client = 1U << 0U,
    DedicatedServer = 1U << 1U,
    Editor = 1U << 2U,
    ClientAndEditor = (1U << 0U) | (1U << 2U),
    All = (1U << 0U) | (1U << 1U) | (1U << 2U),
};

[[nodiscard]] constexpr ApplicationModuleMode operator|(ApplicationModuleMode lhs, ApplicationModuleMode rhs) noexcept
{
    return static_cast<ApplicationModuleMode>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] bool application_module_runs_in_mode(ApplicationModuleMode modes, AppMode mode) noexcept;

struct ApplicationModuleRegistration
{
    ApplicationModuleMode modes = ApplicationModuleMode::All;
    std::function<std::unique_ptr<EngineModule>()> create;
};

struct ApplicationDefinition
{
    std::string name;
    std::vector<ApplicationModuleRegistration> modules;
};

class Application final
{
public:
    explicit Application(RuntimeConfig config);
    Application(RuntimeConfig config, ApplicationDefinition definition);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] int run();
    [[nodiscard]] const RuntimeConfig& config() const noexcept;

private:
    RuntimeConfig config_;
    ApplicationDefinition definition_;
    std::unique_ptr<Engine> engine_;
};

int run_application(const RuntimeConfig& config);
int run_application(const RuntimeConfig& config, ApplicationDefinition definition);
}
