#include <amrexplorer/core/StopToken.hpp>

#include <atomic>
#include <cstdlib>
#include <iostream>
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

} // namespace

int main()
{
    amrvis::StopToken empty;
    require(!empty.stop_possible(), "default token should not be stoppable");
    require(!empty.stop_requested(), "default token should not be stopped");

    amrvis::StopSource source;
    const auto token = source.get_token();
    require(source.stop_possible(), "source should be stoppable");
    require(token.stop_possible(), "source token should be stoppable");
    require(!token.stop_requested(), "new source token should not be stopped");
    require(source.request_stop(), "first stop request should succeed");
    require(source.stop_requested(), "source should report requested stop");
    require(token.stop_requested(), "token should observe requested stop");
    require(!source.request_stop(), "second stop request should report no change");

    // Cross-thread stop: the production shape is a GUI thread requesting stop
    // while workers poll their token copies. Run it multithreaded so the TSan
    // suite validates the fallback's atomic ordering (this target is compiled
    // with AMREXPLORER_TEST_FORCE_FALLBACK_STOP_TOKEN; a race here would be
    // invisible to a single-threaded test).
    amrvis::StopSource shared;
    std::atomic<int> observed{0};
    std::vector<std::thread> pollers;
    for (int poller = 0; poller < 4; ++poller) {
        pollers.emplace_back([token = shared.get_token(), &observed] {
            while (!token.stop_requested()) {
            }
            ++observed;
        });
    }
    std::thread stopper([&shared] { shared.request_stop(); });
    stopper.join();
    for (auto& poller : pollers) {
        poller.join();
    }
    require(observed.load() == 4,
        "every polling thread should observe the cross-thread stop");
}
