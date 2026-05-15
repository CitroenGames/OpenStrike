#pragma once

#include "openstrike/engine/module.hpp"

namespace openstrike
{
class ClientModule final : public EngineModule
{
public:
    const char* name() const override;
    void on_start(const RuntimeConfig& config, EngineContext& engine) override;
    void on_frame(const FrameContext& context, EngineContext& engine) override;
};
}
