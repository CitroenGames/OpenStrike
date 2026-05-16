#include "openstrike/network/network_session.hpp"

#include <algorithm>
#include <utility>

namespace openstrike
{
namespace
{
std::span<const unsigned char> as_payload(const std::vector<unsigned char>& bytes)
{
    return std::span<const unsigned char>(bytes.data(), bytes.size());
}

constexpr std::size_t kMaxPendingClientChannelDatagrams = 32;
}

bool NetworkServer::start(std::uint16_t port)
{
    stop();
    stats_ = {};
    if (!socket_.open(port))
    {
        push_event(NetworkEvent{
            .type = NetworkEventType::SocketError,
            .text = socket_.last_error(),
        });
        return false;
    }

    push_event(NetworkEvent{
        .type = NetworkEventType::ServerStarted,
        .remote = NetworkAddress::any(socket_.local_port()),
    });
    return true;
}

void NetworkServer::stop()
{
    if (!socket_.is_open())
    {
        return;
    }

    for (NetworkPeer& peer : clients_)
    {
        send_packet(peer, NetworkMessageType::Disconnect, {}, peer.last_received_tick);
    }
    socket_.close();
    clients_.clear();
    push_event(NetworkEvent{.type = NetworkEventType::ServerStopped});
}

void NetworkServer::poll(std::uint64_t tick)
{
    if (!socket_.is_open())
    {
        return;
    }

    for (;;)
    {
        std::optional<UdpPacket> incoming = socket_.receive();
        if (!incoming)
        {
            break;
        }

        ++stats_.packets_received;
        stats_.bytes_received += incoming->bytes.size();
        std::optional<NetworkPacket> decoded = decode_network_packet(incoming->bytes);
        if (!decoded)
        {
            ++stats_.packets_dropped;
            push_event(NetworkEvent{
                .type = NetworkEventType::PacketDropped,
                .remote = incoming->sender,
            });
            continue;
        }

        NetworkPacket& packet = *decoded;
        packet.sender = incoming->sender;
        NetworkPeer* peer = find_peer(packet.sender);
        if (packet.header.type == NetworkMessageType::Connect)
        {
            peer = &find_or_add_peer(packet.sender, tick);
            peer->last_received_sequence = packet.header.sequence;
            peer->last_received_tick = tick;
            peer->signon_state = NetSignonState::Connected;
            const std::vector<unsigned char> payload = make_connect_accept_payload(peer->connection_id);
            send_packet(*peer, NetworkMessageType::ConnectAccept, as_payload(payload), tick);
            const std::uint32_t player_count = static_cast<std::uint32_t>(clients_.size());
            peer->channel.queue_message(make_signon_state_message(NetSignonStateMessage{
                .state = NetSignonState::Connected,
                .spawn_count = 1,
                .num_server_players = player_count,
            }));
            peer->channel.queue_message(make_server_info_message(NetServerInfoMessage{
                .server_count = 1,
                .dedicated = true,
                .max_clients = 64,
                .max_classes = 1,
                .player_slot = static_cast<std::int32_t>(peer->connection_id),
            }));
            peer->channel.queue_message(make_signon_state_message(NetSignonStateMessage{
                .state = NetSignonState::New,
                .spawn_count = 1,
                .num_server_players = player_count,
            }));
            peer->channel.queue_message(make_signon_state_message(NetSignonStateMessage{
                .state = NetSignonState::Prespawn,
                .spawn_count = 1,
                .num_server_players = player_count,
            }));
            peer->channel.queue_message(make_signon_state_message(NetSignonStateMessage{
                .state = NetSignonState::Spawn,
                .spawn_count = 1,
                .num_server_players = player_count,
            }));
            peer->channel.queue_message(make_signon_state_message(NetSignonStateMessage{
                .state = NetSignonState::Full,
                .spawn_count = 1,
                .num_server_players = player_count,
            }));
            send_channel(*peer, tick);
            continue;
        }

        if (peer == nullptr)
        {
            ++stats_.packets_dropped;
            push_event(NetworkEvent{
                .type = NetworkEventType::PacketDropped,
                .remote = packet.sender,
            });
            continue;
        }

        peer->last_received_sequence = packet.header.sequence;
        peer->last_received_tick = tick;
        switch (packet.header.type)
        {
        case NetworkMessageType::Disconnect:
            push_event(NetworkEvent{
                .type = NetworkEventType::ClientDisconnected,
                .remote = peer->address,
                .connection_id = peer->connection_id,
            });
            clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [&](const NetworkPeer& candidate) {
                               return candidate.address == packet.sender;
                           }),
                clients_.end());
            break;
        case NetworkMessageType::Ping:
            send_packet(*peer, NetworkMessageType::Pong, packet.payload, tick);
            break;
        case NetworkMessageType::Text:
            if (const std::optional<std::string> text = read_text_payload(packet.payload))
            {
                push_event(NetworkEvent{
                    .type = NetworkEventType::TextReceived,
                    .remote = peer->address,
                    .connection_id = peer->connection_id,
                    .text = *text,
                });
            }
            break;
        case NetworkMessageType::UserCommand:
            push_event(NetworkEvent{
                .type = NetworkEventType::UserCommandReceived,
                .remote = peer->address,
                .connection_id = peer->connection_id,
                .tick = packet.header.tick,
                .payload = std::move(packet.payload),
            });
            break;
        case NetworkMessageType::Channel:
        {
            NetChannelProcessResult channel_result = peer->channel.process_datagram(packet.payload);
            if (!channel_result.accepted)
            {
                ++stats_.packets_dropped;
                push_event(NetworkEvent{
                    .type = NetworkEventType::PacketDropped,
                    .remote = peer->address,
                });
                break;
            }
            const bool should_reply =
                channel_result.needs_ack || !channel_result.messages.empty() || peer->channel.has_pending_reliable_data();
            handle_channel_messages(*peer, std::move(channel_result.messages), tick);
            if (should_reply)
            {
                send_channel(*peer, tick);
            }
            break;
        }
        default:
            break;
        }
    }
}

bool NetworkServer::send_text(const NetworkAddress& address, std::string_view text, std::uint64_t tick)
{
    NetworkPeer* peer = find_peer(address);
    if (peer == nullptr)
    {
        return false;
    }

    peer->channel.queue_message(make_string_command_message(text));
    return send_channel(*peer, tick);
}

void NetworkServer::broadcast_text(std::string_view text, std::uint64_t tick)
{
    for (NetworkPeer& peer : clients_)
    {
        peer.channel.queue_message(make_string_command_message(text));
        send_channel(peer, tick);
    }
}

bool NetworkServer::send_snapshot(const NetworkAddress& address, std::span<const unsigned char> payload, std::uint64_t tick)
{
    NetworkPeer* peer = find_peer(address);
    if (peer == nullptr)
    {
        return false;
    }

    peer->channel.queue_message(make_packet_entities_message(NetPacketEntitiesMessage{
        .max_entries = 2048,
        .updated_entries = 1,
        .entity_data = std::vector<unsigned char>(payload.begin(), payload.end()),
    }));
    return send_channel(*peer, tick);
}

void NetworkServer::broadcast_snapshot(std::span<const unsigned char> payload, std::uint64_t tick)
{
    for (NetworkPeer& peer : clients_)
    {
        peer.channel.queue_message(make_packet_entities_message(NetPacketEntitiesMessage{
            .max_entries = 2048,
            .updated_entries = 1,
            .entity_data = std::vector<unsigned char>(payload.begin(), payload.end()),
        }));
        send_channel(peer, tick);
    }
}

bool NetworkServer::send_message(const NetworkAddress& address, NetMessage message, std::uint64_t tick)
{
    NetworkPeer* peer = find_peer(address);
    if (peer == nullptr)
    {
        return false;
    }
    peer->channel.queue_message(std::move(message));
    return send_channel(*peer, tick);
}

void NetworkServer::broadcast_message(NetMessage message, std::uint64_t tick)
{
    for (NetworkPeer& peer : clients_)
    {
        peer.channel.queue_message(message);
        send_channel(peer, tick);
    }
}

bool NetworkServer::is_running() const
{
    return socket_.is_open();
}

std::uint16_t NetworkServer::local_port() const
{
    return socket_.local_port();
}

const std::vector<NetworkPeer>& NetworkServer::clients() const
{
    return clients_;
}

const NetworkStats& NetworkServer::stats() const
{
    return stats_;
}

std::vector<NetworkEvent> NetworkServer::drain_events()
{
    return std::exchange(events_, {});
}

NetworkPeer* NetworkServer::find_peer(const NetworkAddress& address)
{
    const auto it = std::find_if(clients_.begin(), clients_.end(), [&](const NetworkPeer& peer) {
        return peer.address == address;
    });
    return it == clients_.end() ? nullptr : &*it;
}

NetworkPeer& NetworkServer::find_or_add_peer(const NetworkAddress& address, std::uint64_t tick)
{
    if (NetworkPeer* peer = find_peer(address))
    {
        return *peer;
    }

    NetworkPeer peer;
    peer.address = address;
    peer.connection_id = next_connection_id_++;
    peer.last_received_tick = tick;
    peer.channel.set_name("server:" + address.to_string());
    clients_.push_back(peer);
    NetworkPeer& stored = clients_.back();
    push_event(NetworkEvent{
        .type = NetworkEventType::ClientConnected,
        .remote = stored.address,
        .connection_id = stored.connection_id,
    });
    return stored;
}

bool NetworkServer::send_packet(NetworkPeer& peer, NetworkMessageType type, std::span<const unsigned char> payload, std::uint64_t tick)
{
    std::vector<unsigned char> bytes = encode_network_packet(type, peer.next_sequence++, peer.last_received_sequence, tick, payload);
    if (bytes.empty())
    {
        return false;
    }

    if (!socket_.send_to(peer.address, bytes))
    {
        push_event(NetworkEvent{
            .type = NetworkEventType::SocketError,
            .remote = peer.address,
            .text = socket_.last_error(),
        });
        return false;
    }

    ++stats_.packets_sent;
    stats_.bytes_sent += bytes.size();
    return true;
}

bool NetworkServer::send_channel(NetworkPeer& peer, std::uint64_t tick, bool force)
{
    bool sent_any = false;
    const std::vector<std::vector<unsigned char>> datagrams = peer.channel.transmit(static_cast<double>(tick), force);
    for (const std::vector<unsigned char>& datagram : datagrams)
    {
        sent_any = send_packet(peer, NetworkMessageType::Channel, datagram, tick) || sent_any;
    }
    return sent_any;
}

void NetworkServer::handle_channel_messages(NetworkPeer& peer, std::vector<NetMessage> messages, std::uint64_t tick)
{
    for (const NetMessage& message : messages)
    {
        if (message.kind == NetMessageKind::StringCommand)
        {
            if (const std::optional<std::string> text = read_string_command_message(message))
            {
                push_event(NetworkEvent{
                    .type = NetworkEventType::TextReceived,
                    .remote = peer.address,
                    .connection_id = peer.connection_id,
                    .tick = tick,
                    .text = *text,
                });
            }
            continue;
        }
        if (message.kind == NetMessageKind::Move)
        {
            if (const std::optional<UserCommandBatch> batch = read_move_message(message))
            {
                push_event(NetworkEvent{
                    .type = NetworkEventType::UserCommandBatchReceived,
                    .remote = peer.address,
                    .connection_id = peer.connection_id,
                    .tick = tick,
                    .payload = message.payload,
                    .user_commands = batch->commands,
                    .num_backup_commands = batch->num_backup_commands,
                    .num_new_commands = batch->num_new_commands,
                });
            }
            else
            {
                push_event(NetworkEvent{
                    .type = NetworkEventType::UserCommandReceived,
                    .remote = peer.address,
                    .connection_id = peer.connection_id,
                    .tick = tick,
                    .payload = message.payload,
                });
            }
            continue;
        }
        if (message.kind == NetMessageKind::PacketEntities)
        {
            if (const std::optional<NetPacketEntitiesMessage> packet = read_packet_entities_message(message))
            {
                push_event(NetworkEvent{
                    .type = NetworkEventType::SnapshotReceived,
                    .remote = peer.address,
                    .connection_id = peer.connection_id,
                    .tick = tick,
                    .payload = packet->entity_data,
                });
            }
            continue;
        }
        if (message.kind == NetMessageKind::SignonState)
        {
            if (const std::optional<NetSignonStateMessage> signon = read_signon_state_message(message))
            {
                peer.signon_state = signon->state;
                push_event(NetworkEvent{
                    .type = NetworkEventType::SignonStateChanged,
                    .remote = peer.address,
                    .connection_id = peer.connection_id,
                    .tick = tick,
                    .signon_state = signon->state,
                    .text = std::string(to_string(signon->state)),
                });
            }
            continue;
        }
        if (message.kind == NetMessageKind::ServerInfo)
        {
            if (const std::optional<NetServerInfoMessage> info = read_server_info_message(message))
            {
                push_event(NetworkEvent{
                    .type = NetworkEventType::ServerInfoReceived,
                    .remote = peer.address,
                    .connection_id = peer.connection_id,
                    .tick = tick,
                    .text = info->map_name,
                });
            }
        }
    }
}

void NetworkServer::push_event(NetworkEvent event)
{
    events_.push_back(std::move(event));
}

bool NetworkClient::connect(const NetworkAddress& address, std::uint64_t tick)
{
    if (!address.valid)
    {
        return false;
    }

    disconnect(tick);
    if (!socket_.open(0))
    {
        push_event(NetworkEvent{
            .type = NetworkEventType::SocketError,
            .remote = address,
            .text = socket_.last_error(),
        });
        return false;
    }

    remote_ = address;
    state_ = NetworkConnectionState::Connecting;
    signon_state_ = NetSignonState::Challenge;
    channel_.reset();
    channel_.set_name("client:" + address.to_string());
    pending_channel_datagrams_.clear();
    const std::vector<unsigned char> payload = make_connect_payload("OpenStrike");
    return send_packet(NetworkMessageType::Connect, as_payload(payload), tick);
}

void NetworkClient::disconnect(std::uint64_t tick)
{
    if (socket_.is_open() && remote_.valid)
    {
        send_packet(NetworkMessageType::Disconnect, {}, tick);
    }
    socket_.close();
    if (state_ != NetworkConnectionState::Disconnected)
    {
        push_event(NetworkEvent{
            .type = NetworkEventType::DisconnectedFromServer,
            .remote = remote_,
            .connection_id = connection_id_,
        });
    }
    remote_ = {};
    state_ = NetworkConnectionState::Disconnected;
    connection_id_ = 0;
    next_sequence_ = 1;
    last_received_sequence_ = 0;
    signon_state_ = NetSignonState::None;
    channel_.reset();
    pending_channel_datagrams_.clear();
}

void NetworkClient::poll(std::uint64_t tick)
{
    if (!socket_.is_open())
    {
        return;
    }

    for (;;)
    {
        std::optional<UdpPacket> incoming = socket_.receive();
        if (!incoming)
        {
            break;
        }

        ++stats_.packets_received;
        stats_.bytes_received += incoming->bytes.size();
        if (incoming->sender != remote_)
        {
            ++stats_.packets_dropped;
            continue;
        }

        std::optional<NetworkPacket> decoded = decode_network_packet(incoming->bytes);
        if (!decoded)
        {
            ++stats_.packets_dropped;
            push_event(NetworkEvent{
                .type = NetworkEventType::PacketDropped,
                .remote = incoming->sender,
            });
            continue;
        }

        NetworkPacket& packet = *decoded;
        last_received_sequence_ = packet.header.sequence;
        switch (packet.header.type)
        {
        case NetworkMessageType::ConnectAccept:
            if (const std::optional<std::uint32_t> accepted_id = read_connect_accept_payload(packet.payload))
            {
                connection_id_ = *accepted_id;
                state_ = NetworkConnectionState::Connected;
                signon_state_ = NetSignonState::Connected;
                push_event(NetworkEvent{
                    .type = NetworkEventType::ConnectedToServer,
                    .remote = remote_,
                    .connection_id = connection_id_,
                });
                push_event(NetworkEvent{
                    .type = NetworkEventType::SignonStateChanged,
                    .remote = remote_,
                    .connection_id = connection_id_,
                    .tick = tick,
                    .signon_state = signon_state_,
                    .text = std::string(to_string(signon_state_)),
                });
                channel_.queue_message(make_client_info_message(NetClientInfoMessage{
                    .server_count = 1,
                    .friends_id = connection_id_,
                    .friends_name = "OpenStrike",
                }));
                channel_.queue_message(make_signon_state_message(NetSignonStateMessage{
                    .state = NetSignonState::Connected,
                    .spawn_count = 1,
                    .num_server_players = 0,
                }));
                send_channel(tick);
                std::vector<std::vector<unsigned char>> pending = std::exchange(pending_channel_datagrams_, {});
                for (const std::vector<unsigned char>& channel_payload : pending)
                {
                    process_channel_datagram(channel_payload, tick);
                }
            }
            break;
        case NetworkMessageType::Disconnect:
            disconnect(tick);
            break;
        case NetworkMessageType::Pong:
            break;
        case NetworkMessageType::Text:
            if (const std::optional<std::string> text = read_text_payload(packet.payload))
            {
                push_event(NetworkEvent{
                    .type = NetworkEventType::TextReceived,
                    .remote = remote_,
                    .connection_id = connection_id_,
                    .text = *text,
                });
            }
            break;
        case NetworkMessageType::Snapshot:
            push_event(NetworkEvent{
                .type = NetworkEventType::SnapshotReceived,
                .remote = remote_,
                .connection_id = connection_id_,
                .tick = packet.header.tick,
                .payload = std::move(packet.payload),
            });
            break;
        case NetworkMessageType::Channel:
        {
            if (state_ != NetworkConnectionState::Connected)
            {
                if (state_ == NetworkConnectionState::Connecting &&
                    pending_channel_datagrams_.size() < kMaxPendingClientChannelDatagrams)
                {
                    pending_channel_datagrams_.push_back(std::move(packet.payload));
                }
                else
                {
                    ++stats_.packets_dropped;
                }
                break;
            }
            process_channel_datagram(packet.payload, tick);
            break;
        }
        default:
            break;
        }
    }
}

bool NetworkClient::send_text(std::string_view text, std::uint64_t tick)
{
    if (state_ != NetworkConnectionState::Connected)
    {
        return false;
    }

    channel_.queue_message(make_string_command_message(text));
    return send_channel(tick);
}

bool NetworkClient::send_user_command(std::span<const unsigned char> payload, std::uint64_t tick)
{
    if (state_ != NetworkConnectionState::Connected)
    {
        return false;
    }

    return send_packet(NetworkMessageType::UserCommand, payload, tick);
}

bool NetworkClient::send_user_commands(const UserCommandBatch& batch, std::uint64_t tick)
{
    if (state_ != NetworkConnectionState::Connected)
    {
        return false;
    }

    channel_.queue_message(make_move_message(batch));
    return send_channel(tick);
}

bool NetworkClient::send_message(NetMessage message, std::uint64_t tick)
{
    if (state_ != NetworkConnectionState::Connected)
    {
        return false;
    }

    channel_.queue_message(std::move(message));
    return send_channel(tick);
}

NetworkConnectionState NetworkClient::state() const
{
    return state_;
}

NetSignonState NetworkClient::signon_state() const
{
    return signon_state_;
}

bool NetworkClient::is_connected() const
{
    return state_ == NetworkConnectionState::Connected;
}

std::uint16_t NetworkClient::local_port() const
{
    return socket_.local_port();
}

NetworkAddress NetworkClient::remote_address() const
{
    return remote_;
}

std::uint32_t NetworkClient::connection_id() const
{
    return connection_id_;
}

const NetworkStats& NetworkClient::stats() const
{
    return stats_;
}

std::vector<NetworkEvent> NetworkClient::drain_events()
{
    return std::exchange(events_, {});
}

bool NetworkClient::send_packet(NetworkMessageType type, std::span<const unsigned char> payload, std::uint64_t tick)
{
    if (!socket_.is_open() || !remote_.valid)
    {
        return false;
    }

    std::vector<unsigned char> bytes = encode_network_packet(type, next_sequence_++, last_received_sequence_, tick, payload);
    if (bytes.empty())
    {
        return false;
    }

    if (!socket_.send_to(remote_, bytes))
    {
        push_event(NetworkEvent{
            .type = NetworkEventType::SocketError,
            .remote = remote_,
            .text = socket_.last_error(),
        });
        return false;
    }

    ++stats_.packets_sent;
    stats_.bytes_sent += bytes.size();
    return true;
}

bool NetworkClient::send_channel(std::uint64_t tick, bool force)
{
    bool sent_any = false;
    const std::vector<std::vector<unsigned char>> datagrams = channel_.transmit(static_cast<double>(tick), force);
    for (const std::vector<unsigned char>& datagram : datagrams)
    {
        sent_any = send_packet(NetworkMessageType::Channel, datagram, tick) || sent_any;
    }
    return sent_any;
}

void NetworkClient::process_channel_datagram(std::span<const unsigned char> payload, std::uint64_t tick)
{
    NetChannelProcessResult channel_result = channel_.process_datagram(payload);
    if (!channel_result.accepted)
    {
        ++stats_.packets_dropped;
        push_event(NetworkEvent{
            .type = NetworkEventType::PacketDropped,
            .remote = remote_,
        });
        return;
    }

    const bool should_reply =
        channel_result.needs_ack || !channel_result.messages.empty() || channel_.has_pending_reliable_data();
    handle_channel_messages(std::move(channel_result.messages), tick);
    if (should_reply)
    {
        send_channel(tick);
    }
}

void NetworkClient::handle_channel_messages(std::vector<NetMessage> messages, std::uint64_t tick)
{
    for (const NetMessage& message : messages)
    {
        if (message.kind == NetMessageKind::StringCommand)
        {
            if (const std::optional<std::string> text = read_string_command_message(message))
            {
                push_event(NetworkEvent{
                    .type = NetworkEventType::TextReceived,
                    .remote = remote_,
                    .connection_id = connection_id_,
                    .tick = tick,
                    .text = *text,
                });
            }
            continue;
        }
        if (message.kind == NetMessageKind::PacketEntities)
        {
            if (const std::optional<NetPacketEntitiesMessage> packet = read_packet_entities_message(message))
            {
                push_event(NetworkEvent{
                    .type = NetworkEventType::SnapshotReceived,
                    .remote = remote_,
                    .connection_id = connection_id_,
                    .tick = tick,
                    .payload = packet->entity_data,
                });
            }
            continue;
        }
        if (message.kind == NetMessageKind::SignonState)
        {
            if (const std::optional<NetSignonStateMessage> signon = read_signon_state_message(message))
            {
                signon_state_ = signon->state;
                push_event(NetworkEvent{
                    .type = NetworkEventType::SignonStateChanged,
                    .remote = remote_,
                    .connection_id = connection_id_,
                    .tick = tick,
                    .signon_state = signon_state_,
                    .text = std::string(to_string(signon_state_)),
                });
            }
            continue;
        }
        if (message.kind == NetMessageKind::ServerInfo)
        {
            if (const std::optional<NetServerInfoMessage> info = read_server_info_message(message))
            {
                push_event(NetworkEvent{
                    .type = NetworkEventType::ServerInfoReceived,
                    .remote = remote_,
                    .connection_id = connection_id_,
                    .tick = tick,
                    .text = info->map_name,
                });
            }
            continue;
        }
        if (message.kind == NetMessageKind::Disconnect)
        {
            disconnect(tick);
            continue;
        }
        if (message.kind == NetMessageKind::Move)
        {
            if (const std::optional<UserCommandBatch> batch = read_move_message(message))
            {
                push_event(NetworkEvent{
                    .type = NetworkEventType::UserCommandBatchReceived,
                    .remote = remote_,
                    .connection_id = connection_id_,
                    .tick = tick,
                    .payload = message.payload,
                    .user_commands = batch->commands,
                    .num_backup_commands = batch->num_backup_commands,
                    .num_new_commands = batch->num_new_commands,
                });
            }
        }
    }
}

void NetworkClient::push_event(NetworkEvent event)
{
    events_.push_back(std::move(event));
}

std::string_view to_string(NetworkConnectionState state)
{
    switch (state)
    {
    case NetworkConnectionState::Disconnected:
        return "disconnected";
    case NetworkConnectionState::Connecting:
        return "connecting";
    case NetworkConnectionState::Connected:
        return "connected";
    }
    return "unknown";
}

std::string_view to_string(NetworkEventType type)
{
    switch (type)
    {
    case NetworkEventType::ServerStarted:
        return "server_started";
    case NetworkEventType::ServerStopped:
        return "server_stopped";
    case NetworkEventType::ClientConnected:
        return "client_connected";
    case NetworkEventType::ClientDisconnected:
        return "client_disconnected";
    case NetworkEventType::ConnectedToServer:
        return "connected_to_server";
    case NetworkEventType::DisconnectedFromServer:
        return "disconnected_from_server";
    case NetworkEventType::TextReceived:
        return "text_received";
    case NetworkEventType::UserCommandReceived:
        return "user_command_received";
    case NetworkEventType::UserCommandBatchReceived:
        return "user_command_batch_received";
    case NetworkEventType::SnapshotReceived:
        return "snapshot_received";
    case NetworkEventType::SignonStateChanged:
        return "signon_state_changed";
    case NetworkEventType::ServerInfoReceived:
        return "server_info_received";
    case NetworkEventType::PacketDropped:
        return "packet_dropped";
    case NetworkEventType::SocketError:
        return "socket_error";
    }
    return "unknown";
}
}
