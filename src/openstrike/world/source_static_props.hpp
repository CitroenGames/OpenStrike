#pragma once

#include "openstrike/world/world.hpp"

#include <span>
#include <vector>

namespace openstrike
{
[[nodiscard]] std::vector<WorldProp> read_source_static_props(std::span<const unsigned char> bsp_bytes);
}
