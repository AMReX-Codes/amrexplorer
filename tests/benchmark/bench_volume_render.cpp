// Volume ray-caster benchmark: times raycastVolume over a synthetic grid to
// put numbers on the per-frame cost the interactive volume view pays on
// every camera change (the AMR sample is cached; only the cast reruns). Like
// bench_slice_query it is registered as a ctest with a generous timeout and a
// correctness check, NOT a wall-clock threshold. Read the timings by hand and
// scale the workload via argv for real numbers:
//
//   bench_volume_render [gridDim] [outputDim] [samplesPerVoxel] [iterations]
//                       [threads] [linear]
//
// `linear` is 1 for trilinear sampling (the renderer's default) and 0 for
// nearest, so the eight-fetch and one-fetch costs can be compared directly.
// Measure at a grid that does not fit in cache -- 64^3 of float is 1 MiB and
// sits in L2, where the seven extra fetches look free -- and pin the thread
// count so two runs are comparable.
//
// The grid is a smooth radial field with an opaque core, so the rays do real
// compositing work and the early-out fires for the central pixels as it does
// on real data. The check: the centre pixel of an XY view is fully opaque
// with the core's colour, and the corners (outside the domain) are empty.
// The default workload is small so it stays fast in CI.
#include <amrexplorer/render3d/VolumeRaycaster.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

// std::strtol, not std::atoi: atoi's behaviour is undefined when the text
// does not fit in an int, which is exactly what an argv parser must survive.
int argument(int argc, char** argv, int index, int fallback)
{
    if (argc <= index) {
        return fallback;
    }
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtol(argv[index], &end, 10);
    require(end != argv[index] && *end == '\0' && errno != ERANGE
            && value >= std::numeric_limits<int>::min()
            && value <= std::numeric_limits<int>::max(),
        "an argument is not a representable integer");
    return static_cast<int>(value);
}

} // namespace

int main(int argc, char** argv)
{
    const int gridDim = argument(argc, argv, 1, 64);
    const int outputDim = argument(argc, argv, 2, 256);
    const int samplesPerVoxel = argument(argc, argv, 3, 2);
    const int iterations = argument(argc, argv, 4, 2);
    const int threads = argument(argc, argv, 5, 0);
    const int linear = argument(argc, argv, 6, 1);
    // Bounded as the renderer bounds them, so an out-of-range argument is
    // this file's own message rather than an uncaught std::invalid_argument
    // escaping main and aborting through std::terminate.
    require(gridDim > 0 && outputDim > 0 && samplesPerVoxel > 0 && iterations > 0,
        "arguments must be positive");
    require(outputDim <= amrvis::maxVolumeOutputDimension,
        "the output dimension exceeds the renderer's limit");
    // A floor as well as a cap: viewportFrame keeps a 12-pixel margin and
    // clamps the scale at 1, so below about 42 pixels the whole domain
    // projects onto a pixel or two and the centre-pixel check below fails on
    // a perfectly good render. 64 leaves clear air above that.
    require(outputDim >= 64,
        "the output dimension is too small for the check below to mean anything");
    require(samplesPerVoxel <= amrvis::maxVolumeSamplesPerVoxel,
        "samples per voxel exceeds the renderer's limit");
    require(linear == 0 || linear == 1, "linear must be 0 or 1");
    require(amrvis::volumeVoxelCount({gridDim, gridDim, gridDim})
            <= amrvis::maxVolumeVoxelBudget,
        "the grid exceeds the renderer's voxel budget");

    amrvis::VolumeGrid grid;
    grid.dims = {gridDim, gridDim, gridDim};
    grid.region.lower = {{0.0, 0.0, 0.0}};
    grid.region.upper = {{1.0, 1.0, 1.0}};
    grid.values.resize(static_cast<std::size_t>(gridDim) * static_cast<std::size_t>(gridDim)
        * static_cast<std::size_t>(gridDim));
    // Radial field: 1 at the centre falling to 0 at the corners.
    for (int k = 0; k < gridDim; ++k) {
        for (int j = 0; j < gridDim; ++j) {
            for (int i = 0; i < gridDim; ++i) {
                const auto x = (static_cast<double>(i) + 0.5) / gridDim - 0.5;
                const auto y = (static_cast<double>(j) + 0.5) / gridDim - 0.5;
                const auto z = (static_cast<double>(k) + 0.5) / gridDim - 0.5;
                const auto radius = std::sqrt(x * x + y * y + z * z);
                grid.values[static_cast<std::size_t>(i)
                    + static_cast<std::size_t>(gridDim)
                        * (static_cast<std::size_t>(j)
                            + static_cast<std::size_t>(gridDim) * static_cast<std::size_t>(k))]
                    = static_cast<float>(std::max(0.0, 1.0 - radius / 0.866));
            }
        }
    }
    grid.coveredVoxels = grid.values.size();

    amrvis::RaycastSettings settings;
    settings.camera = amrvis::orthoPresetXY;
    settings.domain = grid.region;
    settings.outputSize = {outputDim, outputDim};
    settings.range = {0.0, 1.0, false};
    settings.samplesPerVoxel = samplesPerVoxel;
    settings.threadCount = static_cast<unsigned>(std::max(0, threads));
    settings.sampling = linear == 1 ? amrvis::SamplingPolicy::Linear
                                    : amrvis::SamplingPolicy::Nearest;
    // A ramp that is transparent below 0.4 and opaque white above 0.9, so
    // the core stops rays and the halo composites.
    for (int entry = 0; entry < 253; ++entry) {
        const auto t = static_cast<double>(entry) / 252.0;
        const auto opacity = t < 0.4 ? 0.0 : t > 0.9 ? 1.0 : (t - 0.4) / 0.5 * 0.2;
        settings.transfer.colors.push_back(0xFFFFFFU);
        settings.transfer.opacities.push_back(static_cast<float>(opacity));
    }

    amrvis::VolumeFrame frame;
    const auto started = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        frame = amrvis::raycastVolume(grid, settings);
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();

    const auto centre = frame.pixels[static_cast<std::size_t>(outputDim / 2)
        * static_cast<std::size_t>(outputDim) + static_cast<std::size_t>(outputDim / 2)];
    require((centre >> 24U) == 255U && (centre & 0xFFU) == 255U,
        "the opaque core did not saturate the centre pixel");
    require(frame.pixels.front() == 0U && frame.pixels.back() == 0U,
        "the corners outside the domain were lit");

    const auto pixels = static_cast<double>(outputDim) * outputDim * iterations;
    const auto perFrame = elapsed / iterations;
    std::cout << "grid " << gridDim << "^3, output " << outputDim << "^2, "
              << samplesPerVoxel << " samples/voxel, "
              << amrvis::raycastThreadCount(
                     settings.threadCount, settings.outputSize[1])
              << " threads, " << (linear == 1 ? "linear" : "nearest")
              << ": " << perFrame * 1000.0 << " ms/frame, "
              << pixels / elapsed / 1.0e6 << " Mpx/s\n";
    return 0;
}
