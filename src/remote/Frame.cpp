#include <amrexplorer/remote/Frame.hpp>

#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
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
    const std::string& operation, int error = lastSocketError())
{
#ifdef _WIN32
    throw std::runtime_error(
        operation + " failed with socket error " + std::to_string(error));
#else
    throw std::runtime_error(
        operation + ": " + std::string(std::strerror(error)));
#endif
}

bool readExact(NativeSocket descriptor, std::span<std::uint8_t> destination,
    bool allowCleanEof)
{
    std::size_t completed = 0;
    while (completed < destination.size()) {
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
            if (interrupted(error)) {
                continue;
            }
            throwSocketError("recv", error);
        }
        completed += static_cast<std::size_t>(count);
    }
    return true;
}

void writeExact(
    NativeSocket descriptor, std::span<const std::uint8_t> source)
{
    std::size_t completed = 0;
    while (completed < source.size()) {
#ifdef _WIN32
        const auto remaining = std::min<std::size_t>(
            source.size() - completed,
            static_cast<std::size_t>(std::numeric_limits<int>::max()));
        const auto count = ::send(descriptor,
            reinterpret_cast<const char*>(source.data() + completed),
            static_cast<int>(remaining), 0);
#else
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto count = ::send(descriptor, source.data() + completed,
            source.size() - completed, flags);
#endif
        if (count < 0) {
            const auto error = lastSocketError();
            if (interrupted(error)) {
                continue;
            }
            throwSocketError("send", error);
        }
        completed += static_cast<std::size_t>(count);
    }
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
    const int reuse = 1;
#ifdef _WIN32
    const auto* reuseBytes = reinterpret_cast<const char*>(&reuse);
#else
    const auto* reuseBytes = &reuse;
#endif
    if (::setsockopt(native(socket.descriptor()), SOL_SOCKET, SO_REUSEADDR,
            reuseBytes, sizeof(reuse))
        != 0) {
        throwSocketError("setsockopt");
    }

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

    SocketLength size = static_cast<SocketLength>(sizeof(address));
    if (::getsockname(native(socket.descriptor()),
            reinterpret_cast<sockaddr*>(&address), &size)
        != 0) {
        throwSocketError("getsockname");
    }
    return {std::move(socket), ntohs(address.sin_port)};
}

Socket acceptConnection(const Socket& listener)
{
    ensureSockets();
    for (;;) {
        const auto descriptor
            = ::accept(native(listener.descriptor()), nullptr, nullptr);
        if (descriptor != invalidSocket) {
            return Socket(static_cast<Socket::Native>(descriptor));
        }
        const auto error = lastSocketError();
        if (!interrupted(error)) {
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

std::optional<std::vector<std::uint8_t>> readFrame(
    const Socket& socket, std::uint32_t maximumBytes)
{
    std::array<std::uint8_t, sizeof(std::uint32_t)> sizeBytes{};
    if (!readExact(native(socket.descriptor()), sizeBytes, true)) {
        return std::nullopt;
    }
    std::uint32_t networkSize = 0;
    std::memcpy(&networkSize, sizeBytes.data(), sizeof(networkSize));
    const auto size = ntohl(networkSize);
    if (size == 0 || size > maximumBytes) {
        throw std::runtime_error("wire frame size is outside the allowed range");
    }
    std::vector<std::uint8_t> payload(size);
    static_cast<void>(
        readExact(native(socket.descriptor()), payload, false));
    return payload;
}

} // namespace amrvis::remote
