#include <amrexplorer/core/Metadata.hpp>
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

amrvis::SliceQueryResult sliceResult(const amrvis::RealBox& region)
{
    amrvis::SliceQueryResult result;
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
                metadata, withoutView, sliceResult(region));
        }, "a slice without overlays was rejected");
        auto unrequested = sliceResult(region);
        unrequested.gridBoxesIncluded = true;
        requireRejected([&] {
            validateSessionViewResult(metadata, withoutView, unrequested);
        }, "an unrequested overlay flag was accepted");
        unrequested = sliceResult(region);
        unrequested.gridBoxes.push_back({1, region});
        requireRejected([&] {
            validateSessionViewResult(metadata, withoutView, unrequested);
        }, "an unrequested grid box was accepted");

        auto outside = sliceResult(region);
        auto beyondWindow = region;
        beyondWindow.lower[0] -= 1.0;
        outside.gridBoxes.push_back({1, beyondWindow});
        requireRejected([&] {
            validateSessionViewResult(metadata, view, outside);
        }, "a grid box outside the visible region was accepted");

        // A grid box may keep its own extent on the normal axis: only the plane
        // axes are clipped to the visible region.
        if (dimension == 3) {
            auto normalExtent = sliceResult(region);
            auto box = region;
            box.lower[2] -= 1.0;
            box.upper[2] += 1.0;
            normalExtent.gridBoxes.push_back({1, box});
            requireAccepted([&] {
                validateSessionViewResult(metadata, view, normalExtent);
            }, "a grid box with its own normal-axis extent was rejected");
        }

        // A line answer to a slice request is not an answer at all.
        requireRejected([&] {
            validateSessionViewResult(metadata, view, lineResult(0));
        }, "a line result answered a slice request");
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
    return 0;
}
