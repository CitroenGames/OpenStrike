#pragma once

#include "openstrike/core/math.hpp"

#include <optional>

namespace openstrike
{
struct InputCommand
{
    float move_x = 0.0F;
    float move_y = 0.0F;
    bool jump = false;
    bool duck = false;
    bool walk = false;
};

struct PlayerState
{
    Vec3 origin;
    Vec3 velocity;
    bool on_ground = true;
    bool jump_was_down = false;
    bool ducked = false;
    bool ducking = false;
    float duck_amount = 0.0F;
    float duck_speed = 8.0F;
    float stamina = 0.0F;
    float velocity_modifier = 1.0F;
    float surface_friction = 1.0F;
    float surface_speed_factor = 1.0F;
    float fall_velocity = 0.0F;
};

struct MovementTuning
{
    float tick_interval_seconds = 1.0F / 64.0F;
    float max_ground_speed = 250.0F;
    float acceleration = 5.5F;
    float air_acceleration = 12.0F;
    float air_speed_cap = 30.0F;
    float friction = 5.2F;
    float stop_speed = 80.0F;
    float jump_impulse = 301.993377F;
    float gravity = 800.0F;
    float walk_speed_modifier = 0.52F;
    float duck_speed_modifier = 0.34F;
    float ideal_duck_speed = 8.0F;
    float stamina_jump_cost = 0.080F;
    float stamina_land_cost = 0.050F;
    float stamina_recovery_rate = 60.0F;
    float stamina_max = 80.0F;
    float stamina_range = 100.0F;
    float velocity_modifier_recovery_rate = 1.0F / 2.5F;
    float bunnyhop_max_speed_factor = 1.1F;
    bool enable_bunny_hopping = false;
    bool auto_bunny_hopping = false;
};

void simulate_player_move(PlayerState& player, const InputCommand& input, const MovementTuning& tuning);
void simulate_player_move(PlayerState& player, const InputCommand& input, const MovementTuning& tuning, std::optional<float> ground_z);
}
