#pragma once

#include "openstrike/renderer/renderer.hpp"

#include <memory>

namespace openstrike
{
class EngineContext;

class MetalRenderer final : public IRenderer
{
public:
    MetalRenderer();
    ~MetalRenderer() override;

    void set_engine_context(EngineContext* context) override;
    bool initialize(const RuntimeConfig& config) override;
    void render(const RenderFrame& frame) override;
    bool should_close() const override;
    void shutdown() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
