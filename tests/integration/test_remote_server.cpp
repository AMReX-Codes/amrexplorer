#include "../../src/remote/Codec.hpp"

#include <amrexplorer/data/ViewData.hpp>
#include <amrexplorer/remote/Frame.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
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

private:
    amrvis::remote::Server& m_server;
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

} // namespace

int main(int argc, char* argv[])
{
    using namespace amrvis;
    using namespace amrvis::remote;

    if (argc != 2) {
        std::cerr << "usage: test_remote_server MATERIALIZED_PLOTFILE\n";
        return 2;
    }

    ServerOptions options;
    options.workerCount = 2;
    options.maximumDatasets = 2;
    options.maximumOutstandingRequests = 8;
    Server server(options);
    RunningServer running(server);

    auto socket = connectTo("127.0.0.1", server.port());
    auto envelope = exchange(socket, 1,
        codec::toWire(HelloRequestData{
            "server integration test", "test", 0, protocolMinor,
            defaultMaximumFrameBytes}),
        defaultMaximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse,
        "server rejected a compatible handshake");
    const auto hello = codec::fromWire(
        *envelope->payload.AsHelloResponse());
    require(hello.selectedMinor == protocolMinor
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

    codec::fb::CloseDatasetRequestT close;
    close.dataset_id = opened.id.value;
    envelope = exchange(socket, 5, std::move(close),
        hello.maximumFrameBytes);
    require(codec::inspect(*envelope).payload == PayloadKind::DatasetClosed,
        "server did not close the dataset");
    return 0;
}
