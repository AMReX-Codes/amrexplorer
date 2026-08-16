#include <amrexplorer/remote/Frame.hpp>

#include <amrexplorer/io/PlotfileBlockReader.hpp>

#ifdef AMREXPLORER_SERVER_TEST_HOOKS
#include "ServerTestHooks.hpp"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace amrvis::remote {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
using SocketLength = int;
constexpr NativeSocket invalidSocket = INVALID_SOCKET;

struct WinsockRuntime {
    WinsockRuntime()
    {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }
    ~WinsockRuntime() { ::WSACleanup(); }
};

void ensureSockets()
{
    static WinsockRuntime runtime;
    static_cast<void>(runtime);
}

int lastSocketError()
{
    return ::WSAGetLastError();
}

bool interrupted(int error)
{
    return error == WSAEINTR;
}

bool transientAcceptFailure(int error)
{
    return interrupted(error) || error == WSAECONNABORTED;
}

bool connectInProgress(int error)
{
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS
        || error == WSAEINVAL;
}

bool wouldBlock(int error)
{
    return error == WSAEWOULDBLOCK;
}

void closeNative(NativeSocket socket)
{
    ::closesocket(socket);
}
#else
using NativeSocket = int;
using SocketLength = socklen_t;
constexpr NativeSocket invalidSocket = -1;

void ensureSockets() {}

int lastSocketError()
{
    return errno;
}

bool interrupted(int error)
{
    return error == EINTR;
}

bool transientAcceptFailure(int error)
{
    return interrupted(error) || error == ECONNABORTED;
}

bool connectInProgress(int error)
{
    return error == EINPROGRESS || error == EWOULDBLOCK;
}

bool wouldBlock(int error)
{
    return error == EAGAIN || error == EWOULDBLOCK;
}

void closeNative(NativeSocket socket)
{
    ::close(socket);
}
#endif

NativeSocket native(Socket::Native descriptor)
{
    return static_cast<NativeSocket>(descriptor);
}

[[noreturn]] void throwSocketError(
    const std::string& operation, int error = lastSocketError());

void setNonBlocking(NativeSocket socket, bool enabled)
{
#ifdef _WIN32
    u_long mode = enabled ? 1UL : 0UL;
    if (::ioctlsocket(socket, FIONBIO, &mode) != 0) {
        throwSocketError("ioctlsocket(FIONBIO)");
    }
#else
    const auto flags = ::fcntl(socket, F_GETFL, 0);
    if (flags < 0
        || ::fcntl(socket, F_SETFL,
               enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK)
            != 0) {
        throwSocketError("fcntl(O_NONBLOCK)");
    }
#endif
}

int waitForConnect(NativeSocket socket,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation)
{
    using namespace std::chrono_literals;
    for (;;) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw std::runtime_error("connection attempt timed out");
        }
        auto wait = 50ms;
        if (deadline != std::chrono::steady_clock::time_point::max()) {
            wait = std::min(wait,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now));
        }
#ifdef _WIN32
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(wait.count() / 1000);
        timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(
            (wait.count() % 1000) * 1000);
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(socket, &writable);
        const auto ready = ::select(0, nullptr, &writable, nullptr, &timeout);
#else
        pollfd descriptor{socket, POLLOUT, 0};
        const auto ready = ::poll(&descriptor, 1, static_cast<int>(wait.count()));
#endif
        if (ready == 0) {
            continue;
        }
        if (ready < 0) {
            const auto error = lastSocketError();
            if (interrupted(error)) {
                continue;
            }
            return error;
        }
        int error = 0;
        SocketLength size = static_cast<SocketLength>(sizeof(error));
#ifdef _WIN32
        auto* bytes = reinterpret_cast<char*>(&error);
#else
        auto* bytes = &error;
#endif
        if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, bytes, &size) != 0) {
            return lastSocketError();
        }
        return error;
    }
}

void setIntegerSocketOption(NativeSocket descriptor, int level, int option,
    int value, const char* name)
{
#ifdef _WIN32
    const auto* bytes = reinterpret_cast<const char*>(&value);
#else
    const auto* bytes = &value;
#endif
    if (::setsockopt(descriptor, level, option, bytes, sizeof(value)) != 0) {
        throwSocketError(std::string("setsockopt(") + name + ')');
    }
}

void configureConnectedSocket(NativeSocket descriptor)
{
#ifdef SO_NOSIGPIPE
    setIntegerSocketOption(
        descriptor, SOL_SOCKET, SO_NOSIGPIPE, 1, "SO_NOSIGPIPE");
#endif
    setIntegerSocketOption(
        descriptor, IPPROTO_TCP, TCP_NODELAY, 1, "TCP_NODELAY");
    // Keep connected sockets nonblocking. All frame reads/writes wait through
    // poll/select first, which makes their deadline and shutdown paths
    // authoritative instead of allowing a kernel send/recv to block forever.
    setNonBlocking(descriptor, true);
}

[[noreturn]] void throwSocketError(
    const std::string& operation, int error)
{
#ifdef _WIN32
    throw std::runtime_error(
        operation + " failed with socket error " + std::to_string(error));
#else
    throw std::runtime_error(
        operation + ": " + std::string(std::strerror(error)));
#endif
}

void waitForReadable(NativeSocket socket,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation)
{
    using namespace std::chrono_literals;
    for (;;) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto now = std::chrono::steady_clock::now();
        if (deadline != std::chrono::steady_clock::time_point::max()
            && now >= deadline) {
            throw std::runtime_error("wire frame read timed out");
        }
        auto wait = 50ms;
        if (deadline != std::chrono::steady_clock::time_point::max()) {
            wait = std::min(wait,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now));
            wait = std::max(wait, 1ms);
        }
#ifdef _WIN32
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(wait.count() / 1000);
        timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(
            (wait.count() % 1000) * 1000);
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket, &readable);
        const auto ready = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
        pollfd descriptor{socket, POLLIN, 0};
        const auto ready
            = ::poll(&descriptor, 1, static_cast<int>(wait.count()));
#endif
        if (ready > 0) {
            return;
        }
        if (ready == 0) {
            continue;
        }
        const auto error = lastSocketError();
        if (!interrupted(error)) {
            throwSocketError("socket readiness wait", error);
        }
    }
}

void waitForWritable(NativeSocket socket,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation, StopToken lifecycle,
    const char* timeoutMessage)
{
    using namespace std::chrono_literals;
    for (;;) {
        if (cancellation.stop_requested() || lifecycle.stop_requested()) {
            throw ReadCancelled();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw std::runtime_error(timeoutMessage);
        }
        auto wait = 50ms;
        if (deadline != std::chrono::steady_clock::time_point::max()) {
            wait = std::min(wait,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now));
            wait = std::max(wait, 1ms);
        }
#ifdef _WIN32
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(wait.count() / 1000);
        timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(
            (wait.count() % 1000) * 1000);
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(socket, &writable);
        const auto ready = ::select(0, nullptr, &writable, nullptr, &timeout);
#else
        pollfd descriptor{socket, POLLOUT, 0};
        const auto ready = ::poll(
            &descriptor, 1, static_cast<int>(wait.count()));
#endif
        if (ready > 0) {
            return;
        }
        if (ready == 0) {
            continue;
        }
        const auto error = lastSocketError();
        if (!interrupted(error)) {
            throwSocketError("socket write readiness wait", error);
        }
    }
}

bool readExact(const Channel& channel, std::span<std::uint8_t> destination,
    bool allowCleanEof,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation)
{
    std::size_t completed = 0;
    while (completed < destination.size()) {
        const auto count = channel.readSome(
            destination.subspan(completed), deadline, cancellation);
        if (count == 0) {
            if (completed == 0 && allowCleanEof) {
                return false;
            }
            throw std::runtime_error("connection closed inside a wire frame");
        }
        completed += count;
    }
    return true;
}

void writeExact(const Channel& channel,
    std::span<const std::uint8_t> source,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation, StopToken lifecycle,
    std::optional<std::chrono::milliseconds> stallTimeout = std::nullopt)
{
    std::size_t completed = 0;
    auto progressDeadline = stallTimeout
        ? std::chrono::steady_clock::now() + *stallTimeout
        : std::chrono::steady_clock::time_point::max();
    while (completed < source.size()) {
        // Whichever limit bites first, and the message that explains it: with a
        // stall timeout the absolute deadline is the whole-write budget, so
        // hitting it means the peer stayed under the floor throughput rather
        // than stopping outright.
        const auto limit = std::min(deadline, progressDeadline);
        const auto* reason = "wire frame write timed out";
        if (stallTimeout) {
            reason = limit == progressDeadline
                ? "wire frame write stalled"
                : "wire frame write stayed below the minimum throughput";
        }
        auto offered = source.size() - completed;
#ifdef AMREXPLORER_SERVER_TEST_HOOKS
        offered = testing::writeChunkLimit(offered);
#endif
        const auto count = channel.writeSome(source.subspan(completed, offered),
            limit, cancellation, lifecycle, reason);
        completed += count;
        if (stallTimeout) {
            progressDeadline
                = std::chrono::steady_clock::now() + *stallTimeout;
        }
    }
}

void writeExact(const Channel& channel, std::span<const std::uint8_t> source)
{
    writeExact(channel, source,
        std::chrono::steady_clock::time_point::max(), {}, {});
}

} // namespace

Socket::Socket(Native descriptor) noexcept
    : m_descriptor(descriptor)
{
}

Socket::~Socket()
{
    close();
}

Socket::Socket(Socket&& other) noexcept
    : m_descriptor(std::exchange(other.m_descriptor, -1))
{
}

Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other) {
        close();
        m_descriptor = std::exchange(other.m_descriptor, -1);
    }
    return *this;
}

Socket::Native Socket::descriptor() const noexcept
{
    return m_descriptor;
}

bool Socket::valid() const noexcept
{
    return m_descriptor != -1;
}

void Socket::shutdown() noexcept
{
    if (!valid()) {
        return;
    }
#ifdef _WIN32
    ::shutdown(native(m_descriptor), SD_BOTH);
#else
    ::shutdown(native(m_descriptor), SHUT_RDWR);
#endif
}

void Socket::close() noexcept
{
    if (valid()) {
        closeNative(native(m_descriptor));
        m_descriptor = -1;
    }
}

std::size_t Socket::readSome(std::span<std::uint8_t> destination,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation) const
{
    const auto descriptor = native(m_descriptor);
    for (;;) {
        waitForReadable(descriptor, deadline, cancellation);
#ifdef _WIN32
        const auto remaining = std::min<std::size_t>(destination.size(),
            static_cast<std::size_t>(std::numeric_limits<int>::max()));
        const auto count = ::recv(descriptor,
            reinterpret_cast<char*>(destination.data()),
            static_cast<int>(remaining), 0);
#else
        const auto count
            = ::recv(descriptor, destination.data(), destination.size(), 0);
#endif
        if (count >= 0) {
            return static_cast<std::size_t>(count);
        }
        const auto error = lastSocketError();
        if (interrupted(error) || wouldBlock(error)) {
            continue;
        }
        throwSocketError("recv", error);
    }
}

std::size_t Socket::writeSome(std::span<const std::uint8_t> source,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation, StopToken lifecycle,
    const char* timeoutMessage) const
{
    const auto descriptor = native(m_descriptor);
    for (;;) {
        waitForWritable(
            descriptor, deadline, cancellation, lifecycle, timeoutMessage);
#ifdef _WIN32
        const auto remaining = std::min<std::size_t>(source.size(), 64U * 1024U);
        const auto count = ::send(descriptor,
            reinterpret_cast<const char*>(source.data()),
            static_cast<int>(remaining), 0);
#else
        constexpr int noSignal =
#ifdef MSG_NOSIGNAL
            MSG_NOSIGNAL |
#endif
            MSG_DONTWAIT;
        const auto count
            = ::send(descriptor, source.data(), source.size(), noSignal);
#endif
        if (count < 0) {
            const auto error = lastSocketError();
            if (interrupted(error) || wouldBlock(error)) {
                continue;
            }
            throwSocketError("send", error);
        }
        if (count == 0) {
            throw std::runtime_error(
                "connection closed during wire frame write");
        }
        return static_cast<std::size_t>(count);
    }
}

Socket adoptStreamSocket(Socket::Native descriptor)
{
    ensureSockets();
    Socket socket(descriptor);
    if (!socket.valid() || native(descriptor) == invalidSocket) {
        throw std::runtime_error("adoptStreamSocket: invalid descriptor");
    }
#ifdef SO_NOSIGPIPE
    setIntegerSocketOption(
        native(descriptor), SOL_SOCKET, SO_NOSIGPIPE, 1, "SO_NOSIGPIPE");
#endif
    setNonBlocking(native(descriptor), true);
    return socket;
}

#ifndef _WIN32
DescriptorChannel::DescriptorChannel(int readDescriptor, int writeDescriptor)
    : m_read(readDescriptor)
    , m_write(writeDescriptor)
{
    try {
        if (m_read < 0 || m_write < 0) {
            throw std::runtime_error("DescriptorChannel: invalid descriptor");
        }
        // Both directions wait through poll first; a blocking write of a large
        // frame into a pipe would otherwise ignore the deadline entirely.
        setNonBlocking(m_read, true);
        if (m_write != m_read) {
            setNonBlocking(m_write, true);
        }
    } catch (...) {
        close();
        throw;
    }
}

DescriptorChannel::~DescriptorChannel()
{
    close();
}

int DescriptorChannel::readDescriptor() const noexcept
{
    return m_read;
}

int DescriptorChannel::writeDescriptor() const noexcept
{
    return m_write;
}

std::size_t DescriptorChannel::readSome(std::span<std::uint8_t> destination,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation) const
{
    for (;;) {
        waitForReadable(m_read, deadline, cancellation);
        const auto count
            = ::read(m_read, destination.data(), destination.size());
        if (count >= 0) {
            return static_cast<std::size_t>(count);
        }
        const auto error = errno;
        if (interrupted(error) || wouldBlock(error)) {
            continue;
        }
        throwSocketError("read", error);
    }
}

std::size_t DescriptorChannel::writeSome(std::span<const std::uint8_t> source,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation, StopToken lifecycle,
    const char* timeoutMessage) const
{
    for (;;) {
        waitForWritable(
            m_write, deadline, cancellation, lifecycle, timeoutMessage);
        // No MSG_NOSIGNAL for write(); a process using this channel is
        // expected to ignore SIGPIPE so a departed peer surfaces as EPIPE.
        const auto count = ::write(m_write, source.data(), source.size());
        if (count < 0) {
            const auto error = errno;
            if (interrupted(error) || wouldBlock(error)) {
                continue;
            }
            throwSocketError("write", error);
        }
        if (count == 0) {
            throw std::runtime_error(
                "connection closed during wire frame write");
        }
        return static_cast<std::size_t>(count);
    }
}

void DescriptorChannel::shutdown() noexcept
{
    if (m_write < 0) {
        return;
    }
    struct stat status {};
    if (::fstat(m_write, &status) == 0 && S_ISSOCK(status.st_mode)) {
        // The read and write descriptors may share one socket; closing the
        // write descriptor alone would then leave the socket open. Half-closing
        // delivers end of stream to the peer either way.
        ::shutdown(m_write, SHUT_WR);
        return;
    }
    if (m_write != m_read) {
        ::close(m_write);
    }
    m_write = -1;
}

void DescriptorChannel::close() noexcept
{
    if (m_write >= 0 && m_write != m_read) {
        ::close(m_write);
    }
    m_write = -1;
    if (m_read >= 0) {
        ::close(m_read);
        m_read = -1;
    }
}
#endif

Listener listenOnLoopback(std::uint16_t port, int backlog)
{
    ensureSockets();
    Socket socket(static_cast<Socket::Native>(
        ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
    if (!socket.valid()
        || native(socket.descriptor()) == invalidSocket) {
        throwSocketError("socket");
    }
#ifdef _WIN32
    setIntegerSocketOption(native(socket.descriptor()), SOL_SOCKET,
        SO_EXCLUSIVEADDRUSE, 1, "SO_EXCLUSIVEADDRUSE");
#else
    setIntegerSocketOption(native(socket.descriptor()), SOL_SOCKET,
        SO_REUSEADDR, 1, "SO_REUSEADDR");
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(native(socket.descriptor()),
            reinterpret_cast<const sockaddr*>(&address), sizeof(address))
        != 0) {
        throwSocketError("bind");
    }
    if (::listen(native(socket.descriptor()), backlog) != 0) {
        throwSocketError("listen");
    }
    setNonBlocking(native(socket.descriptor()), true);

    SocketLength size = static_cast<SocketLength>(sizeof(address));
    if (::getsockname(native(socket.descriptor()),
            reinterpret_cast<sockaddr*>(&address), &size)
        != 0) {
        throwSocketError("getsockname");
    }
    return {std::move(socket), ntohs(address.sin_port)};
}

Socket acceptConnection(const Socket& listener, StopToken cancellation)
{
    ensureSockets();
    for (;;) {
        waitForReadable(native(listener.descriptor()),
            std::chrono::steady_clock::time_point::max(), cancellation);
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        const auto descriptor
            = ::accept(native(listener.descriptor()), nullptr, nullptr);
        if (descriptor != invalidSocket) {
            Socket socket(static_cast<Socket::Native>(descriptor));
            configureConnectedSocket(descriptor);
            return socket;
        }
        const auto error = lastSocketError();
        if (!transientAcceptFailure(error) && !wouldBlock(error)) {
            throwSocketError("accept", error);
        }
    }
}

bool isNumericAddress(const std::string& host) noexcept
{
    try {
        ensureSockets();
    } catch (...) {
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo* addresses = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &addresses) != 0) {
        return false;
    }
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addressOwner(
        addresses, &::freeaddrinfo);
    return addresses != nullptr;
}

Socket connectTo(const std::string& host, std::uint16_t port)
{
    return connectTo(host, port,
        std::chrono::steady_clock::time_point::max(), {});
}

Socket connectTo(const std::string& host, std::uint16_t port,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation)
{
    ensureSockets();
    if (cancellation.stop_requested()) {
        throw ReadCancelled();
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        throw std::runtime_error("connection attempt timed out");
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    const auto status
        = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (status != 0) {
        throw std::runtime_error("remote host must be a numeric IPv4 or IPv6 "
                                 "address: "
            + std::string(gai_strerror(status)));
    }
    if (addresses == nullptr) {
        throw std::runtime_error(
            "getaddrinfo succeeded without returning a usable address");
    }
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addressOwner(
        addresses, &::freeaddrinfo);
    int lastError = 0;
    for (auto* address = addresses; address != nullptr;
         address = address->ai_next) {
        Socket socket(static_cast<Socket::Native>(::socket(address->ai_family,
            address->ai_socktype, address->ai_protocol)));
        if (!socket.valid()
            || native(socket.descriptor()) == invalidSocket) {
            lastError = lastSocketError();
            continue;
        }
        const auto descriptor = native(socket.descriptor());
        setNonBlocking(descriptor, true);
        if (::connect(descriptor, address->ai_addr,
                static_cast<SocketLength>(address->ai_addrlen))
            == 0) {
            setNonBlocking(descriptor, false);
            configureConnectedSocket(descriptor);
            return socket;
        }
        lastError = lastSocketError();
        if (!connectInProgress(lastError)) {
            continue;
        }
        lastError = waitForConnect(descriptor, deadline, cancellation);
        if (lastError == 0) {
            setNonBlocking(descriptor, false);
            configureConnectedSocket(descriptor);
            return socket;
        }
    }
    throwSocketError("connect", lastError);
}

void writeFrame(const Channel& channel, std::span<const std::uint8_t> payload,
    std::uint32_t maximumBytes)
{
    if (payload.empty() || payload.size() > maximumBytes
        || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("wire frame size is outside the allowed range");
    }
    const auto networkSize = htonl(static_cast<std::uint32_t>(payload.size()));
    const auto* sizeBytes
        = reinterpret_cast<const std::uint8_t*>(&networkSize);
    writeExact(channel,
        std::span<const std::uint8_t>(sizeBytes, sizeof(networkSize)));
    writeExact(channel, payload);
}

void writeFrame(const Channel& channel, std::span<const std::uint8_t> payload,
    std::uint32_t maximumBytes,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation, StopToken lifecycle)
{
    if (payload.empty() || payload.size() > maximumBytes
        || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("wire frame size is outside the allowed range");
    }
    const auto networkSize = htonl(static_cast<std::uint32_t>(payload.size()));
    const auto* sizeBytes
        = reinterpret_cast<const std::uint8_t*>(&networkSize);
    writeExact(channel,
        std::span<const std::uint8_t>(sizeBytes, sizeof(networkSize)),
        deadline, cancellation, lifecycle);
    writeExact(channel, payload, deadline, cancellation,
        lifecycle);
}

std::chrono::milliseconds FrameWriteBudget::total(
    std::size_t payloadBytes) const noexcept
{
    // Fixed grace plus the payload's time at the floor throughput, rounded up.
    // The floor is clamped rather than trusted: a zero would mean no bound at
    // all, which is the defect this budget exists to close.
    const std::uint64_t floor = minimumBytesPerSecond > 0
        ? minimumBytesPerSecond : 1;
    const auto transfer
        = (static_cast<std::uint64_t>(payloadBytes) * 1000ULL + floor - 1ULL)
        / floor;
    const std::uint64_t grace = stallTimeout > std::chrono::milliseconds::zero()
        ? static_cast<std::uint64_t>(stallTimeout.count()) : 0ULL;
    // Thirty days is beyond any real transfer and keeps now() + total() far
    // from overflowing a steady_clock time point.
    constexpr std::uint64_t cap = 30ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
    const auto bounded = std::min<std::uint64_t>(cap, transfer);
    const auto total = grace > cap - bounded ? cap : grace + bounded;
    return std::chrono::milliseconds(
        static_cast<std::chrono::milliseconds::rep>(total));
}

void writeFrameWithBudget(const Channel& channel,
    std::span<const std::uint8_t> payload, std::uint32_t maximumBytes,
    const FrameWriteBudget& budget, StopToken cancellation,
    StopToken lifecycle)
{
    if (payload.empty() || payload.size() > maximumBytes
        || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("wire frame size is outside the allowed range");
    }
    if (budget.stallTimeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "wire frame write stall timeout must be greater than zero");
    }
    if (budget.minimumBytesPerSecond == 0) {
        throw std::invalid_argument(
            "wire frame write minimum throughput must be greater than zero");
    }
    // One deadline for the whole frame, so the four-byte header cannot buy the
    // payload extra time.
    const auto deadline
        = std::chrono::steady_clock::now() + budget.total(payload.size());
    const auto networkSize = htonl(static_cast<std::uint32_t>(payload.size()));
    const auto* sizeBytes
        = reinterpret_cast<const std::uint8_t*>(&networkSize);
    writeExact(channel,
        std::span<const std::uint8_t>(sizeBytes, sizeof(networkSize)),
        deadline, cancellation, lifecycle, budget.stallTimeout);
    writeExact(channel, payload, deadline, cancellation,
        lifecycle, budget.stallTimeout);
}

std::optional<std::vector<std::uint8_t>> readFrame(
    const Channel& channel, std::uint32_t maximumBytes)
{
    return readFrame(channel, maximumBytes,
        std::chrono::steady_clock::time_point::max(), {});
}

std::optional<std::vector<std::uint8_t>> readFrame(const Channel& channel,
    std::uint32_t maximumBytes,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation)
{
    std::array<std::uint8_t, sizeof(std::uint32_t)> sizeBytes{};
    if (!readExact(channel, sizeBytes, true, deadline, cancellation)) {
        return std::nullopt;
    }
    std::uint32_t networkSize = 0;
    std::memcpy(&networkSize, sizeBytes.data(), sizeof(networkSize));
    const auto size = ntohl(networkSize);
    if (size == 0 || size > maximumBytes) {
        throw std::runtime_error("wire frame size is outside the allowed range");
    }
    std::vector<std::uint8_t> payload(size);
    static_cast<void>(readExact(channel, payload, false, deadline, cancellation));
    return payload;
}

} // namespace amrvis::remote
