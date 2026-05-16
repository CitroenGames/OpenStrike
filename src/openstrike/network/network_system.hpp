#pragma once

#include "openstrike/network/network_session.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace openstrike
{
class NetworkSystem
{
public:
    bool start_server(std::uint16_t port);
    void stop_server();
    bool connect_client(const NetworkAddress& address, std::uint64_t tick);
    void disconnect_client(std::uint64_t tick);
    void poll(std::uint64_t tick);
    bool send_client_text(std::string_view text, std::uint64_t tick);
    void broadcast_server_text(std::string_view text, std::uint64_t tick);
    bool send_client_user_command(std::span<const unsigned char> payload, std::uint64_t tick);
    bool send_server_snapshot(const NetworkAddress& address, std::span<const unsigned char> payload, std::uint64_t tick);
    void broadcast_server_snapshot(std::span<const unsigned char> payload, std::uint64_t tick);

    [[nodiscard]] NetworkServer& server();
    [[nodiscard]] NetworkClient& client();
    [[nodiscard]] const NetworkServer& server() const;
    [[nodiscard]] const NetworkClient& client() const;
    [[nodiscard]] std::vector<NetworkEvent> drain_events();

private:
    NetworkServer server_;
    NetworkClient client_;
    std::vector<NetworkEvent> events_;
};
}
