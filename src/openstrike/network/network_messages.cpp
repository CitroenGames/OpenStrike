#include "openstrike/network/network_messages.hpp"

#include "openstrike/network/network_stream.hpp"

#include <bit>
#include <limits>

namespace openstrike
{
namespace
{
constexpr std::uint32_t kNetMessagesMagic = 0x47534D4EU; // NMSG
constexpr std::uint16_t kNetMessagesVersion = 1;
constexpr std::uint16_t kMaxMessagesPerDatagram = 128;
constexpr std::uint16_t kMaxStringTableEntries = 4096;

void write_bool(NetworkByteWriter& writer, bool value)
{
    writer.write_u8(value ? 1 : 0);
}

bool read_bool(NetworkByteReader& reader, bool& value)
{
    std::uint8_t raw = 0;
    if (!reader.read_u8(raw) || raw > 1)
    {
        return false;
    }
    value = raw != 0;
    return true;
}

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

bool write_payload(NetworkByteWriter& writer, std::span<const unsigned char> payload)
{
    if (payload.size() > std::numeric_limits<std::uint32_t>::max())
    {
        return false;
    }
    writer.write_u32(static_cast<std::uint32_t>(payload.size()));
    writer.write_bytes(payload);
    return true;
}

bool read_payload(NetworkByteReader& reader, std::vector<unsigned char>& payload)
{
    std::uint32_t size = 0;
    if (!reader.read_u32(size) || reader.remaining_bytes().size() < size)
    {
        return false;
    }
    payload.resize(size);
    return reader.read_bytes(payload);
}

bool write_string_table(NetworkByteWriter& writer, const NetStringTableMessage& table, bool include_name)
{
    writer.write_u32(table.table_id);
    writer.write_u32(table.revision);
    if (include_name)
    {
        if (!writer.write_string(table.name))
        {
            return false;
        }
        writer.write_u32(table.max_entries);
    }
    if (table.entries.size() > kMaxStringTableEntries)
    {
        return false;
    }
    writer.write_u16(static_cast<std::uint16_t>(table.entries.size()));
    for (const NetStringTableEntry& entry : table.entries)
    {
        writer.write_u32(entry.index);
        if (!writer.write_string(entry.value) || !write_payload(writer, entry.user_data))
        {
            return false;
        }
    }
    return true;
}

std::optional<NetStringTableMessage> read_string_table(std::span<const unsigned char> payload, bool include_name)
{
    NetworkByteReader reader(payload);
    NetStringTableMessage table;
    std::uint16_t entry_count = 0;
    if (!reader.read_u32(table.table_id) || !reader.read_u32(table.revision))
    {
        return std::nullopt;
    }
    if (include_name && (!reader.read_string(table.name) || !reader.read_u32(table.max_entries)))
    {
        return std::nullopt;
    }
    if (!reader.read_u16(entry_count) || entry_count > kMaxStringTableEntries)
    {
        return std::nullopt;
    }
    table.entries.reserve(entry_count);
    for (std::uint16_t index = 0; index < entry_count; ++index)
    {
        NetStringTableEntry entry;
        if (!reader.read_u32(entry.index) || !reader.read_string(entry.value) || !read_payload(reader, entry.user_data))
        {
            return std::nullopt;
        }
        table.entries.push_back(std::move(entry));
    }
    if (!reader.empty())
    {
        return std::nullopt;
    }
    return table;
}
}

std::string_view to_string(NetSignonState state)
{
    switch (state)
    {
    case NetSignonState::None:
        return "none";
    case NetSignonState::Challenge:
        return "challenge";
    case NetSignonState::Connected:
        return "connected";
    case NetSignonState::New:
        return "new";
    case NetSignonState::Prespawn:
        return "prespawn";
    case NetSignonState::Spawn:
        return "spawn";
    case NetSignonState::Full:
        return "full";
    case NetSignonState::ChangeLevel:
        return "changelevel";
    }
    return "unknown";
}

NetMessageGroup default_message_group(NetMessageKind kind)
{
    switch (kind)
    {
    case NetMessageKind::Move:
        return NetMessageGroup::Move;
    case NetMessageKind::StringCommand:
        return NetMessageGroup::StringCommand;
    case NetMessageKind::SignonState:
    case NetMessageKind::ServerInfo:
    case NetMessageKind::ClientInfo:
    case NetMessageKind::SendTable:
    case NetMessageKind::ClassInfo:
        return NetMessageGroup::Signon;
    case NetMessageKind::CreateStringTable:
    case NetMessageKind::UpdateStringTable:
        return NetMessageGroup::StringTable;
    case NetMessageKind::PacketEntities:
        return NetMessageGroup::Entities;
    case NetMessageKind::TempEntities:
        return NetMessageGroup::TempEntities;
    case NetMessageKind::GameEvent:
        return NetMessageGroup::Events;
    case NetMessageKind::UserMessage:
        return NetMessageGroup::UserMessages;
    default:
        return NetMessageGroup::Generic;
    }
}

bool default_message_reliability(NetMessageKind kind)
{
    switch (kind)
    {
    case NetMessageKind::Move:
    case NetMessageKind::Tick:
    case NetMessageKind::PacketEntities:
    case NetMessageKind::TempEntities:
        return false;
    default:
        return true;
    }
}

std::vector<unsigned char> encode_net_messages(std::span<const NetMessage> messages)
{
    if (messages.size() > kMaxMessagesPerDatagram)
    {
        return {};
    }

    NetworkByteWriter writer;
    writer.write_u32(kNetMessagesMagic);
    writer.write_u16(kNetMessagesVersion);
    writer.write_u16(static_cast<std::uint16_t>(messages.size()));
    for (const NetMessage& message : messages)
    {
        if (message.payload.size() > std::numeric_limits<std::uint32_t>::max())
        {
            return {};
        }
        writer.write_u16(static_cast<std::uint16_t>(message.kind));
        writer.write_u8(static_cast<std::uint8_t>(message.group));
        writer.write_u8(message.reliable ? 1 : 0);
        writer.write_u32(static_cast<std::uint32_t>(message.payload.size()));
        writer.write_bytes(message.payload);
    }
    return writer.take_bytes();
}

std::optional<std::vector<NetMessage>> decode_net_messages(std::span<const unsigned char> bytes)
{
    NetworkByteReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t count = 0;
    if (!reader.read_u32(magic) || !reader.read_u16(version) || !reader.read_u16(count) || magic != kNetMessagesMagic ||
        version != kNetMessagesVersion || count > kMaxMessagesPerDatagram)
    {
        return std::nullopt;
    }

    std::vector<NetMessage> messages;
    messages.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index)
    {
        std::uint16_t kind = 0;
        std::uint8_t group = 0;
        std::uint8_t reliable = 0;
        std::uint32_t payload_size = 0;
        if (!reader.read_u16(kind) || !reader.read_u8(group) || !reader.read_u8(reliable) || !reader.read_u32(payload_size) ||
            reader.remaining_bytes().size() < payload_size)
        {
            return std::nullopt;
        }
        NetMessage message;
        message.kind = static_cast<NetMessageKind>(kind);
        message.group = static_cast<NetMessageGroup>(group);
        message.reliable = reliable != 0;
        message.payload.resize(payload_size);
        if (!reader.read_bytes(message.payload))
        {
            return std::nullopt;
        }
        messages.push_back(std::move(message));
    }
    if (!reader.empty())
    {
        return std::nullopt;
    }
    return messages;
}

NetMessage make_string_command_message(std::string_view command)
{
    NetworkByteWriter writer;
    if (!writer.write_string(command))
    {
        return {};
    }
    return NetMessage{
        .kind = NetMessageKind::StringCommand,
        .group = NetMessageGroup::StringCommand,
        .reliable = true,
        .payload = writer.take_bytes(),
    };
}

std::optional<std::string> read_string_command_message(const NetMessage& message)
{
    if (message.kind != NetMessageKind::StringCommand)
    {
        return std::nullopt;
    }
    NetworkByteReader reader(message.payload);
    std::string command;
    if (!reader.read_string(command) || !reader.empty())
    {
        return std::nullopt;
    }
    return command;
}

NetMessage make_disconnect_message(std::string_view reason)
{
    NetworkByteWriter writer;
    if (!writer.write_string(reason))
    {
        return {};
    }
    return NetMessage{
        .kind = NetMessageKind::Disconnect,
        .group = NetMessageGroup::Generic,
        .reliable = true,
        .payload = writer.take_bytes(),
    };
}

std::optional<std::string> read_disconnect_message(const NetMessage& message)
{
    if (message.kind != NetMessageKind::Disconnect)
    {
        return std::nullopt;
    }
    NetworkByteReader reader(message.payload);
    std::string reason;
    if (!reader.read_string(reason) || !reader.empty())
    {
        return std::nullopt;
    }
    return reason;
}

NetMessage make_tick_message(const NetTickMessage& tick)
{
    NetworkByteWriter writer;
    writer.write_u64(tick.tick);
    writer.write_u32(tick.host_computation_us);
    writer.write_u32(tick.host_computation_stddev_us);
    writer.write_u32(tick.host_framestart_stddev_us);
    return NetMessage{.kind = NetMessageKind::Tick, .group = NetMessageGroup::Generic, .payload = writer.take_bytes()};
}

std::optional<NetTickMessage> read_tick_message(const NetMessage& message)
{
    if (message.kind != NetMessageKind::Tick)
    {
        return std::nullopt;
    }
    NetworkByteReader reader(message.payload);
    NetTickMessage tick;
    if (!reader.read_u64(tick.tick) || !reader.read_u32(tick.host_computation_us) ||
        !reader.read_u32(tick.host_computation_stddev_us) || !reader.read_u32(tick.host_framestart_stddev_us) || !reader.empty())
    {
        return std::nullopt;
    }
    return tick;
}

NetMessage make_signon_state_message(const NetSignonStateMessage& signon)
{
    NetworkByteWriter writer;
    writer.write_u8(static_cast<std::uint8_t>(signon.state));
    writer.write_u32(signon.spawn_count);
    writer.write_u32(signon.num_server_players);
    if (!writer.write_string(signon.map_name))
    {
        return {};
    }
    return NetMessage{
        .kind = NetMessageKind::SignonState,
        .group = NetMessageGroup::Signon,
        .reliable = true,
        .payload = writer.take_bytes(),
    };
}

std::optional<NetSignonStateMessage> read_signon_state_message(const NetMessage& message)
{
    if (message.kind != NetMessageKind::SignonState)
    {
        return std::nullopt;
    }
    NetworkByteReader reader(message.payload);
    std::uint8_t state = 0;
    NetSignonStateMessage signon;
    if (!reader.read_u8(state) || state > static_cast<std::uint8_t>(NetSignonState::ChangeLevel) ||
        !reader.read_u32(signon.spawn_count) || !reader.read_u32(signon.num_server_players) || !reader.read_string(signon.map_name) ||
        !reader.empty())
    {
        return std::nullopt;
    }
    signon.state = static_cast<NetSignonState>(state);
    return signon;
}

NetMessage make_server_info_message(const NetServerInfoMessage& info)
{
    NetworkByteWriter writer;
    write_i32(writer, info.protocol);
    writer.write_u32(info.server_count);
    write_bool(writer, info.dedicated);
    write_i32(writer, info.max_clients);
    write_i32(writer, info.max_classes);
    write_i32(writer, info.player_slot);
    write_f32(writer, info.tick_interval);
    if (!writer.write_string(info.game_dir) || !writer.write_string(info.map_name) || !writer.write_string(info.host_name))
    {
        return {};
    }
    return NetMessage{
        .kind = NetMessageKind::ServerInfo,
        .group = NetMessageGroup::Signon,
        .reliable = true,
        .payload = writer.take_bytes(),
    };
}

std::optional<NetServerInfoMessage> read_server_info_message(const NetMessage& message)
{
    if (message.kind != NetMessageKind::ServerInfo)
    {
        return std::nullopt;
    }
    NetworkByteReader reader(message.payload);
    NetServerInfoMessage info;
    if (!read_i32(reader, info.protocol) || !reader.read_u32(info.server_count) || !read_bool(reader, info.dedicated) ||
        !read_i32(reader, info.max_clients) || !read_i32(reader, info.max_classes) || !read_i32(reader, info.player_slot) ||
        !read_f32(reader, info.tick_interval) || !reader.read_string(info.game_dir) || !reader.read_string(info.map_name) ||
        !reader.read_string(info.host_name) || !reader.empty())
    {
        return std::nullopt;
    }
    return info;
}

NetMessage make_client_info_message(const NetClientInfoMessage& info)
{
    NetworkByteWriter writer;
    writer.write_u32(info.send_table_crc);
    writer.write_u32(info.server_count);
    write_bool(writer, info.hltv);
    write_bool(writer, info.replay);
    writer.write_u32(info.friends_id);
    if (!writer.write_string(info.friends_name))
    {
        return {};
    }
    return NetMessage{
        .kind = NetMessageKind::ClientInfo,
        .group = NetMessageGroup::Signon,
        .reliable = true,
        .payload = writer.take_bytes(),
    };
}

std::optional<NetClientInfoMessage> read_client_info_message(const NetMessage& message)
{
    if (message.kind != NetMessageKind::ClientInfo)
    {
        return std::nullopt;
    }
    NetworkByteReader reader(message.payload);
    NetClientInfoMessage info;
    if (!reader.read_u32(info.send_table_crc) || !reader.read_u32(info.server_count) || !read_bool(reader, info.hltv) ||
        !read_bool(reader, info.replay) || !reader.read_u32(info.friends_id) || !reader.read_string(info.friends_name) ||
        !reader.empty())
    {
        return std::nullopt;
    }
    return info;
}

NetMessage make_move_message(const UserCommandBatch& batch)
{
    return NetMessage{.kind = NetMessageKind::Move, .group = NetMessageGroup::Move, .payload = encode_user_command_batch(batch)};
}

std::optional<UserCommandBatch> read_move_message(const NetMessage& message)
{
    if (message.kind != NetMessageKind::Move)
    {
        return std::nullopt;
    }
    return decode_user_command_batch(message.payload);
}

NetMessage make_create_string_table_message(const NetStringTableMessage& table)
{
    NetworkByteWriter writer;
    if (!write_string_table(writer, table, true))
    {
        return {};
    }
    return NetMessage{
        .kind = NetMessageKind::CreateStringTable,
        .group = NetMessageGroup::StringTable,
        .reliable = true,
        .payload = writer.take_bytes(),
    };
}

NetMessage make_update_string_table_message(const NetStringTableMessage& table)
{
    NetworkByteWriter writer;
    if (!write_string_table(writer, table, false))
    {
        return {};
    }
    return NetMessage{
        .kind = NetMessageKind::UpdateStringTable,
        .group = NetMessageGroup::StringTable,
        .reliable = true,
        .payload = writer.take_bytes(),
    };
}

std::optional<NetStringTableMessage> read_string_table_message(const NetMessage& message)
{
    if (message.kind == NetMessageKind::CreateStringTable)
    {
        return read_string_table(message.payload, true);
    }
    if (message.kind == NetMessageKind::UpdateStringTable)
    {
        return read_string_table(message.payload, false);
    }
    return std::nullopt;
}

NetMessage make_packet_entities_message(const NetPacketEntitiesMessage& packet)
{
    NetworkByteWriter writer;
    write_i32(writer, packet.max_entries);
    write_i32(writer, packet.updated_entries);
    write_bool(writer, packet.is_delta);
    write_bool(writer, packet.update_baseline);
    write_i32(writer, packet.baseline);
    write_i32(writer, packet.delta_from);
    if (!write_payload(writer, packet.entity_data))
    {
        return {};
    }
    return NetMessage{.kind = NetMessageKind::PacketEntities, .group = NetMessageGroup::Entities, .payload = writer.take_bytes()};
}

std::optional<NetPacketEntitiesMessage> read_packet_entities_message(const NetMessage& message)
{
    if (message.kind != NetMessageKind::PacketEntities)
    {
        return std::nullopt;
    }
    NetworkByteReader reader(message.payload);
    NetPacketEntitiesMessage packet;
    if (!read_i32(reader, packet.max_entries) || !read_i32(reader, packet.updated_entries) ||
        !read_bool(reader, packet.is_delta) || !read_bool(reader, packet.update_baseline) || !read_i32(reader, packet.baseline) ||
        !read_i32(reader, packet.delta_from) || !read_payload(reader, packet.entity_data) || !reader.empty())
    {
        return std::nullopt;
    }
    return packet;
}
}
