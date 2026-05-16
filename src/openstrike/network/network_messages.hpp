#pragma once

#include "openstrike/network/user_command.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike
{
enum class NetMessageGroup : std::uint8_t
{
    Generic = 0,
    LocalPlayer,
    OtherPlayers,
    Entities,
    Sounds,
    Events,
    TempEntities,
    UserMessages,
    EntityMessages,
    Voice,
    StringTable,
    Move,
    StringCommand,
    Signon,
    Total
};

enum class NetSignonState : std::uint8_t
{
    None = 0,
    Challenge = 1,
    Connected = 2,
    New = 3,
    Prespawn = 4,
    Spawn = 5,
    Full = 6,
    ChangeLevel = 7
};

enum class NetMessageKind : std::uint16_t
{
    Nop = 0,
    Disconnect = 1,
    File = 2,
    SplitScreenUser = 3,
    Tick = 4,
    StringCommand = 5,
    SetConVar = 6,
    SignonState = 7,
    ClientInfo = 8,
    Move = 9,
    BaselineAck = 11,
    LoadingProgress = 15,
    ServerInfo = 32,
    SendTable = 33,
    ClassInfo = 34,
    CreateStringTable = 35,
    UpdateStringTable = 36,
    SetView = 37,
    FixAngle = 38,
    UserMessage = 39,
    GameEvent = 40,
    PacketEntities = 41,
    TempEntities = 42,
};

struct NetMessage
{
    NetMessageKind kind = NetMessageKind::Nop;
    NetMessageGroup group = NetMessageGroup::Generic;
    bool reliable = false;
    std::vector<unsigned char> payload;
};

struct NetTickMessage
{
    std::uint64_t tick = 0;
    std::uint32_t host_computation_us = 0;
    std::uint32_t host_computation_stddev_us = 0;
    std::uint32_t host_framestart_stddev_us = 0;
};

struct NetSignonStateMessage
{
    NetSignonState state = NetSignonState::None;
    std::uint32_t spawn_count = 0;
    std::uint32_t num_server_players = 0;
    std::string map_name;
};

struct NetServerInfoMessage
{
    std::int32_t protocol = 1;
    std::uint32_t server_count = 0;
    bool dedicated = false;
    std::int32_t max_clients = 0;
    std::int32_t max_classes = 0;
    std::int32_t player_slot = 0;
    float tick_interval = 1.0F / 64.0F;
    std::string game_dir = "openstrike";
    std::string map_name;
    std::string host_name = "OpenStrike";
};

struct NetClientInfoMessage
{
    std::uint32_t send_table_crc = 0;
    std::uint32_t server_count = 0;
    bool hltv = false;
    bool replay = false;
    std::uint32_t friends_id = 0;
    std::string friends_name;
};

struct NetStringTableEntry
{
    std::uint32_t index = 0;
    std::string value;
    std::vector<unsigned char> user_data;
};

struct NetStringTableMessage
{
    std::uint32_t table_id = 0;
    std::string name;
    std::uint32_t max_entries = 0;
    std::uint32_t revision = 0;
    std::vector<NetStringTableEntry> entries;
};

struct NetPacketEntitiesMessage
{
    std::int32_t max_entries = 0;
    std::int32_t updated_entries = 0;
    bool is_delta = false;
    bool update_baseline = false;
    std::int32_t baseline = 0;
    std::int32_t delta_from = -1;
    std::vector<unsigned char> entity_data;
};

[[nodiscard]] std::string_view to_string(NetSignonState state);
[[nodiscard]] NetMessageGroup default_message_group(NetMessageKind kind);
[[nodiscard]] bool default_message_reliability(NetMessageKind kind);
[[nodiscard]] std::vector<unsigned char> encode_net_messages(std::span<const NetMessage> messages);
[[nodiscard]] std::optional<std::vector<NetMessage>> decode_net_messages(std::span<const unsigned char> bytes);

[[nodiscard]] NetMessage make_string_command_message(std::string_view command);
[[nodiscard]] std::optional<std::string> read_string_command_message(const NetMessage& message);
[[nodiscard]] NetMessage make_disconnect_message(std::string_view reason);
[[nodiscard]] std::optional<std::string> read_disconnect_message(const NetMessage& message);
[[nodiscard]] NetMessage make_tick_message(const NetTickMessage& tick);
[[nodiscard]] std::optional<NetTickMessage> read_tick_message(const NetMessage& message);
[[nodiscard]] NetMessage make_signon_state_message(const NetSignonStateMessage& signon);
[[nodiscard]] std::optional<NetSignonStateMessage> read_signon_state_message(const NetMessage& message);
[[nodiscard]] NetMessage make_server_info_message(const NetServerInfoMessage& info);
[[nodiscard]] std::optional<NetServerInfoMessage> read_server_info_message(const NetMessage& message);
[[nodiscard]] NetMessage make_client_info_message(const NetClientInfoMessage& info);
[[nodiscard]] std::optional<NetClientInfoMessage> read_client_info_message(const NetMessage& message);
[[nodiscard]] NetMessage make_move_message(const UserCommandBatch& batch);
[[nodiscard]] std::optional<UserCommandBatch> read_move_message(const NetMessage& message);
[[nodiscard]] NetMessage make_create_string_table_message(const NetStringTableMessage& table);
[[nodiscard]] NetMessage make_update_string_table_message(const NetStringTableMessage& table);
[[nodiscard]] std::optional<NetStringTableMessage> read_string_table_message(const NetMessage& message);
[[nodiscard]] NetMessage make_packet_entities_message(const NetPacketEntitiesMessage& packet);
[[nodiscard]] std::optional<NetPacketEntitiesMessage> read_packet_entities_message(const NetMessage& message);
}
