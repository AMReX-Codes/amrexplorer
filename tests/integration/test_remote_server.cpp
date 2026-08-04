#include "../../src/remote/Codec.hpp"

#include <amrexplorer/data/ViewData.hpp>
#include <amrexplorer/remote/Frame.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>

namespace {

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
        , m_done(m_donePromise.get_future())
        , m_thread([this] {
            try {
                m_server.run();
            } catch (...) {
                m_failure = std::current_exception();
            }
            m_donePromise.set_value();
        })
    {
    }

    ~RunningServer()
    {
        m_server.requestStop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_failure) {
            try {
                std::rethrow_exception(m_failure);
            } catch (const std::exception& error) {
                std::cerr << "server thread failed: " << error.what() << '\n';
                std::terminate();
            }
        }
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    bool stopWithin(std::chrono::milliseconds timeout)
    {
        m_server.requestStop();
        if (m_done.wait_for(timeout) != std::future_status::ready) {
            return false;
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        return true;
    }

private:
    amrvis::remote::Server& m_server;
    std::promise<void> m_donePromise;
    std::future<void> m_done;
    std::thread m_thread;
    std::exception_ptr m_failure;
};

template <typename Payload>
std::unique_ptr<amrvis::remote::codec::NativeEnvelope> exchange(
    const amrvis::remote::Socket& socket, std::uint64_t requestId,
    Payload payload, std::uint32_t maximumFrameBytes)
{
    using namespace amrvis::remote;
    writeFrame(socket,
        codec::encode(requestId, std::move(payload)), maximumFrameBytes);
    const auto response = readFrame(socket, maximumFrameBytes);
    require(response.has_value(), "server closed before sending a response");
    auto envelope = codec::decode(*response);
    require(envelope->request_id == requestId,
        "server response used the wrong request ID");
    return envelope;
}

std::unique_ptr<amrvis::remote::codec::NativeEnvelope> readWithDeadline(
    amrvis::remote::Socket& socket, std::uint32_t maximumFrameBytes,
    std::chrono::milliseconds timeout)
{
    auto pending = std::async(std::launch::async,
        [&socket, maximumFrameBytes] {
            const auto response
                = amrvis::remote::readFrame(socket, maximumFrameBytes);
            return response
                ? amrvis::remote::codec::decode(*response)
                : std::unique_ptr<
                      amrvis::remote::codec::NativeEnvelope>{};
        });
    if (pending.wait_for(timeout) != std::future_status::ready) {
        socket.shutdown();
        pending.wait();
        return {};
    }
    return pending.get();
}

amrvis::remote::HelloRequestData helloRequest()
{
    using namespace amrvis::remote;
    return {"server integration test", "test", 0, protocolMinorVersion,
        defaultMaximumFrameBytes, {}, {}};
}

void writeOversizedParticleHeader(const std::filesystem::path& root)
{
    using namespace amrvis::remote;
    constexpr std::uint64_t bytesPerPoint
        = sizeof(std::uint64_t) + 3 * sizeof(double);
    constexpr std::uint64_t particleCount
        = defaultMaximumFrameBytes / bytesPerPoint + 1;
    const auto species = root / "Oversized";
    std::filesystem::create_directories(species);
    std::ofstream header(species / "Header");
    require(static_cast<bool>(header),
        "could not create oversized particle Header");
    header << "Version_Two_Dot_Zero_double\n"
           << "2\n0\n0\n1\n"
           << particleCount << '\n'
           << particleCount + 1 << "\n0\n1\n"
           << "0 " << particleCount << " 0\n";
    require(static_cast<bool>(header),
        "could not write oversized particle Header");
}

} // namespace

int main(int argc, char* argv[])
{
    using namespace amrvis;
    using namespace amrvis::remote;

    if (argc != 2) {
        std::cerr << "usage: test_remote_server MATERIALIZED_PLOTFILE\n";
        return 2;
    }
    writeOversizedParticleHeader(argv[1]);

    ServerOptions options;
    options.workerCount = 2;
    options.maximumDatasets = 2;
    options.maximumOutstandingRequests = 8;
    options.maximumConnections = 4;
    Server server(options);
    RunningServer running(server);

    auto socket = connectTo("127.0.0.1", server.port());
    constexpr std::uint32_t reducedFrameBytes = 1U * 1024U * 1024U;
    auto reducedFrameHello = helloRequest();
    reducedFrameHello.maximumFrameBytes = reducedFrameBytes;
    auto envelope = exchange(socket, 1,
        codec::toWire(reducedFrameHello), reducedFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse,
        "server rejected a compatible handshake");
    const auto hello = codec::fromWire(
        *envelope->payload.AsHelloResponse());
    require(hello.selectedMinorVersion == protocolMinorVersion
            && hello.workerCount == options.workerCount,
        "server handshake reported the wrong limits");

    envelope = exchange(socket, 2,
        codec::toWire(OpenDatasetData{
            std::filesystem::path(argv[1]).string(),
            16ULL * 1024ULL * 1024ULL}),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetOpened,
        "server did not open the materialized plotfile");
    const auto opened = codec::fromWire(
        *envelope->payload.AsDatasetOpened());
    require(opened.catalog.dimension == 2
            && opened.catalog.levels.size() == 2,
        "server returned an incomplete dataset catalog");

    DatasetPageRequest smallPage;
    smallPage.dataset = opened.id;
    smallPage.field = FieldId{0};
    smallPage.level = 0;
    smallPage.region = opened.catalog.physicalDomain;
    smallPage.normalAxis = 1;
    smallPage.maximumExtent = datasetPageMaxExtent;
    envelope = exchange(socket, 9, codec::toWire(smallPage),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload
                == PayloadKind::DatasetPageResponse,
        "server rejected a small dataset page under a reduced frame limit");
    const auto page
        = codec::fromWire(*envelope->payload.AsDatasetPageResponse());
    require(page.nx == 4 && page.ny == 4 && page.values.size() == 16,
        "server returned the wrong reduced-frame dataset page");

    DatasetPageRequest reversedPage;
    reversedPage.dataset = opened.id;
    reversedPage.field = FieldId{0};
    reversedPage.level = 0;
    reversedPage.region = opened.catalog.physicalDomain;
    std::swap(reversedPage.region.lower[0], reversedPage.region.upper[0]);
    reversedPage.normalAxis = 1;
    envelope = exchange(socket, 10, codec::toWire(reversedPage),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse
            && codec::fromWire(*envelope->payload.AsErrorResponse()).code
                == ErrorCode::InvalidRequest,
        "server did not reject a reversed dataset-page region");

    envelope = exchange(socket, 8,
        codec::toWire(opened.id, "Oversized", 1.0, 0),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse
            && codec::fromWire(*envelope->payload.AsErrorResponse()).code
                == ErrorCode::ResourceLimitExceeded,
        "oversized particle request reached the particle reader");

    SliceRequest request;
    request.dataset = opened.id;
    request.field = FieldId{0};
    request.normalDirection = 1;
    request.visibleRegion = opened.catalog.physicalDomain;
    request.physicalPosition = 0.5
        * (request.visibleRegion.lower[1] + request.visibleRegion.upper[1]);
    request.maximumLevel = opened.catalog.finestLevel;
    request.outputSize = {8, 6};
    envelope = exchange(socket, 3, codec::toWire(request),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload
            == PayloadKind::SliceViewResponse,
        "server did not return a slice response");
    const auto slice = codec::fromWire(
        *envelope->payload.AsSliceViewResponse());
    require(slice.plane.width == 8 && slice.plane.height == 6
            && slice.plane.values.size() == 48,
        "server did not honor the bounded slice extent");

    request.outputSize = {maxViewOutputDimension + 1, 1};
    envelope = exchange(socket, 4, codec::toWire(request),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse,
        "server accepted an oversized slice");
    const auto error = codec::fromWire(
        *envelope->payload.AsErrorResponse());
    require(error.code == ErrorCode::ResourceLimitExceeded,
        "oversized slice returned the wrong error");

    envelope = exchange(socket, 5,
        codec::toWire(OpenDatasetData{
            std::filesystem::path(argv[1]).string(),
            16ULL * 1024ULL * 1024ULL}),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetOpened,
        "server rejected the second allowed dataset");
    const auto secondOpened
        = codec::fromWire(*envelope->payload.AsDatasetOpened());

    envelope = exchange(socket, 6,
        codec::toWire(OpenDatasetData{
            std::filesystem::path(argv[1]).string(),
            16ULL * 1024ULL * 1024ULL}),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse
            && codec::fromWire(*envelope->payload.AsErrorResponse()).code
                == ErrorCode::ResourceLimitExceeded,
        "server did not enforce the configured dataset limit");

    codec::fb::CloseDatasetRequestT closeSecond;
    closeSecond.dataset_id = secondOpened.id.value;
    envelope = exchange(socket, 7, std::move(closeSecond),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetClosed,
        "server did not close the second dataset");

    ServerOptions duplicateOptions;
    duplicateOptions.workerCount = 2;
    duplicateOptions.maximumDatasets = 1;
    duplicateOptions.maximumOutstandingRequests = 2;
    duplicateOptions.maximumConnections = 1;
    Server duplicateServer(duplicateOptions);
    RunningServer duplicateRunning(duplicateServer);
    auto duplicateSocket
        = connectTo("127.0.0.1", duplicateServer.port());
    envelope = exchange(duplicateSocket, 1, codec::toWire(helloRequest()),
        defaultMaximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse,
        "server rejected the duplicate-ID test connection");
    const auto duplicateHello = codec::fromWire(
        *envelope->payload.AsHelloResponse());
    const OpenDatasetData duplicateOpen{
        std::filesystem::path(argv[1]).string(),
        16ULL * 1024ULL * 1024ULL};
    envelope = exchange(duplicateSocket, 2, codec::toWire(duplicateOpen),
        duplicateHello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetOpened,
        "server did not open the duplicate-ID test dataset");
    const auto duplicateOpened = codec::fromWire(
        *envelope->payload.AsDatasetOpened());
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    SliceRequest duplicateSlice;
    duplicateSlice.dataset = duplicateOpened.id;
    duplicateSlice.field = FieldId{0};
    duplicateSlice.normalDirection = 1;
    duplicateSlice.visibleRegion = duplicateOpened.catalog.physicalDomain;
    duplicateSlice.physicalPosition = 0.5
        * (duplicateSlice.visibleRegion.lower[1]
            + duplicateSlice.visibleRegion.upper[1]);
    duplicateSlice.maximumLevel = duplicateOpened.catalog.finestLevel;
    // Keep the first request live while the reader dispatches the duplicate:
    // its response is larger than the socket's send buffer and cannot finish
    // until this client starts reading below.
    duplicateSlice.outputSize = {1536, 1536};
    writeFrame(duplicateSocket,
        codec::encode(3, codec::toWire(duplicateSlice)),
        duplicateHello.maximumFrameBytes);
    writeFrame(duplicateSocket,
        codec::encode(4, codec::toWire(duplicateSlice)),
        duplicateHello.maximumFrameBytes);
    writeFrame(duplicateSocket,
        codec::encode(3, codec::toWire(duplicateSlice)),
        duplicateHello.maximumFrameBytes);
    bool duplicateRejected = false;
    for (int response = 0; response < 3 && !duplicateRejected; ++response) {
        auto duplicateResponse = readWithDeadline(duplicateSocket,
            duplicateHello.maximumFrameBytes, std::chrono::seconds{10});
        require(duplicateResponse != nullptr,
            "duplicate request ID caused an unbounded disconnect");
        if (codec::inspect(*duplicateResponse).payload
            == PayloadKind::ErrorResponse) {
            duplicateRejected
                = codec::fromWire(
                      *duplicateResponse->payload.AsErrorResponse())
                      .code
                == ErrorCode::InvalidRequest;
        }
    }
    require(duplicateRejected,
        "duplicate live request ID did not receive a bounded error");
    require(duplicateRunning.stopWithin(std::chrono::seconds{2}),
        "duplicate-ID server shutdown exceeded its deadline");

    // A large response to a peer that stops reading must time out, retire that
    // session, and release the shared worker for another connection.
    ServerOptions stalledOptions;
    stalledOptions.workerCount = 1;
    stalledOptions.maximumDatasets = 1;
    stalledOptions.maximumConnections = 2;
    stalledOptions.responseWriteStallTimeout
        = std::chrono::milliseconds{100};
    Server stalledServer(stalledOptions);
    RunningServer stalledRunning(stalledServer);
    auto stalledSocket = connectTo("127.0.0.1", stalledServer.port());
    envelope = exchange(stalledSocket, 1, codec::toWire(helloRequest()),
        defaultMaximumFrameBytes);
    const auto stalledHello
        = codec::fromWire(*envelope->payload.AsHelloResponse());
    envelope = exchange(stalledSocket, 2, codec::toWire(duplicateOpen),
        stalledHello.maximumFrameBytes);
    const auto stalledOpened
        = codec::fromWire(*envelope->payload.AsDatasetOpened());
    SliceRequest stalledSlice = duplicateSlice;
    stalledSlice.dataset = stalledOpened.id;
    stalledSlice.outputSize = {2048, 2048};
    writeFrame(stalledSocket,
        codec::encode(3, codec::toWire(stalledSlice)),
        stalledHello.maximumFrameBytes);

    auto healthySocket = connectTo("127.0.0.1", stalledServer.port());
    envelope = exchange(healthySocket, 1, codec::toWire(helloRequest()),
        defaultMaximumFrameBytes);
    const auto healthyHello
        = codec::fromWire(*envelope->payload.AsHelloResponse());
    envelope = exchange(healthySocket, 2, codec::toWire(duplicateOpen),
        healthyHello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetOpened,
        "stalled response retained the shared server worker");
    bool stalledRetired = false;
    try {
        stalledRetired = readWithDeadline(stalledSocket,
            stalledHello.maximumFrameBytes, std::chrono::seconds{2})
            == nullptr;
    } catch (const std::exception&) {
        stalledRetired = true;
    }
    require(stalledRetired,
        "server did not retire the stalled response session");
    require(stalledRunning.stopWithin(std::chrono::seconds{2}),
        "stalled-response server shutdown exceeded its deadline");

    codec::fb::CloseDatasetRequestT close;
    close.dataset_id = opened.id.value;
    envelope = exchange(socket, 9, std::move(close),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetClosed,
        "server did not close the dataset");
    require(running.stopWithin(std::chrono::seconds{2}),
        "server shutdown exceeded its deadline");

    ServerOptions connectionOptions;
    connectionOptions.workerCount = 1;
    connectionOptions.maximumConnections = 1;
    Server connectionLimitedServer(connectionOptions);
    RunningServer connectionLimitedRunning(connectionLimitedServer);
    auto allowedSocket
        = connectTo("127.0.0.1", connectionLimitedServer.port());
    envelope = exchange(allowedSocket, 1, codec::toWire(helloRequest()),
        defaultMaximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse,
        "server rejected the allowed connection");
    auto excessSocket
        = connectTo("127.0.0.1", connectionLimitedServer.port());
    bool excessDisconnected = false;
    try {
        excessDisconnected = readWithDeadline(excessSocket,
            defaultMaximumFrameBytes, std::chrono::seconds{2})
            == nullptr;
    } catch (const std::exception&) {
        excessDisconnected = true;
    }
    require(excessDisconnected,
        "server did not enforce the configured connection limit");
    require(connectionLimitedRunning.stopWithin(std::chrono::seconds{2}),
        "connection-limited server shutdown exceeded its deadline");

    ServerOptions handshakeTimeoutOptions;
    handshakeTimeoutOptions.workerCount = 1;
    handshakeTimeoutOptions.maximumConnections = 1;
    handshakeTimeoutOptions.handshakeTimeout
        = std::chrono::milliseconds{100};
    Server handshakeTimeoutServer(handshakeTimeoutOptions);
    RunningServer handshakeTimeoutRunning(handshakeTimeoutServer);
    auto silentSocket
        = connectTo("127.0.0.1", handshakeTimeoutServer.port());
    const auto silentStart = std::chrono::steady_clock::now();
    require(readWithDeadline(silentSocket, defaultMaximumFrameBytes,
                std::chrono::seconds{2})
            == nullptr,
        "silent unauthenticated connection did not time out");
    require(std::chrono::steady_clock::now() - silentStart
            < std::chrono::seconds{1},
        "silent unauthenticated connection exceeded its handshake deadline");
    auto recoveredSocket
        = connectTo("127.0.0.1", handshakeTimeoutServer.port());
    envelope = exchange(recoveredSocket, 1,
        codec::toWire(helloRequest()),
        defaultMaximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse,
        "authorized client could not reclaim a timed-out connection slot");
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    codec::fb::PingRequestT delayedPing;
    delayedPing.nonce = 17;
    envelope = exchange(recoveredSocket, 2, std::move(delayedPing),
        defaultMaximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::PongResponse,
        "server retained the handshake deadline after authentication");
    require(handshakeTimeoutRunning.stopWithin(std::chrono::seconds{2}),
        "handshake-timeout server shutdown exceeded its deadline");

    ServerOptions handshakeFrameOptions;
    handshakeFrameOptions.workerCount = 1;
    handshakeFrameOptions.maximumConnections = 1;
    const auto normalHelloBytes = codec::encode(
        1, codec::toWire(helloRequest()));
    handshakeFrameOptions.maximumHandshakeFrameBytes
        = static_cast<std::uint32_t>(normalHelloBytes.size() + 16);
    Server handshakeFrameServer(handshakeFrameOptions);
    RunningServer handshakeFrameRunning(handshakeFrameServer);
    auto oversizedHello = helloRequest();
    oversizedHello.clientName.assign(
        handshakeFrameOptions.maximumHandshakeFrameBytes, 'x');
    auto oversizedHelloBytes
        = codec::encode(1, codec::toWire(std::move(oversizedHello)));
    require(oversizedHelloBytes.size()
            > handshakeFrameOptions.maximumHandshakeFrameBytes,
        "oversized hello fixture did not exceed the handshake frame cap");
    auto oversizedHelloSocket
        = connectTo("127.0.0.1", handshakeFrameServer.port());
    writeFrame(oversizedHelloSocket, oversizedHelloBytes,
        defaultMaximumFrameBytes);
    bool oversizedHelloRejected = false;
    try {
        oversizedHelloRejected = readWithDeadline(oversizedHelloSocket,
                                     defaultMaximumFrameBytes,
                                     std::chrono::seconds{2})
            == nullptr;
    } catch (const std::exception&) {
        oversizedHelloRejected = true;
    }
    require(oversizedHelloRejected,
        "server accepted a hello above the pre-authentication frame cap");
    auto boundedHandshakeSocket
        = connectTo("127.0.0.1", handshakeFrameServer.port());
    envelope = exchange(boundedHandshakeSocket, 1,
        codec::toWire(helloRequest()),
        defaultMaximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse,
        "server rejected a hello within the pre-authentication frame cap");
    envelope = exchange(boundedHandshakeSocket, 2,
        codec::toWire(OpenDatasetData{
            std::string(
                handshakeFrameOptions.maximumHandshakeFrameBytes * 2U, 'x'),
            16ULL * 1024ULL * 1024ULL}),
        defaultMaximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse,
        "server retained the handshake frame cap after authentication");
    require(handshakeFrameRunning.stopWithin(std::chrono::seconds{2}),
        "handshake-frame server shutdown exceeded its deadline");

    ServerOptions boundedOptions;
    boundedOptions.workerCount = 1;
    boundedOptions.maximumDatasets = 1;
    boundedOptions.maximumFrameBytes = 1024;
    Server boundedServer(boundedOptions);
    RunningServer boundedRunning(boundedServer);
    auto boundedSocket = connectTo("127.0.0.1", boundedServer.port());
    auto boundedHello = helloRequest();
    boundedHello.maximumFrameBytes = boundedOptions.maximumFrameBytes;
    envelope = exchange(boundedSocket, 1, codec::toWire(boundedHello),
        boundedOptions.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse,
        "small-frame server rejected a fitting hello response");
    for (std::uint64_t requestId : {2ULL, 3ULL}) {
        envelope = exchange(boundedSocket, requestId,
            codec::toWire(OpenDatasetData{
                std::filesystem::path(argv[1]).string(),
                16ULL * 1024ULL * 1024ULL}),
            boundedOptions.maximumFrameBytes);
        require(codec::inspect(*envelope).payload
                    == PayloadKind::ErrorResponse
                && codec::fromWire(*envelope->payload.AsErrorResponse()).code
                    == ErrorCode::ResourceLimitExceeded
                && codec::fromWire(*envelope->payload.AsErrorResponse())
                       .message.find("catalog")
                    != std::string::npos,
            "oversized dataset catalog was not rejected before publication");
    }
    require(boundedRunning.stopWithin(std::chrono::seconds{2}),
        "small-frame server shutdown exceeded its deadline");
    std::filesystem::remove_all(
        std::filesystem::path(argv[1]) / "Oversized");
    return 0;
}
