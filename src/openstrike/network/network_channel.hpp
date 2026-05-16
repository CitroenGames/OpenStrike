#pragma once

#include "openstrike/network/network_messages.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace openstrike
{
enum NetChannelPacketFlags : std::uint8_t
{
    NetChannelPacketReliable = 1U << 0U,
    NetChannelPacketSplit = 1U << 3U,
    NetChannelPacketChoked = 1U << 4U,
};

struct NetChannelConfig
{
    std::size_t max_routable_payload_bytes = 900;
    std::size_t fragment_payload_bytes = 860;
    float data_rate_bytes_per_second = 0.0F;
};

struct NetChannelStats
{
    std::uint32_t outgoing_sequence = 1;
    std::uint32_t incoming_sequence = 0;
    std::uint32_t outgoing_ack = 0;
    std::uint32_t reliable_outgoing_sequence = 0;
    std::uint32_t reliable_incoming_sequence = 0;
    std::uint32_t reliable_ack = 0;
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_received = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t packets_dropped = 0;
    std::uint64_t choked_packets = 0;
    std::size_t reliable_pending_messages = 0;
    std::size_t reliable_in_flight_messages = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(NetMessageGroup::Total)> message_bytes{};
};

struct NetChannelProcessResult
{
    bool accepted = false;
    bool duplicate = false;
    bool needs_ack = false;
    std::uint32_t sequence = 0;
    std::uint32_t ack = 0;
    std::uint8_t choked_packets = 0;
    std::vector<NetMessage> messages;
};

class NetChannel
{
public:
    explicit NetChannel(std::string name = {});

    void reset();
    void set_name(std::string name);
    void set_config(const NetChannelConfig& config);
    void set_data_rate(float bytes_per_second);
    void set_max_routable_payload_size(std::size_t bytes);
    [[nodiscard]] const NetChannelConfig& config() const;

    void queue_message(NetMessage message);
    [[nodiscard]] std::vector<std::vector<unsigned char>> transmit(double now_seconds = 0.0, bool force = true);
    [[nodiscard]] NetChannelProcessResult process_datagram(std::span<const unsigned char> bytes);
    void mark_choked();

    [[nodiscard]] bool has_pending_reliable_data() const;
    [[nodiscard]] bool can_packet(double now_seconds, std::size_t estimated_bytes);
    [[nodiscard]] const NetChannelStats& stats() const;
    [[nodiscard]] std::string_view name() const;

private:
    struct ReliableFlight
    {
        std::uint32_t sequence = 0;
        std::vector<NetMessage> messages;
    };

    struct FragmentAssembly
    {
        std::uint32_t sequence = 0;
        std::uint32_t ack = 0;
        std::uint32_t reliable_sequence = 0;
        std::uint32_t reliable_ack = 0;
        std::uint8_t flags = 0;
        std::uint8_t choked_packets = 0;
        std::uint32_t total_size = 0;
        std::vector<std::vector<unsigned char>> fragments;
        std::vector<bool> received;
    };

    [[nodiscard]] std::vector<std::vector<unsigned char>> encode_payload(std::vector<unsigned char> payload, std::uint8_t flags);
    [[nodiscard]] std::optional<std::vector<unsigned char>> accept_fragment(
        std::uint16_t fragment_id,
        std::uint16_t fragment_count,
        std::uint16_t fragment_index,
        FragmentAssembly assembly,
        std::vector<unsigned char> fragment_payload);
    void acknowledge_remote(std::uint32_t ack);
    void update_message_stats(const std::vector<NetMessage>& messages);
    void update_rate_allowance(double now_seconds);

    std::string name_;
    NetChannelConfig config_;
    NetChannelStats stats_;
    std::vector<NetMessage> reliable_queue_;
    std::vector<NetMessage> unreliable_queue_;
    std::optional<ReliableFlight> reliable_in_flight_;
    std::map<std::uint16_t, FragmentAssembly> fragment_assemblies_;
    std::uint16_t next_fragment_id_ = 1;
    double last_rate_time_ = -1.0;
    double rate_allowance_ = 0.0;
    std::uint8_t pending_choked_packets_ = 0;
};
}
