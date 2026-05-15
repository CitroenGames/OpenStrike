#include "openstrike/game/fps_controller.hpp"

#include <algorithm>
#include <cmath>

namespace openstrike
{
namespace
{
constexpr float kPi = 3.14159265358979323846F;

float degrees_to_radians(float degrees)
{
    return degrees * (kPi / 180.0F);
}
}

FpsInputSample sample_fps_input(InputState& input)
{
    return FpsInputSample{
        .move_forward = input.move_forward,
        .move_back = input.move_back,
        .move_left = input.move_left,
        .move_right = input.move_right,
        .jump = input.jump,
        .duck = input.duck,
        .walk = input.sprint,
        .mouse_delta = input.consume_mouse_delta(),
    };
}

void update_fps_view(FpsViewState& view, const FpsInputSample& input, const FpsControllerSettings& settings)
{
    const float yaw_delta = input.mouse_delta.x * settings.mouse_sensitivity * settings.mouse_yaw_scale;
    const float pitch_delta = input.mouse_delta.y * settings.mouse_sensitivity * settings.mouse_pitch_scale;
    view.yaw_degrees -= yaw_delta;
    view.pitch_degrees += pitch_delta;
    view.pitch_degrees = std::clamp(view.pitch_degrees, settings.min_pitch_degrees, settings.max_pitch_degrees);
}

InputCommand build_fps_move_command(const FpsViewState& view, const FpsInputSample& input)
{
    const float forward_axis = (input.move_forward ? 1.0F : 0.0F) - (input.move_back ? 1.0F : 0.0F);
    const float side_axis = (input.move_right ? 1.0F : 0.0F) - (input.move_left ? 1.0F : 0.0F);

    Vec3 wish_direction = (fps_forward_2d(view.yaw_degrees) * forward_axis) + (fps_right_2d(view.yaw_degrees) * side_axis);
    wish_direction = normalize_2d(wish_direction);

    return InputCommand{
        .move_x = wish_direction.x,
        .move_y = wish_direction.y,
        .jump = input.jump,
        .duck = input.duck,
        .walk = input.walk,
    };
}

Vec3 fps_forward_2d(float yaw_degrees)
{
    const float yaw_radians = degrees_to_radians(yaw_degrees);
    return normalize_2d({std::cos(yaw_radians), std::sin(yaw_radians), 0.0F});
}

Vec3 fps_right_2d(float yaw_degrees)
{
    const float yaw_radians = degrees_to_radians(yaw_degrees);
    return normalize_2d({std::sin(yaw_radians), -std::cos(yaw_radians), 0.0F});
}

Vec3 fps_eye_origin(const PlayerState& player, const FpsControllerSettings& settings)
{
    const float duck_fraction = std::clamp(player.duck_amount, 0.0F, 1.0F);
    const float eye_height = settings.eye_height + ((settings.duck_eye_height - settings.eye_height) * duck_fraction);
    return player.origin + Vec3{0.0F, 0.0F, eye_height};
}
}
