#pragma once

#include "openstrike/core/math.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike
{
class SourceAssetStore;

constexpr std::uint32_t kSourceStudioIdent = 0x54534449U; // "IDST"
constexpr std::int32_t kSourceStudioLooping = 0x0001;
constexpr std::size_t kSourceStudioMaxOverlays = 15;

struct StudioBone
{
    std::string name;
    std::int32_t parent = -1;
    std::array<std::int32_t, 6> bone_controller{-1, -1, -1, -1, -1, -1};
    Vec3 position;
    Quat rotation;
    Vec3 euler_rotation;
    Vec3 position_scale;
    Vec3 rotation_scale;
    Mat3x4 pose_to_bone;
    Quat alignment;
    std::int32_t flags = 0;
    std::int32_t procedural_type = 0;
    std::int32_t procedural_index = 0;
    std::int32_t physics_bone = -1;
    std::string surface_prop;
    std::int32_t contents = 0;
};

struct StudioBoneController
{
    std::int32_t bone = -1;
    std::int32_t type = 0;
    float start = 0.0F;
    float end = 0.0F;
    std::int32_t rest = 0;
    std::int32_t input_field = 0;
};

struct StudioHitbox
{
    std::int32_t bone = -1;
    std::int32_t group = 0;
    Vec3 mins;
    Vec3 maxs;
    std::string name;
    Vec3 angle_offset;
    float capsule_radius = 0.0F;
};

struct StudioHitboxSet
{
    std::string name;
    std::vector<StudioHitbox> hitboxes;
};

struct StudioAttachment
{
    std::string name;
    std::uint32_t flags = 0;
    std::int32_t local_bone = -1;
    Mat3x4 local;
};

struct StudioPoseParameter
{
    std::string name;
    std::int32_t flags = 0;
    float start = 0.0F;
    float end = 1.0F;
    float loop = 0.0F;
};

struct StudioEvent
{
    float cycle = 0.0F;
    std::int32_t event = 0;
    std::int32_t type = 0;
    std::string options;
    std::string name;
};

struct StudioAutolayer
{
    std::int16_t sequence = -1;
    std::int16_t pose = -1;
    std::int32_t flags = 0;
    float start = 0.0F;
    float peak = 0.0F;
    float tail = 1.0F;
    float end = 1.0F;
};

struct StudioActivityModifier
{
    std::string name;
};

struct StudioAnimTag
{
    std::int32_t tag = 0;
    float cycle = 0.0F;
    std::string name;
};

struct StudioAnimationTrack
{
    std::int32_t bone = -1;
    std::int32_t flags = 0;
    bool delta = false;
    bool has_position = false;
    bool has_rotation = false;
    Vec3 raw_position;
    Quat raw_rotation;
    std::array<std::vector<float>, 3> position_values;
    std::array<std::vector<float>, 3> rotation_values;
};

struct StudioAnimationClip
{
    std::vector<StudioAnimationTrack> tracks;
};

struct StudioAnimationDesc
{
    std::string name;
    float fps = 30.0F;
    std::int32_t flags = 0;
    std::int32_t frame_count = 1;
    std::int32_t movement_count = 0;
    std::int32_t movement_index = 0;
    std::int32_t anim_block = 0;
    std::int32_t anim_index = 0;
    std::int32_t ik_rule_count = 0;
    std::int32_t ik_rule_index = 0;
    std::int32_t section_count = 0;
    std::int32_t section_frame_count = 0;
    StudioAnimationClip clip;
};

struct StudioSequenceDesc
{
    std::string label;
    std::string activity_name;
    std::int32_t flags = 0;
    std::int32_t activity = -1;
    std::int32_t activity_weight = 0;
    std::vector<StudioEvent> events;
    Vec3 bounds_min;
    Vec3 bounds_max;
    std::int32_t blend_count = 1;
    std::vector<std::int16_t> animation_indices;
    std::array<std::int32_t, 2> group_size{1, 1};
    std::array<std::int32_t, 2> pose_parameter_index{-1, -1};
    std::array<float, 2> pose_parameter_start{0.0F, 0.0F};
    std::array<float, 2> pose_parameter_end{1.0F, 1.0F};
    std::int32_t pose_parameter_parent = -1;
    float fade_in_time = 0.2F;
    float fade_out_time = 0.2F;
    std::int32_t local_entry_node = 0;
    std::int32_t local_exit_node = 0;
    std::int32_t node_flags = 0;
    float entry_phase = 0.0F;
    float exit_phase = 0.0F;
    float last_frame = 1.0F;
    std::int32_t next_sequence = -1;
    std::int32_t pose = -1;
    std::int32_t ik_rule_count = 0;
    std::vector<StudioAutolayer> autolayers;
    std::vector<float> bone_weights;
    std::vector<float> pose_keys;
    std::string key_values;
    std::int32_t cycle_pose_index = -1;
    std::vector<StudioActivityModifier> activity_modifiers;
    std::vector<StudioAnimTag> anim_tags;
    std::int32_t root_driver_index = -1;
};

struct StudioModel
{
    std::string name;
    std::filesystem::path path;
    std::int32_t version = 0;
    std::uint32_t checksum = 0;
    Vec3 eye_position;
    Vec3 illum_position;
    Aabb hull_bounds;
    Aabb view_bounds;
    std::int32_t flags = 0;
    std::string surface_prop;
    std::string key_values;
    float mass = 0.0F;
    std::int32_t contents = 0;
    std::vector<StudioBone> bones;
    std::vector<StudioBoneController> bone_controllers;
    std::vector<StudioHitboxSet> hitbox_sets;
    std::vector<StudioAnimationDesc> animations;
    std::vector<StudioSequenceDesc> sequences;
    std::vector<StudioPoseParameter> pose_parameters;
    std::vector<StudioAttachment> attachments;
};

struct StudioPose
{
    std::vector<Vec3> positions;
    std::vector<Quat> rotations;
    std::vector<Mat3x4> bone_to_model;
};

struct AnimationLayer
{
    std::int32_t sequence = -1;
    float cycle = 0.0F;
    float previous_cycle = 0.0F;
    float playback_rate = 1.0F;
    float weight = 0.0F;
    float weight_delta_rate = 0.0F;
    std::int32_t order = static_cast<std::int32_t>(kSourceStudioMaxOverlays);
    bool active = false;
};

struct AnimationPlaybackState
{
    std::int32_t sequence = 0;
    float cycle = 0.0F;
    float previous_cycle = 0.0F;
    float playback_rate = 1.0F;
    bool sequence_finished = false;
    bool sequence_loops = false;
    float ground_speed = 0.0F;
    std::vector<float> pose_parameters;
    std::vector<AnimationLayer> overlays;
    std::vector<StudioEvent> fired_events;
};

[[nodiscard]] std::optional<StudioModel> load_source_studio_model(
    const SourceAssetStore& assets,
    const std::filesystem::path& model_path);

[[nodiscard]] std::optional<StudioModel> parse_source_studio_model(
    std::span<const unsigned char> bytes,
    std::filesystem::path model_path = {});

[[nodiscard]] std::int32_t lookup_sequence(const StudioModel& model, std::string_view label_or_activity);
[[nodiscard]] std::int32_t lookup_pose_parameter(const StudioModel& model, std::string_view name);
[[nodiscard]] std::int32_t select_weighted_sequence(const StudioModel& model, std::string_view activity_name, std::int32_t current_sequence = -1);
[[nodiscard]] float source_pose_parameter_to_normalized(const StudioPoseParameter& parameter, float value);
[[nodiscard]] float normalized_pose_parameter_to_source(const StudioPoseParameter& parameter, float normalized);
[[nodiscard]] float sequence_duration(const StudioModel& model, std::int32_t sequence);
[[nodiscard]] float sequence_cycle_rate(const StudioModel& model, std::int32_t sequence);
[[nodiscard]] bool sequence_loops(const StudioModel& model, std::int32_t sequence);

void reset_animation_state(AnimationPlaybackState& state, const StudioModel& model, std::int32_t sequence = 0);
void set_sequence(AnimationPlaybackState& state, const StudioModel& model, std::int32_t sequence);
void advance_animation_state(AnimationPlaybackState& state, const StudioModel& model, float delta_seconds);
void collect_sequence_events(const StudioModel& model, std::int32_t sequence, float previous_cycle, float current_cycle, bool looping, std::vector<StudioEvent>& events);
[[nodiscard]] StudioPose evaluate_studio_pose(const StudioModel& model, const AnimationPlaybackState& state);
}
