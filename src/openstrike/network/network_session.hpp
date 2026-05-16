#pragma once

#include "openstrike/network/network_address.hpp"
#include "openstrike/network/network_channel.hpp"
#include "openstrike/network/network_messages.hpp"
#include "openstrike/network/network_protocol.hpp"
#include "openstrike/network/network_socket.hpp"
#include "openstrike/network/user_command.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openstrike
{
enum class NetworkConnectionState
{
    Disconnected,
    Connecting,
    Connected
};

enum class NetworkEventType
{
    ServerStarted,
    ServerStopped,
    ClientConnected,
    ClientDisconnected,
    ConnectedToServer,
    DisconnectedFromServer,
    TextReceived,
    UserCommandReceived,
    UserCommandBatchReceived,
    SnapshotReceived,
    SignonStateChanged,
    ServerInfoReceived,
    PacketDropped,
    SocketError
};

struct NetworkEvent
{
    NetworkEventType type = NetworkEventType::PacketDropped;
    NetworkAddress remote;
    std::uint32_t connection_id = 0;
    std::uint64_t tick = 0;
    NetSignonState signon_state = NetSignonState::None;
    std::string text;
    std::vector<unsigned char> payload;
    std::vector<UserCommand> user_commands;
    std::uint8_t num_backup_commands = 0;
    std::uint8_t num_new_commands = 0;
};

struct NetworkStats
{
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_received = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t packets_dropped = 0;
};

struct NetworkPeer
{
    NetworkAddress address;
    std::uint32_t connection_id = 0;
    std::uint32_t next_sequence = 1;
    std::uint32_t last_received_sequence = 0;
    std::uint64_t last_received_tick = 0;
    NetSignonState signon_state = NetSignonState::None;
    NetChannel channel;
};

class NetworkServer
{
public:
    bool start(std::uint16_t port);
    void stop();
    void poll(std::uint64_t tick);

    bool send_text(const NetworkAddress& address, std::string_view text, std::uint64_t tick);
    void broadcast_text(std::string_view text, std::uint64_t tick);
    bool send_snapshot(const NetworkAddress& address, std::span<const unsigned char> payload, std::uint64_t tick);
    void broadcast_snapshot(std::span<const unsigned char> payload, std::uint64_t tick);
    bool send_message(const NetworkAddress& address, NetMessage message, std::uint64_t tick);
    void broadcast_message(NetMessage message, std::uint64_t tick);

    [[nodiscard]] bool is_running() const;
    [[nodiscard]] std::uint16_t local_port() const;
    [[nodiscard]] const std::vector<NetworkPeer>& clients() const;
    [[nodiscard]] const NetworkStats& stats() const;
    [[nodiscard]] std::vector<NetworkEvent> drain_events();

private:
    NetworkPeer* find_peer(const NetworkAddress& address);
    NetworkPeer& find_or_add_peer(const NetworkAddress& address, std::uint64_t tick);
    bool send_packet(NetworkPeer& peer, NetworkMessageType type, std::span<const unsigned char> payload, std::uint64_t tick);
    bool send_channel(NetworkPeer& peer, std::uint64_t tick, bool force = true);
    void handle_channel_messages(NetworkPeer& peer, std::vector<NetMessage> messages, std::uint64_t tick);
    void push_event(NetworkEvent event);

    UdpSocket socket_;
    std::vector<NetworkPeer> clients_;
    std::vector<NetworkEvent> events_;
    NetworkStats stats_;
    std::uint32_t next_connection_id_ = 1;
};

class NetworkClient
{
public:
    bool connect(const NetworkAddress& address, std::uint64_t tick);
    void disconnect(std::uint64_t tick);
    void poll(std::uint64_t tick);
    bool send_text(std::string_view text, std::uint64_t tick);
    bool send_user_command(std::span<const unsigned char> payload, std::uint64_t tick);
    bool send_user_commands(const UserCommandBatch& batch, std::uint64_t tick);
    bool send_message(NetMessage message, std::uint64_t tick);

    [[nodiscard]] NetworkConnectionState state() const;
    [[nodiscard]] NetSignonState signon_state() const;
    [[nodiscard]] bool is_connected() const;
    [[nodiscard]] std::uint16_t local_port() const;
    [[nodiscard]] NetworkAddress remote_address() const;
    [[nodiscard]] std::uint32_t connection_id() const;
    [[nodiscard]] const NetworkStats& stats() const;
    [[nodiscard]] std::vector<NetworkEvent> drain_events();

private:
    bool send_packet(NetworkMessageType type, std::span<const unsigned char> payload, std::uint64_t tick);
    bool send_channel(std::uint64_t tick, bool force = true);
    void process_channel_datagram(std::span<const unsigned char> payload, std::uint64_t tick);
    void handle_channel_messages(std::vector<NetMessage> messages, std::uint64_t tick);
    void push_event(NetworkEvent event);

    UdpSocket socket_;
    NetworkAddress remote_;
    NetworkConnectionState state_ = NetworkConnectionState::Disconnected;
    std::uint32_t connection_id_ = 0;
    std::uint32_t next_sequence_ = 1;
    std::uint32_t last_received_sequence_ = 0;
    NetSignonState signon_state_ = NetSignonState::None;
    NetChannel channel_;
    NetworkStats stats_;
    std::vector<NetworkEvent> events_;
    std::vector<std::vector<unsigned char>> pending_channel_datagrams_;
};

[[nodiscard]] std::string_view to_string(NetworkConnectionState state);
[[nodiscard]] std::string_view to_string(NetworkEventType type);
}
