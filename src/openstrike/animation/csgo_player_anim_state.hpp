#pragma once

#include "openstrike/animation/source_studio.hpp"

#include <array>
#include <cstdint>

namespace openstrike
{
enum class CsgoAnimLayer : std::uint8_t
{
    AimMatrix = 0,
    WeaponAction,
    WeaponActionRecrouch,
    Adjust,
    MovementJumpOrFall,
    MovementLandOrClimb,
    MovementMove,
    MovementStrafeChange,
    WholeBody,
    Flashed,
    Flinch,
    AliveLoop,
    Lean,
    Count
};

struct CsgoAnimInput
{
    float delta_seconds = 0.0F;
    std::uint64_t frame_index = 0;
    Vec3 origin;
    Vec3 velocity;
    float eye_yaw = 0.0F;
    float eye_pitch = 0.0F;
    float lower_body_yaw_target = 0.0F;
    float duck_amount = 0.0F;
    bool alive = true;
    bool on_ground = true;
    bool on_ladder = false;
    bool walking = false;
    bool firing = false;
    bool reloading = false;
    bool deploying = false;
    bool flashed = false;
    bool flinching = false;
};

struct CsgoPlayerAnimState
{
    bool first_run_since_init = true;
    float last_update_time = 0.0F;
    std::uint64_t last_update_frame = 0;
    float last_update_increment = 0.0F;
    float eye_yaw = 0.0F;
    float eye_pitch = 0.0F;
    float foot_yaw = 0.0F;
    float foot_yaw_last = 0.0F;
    float move_yaw = 0.0F;
    float move_yaw_ideal = 0.0F;
    float primary_cycle = 0.0F;
    float move_weight = 0.0F;
    float move_weight_smoothed = 0.0F;
    float anim_duck_amount = 0.0F;
    float velocity_length_xy = 0.0F;
    float velocity_length_z = 0.0F;
    float speed_as_portion_of_run = 0.0F;
    float speed_as_portion_of_walk = 0.0F;
    float speed_as_portion_of_crouch = 0.0F;
    float duration_moving = 0.0F;
    float duration_still = 0.0F;
    float duration_in_air = 0.0F;
    bool on_ground = true;
    bool landed_on_ground_this_frame = false;
    bool left_ground_this_frame = false;
    bool landing = false;
    float walk_to_run_transition = 0.0F;
    float in_air_smooth_value = 1.0F;
    float aim_yaw_min = -58.0F;
    float aim_yaw_max = 58.0F;
    float aim_pitch_min = -90.0F;
    float aim_pitch_max = 90.0F;
};

void reset_csgo_player_anim_state(CsgoPlayerAnimState& state);
void update_csgo_player_anim_state(
    CsgoPlayerAnimState& state,
    AnimationPlaybackState& playback,
    const StudioModel& model,
    const CsgoAnimInput& input);
}
