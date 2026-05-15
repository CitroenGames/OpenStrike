#include "openstrike/game/game_simulation.hpp"

#include "openstrike/core/log.hpp"

namespace openstrike
{
const char* GameSimulation::name() const
{
    return "game";
}

void GameSimulation::on_start(const RuntimeConfig& config, EngineContext&)
{
    movement_.tick_interval_seconds = static_cast<float>(config.tick_interval_seconds());
    input_.move_y = 1.0F;
    local_player_.on_ground = true;
    log_info("game simulation started");
}

void GameSimulation::on_fixed_update(const SimulationStep&, EngineContext&)
{
    simulate_player_move(local_player_, input_, movement_);
}

void GameSimulation::on_frame(const FrameContext& context, EngineContext&)
{
    if (context.frame_index == 0)
    {
        log_info("player origin=({}, {}, {})", local_player_.origin.x, local_player_.origin.y, local_player_.origin.z);
    }
}

const PlayerState& GameSimulation::local_player() const
{
    return local_player_;
}
}
