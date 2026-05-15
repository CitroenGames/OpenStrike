#pragma once

#include "openstrike/core/math.hpp"

namespace openstrike
{
struct InputCommand
{
    float move_x = 0.0F;
    float move_y = 0.0F;
    bool jump = false;
};

struct PlayerState
{
    Vec3 origin;
    Vec3 velocity;
    bool on_ground = true;
};

struct MovementTuning
{
    float tick_interval_seconds = 1.0F / 64.0F;
    float max_ground_speed = 250.0F;
    float acceleration = 10.0F;
    float friction = 4.0F;
    float stop_speed = 100.0F;
    float jump_impulse = 265.0F;
    float gravity = 800.0F;
};

void simulate_player_move(PlayerState& player, const InputCommand& input, const MovementTuning& tuning);
}

