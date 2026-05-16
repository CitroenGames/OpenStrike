#include "openstrike/network/network_replication.hpp"

#include "openstrike/network/network_stream.hpp"

#include <algorithm>
#include <bit>

namespace openstrike
{
namespace
{
constexpr std::uint32_t kSnapshotMagic = 0x50414E53U; // SNAP
constexpr std::uint32_t kSnapshotDeltaMagic = 0x544C4453U; // SDLT
constexpr std::uint16_t kSnapshotVersion = 1;
constexpr std::uint16_t kMaxNetworkEntities = 2048;
constexpr std::uint16_t kMaxNetworkFields = 128;

void write_i32(NetworkByteWriter& writer, std::int32_t value)
{
    writer.write_u32(static_cast<std::uint32_t>(value));
}

bool read_i32(NetworkByteReader& reader, std::int32_t& value)
{
    std::uint32_t raw = 0;
    if (!reader.read_u32(raw))
    {
        return false;
    }
    value = static_cast<std::int32_t>(raw);
    return true;
}

void write_f32(NetworkByteWriter& writer, float value)
{
    writer.write_u32(std::bit_cast<std::uint32_t>(value));
}

bool read_f32(NetworkByteReader& reader, float& value)
{
    std::uint32_t raw = 0;
    if (!reader.read_u32(raw))
    {
        return false;
    }
    value = std::bit_cast<float>(raw);
    return true;
}

NetworkFieldType value_type(const NetworkFieldValue& value)
{
    return static_cast<NetworkFieldType>(value.index() + 1);
}

bool write_value(NetworkByteWriter& writer, const NetworkFieldValue& value)
{
    writer.write_u8(static_cast<std::uint8_t>(value_type(value)));
    switch (value_type(value))
    {
    case NetworkFieldType::Int32:
        write_i32(writer, std::get<std::int32_t>(value));
        return true;
    case NetworkFieldType::UInt32:
        writer.write_u32(std::get<std::uint32_t>(value));
        return true;
    case NetworkFieldType::Float:
        write_f32(writer, std::get<float>(value));
        return true;
    case NetworkFieldType::Bool:
        writer.write_u8(std::get<bool>(value) ? 1 : 0);
        return true;
    case NetworkFieldType::Vec3:
    {
        const Vec3 vector = std::get<Vec3>(value);
        write_f32(writer, vector.x);
        write_f32(writer, vector.y);
        write_f32(writer, vector.z);
        return true;
    }
    case NetworkFieldType::String:
        return writer.write_string(std::get<std::string>(value));
    }
    return false;
}

bool read_value(NetworkByteReader& reader, NetworkFieldValue& value)
{
    std::uint8_t type = 0;
    if (!reader.read_u8(type))
    {
        return false;
    }
    switch (static_cast<NetworkFieldType>(type))
    {
    case NetworkFieldType::Int32:
    {
        std::int32_t parsed = 0;
        if (!read_i32(reader, parsed))
        {
            return false;
        }
        value = parsed;
        return true;
    }
    case NetworkFieldType::UInt32:
    {
        std::uint32_t parsed = 0;
        if (!reader.read_u32(parsed))
        {
            return false;
        }
        value = parsed;
        return true;
    }
    case NetworkFieldType::Float:
    {
        float parsed = 0.0F;
        if (!read_f32(reader, parsed))
        {
            return false;
        }
        value = parsed;
        return true;
    }
    case NetworkFieldType::Bool:
    {
        std::uint8_t parsed = 0;
        if (!reader.read_u8(parsed) || parsed > 1)
        {
            return false;
        }
        value = parsed != 0;
        return true;
    }
    case NetworkFieldType::Vec3:
    {
        Vec3 parsed;
        if (!read_f32(reader, parsed.x) || !read_f32(reader, parsed.y) || !read_f32(reader, parsed.z))
        {
            return false;
        }
        value = parsed;
        return true;
    }
    case NetworkFieldType::String:
    {
        std::string parsed;
        if (!reader.read_string(parsed))
        {
            return false;
        }
        value = std::move(parsed);
        return true;
    }
    }
    return false;
}

bool write_entity(NetworkByteWriter& writer, const NetworkEntityState& entity)
{
    if (entity.fields.size() > kMaxNetworkFields)
    {
        return false;
    }
    writer.write_u32(entity.entity_id);
    writer.write_u16(entity.class_id);
    writer.write_u32(entity.serial);
    writer.write_u16(static_cast<std::uint16_t>(entity.fields.size()));
    for (const NetworkFieldValue& field : entity.fields)
    {
        if (!write_value(writer, field))
        {
            return false;
        }
    }
    return true;
}

bool read_entity(NetworkByteReader& reader, NetworkEntityState& entity)
{
    std::uint16_t field_count = 0;
    if (!reader.read_u32(entity.entity_id) || !reader.read_u16(entity.class_id) || !reader.read_u32(entity.serial) ||
        !reader.read_u16(field_count) || field_count > kMaxNetworkFields)
    {
        return false;
    }
    entity.fields.reserve(field_count);
    for (std::uint16_t index = 0; index < field_count; ++index)
    {
        NetworkFieldValue value;
        if (!read_value(reader, value))
        {
            return false;
        }
        entity.fields.push_back(std::move(value));
    }
    return true;
}

const NetworkEntityState* find_entity(const NetworkSnapshot& snapshot, std::uint32_t entity_id)
{
    const auto it = std::find_if(snapshot.entities.begin(), snapshot.entities.end(), [&](const NetworkEntityState& entity) {
        return entity.entity_id == entity_id;
    });
    return it == snapshot.entities.end() ? nullptr : &*it;
}

bool value_equal(const NetworkFieldValue& lhs, const NetworkFieldValue& rhs)
{
    if (lhs.index() != rhs.index())
    {
        return false;
    }
    switch (value_type(lhs))
    {
    case NetworkFieldType::Int32:
        return std::get<std::int32_t>(lhs) == std::get<std::int32_t>(rhs);
    case NetworkFieldType::UInt32:
        return std::get<std::uint32_t>(lhs) == std::get<std::uint32_t>(rhs);
    case NetworkFieldType::Float:
        return std::get<float>(lhs) == std::get<float>(rhs);
    case NetworkFieldType::Bool:
        return std::get<bool>(lhs) == std::get<bool>(rhs);
    case NetworkFieldType::Vec3:
    {
        const Vec3 left = std::get<Vec3>(lhs);
        const Vec3 right = std::get<Vec3>(rhs);
        return left.x == right.x && left.y == right.y && left.z == right.z;
    }
    case NetworkFieldType::String:
        return std::get<std::string>(lhs) == std::get<std::string>(rhs);
    }
    return false;
}

bool fields_equal(const std::vector<NetworkFieldValue>& lhs, const std::vector<NetworkFieldValue>& rhs)
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index)
    {
        if (!value_equal(lhs[index], rhs[index]))
        {
            return false;
        }
    }
    return true;
}

std::uint32_t crc_append(std::uint32_t hash, std::string_view text)
{
    for (char ch : text)
    {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 16777619U;
    }
    return hash;
}
}

bool NetworkClassRegistry::register_class(NetworkClassDefinition definition)
{
    if (definition.class_id == 0 || definition.fields.size() > kMaxNetworkFields || find(definition.class_id) != nullptr)
    {
        return false;
    }
    classes_.push_back(std::move(definition));
    std::sort(classes_.begin(), classes_.end(), [](const NetworkClassDefinition& lhs, const NetworkClassDefinition& rhs) {
        return lhs.class_id < rhs.class_id;
    });
    return true;
}

const NetworkClassDefinition* NetworkClassRegistry::find(std::uint16_t class_id) const
{
    const auto it = std::find_if(classes_.begin(), classes_.end(), [&](const NetworkClassDefinition& definition) {
        return definition.class_id == class_id;
    });
    return it == classes_.end() ? nullptr : &*it;
}

const std::vector<NetworkClassDefinition>& NetworkClassRegistry::classes() const
{
    return classes_;
}

std::uint32_t NetworkClassRegistry::crc() const
{
    std::uint32_t hash = 2166136261U;
    for (const NetworkClassDefinition& definition : classes_)
    {
        hash ^= definition.class_id;
        hash *= 16777619U;
        hash = crc_append(hash, definition.class_name);
        for (const NetworkFieldDefinition& field : definition.fields)
        {
            hash = crc_append(hash, field.name);
            hash ^= static_cast<std::uint8_t>(field.type);
            hash *= 16777619U;
        }
    }
    return hash;
}

std::vector<unsigned char> encode_network_snapshot(const NetworkSnapshot& snapshot)
{
    if (snapshot.entities.size() > kMaxNetworkEntities)
    {
        return {};
    }
    NetworkByteWriter writer;
    writer.write_u32(kSnapshotMagic);
    writer.write_u16(kSnapshotVersion);
    writer.write_u64(snapshot.tick);
    writer.write_u16(static_cast<std::uint16_t>(snapshot.entities.size()));
    for (const NetworkEntityState& entity : snapshot.entities)
    {
        if (!write_entity(writer, entity))
        {
            return {};
        }
    }
    return writer.take_bytes();
}

std::optional<NetworkSnapshot> decode_network_snapshot(std::span<const unsigned char> bytes)
{
    NetworkByteReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t entity_count = 0;
    NetworkSnapshot snapshot;
    if (!reader.read_u32(magic) || !reader.read_u16(version) || !reader.read_u64(snapshot.tick) ||
        !reader.read_u16(entity_count) || magic != kSnapshotMagic || version != kSnapshotVersion ||
        entity_count > kMaxNetworkEntities)
    {
        return std::nullopt;
    }
    snapshot.entities.reserve(entity_count);
    for (std::uint16_t index = 0; index < entity_count; ++index)
    {
        NetworkEntityState entity;
        if (!read_entity(reader, entity))
        {
            return std::nullopt;
        }
        snapshot.entities.push_back(std::move(entity));
    }
    if (!reader.empty())
    {
        return std::nullopt;
    }
    return snapshot;
}

NetworkSnapshotDelta make_network_snapshot_delta(const NetworkSnapshot& from, const NetworkSnapshot& to)
{
    NetworkSnapshotDelta delta;
    delta.from_tick = from.tick;
    delta.to_tick = to.tick;
    for (const NetworkEntityState& entity : to.entities)
    {
        const NetworkEntityState* previous = find_entity(from, entity.entity_id);
        if (previous == nullptr || previous->class_id != entity.class_id || previous->serial != entity.serial ||
            !fields_equal(previous->fields, entity.fields))
        {
            delta.changed_entities.push_back(entity);
        }
    }
    for (const NetworkEntityState& entity : from.entities)
    {
        if (find_entity(to, entity.entity_id) == nullptr)
        {
            delta.removed_entities.push_back(entity.entity_id);
        }
    }
    return delta;
}

std::vector<unsigned char> encode_network_snapshot_delta(const NetworkSnapshotDelta& delta)
{
    if (delta.changed_entities.size() > kMaxNetworkEntities || delta.removed_entities.size() > kMaxNetworkEntities)
    {
        return {};
    }
    NetworkByteWriter writer;
    writer.write_u32(kSnapshotDeltaMagic);
    writer.write_u16(kSnapshotVersion);
    writer.write_u64(delta.from_tick);
    writer.write_u64(delta.to_tick);
    writer.write_u16(static_cast<std::uint16_t>(delta.changed_entities.size()));
    for (const NetworkEntityState& entity : delta.changed_entities)
    {
        if (!write_entity(writer, entity))
        {
            return {};
        }
    }
    writer.write_u16(static_cast<std::uint16_t>(delta.removed_entities.size()));
    for (std::uint32_t entity_id : delta.removed_entities)
    {
        writer.write_u32(entity_id);
    }
    return writer.take_bytes();
}

std::optional<NetworkSnapshotDelta> decode_network_snapshot_delta(std::span<const unsigned char> bytes)
{
    NetworkByteReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t changed_count = 0;
    NetworkSnapshotDelta delta;
    if (!reader.read_u32(magic) || !reader.read_u16(version) || !reader.read_u64(delta.from_tick) ||
        !reader.read_u64(delta.to_tick) || !reader.read_u16(changed_count) || magic != kSnapshotDeltaMagic ||
        version != kSnapshotVersion || changed_count > kMaxNetworkEntities)
    {
        return std::nullopt;
    }
    delta.changed_entities.reserve(changed_count);
    for (std::uint16_t index = 0; index < changed_count; ++index)
    {
        NetworkEntityState entity;
        if (!read_entity(reader, entity))
        {
            return std::nullopt;
        }
        delta.changed_entities.push_back(std::move(entity));
    }
    std::uint16_t removed_count = 0;
    if (!reader.read_u16(removed_count) || removed_count > kMaxNetworkEntities)
    {
        return std::nullopt;
    }
    delta.removed_entities.reserve(removed_count);
    for (std::uint16_t index = 0; index < removed_count; ++index)
    {
        std::uint32_t entity_id = 0;
        if (!reader.read_u32(entity_id))
        {
            return std::nullopt;
        }
        delta.removed_entities.push_back(entity_id);
    }
    if (!reader.empty())
    {
        return std::nullopt;
    }
    return delta;
}

std::optional<NetworkSnapshot> apply_network_snapshot_delta(const NetworkSnapshot& from, const NetworkSnapshotDelta& delta)
{
    if (from.tick != delta.from_tick)
    {
        return std::nullopt;
    }

    NetworkSnapshot snapshot = from;
    snapshot.tick = delta.to_tick;
    for (std::uint32_t entity_id : delta.removed_entities)
    {
        snapshot.entities.erase(std::remove_if(snapshot.entities.begin(), snapshot.entities.end(), [&](const NetworkEntityState& entity) {
                                    return entity.entity_id == entity_id;
                                }),
            snapshot.entities.end());
    }
    for (const NetworkEntityState& changed : delta.changed_entities)
    {
        auto it = std::find_if(snapshot.entities.begin(), snapshot.entities.end(), [&](const NetworkEntityState& entity) {
            return entity.entity_id == changed.entity_id;
        });
        if (it == snapshot.entities.end())
        {
            snapshot.entities.push_back(changed);
        }
        else
        {
            *it = changed;
        }
    }
    std::sort(snapshot.entities.begin(), snapshot.entities.end(), [](const NetworkEntityState& lhs, const NetworkEntityState& rhs) {
        return lhs.entity_id < rhs.entity_id;
    });
    return snapshot;
}
}
