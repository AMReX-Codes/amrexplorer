// Standalone reproduction for the single-level line-plot hang. Mimics the
// GUI's line request (region = full physical domain, FinestAvailable, both
// axes) and runs LineQuery under a watchdog so a hang is caught instead of
// looping forever. Prints the geometry first (incl. |prob_lo|/cellSize, the
// floating-point-cancellation danger ratio) so a stuck run still reports data.
#include <amrexplorer/query/LineQuery.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

// Run fn() on a worker; if it does not finish within timeoutSeconds, report a
// hang and return false (the worker is abandoned; process exit reclaims it).
// Shared state is heap-allocated and captured by value (shared_ptr) so a
// detached worker never touches stack locals that have gone out of scope.
template <typename Fn>
bool runWithTimeout(Fn fn, int timeoutSeconds)
{
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };
    const auto state = std::make_shared<State>();

    std::thread worker([state, fn = std::move(fn)] {
        try {
            fn();
        } catch (const std::exception& error) {
            std::cerr << "  worker threw: " << error.what() << '\n';
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->done = true;
        }
        state->cv.notify_one();
    });

    std::unique_lock<std::mutex> lock(state->mutex);
    const bool ok = state->cv.wait_for(lock, std::chrono::seconds(timeoutSeconds),
        [&] { return state->done; });
    if (ok) {
        lock.unlock();
        worker.join();
        return true;
    }
    worker.detach();
    return false;
}

void printGeometry(const amrvis::DatasetMetadata& md)
{
    std::cout << "dimension: " << md.dimension << '\n'
              << "finestLevel: " << md.finestLevel << '\n'
              << "fields: " << md.fields.size() << '\n';
    std::cout << "physicalDomain.lower: " << md.physicalDomain.lower[0] << ' '
              << md.physicalDomain.lower[1] << ' ' << md.physicalDomain.lower[2] << '\n';
    std::cout << "physicalDomain.upper: " << md.physicalDomain.upper[0] << ' '
              << md.physicalDomain.upper[1] << ' ' << md.physicalDomain.upper[2] << '\n';
    for (std::size_t level = 0; level < md.levels.size(); ++level) {
        const auto& l = md.levels[level];
        std::cout << "level " << level
                  << " cellSize: " << l.cellSize[0] << ' ' << l.cellSize[1] << ' ' << l.cellSize[2]
                  << " domain: [(" << l.domain.lower[0] << ',' << l.domain.lower[1] << ','
                  << l.domain.lower[2] << "),(" << l.domain.upper[0] << ',' << l.domain.upper[1]
                  << ',' << l.domain.upper[2] << ")] blocks: " << l.blocks.size() << '\n';
        for (int axis = 0; axis < md.dimension; ++axis) {
            const auto origin = std::fabs(md.physicalDomain.lower[static_cast<std::size_t>(axis)]);
            const auto ratio = l.cellSize[static_cast<std::size_t>(axis)] > 0.0
                ? origin / l.cellSize[static_cast<std::size_t>(axis)]
                : std::numeric_limits<double>::infinity();
            std::cout << "  |prob_lo|/cellSize[axis " << axis << "] = " << ratio;
            if (ratio > 1.0e15) {
                std::cout << "  <-- DANGER: above the ~9e15 cancellation threshold";
            }
            std::cout << '\n';
        }
    }
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "usage: line_repro PLOTFILE [timeout_seconds=5]\n";
        return 2;
    }
    const std::filesystem::path plotfile(argv[1]);
    const int timeoutSeconds = argc >= 3 ? std::stoi(argv[2]) : 5;

    try {
        amrvis::PlotfileDataset dataset(plotfile, amrvis::DatasetId{1}, 1024ULL * 1024 * 1024);
        const auto& md = dataset.metadata();
        printGeometry(md);

        const auto dim = md.dimension;
        const auto maxLevel = md.finestLevel;
        std::array<double, 3> center{};
        for (int axis = 0; axis < 3; ++axis) {
            center[static_cast<std::size_t>(axis)] =
                0.5 * (md.physicalDomain.lower[static_cast<std::size_t>(axis)]
                       + md.physicalDomain.upper[static_cast<std::size_t>(axis)]);
        }
        // region = full physical domain, exactly as makeLineRequest sets
        // request.region = planeRegion for a freshly opened (unzoomed) slice.
        amrvis::RealBox region{md.physicalDomain.lower, md.physicalDomain.upper};

        const std::size_t fieldCount = std::min<std::size_t>(md.fields.size(), 1);
        for (std::size_t field = 0; field < fieldCount; ++field) {
            for (int axis = 0; axis < dim; ++axis) {
                amrvis::LineRequest request;
                request.dataset = dataset.id();
                request.field.value = static_cast<std::uint32_t>(field);
                request.axis = axis;
                request.maximumLevel = maxLevel;
                request.composition = amrvis::CompositionPolicy::FinestAvailable;
                request.fixedCoordinates = center;
                request.region = region;

                std::cout << "\n=== field " << field << " axis " << axis
                          << " (FinestAvailable, maxLevel=" << maxLevel << ") ===" << std::endl;
                const bool ok = runWithTimeout(
                    [&] {
                        amrvis::LineQuery lines(dataset);
                        amrvis::StopSource alive;
                        const auto result = lines.execute(request, alive.get_token());
                        std::cout << "  samples: " << result.line.positions.size()
                                  << " valid: " << [&] {
                                         std::size_t v = 0;
                                         for (const auto x : result.line.valid) {
                                             v += static_cast<std::size_t>(x != 0);
                                         }
                                         return v;
                                     }()
                                  << " blocksRead: " << result.metrics.blocksRead << '\n';
                    },
                    timeoutSeconds);
                if (!ok) {
                    std::cout << "  >>> HANG: no completion in " << timeoutSeconds << "s <<<\n";
                    return 3;
                }
            }
        }
        std::cout << "\nAll line queries completed.\n";
    } catch (const std::exception& error) {
        std::cerr << "line_repro: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
