#include <amrexplorer/remote/Frame.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

template <typename Function>
void requireRejected(Function&& function, const char* message)
{
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    require(false, message);
}

} // namespace

int main()
{
    using namespace amrvis::remote;

    const auto listener = listenOnLoopback(0);
    std::exception_ptr serverFailure;
    std::thread server([&] {
        try {
            auto peer = acceptConnection(listener.socket);
            const auto request = readFrame(peer, 64);
            require(request == std::optional{
                    std::vector<std::uint8_t>{1, 2, 3, 4}},
                "server did not receive the framed request");
            writeFrame(peer, std::vector<std::uint8_t>{9, 8, 7}, 64);
        } catch (...) {
            serverFailure = std::current_exception();
        }
    });

    auto client = connectTo("127.0.0.1", listener.port);
    writeFrame(client, std::vector<std::uint8_t>{1, 2, 3, 4}, 64);
    const auto response = readFrame(client, 64);
    require(response == std::optional{
            std::vector<std::uint8_t>{9, 8, 7}},
        "client did not receive the framed response");
    server.join();
    if (serverFailure) {
        std::rethrow_exception(serverFailure);
    }

    requireRejected(
        [&] { writeFrame(client, {}, 64); },
        "an empty frame was accepted");
    requireRejected(
        [&] { writeFrame(client, std::vector<std::uint8_t>(65), 64); },
        "an oversized frame was accepted");

    const auto eofListener = listenOnLoopback(0);
    std::thread closer([&] {
        auto peer = acceptConnection(eofListener.socket);
        peer.close();
    });
    auto eofClient = connectTo("127.0.0.1", eofListener.port);
    require(!readFrame(eofClient, 64).has_value(),
        "clean EOF was not distinguished from a partial frame");
    closer.join();
    return 0;
}
