#pragma once

#include "openstrike/engine/runtime_config.hpp"
#include "openstrike/renderer/renderer.hpp"

#include <memory>

namespace openstrike
{
[[nodiscard]] std::unique_ptr<IRenderer> create_renderer(const RuntimeConfig& config);
}

