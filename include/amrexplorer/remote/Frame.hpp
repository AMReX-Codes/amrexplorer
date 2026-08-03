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
[[nodiscard]] Socket acceptConnection(const Socket& listener);
[[nodiscard]] Socket connectTo(const std::string& host, std::uint16_t port);

void writeFrame(const Socket& socket, std::span<const std::uint8_t> payload,
    std::uint32_t maximumBytes = defaultMaximumFrameBytes);
void writeFrame(const Socket& socket, std::span<const std::uint8_t> payload,
    std::uint32_t maximumBytes,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation = {});
// Writes may take arbitrarily long while the peer continues accepting bytes.
// The operation fails only when no bytes can be written for stallTimeout.
void writeFrameWithStallTimeout(const Socket& socket,
    std::span<const std::uint8_t> payload, std::uint32_t maximumBytes,
    std::chrono::milliseconds stallTimeout,
    StopToken cancellation = {});
[[nodiscard]] std::optional<std::vector<std::uint8_t>> readFrame(
    const Socket& socket,
    std::uint32_t maximumBytes = defaultMaximumFrameBytes);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> readFrame(
    const Socket& socket, std::uint32_t maximumBytes,
    std::chrono::steady_clock::time_point deadline);

} // namespace amrvis::remote
