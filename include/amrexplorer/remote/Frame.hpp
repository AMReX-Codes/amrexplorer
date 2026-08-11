#pragma once

#include <amrexplorer/core/StopToken.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace amrvis::remote {

inline constexpr std::uint32_t defaultMaximumFrameBytes
    = 128U * 1024U * 1024U;

class Socket {
public:
    using Native = std::intptr_t;

    Socket() = default;
    explicit Socket(Native descriptor) noexcept;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] Native descriptor() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    void shutdown() noexcept;
    void close() noexcept;

private:
    Native m_descriptor = -1;
};

struct Listener {
    Socket socket;
    std::uint16_t port = 0;
};

[[nodiscard]] Listener listenOnLoopback(
    std::uint16_t port, int backlog = 16);
[[nodiscard]] Socket acceptConnection(
    const Socket& listener, StopToken cancellation = {});
[[nodiscard]] bool isNumericAddress(const std::string& host) noexcept;
// Connections accept numeric IPv4/IPv6 addresses only. Remote deployments use
// a loopback SSH tunnel, and avoiding hostname resolution keeps the deadline
// and cancellation guarantees portable.
[[nodiscard]] Socket connectTo(const std::string& host, std::uint16_t port);
[[nodiscard]] Socket connectTo(const std::string& host, std::uint16_t port,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation = {});

void writeFrame(const Socket& socket, std::span<const std::uint8_t> payload,
    std::uint32_t maximumBytes = defaultMaximumFrameBytes);
void writeFrame(const Socket& socket, std::span<const std::uint8_t> payload,
    std::uint32_t maximumBytes,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation = {}, StopToken lifecycle = {});
// How long one frame write may take. Two limits, because either alone can be
// defeated: stallTimeout bounds a period of no progress at all, and total()
// bounds the whole write, so a peer that accepts a few bytes just before every
// stall deadline -- renewing it forever -- is still retired. The whole-write
// bound is deliberately generous: the stall interval as a fixed grace plus the
// time the payload needs at an assumed floor throughput, so a slow but healthy
// link is not mistaken for a wedged peer.
struct FrameWriteBudget {
    std::chrono::milliseconds stallTimeout{30000};
    // Bytes per second. Must be positive: zero would restore the unbounded
    // case. Lower it for a genuinely slow link rather than removing the bound.
    std::uint64_t minimumBytesPerSecond = 64U * 1024U;

    [[nodiscard]] std::chrono::milliseconds total(
        std::size_t payloadBytes) const noexcept;
};

// Writes may take arbitrarily long while the peer continues accepting bytes, up
// to budget.total(payload.size()). Fails on a stall, on falling below the
// budget's floor throughput, on cancellation, or on a socket error.
void writeFrameWithBudget(const Socket& socket,
    std::span<const std::uint8_t> payload, std::uint32_t maximumBytes,
    const FrameWriteBudget& budget,
    StopToken cancellation = {}, StopToken lifecycle = {});
[[nodiscard]] std::optional<std::vector<std::uint8_t>> readFrame(
    const Socket& socket,
    std::uint32_t maximumBytes = defaultMaximumFrameBytes);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> readFrame(
    const Socket& socket, std::uint32_t maximumBytes,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation = {});

} // namespace amrvis::remote
