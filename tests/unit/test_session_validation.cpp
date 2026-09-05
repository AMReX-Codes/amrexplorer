#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/data/LocalDatasetSession.hpp>
#include <amrexplorer/data/SessionValidation.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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

template <typename Function>
void requireRejected(Function&& function, const char* message)
{
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    require(false, message);
}

template <typename Function>
void requireRejectedWith(
    Function&& function, const char* needle, const char* message)
{
    try {
        function();
    } catch (const std::exception& error) {
        if (std::string(error.what()).find(needle) != std::string::npos) {
            return;
        }
        std::cerr << "rejected for the wrong reason: " << error.what() << '\n';
    }
    require(false, message);
}

template <typename Function>
void requireAccepted(Function&& function, const char* message)
{
    try {
        function();
        return;
    } catch (const std::exception& error) {
        std::cerr << "rejected with: " << error.what() << '\n';
    }
    require(false, message);
}

amrvis::DatasetMetadata dataset(int dimension)
{
    amrvis::DatasetMetadata metadata;
    metadata.dimension = dimension;
    metadata.finestLevel = 1;
    metadata.hasPhysicalGeometry = true;
    // Cell size 1 over index domain 0..3, so each axis spans 0..4 physically.
    metadata.physicalDomain = amrvis::RealBox{
        amrvis::Real3{{0.0, 0.0, 0.0}}, amrvis::Real3{{4.0, 4.0, 4.0}}};
    metadata.fields.push_back({"density", amrvis::Centering::Cell, {}});
    for (int level = 0; level < 2; ++level) {
        amrvis::LevelMetadata entry;
        entry.level = level;
        entry.domain = amrvis::IntBox{amrvis::Int3{{0, 0, 0}},
            amrvis::Int3{{3, 3, 3}}, amrvis::Int3{{0, 0, 0}}};
        // One box covering the level, so a grid overlay has something to be.
        entry.boxes.push_back(entry.domain);
        metadata.levels.push_back(entry);
    }
    return metadata;
}

amrvis::SliceRequest sliceRequest(int dimension)
{
    amrvis::SliceRequest request;
    request.dataset = amrvis::DatasetId{1};
    request.field = amrvis::FieldId{0};
    request.normalDirection = dimension == 3 ? 2 : 1;
    request.visibleRegion = amrvis::RealBox{
        amrvis::Real3{{0.0, 0.0, 0.0}}, amrvis::Real3{{4.0, 4.0, 4.0}}};
    request.physicalPosition = 2.0;
    request.maximumLevel = 1;
    request.outputSize = {2, 2};
    request.includeGridBoxes = true;
    return request;
}

amrvis::SliceQueryResult sliceResult(
    const amrvis::RealBox& region, bool gridBoxesIncluded = true)
{
    amrvis::SliceQueryResult result;
    result.gridBoxesIncluded = gridBoxesIncluded;
    result.plane.width = 2;
    result.plane.height = 2;
    result.plane.physicalRegion = region;
    result.plane.values = {1.0F, 2.0F, 3.0F, 4.0F};
    result.plane.valid = {1, 1, 1, 1};
    result.plane.sourceLevel = {0, 1, -1, 0};
    return result;
}

amrvis::LineViewRequest lineRequest(int dimension, int axis)
{
    amrvis::LineViewRequest request;
    request.query.dataset = amrvis::DatasetId{1};
    request.query.field = amrvis::FieldId{0};
    request.query.axis = axis;
    request.query.maximumLevel = 1;
    request.outputWidth = 8;
    static_cast<void>(dimension);
    return request;
}

amrvis::LineQueryResult lineResult(int axis)
{
    amrvis::LineQueryResult result;
    result.line.axis = axis;
    result.line.positions = {0.25, 0.75};
    result.line.values = {1.0F, 2.0F};
    result.line.valid = {1, 1};
    result.line.sourceLevel = {1, -1};
    return result;
}

double unfusedMultiplyAdd(double left, double right, double addend)
{
    volatile double product = left * right;
    return addend + product;
}

amrvis::RealBox evaluatedBounds(const amrvis::LevelMetadata& level,
    const amrvis::IntBox& box, int dimension, bool fused)
{
    amrvis::RealBox result;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto entry = static_cast<std::size_t>(axis);
        const auto nodalHalf = amrvis::isNodal(box, axis) ? 0.5 : 0.0;
        const auto evaluate = [&](double factor) {
            return fused
                ? std::fma(factor, level.cellSize[entry],
                    level.indexOrigin[entry])
                : unfusedMultiplyAdd(factor, level.cellSize[entry],
                    level.indexOrigin[entry]);
        };
        result.lower[entry]
            = evaluate(static_cast<double>(box.lower[entry]) - nodalHalf);
        result.upper[entry] = evaluate(
            static_cast<double>(box.upper[entry]) + 1.0 - nodalHalf);
    }
    return result;
}

amrvis::DatasetPageRequest pageRequest(int dimension)
{
    amrvis::DatasetPageRequest request;
    request.dataset = amrvis::DatasetId{1};
    request.field = amrvis::FieldId{0};
    request.level = 0;
    // The whole level: index window 0..3 on each in-plane axis.
    request.region = amrvis::RealBox{
        amrvis::Real3{{0.0, 0.0, 0.0}}, amrvis::Real3{{4.0, 4.0, 4.0}}};
    request.normalAxis = dimension == 3 ? 2 : 1;
    request.slicePosition = 2.5;
    request.maximumExtent = 8;
    return request;
}

// A default-constructed page is the empty one: no cells, inverted bounds.
amrvis::DatasetPage page(int nx, int ny)
{
    amrvis::DatasetPage result;
    if (nx <= 0 || ny <= 0) {
        return result;
    }
    result.nx = nx;
    result.ny = ny;
    result.lower = {0, 0};
    result.upper = {nx - 1, ny - 1};
    result.sliceIndex = 2;
    const auto samples
        = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
    result.values.assign(samples, 0.0F);
    result.covered.assign(samples, 1);
    return result;
}

} // namespace

int main()
{
    using namespace amrvis;

    // planeAxes: the in-plane pair, and total for a normal it cannot honour.
    require((planeAxes(2, 1) == std::array<int, 2>{0, 1}),
        "a 2-D plane does not use both axes");
    require((planeAxes(3, 0) == std::array<int, 2>{1, 2})
            && (planeAxes(3, 1) == std::array<int, 2>{0, 2})
            && (planeAxes(3, 2) == std::array<int, 2>{0, 1}),
        "3-D plane axes are wrong for some normal");
    require((planeAxes(3, 9) == std::array<int, 2>{0, 1}),
        "an out-of-range normal did not fall back to a safe pair");

    // A line region is meaningful only along the line axis, in both 2-D and
    // 3-D: reversed or non-finite there is a malformed request, while the
    // off-axis entries stay free to be degenerate.
    for (const int dimension : {2, 3}) {
        auto request = lineRequest(dimension, 0);
        request.query.region = RealBox{
            Real3{{0.25, 0.5, 0.5}}, Real3{{0.75, 0.5, 0.5}}};
        require(validateLineRequest(request.query, dimension).empty(),
            "a line region degenerate off its axis was rejected");

        request.query.region = RealBox{
            Real3{{0.75, 0.0, 0.0}}, Real3{{0.25, 1.0, 1.0}}};
        require(!validateLineRequest(request.query, dimension).empty(),
            "a reversed line region was accepted");

        request.query.region = RealBox{
            Real3{{0.5, 0.0, 0.0}}, Real3{{0.5, 1.0, 1.0}}};
        require(!validateLineRequest(request.query, dimension).empty(),
            "a zero-extent line region was accepted");

        request.query.region = RealBox{
            Real3{{std::nan(""), 0.0, 0.0}}, Real3{{1.0, 1.0, 1.0}}};
        require(!validateLineRequest(request.query, dimension).empty(),
            "a non-finite line region was accepted");

        request.query.region.reset();
        require(validateLineRequest(request.query, dimension).empty(),
            "a line request without a region was rejected");
    }

    // The page region, at the trust boundary rather than inside the builder.
    for (const int dimension : {2, 3}) {
        const auto metadata = dataset(dimension);
        auto request = pageRequest(dimension);
        requireAccepted([&] {
            validateSessionDatasetPageRequest(
                metadata, DatasetId{1}, request);
        }, "a valid dataset page request was rejected");

        auto reversed = request;
        reversed.region.lower[0] = 1.0;
        reversed.region.upper[0] = 0.0;
        requireRejected([&] {
            validateSessionDatasetPageRequest(
                metadata, DatasetId{1}, reversed);
        }, "a reversed page region was accepted");

        auto degenerate = request;
        degenerate.region.upper[1] = degenerate.region.lower[1];
        requireRejected([&] {
            validateSessionDatasetPageRequest(
                metadata, DatasetId{1}, degenerate);
        }, "a page region with no in-plane extent was accepted");
    }

    // In 3-D the normal axis of a page region may be degenerate: it carries the
    // slice position, not an extent.
    {
        const auto metadata = dataset(3);
        auto request = pageRequest(3);
        request.region.upper[2] = request.region.lower[2];
        requireAccepted([&] {
            validateSessionDatasetPageRequest(
                metadata, DatasetId{1}, request);
        }, "a 3-D page region degenerate on its normal axis was rejected");
    }

    // Slice results: tied to the request that was made, and to the catalog.
    for (const int dimension : {2, 3}) {
        const auto metadata = dataset(dimension);
        const auto request = sliceRequest(dimension);
        const ViewDataRequest view = request;
        // A slice too large for one frame is refused rather than reduced, and
        // the query copies the requested region, so a real answer carries both
        // verbatim.
        const auto region = request.visibleRegion;
        requireAccepted([&] {
            validateSessionViewResult(metadata, view, sliceResult(region));
        }, "a valid slice result was rejected");

        auto elsewhere = region;
        elsewhere.lower[0] += 0.25;
        elsewhere.upper[0] += 0.25;
        requireRejected([&] {
            validateSessionViewResult(metadata, view, sliceResult(elsewhere));
        }, "a slice result over another region was accepted");

        auto reversed = region;
        reversed.upper[0] = reversed.lower[0] - 1.0;
        requireRejected([&] {
            validateSessionViewResult(metadata, view, sliceResult(reversed));
        }, "a slice result with a reversed plane region was accepted");

        auto reshaped = sliceResult(region);
        reshaped.plane.width = 3;
        reshaped.plane.height = 1;
        reshaped.plane.values = {1.0F, 2.0F, 3.0F};
        reshaped.plane.valid = {1, 1, 1};
        reshaped.plane.sourceLevel = {0, 0, 0};
        requireRejected([&] {
            validateSessionViewResult(metadata, view, reshaped);
        }, "a slice raster of another shape was accepted");

        auto beyond = sliceResult(region);
        beyond.plane.sourceLevel = {0, 1, 2, 0};
        requireRejected([&] {
            validateSessionViewResult(metadata, view, beyond);
        }, "a slice source level past the finest level was accepted");

        auto sentinel = sliceResult(region);
        sentinel.plane.sourceLevel = {0, 1, -2, 0};
        requireRejected([&] {
            validateSessionViewResult(metadata, view, sentinel);
        }, "a slice source level below the no-data sentinel was accepted");

        // Provenance the dataset has but the request excluded: asking for level
        // 0 and being answered from level 1 is not the requested composition.
        auto coarse = request;
        coarse.maximumLevel = 0;
        const ViewDataRequest coarseView = coarse;
        auto finer = sliceResult(region);
        finer.plane.sourceLevel = {0, 1, 0, 0};
        requireRejected([&] {
            validateSessionViewResult(metadata, coarseView, finer);
        }, "a slice source level above the requested maximum was accepted");
        auto atMaximum = sliceResult(region);
        atMaximum.plane.sourceLevel = {0, 0, -1, 0};
        requireAccepted([&] {
            validateSessionViewResult(metadata, coarseView, atMaximum);
        }, "a slice at the requested maximum level was rejected");

        auto ragged = sliceResult(region);
        ragged.plane.values.pop_back();
        requireRejected([&] {
            validateSessionViewResult(metadata, view, ragged);
        }, "a slice result with a short value vector was accepted");

        auto grid = sliceResult(region);
        grid.gridBoxes.push_back({2, region});
        requireRejected([&] {
            validateSessionViewResult(metadata, view, grid);
        }, "a grid box on a nonexistent level was accepted");

        grid = sliceResult(region);
        grid.gridBoxes.push_back({-1, region});
        requireRejected([&] {
            validateSessionViewResult(metadata, view, grid);
        }, "a grid box with no level was accepted");

        grid = sliceResult(region);
        grid.gridBoxes.push_back({1, reversed});
        requireRejected([&] {
            validateSessionViewResult(metadata, view, grid);
        }, "a grid box with a reversed region was accepted");

        // The overlay is requested or it is not: a peer cannot install one the
        // caller switched off, and cannot place a box outside the window.
        auto without = request;
        without.includeGridBoxes = false;
        const ViewDataRequest withoutView = without;
        requireAccepted([&] {
            validateSessionViewResult(
                metadata, withoutView, sliceResult(region, false));
        }, "a slice without overlays was rejected");
        requireRejected([&] {
            validateSessionViewResult(
                metadata, withoutView, sliceResult(region, true));
        }, "an unrequested overlay flag was accepted");
        auto unrequested = sliceResult(region, false);
        unrequested.gridBoxes.push_back({1, region});
        requireRejected([&] {
            validateSessionViewResult(metadata, withoutView, unrequested);
        }, "an unrequested grid box was accepted");
        // The flag runs both ways: dropping it would leave the window showing
        // the overlay from the previous slice.
        requireRejected([&] {
            validateSessionViewResult(metadata, view, sliceResult(region, false));
        }, "a missing overlay flag was accepted for a request that asked");

        auto outside = sliceResult(region);
        auto beyondWindow = region;
        beyondWindow.lower[0] -= 1.0;
        outside.gridBoxes.push_back({1, beyondWindow});
        requireRejected([&] {
            validateSessionViewResult(metadata, view, outside);
        }, "a grid box outside the visible region was accepted");

        // The catalog says where the boxes are: an invented one inside the
        // window corresponds to no box of that level.
        auto invented = sliceResult(region);
        auto smaller = region;
        smaller.upper[0] -= 0.5;
        invented.gridBoxes.push_back({1, smaller});
        requireRejected([&] {
            validateSessionViewResult(metadata, view, invented);
        }, "a grid box matching no catalog box was accepted");

        // The overlay a good peer sends: each catalog box of the level, in
        // physical space and clipped to the window -- here the level's one
        // box covers the window exactly. sampleBounds leaves the axis past
        // the dimension at zero, the way the peer's own arithmetic does.
        const auto& source = metadata.levels.back();
        auto catalogBox = sampleBounds(
            source, source.boxes.front(), metadata.dimension);
        for (const auto axis :
            planeAxes(metadata.dimension, request.normalDirection)) {
            const auto entry = static_cast<std::size_t>(axis);
            catalogBox.lower[entry] = std::max(catalogBox.lower[entry],
                request.visibleRegion.lower[entry]);
            catalogBox.upper[entry] = std::min(catalogBox.upper[entry],
                request.visibleRegion.upper[entry]);
        }
        auto overlay = sliceResult(region);
        overlay.gridBoxes.push_back({1, catalogBox});
        requireAccepted([&] {
            validateSessionViewResult(metadata, view, overlay);
        }, "a grid box over the catalog box was rejected");

        // This geometry has no fused/unfused disagreement: its origin, indices
        // and cell size are all exact. An arbitrary ulp nudge is therefore not
        // an alternate evaluation of the catalog box and must not be admitted.
        // Nudge inward so the box stays within the window and across the slice:
        // the catalog match is then the only check left to reject it, which is
        // what the named reason confirms.
        auto perturbed = sliceResult(region);
        auto nudged = catalogBox;
        for (std::size_t axis = 0;
            axis < static_cast<std::size_t>(metadata.dimension); ++axis) {
            for (int step = 0; step < 2; ++step) {
                nudged.lower[axis] = std::nextafter(nudged.lower[axis],
                    std::numeric_limits<double>::infinity());
                nudged.upper[axis] = std::nextafter(nudged.upper[axis],
                    -std::numeric_limits<double>::infinity());
            }
        }
        perturbed.gridBoxes.push_back({1, nudged});
        requireRejectedWith([&] {
            validateSessionViewResult(metadata, view, perturbed);
        }, "matches no box",
            "an underived ulp-nudged grid box was accepted");

        if (dimension == 3) {
            // All three axes are compared, so the normal extent has to be the
            // catalog box's own rather than anything plausible.
            auto stretched = sliceResult(region);
            auto box = region;
            box.lower[2] -= 1.0;
            box.upper[2] += 1.0;
            stretched.gridBoxes.push_back({1, box});
            requireRejected([&] {
                validateSessionViewResult(metadata, view, stretched);
            }, "a grid box with an invented normal extent was accepted");

            auto offSliceRequest = request;
            offSliceRequest.physicalPosition = 99.0;
            const ViewDataRequest offSliceView = offSliceRequest;
            auto offSlice = sliceResult(region);
            offSlice.gridBoxes.push_back({1, catalogBox});
            requireRejectedWith([&] {
                validateSessionViewResult(metadata, offSliceView, offSlice);
            }, "misses the requested slice",
                "a catalog box away from the requested slice was accepted");
        }

        // A line answer to a slice request is not an answer at all.
        requireRejected([&] {
            validateSessionViewResult(metadata, view, lineResult(0));
        }, "a line result answered a slice request");
    }

    // The reported failure used coordinates around 1e22. Exercise actual fused
    // and separately-rounded evaluations, including independent edge choices,
    // rather than standing in for them with an arbitrary ulp translation.
    {
        auto metadata = dataset(2);
        constexpr double origin = -4.57724532306911e22;
        constexpr double cellSize = 7.132980051857025e18;
        for (auto& level : metadata.levels) {
            level.domain = IntBox{Int3{{5000, 5000, 0}},
                Int3{{6000, 6000, 0}}, Int3{{0, 0, 0}}};
            level.boxes = {level.domain};
            level.indexOrigin = Real3{{origin, origin, 0.0}};
            level.cellSize = Real3{{cellSize, cellSize, 1.0}};
        }
        metadata.physicalDomain = sampleBounds(
            metadata.levels.front(), metadata.levels.front().domain, 2);
        auto request = sliceRequest(2);
        request.visibleRegion = metadata.physicalDomain;
        for (std::size_t axis = 0; axis < 2; ++axis) {
            request.visibleRegion.lower[axis] -= cellSize;
            request.visibleRegion.upper[axis] += cellSize;
        }
        const ViewDataRequest view = request;
        const auto& level = metadata.levels.back();
        const auto& indexBox = level.boxes.front();
        const auto unfused = evaluatedBounds(level, indexBox, 2, false);
        const auto fused = evaluatedBounds(level, indexBox, 2, true);
        require(unfused != fused,
            "the large-coordinate fused/unfused fixture is vacuous");
        require(sampleBounds(level, indexBox, 2) == fused,
            "sampleBounds did not use the canonical fused evaluation");
        auto mixed = unfused;
        mixed.lower[0] = fused.lower[0];
        mixed.upper[1] = fused.upper[1];
        auto inward = unfused;
        for (std::size_t axis = 0; axis < 2; ++axis) {
            inward.lower[axis]
                = std::max(unfused.lower[axis], fused.lower[axis]);
            inward.upper[axis]
                = std::min(unfused.upper[axis], fused.upper[axis]);
        }
        for (const auto& box : {unfused, fused, mixed, inward}) {
            auto answer = sliceResult(request.visibleRegion);
            answer.gridBoxes.push_back({1, box});
            requireAccepted([&] {
                validateSessionViewResult(metadata, view, answer);
            }, "a legitimate fused/unfused grid-box edge was rejected");
        }
    }

    // Coordinate magnitude must not buy an answer any slack. At 1e16 the
    // spacing between doubles is 2, a whole cell here, so this one-cell
    // translation is exactly what an allowance scaled to the coordinate would
    // have admitted.
    {
        auto metadata = dataset(2);
        constexpr double origin = 1.0e16;
        constexpr double cellSize = 2.0;
        for (auto& level : metadata.levels) {
            level.indexOrigin = Real3{{origin, origin, 0.0}};
            level.cellSize = Real3{{cellSize, cellSize, 1.0}};
        }
        metadata.physicalDomain = sampleBounds(
            metadata.levels.front(), metadata.levels.front().domain, 2);
        auto request = sliceRequest(2);
        request.visibleRegion = metadata.physicalDomain;
        for (std::size_t axis = 0; axis < 2; ++axis) {
            request.visibleRegion.lower[axis] -= cellSize;
            request.visibleRegion.upper[axis] += cellSize;
        }
        const ViewDataRequest view = request;
        auto answer = sliceResult(request.visibleRegion);
        auto shifted = metadata.physicalDomain;
        for (std::size_t axis = 0; axis < 2; ++axis) {
            shifted.lower[axis] += cellSize;
            shifted.upper[axis] += cellSize;
        }
        answer.gridBoxes.push_back({1, shifted});
        requireRejectedWith([&] {
            validateSessionViewResult(metadata, view, answer);
        }, "matches no box",
            "a whole-cell-shifted box at large coordinates was accepted");
    }

    // A catalog box merely touching the viewport is clipped to zero width and
    // SliceQuery drops it. It cannot authorize a positive sliver one ulp wide
    // at the same edge.
    {
        auto metadata = dataset(2);
        constexpr double origin = 1.0e13;
        for (auto& level : metadata.levels) {
            level.domain = IntBox{Int3{{0, 0, 0}}, Int3{{1, 1, 0}},
                Int3{{0, 0, 0}}};
            level.boxes = {IntBox{Int3{{0, 0, 0}}, Int3{{0, 0, 0}},
                Int3{{0, 0, 0}}}};
            level.indexOrigin = Real3{{origin, origin, 0.0}};
            level.cellSize = Real3{{1.0, 1.0, 1.0}};
        }
        metadata.physicalDomain = sampleBounds(
            metadata.levels.front(), metadata.levels.front().domain, 2);
        auto request = sliceRequest(2);
        request.visibleRegion = RealBox{Real3{{origin + 1.0, origin, 0.0}},
            Real3{{origin + 2.0, origin + 1.0, 0.0}}};
        const ViewDataRequest view = request;
        auto answer = sliceResult(request.visibleRegion);
        auto sliver = RealBox{Real3{{origin + 1.0, origin, 0.0}},
            Real3{{std::nextafter(origin + 1.0,
                       std::numeric_limits<double>::infinity()),
                origin + 1.0, 0.0}}};
        answer.gridBoxes.push_back({1, sliver});
        requireRejectedWith([&] {
            validateSessionViewResult(metadata, view, answer);
        }, "matches no box",
            "a positive sliver derived from a zero-width clip was accepted");
    }

    // A cell small beside its coordinate puts the fused and separately-rounded
    // evaluations of a legal int-index edge more than a quarter of a cell
    // apart. Both are still exact evaluations of the same catalog box, so both
    // have to be accepted -- which is why the accepted set is the two
    // alternatives and not an interval scaled to the cell.
    {
        auto metadata = dataset(2);
        metadata.finestLevel = 0;
        metadata.levels.resize(1);
        auto& level = metadata.levels.front();
        constexpr double origin = 1.0e8;
        constexpr double cellSize = 2.9224442392587661e-8;
        constexpr int gridIndex = 1671731333;
        level.level = 0;
        level.domain = IntBox{Int3{{0, 0, 0}},
            Int3{{2000000000, 2000000000, 0}}, Int3{{0, 0, 0}}};
        level.boxes = {IntBox{Int3{{gridIndex, gridIndex, 0}},
            Int3{{gridIndex, gridIndex, 0}}, Int3{{0, 0, 0}}}};
        level.indexOrigin = Real3{{origin, origin, 0.0}};
        level.cellSize = Real3{{cellSize, cellSize, 1.0}};
        metadata.physicalDomain = sampleBounds(level, level.domain, 2);
        const auto unfused = evaluatedBounds(level, level.boxes.front(), 2,
            false);
        const auto fused = evaluatedBounds(level, level.boxes.front(), 2, true);
        require(unfused.lower[0] != fused.lower[0]
                && std::fabs(unfused.lower[0] - fused.lower[0])
                    > 0.25 * cellSize,
            "the wide-ulp fused/unfused fixture is vacuous");
        auto request = sliceRequest(2);
        request.maximumLevel = 0;
        request.visibleRegion = metadata.physicalDomain;
        const ViewDataRequest view = request;
        for (const auto& box : {unfused, fused}) {
            auto answer = sliceResult(request.visibleRegion);
            answer.plane.sourceLevel = {0, 0, -1, 0};
            answer.gridBoxes.push_back({0, box});
            requireAccepted([&] {
                validateSessionViewResult(metadata, view, answer);
            }, "a wide-ulp fused/unfused grid box was rejected");
        }
    }

    // Per-level identity is part of the result. A fine box one eighth of a
    // coarse cell inside the coarse box is valid under its own tag and matches
    // nothing under the coarse one, so the level an answer names is the level
    // it has to be matched against.
    {
        auto metadata = dataset(2);
        constexpr double origin = 1.0e14;
        auto& coarse = metadata.levels[0];
        coarse.domain = IntBox{Int3{{0, 0, 0}}, Int3{{15, 15, 0}},
            Int3{{0, 0, 0}}};
        coarse.boxes = {IntBox{Int3{{0, 0, 0}}, Int3{{7, 7, 0}},
            Int3{{0, 0, 0}}}};
        coarse.indexOrigin = Real3{{origin, origin, 0.0}};
        coarse.cellSize = Real3{{1.0, 1.0, 1.0}};
        auto& fine = metadata.levels[1];
        fine.domain = IntBox{Int3{{0, 0, 0}}, Int3{{127, 127, 0}},
            Int3{{0, 0, 0}}};
        fine.boxes = {IntBox{Int3{{1, 1, 0}}, Int3{{63, 63, 0}},
            Int3{{0, 0, 0}}}};
        fine.indexOrigin = Real3{{origin, origin, 0.0}};
        fine.cellSize = Real3{{0.125, 0.125, 1.0}};
        metadata.physicalDomain = sampleBounds(coarse, coarse.domain, 2);
        auto request = sliceRequest(2);
        request.visibleRegion = metadata.physicalDomain;
        const ViewDataRequest view = request;
        const auto fineBox = sampleBounds(fine, fine.boxes.front(), 2);
        auto correct = sliceResult(request.visibleRegion);
        correct.gridBoxes.push_back({1, fineBox});
        requireAccepted([&] {
            validateSessionViewResult(metadata, view, correct);
        }, "a fine grid box with its correct level was rejected");
        auto mistagged = sliceResult(request.visibleRegion);
        mistagged.gridBoxes.push_back({0, fineBox});
        requireRejectedWith([&] {
            validateSessionViewResult(metadata, view, mistagged);
        }, "matches no box", "fine geometry falsely tagged coarse was accepted");
    }

    // The search band comes from the catalog boxes' own fused/unfused width and
    // from nothing else, so a level whose domain runs out to index 1e9 grants
    // no slack at all to a box sitting at the origin.
    {
        auto metadata = dataset(2);
        metadata.finestLevel = 0;
        metadata.levels.resize(1);
        auto& level = metadata.levels.front();
        level.level = 0;
        level.domain = IntBox{Int3{{0, 0, 0}},
            Int3{{1000000000, 1000000000, 0}}, Int3{{0, 0, 0}}};
        level.boxes = {IntBox{Int3{{0, 0, 0}}, Int3{{0, 0, 0}},
            Int3{{0, 0, 0}}}};
        metadata.physicalDomain = sampleBounds(level, level.domain, 2);
        auto request = sliceRequest(2);
        request.maximumLevel = 0;
        request.visibleRegion
            = RealBox{Real3{{0.0, 0.0, 0.0}}, Real3{{2.0, 2.0, 0.0}}};
        const ViewDataRequest view = request;
        auto shifted = sampleBounds(level, level.boxes.front(), 2);
        for (std::size_t axis = 0; axis < 2; ++axis) {
            shifted.lower[axis] += 1.0e-6;
            shifted.upper[axis] += 1.0e-6;
        }
        auto answer = sliceResult(request.visibleRegion);
        answer.plane.sourceLevel = {0, 0, -1, 0};
        answer.gridBoxes.push_back({0, shifted});
        requireRejectedWith([&] {
            validateSessionViewResult(metadata, view, answer);
        }, "matches no box",
            "a near-origin box used the far domain edge as slack");
    }

    // A slab decomposition gives many boxes the same first coordinate. The
    // search descends coordinate by coordinate, so every later y group has to
    // stay reachable behind that shared x edge, including boxes whose edges mix
    // fused and separately-rounded evaluation.
    {
        auto metadata = dataset(2);
        constexpr double origin = -4.57724532306911e22;
        constexpr double cellSize = 7.132980051857025e18;
        for (auto& level : metadata.levels) {
            level.domain = IntBox{Int3{{5000, 5000, 0}},
                Int3{{5000, 5063, 0}}, Int3{{0, 0, 0}}};
            level.boxes.clear();
            for (int y = 0; y < 64; ++y) {
                level.boxes.push_back(IntBox{Int3{{5000, 5000 + y, 0}},
                    Int3{{5000, 5000 + y, 0}}, Int3{{0, 0, 0}}});
            }
            level.indexOrigin = Real3{{origin, origin, 0.0}};
            level.cellSize = Real3{{cellSize, cellSize, 1.0}};
        }
        metadata.physicalDomain = sampleBounds(
            metadata.levels.front(), metadata.levels.front().domain, 2);
        auto request = sliceRequest(2);
        request.visibleRegion = metadata.physicalDomain;
        for (std::size_t axis = 0; axis < 2; ++axis) {
            request.visibleRegion.lower[axis] -= cellSize;
            request.visibleRegion.upper[axis] += cellSize;
        }
        const ViewDataRequest view = request;
        auto answer = sliceResult(request.visibleRegion);
        auto sawDisagreement = false;
        for (const auto& indexBox : metadata.levels.back().boxes) {
            const auto unfused = evaluatedBounds(
                metadata.levels.back(), indexBox, 2, false);
            const auto fused = evaluatedBounds(
                metadata.levels.back(), indexBox, 2, true);
            sawDisagreement = sawDisagreement || unfused != fused;
            auto box = unfused;
            box.lower[0] = fused.lower[0];
            box.upper[1] = fused.upper[1];
            answer.gridBoxes.push_back({1, box});
        }
        require(sawDisagreement,
            "the slab fused/unfused fixture is vacuous");
        requireAccepted([&] {
            validateSessionViewResult(metadata, view, answer);
        }, "a later grid box sharing the first coordinate was rejected");
    }

    // Line results.
    {
        const auto metadata = dataset(3);
        const ViewDataRequest view = lineRequest(3, 1);
        requireAccepted([&] {
            validateSessionViewResult(metadata, view, lineResult(1));
        }, "a valid line result was rejected");

        requireRejected([&] {
            validateSessionViewResult(metadata, view, lineResult(0));
        }, "a line result along the wrong axis was accepted");

        auto beyond = lineResult(1);
        beyond.line.sourceLevel = {1, 4};
        requireRejected([&] {
            validateSessionViewResult(metadata, view, beyond);
        }, "a line source level past the finest level was accepted");

        auto ragged = lineResult(1);
        ragged.line.valid.pop_back();
        requireRejected([&] {
            validateSessionViewResult(metadata, view, ragged);
        }, "a line result with a short validity vector was accepted");

        // boundLineToViewport allows at most two samples per output pixel.
        auto narrow = lineRequest(3, 1);
        narrow.outputWidth = 1;
        const ViewDataRequest narrowView = narrow;
        requireAccepted([&] {
            validateSessionViewResult(metadata, narrowView, lineResult(1));
        }, "a line at exactly two samples per pixel was rejected");

        auto dense = lineResult(1);
        for (int index = 0; index < 8; ++index) {
            dense.line.positions.push_back(0.1 * index);
            dense.line.values.push_back(1.0F);
            dense.line.valid.push_back(1);
            dense.line.sourceLevel.push_back(0);
        }
        requireRejected([&] {
            validateSessionViewResult(metadata, narrowView, dense);
        }, "a line denser than its viewport allows was accepted");

        // Whether the horizontal axis is physical or an index belongs to the
        // dataset: this one has geometry, so index positions contradict it.
        auto indexed = lineResult(1);
        indexed.line.positionsAreIndices = true;
        requireRejected([&] {
            validateSessionViewResult(metadata, view, indexed);
        }, "index positions were accepted for a physical dataset");

        auto unsorted = lineResult(1);
        unsorted.line.positions = {0.75, 0.25};
        requireRejected([&] {
            validateSessionViewResult(metadata, view, unsorted);
        }, "unordered line positions were accepted");

        auto beyondExtent = lineResult(1);
        beyondExtent.line.positions = {0.25, 99.0};
        requireRejected([&] {
            validateSessionViewResult(metadata, view, beyondExtent);
        }, "a line position outside the sampled level was accepted");

        // A region narrows the extent, so a sample outside it is not an answer
        // to this request even though the level contains it.
        auto narrowed = lineRequest(3, 1);
        narrowed.query.region = RealBox{
            Real3{{1.0, 1.0, 1.0}}, Real3{{2.0, 2.0, 2.0}}};
        const ViewDataRequest narrowedView = narrowed;
        auto inside = lineResult(1);
        inside.line.positions = {1.5, 1.75};
        requireAccepted([&] {
            validateSessionViewResult(metadata, narrowedView, inside);
        }, "a line inside the requested region was rejected");
        auto strays = lineResult(1);
        strays.line.positions = {0.5, 3.5};
        requireRejected([&] {
            validateSessionViewResult(metadata, narrowedView, strays);
        }, "a line straying outside the requested region was accepted");

        // And a region may legitimately reach past the domain: the walk then
        // reports invalid samples out there, which must not be refused.
        auto wide = lineRequest(3, 1);
        wide.query.region = RealBox{
            Real3{{-2.0, -2.0, -2.0}}, Real3{{6.0, 6.0, 6.0}}};
        const ViewDataRequest wideView = wide;
        auto outsideDomain = lineResult(1);
        outsideDomain.line.positions = {-1.5, 5.5};
        outsideDomain.line.valid = {0, 0};
        outsideDomain.line.sourceLevel = {-1, -1};
        requireAccepted([&] {
            validateSessionViewResult(metadata, wideView, outsideDomain);
        }, "invalid samples outside the domain were refused");

        auto nonFinite = lineResult(1);
        nonFinite.line.positions = {0.25, std::nan("")};
        requireRejected([&] {
            validateSessionViewResult(metadata, view, nonFinite);
        }, "a non-finite line position was accepted");
    }

    // A dataset without physical geometry reports index positions, bounded by
    // the level's index domain rather than its physical extent.
    {
        auto metadata = dataset(2);
        metadata.hasPhysicalGeometry = false;
        const ViewDataRequest view = lineRequest(2, 0);
        auto indexed = lineResult(0);
        indexed.line.positionsAreIndices = true;
        indexed.line.positions = {0.0, 3.0};
        requireAccepted([&] {
            validateSessionViewResult(metadata, view, indexed);
        }, "index positions were rejected for a geometry-free dataset");

        auto physical = lineResult(0);
        physical.line.positions = {0.25, 0.75};
        requireRejected([&] {
            validateSessionViewResult(metadata, view, physical);
        }, "physical positions were accepted for a geometry-free dataset");

        auto beyondDomain = indexed;
        beyondDomain.line.positions = {0.0, 9.0};
        requireRejected([&] {
            validateSessionViewResult(metadata, view, beyondDomain);
        }, "an index position outside the level domain was accepted");

        // A duplicated position is cosmetic, not a lie, and strict increase is
        // not something the producer promises across a level transition.
        auto repeated = indexed;
        repeated.line.positions = {1.0, 1.0};
        requireAccepted([&] {
            validateSessionViewResult(metadata, view, repeated);
        }, "a repeated line position was refused");

        // An index arrives as an int widened to a double. A fractional one is
        // not an index, and one outside int's range cannot even be converted
        // back -- doing so is undefined, so it has to be refused before the
        // cast rather than after it (UBSan proves this pair).
        auto fractional = indexed;
        fractional.line.positions = {0.0, 1.5};
        requireRejectedWith([&] {
            validateSessionViewResult(metadata, view, fractional);
        }, "not an index", "a fractional index position was accepted");

        // The reason matters here: the guard has to refuse these *before* the
        // conversion, and an out-of-range index that survived the cast would be
        // rejected further down for lying outside the extent, which would look
        // like a pass while the undefined conversion had already happened.
        for (const double extreme : {1.0e100, -1.0e100}) {
            auto beyondInt = indexed;
            beyondInt.line.positions = {extreme};
            beyondInt.line.values = {1.0F};
            beyondInt.line.valid = {1};
            beyondInt.line.sourceLevel = {0};
            requireRejectedWith([&] {
                validateSessionViewResult(metadata, view, beyondInt);
            }, "not an index",
                "an index position outside int's range was accepted");
        }
    }

    // Dataset page results.
    {
        const auto metadata = dataset(2);
        const auto request = pageRequest(2);
        requireAccepted([&] {
            validateSessionDatasetPageResult(metadata, request, page(4, 4));
        }, "a valid dataset page was rejected");
        auto oversized = page(4, 4);
        oversized.nx = request.maximumExtent + 1;
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, request, oversized);
        }, "a page larger than the requested extent was accepted");

        auto reversed = page(4, 4);
        reversed.lower[1] = reversed.upper[1] + 1;
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, request, reversed);
        }, "a page with reversed index bounds was accepted");

        auto mismatched = page(4, 4);
        mismatched.upper[0] += 1;
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, request, mismatched);
        }, "a page whose bounds outrun its extent was accepted");

        // The window is determined by the region: a page that starts elsewhere,
        // or stops short without saying it truncated, is not this answer.
        auto shifted = page(2, 2);
        shifted.lower = {2, 2};
        shifted.upper = {3, 3};
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, request, shifted);
        }, "a page starting away from the requested region was accepted");

        auto short_ = page(2, 2);
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, request, short_);
        }, "a page short of the region without truncation was accepted");

        auto truncated = page(2, 2);
        truncated.truncatedX = true;
        truncated.truncatedY = true;
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, request, truncated);
        }, "a truncation claim below the extent limit was accepted");

        // The requested span here is exactly the limit, which the builder does
        // not treat as truncation.
        auto exact = request;
        exact.maximumExtent = 4;
        auto claimed = page(4, 4);
        claimed.truncatedX = true;
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, exact, claimed);
        }, "truncation was accepted for a span equal to the limit");
        requireAccepted([&] {
            validateSessionDatasetPageResult(metadata, exact, page(4, 4));
        }, "a page filling the limit exactly was rejected");

        auto limited = request;
        limited.maximumExtent = 2;
        auto atLimit = page(2, 2);
        atLimit.truncatedX = true;
        atLimit.truncatedY = true;
        requireAccepted([&] {
            validateSessionDatasetPageResult(metadata, limited, atLimit);
        }, "a page truncated to the extent limit was rejected");

        // Emptiness is determined too, in both directions.
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, request, page(0, 0));
        }, "an empty page was accepted where the region meets the level");

        auto away = request;
        away.region.lower[0] = 90.0;
        away.region.upper[0] = 99.0;
        requireAccepted([&] {
            validateSessionDatasetPageResult(metadata, away, page(0, 0));
        }, "an empty page was rejected where the region misses the level");
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, away, page(4, 4));
        }, "a populated page was accepted where the region misses the level");

        // A region too far out to index cannot be answered with a page at all:
        // the request is refused before it is sent, and a page claiming to
        // answer one is impossible rather than merely unverifiable.
        auto unindexable = request;
        unindexable.region.lower[0] = 1.0e100;
        unindexable.region.upper[0] = 2.0e100;
        requireRejected([&] {
            validateSessionDatasetPageRequest(
                metadata, DatasetId{1}, unindexable);
        }, "a region too far out to index was accepted");
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, unindexable, page(4, 4));
        }, "a page answering an unindexable request was accepted");

        auto ragged = page(4, 4);
        ragged.covered.pop_back();
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, request, ragged);
        }, "a page with a short coverage vector was accepted");

        auto lopsided = page(4, 4);
        lopsided.ny = 0;
        lopsided.values.clear();
        lopsided.covered.clear();
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, request, lopsided);
        }, "a page empty on one axis only was accepted");

        // The page indexes the level it named: 1000..1003 is not inside 0..3.
        auto outside = page(4, 4);
        outside.lower[0] = 1000;
        outside.upper[0] = 1003;
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, request, outside);
        }, "a page outside the level domain was accepted");

        // Extreme bounds come straight off the wire; the span arithmetic must
        // not overflow while rejecting them (UBSan proves this one).
        auto extreme = page(4, 4);
        extreme.lower[0] = std::numeric_limits<int>::min();
        extreme.upper[0] = std::numeric_limits<int>::max();
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, request, extreme);
        }, "a page spanning the whole int range was accepted");
        extreme = page(4, 4);
        extreme.lower[1] = std::numeric_limits<int>::max();
        extreme.upper[1] = std::numeric_limits<int>::min();
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, request, extreme);
        }, "a page with maximally reversed bounds was accepted");
    }

    // A 3-D page also names a slice index, which has to be in the level domain.
    {
        const auto metadata = dataset(3);
        const auto request = pageRequest(3);
        auto valid = page(4, 4);
        valid.sliceIndex = 2;
        requireAccepted([&] {
            validateSessionDatasetPageResult(metadata, request, valid);
        }, "a page with an in-domain slice index was rejected");

        auto beyond = page(4, 4);
        beyond.sliceIndex = 99;
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, request, beyond);
        }, "a page with a slice index outside the level domain was accepted");

        auto elsewhere = page(4, 4);
        elsewhere.sliceIndex = 0;
        requireRejected([&] {
            validateSessionDatasetPageResult(metadata, request, elsewhere);
        }, "a page slicing at another position than requested was accepted");
    }

    // Particle samples against the catalog they claim to sample.
    {
        const std::vector<ParticleSpeciesMetadata> species{
            {"electrons", 3, 0, 0, 2, ParticleRealPrecision::Double}};
        ParticleSample sample;
        sample.species = species.front();
        sample.points.push_back({1, Real3{{0.5, 0.5, 0.5}}});
        requireAccepted([&] {
            validateSessionParticleSampleResult(
                species, "electrons", sample);
        }, "a valid particle sample was rejected");

        auto renamed = sample;
        renamed.species.name = "ions";
        requireRejected([&] {
            validateSessionParticleSampleResult(
                species, "electrons", renamed);
        }, "a sample of the wrong species was accepted");

        requireRejected([&] {
            validateSessionParticleSampleResult(species, "ions", sample);
        }, "a sample of an uncatalogued species was accepted");

        auto flattened = sample;
        flattened.species.dimension = 0;
        requireRejected([&] {
            validateSessionParticleSampleResult(
                species, "electrons", flattened);
        }, "a sample with an impossible dimension was accepted");

        // A shape that contradicts the catalog entry, in each field that could
        // be reported back to the user as the species' own.
        auto reshaped = sample;
        reshaped.species.dimension = 2;
        requireRejected([&] {
            validateSessionParticleSampleResult(
                species, "electrons", reshaped);
        }, "a sample of another dimension than the catalog was accepted");

        reshaped = sample;
        reshaped.species.precision = ParticleRealPrecision::Single;
        requireRejected([&] {
            validateSessionParticleSampleResult(
                species, "electrons", reshaped);
        }, "a sample of another precision than the catalog was accepted");

        reshaped = sample;
        reshaped.species.realComponentCount = 7;
        requireRejected([&] {
            validateSessionParticleSampleResult(
                species, "electrons", reshaped);
        }, "a sample with another component count was accepted");

        reshaped = sample;
        reshaped.species.particleCount = 1000;
        requireRejected([&] {
            validateSessionParticleSampleResult(
                species, "electrons", reshaped);
        }, "a sample inflating the species particle count was accepted");

        auto overfull = sample;
        overfull.points.push_back({2, Real3{{0.25, 0.25, 0.25}}});
        overfull.points.push_back({3, Real3{{0.75, 0.75, 0.75}}});
        requireRejected([&] {
            validateSessionParticleSampleResult(
                species, "electrons", overfull);
        }, "a sample larger than its species was accepted");
    }

    // Volume requests: the structural validator plus what the catalog knows
    // (field, level, component, the region inside the domain), and frames:
    // the requested size, a usable range that honours the request, and
    // metrics the dataset and budget could have produced.
    {
        const auto metadata = dataset(3);
        const DatasetId id{1};
        VolumeRenderRequest request;
        request.dataset = id;
        request.field = FieldId{0};
        request.maximumLevel = 1;
        request.region = RealBox{Real3{{0.0, 0.0, 0.0}}, Real3{{4.0, 4.0, 4.0}}};
        request.outputSize = {16, 12};
        request.transfer.colors = {0x0U, 0xFFFFFFU};
        request.transfer.opacities = {0.0F, 1.0F};
        requireAccepted([&] {
            validateSessionVolumeRequest(metadata, id, request);
        }, "a well-formed volume request was rejected");
        requireRejectedWith([&] {
            validateSessionVolumeRequest(metadata, DatasetId{2}, request);
        }, "wrong dataset", "a volume request for another dataset was accepted");
        requireRejectedWith([&] {
            validateSessionVolumeRequest(dataset(2), id, request);
        }, "3-D", "a volume request over a 2-D dataset was accepted");
        {
            auto bad = request;
            bad.field = FieldId{3};
            requireRejectedWith([&] {
                validateSessionVolumeRequest(metadata, id, bad);
            }, "field", "an unknown volume field was accepted");
            bad = request;
            bad.maximumLevel = 2;
            requireRejectedWith([&] {
                validateSessionVolumeRequest(metadata, id, bad);
            }, "level", "a volume level past the finest was accepted");
            bad = request;
            bad.component = 1;
            requireRejectedWith([&] {
                validateSessionVolumeRequest(metadata, id, bad);
            }, "component", "a missing volume component was accepted");
            // A volume region may reach past the domain, exactly as a view
            // region may (see the walk above): VolumeQuery fills the part
            // with no data with NaN and the renderer treats it as
            // transparent, so refusing here would throw away a rubber-band
            // selection that renders perfectly well as a slice.
            bad = request;
            bad.region.upper[2] = 4.5;
            requireAccepted([&] {
                validateSessionVolumeRequest(metadata, id, bad);
            }, "a volume region past the domain was rejected");
            bad = request;
            bad.region.upper[2] = 4.0 + 1.0e-12;   // a rounding hair: fine
            requireAccepted([&] {
                validateSessionVolumeRequest(metadata, id, bad);
            }, "a region a rounding hair past the domain was rejected");
            bad = request;
            bad.outputSize = {0, 12};
            requireRejected([&] {
                validateSessionVolumeRequest(metadata, id, bad);
            }, "a structurally invalid volume request was accepted");
        }

        VolumeFrame frame;
        frame.width = 16;
        frame.height = 12;
        frame.pixels.assign(16 * 12, 0U);
        frame.usedRange = {0.0, 1.0, {amrvis::ColorScale::Linear}};
        frame.metrics.gridDims = {4, 4, 4};
        frame.metrics.coveredVoxels = 64;
        frame.metrics.sampledMaximumLevel = 1;
        requireAccepted([&] {
            validateSessionVolumeResult(metadata, request, frame);
        }, "a well-formed volume frame was rejected");
        {
            auto bad = frame;
            bad.width = 15;
            requireRejectedWith([&] {
                validateSessionVolumeResult(metadata, request, bad);
            }, "requested size", "a frame of another size was accepted");
            bad = frame;
            bad.pixels.pop_back();
            requireRejectedWith([&] {
                validateSessionVolumeResult(metadata, request, bad);
            }, "storage", "a frame with short pixel storage was accepted");
            bad = frame;
            bad.usedRange = {1.0, 1.0, {amrvis::ColorScale::Linear}};
            requireRejectedWith([&] {
                validateSessionVolumeResult(metadata, request, bad);
            }, "unusable range", "a frame with an empty range was accepted");
            bad = frame;
            bad.usedRange = {0.0, 1.0, {amrvis::ColorScale::Logarithmic}};
            requireRejectedWith([&] {
                validateSessionVolumeResult(metadata, request, bad);
            }, "unusable range", "a non-positive logarithmic range was accepted");
            bad = frame;
            bad.usedRange = {0.5, 2.0, {amrvis::ColorScale::Logarithmic}};
            requireRejectedWith([&] {
                validateSessionVolumeResult(metadata, request, bad);
            }, "not requested", "an unrequested logarithmic range was accepted");
            auto logarithmic = request;
            logarithmic.scale = {ColorScale::Logarithmic};
            requireAccepted([&] { validateSessionVolumeResult(metadata, logarithmic, bad); },
                            "a requested Visible logarithmic response was rejected");
            requireAccepted([&] { validateSessionVolumeResult(metadata, logarithmic, frame); },
                            "a Visible logarithmic request could not fall back to linear");
            auto ranged = request;
            ranged.range = VolumeRange{0.25, 0.75, {amrvis::ColorScale::Linear}};
            requireRejectedWith([&] {
                validateSessionVolumeResult(metadata, ranged, frame);
            }, "requested range", "a frame ignoring the explicit range was accepted");
            bad = frame;
            bad.usedRange = *ranged.range;
            requireAccepted([&] {
                validateSessionVolumeResult(metadata, ranged, bad);
            }, "a frame honouring the explicit range was rejected");
            ranged.range->scale = {ColorScale::Linear, 0.01};
            requireAccepted([&] {
                validateSessionVolumeResult(metadata, ranged, bad);
            }, "an inactive threshold caused a valid linear response to be rejected");
            bad = frame;
            bad.metrics.gridDims = {0, 4, 4};
            requireRejectedWith([&] {
                validateSessionVolumeResult(metadata, request, bad);
            }, "empty sampled grid", "an empty grid was accepted");
            auto small = request;
            small.maximumVoxels = 32;
            requireRejectedWith([&] {
                validateSessionVolumeResult(metadata, small, frame);
            }, "voxel budget", "a grid over the voxel budget was accepted");
            bad = frame;
            bad.metrics.coveredVoxels = 65;
            requireRejectedWith([&] {
                validateSessionVolumeResult(metadata, request, bad);
            }, "covered voxels", "an over-covered grid was accepted");
            bad = frame;
            bad.metrics.sampledMaximumLevel = 2;
            requireRejectedWith([&] {
                validateSessionVolumeResult(metadata, request, bad);
            }, "sampled level", "a sampled level past the request was accepted");
            bad = frame;
            bad.cacheFallbackFromLevel = 1;
            requireRejectedWith([&] {
                validateSessionVolumeResult(metadata, request, bad);
            }, "half-specified", "a half-specified fallback was accepted");
            bad.cacheFallbackToLevel = 1;
            requireRejectedWith([&] {
                validateSessionVolumeResult(metadata, request, bad);
            }, "impossible cache fallback", "a non-descending fallback was accepted");
            // A frame that fell back to level 0 cannot also report having
            // sampled level 1: it was rendered with maximumLevel == 0, so
            // nothing finer can have put a value in its grid.
            bad.cacheFallbackToLevel = 0;
            requireRejectedWith([&] {
                validateSessionVolumeResult(metadata, request, bad);
            }, "impossible cache fallback",
                "a sampled level finer than the fallback was accepted");
            bad.metrics.sampledMaximumLevel = 0;
            requireAccepted([&] {
                validateSessionVolumeResult(metadata, request, bad);
            }, "a fallback from level 1 to 0 was rejected");

            // Dims whose product wraps 64 bits: unsaturated it comes out as
            // zero, which is under every budget and holds every coverage
            // count. This validates a peer's numbers, so it is the one place
            // the wrap is reachable from outside.
            bad = frame;
            bad.metrics.gridDims = {4194304, 2097152, 2097152};
            bad.metrics.coveredVoxels = 0;
            requireRejectedWith([&] {
                validateSessionVolumeResult(metadata, request, bad);
            }, "voxel budget", "grid dims whose product wraps were accepted");
        }
    }

    // --- the Visible range rule, which the slice path has to agree with ---
    {
        const auto gridOf = [](std::vector<float> values) {
            amrvis::VolumeGrid grid;
            grid.dims = {static_cast<int>(values.size()), 1, 1};
            grid.region.lower = {{0.0, 0.0, 0.0}};
            grid.region.upper = {{1.0, 1.0, 1.0}};
            grid.coveredVoxels = values.size();
            grid.values = std::move(values);
            return grid;
        };
        // Mixed signs with a logarithmic request: the negatives are part of
        // the field, so the range spans them and the mapping falls back to
        // linear -- what resolveRange does for the same data on a plane.
        // Deciding from the positive values alone would answer "logarithmic
        // over [2, 2]" and make every negative voxel vanish.
        const auto mixed =
            amrvis::visibleVolumeRange(gridOf({-1.0F, 2.0F}), {amrvis::ColorScale::Logarithmic});
        require(mixed.scale.scale != amrvis::ColorScale::Logarithmic && mixed.minimum <= -1.0 &&
                    mixed.maximum >= 2.0,
                "a mixed-sign Visible range went logarithmic");
        // Strictly positive: logarithmic is viable and is used.
        const auto positive =
            amrvis::visibleVolumeRange(gridOf({1.0F, 100.0F}), {amrvis::ColorScale::Logarithmic});
        require((positive.scale.scale == amrvis::ColorScale::Logarithmic) &&
                    positive.minimum > 0.0 && positive.maximum >= 100.0,
                "a positive Visible range did not go logarithmic");
        // The same grid without the logarithmic request stays linear.
        require(amrvis::visibleVolumeRange(gridOf({1.0F, 100.0F}), {amrvis::ColorScale::Linear})
                        .scale.scale == amrvis::ColorScale::Linear,
                "a linear Visible request came back logarithmic");
        // Degenerate: padded either side rather than left empty.
        const auto degenerate =
            amrvis::visibleVolumeRange(gridOf({2.0F, 2.0F}), {amrvis::ColorScale::Linear});
        require(degenerate.minimum < 2.0 && degenerate.maximum > 2.0,
            "a degenerate Visible range was not padded");
        // Nothing finite: a neutral range that keeps the requested mapping.
        const auto quietNaN = std::numeric_limits<float>::quiet_NaN();
        require(amrvis::visibleVolumeRange(gridOf({quietNaN}), {amrvis::ColorScale::Logarithmic})
                            .scale.scale == amrvis::ColorScale::Logarithmic &&
                    amrvis::visibleVolumeRange(gridOf({quietNaN}), {amrvis::ColorScale::Linear})
                            .scale.scale == amrvis::ColorScale::Linear,
                "an all-NaN grid did not fall back to a neutral range");
        // The scan honours the token: it walks the whole grid otherwise.
        amrvis::StopSource stop;
        stop.request_stop();
        bool cancelled = false;
        try {
            static_cast<void>(amrvis::visibleVolumeRange(
                gridOf({1.0F, 2.0F}), {amrvis::ColorScale::Linear}, stop.get_token()));
        } catch (const amrvis::ReadCancelled&) {
            cancelled = true;
        }
        require(cancelled, "a cancelled Visible range scan did not throw");
    }
    {
        // The derived-field half of an open reply. The catalog is larger than
        // the request in every case below and each refusal is matched on its
        // own message, so a call site that swapped fieldCount for
        // requestedCount fails here instead of passing silently.
        const std::vector<amrvis::DerivedFieldSkip> noSkips;
        const std::vector<amrvis::DerivedFieldSkip> oneSkip{
            {1, "twice", "no field or coordinate is named 'speed'"}};
        requireAccepted([&] {
            amrvis::validateSessionOpenedDerivedFields({
                .fieldCount = 5,
                .derivedFieldCount = 1,
                .skips = oneSkip,
                .requestedCount = 2,
            });
        }, "a well-formed derived-field reply was rejected");
        requireRejectedWith([&] {
            amrvis::validateSessionOpenedDerivedFields({
                .fieldCount = 1,
                .derivedFieldCount = 2,
                .skips = noSkips,
                .requestedCount = 2,
            });
        }, "than it has fields",
            "a catalog claiming more derived fields than fields was accepted");
        requireRejectedWith([&] {
            amrvis::validateSessionOpenedDerivedFields({
                .fieldCount = 5,
                .derivedFieldCount = 3,
                .skips = noSkips,
                .requestedCount = 2,
            });
        }, "reports more derived fields",
            "a reply installing more definitions than were sent was accepted");
        const std::vector<amrvis::DerivedFieldSkip> threeSkips{
            {0, "a", "why"}, {1, "b", "why"}, {2, "c", "why"}};
        requireRejectedWith([&] {
            amrvis::validateSessionOpenedDerivedFields({
                .fieldCount = 5,
                .derivedFieldCount = 0,
                .skips = threeSkips,
                .requestedCount = 2,
            });
        }, "skips more definitions",
            "a reply skipping more definitions than were sent was accepted");
        // The sum, which is the only check the two above cannot stand in for:
        // neither count alone is past the request.
        const std::vector<amrvis::DerivedFieldSkip> twoSkips{
            {0, "a", "why"}, {1, "b", "why"}};
        requireRejectedWith([&] {
            amrvis::validateSessionOpenedDerivedFields({
                .fieldCount = 5,
                .derivedFieldCount = 2,
                .skips = twoSkips,
                .requestedCount = 3,
            });
        }, "installs and skips",
            "a reply accounting for more definitions than were sent was "
            "accepted");
        // Two skips naming the same definition are accepted on purpose: a
        // peer that leaves definition_index at its default sends all zeroes,
        // and nothing reads the field. See the note in the validator.
        const std::vector<amrvis::DerivedFieldSkip> twiceSkipped{
            {0, "a", "why"}, {0, "a", "why again"}};
        requireAccepted([&] {
            amrvis::validateSessionOpenedDerivedFields({
                .fieldCount = 5,
                .derivedFieldCount = 0,
                .skips = twiceSkipped,
                .requestedCount = 3,
            });
        }, "a peer that left definition_index at its default was refused");
        const std::vector<amrvis::DerivedFieldSkip> straySkip{
            {7, "stray", "why"}};
        requireRejectedWith([&] {
            amrvis::validateSessionOpenedDerivedFields({
                .fieldCount = 5,
                .derivedFieldCount = 0,
                .skips = straySkip,
                .requestedCount = 2,
            });
        }, "was not requested",
            "a skip naming a definition that was never sent was accepted");
        // A peer that installed none is not a peer that lied: the reply a
        // pre-1.4 server sends decodes to zeroes, and an empty request is
        // answered the same way.
        requireAccepted([&] {
            amrvis::validateSessionOpenedDerivedFields({
                .fieldCount = 5,
                .derivedFieldCount = 0,
                .skips = noSkips,
                .requestedCount = 0,
            });
        }, "a reply carrying no derived fields was rejected");
    }
    return 0;
}
