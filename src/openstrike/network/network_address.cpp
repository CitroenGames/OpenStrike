#include "openstrike/network/network_address.hpp"

#include <charconv>
#include <system_error>

namespace openstrike
{
namespace
{
bool parse_u16(std::string_view text, std::uint16_t& value)
{
    unsigned parsed = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed > 65535U)
    {
        return false;
    }

    value = static_cast<std::uint16_t>(parsed);
    return true;
}

bool parse_octet(std::string_view text, std::uint8_t& value)
{
    unsigned parsed = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed > 255U)
    {
        return false;
    }

    value = static_cast<std::uint8_t>(parsed);
    return true;
}

bool parse_ipv4(std::string_view text, std::array<std::uint8_t, 4>& ipv4)
{
    std::size_t begin = 0;
    for (std::size_t octet = 0; octet < ipv4.size(); ++octet)
    {
        const std::size_t dot = octet + 1 < ipv4.size() ? text.find('.', begin) : std::string_view::npos;
        const std::size_t end = dot == std::string_view::npos ? text.size() : dot;
        if (end == begin || !parse_octet(text.substr(begin, end - begin), ipv4[octet]))
        {
            return false;
        }

        begin = end + 1;
    }

    return begin == text.size() + 1;
}
}

NetworkAddress NetworkAddress::any(std::uint16_t port)
{
    return NetworkAddress{
        .ipv4 = {0, 0, 0, 0},
        .port = port,
        .valid = true,
    };
}

NetworkAddress NetworkAddress::localhost(std::uint16_t port)
{
    return NetworkAddress{
        .ipv4 = {127, 0, 0, 1},
        .port = port,
        .valid = true,
    };
}

bool NetworkAddress::is_loopback() const
{
    return valid && ipv4[0] == 127;
}

std::string NetworkAddress::to_string(bool include_port) const
{
    if (!valid)
    {
        return "<invalid>";
    }

    std::string result = std::to_string(ipv4[0]) + "." + std::to_string(ipv4[1]) + "." + std::to_string(ipv4[2]) + "." + std::to_string(ipv4[3]);
    if (include_port)
    {
        result += ':';
        result += std::to_string(port);
    }
    return result;
}

bool operator==(const NetworkAddress& lhs, const NetworkAddress& rhs)
{
    return lhs.valid == rhs.valid && lhs.ipv4 == rhs.ipv4 && lhs.port == rhs.port;
}

bool operator!=(const NetworkAddress& lhs, const NetworkAddress& rhs)
{
    return !(lhs == rhs);
}

std::optional<NetworkAddress> parse_network_address(std::string_view text, std::uint16_t default_port)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    {
        text.remove_suffix(1);
    }
    if (text.empty())
    {
        return std::nullopt;
    }

    std::uint16_t port = default_port;
    std::string_view host = text;
    const std::size_t colon = text.rfind(':');
    if (colon != std::string_view::npos)
    {
        host = text.substr(0, colon);
        if (!parse_u16(text.substr(colon + 1), port))
        {
            return std::nullopt;
        }
    }

    NetworkAddress address;
    address.port = port;
    address.valid = true;
    if (host == "localhost")
    {
        address.ipv4 = {127, 0, 0, 1};
        return address;
    }
    if (host == "*" || host == "any")
    {
        address.ipv4 = {0, 0, 0, 0};
        return address;
    }
    if (!parse_ipv4(host, address.ipv4))
    {
        return std::nullopt;
    }
    return address;
}
}
