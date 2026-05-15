#pragma once

#include "openstrike/engine/module.hpp"

namespace openstrike
{
class AudioModule final : public EngineModule
{
public:
    const char* name() const override;
    void on_start(const RuntimeConfig& config, EngineContext& engine) override;
    void on_frame(const FrameContext& context, EngineContext& engine) override;
    void on_stop(EngineContext& engine) override;

private:
    void sync_menu_music(EngineContext& engine);

    int menu_music_guid_ = 0;
};
}
