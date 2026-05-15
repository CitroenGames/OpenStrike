#pragma once

#include "openstrike/core/math.hpp"

namespace openstrike
{
struct InputState
{
    bool move_forward = false;
    bool move_back = false;
    bool move_left = false;
    bool move_right = false;
    bool jump = false;
    bool duck = false;
    bool sprint = false;
    bool mouse_captured = false;
    Vec2 mouse_delta;

    [[nodiscard]] Vec2 consume_mouse_delta()
    {
        const Vec2 result = mouse_delta;
        mouse_delta = {};
        return result;
    }

    void clear_gameplay_buttons()
    {
        move_forward = false;
        move_back = false;
        move_left = false;
        move_right = false;
        jump = false;
        duck = false;
        sprint = false;
    }
};

struct CameraState
{
    Vec3 origin;
    float pitch_degrees = 0.0F;
    float yaw_degrees = 0.0F;
    float fov_degrees = 75.0F;
    bool active = false;
};
}
