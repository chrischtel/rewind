#include "rewind/net/udp.h"

#include "rewind/core/log.h"

#include <algorithm>
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
using native_socket = SOCKET;
#else
bool winsock_ready() { return true; }
std::string socket_error() { return std::string("socket error ") + std::strerror(errno); }
void close_socket(long long s) { ::close(static_cast<int>(s)); }
bool set_nonblocking(long long s) {
    const int flags = fcntl(static_cast<int>(s), F_GETFL, 0);
    return flags >= 0 && fcntl(static_cast<int>(s), F_SETFL, flags | O_NONBLOCK) == 0;
}
bool would_block() { return errno == EWOULDBLOCK || errno == EAGAIN; }
using native_socket = int;
#endif

Address address_from(const sockaddr_in& in) {
    char text[64] = "?";
    inet_ntop(AF_INET, &in.sin_addr, text, sizeof text);
    return Address{text, ntohs(in.sin_port)};
}

} // namespace

UdpTransport::UdpTransport() = default;
UdpTransport::~UdpTransport() { close(); }

bool UdpTransport::resolve(const Address& address, u8* out, int& len) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(address.host.c_str(), nullptr, &hints, &result) != 0 || !result) {
        error_ = "could not resolve " + address.host;
        return false;
    }
    sockaddr_in peer{};
    std::memcpy(&peer, result->ai_addr, sizeof peer);
    peer.sin_port = htons(address.port);
    freeaddrinfo(result);
    std::memcpy(out, &peer, sizeof peer);
    len = sizeof peer;
    return true;
}

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
    if (::bind(static_cast<native_socket>(s), reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
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
    sockaddr_in bound{};
    socklen_type bound_len = sizeof bound;
    if (getsockname(static_cast<native_socket>(s), reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0)
        local_port_ = ntohs(bound.sin_port);
    else
        local_port_ = local_port;

    if (!peer_host.empty()) {
        u8 raw[32];
        int len = 0;
        if (!resolve(Address{peer_host, peer_port}, raw, len)) {
            close();
            return false;
        }
        peers_.push_back(Address{peer_host, peer_port});
        RW_LOG_INFO("net", "udp: port %u -> %s:%u", local_port_, peer_host.c_str(), peer_port);
    } else {
        RW_LOG_INFO("net", "udp: listening on port %u", local_port_);
    }
    error_.clear();
    return true;
}

void UdpTransport::close() {
    if (socket_ != kInvalid) close_socket(socket_);
    socket_ = kInvalid;
    peers_.clear();
    local_port_ = 0;
}

void UdpTransport::add_peer(const Address& peer) {
    if (std::find(peers_.begin(), peers_.end(), peer) == peers_.end()) peers_.push_back(peer);
}

void UdpTransport::send_to(const std::vector<u8>& packet, const Address& to) {
    if (socket_ == kInvalid || packet.empty()) return;
    u8 raw[32];
    int len = 0;
    if (!resolve(to, raw, len)) return;
    const auto sent = ::sendto(static_cast<native_socket>(socket_), reinterpret_cast<const char*>(packet.data()),
                               static_cast<int>(packet.size()), 0, reinterpret_cast<const sockaddr*>(raw),
                               static_cast<socklen_type>(len));
    if (sent >= 0) ++sent_;
}

void UdpTransport::send(const std::vector<u8>& packet) {
    for (const Address& p : peers_) send_to(packet, p);
}

bool UdpTransport::receive_from(std::vector<u8>& out, Address& from) {
    if (socket_ == kInvalid) return false;
    u8 buffer[2048];
    sockaddr_in sender{};
    socklen_type sender_len = sizeof sender;
    const auto got = ::recvfrom(static_cast<native_socket>(socket_), reinterpret_cast<char*>(buffer), sizeof buffer, 0,
                                reinterpret_cast<sockaddr*>(&sender), &sender_len);
    if (got <= 0) {
        if (got < 0 && !would_block()) {
            // ICMP "port unreachable" shows up here on Windows; harmless for us.
        }
        return false;
    }
    from = address_from(sender);
    if (peers_.empty() && adopt_) {
        peers_.push_back(from);
        RW_LOG_INFO("net", "udp: peer connected from %s:%u", from.host.c_str(), from.port);
    }
    out.assign(buffer, buffer + got);
    ++received_;
    return true;
}

} // namespace rw::net
