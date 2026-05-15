#include "openstrike/audio/audio_module.hpp"

#include "openstrike/audio/audio_system.hpp"
#include "openstrike/core/log.hpp"
#include "openstrike/engine/engine_context.hpp"

#include <algorithm>
#include <string>

namespace openstrike
{
const char* AudioModule::name() const
{
    return "audio";
}

void AudioModule::on_start(const RuntimeConfig&, EngineContext& engine)
{
    engine.audio.initialize(engine.variables);
    sync_menu_music(engine);
}

void AudioModule::on_frame(const FrameContext&, EngineContext& engine)
{
    sync_menu_music(engine);
    engine.audio.update(engine.camera, engine.world.current_world(), engine.world.generation(), engine.variables);
}

void AudioModule::on_stop(EngineContext& engine)
{
    menu_music_guid_ = 0;
    engine.audio.shutdown();
}

void AudioModule::sync_menu_music(EngineContext& engine)
{
    const bool should_play =
        engine.audio.initialized() && engine.world.current_world() == nullptr && engine.variables.get_bool("snd_menu_music", true);
    if (!should_play)
    {
        if (menu_music_guid_ != 0)
        {
            engine.audio.stop_sound_by_guid(menu_music_guid_);
            menu_music_guid_ = 0;
        }
        return;
    }

    if (menu_music_guid_ != 0 && engine.audio.is_sound_still_playing(menu_music_guid_))
    {
        return;
    }

    menu_music_guid_ = 0;
    const std::string entry = engine.variables.get_string("snd_menu_music_entry", "Musix.HalfTime.valve_csgo_01");
    if (entry.empty())
    {
        return;
    }

    AudioStartParams params;
    params.sample = entry;
    params.channel = ChanStatic;
    params.sound_level = 0;
    params.static_sound = true;
    params.update_positions = false;
    params.loop = true;
    params.volume = std::clamp(static_cast<float>(engine.variables.get_double("snd_musicvolume", 0.45)), 0.0F, 1.0F);

    const int guid = engine.audio.emit_sound(params, engine.filesystem);
    if (guid != 0)
    {
        menu_music_guid_ = guid;
        log_info("started menu music '{}'", entry);
    }
}
}
