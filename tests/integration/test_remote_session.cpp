#include <amrexplorer/pipeline/SlicePipeline.hpp>
#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/RemoteDatasetSession.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <variant>

namespace {

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

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

amrvis::SliceRequest sliceRequest(
    const amrvis::DatasetSession& dataset, int width, int height)
{
    amrvis::SliceRequest request;
    request.dataset = dataset.id();
    request.field = amrvis::FieldId{0};
    request.normalDirection
        = std::max(0, dataset.metadata().dimension - 1);
    request.visibleRegion
        = amrvis::datasetSampleBounds(dataset.metadata());
    const auto normal = static_cast<std::size_t>(request.normalDirection);
    request.physicalPosition
        = 0.5 * (request.visibleRegion.lower[normal]
            + request.visibleRegion.upper[normal]);
    request.maximumLevel = dataset.metadata().finestLevel;
    request.outputSize = {width, height};
    return request;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "usage: test_remote_session MATERIALIZED_PLOTFILE\n";
        return 2;
    }
    try {
        amrvis::remote::ServerOptions options;
        options.workerCount = 3;
        options.maximumDatasets = 4;
        options.maximumOutstandingRequests = 16;
        amrvis::remote::Server server(options);
        ServerThread serverThread(server);

        auto connection = std::make_shared<amrvis::remote::Connection>(
            "127.0.0.1", server.port());
        require(connection->serverInfo().workerCount == 3,
            "handshake did not report worker count");
        auto dataset = amrvis::remote::RemoteDatasetSession::open(
            connection, std::filesystem::path(argv[1]).string(),
            16ULL * 1024ULL * 1024ULL);
        require(dataset->metadata().dimension == 2,
            "remote catalog has the wrong dimension");
        require(dataset->metadata().levels.size() == 2,
            "remote catalog has the wrong level count");
        for (const auto& level : dataset->metadata().levels) {
            require(level.blocks.empty(),
                "remote catalog exposed storage block metadata");
            require(!level.boxes.empty(),
                "remote catalog omitted AMR wireframe boxes");
        }

        const auto slice = std::get<amrvis::SliceQueryResult>(
            dataset->requestView(sliceRequest(*dataset, 8, 6)));
        require(slice.plane.width == 8 && slice.plane.height == 6
                && slice.plane.values.size() == 48,
            "remote slice did not honor the viewport extent");

        amrvis::LineViewRequest line;
        line.query.dataset = dataset->id();
        line.query.field = amrvis::FieldId{0};
        line.query.axis = 0;
        line.query.fixedCoordinates = {0.0, 0.5, 0.0};
        line.query.maximumLevel = dataset->metadata().finestLevel;
        line.query.region = amrvis::datasetSampleBounds(dataset->metadata());
        line.outputWidth = 2;
        const auto lineResult = std::get<amrvis::LineQueryResult>(
            dataset->requestView(line));
        require(lineResult.line.values.size() <= 4,
            "remote line exceeded its two-samples-per-pixel bound");
        line.outputWidth = 20000;
        try {
            static_cast<void>(dataset->requestView(line));
            require(false, "oversized line viewport was accepted");
        } catch (const amrvis::remote::RemoteError& error) {
            require(error.code()
                    == amrvis::remote::ErrorCode::ResourceLimitExceeded,
                "oversized line returned the wrong error");
        }

        amrvis::DatasetPageRequest pageRequest;
        pageRequest.dataset = dataset->id();
        pageRequest.field = amrvis::FieldId{0};
        pageRequest.level = 0;
        pageRequest.region = amrvis::datasetSampleBounds(dataset->metadata());
        pageRequest.normalAxis = 1;
        pageRequest.maximumExtent = 2;
        const auto page = dataset->requestDatasetPage(pageRequest);
        require(page.nx <= 2 && page.ny <= 2
                && page.values.size() <= 4,
            "remote dataset page exceeded its requested extent");

        const auto range = dataset->requestRange(amrvis::RangeRequest{
            .field = amrvis::FieldId{0},
            .maximumLevel = dataset->metadata().finestLevel,
            .composition = amrvis::CompositionPolicy::FinestAvailable,
            .scope = amrvis::RangeScope::File});
        require(range.has_value(), "remote file range is missing");
        require(!dataset->particleSpecies().empty(),
            "remote particle catalog is missing");
        const auto particles = dataset->requestParticleSample(
            dataset->particleSpecies().front().name, 1.0, 37);
        require(!particles.points.empty(),
            "remote particle sample is empty");

        require(dataset->setCacheBudget(8ULL * 1024ULL * 1024ULL),
            "remote cache budget update was rejected");
        dataset->clearUnpinnedCache();
        require(dataset->cacheMetrics().budgetBytes
                == 8ULL * 1024ULL * 1024ULL,
            "remote cache snapshot is stale");

        amrvis::StopSource cancelled;
        cancelled.request_stop();
        try {
            static_cast<void>(dataset->requestView(
                sliceRequest(*dataset, 4, 4), cancelled.get_token()));
            require(false, "pre-cancelled remote request completed");
        } catch (const amrvis::ReadCancelled&) {
        }

        auto secondDataset = amrvis::remote::RemoteDatasetSession::open(
            connection, std::filesystem::path(argv[1]).string(),
            8ULL * 1024ULL * 1024ULL);
        require(secondDataset->id() != dataset->id(),
            "multiple remote datasets reused one handle");
        secondDataset->close();

        auto parallelConnection
            = std::make_shared<amrvis::remote::Connection>(
                "127.0.0.1", server.port());
        auto parallelDataset
            = amrvis::remote::RemoteDatasetSession::open(
                parallelConnection,
                std::filesystem::path(argv[1]).string(),
                8ULL * 1024ULL * 1024ULL);
        require(std::get<amrvis::SliceQueryResult>(
                    parallelDataset->requestView(
                        sliceRequest(*parallelDataset, 2, 2)))
                    .plane.values.size()
                == 4,
            "second client connection could not query");
        parallelDataset->close();
        parallelConnection->close();

        const auto request = sliceRequest(*dataset, 7, 5);
        auto first = std::async(std::launch::async,
            [&] { return dataset->requestView(request); });
        auto second = std::async(std::launch::async,
            [&] { return dataset->requestView(request); });
        require(std::get<amrvis::SliceQueryResult>(first.get())
                    .plane.values.size()
                == 35
                && std::get<amrvis::SliceQueryResult>(second.get())
                    .plane.values.size()
                == 35,
            "concurrent remote requests were not matched correctly");

        dataset->close();
        connection->close();

        auto reconnected = std::make_shared<amrvis::remote::Connection>(
            "127.0.0.1", server.port());
        auto reopened = amrvis::remote::RemoteDatasetSession::open(
            reconnected, std::filesystem::path(argv[1]).string(),
            8ULL * 1024ULL * 1024ULL);
        require(std::get<amrvis::SliceQueryResult>(
                    reopened->requestView(sliceRequest(*reopened, 3, 2)))
                    .plane.values.size()
                == 6,
            "reconnect/reopen did not resume queries");
        reopened->close();
        reconnected->close();

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
