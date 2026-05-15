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
            const std::vector<unsigned char> payload = make_connect_accept_payload(peer->connection_id);
            send_packet(*peer, NetworkMessageType::ConnectAccept, as_payload(payload), tick);
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

    const std::vector<unsigned char> payload = make_text_payload(text);
    return send_packet(*peer, NetworkMessageType::Text, as_payload(payload), tick);
}

void NetworkServer::broadcast_text(std::string_view text, std::uint64_t tick)
{
    const std::vector<unsigned char> payload = make_text_payload(text);
    for (NetworkPeer& peer : clients_)
    {
        send_packet(peer, NetworkMessageType::Text, as_payload(payload), tick);
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
                push_event(NetworkEvent{
                    .type = NetworkEventType::ConnectedToServer,
                    .remote = remote_,
                    .connection_id = connection_id_,
                });
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

    const std::vector<unsigned char> payload = make_text_payload(text);
    return send_packet(NetworkMessageType::Text, as_payload(payload), tick);
}

NetworkConnectionState NetworkClient::state() const
{
    return state_;
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
    case NetworkEventType::PacketDropped:
        return "packet_dropped";
    case NetworkEventType::SocketError:
        return "socket_error";
    }
    return "unknown";
}
}
