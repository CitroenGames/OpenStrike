#pragma once

#include "openstrike/engine/input.hpp"

#include <SDL3/SDL.h>

namespace openstrike
{
[[nodiscard]] bool apply_sdl_gameplay_key(InputState& input, SDL_Keycode key, bool pressed);
[[nodiscard]] bool handle_sdl_gameplay_input_event(InputState& input, const SDL_Event& event);
}
