#pragma once

#include "openstrike/core/math.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace openstrike
{
enum class NetworkFieldType : std::uint8_t
{
    Int32 = 1,
    UInt32 = 2,
    Float = 3,
    Bool = 4,
    Vec3 = 5,
    String = 6,
};

using NetworkFieldValue = std::variant<std::int32_t, std::uint32_t, float, bool, Vec3, std::string>;

struct NetworkFieldDefinition
{
    std::string name;
    NetworkFieldType type = NetworkFieldType::Int32;
};

struct NetworkClassDefinition
{
    std::uint16_t class_id = 0;
    std::string class_name;
    std::vector<NetworkFieldDefinition> fields;
};

struct NetworkEntityState
{
    std::uint32_t entity_id = 0;
    std::uint16_t class_id = 0;
    std::uint32_t serial = 0;
    std::vector<NetworkFieldValue> fields;
};

struct NetworkSnapshot
{
    std::uint64_t tick = 0;
    std::vector<NetworkEntityState> entities;
};

struct NetworkSnapshotDelta
{
    std::uint64_t from_tick = 0;
    std::uint64_t to_tick = 0;
    std::vector<NetworkEntityState> changed_entities;
    std::vector<std::uint32_t> removed_entities;
};

class NetworkClassRegistry
{
public:
    bool register_class(NetworkClassDefinition definition);
    [[nodiscard]] const NetworkClassDefinition* find(std::uint16_t class_id) const;
    [[nodiscard]] const std::vector<NetworkClassDefinition>& classes() const;
    [[nodiscard]] std::uint32_t crc() const;

private:
    std::vector<NetworkClassDefinition> classes_;
};

[[nodiscard]] std::vector<unsigned char> encode_network_snapshot(const NetworkSnapshot& snapshot);
[[nodiscard]] std::optional<NetworkSnapshot> decode_network_snapshot(std::span<const unsigned char> bytes);
[[nodiscard]] NetworkSnapshotDelta make_network_snapshot_delta(const NetworkSnapshot& from, const NetworkSnapshot& to);
[[nodiscard]] std::vector<unsigned char> encode_network_snapshot_delta(const NetworkSnapshotDelta& delta);
[[nodiscard]] std::optional<NetworkSnapshotDelta> decode_network_snapshot_delta(std::span<const unsigned char> bytes);
[[nodiscard]] std::optional<NetworkSnapshot> apply_network_snapshot_delta(const NetworkSnapshot& from, const NetworkSnapshotDelta& delta);
}
