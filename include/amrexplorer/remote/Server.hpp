#pragma once

#include <amrexplorer/remote/Frame.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace amrvis::remote {

struct ServerOptions {
    std::uint16_t port = 0;
    unsigned int workerCount = 0;
    std::uint32_t maximumFrameBytes = defaultMaximumFrameBytes;
    std::uint32_t maximumDatasets = 8;
    std::uint32_t maximumOutstandingRequests = 64;
    std::uint32_t maximumConnections = 32;
    // Bounds the unauthenticated hello read with one absolute deadline and a
    // small frame cap. Both limits are removed after a valid hello.
    std::chrono::milliseconds handshakeTimeout{5000};
    std::uint32_t maximumHandshakeFrameBytes = 64U * 1024U;
    // A response may take arbitrarily long while bytes continue moving. A peer
    // that makes no write progress for this interval is disconnected so it
    // cannot retain a worker or the session write mutex indefinitely.
    std::chrono::milliseconds responseWriteStallTimeout{30000};
    std::string softwareVersion = "unknown";
    // Per-session access token. Clients must present a byte-identical token in
    // their handshake or the connection is refused. Left empty, the server
    // generates a fresh random token at construction; there is no way to
    // disable the check. See token().
    std::string sessionToken;
};

class Server {
public:
    explicit Server(ServerOptions options = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::string lastError() const;
    // The access token clients must present. Either the token supplied in
    // ServerOptions or, when that was empty, the one generated at construction.
    [[nodiscard]] const std::string& token() const noexcept;
    void run();
    void requestStop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace amrvis::remote
