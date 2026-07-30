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
    return 0;
}
