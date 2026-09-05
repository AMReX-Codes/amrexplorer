// Volume rendering over the wire (protocol 1.2), against an in-process
// server on the materialized 3-D fixture: a remote render is the local
// render byte for byte; the server's grid cache serves the second frame;
// a Visible range is resolved on the server and reported back; the server's
// voxel cap clamps a larger request; a frame that cannot fit the negotiated
// frame is refused; a cancelled render throws; and a client that negotiated
// protocol 1.1 is told the capability needs 1.2.

#include "../../src/remote/Codec.hpp"

#include <amrexplorer/data/LocalDatasetSession.hpp>
#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/Frame.hpp>
#include <amrexplorer/remote/RemoteDatasetSession.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

class ServerThread {
public:
    explicit ServerThread(amrvis::remote::Server& server)
        : m_server(server)
        , m_thread([&server] { server.run(); })
    {
    }

    ~ServerThread()
    {
        m_server.requestStop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    ServerThread(const ServerThread&) = delete;
    ServerThread& operator=(const ServerThread&) = delete;

private:
    amrvis::remote::Server& m_server;
    std::thread m_thread;
};

amrvis::VolumeRenderRequest requestFor(const amrvis::DatasetSession& dataset)
{
    amrvis::VolumeRenderRequest request;
    request.dataset = dataset.id();
    request.field = amrvis::FieldId{0};
    request.maximumLevel = dataset.metadata().finestLevel;
    request.region = amrvis::datasetSampleBounds(dataset.metadata());
    request.camera = {0.55, 0.35, 1.2};
    request.outputSize = {96, 80};
    request.range = amrvis::VolumeRange{0.0, 1.0, {amrvis::ColorScale::Linear}};
    // A ramp from transparent to opaque with a bright colour, so the
    // analytic (i + j + k) / 9 field draws something over its whole range.
    for (int entry = 0; entry < 16; ++entry) {
        request.transfer.colors.push_back(0xFF8000U + static_cast<std::uint32_t>(entry));
        request.transfer.opacities.push_back(static_cast<float>(entry) / 15.0F);
    }
    request.samplesPerVoxel = 2;
    request.maximumVoxels = 4096;
    return request;
}

std::size_t litPixels(const amrvis::VolumeFrame& frame)
{
    std::size_t lit = 0;
    for (const auto pixel : frame.pixels) {
        lit += (pixel >> 24U) != 0U;
    }
    return lit;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "usage: test_remote_volume MATERIALIZED_3D_PLOTFILE\n";
        return 2;
    }
    const std::string path = std::filesystem::path(argv[1]).string();
    try {
        using namespace amrvis;
        using namespace amrvis::remote;

        ServerOptions options;
        options.workerCount = 2;
        Server server(options);
        ServerThread running(server);
        auto connection = std::make_shared<Connection>("127.0.0.1", server.port(),
            ConnectionOptions{.sessionToken = server.token()});
        require(connection->serverInfo().selectedMinorVersion >= 2,
            "the handshake did not negotiate protocol 1.2");

        auto remote = RemoteDatasetSession::open(connection, path, 16ULL << 20);
        auto local = std::make_shared<LocalDatasetSession>(
            std::filesystem::path(argv[1]), DatasetId{77}, 16ULL << 20);
        require(remote->supportsVolumeRendering() && local->supportsVolumeRendering(),
            "the 3-D fixture is not volume-renderable on both sides");

        // --- the same request renders the same frame -------------------
        const auto remoteRequest = requestFor(*remote);
        auto localRequest = remoteRequest;
        localRequest.dataset = local->id();
        const auto remoteFrame = remote->renderVolume(remoteRequest);
        const auto localFrame = local->renderVolume(localRequest);
        require(remoteFrame.width == 96 && remoteFrame.height == 80
                && litPixels(remoteFrame) > 0,
            "the remote frame is empty or the wrong size");
        require(remoteFrame.pixels == localFrame.pixels,
            "the remote and local frames differ");
        require(remoteFrame.usedRange == localFrame.usedRange
                && remoteFrame.metrics.gridDims == localFrame.metrics.gridDims
                && remoteFrame.metrics.coveredVoxels == localFrame.metrics.coveredVoxels
                && remoteFrame.metrics.sampledMaximumLevel
                    == localFrame.metrics.sampledMaximumLevel
                && !remoteFrame.metrics.gridFromCache,
            "the remote and local metrics differ");
        require(remoteFrame.metrics.gridDims == (std::array<int, 3>{4, 4, 4})
                && remoteFrame.metrics.coveredVoxels == 64,
            "the fixture did not sample to its native 4x4x4 grid");

        // --- the second frame comes from the server's grid cache --------
        const auto cached = remote->renderVolume(remoteRequest);
        require(cached.metrics.gridFromCache && cached.pixels == remoteFrame.pixels,
            "the server did not serve the second frame from its grid cache");

        // --- a Visible range is resolved on the server ------------------
        auto visible = remoteRequest;
        visible.range.reset();
        const auto resolved = remote->renderVolume(visible);
        require(resolved.usedRange.minimum == 0.0 && resolved.usedRange.maximum == 1.0 &&
                    resolved.usedRange.scale.scale != amrvis::ColorScale::Logarithmic,
                "the server did not report the range it resolved");
        visible.scale = {amrvis::ColorScale::Logarithmic};
        const auto resolvedLog = remote->renderVolume(visible);
        // The field runs down to zero, so a logarithmic range is not viable
        // and the server falls back to linear over every finite value --
        // visibleVolumeRange's rule, answered the same way it would be
        // locally. Asserting the fallback itself, because the "usable and
        // honest" form of this was a tautology: validateSessionVolumeResult
        // has already thrown on every frame that fails it, so it could not
        // fire.
        require(resolvedLog.usedRange.scale.scale != amrvis::ColorScale::Logarithmic &&
                    resolvedLog.usedRange.minimum == resolved.usedRange.minimum &&
                    resolvedLog.usedRange.maximum == resolved.usedRange.maximum,
                "asking for a logarithmic Visible range on non-positive data did "
                "not fall back to the linear one");

        // --- cancellation ------------------------------------------------
        {
            StopSource stop;
            stop.request_stop();
            bool threw = false;
            try {
                static_cast<void>(remote->renderVolume(remoteRequest, stop.get_token()));
            } catch (const ReadCancelled&) {
                threw = true;
            }
            require(threw, "a cancelled remote render did not throw ReadCancelled");
        }

        // --- a frame that cannot fit the negotiated frame is refused -----
        {
            auto small = std::make_shared<Connection>("127.0.0.1", server.port(),
                ConnectionOptions{.maximumFrameBytes = 64U * 1024U,
                    .sessionToken = server.token()});
            auto smallSession = RemoteDatasetSession::open(small, path, 16ULL << 20);
            auto huge = requestFor(*smallSession);
            huge.outputSize = {512, 512};   // 1 MiB of pixels
            bool refused = false;
            try {
                static_cast<void>(smallSession->renderVolume(huge));
            } catch (const RemoteError& error) {
                refused = error.code() == ErrorCode::ResourceLimitExceeded;
            }
            require(refused, "an oversized frame was not refused as a resource limit");
            require(small->connected(), "the refusal cost the connection");
            smallSession->close();
        }

        // --- an unconfigured server does not cap below the protocol -----
        // It used to default to 256^3 while a request may legally ask for
        // 512^3 and the volume window's High preset asks for 384^3, so every
        // High render against a default server came back at Normal's detail
        // with nothing saying so. Asserted on the option rather than on a
        // rendered grid because the fixtures here are far smaller than any of
        // these budgets: their grids are bounded by the data, so no frame from
        // them can tell the two defaults apart.
        require(ServerOptions{}.maximumVolumeVoxels
                == amrvis::maxVolumeVoxelBudget,
            "an unconfigured server caps the sampled grid below the ceiling "
            "a request may ask for");

        // --- the server's voxel cap clamps the request ------------------
        // And the contrast: a cap the operator chose still clamps, which is
        // what --max-volume-voxels is now solely for.
        {
            ServerOptions capped;
            capped.workerCount = 1;
            capped.maximumVolumeVoxels = 8;
            Server cappedServer(capped);
            ServerThread cappedRunning(cappedServer);
            auto cappedConnection = std::make_shared<Connection>("127.0.0.1",
                cappedServer.port(), ConnectionOptions{.sessionToken = cappedServer.token()});
            auto cappedSession = RemoteDatasetSession::open(
                cappedConnection, path, 16ULL << 20);
            const auto frame = cappedSession->renderVolume(requestFor(*cappedSession));
            require(frame.metrics.gridDims == (std::array<int, 3>{2, 2, 2}),
                "the server's voxel cap did not clamp the sampled grid");
            cappedSession->close();
        }

        // --- the grid pool answers to the operator, not to the peer -------
        // --volume-cache-mib sets the sampled-grid pool, and a client's
        // SetCacheBudgetRequest reaches only the block pool. The client's
        // number does not lower the grid pool either: it is a budget for a
        // different pool holding different things, and letting it stand in
        // for one would let a peer starve or zero a pool the operator sized.
        {
            ServerOptions bounded;
            bounded.workerCount = 1;
            bounded.volumeGridCacheBytes = 64;   // below any grid here
            Server boundedServer(bounded);
            ServerThread boundedRunning(boundedServer);
            auto boundedConnection = std::make_shared<Connection>("127.0.0.1",
                boundedServer.port(),
                ConnectionOptions{.sessionToken = boundedServer.token()});
            auto boundedSession = RemoteDatasetSession::open(
                boundedConnection, path, 16ULL << 20);
            const auto request = requestFor(*boundedSession);
            // The grid outsizes the pool, so it is never served from cache
            // however many times it is asked for.
            static_cast<void>(boundedSession->renderVolume(request));
            const auto again = boundedSession->renderVolume(request);
            require(!again.metrics.gridFromCache,
                "a grid over the server's cap was cached anyway");
            // Asking for a gigabyte does not change that.
            require(boundedSession->setCacheBudget(1ULL << 30),
                "the remote cache budget update was refused");
            static_cast<void>(boundedSession->renderVolume(request));
            const auto afterRaise = boundedSession->renderVolume(request);
            require(!afterRaise.metrics.gridFromCache,
                "a client raised the server's grid-cache bound");
            require(boundedConnection->connected(),
                "the budget exchange cost the connection");
            boundedSession->close();
        }

        // --- a 1.1 client is told volume rendering needs 1.2 ------------
        {
            auto socket = connectTo("127.0.0.1", server.port());
            HelloRequestData hello{"volume test", "test", 0, 1,
                defaultMaximumFrameBytes, server.token(), {}};
            writeFrame(socket, codec::encode(1, codec::toWire(hello), 1),
                defaultMaximumFrameBytes);
            auto response = readFrame(socket, defaultMaximumFrameBytes);
            require(response.has_value(), "the server closed on a 1.1 hello");
            auto envelope = codec::decode(*response);
            require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse
                    && codec::fromWire(*envelope->payload.AsHelloResponse())
                            .selectedMinorVersion == 1,
                "the server did not negotiate 1.1 with a 1.1 client");
            auto request = requestFor(*remote);
            request.dataset = DatasetId{1};   // any: the gate fires first
            writeFrame(socket, codec::encode(2, codec::toWire(request), 1),
                defaultMaximumFrameBytes);
            response = readFrame(socket, defaultMaximumFrameBytes);
            require(response.has_value(), "the server closed on a 1.1 volume request");
            envelope = codec::decode(*response);
            require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse
                    && codec::fromWire(*envelope->payload.AsErrorResponse()).code
                        == ErrorCode::UnsupportedProtocol,
                "a 1.1 client's volume request was not refused as unsupported");
        }

        // --- a 1.2 client is told smooth sampling needs 1.3 -------------
        //
        // The narrower gate: 1.2 renders volumes perfectly well and simply
        // has no field to ask how they are sampled. A real 1.2 client cannot
        // send one, so reaching the server with Linear means a peer claiming
        // one version and speaking another -- and the answer has to be a
        // refusal it can read rather than a frame sampled by another rule.
        // The connection has to survive it: nothing about this says the peer
        // misbehaved badly enough to lose every other dataset on it.
        {
            auto socket = connectTo("127.0.0.1", server.port());
            HelloRequestData hello{"volume test", "test", 0, 2,
                defaultMaximumFrameBytes, server.token(), {}};
            writeFrame(socket, codec::encode(1, codec::toWire(hello), 2),
                defaultMaximumFrameBytes);
            auto response = readFrame(socket, defaultMaximumFrameBytes);
            require(response.has_value(), "the server closed on a 1.2 hello");
            auto envelope = codec::decode(*response);
            require(codec::inspect(*envelope).payload == PayloadKind::HelloResponse
                    && codec::fromWire(*envelope->payload.AsHelloResponse())
                            .selectedMinorVersion == 2,
                "the server did not negotiate 1.2 with a 1.2 client");
            auto request = requestFor(*remote);
            request.sampling = SamplingPolicy::Linear;
            writeFrame(socket, codec::encode(2, codec::toWire(request), 2),
                defaultMaximumFrameBytes);
            response = readFrame(socket, defaultMaximumFrameBytes);
            require(response.has_value(),
                "the server closed on a 1.2 client asking for smooth sampling");
            envelope = codec::decode(*response);
            require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse
                    && codec::fromWire(*envelope->payload.AsErrorResponse()).code
                        == ErrorCode::UnsupportedProtocol,
                "a 1.2 client's smooth-sampling request was not refused");

            // And the connection is still there to be asked again. A
            // refusal over a capability is not misbehaviour, and losing the
            // session -- with every other dataset open on it -- would be a far
            // larger answer than the question deserves.
            writeFrame(socket, codec::encode(3, codec::toWire(request), 2),
                defaultMaximumFrameBytes);
            response = readFrame(socket, defaultMaximumFrameBytes);
            require(response.has_value(),
                "the refusal closed the connection instead of answering");
            envelope = codec::decode(*response);
            require(codec::inspect(*envelope).payload == PayloadKind::ErrorResponse
                    && codec::fromWire(*envelope->payload.AsErrorResponse()).code
                        == ErrorCode::UnsupportedProtocol,
                "the connection stopped answering after one refusal");
        }
        remote->close();
        local->close();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
