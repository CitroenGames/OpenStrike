#include "openstrike/animation/csgo_player_anim_state.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace openstrike
{
namespace
{
constexpr float kPi = 3.14159265358979323846F;
constexpr float kRunTopSpeed = 250.0F;
constexpr float kWalkTopSpeed = 130.0F;
constexpr float kCrouchTopSpeed = 85.0F;

float angle_normalize(float degrees)
{
    degrees = std::fmod(degrees, 360.0F);
    if (degrees > 180.0F)
    {
        degrees -= 360.0F;
    }
    else if (degrees < -180.0F)
    {
        degrees += 360.0F;
    }
    return degrees;
}

float angle_diff(float destination, float source)
{
    return angle_normalize(destination - source);
}

float approach(float target, float value, float speed)
{
    const float delta = target - value;
    if (delta > speed)
    {
        return value + speed;
    }
    if (delta < -speed)
    {
        return value - speed;
    }
    return target;
}

float approach_angle(float target, float value, float speed)
{
    const float delta = angle_diff(target, value);
    if (delta > speed)
    {
        value += speed;
    }
    else if (delta < -speed)
    {
        value -= speed;
    }
    else
    {
        value = target;
    }
    return angle_normalize(value);
}

float velocity_yaw(Vec3 velocity)
{
    if (velocity.length_2d() <= 0.001F)
    {
        return 0.0F;
    }
    return std::atan2(velocity.y, velocity.x) * (180.0F / kPi);
}

float layer_cycle_after(float cycle, float rate, float delta_seconds, bool loops)
{
    cycle += rate * delta_seconds;
    if (loops)
    {
        cycle -= std::floor(cycle);
    }
    else
    {
        cycle = std::clamp(cycle, 0.0F, 1.0F);
    }
    return cycle;
}

std::size_t layer_index(CsgoAnimLayer layer)
{
    return static_cast<std::size_t>(layer);
}

void ensure_layers(AnimationPlaybackState& playback)
{
    const std::size_t required = layer_index(CsgoAnimLayer::Count);
    if (playback.overlays.size() < required)
    {
        playback.overlays.resize(required);
    }
}

void update_layer(
    AnimationPlaybackState& playback,
    const StudioModel& model,
    CsgoAnimLayer layer,
    std::string_view activity,
    float playback_rate,
    float weight,
    float delta_seconds,
    bool restart)
{
    ensure_layers(playback);
    AnimationLayer& anim_layer = playback.overlays[layer_index(layer)];
    const std::int32_t sequence = select_weighted_sequence(model, activity, anim_layer.sequence);
    if (sequence < 0 || weight <= 0.0F)
    {
        anim_layer.weight = 0.0F;
        anim_layer.active = false;
        return;
    }

    if (anim_layer.sequence != sequence || restart)
    {
        anim_layer.previous_cycle = anim_layer.cycle;
        anim_layer.cycle = 0.0F;
        anim_layer.sequence = sequence;
    }

    anim_layer.active = true;
    anim_layer.order = static_cast<std::int32_t>(layer_index(layer));
    anim_layer.playback_rate = playback_rate;
    anim_layer.weight_delta_rate = delta_seconds > 0.0F ? (weight - anim_layer.weight) / delta_seconds : 0.0F;
    anim_layer.weight = std::clamp(weight, 0.0F, 1.0F);
    anim_layer.previous_cycle = anim_layer.cycle;
    anim_layer.cycle = layer_cycle_after(anim_layer.cycle, playback_rate, delta_seconds, sequence_loops(model, sequence));
}

void set_pose(AnimationPlaybackState& playback, const StudioModel& model, std::string_view name, float source_value)
{
    const std::int32_t pose = lookup_pose_parameter(model, name);
    if (pose < 0)
    {
        return;
    }

    if (playback.pose_parameters.size() < model.pose_parameters.size())
    {
        playback.pose_parameters.resize(model.pose_parameters.size(), 0.0F);
    }
    playback.pose_parameters[static_cast<std::size_t>(pose)] =
        source_pose_parameter_to_normalized(model.pose_parameters[static_cast<std::size_t>(pose)], source_value);
}
}

void reset_csgo_player_anim_state(CsgoPlayerAnimState& state)
{
    state = {};
    state.first_run_since_init = true;
    state.on_ground = true;
    state.in_air_smooth_value = 1.0F;
    state.aim_yaw_min = -58.0F;
    state.aim_yaw_max = 58.0F;
    state.aim_pitch_min = -90.0F;
    state.aim_pitch_max = 90.0F;
}

void update_csgo_player_anim_state(
    CsgoPlayerAnimState& state,
    AnimationPlaybackState& playback,
    const StudioModel& model,
    const CsgoAnimInput& input)
{
    if (!input.alive || model.sequences.empty())
    {
        return;
    }

    if (playback.pose_parameters.size() < model.pose_parameters.size())
    {
        playback.pose_parameters.resize(model.pose_parameters.size(), 0.0F);
    }
    ensure_layers(playback);

    state.last_update_increment = std::max(0.0F, input.delta_seconds);
    state.last_update_frame = input.frame_index;
    state.eye_yaw = angle_normalize(input.eye_yaw);
    state.eye_pitch = angle_normalize(input.eye_pitch);
    state.velocity_length_xy = input.velocity.length_2d();
    state.velocity_length_z = input.velocity.z;
    state.speed_as_portion_of_run = std::clamp(state.velocity_length_xy / kRunTopSpeed, 0.0F, 1.0F);
    state.speed_as_portion_of_walk = std::clamp(state.velocity_length_xy / kWalkTopSpeed, 0.0F, 1.0F);
    state.speed_as_portion_of_crouch = std::clamp(state.velocity_length_xy / kCrouchTopSpeed, 0.0F, 1.0F);
    state.anim_duck_amount = std::clamp(approach(std::clamp(input.duck_amount, 0.0F, 1.0F),
                                             state.anim_duck_amount,
                                             state.last_update_increment * 6.0F),
        0.0F,
        1.0F);

    state.landed_on_ground_this_frame = input.on_ground && !state.on_ground;
    state.left_ground_this_frame = !input.on_ground && state.on_ground;
    state.on_ground = input.on_ground;
    if (state.velocity_length_xy > 0.5F)
    {
        state.duration_moving += state.last_update_increment;
        state.duration_still = 0.0F;
    }
    else
    {
        state.duration_still += state.last_update_increment;
        state.duration_moving = 0.0F;
    }

    if (!input.on_ground)
    {
        state.duration_in_air += state.last_update_increment;
    }
    else if (state.landed_on_ground_this_frame)
    {
        state.landing = true;
    }
    else
    {
        state.duration_in_air = 0.0F;
    }

    const float move_yaw_world = velocity_yaw(input.velocity);
    state.move_yaw_ideal = angle_normalize(move_yaw_world - state.foot_yaw);
    state.move_yaw = approach_angle(state.move_yaw_ideal, state.move_yaw, state.last_update_increment * 720.0F);
    state.walk_to_run_transition = approach(input.walking ? 0.0F : 1.0F, state.walk_to_run_transition, state.last_update_increment * 4.0F);

    state.foot_yaw_last = state.foot_yaw;
    const float eye_foot_delta = angle_diff(state.eye_yaw, state.foot_yaw);
    const float yaw_min = state.aim_yaw_min;
    const float yaw_max = state.aim_yaw_max;
    if (eye_foot_delta > yaw_max)
    {
        state.foot_yaw = state.eye_yaw - yaw_max;
    }
    else if (eye_foot_delta < yaw_min)
    {
        state.foot_yaw = state.eye_yaw - yaw_min;
    }

    if (state.velocity_length_xy > 0.5F || !input.on_ground)
    {
        state.foot_yaw = approach_angle(state.eye_yaw, state.foot_yaw, state.last_update_increment * (30.0F + (20.0F * state.walk_to_run_transition)));
    }
    else
    {
        state.foot_yaw = approach_angle(input.lower_body_yaw_target, state.foot_yaw, state.last_update_increment * 100.0F);
    }
    state.foot_yaw = angle_normalize(state.foot_yaw);

    state.move_weight = std::clamp(state.velocity_length_xy / kRunTopSpeed, 0.0F, 1.0F);
    state.move_weight_smoothed = approach(state.move_weight, state.move_weight_smoothed, state.last_update_increment * 5.0F);
    state.primary_cycle += state.last_update_increment * std::clamp(state.velocity_length_xy / 260.0F, 0.0F, 1.0F);
    state.primary_cycle -= std::floor(state.primary_cycle);

    set_sequence(playback, model, 0);
    playback.playback_rate = 0.0F;
    playback.cycle = 0.0F;
    playback.sequence_finished = false;

    set_pose(playback, model, "move_yaw", state.move_yaw);
    set_pose(playback, model, "body_yaw", angle_diff(state.eye_yaw, state.foot_yaw));
    set_pose(playback, model, "body_pitch", state.eye_pitch);
    set_pose(playback, model, "aim_pitch", state.eye_pitch);
    set_pose(playback, model, "aim_yaw", angle_diff(state.eye_yaw, state.foot_yaw));
    set_pose(playback, model, "lean_yaw", angle_diff(state.foot_yaw, move_yaw_world));

    update_layer(playback, model, CsgoAnimLayer::AimMatrix, "ACT_CSGO_IDLE", 0.0F, 1.0F, state.last_update_increment, state.first_run_since_init);
    if (state.velocity_length_xy > 0.5F && input.on_ground)
    {
        update_layer(playback,
            model,
            CsgoAnimLayer::MovementMove,
            input.walking ? "ACT_CSGO_WALK" : "ACT_CSGO_RUN",
            std::max(0.01F, state.velocity_length_xy / kRunTopSpeed),
            state.move_weight_smoothed,
            state.last_update_increment,
            false);
    }
    else
    {
        playback.overlays[layer_index(CsgoAnimLayer::MovementMove)].weight = 0.0F;
        playback.overlays[layer_index(CsgoAnimLayer::MovementMove)].active = false;
    }

    if (!input.on_ground)
    {
        update_layer(playback,
            model,
            CsgoAnimLayer::MovementJumpOrFall,
            state.left_ground_this_frame ? "ACT_CSGO_JUMP" : "ACT_CSGO_FALL",
            1.0F,
            1.0F,
            state.last_update_increment,
            state.left_ground_this_frame);
    }
    else if (state.landed_on_ground_this_frame)
    {
        update_layer(playback,
            model,
            CsgoAnimLayer::MovementLandOrClimb,
            state.duration_in_air > 1.0F ? "ACT_CSGO_LAND_HEAVY" : "ACT_CSGO_LAND_LIGHT",
            1.0F,
            1.0F,
            state.last_update_increment,
            true);
        state.duration_in_air = 0.0F;
    }

    if (input.firing)
    {
        update_layer(playback,
            model,
            CsgoAnimLayer::WeaponAction,
            "ACT_CSGO_FIRE_PRIMARY",
            1.0F,
            1.0F,
            state.last_update_increment,
            true);
    }
    else if (input.reloading)
    {
        update_layer(playback, model, CsgoAnimLayer::WeaponAction, "ACT_CSGO_RELOAD", 1.0F, 1.0F, state.last_update_increment, false);
    }
    else if (input.deploying)
    {
        update_layer(playback, model, CsgoAnimLayer::WeaponAction, "ACT_CSGO_DEPLOY", 1.0F, 1.0F, state.last_update_increment, false);
    }

    update_layer(playback, model, CsgoAnimLayer::AliveLoop, "ACT_CSGO_ALIVE_LOOP", 1.0F, 1.0F, state.last_update_increment, state.first_run_since_init);
    update_layer(playback, model, CsgoAnimLayer::Flashed, "ACT_CSGO_FLASHBANG_REACTION", 1.0F, input.flashed ? 1.0F : 0.0F, state.last_update_increment, false);
    update_layer(playback, model, CsgoAnimLayer::Flinch, "ACT_CSGO_FLINCH", 1.0F, input.flinching ? 1.0F : 0.0F, state.last_update_increment, input.flinching);
    update_layer(playback, model, CsgoAnimLayer::Lean, "ACT_CSGO_LEAN", 0.0F, std::clamp(state.velocity_length_xy / kRunTopSpeed, 0.0F, 1.0F), state.last_update_increment, false);

    state.first_run_since_init = false;
}
}
