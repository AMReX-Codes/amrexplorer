#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace amrvis::remote {

inline constexpr std::uint16_t protocolMajor = 1;
inline constexpr std::uint16_t protocolMinor = 1;
inline constexpr std::uint32_t defaultMaximumFrameBytes
    = 128U * 1024U * 1024U;

enum class FrameCompression : std::uint8_t {
    None,
    Zstd
};

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
    std::uint32_t maximumBytes = defaultMaximumFrameBytes,
    FrameCompression compression = FrameCompression::None);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> readFrame(
    const Socket& socket,
    std::uint32_t maximumBytes = defaultMaximumFrameBytes,
    FrameCompression compression = FrameCompression::None);

} // namespace amrvis::remote
