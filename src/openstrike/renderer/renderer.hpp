#pragma once

#include "openstrike/engine/input.hpp"
#include "openstrike/engine/module.hpp"
#include "openstrike/engine/runtime_config.hpp"

#include <cstdint>

namespace openstrike
{
class ContentFileSystem;
class AnimationScene;
class EngineContext;
struct LoadedWorld;

struct RenderScene
{
    const LoadedWorld* world = nullptr;
    std::uint64_t world_generation = 0;
    const ContentFileSystem* filesystem = nullptr;
    const AnimationScene* animation = nullptr;
};

struct RenderFrame
{
    FrameContext timing;
    RenderScene scene;
    CameraState camera;
};

class IRenderer
{
public:
    virtual ~IRenderer() = default;

    virtual void set_engine_context(EngineContext* context) { (void)context; }
    [[nodiscard]] virtual bool initialize(const RuntimeConfig& config) = 0;
    virtual void render(const RenderFrame& frame) = 0;
    [[nodiscard]] virtual bool should_close() const { return false; }
    virtual void shutdown() = 0;
};
}
