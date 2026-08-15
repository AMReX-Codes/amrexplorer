// Slice-path benchmark: times the composited per-pixel block lookup that
// SliceQuery runs for every output pixel (five times per pixel for linear
// sampling). It exists to put numbers on the block-index work (the O(pixels x
// blocks) linear scan the uniform grid replaced) and to guard that path from
// rotting -- it is registered as a ctest with a generous timeout and a
// correctness check, NOT a wall-clock threshold (those flake in CI). Read the
// printed timings by hand; scale the workload up via argv for real numbers:
//
//   bench_slice_query [blocksPerAxis] [cellsPerBlock] [outputDim] [iterations]
//
// The default workload is small so it stays fast in CI.
#include <amrexplorer/query/SliceQuery.hpp>

#include <amrexplorer/io/PlotfileDataset.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view realDescriptor =
    "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))";

void die(const char* message)
{
    std::fprintf(stderr, "bench_slice_query: %s\n", message);
    std::exit(1);
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        die("could not write fixture text");
    }
    output << text;
}

void writeFab(const std::filesystem::path& path, const std::string& box,
    std::span<const double> values)
{
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        die("could not write fixture FAB");
    }
    output << "FAB " << realDescriptor << box << " 1\n";
    output.write(reinterpret_cast<const char*>(values.data()),
        static_cast<std::streamsize>(values.size() * sizeof(double)));
}

double medianMillis(std::vector<double>& samples)
{
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

} // namespace

int main(int argc, char** argv)
{
    // A single level tiled into blocksPerAxis^2 blocks of cellsPerBlock^2 cells,
    // unit cell size, field phi(i, j) = i + j.
    const int blocksPerAxis = argc > 1 ? std::atoi(argv[1]) : 16;
    const int cellsPerBlock = argc > 2 ? std::atoi(argv[2]) : 8;
    const int domain = blocksPerAxis * cellsPerBlock;
    const int outputDim = argc > 3 ? std::atoi(argv[3]) : domain;
    const int iterations = argc > 4 ? std::atoi(argv[4]) : 5;
    if (blocksPerAxis < 1 || cellsPerBlock < 1 || outputDim < 1
        || iterations < 1) {
        die("arguments must be positive");
    }
    const int blockCount = blocksPerAxis * blocksPerAxis;

    const auto unique
        = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("amrexplorer-bench-slice-" + std::to_string(unique));
    std::filesystem::create_directories(root / "Level_0");

    const auto idx = [](int v) { return std::to_string(v); };
    std::string header =
        "HyperCLaw-V1.1\n1\nphi\n2\n0.0\n0\n0.0 0.0\n"
        + idx(domain) + " " + idx(domain) + "\n\n"
        + "((0,0) (" + idx(domain - 1) + "," + idx(domain - 1) + ") (0,0))\n"
        + "0\n1.0 1.0\n0\n0\n0 " + idx(blockCount) + " 0.0\n0\n";
    std::string boxes;
    std::string fabList;
    std::string minRows;
    std::string maxRows;
    int fabNumber = 0;
    for (int gj = 0; gj < blocksPerAxis; ++gj) {
        for (int gi = 0; gi < blocksPerAxis; ++gi) {
            const int i0 = gi * cellsPerBlock;
            const int j0 = gj * cellsPerBlock;
            const int i1 = i0 + cellsPerBlock - 1;
            const int j1 = j0 + cellsPerBlock - 1;
            header += idx(i0) + " " + idx(i1 + 1) + "\n"
                + idx(j0) + " " + idx(j1 + 1) + "\n";
            const auto box = "((" + idx(i0) + "," + idx(j0) + ") ("
                + idx(i1) + "," + idx(j1) + ") (0,0))";
            boxes += box + "\n";
            char name[32];
            std::snprintf(name, sizeof(name), "Cell_D_%05d", fabNumber++);
            fabList += std::string("FabOnDisk: ") + name + " 0\n";
            std::vector<double> payload;
            payload.reserve(static_cast<std::size_t>(cellsPerBlock)
                * static_cast<std::size_t>(cellsPerBlock));
            double lo = 1e300;
            double hi = -1e300;
            for (int j = j0; j <= j1; ++j) {
                for (int i = i0; i <= i1; ++i) {
                    const double v = static_cast<double>(i + j);
                    payload.push_back(v);
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
            }
            minRows += std::to_string(lo) + ",\n";
            maxRows += std::to_string(hi) + ",\n";
            writeFab(root / "Level_0" / name, box, payload);
        }
    }
    header += "Level_0/Cell\n";
    writeText(root / "Header", header);
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n(" + idx(blockCount) + " 0\n" + boxes + ")\n"
        + idx(blockCount) + "\n" + fabList + "\n"
        + idx(blockCount) + ",1\n" + minRows + "\n"
        + idx(blockCount) + ",1\n" + maxRows + "\n");

    amrvis::PlotfileDataset dataset(
        root, amrvis::DatasetId{1}, std::uint64_t{4} * 1024 * 1024 * 1024);
    amrvis::SliceQuery query(dataset);

    amrvis::SliceRequest request;
    request.dataset.value = 1;
    request.field.value = 0;
    request.normalDirection = 1;
    request.visibleRegion = {{{0.0, 0.0, 0.0}},
        {{static_cast<double>(domain), static_cast<double>(domain), 0.0}}};
    request.maximumLevel = 0;
    request.outputSize = {outputDim, outputDim};

    const auto run = [&](amrvis::SamplingPolicy sampling, const char* label) {
        request.sampling = sampling;
        // Warm the block cache so we time the composite lookup, not the FAB I/O.
        const auto warm = query.execute(request);
        std::size_t covered = 0;
        bool correct = true;
        for (int y = 0; y < outputDim; ++y) {
            for (int x = 0; x < outputDim; ++x) {
                const auto off = static_cast<std::size_t>(x + outputDim * y);
                if (warm.plane.valid[off] == 0) {
                    continue;
                }
                ++covered;
                // Cell (i, j) = (x, y) at 1:1 output; phi = i + j. Piecewise is
                // exact; linear reproduces the linear field at interior pixels.
                if (sampling == amrvis::SamplingPolicy::Nearest
                    || sampling == amrvis::SamplingPolicy::PiecewiseConstant) {
                    const auto expected = static_cast<float>(x + y);
                    if (warm.plane.values[off] != expected) {
                        correct = false;
                    }
                }
            }
        }
        if (!correct) {
            die("benchmark slice produced an incorrect value");
        }
        if (covered == 0) {
            die("benchmark slice covered no pixels");
        }
        std::vector<double> samples;
        samples.reserve(static_cast<std::size_t>(iterations));
        for (int it = 0; it < iterations; ++it) {
            const auto start = std::chrono::steady_clock::now();
            const auto result = query.execute(request);
            const auto end = std::chrono::steady_clock::now();
            if (result.plane.width != outputDim) {
                die("benchmark slice returned the wrong size");
            }
            samples.push_back(
                std::chrono::duration<double, std::milli>(end - start).count());
        }
        const auto ms = medianMillis(samples);
        std::printf("  %-10s %5d blocks  %d x %d px  median %8.3f ms  "
            "(%6.1f Mpx/s)\n",
            label, blockCount, outputDim, outputDim, ms,
            static_cast<double>(outputDim) * outputDim / (ms * 1000.0));
    };

    std::printf("slice-query benchmark (cache-warm, median of %d):\n",
        iterations);
    run(amrvis::SamplingPolicy::PiecewiseConstant, "piecewise");
    run(amrvis::SamplingPolicy::Linear, "linear");

    std::filesystem::remove_all(root);
    return 0;
}
