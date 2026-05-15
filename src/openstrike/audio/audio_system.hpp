#pragma once

#include "openstrike/core/math.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike
{
class CommandRegistry;
class ConsoleVariables;
class ContentFileSystem;
struct CameraState;
struct LoadedWorld;

enum AudioChannel : int
{
    ChanReplace = -1,
    ChanAuto = 0,
    ChanWeapon = 1,
    ChanVoice = 2,
    ChanItem = 3,
    ChanBody = 4,
    ChanStream = 5,
    ChanStatic = 6,
    ChanVoiceBase = 7,
    ChanUserBase = ChanVoiceBase + 128,
};

enum SoundFlags : int
{
    SoundNoFlags = 0,
    SoundChangeVolume = 1 << 0,
    SoundChangePitch = 1 << 1,
    SoundStop = 1 << 2,
    SoundSpawning = 1 << 3,
    SoundDelay = 1 << 4,
    SoundStopLooping = 1 << 5,
    SoundSpeaker = 1 << 6,
    SoundShouldPause = 1 << 7,
    SoundIgnorePhonemes = 1 << 8,
    SoundIgnoreName = 1 << 9,
    SoundIsScriptHandle = 1 << 10,
    SoundUpdateDelayForChoreo = 1 << 11,
    SoundGenerateGuid = 1 << 12,
    SoundOverridePitch = 1 << 13,
};

struct AudioStartParams
{
    int sound_source = 0;
    int channel = ChanAuto;
    std::string sample;
    Vec3 origin;
    Vec3 direction;
    float volume = 1.0F;
    int sound_level = 75;
    int flags = SoundNoFlags;
    int pitch = 100;
    float delay_seconds = 0.0F;
    int speaker_entity = -1;
    bool static_sound = false;
    bool update_positions = true;
    bool loop = false;
    bool from_server = false;
    bool hrtf_follow_entity = false;
    bool hrtf_bilinear = false;
    bool hrtf_lock = false;
    bool in_eye_sound = false;
};

struct ActiveSoundInfo
{
    int guid = 0;
    int sound_source = 0;
    int channel = ChanAuto;
    std::string sample;
    Vec3 origin;
    float left_volume = 0.0F;
    float right_volume = 0.0F;
    float seconds_remaining = 0.0F;
    bool loop = false;
    bool hrtf = false;
    bool from_server = false;
};

class AudioSystem
{
public:
    AudioSystem();
    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    AudioSystem(AudioSystem&&) noexcept;
    AudioSystem& operator=(AudioSystem&&) noexcept;

    static void register_variables(ConsoleVariables& variables);
    static void register_commands(CommandRegistry& commands);

    bool initialize(ConsoleVariables& variables);
    void shutdown();
    void update(const CameraState& camera, const LoadedWorld* world, std::uint64_t world_generation, ConsoleVariables& variables);

    bool precache_sound(std::string_view sample, ContentFileSystem& filesystem);
    int emit_sound(const AudioStartParams& params, ContentFileSystem& filesystem);
    int emit_ambient_sound(std::string_view sample, float volume, int pitch, int flags, ContentFileSystem& filesystem);
    void stop_sound(int sound_source, int channel, std::string_view sample = {});
    void stop_sound_by_guid(int guid);
    void stop_all_sounds(bool clear_cached_sources = false);
    void set_volume_by_guid(int guid, float volume);

    [[nodiscard]] bool initialized() const;
    [[nodiscard]] int guid_for_last_sound_emitted() const;
    [[nodiscard]] bool is_sound_still_playing(int guid) const;
    [[nodiscard]] float elapsed_time_by_guid(int guid) const;
    [[nodiscard]] std::vector<ActiveSoundInfo> active_sounds() const;
    [[nodiscard]] Vec3 listener_origin() const;
    [[nodiscard]] Vec3 listener_forward() const;

    [[nodiscard]] static bool is_sound_char(char value);
    [[nodiscard]] static std::string_view skip_sound_chars(std::string_view sample);
    [[nodiscard]] static bool test_sound_char(std::string_view sample, char value);
    [[nodiscard]] static float sound_level_to_distance_multiplier(int sound_level, float reference_db = 60.0F, float reference_distance = 36.0F);
    [[nodiscard]] static float gain_from_sound_level(int sound_level, float distance, float gain = 1.0F, float reference_db = 60.0F, float reference_distance = 36.0F);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
