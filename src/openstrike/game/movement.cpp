#include "openstrike/game/movement.hpp"

#include <algorithm>

namespace openstrike
{
namespace
{
void apply_ground_friction(PlayerState& player, const MovementTuning& tuning)
{
    const float speed = player.velocity.length_2d();
    if (speed <= 0.0001F)
    {
        player.velocity.x = 0.0F;
        player.velocity.y = 0.0F;
        return;
    }

    const float control = std::max(speed, tuning.stop_speed);
    const float drop = control * tuning.friction * tuning.tick_interval_seconds;
    const float new_speed = std::max(0.0F, speed - drop);
    const float scale = new_speed / speed;
    player.velocity.x *= scale;
    player.velocity.y *= scale;
}

void accelerate(PlayerState& player, Vec3 wish_direction, float wish_speed, const MovementTuning& tuning)
{
    const float current_speed = dot_2d(player.velocity, wish_direction);
    const float add_speed = wish_speed - current_speed;
    if (add_speed <= 0.0F)
    {
        return;
    }

    const float accel_speed = std::min(tuning.acceleration * wish_speed * tuning.tick_interval_seconds, add_speed);
    player.velocity.x += accel_speed * wish_direction.x;
    player.velocity.y += accel_speed * wish_direction.y;
}
}

void simulate_player_move(PlayerState& player, const InputCommand& input, const MovementTuning& tuning)
{
    if (player.on_ground)
    {
        apply_ground_friction(player, tuning);
    }

    Vec3 wish_direction{input.move_x, input.move_y, 0.0F};
    const float input_length = std::min(1.0F, wish_direction.length_2d());
    wish_direction = normalize_2d(wish_direction);
    const float wish_speed = tuning.max_ground_speed * input_length;

    accelerate(player, wish_direction, wish_speed, tuning);

    if (input.jump && player.on_ground)
    {
        player.velocity.z = tuning.jump_impulse;
        player.on_ground = false;
    }

    if (!player.on_ground)
    {
        player.velocity.z -= tuning.gravity * tuning.tick_interval_seconds;
    }

    player.origin += player.velocity * tuning.tick_interval_seconds;

    if (player.origin.z <= 0.0F)
    {
        player.origin.z = 0.0F;
        player.velocity.z = 0.0F;
        player.on_ground = true;
    }
}
}

