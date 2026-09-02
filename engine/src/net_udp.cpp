#include "rewind/net/udp.h"

#include "rewind/core/log.h"

#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_type = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socklen_type = socklen_t;
#endif

namespace rw::net {

namespace {

#if defined(_WIN32)
bool winsock_ready() {
    static bool started = false;
    if (!started) {
        WSADATA data;
        started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    return started;
}

std::string socket_error() { return "winsock error " + std::to_string(WSAGetLastError()); }
void close_socket(long long s) { closesocket(static_cast<SOCKET>(s)); }
bool set_nonblocking(long long s) {
    u_long mode = 1;
    return ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &mode) == 0;
}
bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
bool winsock_ready() { return true; }
std::string socket_error() { return std::string("socket error ") + std::strerror(errno); }
void close_socket(long long s) { ::close(static_cast<int>(s)); }
bool set_nonblocking(long long s) {
    const int flags = fcntl(static_cast<int>(s), F_GETFL, 0);
    return flags >= 0 && fcntl(static_cast<int>(s), F_SETFL, flags | O_NONBLOCK) == 0;
}
bool would_block() { return errno == EWOULDBLOCK || errno == EAGAIN; }
#endif

} // namespace

UdpTransport::UdpTransport() = default;
UdpTransport::~UdpTransport() { close(); }

bool UdpTransport::open(u16 local_port, const std::string& peer_host, u16 peer_port) {
    close();
    if (!winsock_ready()) {
        error_ = "winsock could not be initialised";
        return false;
    }

    const long long s = static_cast<long long>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (s < 0 || s == kInvalid) {
        error_ = "socket(): " + socket_error();
        return false;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(local_port);
    if (::bind(static_cast<decltype(::socket(0, 0, 0))>(s), reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
        error_ = "bind(" + std::to_string(local_port) + "): " + socket_error();
        close_socket(s);
        return false;
    }
    if (!set_nonblocking(s)) {
        error_ = "could not make the socket non-blocking: " + socket_error();
        close_socket(s);
        return false;
    }
    socket_ = s;

    if (!peer_host.empty()) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(peer_host.c_str(), nullptr, &hints, &result) != 0 || !result) {
            error_ = "could not resolve " + peer_host;
            close();
            return false;
        }
        sockaddr_in peer{};
        std::memcpy(&peer, result->ai_addr, sizeof peer);
        peer.sin_port = htons(peer_port);
        freeaddrinfo(result);
        std::memcpy(peer_addr_, &peer, sizeof peer);
        peer_len_ = sizeof peer;
        has_peer_ = true;
        RW_LOG_INFO("net", "udp: port %u -> %s:%u", local_port, peer_host.c_str(), peer_port);
    } else {
        RW_LOG_INFO("net", "udp: listening on port %u, waiting for a peer", local_port);
    }
    error_.clear();
    return true;
}

void UdpTransport::close() {
    if (socket_ != kInvalid) close_socket(socket_);
    socket_ = kInvalid;
    has_peer_ = false;
    peer_len_ = 0;
}

void UdpTransport::send(const std::vector<u8>& packet) {
    if (socket_ == kInvalid || !has_peer_ || packet.empty()) return;
    const auto sent = ::sendto(static_cast<decltype(::socket(0, 0, 0))>(socket_),
                               reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                               reinterpret_cast<const sockaddr*>(peer_addr_), static_cast<socklen_type>(peer_len_));
    if (sent >= 0) ++sent_;
}

bool UdpTransport::receive(std::vector<u8>& out) {
    if (socket_ == kInvalid) return false;
    u8 buffer[2048];
    sockaddr_in from{};
    socklen_type from_len = sizeof from;
    const auto got = ::recvfrom(static_cast<decltype(::socket(0, 0, 0))>(socket_), reinterpret_cast<char*>(buffer),
                                sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
    if (got <= 0) {
        if (got < 0 && !would_block()) {
            // ICMP "port unreachable" shows up here on Windows; harmless for us.
        }
        return false;
    }
    if (!has_peer_) {
        std::memcpy(peer_addr_, &from, sizeof from);
        peer_len_ = sizeof from;
        has_peer_ = true;
        char text[64] = "?";
        inet_ntop(AF_INET, &from.sin_addr, text, sizeof text);
        RW_LOG_INFO("net", "udp: peer connected from %s:%u", text, ntohs(from.sin_port));
    }
    out.assign(buffer, buffer + got);
    ++received_;
    return true;
}

} // namespace rw::net
