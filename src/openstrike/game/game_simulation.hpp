#pragma once

#include "openstrike/engine/module.hpp"
#include "openstrike/game/fps_controller.hpp"
#include "openstrike/game/movement.hpp"
#include "openstrike/physics/physics_world.hpp"

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
    void update_camera(EngineContext& engine) const;
    void update_hud(EngineContext& engine) const;

    PlayerState local_player_;
    MovementTuning movement_;
    FpsControllerSettings fps_settings_;
    FpsViewState fps_view_;
    PhysicsWorld physics_;
    InputCommand input_;
    std::uint64_t observed_world_generation_ = 0;
};
}
