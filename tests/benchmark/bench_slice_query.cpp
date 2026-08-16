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
//                     [clusterGap] [requireLinear]
//
// clusterGap = 0 (the default) tiles the whole domain uniformly. A positive
// clusterGap instead places two blocksPerAxis^2 clusters at opposite corners
// of a domain that many blocks wider and taller -- the ordinary AMR shape of
// separate refined regions -- so the block index is measured on a level whose
// blocks occupy a small fraction of their bounding box, not only on a dense
// tiling. requireLinear (0/1) says whether a linear run that validated no
// interpolated value is a failure: on by default for the uniform tiling,
// where a domain of at least two cells guarantees such a pixel exists, and
// off by default for clustered layouts, where whether any pixel's whole
// bracket falls inside a cluster is a property of the parameters -- a
// registered clustered run passes 1 explicitly so it cannot silently become
// vacuous. The default workload is small so it stays fast in CI.
#include <amrexplorer/query/SliceQuery.hpp>

#include <amrexplorer/io/PlotfileDataset.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
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

// Bounded integer argument, at least `minimum`. std::atoi returns 0 on
// garbage (silently truncating to the guard); parse explicitly and bound so
// the derived fixture sizes below cannot overflow int.
int boundedArg(const char* text, long minimum)
{
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < minimum || value > 65536) {
        die("arguments must be integers <= 65536 (clusterGap may be 0, the "
            "rest at least 1)");
    }
    return static_cast<int>(value);
}

int positiveArg(const char* text)
{
    return boundedArg(text, 1);
}

} // namespace

int runBenchmark(int argc, char** argv)
{
    // A single level of cellsPerBlock^2-cell blocks, unit cell size, field
    // phi(i, j) = i + 3j (asymmetric so a transposed result is not
    // bit-identical to the correct one). Uniform: blocksPerAxis^2 blocks tile
    // the domain. Clustered (clusterGap > 0): two such clusters at opposite
    // corners of a domain clusterGap blocks wider, the rest empty.
    const int blocksPerAxis = argc > 1 ? positiveArg(argv[1]) : 16;
    const int cellsPerBlock = argc > 2 ? positiveArg(argv[2]) : 8;
    const int clusterGap = argc > 5 ? boundedArg(argv[5], 0) : 0;
    const bool requireLinear
        = argc > 6 ? boundedArg(argv[6], 0) != 0 : clusterGap == 0;
    const int blocksAcross
        = clusterGap > 0 ? 2 * blocksPerAxis + clusterGap : blocksPerAxis;
    // Guard the derived sizes (long long) before narrowing to int, so a large
    // pair can't overflow the multiply.
    const long long domainLL
        = static_cast<long long>(blocksAcross) * cellsPerBlock;
    const long long blockCountLL = (clusterGap > 0 ? 2LL : 1LL)
        * static_cast<long long>(blocksPerAxis) * blocksPerAxis;
    if (domainLL > 65536 || blockCountLL > 1000000) {
        die("fixture too large: reduce blocksPerAxis / cellsPerBlock / gap");
    }
    const int domain = static_cast<int>(domainLL);
    const int outputDim = argc > 3 ? positiveArg(argv[3]) : domain;
    // Bound the pixel count so the plane index below stays well inside int and
    // the fixture doesn't try to allocate an absurd raster.
    if (static_cast<long long>(outputDim) * outputDim > 100000000) {
        die("outputDim too large: pixel count capped at 1e8");
    }
    const int iterations = argc > 4 ? positiveArg(argv[4]) : 5;
    const int blockCount = static_cast<int>(blockCountLL);
    // Whether block coordinate (gi, gj) holds a block, and whether cell (i, j)
    // lies in one; the value checks below key on this rather than assuming
    // full coverage.
    const auto blockAt = [&](int gi, int gj) {
        if (clusterGap == 0) {
            return true;
        }
        const int second = blocksPerAxis + clusterGap;
        return (gi < blocksPerAxis && gj < blocksPerAxis)
            || (gi >= second && gj >= second);
    };
    const auto cellCovered = [&](int i, int j) {
        return blockAt(i / cellsPerBlock, j / cellsPerBlock);
    };

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
    for (int gj = 0; gj < blocksAcross; ++gj) {
        for (int gi = 0; gi < blocksAcross; ++gi) {
            if (!blockAt(gi, gj)) {
                continue;
            }
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
                    // Asymmetric in i and j so a transposed plane (swapped
                    // axes / weights) is not bit-identical to the correct one.
                    const double v = static_cast<double>(i + 3 * j);
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
        const std::size_t pixelCount = static_cast<std::size_t>(outputDim)
            * static_cast<std::size_t>(outputDim);
        // Guard the buffers actually indexed below (sized independently of
        // width/height), so a shape/size regression fails cleanly instead of
        // reading out of bounds.
        if (warm.plane.width != outputDim || warm.plane.height != outputDim
            || warm.plane.valid.size() != pixelCount
            || warm.plane.values.size() != pixelCount) {
            die("benchmark warm slice has the wrong shape");
        }
        // Output pixel (x, y) samples a physical position, not cell (x, y) --
        // they coincide only at outputDim == domain. Derive the expected value
        // from that position and require *complete* coverage: this is the full
        // domain, so a block-lookup regression that dropped blocks must fail
        // here, not read as "faster".
        const double lower = 0.0;                            // visibleRegion.lower
        const double extent = static_cast<double>(domain);   // upper - lower
        // The cell a position samples, boundary-tolerant. On a cell edge a
        // 1-ULP difference between our position and SliceQuery's flips floor()
        // and shifts the piecewise value by a whole cell, so accept either
        // bracketing cell within a few ULP of an integer edge. This is a
        // textual copy of SliceQuery's position formula; the tolerance is what
        // keeps a rounding-neutral refactor of it from reinstating false
        // failures (this line has tripped over that three times).
        const auto sampleCells = [domain](double p) -> std::array<int, 2> {
            const double nearest = std::round(p);
            const double tol = 16.0 * std::numeric_limits<double>::epsilon()
                * std::max(1.0, std::fabs(p));
            if (std::fabs(p - nearest) <= tol) {
                return {std::clamp(static_cast<int>(nearest) - 1, 0, domain - 1),
                    std::clamp(static_cast<int>(nearest), 0, domain - 1)};
            }
            const int cell
                = std::clamp(static_cast<int>(std::floor(p)), 0, domain - 1);
            return {cell, cell};
        };
        std::size_t covered = 0;
        std::size_t linearChecked = 0;
        for (int y = 0; y < outputDim; ++y) {
            const double py = lower + (static_cast<double>(y) + 0.5) * extent
                / static_cast<double>(outputDim);
            for (int x = 0; x < outputDim; ++x) {
                const double px = lower + (static_cast<double>(x) + 0.5) * extent
                    / static_cast<double>(outputDim);
                const auto off = static_cast<std::size_t>(x)
                    + static_cast<std::size_t>(outputDim)
                        * static_cast<std::size_t>(y);
                // Coverage must follow the layout exactly: a pixel whose every
                // candidate cell lies in a block is covered, one whose none
                // does is not (a block-lookup regression that dropped blocks
                // must fail here, not read as "faster"); on a block edge
                // either bracketing cell decides.
                int inBlock = 0;
                int candidates = 0;
                for (const int i : sampleCells(px)) {
                    for (const int j : sampleCells(py)) {
                        ++candidates;
                        inBlock += cellCovered(i, j) ? 1 : 0;
                    }
                }
                const bool isCovered = warm.plane.valid[off] != 0;
                if (inBlock == candidates && !isCovered) {
                    die("benchmark slice left a block pixel uncovered");
                }
                if (inBlock == 0 && isCovered) {
                    die("benchmark slice covered a pixel outside every block");
                }
                if (!isCovered) {
                    continue;
                }
                ++covered;
                const auto value = static_cast<double>(warm.plane.values[off]);
                if (sampling == amrvis::SamplingPolicy::Nearest
                    || sampling == amrvis::SamplingPolicy::PiecewiseConstant) {
                    // Piecewise returns the containing cell's phi = i + 3j.
                    bool matched = false;
                    for (const int i : sampleCells(px)) {
                        for (const int j : sampleCells(py)) {
                            if (cellCovered(i, j)
                                && value == static_cast<double>(i + 3 * j)) {
                                matched = true;
                            }
                        }
                    }
                    if (!matched) {
                        die("benchmark piecewise slice value is wrong");
                    }
                } else {
                    // Linear over the linear field phi = i + 3j is exact where
                    // all four bracketing cell centers exist (cell centers sit
                    // at half-integers): phi(px, py) = px + 3py - 2. Pixels
                    // whose bracket reaches the domain border or an empty
                    // region clamp, so only require those to be covered.
                    const int i0 = static_cast<int>(std::floor(px - 0.5));
                    const int j0 = static_cast<int>(std::floor(py - 0.5));
                    if (i0 >= 0 && j0 >= 0 && i0 + 1 <= domain - 1
                        && j0 + 1 <= domain - 1 && cellCovered(i0, j0)
                        && cellCovered(i0 + 1, j0) && cellCovered(i0, j0 + 1)
                        && cellCovered(i0 + 1, j0 + 1)) {
                        ++linearChecked;
                        if (std::fabs(value - (px + 3.0 * py - 2.0)) > 1e-2) {
                            die("benchmark linear slice value is wrong");
                        }
                    }
                }
            }
        }
        // The per-pixel checks above are exact for any layout; these two
        // refuse parameters under which they checked nothing. `covered` is
        // exactly the pixels whose value was compared, so the first is exact
        // for any layout. The second, for linear sampling, holds when the
        // layout guarantees a pixel with its whole bracket inside a block
        // (uniform, domain >= 2) or when the caller vouched for the
        // parameters with requireLinear; for other clustered runs the printed
        // count is the record.
        if (covered == 0) {
            die("no pixel centre lies inside a block: these parameters "
                "validate nothing");
        }
        if (sampling == amrvis::SamplingPolicy::Linear && requireLinear
            && domain >= 2 && linearChecked == 0) {
            die("no pixel has its whole linear bracket inside a block: these "
                "parameters validate no interpolated value");
        }
        std::vector<double> samples;
        samples.reserve(static_cast<std::size_t>(iterations));
        for (int it = 0; it < iterations; ++it) {
            const auto start = std::chrono::steady_clock::now();
            const auto result = query.execute(request);
            const auto end = std::chrono::steady_clock::now();
            if (result.plane.width != outputDim
                || result.plane.height != outputDim) {
                die("benchmark slice returned the wrong size");
            }
            // The measurement claims cache-warm: no block may be read from
            // disk here, or the reported Mpx/s is really an I/O number (e.g. a
            // fixture scaled past the cache budget).
            if (result.metrics.blocksRead != 0) {
                die("benchmark slice was not cache-warm (blocks read while timing)");
            }
            samples.push_back(
                std::chrono::duration<double, std::milli>(end - start).count());
        }
        const auto ms = medianMillis(samples);
        std::printf("  %-10s %5d blocks  %d x %d px  median %8.3f ms  "
            "(%6.1f Mpx/s)",
            label, blockCount, outputDim, outputDim, ms,
            static_cast<double>(outputDim) * outputDim / (ms * 1000.0));
        if (clusterGap > 0 && sampling == amrvis::SamplingPolicy::Linear) {
            std::printf("  [%zu px covered, %zu linear-validated]", covered,
                linearChecked);
        } else if (clusterGap > 0) {
            std::printf("  [%zu px covered]", covered);
        }
        std::printf("\n");
    };

    std::printf("slice-query benchmark (cache-warm, median of %d%s):\n",
        iterations, clusterGap > 0 ? ", two clusters" : "");
    run(amrvis::SamplingPolicy::PiecewiseConstant, "piecewise");
    run(amrvis::SamplingPolicy::Linear, "linear");

    // Non-throwing: a cleanup hiccup must not turn a fully-passing benchmark
    // into a red ctest (e.g. on Windows).
    std::error_code ignore;
    std::filesystem::remove_all(root, ignore);
    return 0;
}

int main(int argc, char** argv)
{
    // Every SliceQuery/IO error path is an exception; catch here so a failure
    // (the regression class the guard exists to catch) exits cleanly and still
    // removes the fixture tree instead of terminating and leaking it.
    try {
        return runBenchmark(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "bench_slice_query: %s\n", error.what());
        if (!g_fixtureRoot.empty()) {
            std::error_code ignore;
            std::filesystem::remove_all(g_fixtureRoot, ignore);
        }
        return 1;
    }
}
