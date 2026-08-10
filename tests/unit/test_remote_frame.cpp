#include <amrexplorer/remote/Frame.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#endif

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

template <typename Function>
void requireRejected(Function&& function, const char* message)
{
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    require(false, message);
}

void writeRaw(const amrvis::remote::Socket& socket,
    const std::vector<std::uint8_t>& bytes)
{
    std::size_t completed = 0;
    while (completed < bytes.size()) {
#ifdef _WIN32
        const auto count = ::send(
            static_cast<SOCKET>(socket.descriptor()),
            reinterpret_cast<const char*>(bytes.data() + completed),
            static_cast<int>(bytes.size() - completed), 0);
        require(count > 0 && count != SOCKET_ERROR, "raw test send failed");
#else
        const auto count = ::send(static_cast<int>(socket.descriptor()),
            bytes.data() + completed, bytes.size() - completed, 0);
        require(count > 0, "raw test send failed");
#endif
        completed += static_cast<std::size_t>(count);
    }
}

std::vector<std::uint8_t> header(std::uint32_t size)
{
    const auto network = htonl(size);
    std::vector<std::uint8_t> bytes(sizeof(network));
    std::memcpy(bytes.data(), &network, sizeof(network));
    return bytes;
}

void closeAbortively(amrvis::remote::Socket& socket)
{
    linger option{};
    option.l_onoff = 1;
    option.l_linger = 0;
#ifdef _WIN32
    const auto result = ::setsockopt(
        static_cast<SOCKET>(socket.descriptor()), SOL_SOCKET, SO_LINGER,
        reinterpret_cast<const char*>(&option), sizeof(option));
    require(result != SOCKET_ERROR, "could not configure abortive close");
#else
    const auto result = ::setsockopt(static_cast<int>(socket.descriptor()),
        SOL_SOCKET, SO_LINGER, &option, sizeof(option));
    require(result == 0, "could not configure abortive close");
#endif
    socket.close();
}

template <typename Writer, typename Reader>
void loopbackExchange(Writer&& writer, Reader&& reader)
{
    using namespace amrvis::remote;
    const auto listener = listenOnLoopback(0);
    std::thread peerThread([&] {
        auto peer = acceptConnection(listener.socket);
        writer(peer);
    });
    auto client = connectTo("127.0.0.1", listener.port);
    reader(client);
    peerThread.join();
}

} // namespace

int main()
{
    using namespace amrvis::remote;

    const auto listener = listenOnLoopback(0);
    std::exception_ptr serverFailure;
    std::thread server([&] {
        try {
            auto peer = acceptConnection(listener.socket);
            const auto request = readFrame(peer, 64);
            require(request == std::optional{
                    std::vector<std::uint8_t>{1, 2, 3, 4}},
                "server did not receive the framed request");
            writeFrame(peer, std::vector<std::uint8_t>{9, 8, 7}, 64);
        } catch (...) {
            serverFailure = std::current_exception();
        }
    });

    auto client = connectTo("127.0.0.1", listener.port);
    writeFrame(client, std::vector<std::uint8_t>{1, 2, 3, 4}, 64);
    const auto response = readFrame(client, 64);
    require(response == std::optional{
            std::vector<std::uint8_t>{9, 8, 7}},
        "client did not receive the framed response");
    server.join();
    if (serverFailure) {
        std::rethrow_exception(serverFailure);
    }

    requireRejected(
        [&] { writeFrame(client, {}, 64); },
        "an empty frame was accepted");
    requireRejected(
        [&] { writeFrame(client, std::vector<std::uint8_t>(65), 64); },
        "an oversized frame was accepted");

    amrvis::StopSource stoppedWrite;
    stoppedWrite.request_stop();
    bool writeCancellationObserved = false;
    try {
        writeFrame(client, std::vector<std::uint8_t>{1}, 64,
            std::chrono::steady_clock::now() + std::chrono::seconds(1),
            stoppedWrite.get_token());
    } catch (const amrvis::ReadCancelled&) {
        writeCancellationObserved = true;
    }
    require(writeCancellationObserved,
        "a cancelled deadline-aware frame write was not interrupted");

    const auto cancelledAcceptListener = listenOnLoopback(0);
    amrvis::StopSource stoppedAccept;
    auto cancelledAccept = std::async(std::launch::async, [&] {
        try {
            static_cast<void>(acceptConnection(
                cancelledAcceptListener.socket, stoppedAccept.get_token()));
        } catch (const amrvis::ReadCancelled&) {
            return true;
        }
        return false;
    });
    stoppedAccept.request_stop();
    require(cancelledAccept.wait_for(std::chrono::seconds(1))
                == std::future_status::ready
            && cancelledAccept.get(),
        "a cancelled listener accept was not interrupted");

    const auto cancelledReadListener = listenOnLoopback(0);
    auto cancelledReadPeer = std::async(std::launch::async, [&] {
        return acceptConnection(cancelledReadListener.socket);
    });
    auto cancelledReadClient
        = connectTo("127.0.0.1", cancelledReadListener.port);
    auto cancelledReadServer = cancelledReadPeer.get();
    amrvis::StopSource stoppedRead;
    auto cancelledRead = std::async(std::launch::async, [&] {
        try {
            static_cast<void>(readFrame(cancelledReadClient, 64,
                std::chrono::steady_clock::time_point::max(),
                stoppedRead.get_token()));
        } catch (const amrvis::ReadCancelled&) {
            return true;
        }
        return false;
    });
    stoppedRead.request_stop();
    require(cancelledRead.wait_for(std::chrono::seconds(1))
                == std::future_status::ready
            && cancelledRead.get(),
        "a cancelled frame read was not interrupted");

#ifndef _WIN32
    // A peer that stops reading must not hold a writer forever. AF_UNIX keeps
    // this deterministic without loopback TCP receive-window autotuning.
    int blockedDescriptors[2]{};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, blockedDescriptors) == 0,
        "could not create the blocked-write socket pair");
    Socket blockedWriter(blockedDescriptors[0]);
    Socket blockedPeer(blockedDescriptors[1]);
    const auto writerFlags = ::fcntl(blockedDescriptors[0], F_GETFL, 0);
    require(writerFlags >= 0
            && ::fcntl(blockedDescriptors[0], F_SETFL,
                   writerFlags | O_NONBLOCK) == 0,
        "could not make the blocked writer nonblocking");
    int sendBufferBytes = 4096;
    require(::setsockopt(blockedDescriptors[0], SOL_SOCKET, SO_SNDBUF,
                &sendBufferBytes, sizeof(sendBufferBytes)) == 0,
        "could not reduce the test send buffer");
    bool timedOut = false;
    try {
        writeFrame(blockedWriter,
            std::vector<std::uint8_t>(4U * 1024U * 1024U),
            4U * 1024U * 1024U,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
    } catch (const std::runtime_error& error) {
        timedOut = std::string(error.what()).find("timed out")
            != std::string::npos;
    }
    require(timedOut, "a peer-blocked frame write ignored its deadline");

    // A slow peer that keeps draining bytes must be allowed to take longer
    // than one stall interval to receive the complete frame.
    int slowDescriptors[2]{};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, slowDescriptors) == 0,
        "could not create the slow-reader socket pair");
    Socket slowWriter(slowDescriptors[0]);
    Socket slowPeer(slowDescriptors[1]);
    const auto slowWriterFlags = ::fcntl(slowDescriptors[0], F_GETFL, 0);
    require(slowWriterFlags >= 0
            && ::fcntl(slowDescriptors[0], F_SETFL,
                   slowWriterFlags | O_NONBLOCK) == 0,
        "could not make the slow writer nonblocking");
    require(::setsockopt(slowDescriptors[0], SOL_SOCKET, SO_SNDBUF,
                &sendBufferBytes, sizeof(sendBufferBytes)) == 0,
        "could not reduce the slow-writer send buffer");
    constexpr std::size_t slowPayloadBytes = 512U * 1024U;
    constexpr auto writeStallTimeout = std::chrono::milliseconds{250};
    std::atomic<std::size_t> slowBytesRead{0};
    std::promise<void> slowReaderReady;
    auto slowReaderStarted = slowReaderReady.get_future();
    std::thread slowReader([&] {
        slowReaderReady.set_value();
        std::array<std::uint8_t, 4096> bytes{};
        while (slowBytesRead.load()
            < slowPayloadBytes + sizeof(std::uint32_t)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
            const auto count = ::recv(slowDescriptors[1], bytes.data(),
                bytes.size(), 0);
            if (count <= 0) {
                break;
            }
            slowBytesRead += static_cast<std::size_t>(count);
        }
    });
    slowReaderStarted.wait();
    bool slowWriteCompleted = true;
    std::string slowWriteError;
    const auto slowWriteStart = std::chrono::steady_clock::now();
    try {
        writeFrameWithStallTimeout(slowWriter,
            std::vector<std::uint8_t>(slowPayloadBytes), slowPayloadBytes,
            writeStallTimeout);
    } catch (const std::exception& error) {
        slowWriteCompleted = false;
        slowWriteError = error.what();
    }
    const auto slowWriteElapsed
        = std::chrono::steady_clock::now() - slowWriteStart;
    slowWriter.shutdown();
    slowReader.join();
    if (!slowWriteCompleted) {
        std::cerr << "slow-reader write failed: " << slowWriteError << '\n';
    }
    require(slowWriteCompleted,
        "a progressing slow-reader frame write was timed out");
    require(slowBytesRead.load()
            == slowPayloadBytes + sizeof(std::uint32_t),
        "the slow reader did not receive the complete frame");
    require(slowWriteElapsed > writeStallTimeout,
        "the slow-reader regression did not exceed one stall interval");
#endif

    // A TCP-segment boundary may tear the four-byte header without making the
    // frame malformed. readFrame must assemble it exactly.
    loopbackExchange(
        [](const Socket& peer) {
            auto bytes = header(3);
            writeRaw(peer, std::vector<std::uint8_t>(
                bytes.begin(), bytes.begin() + 2));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::vector<std::uint8_t> tail(bytes.begin() + 2, bytes.end());
            tail.insert(tail.end(), {4, 5, 6});
            writeRaw(peer, tail);
        },
        [](const Socket& peer) {
            require(readFrame(peer, 64)
                    == std::optional{
                        std::vector<std::uint8_t>{4, 5, 6}},
                "a torn header was not reassembled");
        });

    loopbackExchange(
        [](const Socket& peer) {
            const auto bytes = header(4);
            writeRaw(peer, std::vector<std::uint8_t>(
                bytes.begin(), bytes.begin() + 2));
        },
        [](const Socket& peer) {
            requireRejected([&] { static_cast<void>(readFrame(peer, 64)); },
                "EOF inside a short header was accepted");
        });

    loopbackExchange(
        [](const Socket& peer) { writeRaw(peer, header(0)); },
        [](const Socket& peer) {
            requireRejected([&] { static_cast<void>(readFrame(peer, 64)); },
                "a zero-length injected frame was accepted");
        });

    loopbackExchange(
        [](const Socket& peer) { writeRaw(peer, header(65)); },
        [](const Socket& peer) {
            requireRejected([&] { static_cast<void>(readFrame(peer, 64)); },
                "an injected oversized frame was accepted");
        });

    loopbackExchange(
        [](const Socket& peer) {
            auto bytes = header(4);
            bytes.insert(bytes.end(), {1, 2});
            writeRaw(peer, bytes);
        },
        [](const Socket& peer) {
            requireRejected([&] { static_cast<void>(readFrame(peer, 64)); },
                "EOF inside a short payload was accepted");
        });

    const auto eofListener = listenOnLoopback(0);
    std::thread closer([&] {
        auto peer = acceptConnection(eofListener.socket);
        peer.close();
    });
    auto eofClient = connectTo("127.0.0.1", eofListener.port);
    require(!readFrame(eofClient, 64).has_value(),
        "clean EOF was not distinguished from a partial frame");
    closer.join();

    const auto resetListener = listenOnLoopback(0);
    std::promise<void> resetConnected;
    auto resetMayProceed = resetConnected.get_future();
    std::thread resetter([&] {
        auto peer = acceptConnection(resetListener.socket);
        resetMayProceed.wait();
        closeAbortively(peer);
    });
    auto resetClient = connectTo("127.0.0.1", resetListener.port);
    resetConnected.set_value();
    requireRejected(
        [&] { static_cast<void>(readFrame(resetClient, 64)); },
        "an abortive close was not observed by the client");
    resetter.join();

    // On Unix this reaches EPIPE after the reset. The connected-socket setup and
    // per-send flags must convert it to an exception instead of SIGPIPE process
    // termination; Windows has the equivalent socket-error behavior.
    requireRejected(
        [&] {
            for (int attempt = 0; attempt < 4; ++attempt) {
                writeFrame(resetClient, std::vector<std::uint8_t>{1}, 64);
            }
        },
        "writing to a closed peer did not throw");
    return 0;
}
