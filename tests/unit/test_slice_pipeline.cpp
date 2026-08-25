// Unit tests for the pure slice-decision helpers extracted from MainWindow
// into the pipeline layer: sameSliceSpec (the cached-slice key comparison),
// coveredCells (a request's data resolution along an axis), finestNativeOutputSize
// (native render resolution), and slicePlaneAxes (the in-plane axis pair).
// resolveRange/resolveDisplayRange/effectiveRangeMode already have coverage in
// test_slice_range_resolver.cpp. resolveSpecField (what a carried field
// selection means in the next frame's own field list) is here too.
#include <amrexplorer/pipeline/SlicePipeline.hpp>

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Request.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

// A non-default SliceRequest so that flipping any single field to a different
// value is a real change sameSliceSpec must notice.
amrvis::SliceRequest baseRequest()
{
    amrvis::SliceRequest request;
    request.dataset.value = 7;
    request.field.value = 2;
    request.component = 1;
    request.normalDirection = 1;
    request.physicalPosition = 0.5;
    request.visibleRegion = {{{0.0, 0.0, 0.0}}, {{1.0, 1.0, 1.0}}};
    request.maximumLevel = 3;
    request.outputSize = {640, 480};
    request.sampling = amrvis::SamplingPolicy::PiecewiseConstant;
    request.composition = amrvis::CompositionPolicy::FinestAvailable;
    return request;
}

// A single cell-centered level of `dimension` dimensions with a
// [0,9]^d index domain and the given uniform cell size (origin 0).
amrvis::DatasetMetadata makeMetadata(int dimension, double cellSize)
{
    amrvis::DatasetMetadata metadata;
    metadata.dimension = dimension;
    metadata.finestLevel = 0;
    metadata.fields.push_back({"phi", amrvis::Centering::Cell, {}});
    amrvis::LevelMetadata level;
    level.level = 0;
    level.domain = {{{0, 0, 0}}, {{9, 9, 9}}, {{0, 0, 0}}};
    level.cellSize = {{cellSize, cellSize, cellSize}};
    metadata.levels.push_back(std::move(level));
    return metadata;
}

} // namespace

int main()
{
    // --- sameSliceSpec ----------------------------------------------------
    // Identical requests match; changing any compared field breaks the match.
    const auto base = baseRequest();
    require(amrvis::sameSliceSpec(base, base),
        "identical slice requests were not considered the same");

    {
        auto other = base;
        other.dataset.value = 8;
        require(!amrvis::sameSliceSpec(base, other), "dataset difference missed");
    }
    {
        auto other = base;
        other.field.value = 3;
        require(!amrvis::sameSliceSpec(base, other), "field difference missed");
    }
    {
        auto other = base;
        other.component = 2;
        require(!amrvis::sameSliceSpec(base, other), "component difference missed");
    }
    {
        auto other = base;
        other.normalDirection = 2;
        require(!amrvis::sameSliceSpec(base, other), "normal difference missed");
    }
    {
        auto other = base;
        other.physicalPosition = 0.6;
        require(!amrvis::sameSliceSpec(base, other), "position difference missed");
    }
    {
        auto other = base;
        other.visibleRegion.upper[0] = 2.0;
        require(!amrvis::sameSliceSpec(base, other), "region difference missed");
    }
    {
        auto other = base;
        other.maximumLevel = 4;
        require(!amrvis::sameSliceSpec(base, other), "maximum-level difference missed");
    }
    {
        auto other = base;
        other.outputSize = {320, 240};
        require(!amrvis::sameSliceSpec(base, other), "output-size difference missed");
    }
    {
        auto other = base;
        other.sampling = amrvis::SamplingPolicy::Linear;
        require(!amrvis::sameSliceSpec(base, other), "sampling difference missed");
    }
    {
        auto other = base;
        other.composition = amrvis::CompositionPolicy::ExactLevel;
        require(!amrvis::sameSliceSpec(base, other), "composition difference missed");
    }
    {
        auto other = base;
        other.includeGridBoxes = true;
        require(!amrvis::sameSliceSpec(base, other),
            "grid-overlay difference missed");
    }
    // --- coveredCells -----------------------------------------------------
    // A [0,9] cell-centered level with cell size 1 spans physical [0, 10].
    const auto grid = makeMetadata(2, 1.0);
    require(amrvis::coveredCells(grid, 0, 0, 0.0, 10.0) == 10,
        "the full domain did not cover all ten cells");
    require(amrvis::coveredCells(grid, 0, 0, 0.0, 5.0) == 5,
        "the lower half did not cover five cells");
    require(amrvis::coveredCells(grid, 0, 0, 2.5, 7.5) == 6,
        "an interior span did not cover cells 2..7");
    // Clipped to the domain: a request reaching past both edges still caps at
    // the ten cells present.
    require(amrvis::coveredCells(grid, 0, 0, -5.0, 100.0) == 10,
        "an over-wide request was not clipped to the domain");
    // A region entirely outside the domain has no cells, reported as 1.
    require(amrvis::coveredCells(grid, 0, 0, 20.0, 30.0) == 1,
        "a fully-outside request did not report the one-cell floor");

    // --- finestNativeOutputSize -------------------------------------------
    // Cell size 0.25, so a 2.5-wide region is 10 finest cells per axis.
    const auto fine2d = makeMetadata(2, 0.25);
    const amrvis::RealBox square{{{0.0, 0.0, 0.0}}, {{2.5, 2.5, 0.0}}};
    require(amrvis::finestNativeOutputSize(fine2d, square, 2) == (std::array<int, 2>{10, 10}),
        "2-D native size is not one output cell per finest cell");
    // Below one cell rounds to zero and clamps up to 1; a huge region clamps to
    // the maximum output dimension.
    const amrvis::RealBox sliver{{{0.0, 0.0, 0.0}}, {{0.1, 0.1, 0.0}}};
    require(amrvis::finestNativeOutputSize(fine2d, sliver, 2) == (std::array<int, 2>{1, 1}),
        "a sub-cell region did not clamp up to one output cell");
    const amrvis::RealBox huge{{{0.0, 0.0, 0.0}}, {{2000.0, 2000.0, 0.0}}};
    const auto capped = amrvis::finestNativeOutputSize(fine2d, huge, 2);
    require(capped[0] == amrvis::maxSliceOutputDimension
            && capped[1] == amrvis::maxSliceOutputDimension,
        "an oversized region was not capped at the output-dimension limit");

    // 3-D picks the two axes perpendicular to the normal, in ascending order.
    const auto fine3d = makeMetadata(3, 0.25);
    const amrvis::RealBox box{{{0.0, 0.0, 0.0}}, {{2.5, 5.0, 7.5}}};
    require(amrvis::finestNativeOutputSize(fine3d, box, 0) == (std::array<int, 2>{20, 30}),
        "normal x did not size from the y and z extents");
    require(amrvis::finestNativeOutputSize(fine3d, box, 1) == (std::array<int, 2>{10, 30}),
        "normal y did not size from the x and z extents");
    require(amrvis::finestNativeOutputSize(fine3d, box, 2) == (std::array<int, 2>{10, 20}),
        "normal z did not size from the x and y extents");

    // A negotiated frame cap shrinks the raster uniformly until the server's
    // conservative response model fits.
    const auto budgeted = amrvis::frameBudgetBoundedOutputSize(
        {800, 800}, 4U * 1024U * 1024U);
    const auto budgetCells = static_cast<std::uint64_t>(budgeted[0])
        * static_cast<std::uint64_t>(budgeted[1]);
    require(budgeted[0] == budgeted[1] && budgeted[0] < 800,
        "frame-budget sizing did not preserve a square raster aspect");
    require(budgetCells * amrvis::sliceResponseBytesPerCell
            + amrvis::sliceResponseOverheadBytes <= 4U * 1024U * 1024U,
        "frame-budget sizing still exceeds the negotiated response cap");

    // Spherical aspect uses the unclamped finest-level sample counts. An
    // 8192x1024 logical plane must remain 8:1 even though native output is
    // independently capped at 4096 per axis.
    auto spherical = makeMetadata(2, 1.0);
    spherical.coordinateSystem = 2;
    spherical.levels[0].cellSize = {{1.0 / 8192.0, 1.0 / 1024.0, 1.0}};
    const amrvis::RealBox sphericalRegion{
        {{0.0, 0.0, 0.0}}, {{1.0, 1.0, 0.0}}};
    require(amrvis::viewportBoundedOutputSize(
                spherical, sphericalRegion, 2, {800, 800})
            == (std::array<int, 2>{800, 100}),
        "spherical viewport aspect was computed from clamped sample counts");

    // Cells need not be square: a 100x1600-cell domain that is 1x1600 in
    // physical units displays at the cell aspect (1:16), so the raster
    // fitted into the viewport is 59x945, not a one-pixel strip (1x945,
    // the physical aspect). Both the whole domain and a fractional zoomed
    // window (100x600.5 cells) keep the cell aspect.
    auto anisotropic = makeMetadata(2, 1.0);
    anisotropic.levels[0].cellSize = {{0.01, 1.0, 1.0}};
    const amrvis::RealBox tallDomain{{{0.0, 0.0, 0.0}}, {{1.0, 1600.0, 0.0}}};
    require(amrvis::viewportBoundedOutputSize(
                anisotropic, tallDomain, 2, {1137, 945})
            == (std::array<int, 2>{59, 945}),
        "anisotropic cells were fitted at the physical aspect");
    require(amrvis::nativeBoundedViewportOutputSize(
                anisotropic, tallDomain, 2, {1137, 945})
            == (std::array<int, 2>{59, 945}),
        "the native bound squeezed the anisotropic Fit raster");
    const amrvis::RealBox tallWindow{{{0.0, 0.0, 0.0}}, {{1.0, 600.5, 0.0}}};
    require(amrvis::viewportBoundedOutputSize(
                anisotropic, tallWindow, 2, {1137, 945})
            == (std::array<int, 2>{157, 945}),
        "a zoomed anisotropic window was fitted at the physical aspect");

    // A viewport request may downsample a large native plane, but it must not
    // supersample a small one. Fixed 1x uses the native raster as its logical
    // scale, so enlarging 10x10 to the viewport would make it act like Fit.
    require(amrvis::nativeBoundedViewportOutputSize(
                fine2d, square, 2, {800, 600})
            == (std::array<int, 2>{10, 10}),
        "remote viewport sizing supersampled a small native raster");
    const auto largeViewport = amrvis::nativeBoundedViewportOutputSize(
        fine2d, huge, 2, {800, 600});
    require(largeViewport == (std::array<int, 2>{600, 600}),
        "remote viewport sizing did not retain bounded downsampling");

    // --- resolveSpecField -------------------------------------------------
    {
        const auto listed = [](const std::vector<std::string>& names) {
            amrvis::DatasetMetadata metadata;
            metadata.dimension = 2;
            for (const auto& name : names) {
                metadata.fields.push_back(
                    {name, amrvis::Centering::Cell, {}});
            }
            return metadata;
        };
        // The frame the selection was made on: "speed" is a derived field at
        // id 4, after three stored fields and one earlier derived one.
        const auto before = listed({"rho", "temp", "vel", "flux", "speed"});
        require(amrvis::resolveSpecField(before, "speed", 4) == 4,
            "a name present at its own index did not resolve to it");

        // The next frame lacks "temp", so "flux" (which read it) was skipped
        // and every id after it moved down two. The index alone lands on a
        // different field; the name is what still means the same thing.
        const auto after = listed({"rho", "vel", "speed"});
        require(amrvis::resolveSpecField(after, "speed", 4) == 2,
            "the name did not follow the field through a renumbering");
        require(amrvis::resolveSpecField(after, "", 4) == 2,
            "an index with no name is not clamped into range");
        require(amrvis::resolveSpecField(after, "flux", 3) == 2,
            "a name this frame does not have did not fall back to the index");
        require(amrvis::resolveSpecField(after, "rho", 4) == 0,
            "a name did not win over an out-of-range index");
        // Nothing to select from at all.
        require(amrvis::resolveSpecField(listed({}), "rho", 7) == 0,
            "an empty field list did not resolve to zero");
    }

    // --- slicePlaneAxes ---------------------------------------------------
    require(amrvis::slicePlaneAxes(2, 0) == (std::array<int, 2>{0, 1}),
        "2-D plane axes are not x,y");
    require(amrvis::slicePlaneAxes(3, 0) == (std::array<int, 2>{1, 2}),
        "normal x plane axes are not y,z");
    require(amrvis::slicePlaneAxes(3, 1) == (std::array<int, 2>{0, 2}),
        "normal y plane axes are not x,z");
    require(amrvis::slicePlaneAxes(3, 2) == (std::array<int, 2>{0, 1}),
        "normal z plane axes are not x,y");

    return 0;
}
