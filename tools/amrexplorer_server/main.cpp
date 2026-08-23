#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <unistd.h>
#endif

#ifndef AMREXPLORER_VERSION
#define AMREXPLORER_VERSION "0.3.0-dev"
#endif

namespace {

volatile std::sig_atomic_t stopRequested = 0;

void handleSignal(int)
{
    stopRequested = 1;
}

void printUsage(std::ostream& output)
{
    output
        << "usage: amrexplorer-server [options]\n"
        << "  --stdio              serve one client over stdin/stdout; this is\n"
        << "                       how the AMReXplorer client runs the server\n"
        << "                       through ssh\n"
        << "  --port PORT          listen on a loopback port instead; 0 selects\n"
        << "                       an available port (tests and tools)\n"
        << "  --threads COUNT      worker threads; 0 selects hardware concurrency\n"
        << "  --max-frame-mib MIB  maximum negotiated frame size\n"
        << "  --max-datasets COUNT maximum open datasets per connection\n"
        << "  --max-volume-voxels N\n"
        << "                       maximum voxels sampled per volume render;\n"
        << "                       defaults to the 512^3 ceiling, so this is\n"
        << "                       only for tightening. A request asking for\n"
        << "                       more is clamped, not refused\n"
        << "  --volume-cache-mib MIB\n"
        << "                       per-dataset cache of sampled volume grids\n"
        << "  --write-stall-timeout-seconds SECONDS\n"
        << "                       disconnect after no write progress; also the\n"
        << "                       fixed grace in the whole-response budget\n"
        << "  --write-min-kib-per-second KIB\n"
        << "                       floor throughput for the whole-response\n"
        << "                       budget: a response must finish within the\n"
        << "                       stall grace plus size / this rate, so a peer\n"
        << "                       reading a trickle cannot hold a worker\n"
        << "                       forever. Lower it for a slow link; it cannot\n"
        << "                       be zero\n"
        << "  --help               show this help\n";
}

class SignalWatcher {
public:
    explicit SignalWatcher(amrvis::remote::Server& server)
        : m_thread([this, &server] {
            const auto stop = m_stop.get_token();
            while (!stop.stop_requested() && stopRequested == 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(50));
            }
            if (stopRequested != 0) {
                server.requestStop();
            }
        })
    {
    }

    ~SignalWatcher()
    {
        static_cast<void>(m_stop.request_stop());
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    SignalWatcher(const SignalWatcher&) = delete;
    SignalWatcher& operator=(const SignalWatcher&) = delete;

private:
    amrvis::StopSource m_stop;
    std::thread m_thread;
};

template <typename Value>
Value parseUnsigned(const char* text, const char* option)
{
    std::size_t consumed = 0;
    const std::string input(text);
    const auto parsed = std::stoull(input, &consumed);
    if (consumed != input.size()
        || parsed > static_cast<unsigned long long>(
                        std::numeric_limits<Value>::max())) {
        throw std::invalid_argument(
            std::string(option) + " is outside its allowed range");
    }
    return static_cast<Value>(parsed);
}

#ifdef _WIN32
int serveStdio(amrvis::remote::ServerOptions)
{
    throw std::runtime_error("--stdio is not supported on Windows");
}
#else
void writeFully(int descriptor, const std::string& text)
{
    std::size_t written = 0;
    while (written < text.size()) {
        const auto count = ::write(
            descriptor, text.data() + written, text.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // The channel made the descriptor nonblocking already. The
                // signal handlers are installed by now and SignalWatcher is
                // not, so a stop request is honored here or not at all.
                if (stopRequested != 0) {
                    throw std::runtime_error(
                        "interrupted while writing the ready line");
                }
                pollfd waiting{descriptor, POLLOUT, 0};
                static_cast<void>(::poll(&waiting, 1, 1000));
                continue;
            }
            throw std::runtime_error("could not write the ready line");
        }
        written += static_cast<std::size_t>(count);
    }
}

// One session over the process's own stdin/stdout, which under ssh is the
// channel to the client. The wire moves to private duplicates so that stray
// stdout output from anything in the process lands on stderr instead of
// corrupting a frame, and stdin reads nothing but the wire.
int serveStdio(amrvis::remote::ServerOptions options)
{
    const int wireIn = ::dup(STDIN_FILENO);
    const int wireOut = ::dup(STDOUT_FILENO);
    if (wireIn < 0 || wireOut < 0) {
        throw std::runtime_error("could not duplicate stdin/stdout");
    }
    const int devNull = ::open("/dev/null", O_RDWR);
    if (devNull < 0 || ::dup2(devNull, STDIN_FILENO) < 0
        || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        throw std::runtime_error("could not redirect stdin/stdout");
    }
    ::close(devNull);
    // A departed peer must surface as EPIPE from the channel, not kill us.
    std::signal(SIGPIPE, SIG_IGN);
    // The single peer is the authenticated ssh user; the loopback default is
    // sized against local port scanners.
    options.handshakeTimeout = std::chrono::seconds{30};
    amrvis::remote::Server server(
        std::make_unique<amrvis::remote::DescriptorChannel>(wireIn, wireOut),
        std::move(options));
    // The client discards everything up to this line (login-shell chatter),
    // then speaks frames. Nothing else may ever be written to the wire outside
    // the frame layer, and the token stays off stderr: that stream is shown to
    // the user in the client's diagnostics.
    writeFully(wireOut, "AMREXPLORER-STDIO 1 TOKEN " + server.token() + "\n");
    std::cerr << "amrexplorer-server ready (stdio)\n";
    SignalWatcher signalWatcher(server);
    server.run();
    return 0;
}
#endif

} // namespace

int main(int argc, char* argv[])
{
    try {
        amrvis::remote::ServerOptions options;
        // Whether either volume limit was given, for the warning below.
        bool volumeLimitsChosen = false;
        options.softwareVersion = AMREXPLORER_VERSION;
        bool stdio = false;
        bool portGiven = false;
        for (int index = 1; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--help") {
                printUsage(std::cout);
                return 0;
            }
            if (option == "--stdio") {
                stdio = true;
                continue;
            }
            if (index + 1 >= argc) {
                throw std::invalid_argument(
                    "missing value after " + option);
            }
            const auto* value = argv[++index];
            if (option == "--port") {
                options.port
                    = parseUnsigned<std::uint16_t>(value, "--port");
                portGiven = true;
            } else if (option == "--threads") {
                options.workerCount
                    = parseUnsigned<unsigned int>(value, "--threads");
            } else if (option == "--max-frame-mib") {
                const auto mebibytes
                    = parseUnsigned<std::uint32_t>(
                        value, "--max-frame-mib");
                constexpr std::uint32_t oneMebibyte = 1024U * 1024U;
                if (mebibytes == 0
                    || mebibytes
                        > std::numeric_limits<std::uint32_t>::max()
                            / oneMebibyte) {
                    throw std::invalid_argument(
                        "--max-frame-mib is outside its allowed range");
                }
                options.maximumFrameBytes = mebibytes * oneMebibyte;
            } else if (option == "--max-datasets") {
                const auto maximumDatasets
                    = parseUnsigned<std::uint32_t>(value, "--max-datasets");
                if (maximumDatasets == 0) {
                    throw std::invalid_argument(
                        "--max-datasets must be greater than zero");
                }
                options.maximumDatasets = maximumDatasets;
            } else if (option == "--max-volume-voxels") {
                const auto voxels
                    = parseUnsigned<std::uint64_t>(value, "--max-volume-voxels");
                if (voxels == 0 || voxels > amrvis::maxVolumeVoxelBudget) {
                    throw std::invalid_argument(
                        "--max-volume-voxels is outside its allowed range");
                }
                options.maximumVolumeVoxels = voxels;
                volumeLimitsChosen = true;
            } else if (option == "--volume-cache-mib") {
                const auto mebibytes
                    = parseUnsigned<std::uint32_t>(value, "--volume-cache-mib");
                constexpr std::uint64_t oneMebibyte = 1024U * 1024U;
                // Bounded above as well as below, like every other size
                // flag, against the same cap the server applies to a
                // directly-set ServerOptions.
                constexpr std::uint64_t maximumMebibytes
                    = amrvis::maximumVolumeGridCacheBytes / oneMebibyte;
                if (mebibytes == 0 || mebibytes > maximumMebibytes) {
                    throw std::invalid_argument(
                        "--volume-cache-mib is outside its allowed range");
                }
                options.volumeGridCacheBytes
                    = static_cast<std::uint64_t>(mebibytes) * oneMebibyte;
                volumeLimitsChosen = true;
            } else if (option == "--write-stall-timeout-seconds") {
                const auto seconds = parseUnsigned<std::uint32_t>(
                    value, "--write-stall-timeout-seconds");
                if (seconds == 0) {
                    throw std::invalid_argument(
                        "--write-stall-timeout-seconds must be greater than "
                        "zero");
                }
                options.responseWriteStallTimeout = std::chrono::seconds{
                    static_cast<std::chrono::seconds::rep>(seconds)};
            } else if (option == "--write-min-kib-per-second") {
                const auto kibibytes = parseUnsigned<std::uint64_t>(
                    value, "--write-min-kib-per-second");
                constexpr std::uint64_t oneKibibyte = 1024ULL;
                if (kibibytes == 0
                    || kibibytes > std::numeric_limits<std::uint64_t>::max()
                            / oneKibibyte) {
                    throw std::invalid_argument(
                        "--write-min-kib-per-second is outside its allowed "
                        "range");
                }
                options.responseWriteMinimumBytesPerSecond
                    = kibibytes * oneKibibyte;
            } else {
                throw std::invalid_argument("unknown option: " + option);
            }
        }

        if (stdio && portGiven) {
            throw std::invalid_argument(
                "--stdio and --port are mutually exclusive");
        }

        // A grid is four bytes a voxel, so a cache below four times
        // --max-volume-voxels cannot hold a request that asks for the full
        // budget: those render uncached every time, while smaller requests
        // cache and evict normally. Setting the cache small is a legitimate
        // way to hold grid caching down, but it is also what raising
        // --max-volume-voxels does by accident, and nothing in a rendered
        // frame says which of the two happened. Warned about once here, where
        // the flags that caused it are still in view, rather than refused in
        // the server, which would take the deliberate case with it.
        //
        // Only when one of the two flags was actually given. The voxel cap now
        // defaults to the 512^3 ceiling, which is 512 MiB of floats and more
        // than the default grid cache holds, so the condition is true of every
        // unconfigured server -- and a warning printed on every start is one
        // nobody reads. What it is for is a configuration the operator chose;
        // nothing the GUI asks for reaches 512^3 anyway, its High preset being
        // 384^3, which the default cache does hold.
        if (volumeLimitsChosen
            && options.maximumVolumeVoxels * 4ULL > options.volumeGridCacheBytes) {
            std::cerr << "warning: --volume-cache-mib cannot hold one grid of "
                         "--max-volume-voxels voxels, so a request asking for "
                         "that many will render uncached every time; smaller "
                         "requests still cache normally\n";
        }

        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);
        if (stdio) {
            return serveStdio(std::move(options));
        }
        amrvis::remote::Server server(options);
        // The token gates every connection; clients must present it in their
        // handshake. It is printed here (and only here) so it travels over the
        // same channel the operator already trusts — their terminal or SSH
        // session — never onto the wire in the clear beyond the loopback bind.
        std::cout << "LISTENING 127.0.0.1 " << server.port() << " TOKEN "
                  << server.token() << '\n'
                  << std::flush;
        std::cerr << "amrexplorer-server ready on 127.0.0.1:" << server.port()
                  << "\nsession token: " << server.token()
                  << "\nconnect the client to 127.0.0.1:" << server.port()
                  << " and supply this token\n";
        SignalWatcher signalWatcher(server);
        server.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "server error: " << error.what() << '\n';
        printUsage(std::cerr);
        return 1;
    }
}
