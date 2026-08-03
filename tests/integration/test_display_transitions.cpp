// The display-transition matrix (step 4 of the MainWindow extraction, see
// agent-notes/issues/mainwindow-needs-extraction.md): drives the worker-side
// pipeline and the DisplayCoordinator through the transitions that
// historically produced stale-derived-state bugs, asserting on every cell
// the internal consistency invariants:
//   I1  in 3-D Visible mode all panels share one range;
//   I2  contour levels are derived from the displayed range;
//   I3  the raster is rendered from the displayed range;
//   I5  the request's output size matches its visible region.
// (I4 — the view transform frames the raster — is view-side; its decision
// logic is unit-tested in test_display_coordinator and its wiring by the
// Qt zoom/pan smoke tests.)

#include <amrexplorer/data/LocalDatasetSession.hpp>
#include <amrexplorer/pipeline/DisplayCoordinator.hpp>
#include <amrexplorer/pipeline/ParticleProjection.hpp>
#include <amrexplorer/pipeline/SlicePipeline.hpp>
#include <amrexplorer/render2d/ScalarRenderer.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view realDescriptor =
    "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))";

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool nearlyEqual(double a, double b, double tolerance = 1.0e-9)
{
    return std::fabs(a - b)
        <= tolerance * std::max({1.0, std::fabs(a), std::fabs(b)});
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create fixture text");
    output << text;
}

void writeFab(const std::filesystem::path& path, std::string_view box,
    std::span<const double> values)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create fixture FAB");
    output << "FAB " << realDescriptor << box << " 1\n";
    output.write(reinterpret_cast<const char*>(values.data()),
        static_cast<std::streamsize>(values.size() * sizeof(double)));
}

// Two-level 2-D plotfile: level 0 is 4x4 with phi = 1 + i + j (range 1..7),
// level 1 one fine block over cells (2,2)..(5,5) with phi = 10 + i + j
// (range 14..20). Statistics recorded, so File/Level modes are available;
// the File union is [1, 20].
void write2dPlotfile(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    std::filesystem::create_directories(root / "Level_1");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nphi\n"
        "2\n0.0\n1\n"
        "0.0 0.0\n1.0 1.0\n2\n"
        "((0,0) (3,3) (0,0))\n"
        "((0,0) (7,7) (0,0))\n"
        "0 0\n"
        "0.25 0.25\n0.125 0.125\n"
        "0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n"
        "Level_0/Cell\n"
        "1 1 0.0\n0\n"
        "0.25 0.75\n0.25 0.75\n"
        "Level_1/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((0,0) (3,3) (0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n1.0,\n\n1,1\n7.0,\n\n");
    writeText(root / "Level_1" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((2,2) (5,5) (0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n14.0,\n\n1,1\n20.0,\n\n");
    std::vector<double> coarse;
    for (int j = 0; j <= 3; ++j) {
        for (int i = 0; i <= 3; ++i) {
            coarse.push_back(1.0 + i + j);
        }
    }
    std::vector<double> fine;
    for (int j = 2; j <= 5; ++j) {
        for (int i = 2; i <= 5; ++i) {
            fine.push_back(10.0 + i + j);
        }
    }
    writeFab(root / "Level_0" / "Cell_D_00000", "((0,0) (3,3) (0,0))", coarse);
    writeFab(root / "Level_1" / "Cell_D_00000", "((2,2) (5,5) (0,0))", fine);
}

// Single-level 3-D plotfile: 4x4x4 with q = 1 + i + j + k (range 1..10).
void write3dPlotfile(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nq\n"
        "3\n0.0\n0\n"
        "0.0 0.0 0.0\n1.0 1.0 1.0\n\n"
        "((0,0,0) (3,3,3) (0,0,0))\n"
        "0\n"
        "0.25 0.25 0.25\n"
        "0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n0.0 1.0\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((0,0,0) (3,3,3) (0,0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n1.0,\n\n1,1\n10.0,\n\n");
    std::vector<double> values;
    for (int k = 0; k <= 3; ++k) {
        for (int j = 0; j <= 3; ++j) {
            for (int i = 0; i <= 3; ++i) {
                values.push_back(1.0 + i + j + k);
            }
        }
    }
    writeFab(root / "Level_0" / "Cell_D_00000",
        "((0,0,0) (3,3,3) (0,0,0))", values);
}

// Single-level 3-D plotfile whose three default mid-planes are mixed-sign:
// q = 5 + 0.1*(i + j + k) everywhere (all-positive), except cell (0,0,2) is
// forced to -3. The x=0.5 (i=2) and y=0.5 (j=2) mid-planes are all-positive,
// while the z=0.5 (k=2) mid-plane holds the single negative, so the shared
// Visible union crosses zero even though two panels resolve log on their own.
void write3dMixedSignPlotfile(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nq\n"
        "3\n0.0\n0\n"
        "0.0 0.0 0.0\n1.0 1.0 1.0\n\n"
        "((0,0,0) (3,3,3) (0,0,0))\n"
        "0\n"
        "0.25 0.25 0.25\n"
        "0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n0.0 1.0\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((0,0,0) (3,3,3) (0,0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n-3.0,\n\n1,1\n5.9,\n\n");
    std::vector<double> values;
    for (int k = 0; k <= 3; ++k) {
        for (int j = 0; j <= 3; ++j) {
            for (int i = 0; i <= 3; ++i) {
                const bool negativeCell = i == 0 && j == 0 && k == 2;
                values.push_back(negativeCell
                    ? -3.0 : 5.0 + 0.1 * (i + j + k));
            }
        }
    }
    writeFab(root / "Level_0" / "Cell_D_00000",
        "((0,0,0) (3,3,3) (0,0,0))", values);
}

// The per-display internal-consistency invariants (I2, I3, I5 above).
void requireDisplayInvariants(const amrvis::DatasetMetadata& metadata,
    const amrvis::SliceDisplayResult& d, const amrvis::Palette& palette,
    const std::string& context)
{
    require(d.minimum < d.maximum, context + ": range has no extent");
    if (d.logarithmic) {
        require(d.minimum > 0.0, context + ": logarithmic range not positive");
    }
    // I5: the request is internally consistent — its output size is the
    // native size of its own visible region.
    const auto native = amrvis::finestNativeOutputSize(
        metadata, d.request.visibleRegion, d.request.normalDirection);
    require(native == d.request.outputSize,
        context + ": output size does not match the visible region");
    // I3: the raster is exactly the plane rendered against the displayed
    // range (bit-identical rgba).
    if (!d.rasterUnchanged) {
        const auto reference = amrvis::renderScalarPlane(d.slice.plane,
            amrvis::ScalarRenderSettings{
                .minimum = d.minimum,
                .maximum = d.maximum,
                .logarithmic = d.logarithmic,
                .palette = &palette
            });
        require(reference.rgba == d.image.rgba,
            context + ": raster is not rendered from the displayed range");
    }
    // I2: every contour level is one of the levels derived from the
    // displayed range.
    if (!d.contourPolylines.empty()) {
        const auto expected = amrvis::contourValues(
            d.minimum, d.maximum, d.contourCount, d.logarithmic);
        for (const auto& polyline : d.contourPolylines) {
            bool derived = false;
            for (const auto value : expected) {
                if (nearlyEqual(polyline.value, value)) {
                    derived = true;
                    break;
                }
            }
            require(derived,
                context + ": contour level not derived from the range");
        }
    }
}

std::pair<double, double> planeExtrema(const amrvis::ScalarPlane& plane)
{
    const auto range = amrvis::finiteRange(plane);
    require(range.has_value(), "plane has no finite samples");
    return *range;
}

} // namespace

int main()
{
    const auto unique
        = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path()
        / ("amrexplorer-display-transitions-" + std::to_string(unique));
    const auto root2d = base / "plt2d";
    const auto root3d = base / "plt3d";
    const auto root3dMixed = base / "plt3dmixed";
    write2dPlotfile(root2d);
    write3dPlotfile(root3d);
    write3dMixedSignPlotfile(root3dMixed);

    const amrvis::Palette palette;
    constexpr std::uint64_t bigBudget = 1ULL << 20;
    std::uint64_t nextId = 1;
    const auto load2d = [&](amrvis::FrameSliceSpec spec) {
        return amrvis::executeFrameLoad(
            root2d, amrvis::DatasetId{nextId++}, spec, bigBudget, {});
    };

    // --- initial load, 2-D, across range modes -----------------------------
    {
        amrvis::FrameSliceSpec spec;   // File range, Raster, finest available
        const auto file = load2d(spec);
        require(file.displays.size() == 1, "2-D load produced extra displays");
        const auto& d = file.displays.front();
        requireDisplayInvariants(file.dataset->metadata(), d, palette,
            "initial File");
        require(nearlyEqual(d.minimum, 1.0) && nearlyEqual(d.maximum, 20.0),
            "File range is not the statistics union");
        require(d.request.maximumLevel == 1
                && d.request.composition
                    == amrvis::CompositionPolicy::FinestAvailable,
            "default level selection is not finest-available");
    }
    {
        amrvis::FrameSliceSpec spec;
        spec.rangeMode = amrvis::RangeMode::Visible;
        const auto result = load2d(spec);
        const auto& d = result.displays.front();
        requireDisplayInvariants(result.dataset->metadata(), d, palette,
            "initial Visible");
        const auto extrema = planeExtrema(d.slice.plane);
        require(nearlyEqual(d.minimum, extrema.first)
                && nearlyEqual(d.maximum, extrema.second),
            "Visible range is not the plane extrema");
    }
    {
        amrvis::FrameSliceSpec spec;
        spec.rangeMode = amrvis::RangeMode::User;
        spec.userRange = std::pair{2.0, 6.0};
        const auto result = load2d(spec);
        const auto& d = result.displays.front();
        requireDisplayInvariants(result.dataset->metadata(), d, palette,
            "initial User");
        require(nearlyEqual(d.minimum, 2.0) && nearlyEqual(d.maximum, 6.0),
            "User range was not honored");
    }
    {
        // Logarithmic over a positive range stays logarithmic; over a
        // non-positive user range it falls back to linear (the whole slice
        // must not fail).
        amrvis::FrameSliceSpec spec;
        spec.logarithmic = true;
        const auto log = load2d(spec);
        require(log.displays.front().logarithmic,
            "positive-range logarithmic request fell back");
        requireDisplayInvariants(log.dataset->metadata(),
            log.displays.front(), palette, "initial File log");

        spec.rangeMode = amrvis::RangeMode::User;
        spec.userRange = std::pair{-1.0, 5.0};
        const auto fallback = load2d(spec);
        require(!fallback.displays.front().logarithmic,
            "non-positive logarithmic request did not fall back to linear");
        requireDisplayInvariants(fallback.dataset->metadata(),
            fallback.displays.front(), palette, "log fallback");
    }
    {
        // Contour and vector display modes.
        amrvis::FrameSliceSpec spec;
        spec.displayMode = amrvis::DisplayMode::RasterContours;
        spec.contourCount = 4;
        const auto contours = load2d(spec);
        const auto& d = contours.displays.front();
        require(!d.contourPolylines.empty(), "no contours were extracted");
        requireDisplayInvariants(contours.dataset->metadata(), d, palette,
            "initial contours");

        spec.displayMode = amrvis::DisplayMode::VelocityVectors;
        const auto vectors = load2d(spec);
        require(!vectors.displays.front().vectors.empty(),
            "no vector glyphs were generated");
        requireDisplayInvariants(vectors.dataset->metadata(),
            vectors.displays.front(), palette, "initial vectors");
    }

    // --- zoomed load: region honored, clipped, or dropped ------------------
    {
        amrvis::FrameSliceSpec spec;
        spec.rangeMode = amrvis::RangeMode::Visible;
        amrvis::RealBox zoom;
        zoom.lower = {{0.5, 0.2, 0.0}};
        zoom.upper = {{0.8, 0.8, 0.0}};
        spec.visibleRegions = {zoom};
        const auto result = load2d(spec);
        const auto& d = result.displays.front();
        requireDisplayInvariants(result.dataset->metadata(), d, palette,
            "zoomed Visible");
        require(nearlyEqual(d.request.visibleRegion.lower[0], 0.5)
                && nearlyEqual(d.request.visibleRegion.upper[0], 0.8),
            "an in-bounds zoom region was not honored");
        const auto extrema = planeExtrema(d.slice.plane);
        require(nearlyEqual(d.minimum, extrema.first)
                && nearlyEqual(d.maximum, extrema.second),
            "zoomed Visible range is not the subregion extrema");
    }
    {
        // A preserved zoom that no longer intersects the domain falls back
        // to the whole domain (the FAB-round-trip / frame-step trap).
        amrvis::FrameSliceSpec spec;
        amrvis::RealBox outside;
        outside.lower = {{2.0, 2.0, 0.0}};
        outside.upper = {{3.0, 3.0, 0.0}};
        spec.visibleRegions = {outside};
        const auto result = load2d(spec);
        const auto& d = result.displays.front();
        const auto bounds
            = amrvis::datasetSampleBounds(result.dataset->metadata());
        require(d.request.visibleRegion == bounds,
            "a non-intersecting zoom did not fall back to the whole domain");
        requireDisplayInvariants(result.dataset->metadata(), d, palette,
            "region fallback");
    }
    {
        // Particle projection follows the displayed plane's physical region:
        // a zoom both clips the full-domain endpoints and remaps its midpoint.
        amrvis::FrameSliceSpec spec;
        const auto full = load2d(spec);
        const std::vector particles{
            amrvis::ParticlePoint{.id = 0, .position = {{0.0, 0.0, 0.0}}},
            amrvis::ParticlePoint{
                .id = 0, .position = {{0.625, 0.625, 0.0}}},
            amrvis::ParticlePoint{.id = 0, .position = {{1.0, 1.0, 0.0}}}
        };
        const auto fullProjection = amrvis::projectParticlePoints(
            particles, full.displays.front().slice.plane, 2, 1);
        require(fullProjection.size() == 3,
            "full-domain particle projection clipped an in-bounds point");

        amrvis::RealBox zoom;
        zoom.lower = {{0.5, 0.5, 0.0}};
        zoom.upper = {{0.75, 0.75, 0.0}};
        spec.visibleRegions = {zoom};
        const auto zoomed = load2d(spec);
        const auto& zoomedPlane = zoomed.displays.front().slice.plane;
        const auto zoomedProjection = amrvis::projectParticlePoints(
            particles, zoomedPlane, 2, 1);
        require(zoomedProjection.size() == 1,
            "zoomed particle projection did not clip the full-domain endpoints");
        require(nearlyEqual(zoomedProjection.front().x,
                    0.5 * zoomedPlane.width)
                && nearlyEqual(zoomedProjection.front().y,
                    0.5 * zoomedPlane.height),
            "zoomed particle projection did not remap the physical midpoint");
    }
    {
        // An out-of-range exact level falls back to finest available.
        amrvis::FrameSliceSpec spec;
        spec.levelSelection = 7;
        const auto result = load2d(spec);
        const auto& d = result.displays.front();
        require(d.request.maximumLevel == 1
                && d.request.composition
                    == amrvis::CompositionPolicy::FinestAvailable,
            "an out-of-range level selection did not fall back to finest");
    }

    // --- 3-D shared Visible range (the stale-contours shape) ---------------
    {
        amrvis::FrameSliceSpec spec;
        spec.rangeMode = amrvis::RangeMode::Visible;
        spec.logarithmic = true;
        spec.displayMode = amrvis::DisplayMode::RasterContours;
        spec.contourCount = 3;
        spec.defaultPositions = false;
        spec.slicePositions = {0.875, 0.625, 0.375};
        const auto result = amrvis::executeFrameLoad(
            root3d, amrvis::DatasetId{nextId++}, spec, bigBudget, {});
        require(result.displays.size() == 3, "3-D load lost a panel");
        const auto& first = result.displays.front();
        for (const auto& d : result.displays) {
            require(nearlyEqual(d.minimum, first.minimum)
                    && nearlyEqual(d.maximum, first.maximum),
                "3-D Visible panels do not share one range");
            requireDisplayInvariants(result.dataset->metadata(), d, palette,
                "3-D shared range");
        }
    }

    // --- 3-D shared Visible range, log, mixed-sign field -------------------
    {
        // Regression for shared-log-range-render-throw-fails-load: log
        // requested, two default mid-planes all-positive, but the shared union
        // crosses zero. The per-panel log flag used to keep the positive
        // panels logarithmic against the negative union minimum, so
        // renderScalarPlane threw and executeFrameLoad failed the whole load.
        amrvis::FrameSliceSpec spec;
        spec.rangeMode = amrvis::RangeMode::Visible;
        spec.logarithmic = true;
        spec.displayMode = amrvis::DisplayMode::RasterContours;
        spec.contourCount = 3;
        const auto result = amrvis::executeFrameLoad(
            root3dMixed, amrvis::DatasetId{nextId++}, spec, bigBudget, {});
        require(result.displays.size() == 3, "mixed-sign 3-D load lost a panel");
        const auto& first = result.displays.front();
        require(first.minimum < 0.0,
            "mixed-sign union did not cross zero as the fixture intends");
        for (const auto& d : result.displays) {
            // I1: one shared range across all panels.
            require(nearlyEqual(d.minimum, first.minimum)
                    && nearlyEqual(d.maximum, first.maximum),
                "mixed-sign 3-D panels do not share one range");
            // The whole set degrades to linear together (never per-panel).
            require(!d.logarithmic,
                "a panel stayed logarithmic against a union that crosses zero");
            requireDisplayInvariants(result.dataset->metadata(), d, palette,
                "mixed-sign 3-D shared range");
        }
    }

    // --- cosmetic refresh from cached planes --------------------------------
    {
        amrvis::FrameSliceSpec spec;
        spec.displayMode = amrvis::DisplayMode::RasterContours;
        spec.contourCount = 4;
        const auto baseLoad = load2d(spec);
        const auto& d0 = baseLoad.displays.front();
        const auto& metadata = baseLoad.dataset->metadata();

        // Log toggle re-ranges, re-renders, and re-contours the cached planes.
        const auto log = amrvis::refreshCachedSlice(baseLoad.dataset,
            d0.request, d0.slice.plane, d0.contourPlane, d0.contourFinePlane,
            d0.contourFineFactor, {}, amrvis::RangeMode::File, std::nullopt,
            true, palette, amrvis::DisplayMode::RasterContours, 0, 0, 4, true);
        require(log.logarithmic, "cached-plane log toggle fell back");
        requireDisplayInvariants(metadata, log, palette, "refresh log");

        // A contour-count change with a clean raster leaves the image alone.
        const auto recount = amrvis::refreshCachedSlice(baseLoad.dataset,
            d0.request, d0.slice.plane, d0.contourPlane, d0.contourFinePlane,
            d0.contourFineFactor, {}, amrvis::RangeMode::File, std::nullopt,
            false, palette, amrvis::DisplayMode::RasterContours, 0, 0, 2,
            false);
        require(recount.rasterUnchanged && recount.image.width == 0,
            "a contour-only refresh re-rendered the raster");
        require(!recount.contourPolylines.empty(),
            "a contour-only refresh lost the contours");
        requireDisplayInvariants(metadata, recount, palette, "refresh count");

        // A range-mode change to User re-renders against the user range.
        const auto user = amrvis::refreshCachedSlice(baseLoad.dataset,
            d0.request, d0.slice.plane, d0.contourPlane, d0.contourFinePlane,
            d0.contourFineFactor, {}, amrvis::RangeMode::User,
            std::pair{2.0, 3.0}, false, palette,
            amrvis::DisplayMode::RasterContours, 0, 0, 4, true);
        require(nearlyEqual(user.minimum, 2.0)
                && nearlyEqual(user.maximum, 3.0),
            "cached-plane User range was not honored");
        requireDisplayInvariants(metadata, user, palette, "refresh user");
    }

    // --- zoom arrival realigned to a reused full-domain range ---------------
    {
        // The raster-colorbar and stale-contours shape in one cell: a zoomed
        // Visible slice resolves its own local range, then the arrival is
        // realigned to the cached full-domain range; raster and contours
        // must follow.
        amrvis::FrameSliceSpec spec;
        spec.rangeMode = amrvis::RangeMode::Visible;
        spec.displayMode = amrvis::DisplayMode::RasterContours;
        spec.contourCount = 4;
        amrvis::RealBox zoom;
        zoom.lower = {{0.15, 0.15, 0.0}};
        zoom.upper = {{0.45, 0.45, 0.0}};   // coarse-only corner: local range
        spec.visibleRegions = {zoom};
        auto result = load2d(spec);
        auto d = result.displays.front();
        const auto local = std::pair{d.minimum, d.maximum};
        require(local.second < 20.0,
            "the zoom cell unexpectedly covers the full range");

        amrvis::DisplayCoordinator::realignArrivalToRange(
            d, {1.0, 20.0}, palette, true);
        require(nearlyEqual(d.minimum, 1.0) && nearlyEqual(d.maximum, 20.0),
            "the arrival was not realigned to the cached range");
        requireDisplayInvariants(result.dataset->metadata(), d, palette,
            "realigned arrival");
    }

    // --- cache-pressure fallback via executeSliceWithFallback ---------------
    {
        // Measure the two blocks' resident sizes on a fresh dataset with a
        // generous budget (a frame load would have populated the cache
        // already), then rerun with budgets sized to force each fallback.
        auto measured = std::make_shared<amrvis::LocalDatasetSession>(
            root2d, amrvis::DatasetId{nextId++}, bigBudget);
        const auto metadata = measured->metadata();
        const auto bounds = amrvis::datasetSampleBounds(metadata);
        amrvis::SliceRequest request;
        request.field = amrvis::FieldId{0};
        request.normalDirection = 1;
        request.visibleRegion = bounds;
        request.outputSize
            = amrvis::finestNativeOutputSize(metadata, bounds, 1);
        request.composition = amrvis::CompositionPolicy::ExactLevel;
        request.maximumLevel = 0;
        request.dataset = measured->id();
        (void)amrvis::executeSlice(measured, request,
            amrvis::RangeMode::Visible, std::nullopt, false, palette, {});
        const auto coarseBytes = measured->cacheMetrics().residentBytes;
        request.composition = amrvis::CompositionPolicy::FinestAvailable;
        request.maximumLevel = 1;
        (void)amrvis::executeSlice(measured, request,
            amrvis::RangeMode::Visible, std::nullopt, false, palette, {});
        const auto bothBytes = measured->cacheMetrics().residentBytes;
        const auto fineBytes = bothBytes - coarseBytes;
        require(coarseBytes > 0 && fineBytes > 0,
            "block size measurement failed");

        const auto freshRequest = [&](const amrvis::DatasetSession& dataset) {
            auto fresh = request;
            fresh.dataset = dataset.id();
            return fresh;
        };

        // Composite finest under pressure: falls back to level 0 and records
        // the fallback; the result is still internally consistent.
        auto tight = std::make_shared<amrvis::LocalDatasetSession>(root2d,
            amrvis::DatasetId{nextId++}, coarseBytes + fineBytes / 2);
        const auto fallback = amrvis::executeSliceWithFallback(tight,
            freshRequest(*tight), amrvis::RangeMode::Visible, std::nullopt,
            false, palette, amrvis::DisplayMode::Raster, 0, 0, 4, {});
        require(fallback.cacheFallbackFromLevel == 1
                && fallback.cacheFallbackToLevel == 0
                && fallback.request.maximumLevel == 0,
            "cache pressure did not fall back to level 0");
        requireDisplayInvariants(metadata, fallback, palette,
            "budget fallback");

        // A generous budget records no fallback.
        auto roomy = std::make_shared<amrvis::LocalDatasetSession>(root2d,
            amrvis::DatasetId{nextId++}, bigBudget);
        const auto normal = amrvis::executeSliceWithFallback(roomy,
            freshRequest(*roomy), amrvis::RangeMode::Visible, std::nullopt,
            false, palette, amrvis::DisplayMode::Raster, 0, 0, 4, {});
        require(normal.cacheFallbackFromLevel == -1
                && normal.cacheFallbackToLevel == -1,
            "an unpressured slice recorded a fallback");

        // An exact level cannot shed resolution: actionable error.
        auto exact = std::make_shared<amrvis::LocalDatasetSession>(root2d,
            amrvis::DatasetId{nextId++}, std::min(coarseBytes, fineBytes) / 2);
        bool threw = false;
        try {
            auto exactRequest = freshRequest(*exact);
            exactRequest.composition = amrvis::CompositionPolicy::ExactLevel;
            exactRequest.maximumLevel = 1;
            (void)amrvis::executeSliceWithFallback(exact, exactRequest,
                amrvis::RangeMode::Visible, std::nullopt, false, palette,
                amrvis::DisplayMode::Raster, 0, 0, 4, {});
        } catch (const std::exception& error) {
            threw = true;
            require(std::string(error.what()).find("selected slice level")
                    != std::string::npos,
                "the exact-level error is not actionable");
        }
        require(threw, "an oversized exact level did not report an error");

        // Even level 0 cannot fit: the actionable dead-end error.
        auto hopeless = std::make_shared<amrvis::LocalDatasetSession>(root2d,
            amrvis::DatasetId{nextId++}, coarseBytes / 2);
        threw = false;
        try {
            (void)amrvis::executeSliceWithFallback(hopeless,
                freshRequest(*hopeless), amrvis::RangeMode::Visible,
                std::nullopt, false, palette, amrvis::DisplayMode::Raster,
                0, 0, 4, {});
        } catch (const std::exception& error) {
            threw = true;
            require(std::string(error.what()).find("even at level 0")
                    != std::string::npos,
                "the level-0 dead end is not actionable");
        }
        require(threw, "an impossible budget did not report an error");
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(base, cleanupError);
    return 0;
}
