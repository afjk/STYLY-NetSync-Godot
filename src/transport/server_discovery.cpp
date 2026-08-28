// SPDX-License-Identifier: Apache-2.0
#include "server_discovery.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
using socket_t = SOCKET;
#define STYLY_INVALID_SOCKET INVALID_SOCKET
#define STYLY_CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using socket_t = int;
#define STYLY_INVALID_SOCKET (-1)
#define STYLY_CLOSE_SOCKET ::close
#endif

namespace styly {
namespace netsync {

namespace {

#if defined(_WIN32)
/// Winsock needs explicit start-up; refcounted so repeated construction is fine.
struct WinsockScope {
    WinsockScope() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockScope() { WSACleanup(); }
};
void ensure_winsock() { static WinsockScope scope; }
#else
void ensure_winsock() {}
#endif

void set_non_blocking(socket_t socket) {
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(socket, FIONBIO, &mode);
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(socket, F_SETFL, flags | O_NONBLOCK);
    }
#endif
}

void set_blocking(socket_t socket) {
#if defined(_WIN32)
    u_long mode = 0;
    ioctlsocket(socket, FIONBIO, &mode);
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(socket, F_SETFL, flags & ~O_NONBLOCK);
    }
#endif
}

void set_receive_timeout(socket_t socket, int milliseconds) {
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(milliseconds);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
               sizeof(timeout));
#else
    timeval timeout;
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

std::vector<std::string> split(const std::string &text, char delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t position = text.find(delimiter, start);
        if (position == std::string::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, position - start));
        start = position + 1;
    }
}

bool parse_port(const std::string &text, int &out) {
    if (text.empty()) {
        return false;
    }
    int value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
        if (value > 65535) {
            return false;
        }
    }
    out = value;
    return true;
}

/// Strip trailing CR/LF and spaces — the TCP reply carries a newline.
std::string rstrip(const std::string &text) {
    std::size_t end = text.size();
    while (end > 0) {
        const char c = text[end - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t' || c == '\0') {
            --end;
        } else {
            break;
        }
    }
    return text.substr(0, end);
}

}  // namespace

bool parse_discovery_response(const std::string &message, const std::string &sender_ip,
                              DiscoveredServer &out) {
    const std::vector<std::string> parts = split(rstrip(message), '|');
    // Clients and servers ship together, so anything that is not a current
    // STYLY-NETSYNC3 reply is not a compatible server.
    if (parts.size() < 6 || parts[0] != kDiscoveryResponseVersion) {
        return false;
    }
    DiscoveredServer server;
    if (!parse_port(parts[1], server.control_port) || !parse_port(parts[2], server.transform_port) ||
        !parse_port(parts[3], server.sub_port) || !parse_port(parts[4], server.rest_api_port)) {
        return false;
    }
    // The server name is the remainder, so a name containing '|' survives.
    server.server_name = parts[5];
    for (std::size_t i = 6; i < parts.size(); ++i) {
        server.server_name += "|" + parts[i];
    }
    server.ip = sender_ip;
    server.address = "tcp://" + sender_ip;
    out = server;
    return true;
}

std::vector<LocalInterface> enumerate_local_interfaces() {
    std::vector<LocalInterface> interfaces;
    ensure_winsock();

#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<char> buffer(size);
    auto *addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                          GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, addresses, &size) == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                              GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, addresses, &size) != NO_ERROR) {
            return interfaces;
        }
    }
    for (auto *adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto *unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto *sin = reinterpret_cast<sockaddr_in *>(unicast->Address.lpSockaddr);
            char text[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text));
            LocalInterface entry;
            entry.address = text;
            const ULONG prefix = unicast->OnLinkPrefixLength;
            const std::uint32_t mask =
                prefix == 0 ? 0 : htonl(0xFFFFFFFFu << (32 - std::min<ULONG>(prefix, 32)));
            in_addr broadcast_addr;
            broadcast_addr.s_addr = (sin->sin_addr.s_addr & mask) | ~mask;
            char broadcast_text[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &broadcast_addr, broadcast_text, sizeof(broadcast_text));
            entry.broadcast = broadcast_text;
            interfaces.push_back(entry);
        }
    }
#else
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) {
        return interfaces;
    }
    for (ifaddrs *entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        const auto *sin = reinterpret_cast<sockaddr_in *>(entry->ifa_addr);
        char text[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text));

        LocalInterface item;
        item.address = text;
        if ((entry->ifa_flags & IFF_BROADCAST) != 0 && entry->ifa_broadaddr != nullptr) {
            const auto *broadcast = reinterpret_cast<sockaddr_in *>(entry->ifa_broadaddr);
            char broadcast_text[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &broadcast->sin_addr, broadcast_text, sizeof(broadcast_text));
            item.broadcast = broadcast_text;
        } else if (entry->ifa_netmask != nullptr) {
            const auto *netmask = reinterpret_cast<sockaddr_in *>(entry->ifa_netmask);
            in_addr broadcast_addr;
            broadcast_addr.s_addr =
                (sin->sin_addr.s_addr & netmask->sin_addr.s_addr) | ~netmask->sin_addr.s_addr;
            char broadcast_text[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &broadcast_addr, broadcast_text, sizeof(broadcast_text));
            item.broadcast = broadcast_text;
        }
        if (item.broadcast.empty()) {
            item.broadcast = "255.255.255.255";
        }
        interfaces.push_back(item);
    }
    freeifaddrs(list);
#endif
    return interfaces;
}

ServerDiscovery::ServerDiscovery() { ensure_winsock(); }

ServerDiscovery::~ServerDiscovery() { stop(); }

void ServerDiscovery::log(const std::string &message) {
    if (on_log_) {
        on_log_(message);
    }
}

void ServerDiscovery::report(const DiscoveredServer &server, bool cache_ip) {
    if (cache_ip && cache_writer_ && !server.ip.empty()) {
        cache_writer_(server.ip);
    }
    discovering_.store(false, std::memory_order_release);
    if (on_found_) {
        on_found_(server);
    }
}

bool ServerDiscovery::probe_tcp(const std::string &ip, int timeout_ms, DiscoveredServer &out) {
    ensure_winsock();
    socket_t handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (handle == STYLY_INVALID_SOCKET) {
        return false;
    }

    struct Closer {
        socket_t handle;
        ~Closer() { STYLY_CLOSE_SOCKET(handle); }
    } closer{handle};

    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port_));
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        return false;
    }

    set_non_blocking(handle);
    const int connect_result =
        ::connect(handle, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    if (connect_result != 0) {
#if defined(_WIN32)
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            return false;
        }
#else
        if (errno != EINPROGRESS) {
            return false;
        }
#endif
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(handle, &writable);
        timeval timeout;
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        if (::select(static_cast<int>(handle) + 1, nullptr, &writable, nullptr, &timeout) <= 0) {
            return false;
        }
        int error = 0;
        socklen_t error_size = sizeof(error);
        if (::getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error),
                         &error_size) != 0 ||
            error != 0) {
            return false;
        }
    }
    set_blocking(handle);
    set_receive_timeout(handle, 1000);

    const std::string request = kDiscoveryRequest;
    if (::send(handle, request.data(), static_cast<int>(request.size()), 0) < 0) {
        return false;
    }

    char buffer[1024];
    const auto received = ::recv(handle, buffer, sizeof(buffer), 0);
    if (received <= 0) {
        return false;
    }
    return parse_discovery_response(std::string(buffer, static_cast<std::size_t>(received)), ip,
                                    out);
}

bool ServerDiscovery::start() {
    if (discovering_.load(std::memory_order_acquire) || thread_.joinable()) {
        return false;
    }
    should_stop_.store(false, std::memory_order_release);
    discovering_.store(true, std::memory_order_release);
    thread_ = std::thread(&ServerDiscovery::discovery_loop, this);
    return true;
}

void ServerDiscovery::stop() {
    should_stop_.store(true, std::memory_order_release);
    discovering_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ServerDiscovery::discovery_loop() {
    // 1. Localhost. Never cached: it is always reachable locally, so caching it
    //    would shadow a real LAN server on the next run.
    DiscoveredServer found;
    log("probing localhost over TCP");
    if (probe_tcp("127.0.0.1", tcp_connect_timeout_ms, found)) {
        log("server found on localhost");
        report(found, /*cache_ip=*/false);
        return;
    }
    if (should_stop_.load(std::memory_order_acquire)) {
        return;
    }

    // 2. The last known server.
    if (cache_reader_) {
        const std::string cached = cache_reader_();
        if (!cached.empty()) {
            log("probing cached server " + cached);
            if (probe_tcp(cached, tcp_connect_timeout_ms, found)) {
                log("server found at cached address " + cached);
                report(found, /*cache_ip=*/true);
                return;
            }
        }
    }
    if (should_stop_.load(std::memory_order_acquire)) {
        return;
    }

    // 3. UDP broadcast on every interface until something answers.
    const std::vector<LocalInterface> interfaces = enumerate_local_interfaces();
    std::vector<int> sockets;
    std::vector<LocalInterface> bound;

    for (const LocalInterface &item : interfaces) {
        socket_t handle = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (handle == STYLY_INVALID_SOCKET) {
            continue;
        }
        int enable = 1;
        setsockopt(handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&enable),
                   sizeof(enable));
        sockaddr_in bind_address;
        std::memset(&bind_address, 0, sizeof(bind_address));
        bind_address.sin_family = AF_INET;
        bind_address.sin_port = 0;
        if (inet_pton(AF_INET, item.address.c_str(), &bind_address.sin_addr) != 1 ||
            ::bind(handle, reinterpret_cast<sockaddr *>(&bind_address), sizeof(bind_address)) !=
                0) {
            STYLY_CLOSE_SOCKET(handle);
            continue;
        }
        set_receive_timeout(handle, udp_receive_timeout_ms);
        sockets.push_back(static_cast<int>(handle));
        bound.push_back(item);
        log("discovery socket bound to " + item.address);
    }

    if (sockets.empty()) {
        // Fallback: one unbound socket broadcasting to 255.255.255.255.
        socket_t handle = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (handle != STYLY_INVALID_SOCKET) {
            int enable = 1;
            setsockopt(handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&enable),
                       sizeof(enable));
            set_receive_timeout(handle, udp_receive_timeout_ms);
            sockets.push_back(static_cast<int>(handle));
            LocalInterface fallback;
            fallback.address = "0.0.0.0";
            fallback.broadcast = "255.255.255.255";
            bound.push_back(fallback);
            log("using fallback unbound discovery socket");
        }
    }

    while (!should_stop_.load(std::memory_order_acquire) &&
           discovering_.load(std::memory_order_acquire)) {
        if (broadcast_round(sockets, bound)) {
            break;
        }
    }

    for (int handle : sockets) {
        STYLY_CLOSE_SOCKET(static_cast<socket_t>(handle));
    }
}

bool ServerDiscovery::broadcast_round(const std::vector<int> &sockets,
                                      const std::vector<LocalInterface> &interfaces) {
    const std::string request = kDiscoveryRequest;

    for (std::size_t i = 0; i < sockets.size(); ++i) {
        sockaddr_in destination;
        std::memset(&destination, 0, sizeof(destination));
        destination.sin_family = AF_INET;
        destination.sin_port = htons(static_cast<std::uint16_t>(port_));
        const std::string &broadcast =
            i < interfaces.size() ? interfaces[i].broadcast : std::string("255.255.255.255");
        if (inet_pton(AF_INET, broadcast.c_str(), &destination.sin_addr) != 1) {
            continue;
        }
        ::sendto(static_cast<socket_t>(sockets[i]), request.data(),
                 static_cast<int>(request.size()), 0,
                 reinterpret_cast<sockaddr *>(&destination), sizeof(destination));
    }

    for (int handle : sockets) {
        if (should_stop_.load(std::memory_order_acquire)) {
            return true;
        }
        char buffer[1024];
        sockaddr_in sender;
        socklen_t sender_size = sizeof(sender);
        const auto received =
            ::recvfrom(static_cast<socket_t>(handle), buffer, sizeof(buffer), 0,
                       reinterpret_cast<sockaddr *>(&sender), &sender_size);
        if (received <= 0) {
            continue;  // Timeout is the normal case.
        }
        char sender_text[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &sender.sin_addr, sender_text, sizeof(sender_text));

        DiscoveredServer found;
        if (parse_discovery_response(std::string(buffer, static_cast<std::size_t>(received)),
                                     sender_text, found)) {
            log("discovered server '" + found.server_name + "' at " + found.address);
            report(found, /*cache_ip=*/true);
            return true;
        }
        log(std::string("ignoring non-matching discovery response from ") + sender_text);
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<int>(broadcast_interval * 1000.0)));
    return false;
}

}  // namespace netsync
}  // namespace styly
