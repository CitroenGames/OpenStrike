#pragma once

#include "openstrike/engine/module.hpp"

#include <cstdint>

namespace openstrike
{
class ServerModule final : public EngineModule
{
public:
    const char* name() const override;
    void on_start(const RuntimeConfig& config, EngineContext& engine) override;
    void on_fixed_update(const SimulationStep& step, EngineContext& engine) override;
    void on_frame(const FrameContext& context, EngineContext& engine) override;
    void on_stop(EngineContext& engine) override;

private:
    std::uint64_t simulated_ticks_ = 0;
};
}
