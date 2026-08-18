#pragma once

#include <amrexplorer/core/Volume.hpp>
#include <amrexplorer/data/LocalDatasetSession.hpp>
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
    // A peer that makes no write progress for this interval is disconnected so
    // it cannot retain a worker or the session write mutex indefinitely. It is
    // also the fixed grace in the whole-response budget below.
    std::chrono::milliseconds responseWriteStallTimeout{30000};
    // The stall timeout alone bounds only a period of *zero* progress, which a
    // peer defeats by accepting a few bytes before each deadline. Every response
    // therefore also has to fit in
    //   responseWriteStallTimeout + size / responseWriteMinimumBytesPerSecond,
    // so a trickle-reader is retired within a bound the operator can compute.
    // The default floor, 64 KiB/s, is far below any usable SSH link; lower it
    // for a genuinely slow link (1 allows roughly 18 hours for a 64 MiB
    // response). Zero is rejected: it would restore the unbounded case.
    std::uint64_t responseWriteMinimumBytesPerSecond = 64U * 1024U;
    // Volume rendering (protocol 1.2): the most voxels a client may ask the
    // server to sample per request (its own budget is clamped to this), and
    // the per-dataset cache of sampled grids a rotating client re-casts
    // from. Both bound the server's memory: a grid is four bytes per voxel.
    std::uint64_t maximumVolumeVoxels = defaultVolumeVoxelBudget;
    std::uint64_t volumeGridCacheBytes = defaultVolumeGridCacheBytes;
    std::string softwareVersion = "unknown";
    // Per-session access token. Clients must present a byte-identical token in
    // their handshake or the connection is refused. Left empty, the server
    // generates a fresh random token at construction; there is no way to
    // disable the check. See token().
    std::string sessionToken;
};

class Server {
public:
    // Listens on loopback at options.port (0 = kernel-assigned) and serves
    // every accepted connection.
    explicit Server(ServerOptions options = {});
    // Serves exactly one peer over an already-connected channel, such as the
    // process's own stdin/stdout when launched over ssh; never listens, so
    // port() is 0 and options.port is ignored. run() returns when that
    // session ends.
    Server(std::unique_ptr<Channel> channel, ServerOptions options = {});
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
