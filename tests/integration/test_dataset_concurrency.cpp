// Concurrency coverage for PlotfileDataset::requestBlock after the global IO
// mutex was replaced by a per-dataset timed mutex (see
// global-io-mutex-serializes-reads). Many threads hammer the same dataset's
// blocks concurrently: the double-checked cache lookup must stay correct
// under contention (every request returns the right values, no data race, no
// deadlock), and two datasets must not serialize against each other.

#include <amrexplorer/io/PlotfileDataset.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view realDescriptor =
    "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))";

std::atomic<int> g_failures{0};

void check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    output << text;
}

void writeFab(const std::filesystem::path& path, std::string_view box,
    std::span<const double> values)
{
    std::ofstream output(path, std::ios::binary);
    output << "FAB " << realDescriptor << box << " 1\n";
    output.write(reinterpret_cast<const char*>(values.data()),
        static_cast<std::streamsize>(values.size() * sizeof(double)));
}

// Single-level 2-D plotfile, domain (0,0)-(3,3), two grids fully covering it:
// grid 0 = (0,0)-(1,3), grid 1 = (2,0)-(3,3); phi = 10*j + i everywhere.
void writeTwoGridPlotfile(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nphi\n"
        "2\n0.0\n0\n"
        "0.0 0.0\n4.0 4.0\n\n"
        "((0,0) (3,3) (0,0))\n"
        "0\n1.0 1.0\n0\n0\n"
        "0 2 0.0\n0\n"
        "0.0 2.0\n0.0 4.0\n"
        "2.0 4.0\n0.0 4.0\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(2 0\n"
        "((0,0) (1,3) (0,0))\n"
        "((2,0) (3,3) (0,0))\n"
        ")\n"
        "2\n"
        "FabOnDisk: Cell_D_00000 0\n"
        "FabOnDisk: Cell_D_00001 0\n"
        "\n"
        "2,1\n0.0,\n2.0,\n\n2,1\n31.0,\n33.0,\n");
    std::vector<double> gridA;   // (0,0)-(1,3): i in 0..1, j in 0..3
    for (int j = 0; j <= 3; ++j) {
        for (int i = 0; i <= 1; ++i) {
            gridA.push_back(10.0 * j + i);
        }
    }
    std::vector<double> gridB;   // (2,0)-(3,3): i in 2..3, j in 0..3
    for (int j = 0; j <= 3; ++j) {
        for (int i = 2; i <= 3; ++i) {
            gridB.push_back(10.0 * j + i);
        }
    }
    writeFab(root / "Level_0" / "Cell_D_00000", "((0,0) (1,3) (0,0))", gridA);
    writeFab(root / "Level_0" / "Cell_D_00001", "((2,0) (3,3) (0,0))", gridB);
}

amrvis::BlockRequest blockRequest(std::uint64_t datasetId, int grid)
{
    amrvis::BlockRequest request;
    request.dataset.value = datasetId;
    request.level = 0;
    request.gridIndex = grid;
    request.field.value = 0;
    request.firstComponent = 0;
    request.componentCount = 1;
    return request;
}

// grid 0 spans values [0..31], grid 1 [2..33]; the first-axis-fastest layout
// puts phi(lower) at index 0 and phi(upper) at the last index.
void verifyGrid(amrvis::PlotfileDataset& dataset, std::uint64_t id, int grid)
{
    const auto access = dataset.requestBlock(blockRequest(id, grid));
    const auto& values = access.handle->values;
    check(values.size() == 8, "block value count mismatch");
    if (values.size() == 8) {
        const double expectedFirst = grid == 0 ? 0.0 : 2.0;
        const double expectedLast = grid == 0 ? 31.0 : 33.0;
        check(values[0] == expectedFirst, "block first value mismatch");
        check(values[7] == expectedLast, "block last value mismatch");
    }
}

} // namespace

int main()
{
    const auto unique
        = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path()
        / ("amrexplorer-dataset-concurrency-" + std::to_string(unique));
    const auto rootA = base / "a";
    const auto rootB = base / "b";
    writeTwoGridPlotfile(rootA);
    writeTwoGridPlotfile(rootB);

    constexpr std::uint64_t idA = 1;
    constexpr std::uint64_t idB = 2;
    amrvis::PlotfileDataset datasetA(rootA, amrvis::DatasetId{idA}, 1ULL << 20);
    amrvis::PlotfileDataset datasetB(rootB, amrvis::DatasetId{idB}, 1ULL << 20);

    constexpr int threadCount = 8;
    constexpr int iterations = 400;

    // Contended reads of one dataset: threads race on cold blocks (each block
    // must be read correctly regardless of who wins the double-checked lookup)
    // and then on warm ones.
    {
        std::vector<std::thread> workers;
        for (int worker = 0; worker < threadCount; ++worker) {
            workers.emplace_back([&datasetA, worker] {
                for (int iteration = 0; iteration < iterations; ++iteration) {
                    verifyGrid(datasetA, idA, (worker + iteration) % 2);
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        check(datasetA.cacheMetrics().residentBytes > 0,
            "contended dataset cached nothing");
    }

    // Two datasets read concurrently: the per-dataset mutexes must not
    // serialize or deadlock against each other, and each returns its own data.
    {
        std::vector<std::thread> workers;
        for (int worker = 0; worker < threadCount; ++worker) {
            workers.emplace_back([&datasetA, &datasetB, worker] {
                auto& dataset = (worker % 2 == 0) ? datasetA : datasetB;
                const auto id = (worker % 2 == 0) ? idA : idB;
                for (int iteration = 0; iteration < iterations; ++iteration) {
                    verifyGrid(dataset, id, iteration % 2);
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(base, cleanupError);
    if (g_failures != 0) {
        std::cerr << g_failures << " concurrency test failure(s)\n";
        return 1;
    }
    return 0;
}
