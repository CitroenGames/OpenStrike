#pragma once

#include "openstrike/network/network_address.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace openstrike
{
struct UdpPacket
{
    NetworkAddress sender;
    std::vector<unsigned char> bytes;
};

class UdpSocket
{
public:
#if defined(_WIN32)
    using NativeSocket = std::uintptr_t;
#else
    using NativeSocket = int;
#endif

    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    bool open(std::uint16_t port = 0);
    void close();
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] std::uint16_t local_port() const;
    [[nodiscard]] const std::string& last_error() const;

    bool send_to(const NetworkAddress& address, std::span<const unsigned char> bytes);
    [[nodiscard]] std::optional<UdpPacket> receive(std::size_t max_bytes = 1500);

private:
    NativeSocket socket_ = 0;
    std::uint16_t local_port_ = 0;
    std::string last_error_;
};
}
