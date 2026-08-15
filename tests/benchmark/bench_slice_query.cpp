// Slice-path benchmark: times a whole SliceQuery::execute over a tiled level,
// to put numbers on the block-index work (the O(pixels x blocks) linear scan
// the uniform grid replaced) and to guard that path from rotting. It is
// registered as a ctest with a generous timeout and a correctness check, NOT a
// wall-clock threshold (those flake in CI). Read the printed timings by hand;
// scale the workload up via argv for real numbers:
//
// The reported Mpx/s is whole-execute throughput, not isolated per-pixel cost:
// each execute also does O(blocks) planning (a BlockGrid rebuild and a
// requestBlock cache lookup per candidate block). Vary outputDim to move pixel
// count at fixed block count; expect Mpx/s to fall as block count rises even
// though the per-pixel lookup is O(1) -- that is the planning, not a regression.
//
//   bench_slice_query [blocksPerAxis] [cellsPerBlock] [outputDim] [iterations]
//
// The default workload is small so it stays fast in CI.
#include <amrexplorer/query/SliceQuery.hpp>

#include <amrexplorer/io/PlotfileDataset.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr std::string_view realDescriptor =
    "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))";

// Set once the fixture directory exists, so die() (which exits, skipping
// destructors) still removes it instead of leaking the tree on any failure.
std::filesystem::path g_fixtureRoot;

[[noreturn]] void die(const char* message)
{
    std::fprintf(stderr, "bench_slice_query: %s\n", message);
    if (!g_fixtureRoot.empty()) {
        std::error_code ignore;
        std::filesystem::remove_all(g_fixtureRoot, ignore);
    }
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

// Strictly-positive, bounded integer argument. std::atoi returns 0 on garbage
// (silently truncating to the "positive" guard); parse explicitly and bound so
// the derived fixture sizes below cannot overflow int.
int positiveArg(const char* text)
{
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 1 || value > 65536) {
        die("arguments must be positive integers <= 65536");
    }
    return static_cast<int>(value);
}

} // namespace

int main(int argc, char** argv)
{
    // A single level tiled into blocksPerAxis^2 blocks of cellsPerBlock^2 cells,
    // unit cell size, field phi(i, j) = i + j.
    const int blocksPerAxis = argc > 1 ? positiveArg(argv[1]) : 16;
    const int cellsPerBlock = argc > 2 ? positiveArg(argv[2]) : 8;
    // Guard the derived sizes (long long) before narrowing to int, so a large
    // pair can't overflow the multiply.
    const long long domainLL
        = static_cast<long long>(blocksPerAxis) * cellsPerBlock;
    const long long blockCountLL
        = static_cast<long long>(blocksPerAxis) * blocksPerAxis;
    if (domainLL > 65536 || blockCountLL > 1000000) {
        die("fixture too large: reduce blocksPerAxis / cellsPerBlock");
    }
    const int domain = static_cast<int>(domainLL);
    const int outputDim = argc > 3 ? positiveArg(argv[3]) : domain;
    const int iterations = argc > 4 ? positiveArg(argv[4]) : 5;
    const int blockCount = static_cast<int>(blockCountLL);

    const auto unique
        = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("amrexplorer-bench-slice-" + std::to_string(unique));
    std::filesystem::create_directories(root / "Level_0");
    g_fixtureRoot = root;

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
        // Output pixel (x, y) samples physical position ((x+0.5)*scale,
        // (y+0.5)*scale), not cell (x, y) -- they coincide only at
        // outputDim == domain. Derive the expected value from that position so
        // the check holds at every outputDim, and require *complete* coverage:
        // this is the full domain, so a block-lookup regression that dropped
        // blocks (fewer valid pixels) must fail here, not read as "faster".
        const double scale = static_cast<double>(domain)
            / static_cast<double>(outputDim);
        std::size_t covered = 0;
        for (int y = 0; y < outputDim; ++y) {
            const double py = (static_cast<double>(y) + 0.5) * scale;
            for (int x = 0; x < outputDim; ++x) {
                const double px = (static_cast<double>(x) + 0.5) * scale;
                const auto off = static_cast<std::size_t>(x + outputDim * y);
                if (warm.plane.valid[off] == 0) {
                    continue;
                }
                ++covered;
                const auto value = static_cast<double>(warm.plane.values[off]);
                if (sampling == amrvis::SamplingPolicy::Nearest
                    || sampling == amrvis::SamplingPolicy::PiecewiseConstant) {
                    // Piecewise returns the containing cell's phi = i + j.
                    const int i = std::clamp(
                        static_cast<int>(px), 0, domain - 1);
                    const int j = std::clamp(
                        static_cast<int>(py), 0, domain - 1);
                    if (value != static_cast<double>(i + j)) {
                        die("benchmark piecewise slice value is wrong");
                    }
                } else if (px >= 0.5 && px <= domain - 0.5
                    && py >= 0.5 && py <= domain - 0.5) {
                    // Linear over the linear field phi = i + j is exact away
                    // from the clamped border: phi(px, py) = px + py - 1 (cell
                    // centers sit at half-integers). Border pixels clamp, so
                    // only require them to be covered.
                    if (std::fabs(value - (px + py - 1.0)) > 1e-2) {
                        die("benchmark linear slice value is wrong");
                    }
                }
            }
        }
        if (covered != static_cast<std::size_t>(outputDim)
                * static_cast<std::size_t>(outputDim)) {
            die("benchmark slice left output pixels uncovered");
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
