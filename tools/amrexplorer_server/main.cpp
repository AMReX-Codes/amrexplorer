#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef AMREXPLORER_VERSION
#define AMREXPLORER_VERSION "0.2.0-dev"
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
        << "  --port PORT          loopback port; 0 selects an available port\n"
        << "  --threads COUNT      worker threads; 0 selects hardware concurrency\n"
        << "  --max-frame-mib MIB  maximum negotiated frame size\n"
        << "  --max-datasets COUNT maximum open datasets per connection\n"
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

} // namespace

int main(int argc, char* argv[])
{
    try {
        amrvis::remote::ServerOptions options;
        options.softwareVersion = AMREXPLORER_VERSION;
        for (int index = 1; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--help") {
                printUsage(std::cout);
                return 0;
            }
            if (index + 1 >= argc) {
                throw std::invalid_argument(
                    "missing value after " + option);
            }
            const auto* value = argv[++index];
            if (option == "--port") {
                options.port
                    = parseUnsigned<std::uint16_t>(value, "--port");
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

        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);
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
