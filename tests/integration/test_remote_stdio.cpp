// The stdio transport: a Server serving one pre-connected Channel, in-process
// over a socketpair (the shape Linux sshd gives a command: one socket as both
// stdin and stdout) and as the real amrexplorer-server subprocess over pipes
// (the shape other sshds and shells give it). The property that matters most
// is lifetime: when the client closes its end, run() returns and the process
// exits, which is what keeps ssh-launched servers from outliving the client.

#include <amrexplorer/data/ViewData.hpp>
#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/Frame.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <variant>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

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

// Runs server.run() on a thread and reports whether it returned within a
// bound, which is how these tests observe the session ending.
class RunningServer {
public:
    explicit RunningServer(amrvis::remote::Server& server)
        : m_server(server)
        , m_done(m_promise.get_future())
        , m_thread([this] {
            try {
                m_server.run();
            } catch (...) {
                m_failure = std::current_exception();
            }
            m_promise.set_value();
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

    [[nodiscard]] bool returnedWithin(std::chrono::milliseconds bound)
    {
        return m_done.wait_for(bound) == std::future_status::ready;
    }

private:
    amrvis::remote::Server& m_server;
    std::promise<void> m_promise;
    std::future<void> m_done;
    std::thread m_thread;
    std::exception_ptr m_failure;
};

struct SocketPair {
    int server = -1;
    int client = -1;
};

SocketPair makeSocketPair()
{
    int descriptors[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
        "socketpair failed");
    return {descriptors[0], descriptors[1]};
}

void exerciseDataset(amrvis::remote::Connection& connection,
    const std::string& datasetPath)
{
    connection.ping();
    // main() pointed HOME at the fixture's parent, so the home-relative
    // spelling of the same dataset must open as well: the server expands a
    // leading tilde, since "~/..." never means anything to the filesystem.
    const auto homeRelative = "~/"
        + std::filesystem::path(datasetPath).filename().string();
    const auto viaTilde
        = connection.openDataset(homeRelative, 16ULL * 1024ULL * 1024ULL);
    require(viaTilde.catalog.dimension == 2,
        "server did not expand a home-relative dataset path");
    connection.closeDataset(viaTilde.id);
    const auto opened
        = connection.openDataset(datasetPath, 16ULL * 1024ULL * 1024ULL);
    require(opened.catalog.dimension == 2,
        "stdio connection did not decode the dataset catalog");
    const auto view = connection.requestView(sliceRequest(opened, 7, 5));
    require(std::get<amrvis::SliceQueryResult>(view).plane.values.size() == 35,
        "stdio connection did not return the requested slice");
    connection.closeDataset(opened.id);
}

void inProcessOverSocketPair(const std::string& datasetPath)
{
    using namespace amrvis::remote;

    // The server reads and writes through two descriptors on one socket,
    // exactly what sshd hands a no-pty command on Linux.
    {
        const auto pair = makeSocketPair();
        const int serverWrite = ::dup(pair.server);
        require(serverWrite >= 0, "dup failed");
        ServerOptions options;
        options.workerCount = 2;
        Server server(std::make_unique<DescriptorChannel>(
                          pair.server, serverWrite),
            options);
        require(server.port() == 0, "single-session server reported a port");
        RunningServer running(server);

        Connection connection(
            std::make_unique<Socket>(adoptStreamSocket(pair.client)),
            ConnectionOptions{.sessionToken = server.token()});
        require(connection.connected()
                && connection.serverInfo().workerCount == 2,
            "stdio connection did not complete the handshake");
        exerciseDataset(connection, datasetPath);

        connection.close();
        require(running.returnedWithin(5s),
            "server did not stop when the stdio client closed its end");
    }

    // A wrong token is refused over stdio like everywhere else.
    {
        const auto pair = makeSocketPair();
        Server server(std::make_unique<DescriptorChannel>(
                          pair.server, pair.server),
            ServerOptions{});
        RunningServer running(server);
        try {
            Connection connection(
                std::make_unique<Socket>(adoptStreamSocket(pair.client)),
                ConnectionOptions{.sessionToken = "not-the-token"});
            require(false, "stdio server accepted a wrong token");
        } catch (const std::exception&) {
        }
        require(running.returnedWithin(5s),
            "server did not stop after refusing the handshake");
    }

    // A stream that ends inside a frame ends the session as well.
    {
        const auto pair = makeSocketPair();
        Server server(std::make_unique<DescriptorChannel>(
                          pair.server, pair.server),
            ServerOptions{});
        RunningServer running(server);
        const unsigned char partialHeader[2] = {0, 0};
        require(::send(pair.client, partialHeader, sizeof(partialHeader), 0)
                == 2,
            "could not write the partial header");
        ::close(pair.client);
        require(running.returnedWithin(5s),
            "server did not stop after EOF inside a frame");
    }

    // requestStop() ends an idle session and unblocks the client's reader.
    {
        const auto pair = makeSocketPair();
        Server server(std::make_unique<DescriptorChannel>(
                          pair.server, pair.server),
            ServerOptions{});
        RunningServer running(server);
        Connection connection(
            std::make_unique<Socket>(adoptStreamSocket(pair.client)),
            ConnectionOptions{.sessionToken = server.token()});
        connection.ping();
        server.requestStop();
        require(running.returnedWithin(5s),
            "server did not honour requestStop over stdio");
        const auto lostAt = std::chrono::steady_clock::now() + 5s;
        while (connection.connected()
            && std::chrono::steady_clock::now() < lostAt) {
            std::this_thread::sleep_for(10ms);
        }
        require(!connection.connected(),
            "client did not notice the stdio server leaving");
    }
}

// The installed binary over pipes, as a wrapper script or a non-Linux sshd
// would run it: distinct stdin and stdout, a ready line first, frames after,
// exit 0 once the client hangs up.
void subprocessOverPipes(
    const std::string& datasetPath, const std::string& serverBinary)
{
    using namespace amrvis::remote;

    int toServer[2] = {-1, -1};
    int fromServer[2] = {-1, -1};
    require(::pipe(toServer) == 0 && ::pipe(fromServer) == 0, "pipe failed");
    const pid_t child = ::fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        ::dup2(toServer[0], STDIN_FILENO);
        ::dup2(fromServer[1], STDOUT_FILENO);
        ::close(toServer[0]);
        ::close(toServer[1]);
        ::close(fromServer[0]);
        ::close(fromServer[1]);
        ::execl(serverBinary.c_str(), serverBinary.c_str(), "--stdio",
            "--threads", "2", static_cast<char*>(nullptr));
        std::_Exit(127);
    }
    ::close(toServer[0]);
    ::close(fromServer[1]);

    // The ready line, byte by byte: the server writes nothing after it until
    // it has our hello, so no frame byte can be swallowed here.
    std::string readyLine;
    for (;;) {
        char byte = 0;
        const auto count = ::read(fromServer[0], &byte, 1);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        require(count == 1, "server closed the pipe before the ready line");
        if (byte == '\n') {
            break;
        }
        readyLine.push_back(byte);
        require(readyLine.size() < 4096, "ready line is unreasonably long");
    }
    const std::string prefix = "AMREXPLORER-STDIO 1 TOKEN ";
    require(readyLine.compare(0, prefix.size(), prefix) == 0,
        "server did not print the stdio ready line first");
    const auto token = readyLine.substr(prefix.size());
    require(!token.empty(), "ready line carried no token");

    {
        Connection connection(
            std::make_unique<DescriptorChannel>(fromServer[0], toServer[1]),
            ConnectionOptions{.sessionToken = token});
        require(connection.connected(),
            "subprocess connection did not complete the handshake");
        exerciseDataset(connection, datasetPath);
        connection.close();
    }

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
            break;
        }
        require(waited == 0 || errno == EINTR, "waitpid failed");
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(child, SIGKILL);
            ::waitpid(child, &status, 0);
            require(false,
                "amrexplorer-server --stdio did not exit after the client "
                "closed the stream");
        }
        std::this_thread::sleep_for(20ms);
    }
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "amrexplorer-server --stdio did not exit cleanly");
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 3) {
        std::cerr << "usage: test_remote_stdio DATASET SERVER_BINARY\n";
        return 2;
    }
    // Every peer departure in these tests must arrive as an error code.
    std::signal(SIGPIPE, SIG_IGN);
    const auto datasetPath = std::filesystem::path(argv[1]).string();
    // For the "~/..." expansion checks, in this process and the subprocess.
    // Before any thread exists: setenv is not thread-safe against getenv.
    ::setenv("HOME",
        std::filesystem::path(datasetPath).parent_path().c_str(), 1);
    inProcessOverSocketPair(datasetPath);
    subprocessOverPipes(datasetPath, argv[2]);
    return 0;
}
