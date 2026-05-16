#include "openstrike/audio/audio_system.hpp"

#include "openstrike/audio/source_sound_library.hpp"
#include "openstrike/core/console.hpp"
#include "openstrike/core/content_filesystem.hpp"
#include "openstrike/core/log.hpp"
#include "openstrike/engine/input.hpp"
#include "openstrike/source/source_asset_store.hpp"
#include "openstrike/world/world.hpp"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <miniaudio.h>
#include <phonon.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace openstrike
{
namespace
{
constexpr int kOutputSampleRate = 48000;
constexpr int kOutputChannels = 2;
constexpr int kMixChunkFrames = 512;
constexpr int kMaxChannels = 128;
constexpr int kMaxDynamicChannels = 32;
constexpr int kMaxSteamAudioSources = 64;
constexpr float kPi = 3.14159265358979323846F;
constexpr float kDefaultReferenceDb = 60.0F;
constexpr float kDefaultReferenceDistance = 36.0F;
constexpr float kGainCompressionThreshold = 0.5F;
constexpr float kGainCompressionExpMax = 2.5F;
constexpr float kGainCompressionExpMin = 0.8F;
constexpr float kGainDbMax = 140.0F;
constexpr float kGainDbMedium = 90.0F;
constexpr float kDefaultRadius = 32.0F;

constexpr char kSoundCharStream = '*';
constexpr char kSoundCharUserVox = '?';
constexpr char kSoundCharSentence = '!';
constexpr char kSoundCharDryMix = '#';
constexpr char kSoundCharDoppler = '>';
constexpr char kSoundCharDirectional = '<';
constexpr char kSoundCharDistanceVariant = '^';
constexpr char kSoundCharOmni = '@';
constexpr char kSoundCharHrtf = '~';
constexpr char kSoundCharRadio = '+';
constexpr char kSoundCharSpatialStereo = ')';
constexpr char kSoundCharDirectionalStereo = '(';
constexpr char kSoundCharFastPitch = '}';
constexpr char kSoundCharSubtitled = '$';

float degrees_to_radians(float degrees)
{
    return degrees * (kPi / 180.0F);
}

float length(Vec3 value)
{
    return value.length();
}

Vec3 camera_forward(const CameraState& camera)
{
    const float yaw = degrees_to_radians(camera.yaw_degrees);
    const float pitch = degrees_to_radians(camera.pitch_degrees);
    const float cos_pitch = std::cos(pitch);
    return normalize(Vec3{std::cos(yaw) * cos_pitch, std::sin(yaw) * cos_pitch, -std::sin(pitch)});
}

Vec3 camera_right(const CameraState& camera)
{
    const float yaw = degrees_to_radians(camera.yaw_degrees);
    return normalize(Vec3{std::sin(yaw), -std::cos(yaw), 0.0F});
}

std::string lower_copy(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

void normalize_slashes(std::string& path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
}

bool has_extension(std::string_view path)
{
    const std::filesystem::path fs_path(path);
    return fs_path.has_extension();
}

float clamp01(float value)
{
    return std::clamp(value, 0.0F, 1.0F);
}

std::string join_args(const std::vector<std::string>& args, std::size_t first = 0)
{
    std::string result;
    for (std::size_t index = first; index < args.size(); ++index)
    {
        if (!result.empty())
        {
            result += ' ';
        }
        result += args[index];
    }
    return result;
}

std::optional<float> parse_float(std::string_view text)
{
    std::string value(text);
    char* end = nullptr;
    const float parsed = std::strtof(value.c_str(), &end);
    if (end == value.c_str() || (end != nullptr && *end != '\0'))
    {
        return std::nullopt;
    }
    return parsed;
}

std::optional<Vec3> parse_vec3_args(const std::vector<std::string>& args, std::size_t first)
{
    if (args.size() < first + 3)
    {
        return std::nullopt;
    }

    const std::optional<float> x = parse_float(args[first]);
    const std::optional<float> y = parse_float(args[first + 1]);
    const std::optional<float> z = parse_float(args[first + 2]);
    if (!x || !y || !z)
    {
        return std::nullopt;
    }

    return Vec3{*x, *y, *z};
}

struct ResolvedAudioAsset
{
    std::string logical_name;
    std::string debug_name;
    std::vector<unsigned char> bytes;
};

std::optional<std::vector<unsigned char>> read_absolute_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::nullopt;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0)
    {
        return std::nullopt;
    }
    file.seekg(0, std::ios::beg);

    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty())
    {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!file)
        {
            return std::nullopt;
        }
    }
    return bytes;
}

std::vector<std::string> make_audio_candidates(std::string_view sample)
{
    std::string stripped(AudioSystem::skip_sound_chars(sample));
    normalize_slashes(stripped);
    if (stripped.empty())
    {
        return {};
    }

    std::vector<std::string> candidates;
    auto add_candidate = [&](std::string candidate) {
        normalize_slashes(candidate);
        const std::string key = lower_copy(candidate);
        const auto exists = std::any_of(candidates.begin(), candidates.end(), [&](const std::string& existing) {
            return lower_copy(existing) == key;
        });
        if (!exists)
        {
            candidates.push_back(std::move(candidate));
        }
    };

    add_candidate(stripped);
    if (stripped.rfind("sound/", 0) != 0)
    {
        add_candidate("sound/" + stripped);
    }

    if (!has_extension(stripped))
    {
        constexpr std::array<std::string_view, 3> extensions{".wav", ".mp3", ".flac"};
        for (std::string_view extension : extensions)
        {
            add_candidate(stripped + std::string(extension));
            if (stripped.rfind("sound/", 0) != 0)
            {
                add_candidate("sound/" + stripped + std::string(extension));
            }
        }
    }

    return candidates;
}

std::optional<ResolvedAudioAsset> resolve_audio_asset(std::string_view sample, const SourceAssetStore& assets)
{
    std::string stripped(AudioSystem::skip_sound_chars(sample));
    normalize_slashes(stripped);
    if (stripped.empty())
    {
        return std::nullopt;
    }

    const std::filesystem::path raw_path(stripped);
    if (raw_path.is_absolute())
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(raw_path, error))
        {
            if (std::optional<std::vector<unsigned char>> bytes = read_absolute_file(raw_path))
            {
                return ResolvedAudioAsset{stripped, raw_path.string(), std::move(*bytes)};
            }
        }
    }

    const std::vector<std::string> candidates = make_audio_candidates(sample);
    for (const std::string& candidate : candidates)
    {
        if (std::optional<std::vector<unsigned char>> bytes = assets.read_binary(candidate, "GAME"))
        {
            return ResolvedAudioAsset{stripped, "GAME:" + candidate, std::move(*bytes)};
        }
    }

    return std::nullopt;
}
}

struct AudioSystem::Impl
{
    struct AudioSource
    {
        std::string name;
        std::string debug_name;
        std::vector<float> samples;
        std::uint32_t frame_count = 0;
    };

    struct Listener
    {
        Vec3 origin;
        Vec3 forward{1.0F, 0.0F, 0.0F};
        Vec3 right{0.0F, -1.0F, 0.0F};
        Vec3 up{0.0F, 0.0F, 1.0F};
        bool active = false;
    };

    struct SteamScene
    {
        IPLScene scene = nullptr;
        IPLStaticMesh static_mesh = nullptr;
        IPLSimulator simulator = nullptr;
        std::array<IPLSource, kMaxSteamAudioSources> sources{};
        std::array<bool, kMaxSteamAudioSources> source_in_use{};
        std::array<bool, kMaxSteamAudioSources> source_added{};
        std::array<Vec3, kMaxSteamAudioSources> source_positions{};
        std::array<float, kMaxSteamAudioSources> occlusion{};
        std::array<std::array<float, 3>, kMaxSteamAudioSources> transmission{};
        bool ready = false;

        void reset_results()
        {
            occlusion.fill(1.0F);
            for (auto& bands : transmission)
            {
                bands = {1.0F, 1.0F, 1.0F};
            }
        }

        void shutdown(IPLContext context)
        {
            (void)context;
            for (int index = 0; index < kMaxSteamAudioSources; ++index)
            {
                if (sources[index] != nullptr)
                {
                    if (source_added[index] && simulator != nullptr)
                    {
                        iplSourceRemove(sources[index], simulator);
                    }
                    iplSourceRelease(&sources[index]);
                }
                source_in_use[index] = false;
                source_added[index] = false;
            }

            if (simulator != nullptr)
            {
                iplSimulatorRelease(&simulator);
            }
            if (static_mesh != nullptr)
            {
                if (scene != nullptr)
                {
                    iplStaticMeshRemove(static_mesh, scene);
                }
                iplStaticMeshRelease(&static_mesh);
            }
            if (scene != nullptr)
            {
                iplSceneRelease(&scene);
            }

            ready = false;
            reset_results();
        }

        bool build(IPLContext context, const LoadedWorld& world, ConsoleVariables& variables)
        {
            shutdown(context);
            if (context == nullptr || world.mesh.collision_triangles.empty())
            {
                return false;
            }

            std::vector<IPLVector3> vertices;
            std::vector<IPLTriangle> triangles;
            std::vector<IPLint32> material_indices;
            vertices.reserve(world.mesh.collision_triangles.size() * 3);
            triangles.reserve(world.mesh.collision_triangles.size());
            material_indices.reserve(world.mesh.collision_triangles.size());

            for (const WorldTriangle& triangle : world.mesh.collision_triangles)
            {
                const int base = static_cast<int>(vertices.size());
                for (const Vec3& point : triangle.points)
                {
                    vertices.push_back(IPLVector3{point.x, point.y, point.z});
                }
                IPLTriangle steam_triangle{};
                steam_triangle.indices[0] = base;
                steam_triangle.indices[1] = base + 1;
                steam_triangle.indices[2] = base + 2;
                triangles.push_back(steam_triangle);
                material_indices.push_back(0);
            }

            IPLSceneSettings scene_settings{};
            scene_settings.type = IPL_SCENETYPE_DEFAULT;
            if (iplSceneCreate(context, &scene_settings, &scene) != IPL_STATUS_SUCCESS)
            {
                log_warning("Steam Audio scene creation failed for world '{}'", world.name);
                return false;
            }

            IPLMaterial material{};
            material.absorption[0] = 0.10F;
            material.absorption[1] = 0.20F;
            material.absorption[2] = 0.30F;
            material.scattering = 0.05F;
            material.transmission[0] = 0.10F;
            material.transmission[1] = 0.05F;
            material.transmission[2] = 0.02F;

            IPLStaticMeshSettings mesh_settings{};
            mesh_settings.numVertices = static_cast<IPLint32>(vertices.size());
            mesh_settings.numTriangles = static_cast<IPLint32>(triangles.size());
            mesh_settings.numMaterials = 1;
            mesh_settings.vertices = vertices.data();
            mesh_settings.triangles = triangles.data();
            mesh_settings.materialIndices = material_indices.data();
            mesh_settings.materials = &material;
            if (iplStaticMeshCreate(scene, &mesh_settings, &static_mesh) != IPL_STATUS_SUCCESS)
            {
                log_warning("Steam Audio static mesh creation failed for world '{}'", world.name);
                shutdown(context);
                return false;
            }

            iplStaticMeshAdd(static_mesh, scene);
            iplSceneCommit(scene);

            IPLSimulationSettings simulation_settings{};
            simulation_settings.flags = IPL_SIMULATIONFLAGS_DIRECT;
            simulation_settings.sceneType = IPL_SCENETYPE_DEFAULT;
            simulation_settings.maxNumOcclusionSamples = 32;
            simulation_settings.maxNumRays = 0;
            simulation_settings.maxNumSources = kMaxSteamAudioSources;
            simulation_settings.samplingRate = kOutputSampleRate;
            simulation_settings.frameSize = kMixChunkFrames;
            if (iplSimulatorCreate(context, &simulation_settings, &simulator) != IPL_STATUS_SUCCESS)
            {
                log_warning("Steam Audio simulator creation failed for world '{}'", world.name);
                shutdown(context);
                return false;
            }

            iplSimulatorSetScene(simulator, scene);
            iplSimulatorCommit(simulator);

            IPLSourceSettings source_settings{};
            source_settings.flags = IPL_SIMULATIONFLAGS_DIRECT;
            for (int index = 0; index < kMaxSteamAudioSources; ++index)
            {
                if (iplSourceCreate(simulator, &source_settings, &sources[index]) != IPL_STATUS_SUCCESS)
                {
                    sources[index] = nullptr;
                }
            }

            ready = true;
            reset_results();

            if (variables.get_bool("snd_steamaudio_debug"))
            {
                log_info("Steam Audio scene loaded for '{}' with {} triangles", world.name, triangles.size());
            }
            return true;
        }

        int alloc_source()
        {
            for (int index = 0; index < kMaxSteamAudioSources; ++index)
            {
                if (!source_in_use[index] && sources[index] != nullptr)
                {
                    source_in_use[index] = true;
                    source_added[index] = false;
                    occlusion[index] = 1.0F;
                    transmission[index] = {1.0F, 1.0F, 1.0F};
                    return index;
                }
            }
            return -1;
        }

        void free_source(int slot)
        {
            if (slot < 0 || slot >= kMaxSteamAudioSources)
            {
                return;
            }

            if (source_added[slot] && sources[slot] != nullptr && simulator != nullptr)
            {
                iplSourceRemove(sources[slot], simulator);
            }
            source_in_use[slot] = false;
            source_added[slot] = false;
            occlusion[slot] = 1.0F;
            transmission[slot] = {1.0F, 1.0F, 1.0F};
        }

        void set_source_position(int slot, Vec3 position)
        {
            if (slot < 0 || slot >= kMaxSteamAudioSources)
            {
                return;
            }
            source_positions[slot] = position;
        }

        void update(const Listener& listener, ConsoleVariables& variables)
        {
            if (!ready || simulator == nullptr || !variables.get_bool("snd_steamaudio_occlusion", true))
            {
                return;
            }

            IPLSimulationSharedInputs shared_inputs{};
            shared_inputs.listener.origin = IPLVector3{listener.origin.x, listener.origin.y, listener.origin.z};
            shared_inputs.listener.ahead = IPLVector3{listener.forward.x, listener.forward.y, listener.forward.z};
            shared_inputs.listener.right = IPLVector3{listener.right.x, listener.right.y, listener.right.z};
            shared_inputs.listener.up = IPLVector3{listener.up.x, listener.up.y, listener.up.z};
            iplSimulatorSetSharedInputs(simulator, IPL_SIMULATIONFLAGS_DIRECT, &shared_inputs);

            const float radius = std::max(0.0F, static_cast<float>(variables.get_double("snd_steamaudio_occlusion_radius", kDefaultRadius)));
            const int samples = std::clamp(variables.get_int("snd_steamaudio_occlusion_samples", 16), 1, 32);
            const bool transmission_enabled = variables.get_bool("snd_steamaudio_transmission", true);
            bool any_active = false;

            for (int index = 0; index < kMaxSteamAudioSources; ++index)
            {
                if (!source_in_use[index] || sources[index] == nullptr)
                {
                    continue;
                }

                if (!source_added[index])
                {
                    iplSourceAdd(sources[index], simulator);
                    source_added[index] = true;
                }

                IPLSimulationInputs inputs{};
                inputs.flags = IPL_SIMULATIONFLAGS_DIRECT;
                int direct_flags = IPL_DIRECTSIMULATIONFLAGS_OCCLUSION;
                if (transmission_enabled)
                {
                    direct_flags |= IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION;
                }
                inputs.directFlags = static_cast<IPLDirectSimulationFlags>(direct_flags);
                inputs.source.origin = IPLVector3{source_positions[index].x, source_positions[index].y, source_positions[index].z};
                inputs.source.ahead = IPLVector3{1.0F, 0.0F, 0.0F};
                inputs.source.right = IPLVector3{0.0F, -1.0F, 0.0F};
                inputs.source.up = IPLVector3{0.0F, 0.0F, 1.0F};
                inputs.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
                inputs.occlusionRadius = radius;
                inputs.numOcclusionSamples = samples;
                inputs.numTransmissionRays = 4;
                iplSourceSetInputs(sources[index], IPL_SIMULATIONFLAGS_DIRECT, &inputs);
                any_active = true;
            }

            if (!any_active)
            {
                return;
            }

            iplSimulatorCommit(simulator);
            iplSimulatorRunDirect(simulator);

            for (int index = 0; index < kMaxSteamAudioSources; ++index)
            {
                if (!source_in_use[index] || sources[index] == nullptr || !source_added[index])
                {
                    continue;
                }

                IPLSimulationOutputs outputs{};
                iplSourceGetOutputs(sources[index], IPL_SIMULATIONFLAGS_DIRECT, &outputs);
                occlusion[index] = clamp01(outputs.direct.occlusion);
                transmission[index] = {
                    clamp01(outputs.direct.transmission[0]),
                    clamp01(outputs.direct.transmission[1]),
                    clamp01(outputs.direct.transmission[2]),
                };
            }
        }
    };

    struct Channel
    {
        bool active = false;
        int guid = 0;
        int sound_source = 0;
        int channel = ChanAuto;
        std::shared_ptr<AudioSource> source;
        std::string sample;
        Vec3 origin;
        Vec3 direction;
        float volume = 1.0F;
        int sound_level = 75;
        int flags = SoundNoFlags;
        float pitch_scale = 1.0F;
        double position = 0.0;
        double start_time = 0.0;
        double emitted_time = 0.0;
        int speaker_entity = -1;
        bool static_sound = false;
        bool update_positions = true;
        bool loop = false;
        bool from_server = false;
        bool dry = false;
        bool radio = false;
        bool hrtf = false;
        bool hrtf_bilinear = false;
        bool hrtf_lock = false;
        bool in_eye_sound = false;
        float last_left = 0.0F;
        float last_right = 0.0F;
        int steam_source_slot = -1;
        IPLDirectEffect direct_effect = nullptr;
        IPLBinauralEffect binaural_effect = nullptr;
        IPLAudioBuffer mono_buffer{};
        IPLAudioBuffer direct_buffer{};
        IPLAudioBuffer stereo_buffer{};
    };

    mutable std::recursive_mutex mutex;
    SDL_AudioStream* stream = nullptr;
    bool sdl_audio_initialized = false;
    bool device_initialized = false;
    IPLContext steam_context = nullptr;
    IPLHRTF hrtf = nullptr;
    IPLAudioSettings steam_audio_settings{kOutputSampleRate, kMixChunkFrames};
    SteamScene steam_scene;
    std::array<Channel, kMaxChannels> channels{};
    std::unordered_map<std::string, std::shared_ptr<AudioSource>> source_cache;
    std::unique_ptr<SourceAssetStore> source_assets;
    const ContentFileSystem* source_assets_filesystem = nullptr;
    std::unique_ptr<SourceSoundLibrary> source_sound_library;
    Listener listener;
    std::uint64_t observed_world_generation = std::numeric_limits<std::uint64_t>::max();
    int next_guid = 1;
    int last_guid = 0;
    double mix_clock = 0.0;
    std::array<float, kMixChunkFrames> hrtf_peak_delays{};

    ~Impl()
    {
        shutdown();
    }

    void shutdown()
    {
        std::scoped_lock lock(mutex);
        for (int index = 0; index < kMaxChannels; ++index)
        {
            stop_channel(index);
        }
        source_cache.clear();
        source_sound_library.reset();
        source_assets.reset();
        source_assets_filesystem = nullptr;

        if (stream != nullptr)
        {
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
        }
        device_initialized = false;

        steam_scene.shutdown(steam_context);
        if (hrtf != nullptr)
        {
            iplHRTFRelease(&hrtf);
        }
        if (steam_context != nullptr)
        {
            iplContextRelease(&steam_context);
        }

        if (sdl_audio_initialized)
        {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            sdl_audio_initialized = false;
        }
    }

    bool create_steam_context()
    {
        if (steam_context != nullptr)
        {
            return true;
        }

        IPLContextSettings context_settings{};
        context_settings.version = STEAMAUDIO_VERSION;
        if (iplContextCreate(&context_settings, &steam_context) != IPL_STATUS_SUCCESS)
        {
            log_warning("Steam Audio context creation failed; spatial audio will use SDL panning only");
            return false;
        }

        IPLHRTFSettings hrtf_settings{};
        hrtf_settings.type = IPL_HRTFTYPE_DEFAULT;
        hrtf_settings.volume = 1.0F;
        hrtf_settings.normType = IPL_HRTFNORMTYPE_NONE;
        if (iplHRTFCreate(steam_context, &steam_audio_settings, &hrtf_settings, &hrtf) != IPL_STATUS_SUCCESS)
        {
            log_warning("Steam Audio HRTF creation failed; HRTF sounds will use stereo panning");
            hrtf = nullptr;
        }

        return true;
    }

    void release_channel_effects(Channel& channel)
    {
        if (channel.binaural_effect != nullptr)
        {
            iplBinauralEffectRelease(&channel.binaural_effect);
        }
        if (channel.direct_effect != nullptr)
        {
            iplDirectEffectRelease(&channel.direct_effect);
        }
        if (channel.stereo_buffer.data != nullptr)
        {
            iplAudioBufferFree(steam_context, &channel.stereo_buffer);
        }
        if (channel.direct_buffer.data != nullptr)
        {
            iplAudioBufferFree(steam_context, &channel.direct_buffer);
        }
        if (channel.mono_buffer.data != nullptr)
        {
            iplAudioBufferFree(steam_context, &channel.mono_buffer);
        }
    }

    void stop_channel(int index)
    {
        Channel& channel = channels[static_cast<std::size_t>(index)];
        if (channel.steam_source_slot >= 0)
        {
            steam_scene.free_source(channel.steam_source_slot);
        }
        release_channel_effects(channel);
        channel = Channel{};
    }

    bool should_use_steam_occlusion(const Channel& channel, ConsoleVariables& variables) const
    {
        return steam_context != nullptr && steam_scene.ready && variables.get_bool("snd_steamaudio_occlusion", true) && !channel.radio &&
               !channel.dry && channel.sound_level > 0;
    }

    void sync_channel_steam_sources(ConsoleVariables& variables)
    {
        for (Channel& channel : channels)
        {
            if (!channel.active)
            {
                continue;
            }

            if (!should_use_steam_occlusion(channel, variables))
            {
                if (channel.steam_source_slot >= 0)
                {
                    steam_scene.free_source(channel.steam_source_slot);
                    channel.steam_source_slot = -1;
                }
                continue;
            }

            if (channel.steam_source_slot < 0)
            {
                channel.steam_source_slot = steam_scene.alloc_source();
            }
            if (channel.steam_source_slot >= 0)
            {
                steam_scene.set_source_position(channel.steam_source_slot, channel.origin);
            }
        }
    }

    void rebuild_steam_scene_if_needed(const LoadedWorld* world, std::uint64_t world_generation, ConsoleVariables& variables)
    {
        if (world_generation == observed_world_generation)
        {
            return;
        }

        observed_world_generation = world_generation;
        if (steam_context == nullptr || world == nullptr || !variables.get_bool("snd_steamaudio_occlusion", true))
        {
            steam_scene.shutdown(steam_context);
            return;
        }

        steam_scene.build(steam_context, *world, variables);
    }

    void update_listener(const CameraState& camera)
    {
        listener.active = camera.active;
        if (!camera.active)
        {
            return;
        }

        listener.origin = camera.origin;
        listener.forward = camera_forward(camera);
        listener.right = camera_right(camera);
        listener.up = normalize(cross(listener.right, listener.forward));
    }

    SourceAssetStore& asset_store(ContentFileSystem& filesystem)
    {
        if (!source_assets || source_assets_filesystem != &filesystem)
        {
            source_assets = std::make_unique<SourceAssetStore>(filesystem);
            source_assets_filesystem = &filesystem;
            source_sound_library.reset();
        }
        return *source_assets;
    }

    SourceSoundLibrary& sound_library(ContentFileSystem& filesystem)
    {
        SourceAssetStore& assets = asset_store(filesystem);
        if (!source_sound_library)
        {
            source_sound_library = std::make_unique<SourceSoundLibrary>(assets);
        }
        return *source_sound_library;
    }

    const SourceSoundEntry* find_sound_entry(std::string_view sample, ContentFileSystem& filesystem)
    {
        return sound_library(filesystem).find(sample);
    }

    std::shared_ptr<AudioSource> load_source(std::string_view sample, ContentFileSystem& filesystem)
    {
        std::string stripped(AudioSystem::skip_sound_chars(sample));
        normalize_slashes(stripped);
        const std::string key = lower_copy(stripped);
        if (const auto it = source_cache.find(key); it != source_cache.end())
        {
            return it->second;
        }

        SourceAssetStore& assets = asset_store(filesystem);
        const std::optional<ResolvedAudioAsset> resolved = resolve_audio_asset(sample, assets);
        if (!resolved)
        {
            log_warning("sound '{}' not found", sample);
            return nullptr;
        }

        ma_decoder_config decoder_config = ma_decoder_config_init(ma_format_f32, kOutputChannels, kOutputSampleRate);
        ma_uint64 frame_count = 0;
        void* decoded_frames = nullptr;
        const ma_result decoded =
            ma_decode_memory(resolved->bytes.data(), resolved->bytes.size(), &decoder_config, &frame_count, &decoded_frames);
        if (decoded != MA_SUCCESS || decoded_frames == nullptr)
        {
            log_warning("failed to decode sound '{}' from '{}': miniaudio error {}", sample, resolved->debug_name, static_cast<int>(decoded));
            return nullptr;
        }

        const std::size_t sample_count = static_cast<std::size_t>(frame_count) * kOutputChannels;
        const float* decoded_samples = static_cast<const float*>(decoded_frames);

        auto source = std::make_shared<AudioSource>();
        source->name = resolved->logical_name;
        source->debug_name = resolved->debug_name;
        source->samples.assign(decoded_samples, decoded_samples + sample_count);
        ma_free(decoded_frames, nullptr);
        source->frame_count = static_cast<std::uint32_t>(frame_count);
        if (source->frame_count == 0)
        {
            log_warning("sound '{}' decoded to no samples", sample);
            return nullptr;
        }

        source_cache.emplace(key, source);
        log_info("precached sound '{}' from '{}'", source->name, source->debug_name);
        return source;
    }

    int pick_channel(const AudioStartParams& params)
    {
        const bool static_sound = params.static_sound || params.channel == ChanStatic || params.channel == ChanStream;
        const int first = static_sound ? kMaxDynamicChannels : 0;
        const int last = static_sound ? kMaxChannels : kMaxDynamicChannels;

        if (params.channel > ChanAuto && params.channel != ChanStream && params.channel != ChanStatic)
        {
            for (int index = first; index < last; ++index)
            {
                const Channel& channel = channels[static_cast<std::size_t>(index)];
                if (channel.active && channel.sound_source == params.sound_source && channel.channel == params.channel)
                {
                    stop_channel(index);
                    return index;
                }
            }
        }

        for (int index = first; index < last; ++index)
        {
            if (!channels[static_cast<std::size_t>(index)].active)
            {
                return index;
            }
        }

        int quietest = first;
        float quietest_volume = std::numeric_limits<float>::max();
        for (int index = first; index < last; ++index)
        {
            const Channel& channel = channels[static_cast<std::size_t>(index)];
            const float volume = channel.last_left + channel.last_right;
            if (volume < quietest_volume)
            {
                quietest_volume = volume;
                quietest = index;
            }
        }
        stop_channel(quietest);
        return quietest;
    }

    bool sample_channel(Channel& channel, float& left, float& right)
    {
        if (!channel.source)
        {
            return false;
        }

        if (channel.position >= static_cast<double>(channel.source->frame_count))
        {
            if (!channel.loop)
            {
                return false;
            }
            channel.position = std::fmod(channel.position, static_cast<double>(channel.source->frame_count));
        }

        const std::uint32_t frame = static_cast<std::uint32_t>(channel.position);
        const std::size_t offset = static_cast<std::size_t>(frame) * kOutputChannels;
        left = channel.source->samples[offset];
        right = channel.source->samples[offset + 1];
        channel.position += channel.pitch_scale;
        return true;
    }

    std::array<float, 2> spatial_gains(Channel& channel, ConsoleVariables& variables, bool apply_steam_direct = true)
    {
        const float master_volume = clamp01(static_cast<float>(variables.get_double("snd_volume", 1.0))) * clamp01(channel.volume);
        const bool spatial = listener.active && !channel.radio && !channel.dry && channel.sound_level > 0;
        if (!spatial)
        {
            channel.last_left = master_volume;
            channel.last_right = master_volume;
            return {master_volume, master_volume};
        }

        const Vec3 delta = channel.origin - listener.origin;
        const float distance = length(delta);
        const float reference_db = static_cast<float>(variables.get_double("snd_refdb", kDefaultReferenceDb));
        const float reference_distance = static_cast<float>(variables.get_double("snd_refdist", kDefaultReferenceDistance));
        const float base_gain = static_cast<float>(variables.get_double("snd_gain", 1.0));
        float gain = AudioSystem::gain_from_sound_level(channel.sound_level, distance, base_gain, reference_db, reference_distance);
        gain = std::min(gain, static_cast<float>(variables.get_double("snd_gain_max", 1.0)));

        if (apply_steam_direct && channel.steam_source_slot >= 0 && variables.get_bool("snd_steamaudio_occlusion", true))
        {
            const float occlusion = steam_scene.occlusion[static_cast<std::size_t>(channel.steam_source_slot)];
            const auto& bands = steam_scene.transmission[static_cast<std::size_t>(channel.steam_source_slot)];
            const float transmission = (bands[0] + bands[1] + bands[2]) / 3.0F;
            gain *= std::max(0.0F, occlusion * transmission);
        }

        float left = master_volume * gain;
        float right = master_volume * gain;
        if (distance > kDefaultRadius)
        {
            const Vec3 direction = normalize(delta);
            const float pan = std::clamp(dot(direction, listener.right), -1.0F, 1.0F);
            left *= std::sqrt((1.0F - pan) * 0.5F);
            right *= std::sqrt((1.0F + pan) * 0.5F);
        }

        channel.last_left = left;
        channel.last_right = right;
        return {left, right};
    }

    bool ensure_channel_effects(Channel& channel)
    {
        if (steam_context == nullptr || hrtf == nullptr)
        {
            return false;
        }

        if (channel.mono_buffer.data == nullptr &&
            iplAudioBufferAllocate(steam_context, 1, kMixChunkFrames, &channel.mono_buffer) != IPL_STATUS_SUCCESS)
        {
            return false;
        }
        if (channel.direct_buffer.data == nullptr &&
            iplAudioBufferAllocate(steam_context, 1, kMixChunkFrames, &channel.direct_buffer) != IPL_STATUS_SUCCESS)
        {
            release_channel_effects(channel);
            return false;
        }
        if (channel.stereo_buffer.data == nullptr &&
            iplAudioBufferAllocate(steam_context, 2, kMixChunkFrames, &channel.stereo_buffer) != IPL_STATUS_SUCCESS)
        {
            release_channel_effects(channel);
            return false;
        }

        if (channel.direct_effect == nullptr)
        {
            IPLDirectEffectSettings direct_settings{};
            direct_settings.numChannels = 1;
            if (iplDirectEffectCreate(steam_context, &steam_audio_settings, &direct_settings, &channel.direct_effect) != IPL_STATUS_SUCCESS)
            {
                release_channel_effects(channel);
                return false;
            }
        }

        if (channel.binaural_effect == nullptr)
        {
            IPLBinauralEffectSettings binaural_settings{};
            binaural_settings.hrtf = hrtf;
            if (iplBinauralEffectCreate(steam_context, &steam_audio_settings, &binaural_settings, &channel.binaural_effect) != IPL_STATUS_SUCCESS)
            {
                release_channel_effects(channel);
                return false;
            }
        }

        return true;
    }

    IPLVector3 hrtf_direction(const Channel& channel) const
    {
        Vec3 delta = channel.origin - listener.origin;
        if (length(delta) <= 0.0001F)
        {
            delta = listener.forward;
        }
        const Vec3 world_direction = normalize(delta);
        return IPLVector3{
            dot(world_direction, listener.right),
            dot(world_direction, listener.up),
            dot(world_direction, listener.forward),
        };
    }

    bool mix_channel_hrtf(Channel& channel, float* output, ConsoleVariables& variables)
    {
        if (!listener.active || channel.radio || channel.dry || !variables.get_bool("snd_use_hrtf", true) || !ensure_channel_effects(channel))
        {
            return false;
        }

        const std::array<float, 2> gains = spatial_gains(channel, variables, false);
        const float gain = std::max(gains[0], gains[1]);
        bool ended = false;
        for (int frame = 0; frame < kMixChunkFrames; ++frame)
        {
            channel.mono_buffer.data[0][frame] = 0.0F;
            const double frame_time = mix_clock + (static_cast<double>(frame) / static_cast<double>(kOutputSampleRate));
            if (frame_time < channel.start_time)
            {
                continue;
            }

            float left = 0.0F;
            float right = 0.0F;
            if (!sample_channel(channel, left, right))
            {
                ended = true;
                continue;
            }
            channel.mono_buffer.data[0][frame] = ((left + right) * 0.5F) * gain;
        }

        IPLDirectEffectParams direct_params{};
        int direct_flags = IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION;
        if (variables.get_bool("snd_steamaudio_transmission", true))
        {
            direct_flags |= IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION;
        }
        direct_params.flags = static_cast<IPLDirectEffectFlags>(direct_flags);
        direct_params.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
        direct_params.distanceAttenuation = 1.0F;
        direct_params.airAbsorption[0] = 1.0F;
        direct_params.airAbsorption[1] = 1.0F;
        direct_params.airAbsorption[2] = 1.0F;
        direct_params.directivity = 1.0F;
        direct_params.occlusion = 1.0F;
        direct_params.transmission[0] = 1.0F;
        direct_params.transmission[1] = 1.0F;
        direct_params.transmission[2] = 1.0F;
        if (channel.steam_source_slot >= 0)
        {
            const std::size_t slot = static_cast<std::size_t>(channel.steam_source_slot);
            direct_params.occlusion = steam_scene.occlusion[slot];
            direct_params.transmission[0] = steam_scene.transmission[slot][0];
            direct_params.transmission[1] = steam_scene.transmission[slot][1];
            direct_params.transmission[2] = steam_scene.transmission[slot][2];
        }

        iplDirectEffectApply(channel.direct_effect, &direct_params, &channel.mono_buffer, &channel.direct_buffer);

        IPLBinauralEffectParams binaural_params{};
        binaural_params.direction = hrtf_direction(channel);
        binaural_params.interpolation = channel.hrtf_bilinear ? IPL_HRTFINTERPOLATION_BILINEAR : IPL_HRTFINTERPOLATION_NEAREST;
        binaural_params.spatialBlend = 1.0F;
        binaural_params.hrtf = hrtf;
        binaural_params.peakDelays = hrtf_peak_delays.data();
        iplBinauralEffectApply(channel.binaural_effect, &binaural_params, &channel.direct_buffer, &channel.stereo_buffer);

        for (int frame = 0; frame < kMixChunkFrames; ++frame)
        {
            output[(frame * 2) + 0] += channel.stereo_buffer.data[0][frame];
            output[(frame * 2) + 1] += channel.stereo_buffer.data[1][frame];
        }

        return ended;
    }

    bool mix_channel_stereo(Channel& channel, float* output, ConsoleVariables& variables)
    {
        const std::array<float, 2> gains = spatial_gains(channel, variables);
        bool ended = false;
        for (int frame = 0; frame < kMixChunkFrames; ++frame)
        {
            const double frame_time = mix_clock + (static_cast<double>(frame) / static_cast<double>(kOutputSampleRate));
            if (frame_time < channel.start_time)
            {
                continue;
            }

            float left = 0.0F;
            float right = 0.0F;
            if (!sample_channel(channel, left, right))
            {
                ended = true;
                break;
            }

            output[(frame * 2) + 0] += left * gains[0];
            output[(frame * 2) + 1] += right * gains[1];
        }
        return ended;
    }

    void mix_chunk(float* output, ConsoleVariables& variables)
    {
        std::fill(output, output + (kMixChunkFrames * kOutputChannels), 0.0F);

        std::vector<int> ended_channels;
        for (int index = 0; index < kMaxChannels; ++index)
        {
            Channel& channel = channels[static_cast<std::size_t>(index)];
            if (!channel.active)
            {
                continue;
            }

            const bool ended = channel.hrtf ? mix_channel_hrtf(channel, output, variables) : mix_channel_stereo(channel, output, variables);
            if (ended)
            {
                ended_channels.push_back(index);
            }
        }

        for (int index : ended_channels)
        {
            stop_channel(index);
        }

        for (int frame = 0; frame < kMixChunkFrames * kOutputChannels; ++frame)
        {
            output[frame] = std::clamp(output[frame], -1.0F, 1.0F);
        }

        mix_clock += static_cast<double>(kMixChunkFrames) / static_cast<double>(kOutputSampleRate);
    }

    void fill_device_queue(ConsoleVariables& variables)
    {
        if (stream == nullptr || !device_initialized || !variables.get_bool("snd_enable", true))
        {
            return;
        }

        const double mixahead = std::clamp(variables.get_double("snd_mixahead", 0.10), 0.02, 0.50);
        const int desired_bytes = static_cast<int>(mixahead * kOutputSampleRate * kOutputChannels * static_cast<int>(sizeof(float)));
        int queued_bytes = SDL_GetAudioStreamQueued(stream);
        std::array<float, kMixChunkFrames * kOutputChannels> chunk{};

        while (queued_bytes < desired_bytes)
        {
            mix_chunk(chunk.data(), variables);
            const int bytes = static_cast<int>(chunk.size() * sizeof(float));
            if (!SDL_PutAudioStreamData(stream, chunk.data(), bytes))
            {
                log_warning("failed to queue audio: {}", SDL_GetError());
                break;
            }
            queued_bytes += bytes;
        }
    }
};

AudioSystem::AudioSystem()
    : impl_(std::make_unique<Impl>())
{
}

AudioSystem::~AudioSystem() = default;
AudioSystem::AudioSystem(AudioSystem&&) noexcept = default;
AudioSystem& AudioSystem::operator=(AudioSystem&&) noexcept = default;

void AudioSystem::register_variables(ConsoleVariables& variables)
{
    variables.register_variable("snd_enable", "1", "Enable the client audio device.");
    variables.register_variable("snd_volume", "1", "Master audio volume.");
    variables.register_variable("snd_musicvolume", "0.45", "Music volume multiplier.");
    variables.register_variable("snd_menu_music", "1", "Play menu music when no world is loaded.");
    variables.register_variable("snd_menu_music_entry", "Musix.HalfTime.valve_csgo_01", "Source sound entry used for menu music.");
    variables.register_variable("snd_mixahead", "0.10", "Seconds of audio to keep queued.");
    variables.register_variable("snd_use_hrtf", "1", "Use Steam Audio HRTF for sounds marked with '~'.");
    variables.register_variable("snd_refdist", "36", "Reference distance for soundlevel attenuation.");
    variables.register_variable("snd_refdb", "60", "Reference dB at snd_refdist.");
    variables.register_variable("snd_gain", "1", "Global distance attenuation gain.");
    variables.register_variable("snd_gain_min", "0.01", "Minimum propagated attenuation gain.");
    variables.register_variable("snd_gain_max", "1", "Maximum propagated attenuation gain.");
    variables.register_variable("snd_show", "0", "Print active channel information.");
    variables.register_variable("snd_steamaudio_occlusion", "1", "Enable Steam Audio world-geometry occlusion.");
    variables.register_variable("snd_steamaudio_occlusion_radius", "32", "Steam Audio volumetric occlusion source radius.");
    variables.register_variable("snd_steamaudio_occlusion_samples", "16", "Steam Audio occlusion samples per source.");
    variables.register_variable("snd_steamaudio_transmission", "1", "Enable Steam Audio frequency-dependent transmission.");
    variables.register_variable("snd_steamaudio_debug", "0", "Print Steam Audio scene debug information.");
}

void AudioSystem::register_commands(CommandRegistry& commands)
{
    commands.register_command("snd_precache", "Precache a sound.", [](const CommandInvocation& invocation, ConsoleCommandContext& context) {
        if (context.audio == nullptr || context.filesystem == nullptr || invocation.args.empty())
        {
            log_warning("usage: snd_precache <sound>");
            return;
        }
        context.audio->precache_sound(invocation.args[0], *context.filesystem);
    });

    commands.register_command("play", "Play a local non-spatial sound.", [](const CommandInvocation& invocation, ConsoleCommandContext& context) {
        if (context.audio == nullptr || context.filesystem == nullptr || invocation.args.empty())
        {
            log_warning("usage: play <sound>");
            return;
        }
        context.audio->emit_ambient_sound(invocation.args[0], 1.0F, 100, SoundNoFlags, *context.filesystem);
    });

    commands.register_command("play_at", "Play a spatial sound at world coordinates.", [](const CommandInvocation& invocation, ConsoleCommandContext& context) {
        if (context.audio == nullptr || context.filesystem == nullptr || invocation.args.size() < 4)
        {
            log_warning("usage: play_at <sound> <x> <y> <z>");
            return;
        }

        const std::optional<Vec3> origin = parse_vec3_args(invocation.args, 1);
        if (!origin)
        {
            log_warning("usage: play_at <sound> <x> <y> <z>");
            return;
        }

        AudioStartParams params;
        params.sample = invocation.args[0];
        params.origin = *origin;
        params.channel = ChanAuto;
        params.sound_level = 75;
        context.audio->emit_sound(params, *context.filesystem);
    });

    commands.register_command("play_hrtf", "Play an HRTF-marked sound in front of the listener.", [](const CommandInvocation& invocation, ConsoleCommandContext& context) {
        if (context.audio == nullptr || context.filesystem == nullptr || invocation.args.empty())
        {
            log_warning("usage: play_hrtf <sound>");
            return;
        }

        AudioStartParams params;
        params.sample = std::string("~") + invocation.args[0];
        params.origin = context.audio->listener_origin() + (context.audio->listener_forward() * 128.0F);
        params.channel = ChanAuto;
        params.sound_level = 75;
        params.hrtf_lock = true;
        context.audio->emit_sound(params, *context.filesystem);
    });

    commands.register_command("stopsound", "Stop all active client sounds.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        if (context.audio != nullptr)
        {
            context.audio->stop_all_sounds(false);
        }
    });

    commands.register_command("snd_status", "Print audio backend status.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        if (context.audio == nullptr)
        {
            log_warning("audio services are not available");
            return;
        }
        log_info("audio initialized={} active_sounds={} last_guid={}",
            context.audio->initialized() ? "yes" : "no",
            context.audio->active_sounds().size(),
            context.audio->guid_for_last_sound_emitted());
    });

    commands.register_command("snd_dumpchannels", "Dump active audio channels.", [](const CommandInvocation&, ConsoleCommandContext& context) {
        if (context.audio == nullptr)
        {
            return;
        }
        for (const ActiveSoundInfo& sound : context.audio->active_sounds())
        {
            log_info("{} ch={} ent={} l={:.2f} r={:.2f} hrtf={} loop={} t={:.2f} {}",
                sound.guid,
                sound.channel,
                sound.sound_source,
                sound.left_volume,
                sound.right_volume,
                sound.hrtf ? "yes" : "no",
                sound.loop ? "yes" : "no",
                sound.seconds_remaining,
                sound.sample);
        }
    });
}

bool AudioSystem::initialize(ConsoleVariables& variables)
{
    std::scoped_lock lock(impl_->mutex);
    if (impl_->device_initialized)
    {
        return true;
    }

    impl_->create_steam_context();

    if (!variables.get_bool("snd_enable", true))
    {
        log_info("audio disabled by snd_enable");
        return false;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        log_warning("SDL audio initialization failed: {}", SDL_GetError());
        return false;
    }
    impl_->sdl_audio_initialized = true;

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = kOutputChannels;
    spec.freq = kOutputSampleRate;
    impl_->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (impl_->stream == nullptr)
    {
        log_warning("failed to open SDL audio stream: {}", SDL_GetError());
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(impl_->stream))
    {
        log_warning("failed to resume SDL audio stream: {}", SDL_GetError());
    }

    impl_->device_initialized = true;
    log_info("audio initialized sample_rate={} channels={} backend=SDL3+SteamAudio", kOutputSampleRate, kOutputChannels);
    return true;
}

void AudioSystem::shutdown()
{
    impl_->shutdown();
}

void AudioSystem::update(const CameraState& camera, const LoadedWorld* world, std::uint64_t world_generation, ConsoleVariables& variables)
{
    std::scoped_lock lock(impl_->mutex);
    impl_->update_listener(camera);
    impl_->rebuild_steam_scene_if_needed(world, world_generation, variables);
    impl_->sync_channel_steam_sources(variables);
    impl_->steam_scene.update(impl_->listener, variables);
    impl_->fill_device_queue(variables);

    if (variables.get_bool("snd_show", false))
    {
        for (const ActiveSoundInfo& sound : active_sounds())
        {
            log_info("snd {} ch={} ent={} l={:.2f} r={:.2f} pos=({}, {}, {}) {}",
                sound.guid,
                sound.channel,
                sound.sound_source,
                sound.left_volume,
                sound.right_volume,
                sound.origin.x,
                sound.origin.y,
                sound.origin.z,
                sound.sample);
        }
    }
}

bool AudioSystem::precache_sound(std::string_view sample, ContentFileSystem& filesystem)
{
    std::scoped_lock lock(impl_->mutex);
    if (const SourceSoundEntry* entry = impl_->find_sound_entry(sample, filesystem))
    {
        return impl_->load_source(entry->wave, filesystem) != nullptr;
    }
    return impl_->load_source(sample, filesystem) != nullptr;
}

int AudioSystem::emit_sound(const AudioStartParams& params, ContentFileSystem& filesystem)
{
    std::scoped_lock lock(impl_->mutex);
    if (params.flags & SoundStop)
    {
        stop_sound(params.sound_source, params.channel, params.sample);
        return 0;
    }

    AudioStartParams effective = params;
    if (const SourceSoundEntry* entry = impl_->find_sound_entry(params.sample, filesystem))
    {
        effective.sample = entry->wave;
        if (params.channel == ChanAuto)
        {
            effective.channel = entry->channel;
        }
        if (params.sound_level == AudioStartParams{}.sound_level)
        {
            effective.sound_level = entry->sound_level;
        }
        if (params.pitch == AudioStartParams{}.pitch)
        {
            effective.pitch = entry->pitch;
        }
        effective.volume *= entry->volume;
        effective.loop = params.loop || entry->loop;
    }

    std::shared_ptr<Impl::AudioSource> source = impl_->load_source(effective.sample, filesystem);
    if (!source)
    {
        return 0;
    }

    const int channel_index = impl_->pick_channel(effective);
    Impl::Channel& channel = impl_->channels[static_cast<std::size_t>(channel_index)];
    channel.active = true;
    channel.guid = impl_->next_guid++;
    channel.sound_source = effective.sound_source;
    channel.channel = effective.channel;
    channel.source = std::move(source);
    channel.sample = params.sample;
    channel.origin = effective.origin;
    channel.direction = effective.direction;
    channel.volume = clamp01(effective.volume);
    channel.sound_level = std::clamp(effective.sound_level, 0, 255);
    channel.flags = effective.flags;
    channel.pitch_scale = std::max(1, effective.pitch) / 100.0F;
    channel.position = 0.0;
    channel.start_time = impl_->mix_clock + std::max(0.0F, effective.delay_seconds);
    channel.emitted_time = impl_->mix_clock;
    channel.speaker_entity = effective.speaker_entity;
    channel.static_sound = effective.static_sound || effective.channel == ChanStatic || effective.channel == ChanStream;
    channel.update_positions = effective.update_positions;
    channel.loop = effective.loop;
    channel.from_server = effective.from_server;
    channel.dry = test_sound_char(effective.sample, kSoundCharDryMix);
    channel.radio = test_sound_char(effective.sample, kSoundCharRadio);
    channel.hrtf = (test_sound_char(effective.sample, kSoundCharHrtf) || effective.hrtf_lock) && !channel.radio;
    channel.hrtf_bilinear = effective.hrtf_bilinear;
    channel.hrtf_lock = effective.hrtf_lock;
    channel.in_eye_sound = effective.in_eye_sound;
    impl_->last_guid = channel.guid;
    return channel.guid;
}

int AudioSystem::emit_ambient_sound(std::string_view sample, float volume, int pitch, int flags, ContentFileSystem& filesystem)
{
    AudioStartParams params;
    params.sample = sample;
    params.channel = ChanStatic;
    params.volume = volume;
    params.pitch = pitch;
    params.flags = flags;
    params.sound_level = 0;
    params.static_sound = true;
    params.origin = impl_->listener.origin;
    return emit_sound(params, filesystem);
}

void AudioSystem::stop_sound(int sound_source, int channel_id, std::string_view sample)
{
    std::scoped_lock lock(impl_->mutex);
    const std::string stripped_sample = lower_copy(skip_sound_chars(sample));
    for (int index = 0; index < kMaxChannels; ++index)
    {
        const Impl::Channel& channel = impl_->channels[static_cast<std::size_t>(index)];
        if (!channel.active || channel.sound_source != sound_source)
        {
            continue;
        }
        if (channel_id != ChanAuto && channel.channel != channel_id)
        {
            continue;
        }
        if (!stripped_sample.empty() && lower_copy(skip_sound_chars(channel.sample)) != stripped_sample)
        {
            continue;
        }
        impl_->stop_channel(index);
    }
}

void AudioSystem::stop_sound_by_guid(int guid)
{
    std::scoped_lock lock(impl_->mutex);
    for (int index = 0; index < kMaxChannels; ++index)
    {
        if (impl_->channels[static_cast<std::size_t>(index)].active && impl_->channels[static_cast<std::size_t>(index)].guid == guid)
        {
            impl_->stop_channel(index);
            return;
        }
    }
}

void AudioSystem::stop_all_sounds(bool clear_cached_sources)
{
    std::scoped_lock lock(impl_->mutex);
    for (int index = 0; index < kMaxChannels; ++index)
    {
        impl_->stop_channel(index);
    }
    if (clear_cached_sources)
    {
        impl_->source_cache.clear();
    }
    if (impl_->stream != nullptr)
    {
        SDL_ClearAudioStream(impl_->stream);
    }
}

void AudioSystem::set_volume_by_guid(int guid, float volume)
{
    std::scoped_lock lock(impl_->mutex);
    for (Impl::Channel& channel : impl_->channels)
    {
        if (channel.active && channel.guid == guid)
        {
            channel.volume = clamp01(volume);
            return;
        }
    }
}

bool AudioSystem::initialized() const
{
    std::scoped_lock lock(impl_->mutex);
    return impl_->device_initialized;
}

int AudioSystem::guid_for_last_sound_emitted() const
{
    std::scoped_lock lock(impl_->mutex);
    return impl_->last_guid;
}

bool AudioSystem::is_sound_still_playing(int guid) const
{
    std::scoped_lock lock(impl_->mutex);
    return std::any_of(impl_->channels.begin(), impl_->channels.end(), [&](const Impl::Channel& channel) {
        return channel.active && channel.guid == guid;
    });
}

float AudioSystem::elapsed_time_by_guid(int guid) const
{
    std::scoped_lock lock(impl_->mutex);
    for (const Impl::Channel& channel : impl_->channels)
    {
        if (channel.active && channel.guid == guid)
        {
            return static_cast<float>(std::max(0.0, impl_->mix_clock - channel.emitted_time));
        }
    }
    return 0.0F;
}

std::vector<ActiveSoundInfo> AudioSystem::active_sounds() const
{
    std::scoped_lock lock(impl_->mutex);
    std::vector<ActiveSoundInfo> sounds;
    for (const Impl::Channel& channel : impl_->channels)
    {
        if (!channel.active || !channel.source)
        {
            continue;
        }

        float remaining = 0.0F;
        if (!channel.loop && channel.pitch_scale > 0.0F)
        {
            remaining = static_cast<float>((static_cast<double>(channel.source->frame_count) - channel.position) /
                                           (static_cast<double>(kOutputSampleRate) * channel.pitch_scale));
        }

        sounds.push_back(ActiveSoundInfo{
            .guid = channel.guid,
            .sound_source = channel.sound_source,
            .channel = channel.channel,
            .sample = channel.sample,
            .origin = channel.origin,
            .left_volume = channel.last_left,
            .right_volume = channel.last_right,
            .seconds_remaining = std::max(0.0F, remaining),
            .loop = channel.loop,
            .hrtf = channel.hrtf,
            .from_server = channel.from_server,
        });
    }
    return sounds;
}

Vec3 AudioSystem::listener_origin() const
{
    std::scoped_lock lock(impl_->mutex);
    return impl_->listener.origin;
}

Vec3 AudioSystem::listener_forward() const
{
    std::scoped_lock lock(impl_->mutex);
    return impl_->listener.forward;
}

bool AudioSystem::is_sound_char(char value)
{
    return value == kSoundCharStream || value == kSoundCharUserVox || value == kSoundCharSentence || value == kSoundCharDryMix ||
           value == kSoundCharDoppler || value == kSoundCharDirectional || value == kSoundCharDistanceVariant || value == kSoundCharOmni ||
           value == kSoundCharHrtf || value == kSoundCharRadio || value == kSoundCharSpatialStereo || value == kSoundCharDirectionalStereo ||
           value == kSoundCharFastPitch || value == kSoundCharSubtitled;
}

std::string_view AudioSystem::skip_sound_chars(std::string_view sample)
{
    std::size_t cursor = 0;
    while (cursor < sample.size() && is_sound_char(sample[cursor]))
    {
        ++cursor;
    }
    return sample.substr(cursor);
}

bool AudioSystem::test_sound_char(std::string_view sample, char value)
{
    for (const char ch : sample)
    {
        if (!is_sound_char(ch))
        {
            break;
        }
        if (ch == value)
        {
            return true;
        }
    }
    return false;
}

float AudioSystem::sound_level_to_distance_multiplier(int sound_level, float reference_db, float reference_distance)
{
    if (sound_level <= 0)
    {
        return 0.0F;
    }

    const float reference = std::pow(10.0F, reference_db / 20.0F);
    return (reference / std::pow(10.0F, static_cast<float>(sound_level) / 20.0F)) / std::max(1.0F, reference_distance);
}

float AudioSystem::gain_from_sound_level(int sound_level, float distance, float gain, float reference_db, float reference_distance)
{
    const float dist_mult = sound_level_to_distance_multiplier(sound_level, reference_db, reference_distance);
    if (dist_mult <= 0.0F)
    {
        return gain;
    }

    const float relative_distance = distance * dist_mult;
    if (relative_distance > 0.1F)
    {
        gain *= 1.0F / relative_distance;
    }
    else
    {
        gain *= 10.0F;
    }

    if (gain > kGainCompressionThreshold)
    {
        float compression_power = kGainCompressionExpMax;
        if (sound_level > kGainDbMedium)
        {
            const float t = std::clamp((static_cast<float>(sound_level) - kGainDbMedium) / (kGainDbMax - kGainDbMedium), 0.0F, 1.0F);
            compression_power = kGainCompressionExpMax + ((kGainCompressionExpMin - kGainCompressionExpMax) * t);
        }

        const float y = -1.0F / (std::pow(kGainCompressionThreshold, compression_power) * (kGainCompressionThreshold - 1.0F));
        gain = 1.0F - (1.0F / (y * std::pow(gain, compression_power)));
    }

    if (gain < 0.01F)
    {
        gain = 0.01F * (2.0F - relative_distance * 0.01F);
        if (gain <= 0.0F)
        {
            gain = 0.001F;
        }
    }

    return gain;
}
}
