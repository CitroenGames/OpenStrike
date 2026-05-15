#pragma once

#include "openstrike/network/network_address.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike
{
inline constexpr std::uint32_t kOpenStrikeNetworkMagic = 0x544E534FU;
inline constexpr std::uint16_t kOpenStrikeNetworkVersion = 1;
inline constexpr std::uint16_t kOpenStrikeMaxPayloadBytes = 1200;

enum class NetworkMessageType : std::uint8_t
{
    Invalid = 0,
    Connect = 1,
    ConnectAccept = 2,
    Disconnect = 3,
    Ping = 4,
    Pong = 5,
    Text = 6,
    UserCommand = 16,
    Snapshot = 17
};

struct NetworkPacketHeader
{
    NetworkMessageType type = NetworkMessageType::Invalid;
    std::uint8_t flags = 0;
    std::uint32_t sequence = 0;
    std::uint32_t ack = 0;
    std::uint64_t tick = 0;
};

struct NetworkPacket
{
    NetworkPacketHeader header;
    std::vector<unsigned char> payload;
    NetworkAddress sender;
};

[[nodiscard]] std::vector<unsigned char> encode_network_packet(
    NetworkMessageType type,
    std::uint32_t sequence,
    std::uint32_t ack,
    std::uint64_t tick,
    std::span<const unsigned char> payload = {});

[[nodiscard]] std::optional<NetworkPacket> decode_network_packet(std::span<const unsigned char> bytes);
[[nodiscard]] std::vector<unsigned char> make_text_payload(std::string_view text);
[[nodiscard]] std::optional<std::string> read_text_payload(std::span<const unsigned char> payload);
[[nodiscard]] std::vector<unsigned char> make_connect_payload(std::string_view name);
[[nodiscard]] std::optional<std::string> read_connect_payload(std::span<const unsigned char> payload);
[[nodiscard]] std::vector<unsigned char> make_connect_accept_payload(std::uint32_t connection_id);
[[nodiscard]] std::optional<std::uint32_t> read_connect_accept_payload(std::span<const unsigned char> payload);
}
