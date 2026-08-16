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

// A bidirectional byte stream the frame layer reads and writes. Implementations
// are a connected socket (loopback TCP, or an AF_UNIX pair handed to a child
// process) and, on POSIX, a plain descriptor pair such as a process's own
// stdin/stdout. Both calls block only through a readiness wait, so the
// deadline and the stop tokens stay authoritative.
class Channel {
public:
    virtual ~Channel() = default;

    // Reads at least one byte, or returns 0 at end of stream. Throws
    // std::runtime_error at the deadline and ReadCancelled on cancellation.
    [[nodiscard]] virtual std::size_t readSome(
        std::span<std::uint8_t> destination,
        std::chrono::steady_clock::time_point deadline,
        StopToken cancellation) const = 0;
    // Writes at least one byte. Throws std::runtime_error(timeoutMessage) at
    // the deadline and ReadCancelled when either token stops.
    [[nodiscard]] virtual std::size_t writeSome(
        std::span<const std::uint8_t> source,
        std::chrono::steady_clock::time_point deadline,
        StopToken cancellation, StopToken lifecycle,
        const char* timeoutMessage) const = 0;
    // Interrupts blocked readers on both ends where the transport allows it.
    virtual void shutdown() noexcept = 0;
    virtual void close() noexcept = 0;
};

class Socket : public Channel {
public:
    using Native = std::intptr_t;

    Socket() = default;
    explicit Socket(Native descriptor) noexcept;
    ~Socket() override;

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] Native descriptor() const noexcept;
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] std::size_t readSome(std::span<std::uint8_t> destination,
        std::chrono::steady_clock::time_point deadline,
        StopToken cancellation) const override;
    [[nodiscard]] std::size_t writeSome(std::span<const std::uint8_t> source,
        std::chrono::steady_clock::time_point deadline,
        StopToken cancellation, StopToken lifecycle,
        const char* timeoutMessage) const override;
    void shutdown() noexcept override;
    void close() noexcept override;

private:
    Native m_descriptor = -1;
};

// Takes ownership of an already-connected stream socket of any family (for
// example one end of a socketpair) and configures it the way the frame layer
// expects: nonblocking, and immune to SIGPIPE where the platform offers a
// socket option for that. Unlike an accepted or connected TCP socket it sets no
// TCP-specific options.
[[nodiscard]] Socket adoptStreamSocket(Socket::Native descriptor);

#ifndef _WIN32
// A read descriptor and a write descriptor, such as a process's own stdin and
// stdout, or the two ends of a pipe pair. They may also be two descriptors on
// the same open socket, which is what sshd gives a command without a pty on
// Linux; shutdown() then half-closes the socket instead of closing one
// descriptor, so the peer still sees end of stream. Takes ownership of both.
class DescriptorChannel final : public Channel {
public:
    DescriptorChannel(int readDescriptor, int writeDescriptor);
    ~DescriptorChannel() override;

    DescriptorChannel(const DescriptorChannel&) = delete;
    DescriptorChannel& operator=(const DescriptorChannel&) = delete;

    [[nodiscard]] int readDescriptor() const noexcept;
    [[nodiscard]] int writeDescriptor() const noexcept;

    [[nodiscard]] std::size_t readSome(std::span<std::uint8_t> destination,
        std::chrono::steady_clock::time_point deadline,
        StopToken cancellation) const override;
    [[nodiscard]] std::size_t writeSome(std::span<const std::uint8_t> source,
        std::chrono::steady_clock::time_point deadline,
        StopToken cancellation, StopToken lifecycle,
        const char* timeoutMessage) const override;
    void shutdown() noexcept override;
    void close() noexcept override;

private:
    int m_read = -1;
    int m_write = -1;
};
#endif

struct Listener {
    Socket socket;
    std::uint16_t port = 0;
};

[[nodiscard]] Listener listenOnLoopback(
    std::uint16_t port, int backlog = 16);
[[nodiscard]] Socket acceptConnection(
    const Socket& listener, StopToken cancellation = {});
[[nodiscard]] bool isNumericAddress(const std::string& host) noexcept;
// Connections accept numeric IPv4/IPv6 addresses only. The loopback listener
// serves tests and tools on one host, and avoiding hostname resolution keeps
// the deadline and cancellation guarantees portable.
[[nodiscard]] Socket connectTo(const std::string& host, std::uint16_t port);
[[nodiscard]] Socket connectTo(const std::string& host, std::uint16_t port,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation = {});

void writeFrame(const Channel& channel, std::span<const std::uint8_t> payload,
    std::uint32_t maximumBytes = defaultMaximumFrameBytes);
void writeFrame(const Channel& channel, std::span<const std::uint8_t> payload,
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
// budget's floor throughput, on cancellation, or on a transport error.
void writeFrameWithBudget(const Channel& channel,
    std::span<const std::uint8_t> payload, std::uint32_t maximumBytes,
    const FrameWriteBudget& budget,
    StopToken cancellation = {}, StopToken lifecycle = {});
[[nodiscard]] std::optional<std::vector<std::uint8_t>> readFrame(
    const Channel& channel,
    std::uint32_t maximumBytes = defaultMaximumFrameBytes);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> readFrame(
    const Channel& channel, std::uint32_t maximumBytes,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation = {});

} // namespace amrvis::remote
