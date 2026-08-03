#include "../../src/remote/Codec.hpp"

#include <amrexplorer/data/ViewData.hpp>
#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <thread>
#include <variant>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

class RunningServer {
public:
    explicit RunningServer(amrvis::remote::Server& server)
        : m_server(server)
        , m_thread([this] {
            try {
                m_server.run();
            } catch (...) {
                m_failure = std::current_exception();
            }
        })
    {
    }

    ~RunningServer()
    {
        m_server.requestStop();
        m_thread.join();
        if (m_failure) {
            std::terminate();
        }
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

private:
    amrvis::remote::Server& m_server;
    std::thread m_thread;
    std::exception_ptr m_failure;
};

amrvis::SliceRequest sliceRequest(
    const amrvis::remote::OpenedDataset& opened, int width, int height)
{
    amrvis::SliceRequest request;
    request.dataset = opened.id;
    request.field = amrvis::FieldId{0};
    request.normalDirection = 1;
    request.visibleRegion = opened.catalog.physicalDomain;
    request.physicalPosition = 0.5
        * (request.visibleRegion.lower[1] + request.visibleRegion.upper[1]);
    request.maximumLevel = opened.catalog.finestLevel;
    request.outputSize = {width, height};
    return request;
}

std::uint64_t acceptHello(amrvis::remote::Socket& peer,
    std::uint16_t selectedMinorVersion
    = amrvis::remote::protocolMinorVersion)
{
    using namespace amrvis::remote;
    const auto frame = readFrame(peer, defaultMaximumFrameBytes);
    require(frame.has_value(), "client closed before sending hello");
    const auto request = codec::decode(*frame);
    require(codec::inspect(*request).payload == PayloadKind::HelloRequest,
        "client did not send hello first");
    HelloResponseData hello;
    hello.serverName = "test peer";
    hello.softwareVersion = "test";
    hello.selectedMinorVersion = selectedMinorVersion;
    hello.maximumFrameBytes = defaultMaximumFrameBytes;
    hello.maximumDatasets = 8;
    hello.maximumOutstandingRequests = 8;
    hello.workerCount = 1;
    writeFrame(peer, codec::encode(request->request_id,
                         codec::toWire(hello), selectedMinorVersion),
        defaultMaximumFrameBytes);
    return request->request_id;
}

bool throwsWithin(std::future<void>& operation,
    std::chrono::milliseconds timeout)
{
    if (operation.wait_for(timeout) != std::future_status::ready) {
        return false;
    }
    try {
        operation.get();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char* argv[])
{
    using namespace amrvis;
    using namespace amrvis::remote;

    if (argc != 2) {
        std::cerr << "usage: test_remote_connection MATERIALIZED_PLOTFILE\n";
        return 2;
    }

    ServerOptions options;
    options.workerCount = 3;
    Server server(options);
    RunningServer running(server);

    Connection connection("127.0.0.1", server.port());
    require(connection.connected()
            && connection.serverInfo().workerCount == options.workerCount,
        "connection did not complete the protocol handshake");
    connection.ping();

    const auto opened = connection.openDataset(
        std::filesystem::path(argv[1]).string(),
        16ULL * 1024ULL * 1024ULL);
    require(opened.catalog.dimension == 2,
        "connection did not decode the dataset catalog");

    const auto request = sliceRequest(opened, 7, 5);
    auto first = std::async(std::launch::async,
        [&] { return connection.requestView(request); });
    auto second = std::async(std::launch::async,
        [&] { return connection.requestView(request); });
    require(std::get<SliceQueryResult>(first.get()).plane.values.size() == 35
            && std::get<SliceQueryResult>(second.get())
                    .plane.values.size()
                == 35,
        "connection did not match concurrent responses to their requests");

    StopSource cancelled;
    cancelled.request_stop();
    try {
        static_cast<void>(connection.requestView(
            request, cancelled.get_token()));
        require(false, "connection sent a pre-cancelled request");
    } catch (const ReadCancelled&) {
    }

    connection.closeDataset(opened.id);
    connection.close();
    require(!connection.connected(),
        "connection remained live after close");

    auto negotiatedMinorVersionListener = listenOnLoopback(0);
    constexpr auto selectedMinorVersion = protocolMinorVersion == 0
        ? std::uint16_t{0}
        : static_cast<std::uint16_t>(protocolMinorVersion - 1);
    auto negotiatedMinorVersionPeer = std::async(std::launch::async, [&] {
        auto peer = acceptConnection(negotiatedMinorVersionListener.socket);
        static_cast<void>(acceptHello(peer, selectedMinorVersion));
        const auto frame = readFrame(peer, defaultMaximumFrameBytes);
        require(frame.has_value(),
            "client omitted negotiated minor version ping request");
        const auto request = codec::decode(*frame);
        const auto info = codec::inspect(*request);
        require(info.payload == PayloadKind::PingRequest
                && info.protocolMinorVersion == selectedMinorVersion,
            "client did not use the negotiated protocol minor version");
        codec::fb::PongResponseT pong;
        pong.nonce = request->payload.AsPingRequest()->nonce;
        writeFrame(peer,
            codec::encode(
                info.requestId, std::move(pong), selectedMinorVersion),
            defaultMaximumFrameBytes);
    });
    Connection negotiatedMinorVersionConnection(
        "127.0.0.1", negotiatedMinorVersionListener.port);
    negotiatedMinorVersionConnection.ping();
    negotiatedMinorVersionConnection.close();
    negotiatedMinorVersionPeer.get();

    StopSource preCancelled;
    preCancelled.request_stop();
    try {
        Connection cancelledBeforeConnect("127.0.0.1", server.port(), {},
            preCancelled.get_token());
        require(false, "pre-cancelled connection attempt succeeded");
    } catch (const ReadCancelled&) {
    }

    auto silentListener = listenOnLoopback(0);
    auto silentPeer = std::async(std::launch::async, [&] {
        auto peer = acceptConnection(silentListener.socket);
        std::this_thread::sleep_for(500ms);
    });
    ConnectionOptions shortHandshake;
    shortHandshake.connectionTimeout = 100ms;
    const auto handshakeStart = std::chrono::steady_clock::now();
    bool handshakeTimedOut = false;
    try {
        Connection silentConnection(
            "127.0.0.1", silentListener.port, shortHandshake);
    } catch (const std::exception& error) {
        handshakeTimedOut
            = std::string(error.what()).find("handshake timed out")
            != std::string::npos;
    }
    require(handshakeTimedOut
            && std::chrono::steady_clock::now() - handshakeStart < 1s,
        "silent hello did not honor the connection deadline");
    silentPeer.get();

    auto cancelListener = listenOnLoopback(0);
    std::promise<void> cancelAcceptedPromise;
    auto cancelAccepted = cancelAcceptedPromise.get_future();
    std::promise<void> releaseCancelPeerPromise;
    auto releaseCancelPeer = releaseCancelPeerPromise.get_future();
    auto cancelPeer = std::async(std::launch::async, [&] {
        auto peer = acceptConnection(cancelListener.socket);
        cancelAcceptedPromise.set_value();
        releaseCancelPeer.wait();
    });
    StopSource connectCancellation;
    ConnectionOptions cancellableHandshake;
    cancellableHandshake.connectionTimeout = 5s;
    auto cancelledConnection = std::async(std::launch::async, [&] {
        Connection pending("127.0.0.1", cancelListener.port,
            cancellableHandshake, connectCancellation.get_token());
    });
    require(cancelAccepted.wait_for(1s) == std::future_status::ready,
        "cancellation test peer did not accept the connection");
    connectCancellation.request_stop();
    require(cancelledConnection.wait_for(1s) == std::future_status::ready,
        "hello cancellation did not complete within its deadline");
    bool helloCancelled = false;
    try {
        cancelledConnection.get();
    } catch (const ReadCancelled&) {
        helloCancelled = true;
    }
    require(helloCancelled,
        "hello cancellation returned the wrong exception");
    releaseCancelPeerPromise.set_value();
    cancelPeer.get();

    auto wrongPayloadListener = listenOnLoopback(0);
    auto wrongPayloadPeer = std::async(std::launch::async, [&] {
        auto peer = acceptConnection(wrongPayloadListener.socket);
        static_cast<void>(acceptHello(peer));
        const auto frame = readFrame(peer, defaultMaximumFrameBytes);
        require(frame.has_value(), "client omitted ping request");
        const auto requestEnvelope = codec::decode(*frame);
        const auto requestInfo = codec::inspect(*requestEnvelope);
        const auto* ping = requestEnvelope->payload.AsPingRequest();
        require(requestInfo.requestId == 2 && ping != nullptr
                && ping->nonce == 1,
            "ping consumed an extra request ID");
        codec::fb::DatasetClosedT wrong;
        wrong.dataset_id = 1;
        writeFrame(peer,
            codec::encode(requestInfo.requestId, std::move(wrong)),
            defaultMaximumFrameBytes);
    });
    ConnectionOptions shortRequest;
    shortRequest.requestTimeout = 1s;
    Connection wrongPayloadConnection(
        "127.0.0.1", wrongPayloadListener.port, shortRequest);
    auto wrongPayloadCall = std::async(std::launch::async,
        [&] { wrongPayloadConnection.ping(); });
    require(throwsWithin(wrongPayloadCall, 1s),
        "wrong response payload left its caller waiting");
    require(!wrongPayloadConnection.connected(),
        "wrong response payload did not fail the connection");
    wrongPayloadPeer.get();

    auto fanoutListener = listenOnLoopback(0);
    auto fanoutPeer = std::async(std::launch::async, [&] {
        auto peer = acceptConnection(fanoutListener.socket);
        static_cast<void>(acceptHello(peer));
        for (int pendingRequest = 0; pendingRequest < 2; ++pendingRequest) {
            require(readFrame(peer, defaultMaximumFrameBytes).has_value(),
                "client omitted a concurrent request");
        }
    });
    Connection fanoutConnection(
        "127.0.0.1", fanoutListener.port, shortRequest);
    auto fanoutFirst = std::async(std::launch::async,
        [&] { fanoutConnection.ping(); });
    auto fanoutSecond = std::async(std::launch::async,
        [&] { fanoutConnection.ping(); });
    require(throwsWithin(fanoutFirst, 2s)
            && throwsWithin(fanoutSecond, 2s),
        "disconnect did not fail every pending request");
    fanoutPeer.get();

    auto requestTimeoutListener = listenOnLoopback(0);
    auto requestTimeoutPeer = std::async(std::launch::async, [&] {
        auto peer = acceptConnection(requestTimeoutListener.socket);
        static_cast<void>(acceptHello(peer));
        require(readFrame(peer, defaultMaximumFrameBytes).has_value(),
            "client omitted timeout test request");
        std::this_thread::sleep_for(500ms);
    });
    ConnectionOptions requestDeadline;
    requestDeadline.requestTimeout = 100ms;
    Connection requestTimeoutConnection(
        "127.0.0.1", requestTimeoutListener.port, requestDeadline);
    auto timedRequest = std::async(std::launch::async,
        [&] { requestTimeoutConnection.ping(); });
    require(throwsWithin(timedRequest, 1s),
        "silent request did not honor the request deadline");
    requestTimeoutPeer.get();
    return 0;
}
#include <chrono>
