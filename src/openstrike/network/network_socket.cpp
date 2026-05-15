#include "openstrike/network/network_socket.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace openstrike
{
namespace
{
#if defined(_WIN32)
using PlatformSocket = SOCKET;
using SocketAddressLength = int;
constexpr PlatformSocket kInvalidSocket = INVALID_SOCKET;

bool acquire_socket_runtime(std::string& error)
{
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0)
    {
        error = "WSAStartup failed: " + std::to_string(result);
        return false;
    }
    return true;
}

void release_socket_runtime()
{
    WSACleanup();
}

std::string socket_error_string()
{
    return "winsock error " + std::to_string(WSAGetLastError());
}

bool would_block()
{
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK;
}

void close_platform_socket(PlatformSocket socket)
{
    closesocket(socket);
}

bool set_nonblocking(PlatformSocket socket, std::string& error)
{
    u_long enabled = 1;
    if (ioctlsocket(socket, FIONBIO, &enabled) != 0)
    {
        error = socket_error_string();
        return false;
    }
    return true;
}
#else
using PlatformSocket = int;
using SocketAddressLength = socklen_t;
constexpr PlatformSocket kInvalidSocket = -1;

bool acquire_socket_runtime(std::string&)
{
    return true;
}

void release_socket_runtime()
{
}

std::string socket_error_string()
{
    return std::strerror(errno);
}

bool would_block()
{
    return errno == EWOULDBLOCK || errno == EAGAIN;
}

void close_platform_socket(PlatformSocket socket)
{
    close(socket);
}

bool set_nonblocking(PlatformSocket socket, std::string& error)
{
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0)
    {
        error = socket_error_string();
        return false;
    }
    return true;
}
#endif

PlatformSocket to_platform_socket(UdpSocket::NativeSocket socket)
{
    return static_cast<PlatformSocket>(socket);
}

UdpSocket::NativeSocket from_platform_socket(PlatformSocket socket)
{
    return static_cast<UdpSocket::NativeSocket>(socket);
}

sockaddr_in to_sockaddr(const NetworkAddress& address)
{
    sockaddr_in socket_address{};
    socket_address.sin_family = AF_INET;
    socket_address.sin_port = htons(address.port);
    socket_address.sin_addr.s_addr = htonl((static_cast<std::uint32_t>(address.ipv4[0]) << 24U) |
                                           (static_cast<std::uint32_t>(address.ipv4[1]) << 16U) |
                                           (static_cast<std::uint32_t>(address.ipv4[2]) << 8U) |
                                           static_cast<std::uint32_t>(address.ipv4[3]));
    return socket_address;
}

NetworkAddress from_sockaddr(const sockaddr_in& socket_address)
{
    const std::uint32_t ip = ntohl(socket_address.sin_addr.s_addr);
    return NetworkAddress{
        .ipv4 = {
            static_cast<std::uint8_t>((ip >> 24U) & 0xFFU),
            static_cast<std::uint8_t>((ip >> 16U) & 0xFFU),
            static_cast<std::uint8_t>((ip >> 8U) & 0xFFU),
            static_cast<std::uint8_t>(ip & 0xFFU),
        },
        .port = ntohs(socket_address.sin_port),
        .valid = true,
    };
}
}

UdpSocket::~UdpSocket()
{
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : socket_(other.socket_)
    , local_port_(other.local_port_)
    , last_error_(std::move(other.last_error_))
{
    other.socket_ = 0;
    other.local_port_ = 0;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept
{
    if (this != &other)
    {
        close();
        socket_ = other.socket_;
        local_port_ = other.local_port_;
        last_error_ = std::move(other.last_error_);
        other.socket_ = 0;
        other.local_port_ = 0;
    }
    return *this;
}

bool UdpSocket::open(std::uint16_t port)
{
    close();
    last_error_.clear();
    if (!acquire_socket_runtime(last_error_))
    {
        return false;
    }

    PlatformSocket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidSocket)
    {
        last_error_ = socket_error_string();
        release_socket_runtime();
        return false;
    }

    if (!set_nonblocking(socket, last_error_))
    {
        close_platform_socket(socket);
        release_socket_runtime();
        return false;
    }

    sockaddr_in bind_address = to_sockaddr(NetworkAddress::any(port));
    if (bind(socket, reinterpret_cast<const sockaddr*>(&bind_address), sizeof(bind_address)) != 0)
    {
        last_error_ = socket_error_string();
        close_platform_socket(socket);
        release_socket_runtime();
        return false;
    }

    sockaddr_in local_address{};
    SocketAddressLength local_length = sizeof(local_address);
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&local_address), &local_length) != 0)
    {
        last_error_ = socket_error_string();
        close_platform_socket(socket);
        release_socket_runtime();
        return false;
    }

    socket_ = from_platform_socket(socket);
    local_port_ = ntohs(local_address.sin_port);
    return true;
}

void UdpSocket::close()
{
    const PlatformSocket socket = to_platform_socket(socket_);
    if (socket != kInvalidSocket && socket_ != 0)
    {
        close_platform_socket(socket);
        socket_ = 0;
        local_port_ = 0;
        release_socket_runtime();
    }
}

bool UdpSocket::is_open() const
{
    return socket_ != 0;
}

std::uint16_t UdpSocket::local_port() const
{
    return local_port_;
}

const std::string& UdpSocket::last_error() const
{
    return last_error_;
}

bool UdpSocket::send_to(const NetworkAddress& address, std::span<const unsigned char> bytes)
{
    if (!is_open() || !address.valid || bytes.empty())
    {
        return false;
    }

    const sockaddr_in socket_address = to_sockaddr(address);
    const int sent = sendto(to_platform_socket(socket_),
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<const sockaddr*>(&socket_address),
        sizeof(socket_address));
    if (sent != static_cast<int>(bytes.size()))
    {
        last_error_ = socket_error_string();
        return false;
    }
    return true;
}

std::optional<UdpPacket> UdpSocket::receive(std::size_t max_bytes)
{
    if (!is_open())
    {
        return std::nullopt;
    }

    std::vector<unsigned char> bytes(std::max<std::size_t>(max_bytes, 1));
    sockaddr_in sender{};
    SocketAddressLength sender_length = sizeof(sender);
    const int received = recvfrom(to_platform_socket(socket_),
        reinterpret_cast<char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<sockaddr*>(&sender),
        &sender_length);
    if (received < 0)
    {
        if (!would_block())
        {
            last_error_ = socket_error_string();
        }
        return std::nullopt;
    }

    bytes.resize(static_cast<std::size_t>(received));
    return UdpPacket{
        .sender = from_sockaddr(sender),
        .bytes = std::move(bytes),
    };
}
}
