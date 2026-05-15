#pragma once

#include "openstrike/engine/module.hpp"
#include "openstrike/engine/runtime_config.hpp"

namespace openstrike
{
class EngineContext;

class IRenderer
{
public:
    virtual ~IRenderer() = default;

    virtual void set_engine_context(EngineContext* context) { (void)context; }
    [[nodiscard]] virtual bool initialize(const RuntimeConfig& config) = 0;
    virtual void render(const FrameContext& context) = 0;
    [[nodiscard]] virtual bool should_close() const { return false; }
    virtual void shutdown() = 0;
};
}
