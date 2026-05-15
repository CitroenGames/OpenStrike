#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace openstrike
{
struct NetworkAddress
{
    std::array<std::uint8_t, 4> ipv4 = {127, 0, 0, 1};
    std::uint16_t port = 0;
    bool valid = false;

    [[nodiscard]] static NetworkAddress any(std::uint16_t port);
    [[nodiscard]] static NetworkAddress localhost(std::uint16_t port);

    [[nodiscard]] bool is_loopback() const;
    [[nodiscard]] std::string to_string(bool include_port = true) const;
};

[[nodiscard]] bool operator==(const NetworkAddress& lhs, const NetworkAddress& rhs);
[[nodiscard]] bool operator!=(const NetworkAddress& lhs, const NetworkAddress& rhs);
[[nodiscard]] std::optional<NetworkAddress> parse_network_address(std::string_view text, std::uint16_t default_port = 0);
}
