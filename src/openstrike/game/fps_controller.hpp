#pragma once

#include "openstrike/engine/input.hpp"
#include "openstrike/game/movement.hpp"

namespace openstrike
{
struct FpsControllerSettings
{
    float mouse_sensitivity = 2.5F;
    float mouse_yaw_scale = 0.022F;
    float mouse_pitch_scale = 0.022F;
    float min_pitch_degrees = -89.0F;
    float max_pitch_degrees = 89.0F;
    float eye_height = 64.0F;
};

struct FpsViewState
{
    float yaw_degrees = 0.0F;
    float pitch_degrees = 0.0F;
};

struct FpsInputSample
{
    bool move_forward = false;
    bool move_back = false;
    bool move_left = false;
    bool move_right = false;
    bool jump = false;
    Vec2 mouse_delta;
};

[[nodiscard]] FpsInputSample sample_fps_input(InputState& input);
void update_fps_view(FpsViewState& view, const FpsInputSample& input, const FpsControllerSettings& settings);
[[nodiscard]] InputCommand build_fps_move_command(const FpsViewState& view, const FpsInputSample& input);
[[nodiscard]] Vec3 fps_forward_2d(float yaw_degrees);
[[nodiscard]] Vec3 fps_right_2d(float yaw_degrees);
[[nodiscard]] Vec3 fps_eye_origin(const PlayerState& player, const FpsControllerSettings& settings);
}
