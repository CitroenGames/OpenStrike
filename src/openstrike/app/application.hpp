#pragma once

#include "openstrike/engine/runtime_config.hpp"

#include <memory>

namespace openstrike
{
class Engine;

class Application final
{
public:
    explicit Application(RuntimeConfig config);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] int run();
    [[nodiscard]] const RuntimeConfig& config() const noexcept;

private:
    RuntimeConfig config_;
    std::unique_ptr<Engine> engine_;
};

int run_application(const RuntimeConfig& config);
}
