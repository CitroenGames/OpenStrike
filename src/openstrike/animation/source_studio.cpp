#include "openstrike/animation/source_studio.hpp"

#include "openstrike/source/source_asset_store.hpp"
#include "openstrike/source/source_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace openstrike
{
namespace
{
constexpr std::size_t kStudioLengthOffset = 76;
constexpr std::size_t kStudioNameOffset = 12;
constexpr std::size_t kStudioNameSize = 64;
constexpr std::size_t kStudioEyePositionOffset = 80;
constexpr std::size_t kStudioIllumPositionOffset = 92;
constexpr std::size_t kStudioHullMinOffset = 104;
constexpr std::size_t kStudioHullMaxOffset = 116;
constexpr std::size_t kStudioViewMinOffset = 128;
constexpr std::size_t kStudioViewMaxOffset = 140;
constexpr std::size_t kStudioFlagsOffset = 152;
constexpr std::size_t kStudioNumBonesOffset = 156;
constexpr std::size_t kStudioBoneIndexOffset = 160;
constexpr std::size_t kStudioNumBoneControllersOffset = 164;
constexpr std::size_t kStudioBoneControllerIndexOffset = 168;
constexpr std::size_t kStudioNumHitboxSetsOffset = 172;
constexpr std::size_t kStudioHitboxSetIndexOffset = 176;
constexpr std::size_t kStudioNumLocalAnimationsOffset = 180;
constexpr std::size_t kStudioLocalAnimationIndexOffset = 184;
constexpr std::size_t kStudioNumLocalSequencesOffset = 188;
constexpr std::size_t kStudioLocalSequenceIndexOffset = 192;
constexpr std::size_t kStudioNumLocalAttachmentsOffset = 240;
constexpr std::size_t kStudioLocalAttachmentIndexOffset = 244;
constexpr std::size_t kStudioNumLocalPoseParametersOffset = 300;
constexpr std::size_t kStudioLocalPoseParameterIndexOffset = 304;
constexpr std::size_t kStudioSurfacePropIndexOffset = 308;
constexpr std::size_t kStudioKeyValueIndexOffset = 312;
constexpr std::size_t kStudioKeyValueSizeOffset = 316;
constexpr std::size_t kStudioMassOffset = 328;
constexpr std::size_t kStudioContentsOffset = 332;

constexpr std::size_t kStudioBoneSize = 216;
constexpr std::size_t kStudioBoneControllerSize = 56;
constexpr std::size_t kStudioHitboxSetSize = 12;
constexpr std::size_t kStudioHitboxSize = 68;
constexpr std::size_t kStudioAnimationDescSize = 100;
constexpr std::size_t kStudioSequenceDescSize = 216;
constexpr std::size_t kStudioPoseParameterSize = 20;
constexpr std::size_t kStudioAttachmentSize = 92;
constexpr std::size_t kStudioEventSize = 80;
constexpr std::size_t kStudioAutolayerSize = 24;
constexpr std::size_t kStudioActivityModifierSize = 4;
constexpr std::size_t kStudioAnimTagSize = 12;
constexpr std::uint8_t kStudioAnimRawPosition = 0x01;
constexpr std::uint8_t kStudioAnimRawRotation48 = 0x02;
constexpr std::uint8_t kStudioAnimAnimatedPosition = 0x04;
constexpr std::uint8_t kStudioAnimAnimatedRotation = 0x08;
constexpr std::uint8_t kStudioAnimDelta = 0x10;
constexpr std::uint8_t kStudioAnimRawRotation64 = 0x20;

std::uint32_t read_u32_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size())
    {
        return 0;
    }

    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::uint64_t read_u64_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    if (offset + 8 > bytes.size())
    {
        return 0;
    }

    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index)
    {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

std::int32_t read_i32_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    return static_cast<std::int32_t>(read_u32_le(bytes, offset));
}

std::uint16_t read_u16_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    if (offset + 2 > bytes.size())
    {
        return 0;
    }

    return static_cast<std::uint16_t>(bytes[offset]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::int16_t read_i16_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    return static_cast<std::int16_t>(read_u16_le(bytes, offset));
}

float read_f32_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    const std::uint32_t raw = read_u32_le(bytes, offset);
    float value = 0.0F;
    static_assert(sizeof(value) == sizeof(raw));
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

float read_f16_le(std::span<const unsigned char> bytes, std::size_t offset)
{
    const std::uint16_t raw = read_u16_le(bytes, offset);
    const std::uint32_t sign = static_cast<std::uint32_t>(raw & 0x8000U) << 16U;
    std::uint32_t exponent = (raw >> 10U) & 0x1FU;
    std::uint32_t mantissa = raw & 0x03FFU;

    std::uint32_t result = 0;
    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            result = sign;
        }
        else
        {
            exponent = 1;
            while ((mantissa & 0x0400U) == 0)
            {
                mantissa <<= 1U;
                --exponent;
            }
            mantissa &= 0x03FFU;
            result = sign | ((exponent + (127U - 15U)) << 23U) | (mantissa << 13U);
        }
    }
    else if (exponent == 31)
    {
        result = sign | 0x7F800000U | (mantissa << 13U);
    }
    else
    {
        result = sign | ((exponent + (127U - 15U)) << 23U) | (mantissa << 13U);
    }

    float value = 0.0F;
    std::memcpy(&value, &result, sizeof(value));
    return value;
}

Vec3 read_vec3(std::span<const unsigned char> bytes, std::size_t offset)
{
    return {
        read_f32_le(bytes, offset),
        read_f32_le(bytes, offset + 4),
        read_f32_le(bytes, offset + 8),
    };
}

Vec3 read_vec48(std::span<const unsigned char> bytes, std::size_t offset)
{
    return {
        read_f16_le(bytes, offset),
        read_f16_le(bytes, offset + 2),
        read_f16_le(bytes, offset + 4),
    };
}

Quat read_quat(std::span<const unsigned char> bytes, std::size_t offset)
{
    return normalize(Quat{
        read_f32_le(bytes, offset),
        read_f32_le(bytes, offset + 4),
        read_f32_le(bytes, offset + 8),
        read_f32_le(bytes, offset + 12),
    });
}

Quat read_quat48(std::span<const unsigned char> bytes, std::size_t offset)
{
    if (offset + 6 > bytes.size())
    {
        return {};
    }

    std::uint64_t raw = 0;
    for (std::size_t index = 0; index < 6; ++index)
    {
        raw |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }

    const float x = (static_cast<int>(raw & 0xFFFFU) - 32768) * (1.0F / 32768.5F);
    const float y = (static_cast<int>((raw >> 16U) & 0xFFFFU) - 32768) * (1.0F / 32768.5F);
    const float z = (static_cast<int>((raw >> 32U) & 0x7FFFU) - 16384) * (1.0F / 16384.5F);
    float w = std::sqrt(std::max(0.0F, 1.0F - (x * x) - (y * y) - (z * z)));
    if (((raw >> 47U) & 0x1U) != 0)
    {
        w = -w;
    }
    return normalize(Quat{x, y, z, w});
}

Quat read_quat64(std::span<const unsigned char> bytes, std::size_t offset)
{
    const std::uint64_t raw = read_u64_le(bytes, offset);
    const float x = (static_cast<int>(raw & 0x1FFFFFU) - 1048576) * (1.0F / 1048576.5F);
    const float y = (static_cast<int>((raw >> 21U) & 0x1FFFFFU) - 1048576) * (1.0F / 1048576.5F);
    const float z = (static_cast<int>((raw >> 42U) & 0x1FFFFFU) - 1048576) * (1.0F / 1048576.5F);
    float w = std::sqrt(std::max(0.0F, 1.0F - (x * x) - (y * y) - (z * z)));
    if (((raw >> 63U) & 0x1U) != 0)
    {
        w = -w;
    }
    return normalize(Quat{x, y, z, w});
}

Quat quat_from_radians(Vec3 radians)
{
    const float sx = std::sin(radians.x * 0.5F);
    const float cx = std::cos(radians.x * 0.5F);
    const float sy = std::sin(radians.y * 0.5F);
    const float cy = std::cos(radians.y * 0.5F);
    const float sz = std::sin(radians.z * 0.5F);
    const float cz = std::cos(radians.z * 0.5F);

    return normalize(Quat{
        (sx * cy * cz) + (cx * sy * sz),
        (cx * sy * cz) - (sx * cy * sz),
        (cx * cy * sz) + (sx * sy * cz),
        (cx * cy * cz) - (sx * sy * sz),
    });
}

Mat3x4 read_mat3x4(std::span<const unsigned char> bytes, std::size_t offset)
{
    return {
        {read_f32_le(bytes, offset + 0), read_f32_le(bytes, offset + 4), read_f32_le(bytes, offset + 8)},
        {read_f32_le(bytes, offset + 16), read_f32_le(bytes, offset + 20), read_f32_le(bytes, offset + 24)},
        {read_f32_le(bytes, offset + 32), read_f32_le(bytes, offset + 36), read_f32_le(bytes, offset + 40)},
        {read_f32_le(bytes, offset + 12), read_f32_le(bytes, offset + 28), read_f32_le(bytes, offset + 44)},
    };
}

bool valid_range(std::span<const unsigned char> bytes, std::size_t offset, std::size_t size)
{
    return offset <= bytes.size() && size <= bytes.size() - offset;
}

bool checked_byte_count(std::int32_t count, std::size_t stride, std::size_t& byte_count)
{
    if (count < 0 || stride == 0)
    {
        return false;
    }

    const std::size_t unsigned_count = static_cast<std::size_t>(count);
    if (unsigned_count > std::numeric_limits<std::size_t>::max() / stride)
    {
        return false;
    }

    byte_count = unsigned_count * stride;
    return true;
}

bool valid_count_range(std::span<const unsigned char> bytes, std::int32_t offset, std::int32_t count, std::size_t stride)
{
    std::size_t byte_count = 0;
    return offset >= 0 && checked_byte_count(count, stride, byte_count) && valid_range(bytes, static_cast<std::size_t>(offset), byte_count);
}

bool valid_relative_count_range(
    std::span<const unsigned char> bytes,
    std::size_t base_offset,
    std::int32_t relative_offset,
    std::int32_t count,
    std::size_t stride)
{
    if (relative_offset < 0)
    {
        return false;
    }

    const std::size_t offset = base_offset + static_cast<std::size_t>(relative_offset);
    if (offset < base_offset)
    {
        return false;
    }

    std::size_t byte_count = 0;
    return checked_byte_count(count, stride, byte_count) && valid_range(bytes, offset, byte_count);
}

std::string read_zero_string_at(std::span<const unsigned char> bytes, std::size_t offset, std::size_t max_length = std::numeric_limits<std::size_t>::max())
{
    if (offset >= bytes.size())
    {
        return {};
    }

    const std::size_t begin = offset;
    const std::size_t remaining = bytes.size() - begin;
    const std::size_t limit = begin + std::min(max_length, remaining);
    while (offset < limit && bytes[offset] != 0)
    {
        ++offset;
    }
    return std::string(reinterpret_cast<const char*>(bytes.data() + begin), offset - begin);
}

std::string read_relative_string(std::span<const unsigned char> bytes, std::size_t base_offset, std::int32_t relative_offset)
{
    if (relative_offset <= 0)
    {
        return {};
    }

    const std::size_t offset = base_offset + static_cast<std::size_t>(relative_offset);
    if (offset < base_offset)
    {
        return {};
    }
    return read_zero_string_at(bytes, offset);
}

std::string read_blob_text(std::span<const unsigned char> bytes, std::int32_t offset, std::int32_t size)
{
    if (offset <= 0 || size <= 0 || !valid_range(bytes, static_cast<std::size_t>(offset), static_cast<std::size_t>(size)))
    {
        return {};
    }

    return std::string(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::size_t>(size));
}

std::filesystem::path normalize_model_path(std::filesystem::path path)
{
    std::string normalized = path.generic_string();
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.size() < 4 || normalized.substr(normalized.size() - 4) != ".mdl")
    {
        normalized += ".mdl";
    }
    return normalized;
}

std::vector<StudioEvent> read_events(std::span<const unsigned char> bytes, std::size_t sequence_offset, std::int32_t count, std::int32_t relative_index)
{
    std::vector<StudioEvent> events;
    if (!valid_relative_count_range(bytes, sequence_offset, relative_index, count, kStudioEventSize))
    {
        return events;
    }

    events.reserve(static_cast<std::size_t>(count));
    const std::size_t event_base = sequence_offset + static_cast<std::size_t>(relative_index);
    for (std::int32_t event_index = 0; event_index < count; ++event_index)
    {
        const std::size_t offset = event_base + (static_cast<std::size_t>(event_index) * kStudioEventSize);
        StudioEvent event;
        event.cycle = read_f32_le(bytes, offset + 0);
        event.event = read_i32_le(bytes, offset + 4);
        event.type = read_i32_le(bytes, offset + 8);
        event.options = read_zero_string_at(bytes, offset + 12, 64);
        event.name = read_relative_string(bytes, offset, read_i32_le(bytes, offset + 76));
        events.push_back(std::move(event));
    }
    return events;
}

std::vector<StudioAutolayer> read_autolayers(
    std::span<const unsigned char> bytes,
    std::size_t sequence_offset,
    std::int32_t count,
    std::int32_t relative_index)
{
    std::vector<StudioAutolayer> autolayers;
    if (!valid_relative_count_range(bytes, sequence_offset, relative_index, count, kStudioAutolayerSize))
    {
        return autolayers;
    }

    autolayers.reserve(static_cast<std::size_t>(count));
    const std::size_t base = sequence_offset + static_cast<std::size_t>(relative_index);
    for (std::int32_t index = 0; index < count; ++index)
    {
        const std::size_t offset = base + (static_cast<std::size_t>(index) * kStudioAutolayerSize);
        autolayers.push_back(StudioAutolayer{
            .sequence = read_i16_le(bytes, offset + 0),
            .pose = read_i16_le(bytes, offset + 2),
            .flags = read_i32_le(bytes, offset + 4),
            .start = read_f32_le(bytes, offset + 8),
            .peak = read_f32_le(bytes, offset + 12),
            .tail = read_f32_le(bytes, offset + 16),
            .end = read_f32_le(bytes, offset + 20),
        });
    }
    return autolayers;
}

std::vector<StudioActivityModifier> read_activity_modifiers(
    std::span<const unsigned char> bytes,
    std::size_t sequence_offset,
    std::int32_t relative_index,
    std::int32_t count)
{
    std::vector<StudioActivityModifier> modifiers;
    if (!valid_relative_count_range(bytes, sequence_offset, relative_index, count, kStudioActivityModifierSize))
    {
        return modifiers;
    }

    modifiers.reserve(static_cast<std::size_t>(count));
    const std::size_t base = sequence_offset + static_cast<std::size_t>(relative_index);
    for (std::int32_t index = 0; index < count; ++index)
    {
        const std::size_t offset = base + (static_cast<std::size_t>(index) * kStudioActivityModifierSize);
        modifiers.push_back({.name = read_relative_string(bytes, offset, read_i32_le(bytes, offset))});
    }
    return modifiers;
}

std::vector<StudioAnimTag> read_anim_tags(
    std::span<const unsigned char> bytes,
    std::size_t sequence_offset,
    std::int32_t relative_index,
    std::int32_t count)
{
    std::vector<StudioAnimTag> tags;
    if (!valid_relative_count_range(bytes, sequence_offset, relative_index, count, kStudioAnimTagSize))
    {
        return tags;
    }

    tags.reserve(static_cast<std::size_t>(count));
    const std::size_t base = sequence_offset + static_cast<std::size_t>(relative_index);
    for (std::int32_t index = 0; index < count; ++index)
    {
        const std::size_t offset = base + (static_cast<std::size_t>(index) * kStudioAnimTagSize);
        tags.push_back(StudioAnimTag{
            .tag = read_i32_le(bytes, offset),
            .cycle = read_f32_le(bytes, offset + 4),
            .name = read_relative_string(bytes, offset, read_i32_le(bytes, offset + 8)),
        });
    }
    return tags;
}

float sample_anim_value(std::span<const unsigned char> bytes, std::size_t value_offset, std::int32_t frame, float scale)
{
    if (value_offset == 0)
    {
        return 0.0F;
    }

    std::size_t cursor = value_offset;
    std::int32_t local_frame = std::max(frame, 0);
    for (std::int32_t guard = 0; guard < 4096 && valid_range(bytes, cursor, 2); ++guard)
    {
        const std::uint8_t valid = bytes[cursor];
        const std::uint8_t total = bytes[cursor + 1];
        if (total == 0)
        {
            return 0.0F;
        }

        if (local_frame >= total)
        {
            local_frame -= total;
            cursor += (static_cast<std::size_t>(valid) + 1U) * sizeof(std::int16_t);
            continue;
        }

        const std::size_t value_index = valid > local_frame ? static_cast<std::size_t>(local_frame + 1) : static_cast<std::size_t>(valid);
        const std::size_t sample_offset = cursor + (value_index * sizeof(std::int16_t));
        return static_cast<float>(read_i16_le(bytes, sample_offset)) * scale;
    }

    return 0.0F;
}

std::vector<float> decode_anim_channel(std::span<const unsigned char> bytes, std::size_t value_offset, std::int32_t frame_count, float scale)
{
    std::vector<float> values;
    if (value_offset == 0 || frame_count <= 0)
    {
        return values;
    }

    values.reserve(static_cast<std::size_t>(frame_count));
    for (std::int32_t frame = 0; frame < frame_count; ++frame)
    {
        values.push_back(sample_anim_value(bytes, value_offset, frame, scale));
    }
    return values;
}

void read_animation_clip(
    std::span<const unsigned char> bytes,
    std::size_t animation_desc_offset,
    StudioAnimationDesc& animation,
    const std::vector<StudioBone>& bones)
{
    if (animation.anim_block != 0 || animation.anim_index <= 0)
    {
        return;
    }

    std::size_t offset = animation_desc_offset + static_cast<std::size_t>(animation.anim_index);
    for (std::size_t guard = 0; guard < bones.size() + 64U && valid_range(bytes, offset, 4); ++guard)
    {
        const std::uint8_t bone_index = bytes[offset];
        const std::uint8_t flags = bytes[offset + 1];
        const std::int16_t next_offset = read_i16_le(bytes, offset + 2);
        if (bone_index >= bones.size())
        {
            break;
        }

        const StudioBone& bone = bones[bone_index];
        StudioAnimationTrack track;
        track.bone = bone_index;
        track.flags = flags;
        track.delta = (flags & kStudioAnimDelta) != 0;
        const std::size_t payload = offset + 4;

        if ((flags & kStudioAnimRawRotation48) != 0)
        {
            track.raw_rotation = read_quat48(bytes, payload);
            track.has_rotation = true;
        }
        else if ((flags & kStudioAnimRawRotation64) != 0)
        {
            track.raw_rotation = read_quat64(bytes, payload);
            track.has_rotation = true;
        }

        if ((flags & kStudioAnimRawPosition) != 0)
        {
            const std::size_t position_offset = payload + ((flags & kStudioAnimRawRotation48) != 0 ? 6U : 0U) +
                                                ((flags & kStudioAnimRawRotation64) != 0 ? 8U : 0U);
            track.raw_position = read_vec48(bytes, position_offset);
            track.has_position = true;
        }

        if ((flags & kStudioAnimAnimatedRotation) != 0 && valid_range(bytes, payload, 6))
        {
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                const std::int16_t value_relative_offset = read_i16_le(bytes, payload + (axis * sizeof(std::int16_t)));
                if (value_relative_offset > 0)
                {
                    track.rotation_values[axis] =
                        decode_anim_channel(bytes, payload + static_cast<std::size_t>(value_relative_offset), animation.frame_count, (&bone.rotation_scale.x)[axis]);
                    track.has_rotation = true;
                }
            }
        }

        if ((flags & kStudioAnimAnimatedPosition) != 0)
        {
            const std::size_t position_values = payload + ((flags & kStudioAnimAnimatedRotation) != 0 ? 6U : 0U);
            if (valid_range(bytes, position_values, 6))
            {
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    const std::int16_t value_relative_offset = read_i16_le(bytes, position_values + (axis * sizeof(std::int16_t)));
                    if (value_relative_offset > 0)
                    {
                        track.position_values[axis] = decode_anim_channel(
                            bytes,
                            position_values + static_cast<std::size_t>(value_relative_offset),
                            animation.frame_count,
                            (&bone.position_scale.x)[axis]);
                        track.has_position = true;
                    }
                }
            }
        }

        if (track.has_position || track.has_rotation)
        {
            animation.clip.tracks.push_back(std::move(track));
        }

        if (next_offset <= 0)
        {
            break;
        }

        const std::size_t next = offset + static_cast<std::size_t>(next_offset);
        if (next <= offset)
        {
            break;
        }
        offset = next;
    }
}

void read_bones(std::span<const unsigned char> bytes, StudioModel& model)
{
    const std::int32_t count = read_i32_le(bytes, kStudioNumBonesOffset);
    const std::int32_t table = read_i32_le(bytes, kStudioBoneIndexOffset);
    if (!valid_count_range(bytes, table, count, kStudioBoneSize))
    {
        return;
    }

    model.bones.reserve(static_cast<std::size_t>(count));
    for (std::int32_t index = 0; index < count; ++index)
    {
        const std::size_t offset = static_cast<std::size_t>(table) + (static_cast<std::size_t>(index) * kStudioBoneSize);
        StudioBone bone;
        bone.name = read_relative_string(bytes, offset, read_i32_le(bytes, offset));
        bone.parent = read_i32_le(bytes, offset + 4);
        for (std::size_t controller = 0; controller < bone.bone_controller.size(); ++controller)
        {
            bone.bone_controller[controller] = read_i32_le(bytes, offset + 8 + (controller * 4));
        }
        bone.position = read_vec3(bytes, offset + 32);
        bone.rotation = read_quat(bytes, offset + 44);
        bone.euler_rotation = read_vec3(bytes, offset + 60);
        bone.position_scale = read_vec3(bytes, offset + 72);
        bone.rotation_scale = read_vec3(bytes, offset + 84);
        bone.pose_to_bone = read_mat3x4(bytes, offset + 96);
        bone.alignment = read_quat(bytes, offset + 144);
        bone.flags = read_i32_le(bytes, offset + 160);
        bone.procedural_type = read_i32_le(bytes, offset + 164);
        bone.procedural_index = read_i32_le(bytes, offset + 168);
        bone.physics_bone = read_i32_le(bytes, offset + 172);
        bone.surface_prop = read_relative_string(bytes, offset, read_i32_le(bytes, offset + 176));
        bone.contents = read_i32_le(bytes, offset + 180);
        model.bones.push_back(std::move(bone));
    }
}

void read_bone_controllers(std::span<const unsigned char> bytes, StudioModel& model)
{
    const std::int32_t count = read_i32_le(bytes, kStudioNumBoneControllersOffset);
    const std::int32_t table = read_i32_le(bytes, kStudioBoneControllerIndexOffset);
    if (!valid_count_range(bytes, table, count, kStudioBoneControllerSize))
    {
        return;
    }

    model.bone_controllers.reserve(static_cast<std::size_t>(count));
    for (std::int32_t index = 0; index < count; ++index)
    {
        const std::size_t offset = static_cast<std::size_t>(table) + (static_cast<std::size_t>(index) * kStudioBoneControllerSize);
        model.bone_controllers.push_back(StudioBoneController{
            .bone = read_i32_le(bytes, offset),
            .type = read_i32_le(bytes, offset + 4),
            .start = read_f32_le(bytes, offset + 8),
            .end = read_f32_le(bytes, offset + 12),
            .rest = read_i32_le(bytes, offset + 16),
            .input_field = read_i32_le(bytes, offset + 20),
        });
    }
}

void read_hitbox_sets(std::span<const unsigned char> bytes, StudioModel& model)
{
    const std::int32_t count = read_i32_le(bytes, kStudioNumHitboxSetsOffset);
    const std::int32_t table = read_i32_le(bytes, kStudioHitboxSetIndexOffset);
    if (!valid_count_range(bytes, table, count, kStudioHitboxSetSize))
    {
        return;
    }

    model.hitbox_sets.reserve(static_cast<std::size_t>(count));
    for (std::int32_t set_index = 0; set_index < count; ++set_index)
    {
        const std::size_t offset = static_cast<std::size_t>(table) + (static_cast<std::size_t>(set_index) * kStudioHitboxSetSize);
        StudioHitboxSet set;
        set.name = read_relative_string(bytes, offset, read_i32_le(bytes, offset));
        const std::int32_t hitbox_count = read_i32_le(bytes, offset + 4);
        const std::int32_t hitbox_index = read_i32_le(bytes, offset + 8);
        if (valid_relative_count_range(bytes, offset, hitbox_index, hitbox_count, kStudioHitboxSize))
        {
            const std::size_t hitbox_base = offset + static_cast<std::size_t>(hitbox_index);
            set.hitboxes.reserve(static_cast<std::size_t>(hitbox_count));
            for (std::int32_t hitbox = 0; hitbox < hitbox_count; ++hitbox)
            {
                const std::size_t hitbox_offset = hitbox_base + (static_cast<std::size_t>(hitbox) * kStudioHitboxSize);
                set.hitboxes.push_back(StudioHitbox{
                    .bone = read_i32_le(bytes, hitbox_offset),
                    .group = read_i32_le(bytes, hitbox_offset + 4),
                    .mins = read_vec3(bytes, hitbox_offset + 8),
                    .maxs = read_vec3(bytes, hitbox_offset + 20),
                    .name = read_relative_string(bytes, hitbox_offset, read_i32_le(bytes, hitbox_offset + 32)),
                    .angle_offset = read_vec3(bytes, hitbox_offset + 36),
                    .capsule_radius = read_f32_le(bytes, hitbox_offset + 48),
                });
            }
        }
        model.hitbox_sets.push_back(std::move(set));
    }
}

void read_animation_descs(std::span<const unsigned char> bytes, StudioModel& model)
{
    const std::int32_t count = read_i32_le(bytes, kStudioNumLocalAnimationsOffset);
    const std::int32_t table = read_i32_le(bytes, kStudioLocalAnimationIndexOffset);
    if (!valid_count_range(bytes, table, count, kStudioAnimationDescSize))
    {
        return;
    }

    model.animations.reserve(static_cast<std::size_t>(count));
    for (std::int32_t index = 0; index < count; ++index)
    {
        const std::size_t offset = static_cast<std::size_t>(table) + (static_cast<std::size_t>(index) * kStudioAnimationDescSize);
        StudioAnimationDesc anim;
        anim.name = read_relative_string(bytes, offset, read_i32_le(bytes, offset + 4));
        anim.fps = read_f32_le(bytes, offset + 8);
        anim.flags = read_i32_le(bytes, offset + 12);
        anim.frame_count = std::max(1, read_i32_le(bytes, offset + 16));
        anim.movement_count = read_i32_le(bytes, offset + 20);
        anim.movement_index = read_i32_le(bytes, offset + 24);
        anim.anim_block = read_i32_le(bytes, offset + 52);
        anim.anim_index = read_i32_le(bytes, offset + 56);
        anim.ik_rule_count = read_i32_le(bytes, offset + 60);
        anim.ik_rule_index = read_i32_le(bytes, offset + 64);
        anim.section_frame_count = read_i32_le(bytes, offset + 84);
        if (anim.section_frame_count > 0)
        {
            anim.section_count = (anim.frame_count + anim.section_frame_count - 1) / anim.section_frame_count;
        }
        read_animation_clip(bytes, offset, anim, model.bones);
        model.animations.push_back(std::move(anim));
    }
}

void read_sequences(std::span<const unsigned char> bytes, StudioModel& model)
{
    const std::int32_t count = read_i32_le(bytes, kStudioNumLocalSequencesOffset);
    const std::int32_t table = read_i32_le(bytes, kStudioLocalSequenceIndexOffset);
    if (!valid_count_range(bytes, table, count, kStudioSequenceDescSize))
    {
        return;
    }

    model.sequences.reserve(static_cast<std::size_t>(count));
    for (std::int32_t index = 0; index < count; ++index)
    {
        const std::size_t offset = static_cast<std::size_t>(table) + (static_cast<std::size_t>(index) * kStudioSequenceDescSize);
        StudioSequenceDesc sequence;
        sequence.label = read_relative_string(bytes, offset, read_i32_le(bytes, offset + 4));
        sequence.activity_name = read_relative_string(bytes, offset, read_i32_le(bytes, offset + 8));
        sequence.flags = read_i32_le(bytes, offset + 12);
        sequence.activity = read_i32_le(bytes, offset + 16);
        sequence.activity_weight = read_i32_le(bytes, offset + 20);
        sequence.events = read_events(bytes, offset, read_i32_le(bytes, offset + 24), read_i32_le(bytes, offset + 28));
        sequence.bounds_min = read_vec3(bytes, offset + 32);
        sequence.bounds_max = read_vec3(bytes, offset + 44);
        sequence.blend_count = std::max(1, read_i32_le(bytes, offset + 56));
        const std::int32_t anim_index_index = read_i32_le(bytes, offset + 60);
        sequence.group_size = {std::max(1, read_i32_le(bytes, offset + 68)), std::max(1, read_i32_le(bytes, offset + 72))};
        sequence.pose_parameter_index = {read_i32_le(bytes, offset + 76), read_i32_le(bytes, offset + 80)};
        sequence.pose_parameter_start = {read_f32_le(bytes, offset + 84), read_f32_le(bytes, offset + 88)};
        sequence.pose_parameter_end = {read_f32_le(bytes, offset + 92), read_f32_le(bytes, offset + 96)};
        sequence.pose_parameter_parent = read_i32_le(bytes, offset + 100);
        sequence.fade_in_time = read_f32_le(bytes, offset + 104);
        sequence.fade_out_time = read_f32_le(bytes, offset + 108);
        sequence.local_entry_node = read_i32_le(bytes, offset + 112);
        sequence.local_exit_node = read_i32_le(bytes, offset + 116);
        sequence.node_flags = read_i32_le(bytes, offset + 120);
        sequence.entry_phase = read_f32_le(bytes, offset + 124);
        sequence.exit_phase = read_f32_le(bytes, offset + 128);
        sequence.last_frame = read_f32_le(bytes, offset + 132);
        sequence.next_sequence = read_i32_le(bytes, offset + 136);
        sequence.pose = read_i32_le(bytes, offset + 140);
        sequence.ik_rule_count = read_i32_le(bytes, offset + 144);
        sequence.autolayers = read_autolayers(bytes, offset, read_i32_le(bytes, offset + 148), read_i32_le(bytes, offset + 152));
        const std::int32_t bone_weight_index = read_i32_le(bytes, offset + 156);
        const std::int32_t pose_key_index = read_i32_le(bytes, offset + 160);
        sequence.key_values = read_blob_text(bytes, static_cast<std::int32_t>(offset) + read_i32_le(bytes, offset + 172), read_i32_le(bytes, offset + 176));
        sequence.cycle_pose_index = read_i32_le(bytes, offset + 180);
        sequence.activity_modifiers = read_activity_modifiers(bytes, offset, read_i32_le(bytes, offset + 184), read_i32_le(bytes, offset + 188));
        sequence.anim_tags = read_anim_tags(bytes, offset, read_i32_le(bytes, offset + 192), read_i32_le(bytes, offset + 196));
        sequence.root_driver_index = read_i32_le(bytes, offset + 200);

        const std::int32_t blend_slots = sequence.group_size[0] * sequence.group_size[1];
        if (valid_relative_count_range(bytes, offset, anim_index_index, blend_slots, sizeof(std::int16_t)))
        {
            const std::size_t base = offset + static_cast<std::size_t>(anim_index_index);
            sequence.animation_indices.reserve(static_cast<std::size_t>(blend_slots));
            for (std::int32_t slot = 0; slot < blend_slots; ++slot)
            {
                sequence.animation_indices.push_back(read_i16_le(bytes, base + (static_cast<std::size_t>(slot) * 2U)));
            }
        }

        if (valid_relative_count_range(bytes, offset, bone_weight_index, static_cast<std::int32_t>(model.bones.size()), sizeof(float)))
        {
            const std::size_t base = offset + static_cast<std::size_t>(bone_weight_index);
            sequence.bone_weights.reserve(model.bones.size());
            for (std::size_t bone = 0; bone < model.bones.size(); ++bone)
            {
                sequence.bone_weights.push_back(read_f32_le(bytes, base + (bone * sizeof(float))));
            }
        }

        if (valid_relative_count_range(bytes, offset, pose_key_index, sequence.group_size[0] + sequence.group_size[1], sizeof(float)))
        {
            const std::size_t base = offset + static_cast<std::size_t>(pose_key_index);
            const std::int32_t count_keys = sequence.group_size[0] + sequence.group_size[1];
            sequence.pose_keys.reserve(static_cast<std::size_t>(count_keys));
            for (std::int32_t key = 0; key < count_keys; ++key)
            {
                sequence.pose_keys.push_back(read_f32_le(bytes, base + (static_cast<std::size_t>(key) * sizeof(float))));
            }
        }

        model.sequences.push_back(std::move(sequence));
    }
}

void read_pose_parameters(std::span<const unsigned char> bytes, StudioModel& model)
{
    const std::int32_t count = read_i32_le(bytes, kStudioNumLocalPoseParametersOffset);
    const std::int32_t table = read_i32_le(bytes, kStudioLocalPoseParameterIndexOffset);
    if (!valid_count_range(bytes, table, count, kStudioPoseParameterSize))
    {
        return;
    }

    model.pose_parameters.reserve(static_cast<std::size_t>(count));
    for (std::int32_t index = 0; index < count; ++index)
    {
        const std::size_t offset = static_cast<std::size_t>(table) + (static_cast<std::size_t>(index) * kStudioPoseParameterSize);
        model.pose_parameters.push_back(StudioPoseParameter{
            .name = read_relative_string(bytes, offset, read_i32_le(bytes, offset)),
            .flags = read_i32_le(bytes, offset + 4),
            .start = read_f32_le(bytes, offset + 8),
            .end = read_f32_le(bytes, offset + 12),
            .loop = read_f32_le(bytes, offset + 16),
        });
    }
}

void read_attachments(std::span<const unsigned char> bytes, StudioModel& model)
{
    const std::int32_t count = read_i32_le(bytes, kStudioNumLocalAttachmentsOffset);
    const std::int32_t table = read_i32_le(bytes, kStudioLocalAttachmentIndexOffset);
    if (!valid_count_range(bytes, table, count, kStudioAttachmentSize))
    {
        return;
    }

    model.attachments.reserve(static_cast<std::size_t>(count));
    for (std::int32_t index = 0; index < count; ++index)
    {
        const std::size_t offset = static_cast<std::size_t>(table) + (static_cast<std::size_t>(index) * kStudioAttachmentSize);
        model.attachments.push_back(StudioAttachment{
            .name = read_relative_string(bytes, offset, read_i32_le(bytes, offset)),
            .flags = read_u32_le(bytes, offset + 4),
            .local_bone = read_i32_le(bytes, offset + 8),
            .local = read_mat3x4(bytes, offset + 12),
        });
    }
}

const StudioSequenceDesc* sequence_at(const StudioModel& model, std::int32_t sequence)
{
    if (sequence < 0 || static_cast<std::size_t>(sequence) >= model.sequences.size())
    {
        return nullptr;
    }
    return &model.sequences[static_cast<std::size_t>(sequence)];
}

const StudioAnimationDesc* first_animation_for_sequence(const StudioModel& model, const StudioSequenceDesc& sequence)
{
    if (!sequence.animation_indices.empty())
    {
        const std::int32_t animation = sequence.animation_indices.front();
        if (animation >= 0 && static_cast<std::size_t>(animation) < model.animations.size())
        {
            return &model.animations[static_cast<std::size_t>(animation)];
        }
    }

    if (!model.animations.empty())
    {
        return &model.animations.front();
    }
    return nullptr;
}

bool crosses_event(float previous_cycle, float current_cycle, float event_cycle, bool looping)
{
    previous_cycle = std::clamp(previous_cycle, 0.0F, 1.0F);
    current_cycle = std::clamp(current_cycle, 0.0F, 1.0F);
    event_cycle = std::clamp(event_cycle, 0.0F, 1.0F);
    if (looping && current_cycle < previous_cycle)
    {
        return event_cycle > previous_cycle || event_cycle <= current_cycle;
    }
    return event_cycle > previous_cycle && event_cycle <= current_cycle;
}
}

std::optional<StudioModel> load_source_studio_model(const SourceAssetStore& assets, const std::filesystem::path& model_path)
{
    const std::filesystem::path normalized = normalize_model_path(model_path);
    const std::optional<std::vector<unsigned char>> bytes = assets.read_binary(normalized);
    if (!bytes)
    {
        return std::nullopt;
    }
    return parse_source_studio_model(*bytes, normalized);
}

std::optional<StudioModel> parse_source_studio_model(std::span<const unsigned char> bytes, std::filesystem::path model_path)
{
    if (bytes.size() < kStudioViewMaxOffset + 12 || read_u32_le(bytes, 0) != kSourceStudioIdent)
    {
        return std::nullopt;
    }

    const std::uint32_t declared_length = read_u32_le(bytes, kStudioLengthOffset);
    if (declared_length > bytes.size())
    {
        return std::nullopt;
    }

    StudioModel model;
    model.path = std::move(model_path);
    model.version = read_i32_le(bytes, 4);
    model.checksum = read_u32_le(bytes, 8);
    model.name = read_zero_string_at(bytes, kStudioNameOffset, kStudioNameSize);
    model.eye_position = read_vec3(bytes, kStudioEyePositionOffset);
    model.illum_position = read_vec3(bytes, kStudioIllumPositionOffset);
    model.hull_bounds = {.mins = read_vec3(bytes, kStudioHullMinOffset), .maxs = read_vec3(bytes, kStudioHullMaxOffset)};
    model.view_bounds = {.mins = read_vec3(bytes, kStudioViewMinOffset), .maxs = read_vec3(bytes, kStudioViewMaxOffset)};
    model.flags = read_i32_le(bytes, kStudioFlagsOffset);
    model.surface_prop = read_zero_string_at(bytes, static_cast<std::size_t>(std::max(0, read_i32_le(bytes, kStudioSurfacePropIndexOffset))));
    model.key_values = read_blob_text(bytes, read_i32_le(bytes, kStudioKeyValueIndexOffset), read_i32_le(bytes, kStudioKeyValueSizeOffset));
    model.mass = read_f32_le(bytes, kStudioMassOffset);
    model.contents = read_i32_le(bytes, kStudioContentsOffset);

    read_bones(bytes, model);
    read_bone_controllers(bytes, model);
    read_hitbox_sets(bytes, model);
    read_animation_descs(bytes, model);
    read_pose_parameters(bytes, model);
    read_sequences(bytes, model);
    read_attachments(bytes, model);
    return model;
}

std::int32_t lookup_sequence(const StudioModel& model, std::string_view label_or_activity)
{
    for (std::size_t index = 0; index < model.sequences.size(); ++index)
    {
        const StudioSequenceDesc& sequence = model.sequences[index];
        if (sequence.label == label_or_activity || sequence.activity_name == label_or_activity)
        {
            return static_cast<std::int32_t>(index);
        }
    }
    return -1;
}

std::int32_t lookup_pose_parameter(const StudioModel& model, std::string_view name)
{
    for (std::size_t index = 0; index < model.pose_parameters.size(); ++index)
    {
        if (model.pose_parameters[index].name == name)
        {
            return static_cast<std::int32_t>(index);
        }
    }
    return -1;
}

std::int32_t select_weighted_sequence(const StudioModel& model, std::string_view activity_name, std::int32_t current_sequence)
{
    if (const StudioSequenceDesc* current = sequence_at(model, current_sequence);
        current != nullptr && current->activity_name == activity_name && current->activity_weight > 0)
    {
        return current_sequence;
    }

    std::int32_t best_sequence = -1;
    std::int32_t best_weight = std::numeric_limits<std::int32_t>::min();
    for (std::size_t index = 0; index < model.sequences.size(); ++index)
    {
        const StudioSequenceDesc& sequence = model.sequences[index];
        if (sequence.activity_name == activity_name && sequence.activity_weight > best_weight)
        {
            best_sequence = static_cast<std::int32_t>(index);
            best_weight = sequence.activity_weight;
        }
    }
    return best_sequence;
}

float source_pose_parameter_to_normalized(const StudioPoseParameter& parameter, float value)
{
    if (parameter.loop > 0.0F)
    {
        value = std::fmod(value - parameter.start, parameter.loop);
        if (value < 0.0F)
        {
            value += parameter.loop;
        }
        value += parameter.start;
    }

    const float range = parameter.end - parameter.start;
    if (std::fabs(range) <= 0.00001F)
    {
        return 0.0F;
    }
    return std::clamp((value - parameter.start) / range, 0.0F, 1.0F);
}

float normalized_pose_parameter_to_source(const StudioPoseParameter& parameter, float normalized)
{
    return parameter.start + ((parameter.end - parameter.start) * std::clamp(normalized, 0.0F, 1.0F));
}

float sequence_duration(const StudioModel& model, std::int32_t sequence_index)
{
    const StudioSequenceDesc* sequence = sequence_at(model, sequence_index);
    if (sequence == nullptr)
    {
        return 0.0F;
    }

    const StudioAnimationDesc* animation = first_animation_for_sequence(model, *sequence);
    const float fps = animation != nullptr ? std::max(animation->fps, 0.001F) : 30.0F;
    const float frame_span = std::max(sequence->last_frame > 0.0F ? sequence->last_frame : 0.0F,
        animation != nullptr ? static_cast<float>(std::max(animation->frame_count - 1, 1)) : 1.0F);
    return frame_span / fps;
}

float sequence_cycle_rate(const StudioModel& model, std::int32_t sequence)
{
    const float duration = sequence_duration(model, sequence);
    if (duration <= 0.00001F)
    {
        return 1.0F;
    }
    return 1.0F / duration;
}

bool sequence_loops(const StudioModel& model, std::int32_t sequence_index)
{
    const StudioSequenceDesc* sequence = sequence_at(model, sequence_index);
    return sequence != nullptr && (sequence->flags & kSourceStudioLooping) != 0;
}

void reset_animation_state(AnimationPlaybackState& state, const StudioModel& model, std::int32_t sequence)
{
    state = {};
    state.pose_parameters.assign(model.pose_parameters.size(), 0.0F);
    state.overlays.assign(kSourceStudioMaxOverlays, {});
    set_sequence(state, model, sequence);
}

void set_sequence(AnimationPlaybackState& state, const StudioModel& model, std::int32_t sequence)
{
    if (model.sequences.empty())
    {
        state.sequence = -1;
        state.sequence_loops = false;
        return;
    }

    if (sequence < 0 || static_cast<std::size_t>(sequence) >= model.sequences.size())
    {
        sequence = 0;
    }

    if (state.sequence != sequence)
    {
        state.previous_cycle = state.cycle;
        state.cycle = 0.0F;
        state.sequence_finished = false;
    }
    state.sequence = sequence;
    state.sequence_loops = sequence_loops(model, sequence);
    state.ground_speed = 0.0F;
}

void advance_animation_state(AnimationPlaybackState& state, const StudioModel& model, float delta_seconds)
{
    if (sequence_at(model, state.sequence) == nullptr)
    {
        return;
    }

    state.fired_events.clear();
    state.previous_cycle = state.cycle;
    const float delta_cycle = std::max(delta_seconds, 0.0F) * sequence_cycle_rate(model, state.sequence) * state.playback_rate;
    float new_cycle = state.cycle + delta_cycle;
    if (new_cycle < 0.0F || new_cycle >= 1.0F)
    {
        if (state.sequence_loops)
        {
            new_cycle -= std::floor(new_cycle);
        }
        else
        {
            new_cycle = std::clamp(new_cycle, 0.0F, 1.0F);
            state.sequence_finished = true;
        }
    }
    state.cycle = new_cycle;
    collect_sequence_events(model, state.sequence, state.previous_cycle, state.cycle, state.sequence_loops, state.fired_events);

    for (AnimationLayer& layer : state.overlays)
    {
        if (!layer.active || layer.sequence < 0 || layer.weight <= 0.0F)
        {
            continue;
        }

        layer.previous_cycle = layer.cycle;
        float layer_cycle = layer.cycle + (std::max(delta_seconds, 0.0F) * sequence_cycle_rate(model, layer.sequence) * layer.playback_rate);
        if (sequence_loops(model, layer.sequence))
        {
            layer_cycle -= std::floor(layer_cycle);
        }
        else
        {
            layer_cycle = std::clamp(layer_cycle, 0.0F, 1.0F);
        }
        layer.cycle = layer_cycle;
    }
}

void collect_sequence_events(
    const StudioModel& model,
    std::int32_t sequence_index,
    float previous_cycle,
    float current_cycle,
    bool looping,
    std::vector<StudioEvent>& events)
{
    const StudioSequenceDesc* sequence = sequence_at(model, sequence_index);
    if (sequence == nullptr)
    {
        return;
    }

    for (const StudioEvent& event : sequence->events)
    {
        if (crosses_event(previous_cycle, current_cycle, event.cycle, looping))
        {
            events.push_back(event);
        }
    }
}

float sample_decoded_channel(const std::vector<float>& values, float frame, bool looping)
{
    if (values.empty())
    {
        return 0.0F;
    }
    if (values.size() == 1)
    {
        return values.front();
    }

    const float max_frame = static_cast<float>(values.size() - 1U);
    if (looping)
    {
        frame = std::fmod(frame, max_frame);
        if (frame < 0.0F)
        {
            frame += max_frame;
        }
    }
    else
    {
        frame = std::clamp(frame, 0.0F, max_frame);
    }

    const std::size_t frame0 = static_cast<std::size_t>(std::floor(frame));
    const std::size_t frame1 = frame0 + 1U < values.size() ? frame0 + 1U : (looping ? 0U : frame0);
    const float fraction = frame - static_cast<float>(frame0);
    return values[frame0] + ((values[frame1] - values[frame0]) * fraction);
}

Vec3 sample_position_track(const StudioBone& bone, const StudioAnimationTrack& track, float frame, bool looping)
{
    if ((track.flags & kStudioAnimRawPosition) != 0)
    {
        return track.delta ? track.raw_position : track.raw_position;
    }

    Vec3 position{
        sample_decoded_channel(track.position_values[0], frame, looping),
        sample_decoded_channel(track.position_values[1], frame, looping),
        sample_decoded_channel(track.position_values[2], frame, looping),
    };
    if (!track.delta)
    {
        position += bone.position;
    }
    return position;
}

Quat sample_rotation_track(const StudioBone& bone, const StudioAnimationTrack& track, float frame, bool looping)
{
    if ((track.flags & kStudioAnimRawRotation48) != 0 || (track.flags & kStudioAnimRawRotation64) != 0)
    {
        return track.raw_rotation;
    }

    Vec3 radians{
        sample_decoded_channel(track.rotation_values[0], frame, looping),
        sample_decoded_channel(track.rotation_values[1], frame, looping),
        sample_decoded_channel(track.rotation_values[2], frame, looping),
    };
    if (!track.delta)
    {
        radians += bone.euler_rotation;
    }
    return quat_from_radians(radians);
}

void apply_sequence_pose(
    const StudioModel& model,
    std::int32_t sequence_index,
    float cycle,
    float weight,
    std::vector<Vec3>& positions,
    std::vector<Quat>& rotations)
{
    const StudioSequenceDesc* sequence = sequence_at(model, sequence_index);
    if (sequence == nullptr)
    {
        return;
    }

    const StudioAnimationDesc* animation = first_animation_for_sequence(model, *sequence);
    if (animation == nullptr || animation->clip.tracks.empty())
    {
        return;
    }

    const float frame = std::clamp(cycle, 0.0F, 1.0F) * static_cast<float>(std::max(animation->frame_count - 1, 0));
    const bool looping = sequence_loops(model, sequence_index);
    weight = std::clamp(weight, 0.0F, 1.0F);
    if (weight <= 0.0F)
    {
        return;
    }

    for (const StudioAnimationTrack& track : animation->clip.tracks)
    {
        if (track.bone < 0 || static_cast<std::size_t>(track.bone) >= model.bones.size())
        {
            continue;
        }

        const std::size_t bone_index = static_cast<std::size_t>(track.bone);
        const StudioBone& bone = model.bones[bone_index];
        if (track.has_position)
        {
            const Vec3 target = sample_position_track(bone, track, frame, looping);
            positions[bone_index] = weight >= 1.0F ? target : lerp(positions[bone_index], target, weight);
        }
        if (track.has_rotation)
        {
            const Quat target = sample_rotation_track(bone, track, frame, looping);
            rotations[bone_index] = weight >= 1.0F ? target : slerp(rotations[bone_index], target, weight);
        }
    }
}

StudioPose evaluate_studio_pose(const StudioModel& model, const AnimationPlaybackState& state)
{
    StudioPose pose;
    pose.positions.reserve(model.bones.size());
    pose.rotations.reserve(model.bones.size());
    pose.bone_to_model.resize(model.bones.size());
    for (const StudioBone& bone : model.bones)
    {
        pose.positions.push_back(bone.position);
        pose.rotations.push_back(bone.rotation);
    }

    apply_sequence_pose(model, state.sequence, state.cycle, 1.0F, pose.positions, pose.rotations);
    for (const AnimationLayer& layer : state.overlays)
    {
        if (layer.active && layer.sequence >= 0 && layer.weight > 0.0F)
        {
            apply_sequence_pose(model, layer.sequence, layer.cycle, layer.weight, pose.positions, pose.rotations);
        }
    }

    for (std::size_t index = 0; index < model.bones.size(); ++index)
    {
        const StudioBone& bone = model.bones[index];
        const Mat3x4 local = matrix_from_quat_position(pose.rotations[index], pose.positions[index]);
        if (bone.parent >= 0 && static_cast<std::size_t>(bone.parent) < pose.bone_to_model.size())
        {
            pose.bone_to_model[index] = concat_transforms(pose.bone_to_model[static_cast<std::size_t>(bone.parent)], local);
        }
        else
        {
            pose.bone_to_model[index] = local;
        }
    }
    return pose;
}
}
