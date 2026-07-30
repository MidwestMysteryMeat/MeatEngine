#pragma once
#include <cstdint>
#include <optional>
#include <string>

// Tiny blocking HTTP/1.0 client (winsock/BSD sockets) for master-server talk:
// menu-click server list refreshes and the host's 30 s heartbeat thread. Never
// called on the sim path. Always sends "Connection: close" and reads to EOF;
// returns the response BODY only, and only for status 200 — anything else
// (non-200, timeout, malformed, or a response over 1 MB) is nullopt.
//
// Plain HTTP by design, no TLS: master servers are trusted, self-hosted boxes
// serving a public server list — there is nothing secret on the wire, and a
// TLS stack is not worth its weight here.
namespace meat {

std::optional<std::string> httpGet(const std::string& host, std::uint16_t port,
                                   const std::string& path, int timeoutMs = 3000);
std::optional<std::string> httpPost(const std::string& host, std::uint16_t port,
                                    const std::string& path, const std::string& body,
                                    int timeoutMs = 3000);

} // namespace meat
