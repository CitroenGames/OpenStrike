#include "openstrike/game/game_simulation.hpp"

#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"
#include "openstrike/world/world.hpp"

#include <optional>

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

void GameSimulation::on_fixed_update(const SimulationStep&, EngineContext& engine)
{
    sync_world(engine);
    if (const LoadedWorld* world = engine.world.current_world())
    {
        const std::optional<float> floor_z = find_floor_z(*world, local_player_.origin, 512.0F);
        simulate_player_move(local_player_, input_, movement_, floor_z);
        if (const std::optional<float> post_move_floor_z = find_floor_z(*world, local_player_.origin, 512.0F);
            post_move_floor_z && local_player_.velocity.z <= 0.0F && local_player_.origin.z <= *post_move_floor_z + 2.0F)
        {
            local_player_.origin.z = *post_move_floor_z;
            local_player_.velocity.z = 0.0F;
            local_player_.on_ground = true;
        }
        return;
    }

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

void GameSimulation::sync_world(EngineContext& engine)
{
    const std::uint64_t world_generation = engine.world.generation();
    if (world_generation == observed_world_generation_)
    {
        return;
    }

    observed_world_generation_ = world_generation;
    const LoadedWorld* world = engine.world.current_world();
    if (world == nullptr)
    {
        return;
    }

    local_player_ = {};
    local_player_.on_ground = true;

    if (!world->spawn_points.empty())
    {
        local_player_.origin = world->spawn_points.front().origin;
    }

    if (const std::optional<float> floor_z = find_floor_z(*world, local_player_.origin, 512.0F))
    {
        local_player_.origin.z = *floor_z;
        local_player_.on_ground = true;
    }

    log_info("game simulation entered world '{}' at player origin=({}, {}, {})",
        world->name,
        local_player_.origin.x,
        local_player_.origin.y,
        local_player_.origin.z);
}
}
