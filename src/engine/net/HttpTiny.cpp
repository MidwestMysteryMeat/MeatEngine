#include "engine/net/HttpTiny.h"

#include "engine/core/Log.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstdlib>
#include <cstring>
#include <format>

namespace meat {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kNativeInvalid = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kNativeInvalid = -1;
#endif

constexpr std::size_t kMaxResponseBytes = 1u << 20; // 1 MB — masters send small JSON

void closeNative(NativeSocket s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

// Per-call winsock scope. WSAStartup is internally ref-counted by Windows, so
// nesting with the transports'/discovery's own init is fine. No-op on POSIX.
struct WsaGuard {
#ifdef _WIN32
    bool ok = false;
    WsaGuard() {
        WSADATA data{};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WsaGuard() {
        if (ok) {
            WSACleanup();
        }
    }
#else
    bool ok = true;
#endif
    WsaGuard(const WsaGuard&) = delete;
    WsaGuard& operator=(const WsaGuard&) = delete;
#ifndef _WIN32
    WsaGuard() = default;
#endif
};

bool setNonBlocking(NativeSocket s, bool nonBlocking) {
#ifdef _WIN32
    u_long mode = nonBlocking ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    const int next = nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, next) == 0;
#endif
}

bool connectInProgress() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS;
#endif
}

// Non-blocking connect + select so a dead master costs timeoutMs, not the OS
// default (~21 s on Windows).
bool connectWithTimeout(NativeSocket sock, const addrinfo& addr, int timeoutMs) {
    if (!setNonBlocking(sock, true)) {
        return false;
    }
    const int rc = ::connect(sock, addr.ai_addr, static_cast<int>(addr.ai_addrlen));
    if (rc != 0) {
        if (!connectInProgress()) {
            return false;
        }
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(sock, &writeSet);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        const int nfds =
#ifdef _WIN32
            0; // ignored on Windows
#else
            sock + 1;
#endif
        if (select(nfds, nullptr, &writeSet, nullptr, &tv) != 1) {
            return false; // timeout or error
        }
        int soError = 0;
#ifdef _WIN32
        int optLen = sizeof(soError);
#else
        socklen_t optLen = sizeof(soError);
#endif
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &optLen) !=
                0 ||
            soError != 0) {
            return false;
        }
    }
    return setNonBlocking(sock, false); // back to blocking; SO_*TIMEO covers send/recv
}

bool setIoTimeouts(NativeSocket sock, int timeoutMs) {
#ifdef _WIN32
    const DWORD ms = static_cast<DWORD>(timeoutMs);
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms),
                      sizeof(ms)) == 0 &&
           setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms),
                      sizeof(ms)) == 0;
#else
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
           setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

bool sendAll(NativeSocket sock, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto rc = ::send(sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (rc <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(rc);
    }
    return true;
}

// Returns the raw response (status line + headers + body), or nullopt on any
// socket failure, timeout, or the 1 MB cap.
std::optional<std::string> exchange(const std::string& host, std::uint16_t port,
                                    const std::string& request, int timeoutMs) {
    WsaGuard wsa;
    if (!wsa.ok) {
        log::warn("http: winsock init failed");
        return std::nullopt;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC; // hostnames, dotted IPv4, and IPv6 literals all resolve
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const std::string portText = std::to_string(port);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &results) != 0 || results == nullptr) {
        log::warn("http: cannot resolve '{}'", host);
        return std::nullopt;
    }

    NativeSocket sock = kNativeInvalid;
    for (const addrinfo* addr = results; addr != nullptr; addr = addr->ai_next) {
        const NativeSocket candidate =
            ::socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (candidate == kNativeInvalid) {
            continue;
        }
        if (connectWithTimeout(candidate, *addr, timeoutMs)) {
            sock = candidate;
            break;
        }
        closeNative(candidate);
    }
    freeaddrinfo(results);
    if (sock == kNativeInvalid) {
        log::warn("http: connect to {}:{} failed", host, port);
        return std::nullopt;
    }

    std::optional<std::string> response;
    if (setIoTimeouts(sock, timeoutMs) && sendAll(sock, request)) {
        std::string received;
        char buf[4096];
        for (;;) {
            const auto rc = ::recv(sock, buf, static_cast<int>(sizeof(buf)), 0);
            if (rc == 0) { // clean EOF — HTTP/1.0 + Connection: close ends here
                response = std::move(received);
                break;
            }
            if (rc < 0) {
                break; // timeout or error — fail the whole request
            }
            if (received.size() + static_cast<std::size_t>(rc) > kMaxResponseBytes) {
                log::warn("http: response from {}:{} exceeds 1 MB, dropped", host, port);
                break;
            }
            received.append(buf, static_cast<std::size_t>(rc));
        }
    }
    closeNative(sock);
    return response;
}

// Splits status/headers from body; nullopt unless the status is exactly 200.
std::optional<std::string> extractBody(const std::string& response) {
    const std::size_t headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return std::nullopt;
    }
    // Status line: "HTTP/1.x NNN reason". Anything that doesn't parse is a fail.
    const std::size_t firstSpace = response.find(' ');
    if (firstSpace == std::string::npos || firstSpace > headerEnd) {
        return std::nullopt;
    }
    const int status = std::atoi(response.c_str() + firstSpace + 1);
    if (status != 200) {
        return std::nullopt;
    }
    return response.substr(headerEnd + 4);
}

} // namespace

std::optional<std::string> httpGet(const std::string& host, std::uint16_t port,
                                   const std::string& path, int timeoutMs) {
    const std::string request = std::format("GET {} HTTP/1.0\r\n"
                                            "Host: {}:{}\r\n"
                                            "Connection: close\r\n"
                                            "\r\n",
                                            path, host, port);
    const auto response = exchange(host, port, request, timeoutMs);
    if (!response) {
        return std::nullopt;
    }
    return extractBody(*response);
}

std::optional<std::string> httpPost(const std::string& host, std::uint16_t port,
                                    const std::string& path, const std::string& body,
                                    int timeoutMs) {
    const std::string request = std::format("POST {} HTTP/1.0\r\n"
                                            "Host: {}:{}\r\n"
                                            "Connection: close\r\n"
                                            "Content-Type: application/json\r\n"
                                            "Content-Length: {}\r\n"
                                            "\r\n"
                                            "{}",
                                            path, host, port, body.size(), body);
    const auto response = exchange(host, port, request, timeoutMs);
    if (!response) {
        return std::nullopt;
    }
    return extractBody(*response);
}

} // namespace meat
