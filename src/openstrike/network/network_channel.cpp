#include "openstrike/network/network_channel.hpp"

#include "openstrike/network/network_stream.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <utility>

namespace openstrike
{
namespace
{
constexpr std::uint32_t kNetChannelMagic = 0x4E414843U; // CHAN
constexpr std::uint16_t kNetChannelVersion = 1;
constexpr std::uint16_t kNoFragment = 0;
constexpr std::uint16_t kMaxFragmentsPerPacket = 1024;

struct DecodedChannelDatagram
{
    std::uint32_t sequence = 0;
    std::uint32_t ack = 0;
    std::uint32_t reliable_sequence = 0;
    std::uint32_t reliable_ack = 0;
    std::uint8_t flags = 0;
    std::uint8_t choked_packets = 0;
    std::uint16_t fragment_id = 0;
    std::uint16_t fragment_count = 0;
    std::uint16_t fragment_index = 0;
    std::uint32_t total_payload_size = 0;
    std::vector<unsigned char> payload;
};

bool sequence_after(std::uint32_t sequence, std::uint32_t previous)
{
    return static_cast<std::int32_t>(sequence - previous) > 0;
}

std::vector<unsigned char> encode_datagram(
    std::uint32_t sequence,
    std::uint32_t ack,
    std::uint32_t reliable_sequence,
    std::uint32_t reliable_ack,
    std::uint8_t flags,
    std::uint8_t choked_packets,
    std::uint16_t fragment_id,
    std::uint16_t fragment_count,
    std::uint16_t fragment_index,
    std::uint32_t total_payload_size,
    std::span<const unsigned char> payload)
{
    NetworkByteWriter writer;
    writer.write_u32(kNetChannelMagic);
    writer.write_u16(kNetChannelVersion);
    writer.write_u32(sequence);
    writer.write_u32(ack);
    writer.write_u32(reliable_sequence);
    writer.write_u32(reliable_ack);
    writer.write_u8(flags);
    writer.write_u8(choked_packets);
    writer.write_u16(fragment_id);
    writer.write_u16(fragment_count);
    writer.write_u16(fragment_index);
    writer.write_u32(total_payload_size);
    writer.write_u32(static_cast<std::uint32_t>(payload.size()));
    writer.write_bytes(payload);
    return writer.take_bytes();
}

std::optional<DecodedChannelDatagram> decode_datagram(std::span<const unsigned char> bytes)
{
    NetworkByteReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint32_t payload_size = 0;
    DecodedChannelDatagram datagram;
    if (!reader.read_u32(magic) || !reader.read_u16(version) || !reader.read_u32(datagram.sequence) ||
        !reader.read_u32(datagram.ack) || !reader.read_u32(datagram.reliable_sequence) || !reader.read_u32(datagram.reliable_ack) ||
        !reader.read_u8(datagram.flags) || !reader.read_u8(datagram.choked_packets) || !reader.read_u16(datagram.fragment_id) ||
        !reader.read_u16(datagram.fragment_count) || !reader.read_u16(datagram.fragment_index) ||
        !reader.read_u32(datagram.total_payload_size) || !reader.read_u32(payload_size))
    {
        return std::nullopt;
    }
    if (magic != kNetChannelMagic || version != kNetChannelVersion || reader.remaining_bytes().size() != payload_size)
    {
        return std::nullopt;
    }
    if ((datagram.flags & NetChannelPacketSplit) == 0 &&
        (datagram.fragment_id != kNoFragment || datagram.fragment_count != 0 || datagram.fragment_index != 0))
    {
        return std::nullopt;
    }
    if ((datagram.flags & NetChannelPacketSplit) != 0 &&
        (datagram.fragment_id == kNoFragment || datagram.fragment_count == 0 ||
            datagram.fragment_count > kMaxFragmentsPerPacket || datagram.fragment_index >= datagram.fragment_count ||
            datagram.total_payload_size == 0))
    {
        return std::nullopt;
    }

    datagram.payload.resize(payload_size);
    if (!reader.read_bytes(datagram.payload) || !reader.empty())
    {
        return std::nullopt;
    }
    return datagram;
}

std::size_t group_index(NetMessageGroup group)
{
    const std::size_t index = static_cast<std::size_t>(group);
    return std::min<std::size_t>(index, static_cast<std::size_t>(NetMessageGroup::Total) - 1);
}
}

NetChannel::NetChannel(std::string name)
    : name_(std::move(name))
{
    rate_allowance_ = config_.data_rate_bytes_per_second;
}

void NetChannel::reset()
{
    stats_ = {};
    reliable_queue_.clear();
    unreliable_queue_.clear();
    reliable_in_flight_.reset();
    fragment_assemblies_.clear();
    next_fragment_id_ = 1;
    last_rate_time_ = -1.0;
    rate_allowance_ = config_.data_rate_bytes_per_second;
    pending_choked_packets_ = 0;
}

void NetChannel::set_name(std::string name)
{
    name_ = std::move(name);
}

void NetChannel::set_config(const NetChannelConfig& config)
{
    config_ = config;
    if (config_.fragment_payload_bytes == 0 || config_.fragment_payload_bytes > config_.max_routable_payload_bytes)
    {
        config_.fragment_payload_bytes = config_.max_routable_payload_bytes;
    }
    rate_allowance_ = config_.data_rate_bytes_per_second;
}

void NetChannel::set_data_rate(float bytes_per_second)
{
    config_.data_rate_bytes_per_second = std::max(0.0F, bytes_per_second);
    rate_allowance_ = config_.data_rate_bytes_per_second;
}

void NetChannel::set_max_routable_payload_size(std::size_t bytes)
{
    config_.max_routable_payload_bytes = std::max<std::size_t>(256, bytes);
    config_.fragment_payload_bytes = std::min(config_.fragment_payload_bytes, config_.max_routable_payload_bytes);
}

const NetChannelConfig& NetChannel::config() const
{
    return config_;
}

void NetChannel::queue_message(NetMessage message)
{
    if (message.group == NetMessageGroup::Generic)
    {
        message.group = default_message_group(message.kind);
    }
    message.reliable = message.reliable || default_message_reliability(message.kind);
    if (message.reliable)
    {
        reliable_queue_.push_back(std::move(message));
    }
    else
    {
        unreliable_queue_.push_back(std::move(message));
    }
    stats_.reliable_pending_messages = reliable_queue_.size();
}

std::vector<std::vector<unsigned char>> NetChannel::transmit(double now_seconds, bool force)
{
    if (!reliable_in_flight_ && !reliable_queue_.empty())
    {
        reliable_in_flight_ = ReliableFlight{.sequence = ++stats_.reliable_outgoing_sequence, .messages = std::move(reliable_queue_)};
        reliable_queue_.clear();
    }

    std::vector<NetMessage> messages;
    std::uint8_t flags = 0;
    if (reliable_in_flight_)
    {
        flags |= NetChannelPacketReliable;
        messages.insert(messages.end(), reliable_in_flight_->messages.begin(), reliable_in_flight_->messages.end());
    }
    messages.insert(messages.end(), std::make_move_iterator(unreliable_queue_.begin()), std::make_move_iterator(unreliable_queue_.end()));
    unreliable_queue_.clear();

    if (messages.empty() && !force)
    {
        return {};
    }
    if (pending_choked_packets_ > 0)
    {
        flags |= NetChannelPacketChoked;
    }

    const std::vector<unsigned char> payload = encode_net_messages(messages);
    if (payload.empty())
    {
        return {};
    }

    if (!force && !can_packet(now_seconds, payload.size()))
    {
        mark_choked();
        return {};
    }

    std::vector<std::vector<unsigned char>> datagrams = encode_payload(payload, flags);
    if (datagrams.empty())
    {
        return {};
    }
    update_message_stats(messages);
    for (const std::vector<unsigned char>& datagram : datagrams)
    {
        ++stats_.packets_sent;
        stats_.bytes_sent += datagram.size();
        if (config_.data_rate_bytes_per_second > 0.0F)
        {
            rate_allowance_ = std::max(0.0, rate_allowance_ - static_cast<double>(datagram.size()));
        }
    }
    pending_choked_packets_ = 0;
    stats_.reliable_pending_messages = reliable_queue_.size();
    stats_.reliable_in_flight_messages = reliable_in_flight_ ? reliable_in_flight_->messages.size() : 0;
    return datagrams;
}

NetChannelProcessResult NetChannel::process_datagram(std::span<const unsigned char> bytes)
{
    NetChannelProcessResult result;
    std::optional<DecodedChannelDatagram> decoded = decode_datagram(bytes);
    if (!decoded)
    {
        ++stats_.packets_dropped;
        return result;
    }

    acknowledge_remote(decoded->reliable_ack);
    result.sequence = decoded->sequence;
    result.ack = decoded->ack;
    result.choked_packets = decoded->choked_packets;
    const bool split = (decoded->flags & NetChannelPacketSplit) != 0;
    const bool reliable = (decoded->flags & NetChannelPacketReliable) != 0;
    result.needs_ack = reliable;

    const auto fragment_it = split ? fragment_assemblies_.find(decoded->fragment_id) : fragment_assemblies_.end();
    const bool active_fragment =
        fragment_it != fragment_assemblies_.end() && fragment_it->second.sequence == decoded->sequence;
    const bool new_sequence = sequence_after(decoded->sequence, stats_.incoming_sequence);
    if (!new_sequence && !active_fragment)
    {
        result.duplicate = true;
        if (reliable)
        {
            result.accepted = true;
        }
        else
        {
            ++stats_.packets_dropped;
        }
        return result;
    }

    if (new_sequence)
    {
        if (decoded->sequence > stats_.incoming_sequence + 1)
        {
            stats_.packets_dropped += decoded->sequence - (stats_.incoming_sequence + 1);
        }
        stats_.incoming_sequence = decoded->sequence;
        stats_.outgoing_ack = decoded->sequence;
    }
    ++stats_.packets_received;
    stats_.bytes_received += bytes.size();
    result.accepted = true;

    std::vector<unsigned char> payload;
    if (split)
    {
        FragmentAssembly assembly;
        assembly.sequence = decoded->sequence;
        assembly.ack = decoded->ack;
        assembly.reliable_sequence = decoded->reliable_sequence;
        assembly.reliable_ack = decoded->reliable_ack;
        assembly.flags = decoded->flags;
        assembly.choked_packets = decoded->choked_packets;
        assembly.total_size = decoded->total_payload_size;
        std::optional<std::vector<unsigned char>> completed = accept_fragment(
            decoded->fragment_id,
            decoded->fragment_count,
            decoded->fragment_index,
            std::move(assembly),
            std::move(decoded->payload));
        if (!completed)
        {
            return result;
        }
        payload = std::move(*completed);
    }
    else
    {
        payload = std::move(decoded->payload);
    }

    std::optional<std::vector<NetMessage>> messages = decode_net_messages(payload);
    if (!messages)
    {
        ++stats_.packets_dropped;
        result.accepted = false;
        return result;
    }

    if (reliable)
    {
        if (sequence_after(decoded->reliable_sequence, stats_.reliable_incoming_sequence))
        {
            stats_.reliable_incoming_sequence = decoded->reliable_sequence;
            result.messages = std::move(*messages);
        }
        else
        {
            for (NetMessage& message : *messages)
            {
                if (!message.reliable)
                {
                    result.messages.push_back(std::move(message));
                }
            }
        }
        stats_.reliable_ack = stats_.reliable_incoming_sequence;
    }
    else
    {
        result.messages = std::move(*messages);
    }
    return result;
}

void NetChannel::mark_choked()
{
    ++stats_.choked_packets;
    if (pending_choked_packets_ < std::numeric_limits<std::uint8_t>::max())
    {
        ++pending_choked_packets_;
    }
}

bool NetChannel::has_pending_reliable_data() const
{
    return !reliable_queue_.empty() || reliable_in_flight_.has_value();
}

bool NetChannel::can_packet(double now_seconds, std::size_t estimated_bytes)
{
    if (config_.data_rate_bytes_per_second <= 0.0F)
    {
        return true;
    }
    update_rate_allowance(now_seconds);
    return rate_allowance_ >= static_cast<double>(estimated_bytes);
}

const NetChannelStats& NetChannel::stats() const
{
    return stats_;
}

std::string_view NetChannel::name() const
{
    return name_;
}

std::vector<std::vector<unsigned char>> NetChannel::encode_payload(std::vector<unsigned char> payload, std::uint8_t flags)
{
    std::vector<std::vector<unsigned char>> datagrams;
    const std::uint32_t reliable_sequence = reliable_in_flight_ ? reliable_in_flight_->sequence : stats_.reliable_outgoing_sequence;
    if (payload.size() <= config_.max_routable_payload_bytes)
    {
        const std::uint32_t sequence = stats_.outgoing_sequence++;
        datagrams.push_back(encode_datagram(sequence,
            stats_.outgoing_ack,
            reliable_sequence,
            stats_.reliable_incoming_sequence,
            flags,
            pending_choked_packets_,
            kNoFragment,
            0,
            0,
            static_cast<std::uint32_t>(payload.size()),
            payload));
        return datagrams;
    }

    const std::size_t fragment_size = std::max<std::size_t>(1, std::min(config_.fragment_payload_bytes, config_.max_routable_payload_bytes));
    const std::size_t fragment_count = (payload.size() + fragment_size - 1) / fragment_size;
    if (fragment_count > kMaxFragmentsPerPacket || payload.size() > std::numeric_limits<std::uint32_t>::max())
    {
        return {};
    }

    const std::uint16_t fragment_id = next_fragment_id_++;
    const std::uint8_t fragment_flags = flags | NetChannelPacketSplit;
    const std::uint32_t sequence = stats_.outgoing_sequence++;
    datagrams.reserve(fragment_count);
    for (std::size_t index = 0; index < fragment_count; ++index)
    {
        const std::size_t begin = index * fragment_size;
        const std::size_t count = std::min(fragment_size, payload.size() - begin);
        datagrams.push_back(encode_datagram(sequence,
            stats_.outgoing_ack,
            reliable_sequence,
            stats_.reliable_incoming_sequence,
            fragment_flags,
            pending_choked_packets_,
            fragment_id,
            static_cast<std::uint16_t>(fragment_count),
            static_cast<std::uint16_t>(index),
            static_cast<std::uint32_t>(payload.size()),
            std::span<const unsigned char>(payload.data() + begin, count)));
    }
    return datagrams;
}

std::optional<std::vector<unsigned char>> NetChannel::accept_fragment(
    std::uint16_t fragment_id,
    std::uint16_t fragment_count,
    std::uint16_t fragment_index,
    FragmentAssembly assembly,
    std::vector<unsigned char> fragment_payload)
{
    if (fragment_count == 0 || fragment_count > kMaxFragmentsPerPacket ||
        assembly.total_size > static_cast<std::uint32_t>(fragment_count) * config_.fragment_payload_bytes)
    {
        ++stats_.packets_dropped;
        return std::nullopt;
    }

    FragmentAssembly& stored = fragment_assemblies_[fragment_id];
    if (stored.fragments.empty())
    {
        stored = std::move(assembly);
        stored.fragments.resize(fragment_count);
        stored.received.assign(fragment_count, false);
    }
    if (stored.fragments.size() != fragment_count || fragment_index >= stored.fragments.size())
    {
        fragment_assemblies_.erase(fragment_id);
        ++stats_.packets_dropped;
        return std::nullopt;
    }
    if (stored.received[fragment_index])
    {
        return std::nullopt;
    }
    stored.fragments[fragment_index] = std::move(fragment_payload);
    stored.received[fragment_index] = true;
    if (!std::all_of(stored.received.begin(), stored.received.end(), [](bool received) { return received; }))
    {
        return std::nullopt;
    }

    const std::size_t total_size = std::accumulate(stored.fragments.begin(), stored.fragments.end(), std::size_t{0}, [](std::size_t sum, const auto& fragment) {
        return sum + fragment.size();
    });
    if (total_size != stored.total_size)
    {
        fragment_assemblies_.erase(fragment_id);
        ++stats_.packets_dropped;
        return std::nullopt;
    }

    std::vector<unsigned char> payload;
    payload.reserve(total_size);
    for (const std::vector<unsigned char>& fragment : stored.fragments)
    {
        payload.insert(payload.end(), fragment.begin(), fragment.end());
    }
    fragment_assemblies_.erase(fragment_id);
    return payload;
}

void NetChannel::acknowledge_remote(std::uint32_t ack)
{
    if (reliable_in_flight_ && ack >= reliable_in_flight_->sequence)
    {
        reliable_in_flight_.reset();
    }
    stats_.reliable_in_flight_messages = reliable_in_flight_ ? reliable_in_flight_->messages.size() : 0;
}

void NetChannel::update_message_stats(const std::vector<NetMessage>& messages)
{
    for (const NetMessage& message : messages)
    {
        stats_.message_bytes[group_index(message.group)] += message.payload.size();
    }
}

void NetChannel::update_rate_allowance(double now_seconds)
{
    if (config_.data_rate_bytes_per_second <= 0.0F)
    {
        return;
    }
    if (last_rate_time_ < 0.0)
    {
        last_rate_time_ = now_seconds;
        rate_allowance_ = config_.data_rate_bytes_per_second;
        return;
    }
    const double elapsed = std::max(0.0, now_seconds - last_rate_time_);
    last_rate_time_ = now_seconds;
    rate_allowance_ = std::min<double>(
        config_.data_rate_bytes_per_second,
        rate_allowance_ + elapsed * static_cast<double>(config_.data_rate_bytes_per_second));
}
}
