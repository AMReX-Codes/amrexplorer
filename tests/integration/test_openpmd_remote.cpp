#include <amrexplorer/data/LocalDatasetSession.hpp>
#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/RemoteDatasetSession.hpp>
#include <amrexplorer/remote/Server.hpp>

#include "OpenPmdFixtures.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

// Protocol 1.10 against an in-process server: an open names an openPMD
// iteration, the reply lists the series' iterations, and what the server
// reads for an iteration (catalog, particles) is what a local session reads.

namespace {

using namespace amrvis::test::openpmd;

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

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

constexpr std::uint64_t budget = 16ULL << 20U;

} // namespace

int main()
{
    const auto directory = std::filesystem::temp_directory_path()
        / ("amrexplorer-openpmd-remote-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(directory);
    const auto file = directory / "series.json";
    writeSeries(file);
    try {
        amrvis::remote::Server server(amrvis::remote::ServerOptions{});
        ServerThread serverThread(server);
        auto connection = std::make_shared<amrvis::remote::Connection>(
            "127.0.0.1", server.port(),
            amrvis::remote::ConnectionOptions{.sessionToken = server.token()});
        require(connection->supportsSeriesIterations(),
            "a current server names openPMD iterations");

        const auto expected = std::vector<std::uint64_t>(
            seriesIterations.begin(), seriesIterations.end());
        {
            auto first = amrvis::remote::RemoteDatasetSession::open(
                connection, file.string(), budget);
            require(first->seriesIterations() == expected,
                "the reply lists the series' iterations");
            require(!first->iteration(), "no iteration was named");
            require(first->metadata().levels[0].step == 10,
                "without an iteration the server opens the first");
        }

        auto remote = amrvis::remote::RemoteDatasetSession::open(
            connection, file.string(), budget, {}, {}, 20);
        amrvis::LocalDatasetSession local({file, 20}, amrvis::DatasetId{1}, budget);
        require(remote->iteration() == 20, "the session keeps its iteration");
        require(remote->metadata().levels[0].step == 20,
            "the server opened the iteration asked for");
        require(remote->particleSpecies() == local.particleSpecies(),
            "the remote particle catalog is the local one");
        using Kind = amrvis::ParticleAttribute::Kind;
        const auto remoteSample = remote->requestParticleSample("electrons", 0.5,
            7, {}, amrvis::ParticleAttribute{Kind::Real, 3});
        const auto localSample = local.requestParticleSample("electrons", 0.5,
            7, {}, amrvis::ParticleAttribute{Kind::Real, 3});
        require(!remoteSample.points.empty()
                && remoteSample.points.size() == localSample.points.size(),
            "the remote sample has the local one's points");
        for (std::size_t i = 0; i < localSample.points.size(); ++i) {
            const auto& a = remoteSample.points[i];
            const auto& b = localSample.points[i];
            require(a.id == b.id && a.position == b.position && a.value == b.value,
                "remote particles match local ones");
        }

        bool refused = false;
        try {
            static_cast<void>(amrvis::remote::RemoteDatasetSession::open(
                connection, file.string(), budget, {}, {}, 99));
        } catch (const std::exception&) {
            refused = true;
        }
        require(refused, "an iteration the series lacks is refused");
        require(connection->connected(),
            "a refused iteration leaves the connection usable");

        // The browser's path to a series of one file per iteration: listed
        // as its %T pattern, which opens every iteration.
        const auto perIteration = directory / "per-iteration";
        std::filesystem::create_directories(perIteration);
        writeSeries(perIteration / "diag_%06T.json");
        const auto listing = connection->listDirectory(perIteration.string());
        require(listing.entries.size() == 1
                && listing.entries.front().name == "diag_%T.json"
                && listing.entries.front().isOpenPmd,
            "the series' files are listed as one pattern");
        auto pattern = amrvis::remote::RemoteDatasetSession::open(
            connection, listing.entries.front().path, budget);
        require(pattern->seriesIterations() == expected,
            "the pattern opens every iteration of the series");
        auto last = amrvis::remote::RemoteDatasetSession::open(
            connection, listing.entries.front().path, budget, {}, {}, 30);
        require(last->metadata().levels[0].step == 30,
            "an iteration of the pattern opens its own file");
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::filesystem::remove_all(directory);
    std::cout << "openPMD remote tests passed\n";
    return 0;
}
