#include "openstrike/network/network_protocol.hpp"

#include "openstrike/network/network_stream.hpp"

namespace openstrike
{
std::vector<unsigned char> encode_network_packet(
    NetworkMessageType type,
    std::uint32_t sequence,
    std::uint32_t ack,
    std::uint64_t tick,
    std::span<const unsigned char> payload)
{
    if (payload.size() > kOpenStrikeMaxPayloadBytes)
    {
        return {};
    }

    NetworkByteWriter writer;
    writer.write_u32(kOpenStrikeNetworkMagic);
    writer.write_u16(kOpenStrikeNetworkVersion);
    writer.write_u8(static_cast<std::uint8_t>(type));
    writer.write_u8(0);
    writer.write_u32(sequence);
    writer.write_u32(ack);
    writer.write_u64(tick);
    writer.write_u16(static_cast<std::uint16_t>(payload.size()));
    writer.write_bytes(payload);
    return writer.take_bytes();
}

std::optional<NetworkPacket> decode_network_packet(std::span<const unsigned char> bytes)
{
    NetworkByteReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint8_t type = 0;
    std::uint8_t flags = 0;
    std::uint32_t sequence = 0;
    std::uint32_t ack = 0;
    std::uint64_t tick = 0;
    std::uint16_t payload_size = 0;

    if (!reader.read_u32(magic) || !reader.read_u16(version) || !reader.read_u8(type) || !reader.read_u8(flags) ||
        !reader.read_u32(sequence) || !reader.read_u32(ack) || !reader.read_u64(tick) || !reader.read_u16(payload_size))
    {
        return std::nullopt;
    }
    if (magic != kOpenStrikeNetworkMagic || version != kOpenStrikeNetworkVersion || payload_size > kOpenStrikeMaxPayloadBytes)
    {
        return std::nullopt;
    }
    if (reader.remaining_bytes().size() != payload_size)
    {
        return std::nullopt;
    }

    NetworkPacket packet;
    packet.header.type = static_cast<NetworkMessageType>(type);
    packet.header.flags = flags;
    packet.header.sequence = sequence;
    packet.header.ack = ack;
    packet.header.tick = tick;
    const std::span<const unsigned char> payload = reader.remaining_bytes();
    packet.payload.assign(payload.begin(), payload.end());
    return packet;
}

std::vector<unsigned char> make_text_payload(std::string_view text)
{
    NetworkByteWriter writer;
    if (!writer.write_string(text))
    {
        return {};
    }
    return writer.take_bytes();
}

std::optional<std::string> read_text_payload(std::span<const unsigned char> payload)
{
    NetworkByteReader reader(payload);
    std::string text;
    if (!reader.read_string(text) || !reader.empty())
    {
        return std::nullopt;
    }
    return text;
}

std::vector<unsigned char> make_connect_payload(std::string_view name)
{
    return make_text_payload(name);
}

std::optional<std::string> read_connect_payload(std::span<const unsigned char> payload)
{
    return read_text_payload(payload);
}

std::vector<unsigned char> make_connect_accept_payload(std::uint32_t connection_id)
{
    NetworkByteWriter writer;
    writer.write_u32(connection_id);
    return writer.take_bytes();
}

std::optional<std::uint32_t> read_connect_accept_payload(std::span<const unsigned char> payload)
{
    NetworkByteReader reader(payload);
    std::uint32_t connection_id = 0;
    if (!reader.read_u32(connection_id) || !reader.empty())
    {
        return std::nullopt;
    }
    return connection_id;
}
}
