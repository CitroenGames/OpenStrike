#pragma once

#include "openstrike/renderer/renderer.hpp"

namespace openstrike
{
class NullRenderer final : public IRenderer
{
public:
    bool initialize(const RuntimeConfig& config) override;
    void render(const FrameContext& context) override;
    void shutdown() override;
};
}

