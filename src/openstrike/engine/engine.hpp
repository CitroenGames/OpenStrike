#pragma once

#include "openstrike/engine/module.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/engine/runtime_config.hpp"
#include "openstrike/renderer/renderer.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace openstrike
{
struct EngineStats
{
    std::uint64_t frame_count = 0;
    std::uint64_t tick_count = 0;
    double simulated_seconds = 0.0;
};

class Engine
{
public:
    explicit Engine(std::unique_ptr<IRenderer> renderer);

    void add_module(std::unique_ptr<EngineModule> module);
    [[nodiscard]] EngineStats run(const RuntimeConfig& config);

private:
    std::unique_ptr<IRenderer> renderer_;
    std::vector<std::unique_ptr<EngineModule>> modules_;
    EngineContext context_;
};
}
