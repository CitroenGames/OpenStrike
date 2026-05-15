#pragma once

#include "openstrike/engine/runtime_config.hpp"

#include <cstdint>

namespace openstrike
{
class EngineContext;

struct SimulationStep
{
    std::uint64_t tick_index = 0;
    double delta_seconds = 0.0;
};

struct FrameContext
{
    std::uint64_t frame_index = 0;
    std::uint64_t tick_index = 0;
    double interpolation_alpha = 0.0;
};

class EngineModule
{
public:
    virtual ~EngineModule() = default;

    [[nodiscard]] virtual const char* name() const = 0;
    virtual void on_start(const RuntimeConfig&, EngineContext&) {}
    virtual void on_fixed_update(const SimulationStep&, EngineContext&) {}
    virtual void on_frame(const FrameContext&, EngineContext&) {}
    virtual void on_stop(EngineContext&) {}
};
}
