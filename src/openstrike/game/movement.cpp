#include "openstrike/game/movement.hpp"

#include <algorithm>
#include <cmath>

namespace openstrike
{
namespace
{
float movement_dt(const MovementTuning& tuning)
{
    return std::max(0.0F, tuning.tick_interval_seconds);
}

float clamp01(float value)
{
    return std::clamp(value, 0.0F, 1.0F);
}

float length(Vec3 value)
{
    return std::sqrt((value.x * value.x) + (value.y * value.y) + (value.z * value.z));
}

float approach(float target, float value, float delta)
{
    if (value < target)
    {
        return std::min(value + delta, target);
    }
    return std::max(value - delta, target);
}

float lerp(float a, float b, float t)
{
    return a + ((b - a) * t);
}

void recover_movement_modifiers(PlayerState& player, const MovementTuning& tuning)
{
    const float dt = movement_dt(tuning);
    player.stamina = std::max(0.0F, player.stamina - (tuning.stamina_recovery_rate * dt));
    if (player.on_ground)
    {
        player.velocity_modifier = std::min(1.0F, player.velocity_modifier + (tuning.velocity_modifier_recovery_rate * dt));
    }
}

void update_duck_state(PlayerState& player, const InputCommand& input, const MovementTuning& tuning)
{
    const float dt = movement_dt(tuning);
    if (player.duck_speed <= 0.0F)
    {
        player.duck_speed = tuning.ideal_duck_speed;
    }

    player.duck_speed = approach(tuning.ideal_duck_speed, player.duck_speed, dt * 3.0F);
    const float target_duck = input.duck ? 1.0F : 0.0F;
    const bool wants_transition = (target_duck > player.duck_amount && player.duck_amount < 1.0F) ||
        (target_duck < player.duck_amount && player.duck_amount > 0.0F);

    player.ducking = wants_transition;
    if (!wants_transition)
    {
        player.duck_amount = target_duck;
    }
    else if (!player.on_ground)
    {
        player.duck_amount = target_duck;
    }
    else
    {
        const float duck_rate = std::max(1.5F, player.duck_speed) * (input.duck ? 0.8F : 1.0F);
        player.duck_amount = approach(target_duck, player.duck_amount, dt * duck_rate);
    }

    player.duck_amount = clamp01(player.duck_amount);
    player.ducked = player.duck_amount >= 1.0F;
    player.ducking = player.duck_amount > 0.0F && player.duck_amount < 1.0F;
}

float stamina_scale(const PlayerState& player, const MovementTuning& tuning)
{
    if (tuning.stamina_range <= 0.0F)
    {
        return 1.0F;
    }

    return clamp01(1.0F - (player.stamina / tuning.stamina_range));
}

float effective_max_speed(const PlayerState& player, const InputCommand& input, const MovementTuning& tuning)
{
    float max_speed = std::max(0.0F, tuning.max_ground_speed);
    max_speed *= std::max(0.0F, player.surface_speed_factor);

    if (player.on_ground)
    {
        max_speed *= clamp01(player.velocity_modifier);
    }

    const float stamina = stamina_scale(player, tuning);
    max_speed *= stamina * stamina;

    const bool ducking = input.duck || player.ducking || player.ducked || player.duck_amount > 0.0F;
    if (input.walk && !ducking)
    {
        const float current_speed = player.velocity.length_2d();
        if (current_speed < (max_speed * tuning.walk_speed_modifier) + 25.0F)
        {
            max_speed *= tuning.walk_speed_modifier;
        }
    }

    if (ducking)
    {
        max_speed *= lerp(1.0F, tuning.duck_speed_modifier, clamp01(player.duck_amount));
    }

    return max_speed;
}

void clamp_horizontal_speed(PlayerState& player, float max_speed)
{
    const float speed = player.velocity.length_2d();
    if (max_speed <= 0.0F || speed <= max_speed)
    {
        return;
    }

    const float scale = max_speed / speed;
    player.velocity.x *= scale;
    player.velocity.y *= scale;
}

void prevent_bunny_jumping(PlayerState& player, const MovementTuning& tuning)
{
    const float max_scaled_speed = tuning.bunnyhop_max_speed_factor * tuning.max_ground_speed;
    if (max_scaled_speed <= 0.0F)
    {
        return;
    }

    const float speed = length(player.velocity);
    if (speed <= max_scaled_speed)
    {
        return;
    }

    player.velocity = player.velocity * (max_scaled_speed / speed);
}

void add_stamina(PlayerState& player, float amount, const MovementTuning& tuning)
{
    player.stamina = std::clamp(player.stamina + amount, 0.0F, tuning.stamina_max);
}

void apply_landing(PlayerState& player, const MovementTuning& tuning)
{
    if (player.fall_velocity > 0.0F)
    {
        add_stamina(player, tuning.stamina_land_cost * player.fall_velocity, tuning);
    }
    player.fall_velocity = 0.0F;
}

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
    const float drop = control * tuning.friction * player.surface_friction * movement_dt(tuning);
    const float new_speed = std::max(0.0F, speed - drop);
    const float scale = new_speed / speed;
    player.velocity.x *= scale;
    player.velocity.y *= scale;
}

void accelerate(PlayerState& player, Vec3 wish_direction, float wish_speed, float accel, const MovementTuning& tuning)
{
    const float current_speed = dot_2d(player.velocity, wish_direction);
    const float add_speed = wish_speed - current_speed;
    if (add_speed <= 0.0F)
    {
        return;
    }

    const float acceleration_scale = std::max(250.0F, wish_speed);
    const float accel_speed = std::min(accel * movement_dt(tuning) * acceleration_scale * player.surface_friction, add_speed);
    player.velocity.x += accel_speed * wish_direction.x;
    player.velocity.y += accel_speed * wish_direction.y;
}

void air_accelerate(PlayerState& player, Vec3 wish_direction, float wish_speed, const MovementTuning& tuning)
{
    const float capped_wish_speed = std::min(wish_speed, tuning.air_speed_cap);
    const float current_speed = dot_2d(player.velocity, wish_direction);
    const float add_speed = capped_wish_speed - current_speed;
    if (add_speed <= 0.0F)
    {
        return;
    }

    const float accel_speed =
        std::min(tuning.air_acceleration * wish_speed * movement_dt(tuning) * player.surface_friction, add_speed);
    player.velocity.x += accel_speed * wish_direction.x;
    player.velocity.y += accel_speed * wish_direction.y;
}

void check_jump_button(PlayerState& player, const InputCommand& input, const MovementTuning& tuning)
{
    if (!input.jump)
    {
        player.jump_was_down = false;
        return;
    }

    if (!player.on_ground)
    {
        player.jump_was_down = true;
        return;
    }

    if (player.jump_was_down && !tuning.auto_bunny_hopping)
    {
        return;
    }

    if (!tuning.enable_bunny_hopping)
    {
        prevent_bunny_jumping(player, tuning);
    }

    if (player.duck_amount > 0.0F)
    {
        player.velocity.z = tuning.jump_impulse;
    }
    else
    {
        player.velocity.z += tuning.jump_impulse;
    }

    player.velocity.z *= stamina_scale(player, tuning);
    player.on_ground = false;
    player.jump_was_down = true;
    add_stamina(player, tuning.stamina_jump_cost * std::max(0.0F, player.velocity.z), tuning);
}
}

void simulate_player_move(PlayerState& player, const InputCommand& input, const MovementTuning& tuning)
{
    simulate_player_move(player, input, tuning, 0.0F);
}

void simulate_player_move(PlayerState& player, const InputCommand& input, const MovementTuning& tuning, std::optional<float> ground_z)
{
    const bool was_on_ground = player.on_ground;
    recover_movement_modifiers(player, tuning);
    update_duck_state(player, input, tuning);

    if (!player.on_ground)
    {
        player.velocity.z -= tuning.gravity * movement_dt(tuning) * 0.5F;
        if (player.velocity.z < 0.0F)
        {
            player.fall_velocity = std::max(player.fall_velocity, -player.velocity.z);
        }
    }

    check_jump_button(player, input, tuning);

    if (player.on_ground)
    {
        player.velocity.z = 0.0F;
        player.fall_velocity = 0.0F;
        apply_ground_friction(player, tuning);
    }

    Vec3 wish_direction{input.move_x, input.move_y, 0.0F};
    const float input_length = std::min(1.0F, wish_direction.length_2d());
    wish_direction = normalize_2d(wish_direction);
    const float wish_speed = effective_max_speed(player, input, tuning) * input_length;

    if (player.on_ground)
    {
        accelerate(player, wish_direction, wish_speed, tuning.acceleration, tuning);
        clamp_horizontal_speed(player, effective_max_speed(player, input, tuning));
    }
    else
    {
        air_accelerate(player, wish_direction, wish_speed, tuning);
    }

    if (!player.on_ground)
    {
        player.velocity.z -= tuning.gravity * movement_dt(tuning) * 0.5F;
        if (player.velocity.z < 0.0F)
        {
            player.fall_velocity = std::max(player.fall_velocity, -player.velocity.z);
        }
    }

    player.origin += player.velocity * movement_dt(tuning);

    if (ground_z && player.origin.z <= *ground_z)
    {
        player.origin.z = *ground_z;
        player.velocity.z = 0.0F;
        player.on_ground = true;
        if (!was_on_ground)
        {
            apply_landing(player, tuning);
        }
    }
    else if (!ground_z)
    {
        player.on_ground = false;
    }
}
}
