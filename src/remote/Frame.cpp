#include <amrexplorer/remote/Frame.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
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

bool readExact(NativeSocket descriptor, std::span<std::uint8_t> destination,
    bool allowCleanEof,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation)
{
    std::size_t completed = 0;
    while (completed < destination.size()) {
        waitForReadable(descriptor, deadline, cancellation);
#ifdef _WIN32
        const auto remaining = std::min<std::size_t>(
            destination.size() - completed,
            static_cast<std::size_t>(std::numeric_limits<int>::max()));
        const auto count = ::recv(descriptor,
            reinterpret_cast<char*>(destination.data() + completed),
            static_cast<int>(remaining), 0);
#else
        const auto count = ::recv(descriptor, destination.data() + completed,
            destination.size() - completed, 0);
#endif
        if (count == 0) {
            if (completed == 0 && allowCleanEof) {
                return false;
            }
            throw std::runtime_error("connection closed inside a wire frame");
        }
        if (count < 0) {
            const auto error = lastSocketError();
            if (interrupted(error) || wouldBlock(error)) {
                continue;
            }
            throwSocketError("recv", error);
        }
        completed += static_cast<std::size_t>(count);
    }
    return true;
}

void writeExact(
    NativeSocket descriptor, std::span<const std::uint8_t> source);

void writeExact(NativeSocket descriptor,
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
        waitForWritable(descriptor, std::min(deadline, progressDeadline),
            cancellation, lifecycle,
            stallTimeout ? "wire frame write stalled"
                         : "wire frame write timed out");
#ifdef _WIN32
        const auto remaining = std::min<std::size_t>(
            source.size() - completed, 64U * 1024U);
        const auto count = ::send(descriptor,
            reinterpret_cast<const char*>(source.data() + completed),
            static_cast<int>(remaining), 0);
#else
        constexpr int noSignal =
#ifdef MSG_NOSIGNAL
            MSG_NOSIGNAL |
#endif
            MSG_DONTWAIT;
        const auto count = ::send(descriptor, source.data() + completed,
            source.size() - completed, noSignal);
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
        completed += static_cast<std::size_t>(count);
        if (stallTimeout) {
            progressDeadline
                = std::chrono::steady_clock::now() + *stallTimeout;
        }
    }
}

void writeExact(
    NativeSocket descriptor, std::span<const std::uint8_t> source)
{
    writeExact(descriptor, source,
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
        if (!interrupted(error) && !wouldBlock(error)) {
            throwSocketError("accept", error);
        }
    }
}

Socket connectTo(const std::string& host, std::uint16_t port)
{
    ensureSockets();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    const auto status
        = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (status != 0) {
        throw std::runtime_error(
            "getaddrinfo: " + std::string(gai_strerror(status)));
    }
    if (addresses == nullptr) {
        throw std::runtime_error(
            "getaddrinfo succeeded without returning a usable address");
    }

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
        if (::connect(native(socket.descriptor()), address->ai_addr,
                static_cast<SocketLength>(address->ai_addrlen))
            == 0) {
            ::freeaddrinfo(addresses);
            configureConnectedSocket(native(socket.descriptor()));
            return socket;
        }
        lastError = lastSocketError();
    }
    ::freeaddrinfo(addresses);
    throwSocketError("connect", lastError);
}

void writeFrame(const Socket& socket, std::span<const std::uint8_t> payload,
    std::uint32_t maximumBytes)
{
    if (payload.empty() || payload.size() > maximumBytes
        || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("wire frame size is outside the allowed range");
    }
    const auto networkSize = htonl(static_cast<std::uint32_t>(payload.size()));
    const auto* sizeBytes
        = reinterpret_cast<const std::uint8_t*>(&networkSize);
    writeExact(native(socket.descriptor()),
        std::span<const std::uint8_t>(sizeBytes, sizeof(networkSize)));
    writeExact(native(socket.descriptor()), payload);
}

void writeFrame(const Socket& socket, std::span<const std::uint8_t> payload,
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
    writeExact(native(socket.descriptor()),
        std::span<const std::uint8_t>(sizeBytes, sizeof(networkSize)),
        deadline, cancellation, lifecycle);
    writeExact(native(socket.descriptor()), payload, deadline, cancellation,
        lifecycle);
}

void writeFrameWithStallTimeout(const Socket& socket,
    std::span<const std::uint8_t> payload, std::uint32_t maximumBytes,
    std::chrono::milliseconds stallTimeout, StopToken cancellation,
    StopToken lifecycle)
{
    if (payload.empty() || payload.size() > maximumBytes
        || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("wire frame size is outside the allowed range");
    }
    if (stallTimeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "wire frame write stall timeout must be greater than zero");
    }
    const auto networkSize = htonl(static_cast<std::uint32_t>(payload.size()));
    const auto* sizeBytes
        = reinterpret_cast<const std::uint8_t*>(&networkSize);
    writeExact(native(socket.descriptor()),
        std::span<const std::uint8_t>(sizeBytes, sizeof(networkSize)),
        std::chrono::steady_clock::time_point::max(), cancellation, lifecycle,
        stallTimeout);
    writeExact(native(socket.descriptor()), payload,
        std::chrono::steady_clock::time_point::max(), cancellation, lifecycle,
        stallTimeout);
}

std::optional<std::vector<std::uint8_t>> readFrame(
    const Socket& socket, std::uint32_t maximumBytes)
{
    return readFrame(socket, maximumBytes,
        std::chrono::steady_clock::time_point::max(), {});
}

std::optional<std::vector<std::uint8_t>> readFrame(const Socket& socket,
    std::uint32_t maximumBytes,
    std::chrono::steady_clock::time_point deadline,
    StopToken cancellation)
{
    std::array<std::uint8_t, sizeof(std::uint32_t)> sizeBytes{};
    if (!readExact(
            native(socket.descriptor()), sizeBytes, true, deadline,
            cancellation)) {
        return std::nullopt;
    }
    std::uint32_t networkSize = 0;
    std::memcpy(&networkSize, sizeBytes.data(), sizeof(networkSize));
    const auto size = ntohl(networkSize);
    if (size == 0 || size > maximumBytes) {
        throw std::runtime_error("wire frame size is outside the allowed range");
    }
    std::vector<std::uint8_t> payload(size);
    static_cast<void>(readExact(
        native(socket.descriptor()), payload, false, deadline, cancellation));
    return payload;
}

} // namespace amrvis::remote
