#pragma once

#include "openstrike/engine/module.hpp"
#include "openstrike/game/movement.hpp"

#include <cstdint>

namespace openstrike
{
class GameSimulation final : public EngineModule
{
public:
    const char* name() const override;
    void on_start(const RuntimeConfig& config, EngineContext& engine) override;
    void on_fixed_update(const SimulationStep& step, EngineContext& engine) override;
    void on_frame(const FrameContext& context, EngineContext& engine) override;

    [[nodiscard]] const PlayerState& local_player() const;

private:
    void sync_world(EngineContext& engine);

    PlayerState local_player_;
    MovementTuning movement_;
    InputCommand input_;
    std::uint64_t observed_world_generation_ = 0;
};
}
