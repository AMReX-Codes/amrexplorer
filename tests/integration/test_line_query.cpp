#include <amrexplorer/query/LineQuery.hpp>
#include <amrexplorer/query/SliceQuery.hpp>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view realDescriptor =
    "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))";

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

// Run fn() on a worker thread; return true if it finishes within
// timeoutSeconds, false if it appears to hang (the worker is then abandoned,
// since the caller is expected to fail the test and exit).
template <typename Fn>
bool runWithTimeout(Fn fn, int timeoutSeconds)
{
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::thread worker([&] {
        try {
            fn();
        } catch (const std::exception& error) {
            std::cerr << "worker threw: " << error.what() << '\n';
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        cv.notify_one();
    });
    std::unique_lock<std::mutex> lock(mutex);
    const bool finished = cv.wait_for(lock, std::chrono::seconds(timeoutSeconds),
        [&] { return done; });
    if (finished) {
        lock.unlock();
        worker.join();
        return true;
    }
    worker.detach();
    return false;
}

// The tests/data fixtures carry metadata only; this adds FAB payloads at the
// exact FabOnDisk offsets their Cell_H indices record, with analytic values
// consistent with the per-grid min/max statistics in those indices.
void writeFab(const std::filesystem::path& path, std::uint64_t offset,
    std::string_view box, int components, const std::vector<double>& values)
{
    if (offset == 0) {
        std::ofstream create(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(create), "could not create line fixture FAB");
    }
    std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
    require(static_cast<bool>(output), "could not open line fixture FAB");
    output.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    output << "FAB " << realDescriptor << box << " " << components << "\n";
    output.write(reinterpret_cast<const char*>(values.data()),
        static_cast<std::streamsize>(values.size() * sizeof(double)));
    require(static_cast<bool>(output), "could not write line fixture FAB payload");
}

// 2-D fixture fields: density(i, j) = (i + j) / 2, temperature = 100 + density,
// where i and j are cell indices at the level storing the grid.
std::vector<double> field2d(int i0, int i1, int j0, int j1, int component)
{
    std::vector<double> values;
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            const auto density = 0.5 * static_cast<double>(i + j);
            values.push_back(component == 0 ? density : 100.0 + density);
        }
    }
    return values;
}

std::vector<double> bothComponents2d(int i0, int i1, int j0, int j1)
{
    auto values = field2d(i0, i1, j0, j1, 0);
    const auto temperature = field2d(i0, i1, j0, j1, 1);
    values.insert(values.end(), temperature.begin(), temperature.end());
    return values;
}

// 3-D fixture field: q(i, j, k) = (i + j + k) / 9.
std::vector<double> field3d()
{
    std::vector<double> values;
    for (int k = 0; k <= 3; ++k) {
        for (int j = 0; j <= 3; ++j) {
            for (int i = 0; i <= 3; ++i) {
                values.push_back(static_cast<double>(i + j + k) / 9.0);
            }
        }
    }
    return values;
}

std::filesystem::path materializeFixture(
    const std::filesystem::path& source, const std::filesystem::path& root)
{
    std::filesystem::copy(source, root, std::filesystem::copy_options::recursive);
    return root;
}

void test2d(const std::filesystem::path& source, const std::filesystem::path& work)
{
    const auto root = materializeFixture(source, work / "plotfile_2d");
    writeFab(root / "Level_0" / "Cell_D_00000", 0,
        "((0,0) (1,3) (0,0))", 2, bothComponents2d(0, 1, 0, 3));
    writeFab(root / "Level_0" / "Cell_D_00000", 4096,
        "((2,0) (3,3) (0,0))", 2, bothComponents2d(2, 3, 0, 3));
    writeFab(root / "Level_1" / "Cell_D_00000", 0,
        "((2,2) (5,5) (0,0))", 2, bothComponents2d(2, 5, 2, 5));
    const auto payloadBytes = std::filesystem::file_size(root / "Level_0" / "Cell_D_00000")
        + std::filesystem::file_size(root / "Level_1" / "Cell_D_00000");

    amrvis::PlotfileDataset dataset(root, amrvis::DatasetId{11}, 1024 * 1024);
    amrvis::LineQuery lines(dataset);

    amrvis::LineRequest request;
    request.dataset.value = 11;
    request.field.value = 0;
    request.axis = 0;
    request.fixedCoordinates = {0.0, 0.5, 0.0};
    request.maximumLevel = 1;

    const auto composite = lines.execute(request);
    require(composite.line.axis == 0, "line result axis mismatch");
    // Native composite sampling emits one sample per actual cell: coarse cell 0
    // over [0, 0.25), fine cells 2-5 over [0.25, 0.75), coarse cell 3 over
    // [0.75, 1.0). Six samples total, with no flat coarse steps.
    struct NativeSample { double position; float value; int level; };
    const std::vector<NativeSample> native = {
        {0.125, 1.0F, 0}, {0.3125, 3.0F, 1}, {0.4375, 3.5F, 1},
        {0.5625, 4.0F, 1}, {0.6875, 4.5F, 1}, {0.875, 2.5F, 0},
    };
    require(composite.line.positions.size() == native.size(),
        "native composite sample count mismatch");
    require(!composite.line.positionsAreIndices,
        "plotfile line positions were incorrectly changed to indices");
    require(composite.line.values.size() == native.size()
            && composite.line.valid.size() == native.size()
            && composite.line.sourceLevel.size() == native.size(),
        "line result arrays disagree in size");
    require(composite.metrics.candidateBlocks == 3,
        "line query did not consider all three intersecting blocks");
    require(composite.metrics.blocksRead == 3 && composite.metrics.cacheHits == 0,
        "first line query did not read the three intersecting blocks");
    require(composite.metrics.payloadBytesRead > 0
            && composite.metrics.payloadBytesRead * 2 < payloadBytes,
        "line query payload accounting is not far below the full dataset");
    for (std::size_t i = 0; i < native.size(); ++i) {
        require(composite.line.positions[i] == native[i].position,
            "native composite position mismatch");
        require(composite.line.valid[i] == 1, "native composite left a hole");
        require(composite.line.values[i] == native[i].value,
            "native composite value mismatch");
        require(composite.line.sourceLevel[i] == native[i].level,
            "native composite source-level mismatch");
    }

    const auto cached = lines.execute(request);
    require(cached.metrics.blocksRead == 0 && cached.metrics.cacheHits == 3,
        "repeated line query did not reuse all three blocks");
    require(cached.metrics.payloadBytesRead == 0, "cached line query performed payload I/O");

    // Cross-check against a slice whose single pixel row covers the same cells.
    // The slice composites on a uniform 8-pixel grid, so each native line
    // sample must agree with the slice cell that contains its position.
    amrvis::SliceQuery slices(dataset);
    amrvis::SliceRequest sliceRequest;
    sliceRequest.dataset.value = 11;
    sliceRequest.field.value = 0;
    sliceRequest.normalDirection = 1;
    sliceRequest.visibleRegion = {{{0.0, 0.5, 0.0}}, {{1.0, 0.625, 0.0}}};
    sliceRequest.maximumLevel = 1;
    sliceRequest.outputSize = {8, 1};
    const auto plane = slices.execute(sliceRequest);
    constexpr double sliceCellSize = 0.125;
    for (std::size_t i = 0; i < native.size(); ++i) {
        const auto cell = static_cast<std::size_t>(
            std::floor(composite.line.positions[i] / sliceCellSize));
        require(cell < 8, "native composite position outside the slice row");
        require(plane.plane.valid[cell] == composite.line.valid[i]
                && plane.plane.values[cell] == composite.line.values[i]
                && plane.plane.sourceLevel[cell] == composite.line.sourceLevel[i],
            "line and slice disagree on their common row");
    }

    // The second field shares the grids with an offset of 100.
    request.field.value = 1;
    const auto temperature = lines.execute(request);
    for (std::size_t sample = 0; sample < composite.line.values.size(); ++sample) {
        require(temperature.line.values[sample] == 100.0F + composite.line.values[sample],
            "second field line value mismatch");
    }
    request.field.value = 0;

    // A line outside the physical domain is entirely invalid.
    request.fixedCoordinates = {0.0, 1.5, 0.0};
    const auto outside = lines.execute(request);
    require(outside.line.positions.size() == 8, "outside line lost its positions");
    for (std::size_t sample = 0; sample < 8; ++sample) {
        require(outside.line.valid[sample] == 0, "outside line reported coverage");
        require(outside.line.values[sample] == 0.0F, "outside line invented a value");
        require(outside.line.sourceLevel[sample] == -1,
            "outside line reported a source level");
    }

    // Exact-level composition on level 1 leaves holes outside the fine grid.
    request.fixedCoordinates = {0.0, 0.5, 0.0};
    request.composition = amrvis::CompositionPolicy::ExactLevel;
    const auto exactFine = lines.execute(request);
    for (std::size_t sample = 0; sample < 8; ++sample) {
        const bool fine = sample >= 2 && sample <= 5;
        require(exactFine.line.valid[sample] == (fine ? 1 : 0),
            "exact-level line filled a fine-level hole");
        if (fine) {
            require(exactFine.line.sourceLevel[sample] == 1,
                "exact-level line missed fine data");
        }
    }

    // Exact-level composition on level 0 samples the coarse mesh and ignores
    // the fine grid even where it covers the line.
    request.maximumLevel = 0;
    const auto exactCoarse = lines.execute(request);
    require(exactCoarse.line.positions.size() == 4, "exact coarse line sample count");
    for (std::size_t sample = 0; sample < 4; ++sample) {
        const auto s = static_cast<double>(sample);
        require(exactCoarse.line.positions[sample] == (s + 0.5) * 0.25,
            "exact coarse line position mismatch");
        require(exactCoarse.line.valid[sample] == 1
                && exactCoarse.line.sourceLevel[sample] == 0,
            "exact coarse line coverage mismatch");
        require(exactCoarse.line.values[sample] == static_cast<float>(0.5 * (s + 2.0)),
            "exact coarse line used fine data");
    }

    // A second dataset over the same files tracks selective reads independently.
    amrvis::PlotfileDataset selective(root, amrvis::DatasetId{12}, 1024 * 1024);
    amrvis::LineQuery selectiveLines(selective);
    amrvis::LineRequest alongY;
    alongY.dataset.value = 12;
    alongY.field.value = 0;
    alongY.axis = 1;
    alongY.fixedCoordinates = {0.125, 0.0, 0.0};
    alongY.maximumLevel = 1;
    const auto vertical = selectiveLines.execute(alongY);
    require(vertical.metrics.candidateBlocks == 1 && vertical.metrics.blocksRead == 1,
        "line along y touched grids away from the line");
    require(vertical.metrics.payloadBytesRead > 0
            && vertical.metrics.payloadBytesRead * 8 < payloadBytes,
        "line along y did not read far below the full dataset");
    // The line at x = 0.125 misses the fine grid (i = 1), so the native walk
    // emits one sample per coarse cell along y: four level-0 cell centers.
    require(vertical.line.positions.size() == 4,
        "line along y native sample count mismatch");
    for (std::size_t sample = 0; sample < 4; ++sample) {
        const auto s = static_cast<double>(sample);
        require(vertical.line.positions[sample] == (s + 0.5) * 0.25,
            "line along y position mismatch");
        require(vertical.line.valid[sample] == 1 && vertical.line.sourceLevel[sample] == 0,
            "line along y unexpectedly used the fine grid");
        require(vertical.line.values[sample] == static_cast<float>(0.5 * s),
            "line along y value mismatch");
    }

    amrvis::StopSource stopped;
    stopped.request_stop();
    bool cancelled = false;
    try {
        [[maybe_unused]] auto ignored = selectiveLines.execute(alongY, stopped.get_token());
    } catch (const amrvis::ReadCancelled&) {
        cancelled = true;
    }
    require(cancelled, "pre-cancelled line query proceeded");
}

void test3d(const std::filesystem::path& source, const std::filesystem::path& work)
{
    const auto root = materializeFixture(source, work / "plotfile_3d");
    writeFab(root / "Level_0" / "Cell_D_00000", 0,
        "((0,0,0) (3,3,3) (0,0,0))", 1, field3d());

    amrvis::PlotfileDataset dataset(root, amrvis::DatasetId{13}, 1024 * 1024);
    amrvis::LineQuery lines(dataset);
    amrvis::SliceQuery slices(dataset);

    // Line along x through y = z = 0.5 (coarse cells j = k = 2).
    amrvis::LineRequest request;
    request.dataset.value = 13;
    request.field.value = 0;
    request.axis = 0;
    request.fixedCoordinates = {0.0, 0.5, 0.5};
    request.maximumLevel = 0;
    const auto alongX = lines.execute(request);
    require(alongX.line.positions.size() == 4, "3-D line sample count mismatch");
    require(alongX.metrics.blocksRead == 1 && alongX.metrics.payloadBytesRead > 0,
        "3-D line did not read its single block");
    for (std::size_t sample = 0; sample < 4; ++sample) {
        const auto s = static_cast<double>(sample);
        require(alongX.line.positions[sample] == (s + 0.5) * 0.25,
            "3-D line position mismatch");
        require(alongX.line.valid[sample] == 1 && alongX.line.sourceLevel[sample] == 0,
            "3-D line coverage mismatch");
        require(alongX.line.values[sample] == static_cast<float>((s + 4.0) / 9.0),
            "3-D line value mismatch");
    }

    // Cross-check against the y-normal slice whose single z row covers the line.
    amrvis::SliceRequest sliceRequest;
    sliceRequest.dataset.value = 13;
    sliceRequest.field.value = 0;
    sliceRequest.normalDirection = 1;
    sliceRequest.physicalPosition = 0.5;
    sliceRequest.visibleRegion = {{{0.0, 0.0, 0.5}}, {{1.0, 1.0, 0.75}}};
    sliceRequest.maximumLevel = 0;
    sliceRequest.outputSize = {4, 1};
    const auto planeX = slices.execute(sliceRequest);
    for (std::size_t sample = 0; sample < 4; ++sample) {
        require(planeX.plane.values[sample] == alongX.line.values[sample]
                && planeX.plane.valid[sample] == 1,
            "3-D line and y-normal slice disagree");
    }

    // Line along z through x = 0.25, y = 0.75 (coarse cells i = 1, j = 3).
    request.axis = 2;
    request.fixedCoordinates = {0.25, 0.75, 0.0};
    const auto alongZ = lines.execute(request);
    require(alongZ.line.axis == 2 && alongZ.line.positions.size() == 4,
        "3-D z line shape mismatch");
    for (std::size_t sample = 0; sample < 4; ++sample) {
        const auto s = static_cast<double>(sample);
        require(alongZ.line.positions[sample] == (s + 0.5) * 0.25,
            "3-D z line position mismatch");
        require(alongZ.line.valid[sample] == 1 && alongZ.line.sourceLevel[sample] == 0,
            "3-D z line coverage mismatch");
        require(alongZ.line.values[sample] == static_cast<float>((s + 4.0) / 9.0),
            "3-D z line value mismatch");
    }

    // Cross-check against the x-normal slice whose single y column covers the line.
    sliceRequest.normalDirection = 0;
    sliceRequest.physicalPosition = 0.25;
    sliceRequest.visibleRegion = {{{0.0, 0.75, 0.0}}, {{1.0, 1.0, 1.0}}};
    sliceRequest.outputSize = {1, 4};
    const auto planeZ = slices.execute(sliceRequest);
    for (std::size_t sample = 0; sample < 4; ++sample) {
        require(planeZ.plane.values[sample] == alongZ.line.values[sample]
                && planeZ.plane.valid[sample] == 1,
            "3-D line and x-normal slice disagree");
    }

    // A line outside the domain in z is entirely invalid.
    request.axis = 0;
    request.fixedCoordinates = {0.0, 0.5, 2.0};
    const auto outside = lines.execute(request);
    for (std::size_t sample = 0; sample < 4; ++sample) {
        require(outside.line.valid[sample] == 0 && outside.line.sourceLevel[sample] == -1,
            "3-D outside line reported coverage");
    }
}

// Regression for the single-level line-plot hang: a non-zero physical origin
// combined with a non-dyadic cell size made the walk advance x to a cell
// boundary, where floor((x - probLo) / cellSize) rounded back to the same
// cell, so x never advanced and the query looped forever (spinning the worker
// and hanging the app at shutdown). Reuses the plotfile_3d fixture -- its
// Cell_H and FAB are index-based and unaffected -- but rewrites the Header to
// the triggering geometry (cell size 0.01015625, prob_lo -0.65).
void testOffsetGeometry(const std::filesystem::path& source, const std::filesystem::path& work)
{
    const auto root = materializeFixture(source, work / "plotfile_3d_offset");
    writeFab(root / "Level_0" / "Cell_D_00000", 0,
        "((0,0,0) (3,3,3) (0,0,0))", 1, field3d());
    {
        std::ofstream header(root / "Header", std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(header), "could not open offset Header for writing");
        // prob_hi = prob_lo + 4 * cellSize; grid bounds [prob_lo, prob_hi] map
        // back to the index box ((0,0,0) (3,3,3) ...) via physicalBoundsToCellBox.
        header <<
            "HyperCLaw-V1.1\n"
            "1\n"
            "q\n"
            "3\n"
            "0.0\n"
            "0\n"
            "-0.65 -0.65 -0.65\n"
            "-0.609375 -0.609375 -0.609375\n"
            "\n"
            "((0,0,0) (3,3,3) (0,0,0))\n"
            "0\n"
            "0.01015625 0.01015625 0.01015625\n"
            "0\n"
            "0\n"
            "0 1 0.0\n"
            "0\n"
            "-0.65 -0.609375\n"
            "-0.65 -0.609375\n"
            "-0.65 -0.609375\n"
            "Level_0/Cell\n";
    }

    amrvis::PlotfileDataset dataset(root, amrvis::DatasetId{21}, 1024 * 1024);
    amrvis::LineQuery lines(dataset);
    const auto& metadata = dataset.metadata();
    require(metadata.dimension == 3 && metadata.finestLevel == 0,
        "offset fixture parsed unexpected dimension or level count");

    amrvis::LineRequest request;
    request.dataset.value = 21;
    request.field.value = 0;
    request.axis = 0;
    const double center = 0.5 * (metadata.physicalDomain.lower[1]
        + metadata.physicalDomain.upper[1]);
    request.fixedCoordinates = {center, center, center};
    request.maximumLevel = 0;
    request.region = amrvis::RealBox{metadata.physicalDomain.lower,
        metadata.physicalDomain.upper};

    // Watchdog: a regressed walk loops forever and would hang ctest. Run it on
    // a worker and fail loudly instead of hanging if it does not return.
    auto ranToCompletion = runWithTimeout(
        [&] {
            const auto result = lines.execute(request);
            require(result.line.positions.size() == 4,
                "offset line query did not sample every cell");
            for (std::size_t sample = 0; sample < 4; ++sample) {
                require(result.line.valid[sample] == 1
                        && result.line.sourceLevel[sample] == 0,
                    "offset line query left a hole or reported a wrong level");
            }
        },
        10);
    require(ranToCompletion,
        "offset line query hung (regression: cell-boundary round-trip stall)");
}

} // namespace

int main(int argc, char* argv[])
{
    require(argc == 3, "two test data path arguments are required");
    const std::filesystem::path plotfile2d(argv[1]);
    const std::filesystem::path plotfile3d(argv[2]);
    require(std::filesystem::is_regular_file(plotfile2d / "Header"),
        "2-D fixture is missing its Header");
    require(std::filesystem::is_regular_file(plotfile3d / "Header"),
        "3-D fixture is missing its Header");

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto work = std::filesystem::temp_directory_path()
        / ("amrexplorer-line-query-" + std::to_string(unique));
    std::filesystem::create_directories(work);

    test2d(plotfile2d, work);
    test3d(plotfile3d, work);
    testOffsetGeometry(plotfile3d, work);

    std::filesystem::remove_all(work);
    return 0;
}
