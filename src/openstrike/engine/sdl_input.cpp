#include "openstrike/engine/sdl_input.hpp"

namespace openstrike
{
bool apply_sdl_gameplay_key(InputState& input, SDL_Keycode key, bool pressed)
{
    switch (key)
    {
    case SDLK_W:
        input.move_forward = pressed;
        return true;
    case SDLK_S:
        input.move_back = pressed;
        return true;
    case SDLK_A:
        input.move_left = pressed;
        return true;
    case SDLK_D:
        input.move_right = pressed;
        return true;
    case SDLK_SPACE:
        input.jump = pressed;
        return true;
    case SDLK_LCTRL:
    case SDLK_RCTRL:
    case SDLK_C:
        input.duck = pressed;
        return true;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
        input.sprint = pressed;
        return true;
    default:
        return false;
    }
}

bool handle_sdl_gameplay_input_event(InputState& input, const SDL_Event& event)
{
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
    {
        return apply_sdl_gameplay_key(input, event.key.key, event.type == SDL_EVENT_KEY_DOWN);
    }

    if (input.mouse_captured && event.type == SDL_EVENT_MOUSE_MOTION)
    {
        input.mouse_delta.x += static_cast<float>(event.motion.xrel);
        input.mouse_delta.y += static_cast<float>(event.motion.yrel);
        return true;
    }

    return false;
}
}
