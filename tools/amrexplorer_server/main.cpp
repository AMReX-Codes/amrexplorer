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

namespace {

volatile std::sig_atomic_t stopRequested = 0;

void handleSignal(int)
{
    stopRequested = 1;
}

template <typename Value>
Value parseUnsigned(const char* text, const char* option)
{
    const auto parsed = std::stoull(text);
    if (parsed > static_cast<unsigned long long>(
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
        options.softwareVersion = "0.1.0";
        for (int index = 1; index < argc; ++index) {
            const std::string option(argv[index]);
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
                options.maximumDatasets
                    = parseUnsigned<std::uint32_t>(
                        value, "--max-datasets");
            } else {
                throw std::invalid_argument("unknown option: " + option);
            }
        }

        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);
        amrvis::remote::Server server(options);
        std::cout << "LISTENING 127.0.0.1 " << server.port() << '\n'
                  << std::flush;
        std::jthread signalWatcher([&](std::stop_token stop) {
            while (!stop.stop_requested() && stopRequested == 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(50));
            }
            if (stopRequested != 0) {
                server.requestStop();
            }
        });
        server.run();
        signalWatcher.request_stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "server error: " << error.what() << '\n';
        return 1;
    }
}
