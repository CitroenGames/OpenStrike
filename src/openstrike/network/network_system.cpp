#include "openstrike/network/network_system.hpp"

#include <utility>

namespace openstrike
{
bool NetworkSystem::start_server(std::uint16_t port)
{
    return server_.start(port);
}

void NetworkSystem::stop_server()
{
    server_.stop();
}

bool NetworkSystem::connect_client(const NetworkAddress& address, std::uint64_t tick)
{
    return client_.connect(address, tick);
}

void NetworkSystem::disconnect_client(std::uint64_t tick)
{
    client_.disconnect(tick);
}

void NetworkSystem::poll(std::uint64_t tick)
{
    server_.poll(tick);
    client_.poll(tick);

    std::vector<NetworkEvent> server_events = server_.drain_events();
    std::vector<NetworkEvent> client_events = client_.drain_events();
    events_.insert(events_.end(), std::make_move_iterator(server_events.begin()), std::make_move_iterator(server_events.end()));
    events_.insert(events_.end(), std::make_move_iterator(client_events.begin()), std::make_move_iterator(client_events.end()));
}

bool NetworkSystem::send_client_text(std::string_view text, std::uint64_t tick)
{
    return client_.send_text(text, tick);
}

void NetworkSystem::broadcast_server_text(std::string_view text, std::uint64_t tick)
{
    server_.broadcast_text(text, tick);
}

bool NetworkSystem::send_client_user_command(std::span<const unsigned char> payload, std::uint64_t tick)
{
    return client_.send_user_command(payload, tick);
}

bool NetworkSystem::send_server_snapshot(const NetworkAddress& address, std::span<const unsigned char> payload, std::uint64_t tick)
{
    return server_.send_snapshot(address, payload, tick);
}

void NetworkSystem::broadcast_server_snapshot(std::span<const unsigned char> payload, std::uint64_t tick)
{
    server_.broadcast_snapshot(payload, tick);
}

NetworkServer& NetworkSystem::server()
{
    return server_;
}

NetworkClient& NetworkSystem::client()
{
    return client_;
}

const NetworkServer& NetworkSystem::server() const
{
    return server_;
}

const NetworkClient& NetworkSystem::client() const
{
    return client_;
}

std::vector<NetworkEvent> NetworkSystem::drain_events()
{
    return std::exchange(events_, {});
}
}
