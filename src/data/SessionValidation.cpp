#include <amrexplorer/data/SessionValidation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <type_traits>
#include <variant>
#include <vector>

namespace amrvis {
namespace {

void requireFieldAndLevel(const DatasetMetadata& metadata, FieldId field,
    int maximumLevel, const char* operation)
{
    if (field.value >= metadata.fields.size()) {
        throw std::invalid_argument(
            std::string(operation) + " field is unavailable");
    }
    if (maximumLevel < 0 || maximumLevel > metadata.finestLevel) {
        throw std::invalid_argument(
            std::string(operation) + " level is unavailable");
    }
}

void requireComponent(const DatasetMetadata& metadata, FieldId field,
    int component, const char* operation)
{
    if (component < 0) {
        throw std::invalid_argument(
            std::string(operation) + " component is unavailable");
    }
    const auto& components
        = metadata.fields[static_cast<std::size_t>(field.value)]
              .componentNames;
    const auto count = std::max<std::size_t>(1, components.size());
    if (static_cast<std::size_t>(component) >= count) {
        throw std::invalid_argument(
            std::string(operation) + " component is unavailable");
    }
}

// A plane region is only meaningful in its two in-plane axes: the third axis of
// a 3-D region carries the slice position and is free to be degenerate.
void requirePlaneRegion(const RealBox& region, int dimension, int normalAxis,
    const char* what)
{
    for (const auto axis : planeAxes(dimension, normalAxis)) {
        const auto entry = static_cast<std::size_t>(axis);
        const auto lower = region.lower[entry];
        const auto upper = region.upper[entry];
        if (!std::isfinite(lower) || !std::isfinite(upper)
            || !(lower < upper)) {
            throw std::invalid_argument(std::string(what)
                + " must have finite positive in-plane extent");
        }
    }
}

// -1 marks a sample no level covered; anything else must name a real level.
void requireSourceLevels(const std::vector<std::int16_t>& levels,
    int finestLevel, const char* what)
{
    for (const auto level : levels) {
        if (level < -1 || level > finestLevel) {
            throw std::invalid_argument(
                std::string(what) + " reports a source level the dataset "
                                    "does not have");
        }
    }
}

} // namespace

void validateSessionViewRequest(const DatasetMetadata& metadata,
    DatasetId dataset, const ViewDataRequest& request)
{
    std::visit(
        [&](const auto& typed) {
            using Request = std::decay_t<decltype(typed)>;
            const auto& query = [&]() -> const auto& {
                if constexpr (std::is_same_v<Request, SliceRequest>) {
                    return typed;
                } else {
                    return typed.query;
                }
            }();
            if (query.dataset != dataset) {
                throw std::invalid_argument(
                    "view request uses the wrong dataset");
            }
            requireFieldAndLevel(
                metadata, query.field, query.maximumLevel, "view");
            requireComponent(
                metadata, query.field, query.component, "view");
            if constexpr (std::is_same_v<Request, SliceRequest>) {
                const auto errors
                    = validateSliceRequest(typed, metadata.dimension);
                if (!errors.empty()) {
                    throw std::invalid_argument(errors.front());
                }
            } else {
                const auto errors
                    = validateLineRequest(typed.query, metadata.dimension);
                if (!errors.empty()) {
                    throw std::invalid_argument(errors.front());
                }
                if (typed.outputWidth < 1) {
                    throw std::invalid_argument(
                        "line output width must be positive");
                }
            }
        },
        request);
}

void validateSessionDatasetPageRequest(const DatasetMetadata& metadata,
    DatasetId dataset, const DatasetPageRequest& request)
{
    if (request.dataset != dataset) {
        throw std::invalid_argument("dataset page uses the wrong dataset");
    }
    if (metadata.dimension < 2 || metadata.dimension > 3) {
        throw std::invalid_argument(
            "dataset page requires a 2-D or 3-D dataset");
    }
    if (request.level < 0
        || request.level >= static_cast<int>(metadata.levels.size())) {
        throw std::invalid_argument("dataset page level is unavailable");
    }
    if (request.field.value >= metadata.fields.size()) {
        throw std::invalid_argument("dataset page field is unavailable");
    }
    if (metadata.dimension == 3
        && (request.normalAxis < 0 || request.normalAxis > 2)) {
        throw std::invalid_argument("dataset page normal axis is invalid");
    }
    if (request.maximumExtent < 1
        || request.maximumExtent > datasetPageMaxExtent) {
        throw std::invalid_argument(
            "dataset page extent is outside its limit");
    }
    if (!std::isfinite(request.slicePosition)) {
        throw std::invalid_argument(
            "dataset page slice position must be finite");
    }
    // extractDatasetPage applies the same rule, but only once the request has
    // already reached it. Checking here rejects a malformed region at the trust
    // boundary, and lets the remote client refuse one before a round trip.
    requirePlaneRegion(request.region, metadata.dimension, request.normalAxis,
        "dataset page region");
    // The builder turns these coordinates into indices, and a coordinate too far
    // out to be one makes it throw. Refusing here means no page can ever be a
    // legitimate answer to such a request, which is what lets the result
    // validator treat an unconvertible request as the peer's fault.
    const auto& level
        = metadata.levels[static_cast<std::size_t>(request.level)];
    try {
        for (const auto axis : planeAxes(metadata.dimension,
                 request.normalAxis)) {
            const auto entry = static_cast<std::size_t>(axis);
            static_cast<void>(
                sampleIndex(level, axis, request.region.lower[entry]));
            static_cast<void>(sampleIndex(level, axis,
                std::nextafter(request.region.upper[entry],
                    -std::numeric_limits<double>::infinity())));
        }
        if (metadata.dimension == 3) {
            static_cast<void>(sampleIndex(
                level, request.normalAxis, request.slicePosition));
        }
    } catch (const std::out_of_range&) {
        throw std::invalid_argument(
            "dataset page region is too far out to index at this level");
    }
}

void validateSessionRangeRequest(
    const DatasetMetadata& metadata, const RangeRequest& request)
{
    requireFieldAndLevel(
        metadata, request.field, request.maximumLevel, "range");
    if (request.composition != CompositionPolicy::FinestAvailable
        && request.composition != CompositionPolicy::ExactLevel) {
        throw std::invalid_argument("range composition policy is invalid");
    }
    if (request.scope != RangeScope::File
        && request.scope != RangeScope::Level) {
        throw std::invalid_argument("range scope is invalid");
    }
}

void validateSessionParticleRequest(const DatasetMetadata& metadata,
    const std::vector<ParticleSpeciesMetadata>& species,
    const std::string& name, double fraction)
{
    static_cast<void>(metadata);
    if (name.empty()
        || std::none_of(species.begin(), species.end(),
            [&](const auto& entry) { return entry.name == name; })) {
        throw std::invalid_argument("particle species is unavailable");
    }
    if (!std::isfinite(fraction) || !(fraction > 0.0) || fraction > 1.0) {
        throw std::invalid_argument(
            "particle sample fraction must be in (0, 1]");
    }
}

void validateSessionVolumeRequest(const DatasetMetadata& metadata,
    DatasetId dataset, const VolumeRenderRequest& request)
{
    if (request.dataset != dataset) {
        throw std::invalid_argument("volume request uses the wrong dataset");
    }
    requireFieldAndLevel(metadata, request.field, request.maximumLevel, "volume");
    requireComponent(metadata, request.field, request.component, "volume");
    const auto errors = validateVolumeRenderRequest(request, metadata.dimension);
    if (!errors.empty()) {
        throw std::invalid_argument(errors.front());
    }
    // No containment test. VolumeQuery states that "a region reaching past
    // the domain is not refused. The grid already says 'no data here' with
    // NaN", and validateSessionViewRequest does not refuse one either -- so
    // refusing here would throw away a rubber-band selection that renders
    // perfectly well as a slice, and the relative tolerance it needed
    // collapsed to zero on an axis whose domain extent is degenerate.
    // validateVolumeRenderRequest above already bounds the region's own
    // shape, and volumeGridDims bounds the grid it turns into.
}

void validateSessionVolumeResult(const DatasetMetadata& metadata,
    const VolumeRenderRequest& request, const VolumeFrame& frame)
{
    if (frame.width != request.outputSize[0]
        || frame.height != request.outputSize[1]) {
        throw std::invalid_argument(
            "volume frame is not the requested size");
    }
    if (frame.pixels.size() != static_cast<std::size_t>(frame.width)
            * static_cast<std::size_t>(frame.height)) {
        throw std::invalid_argument(
            "volume frame pixel storage does not match its size");
    }
    const auto& used = frame.usedRange;
    if (!std::isfinite(used.minimum) || !std::isfinite(used.maximum)
        || !(used.minimum < used.maximum)
        || (used.logarithmic && !(used.minimum > 0.0))) {
        throw std::invalid_argument("volume frame reports an unusable range");
    }
    if (request.range && !(used == *request.range)) {
        throw std::invalid_argument(
            "volume frame did not use the requested range");
    }
    if (!request.range && used.logarithmic && !request.logarithmic) {
        throw std::invalid_argument(
            "volume frame used a logarithmic range that was not requested");
    }
    const auto& metrics = frame.metrics;
    for (const auto extent : metrics.gridDims) {
        if (extent < 1) {
            throw std::invalid_argument(
                "volume frame reports an empty sampled grid");
        }
    }
    // Saturating, not a plain product: this validates a peer's numbers, and
    // dims whose product wraps 64 bits (say {2^22, 2^21, 2^21}) would come
    // out as zero and pass both the budget and the coverage test below.
    const auto voxels = volumeVoxelCount(metrics.gridDims);
    if (voxels > request.maximumVoxels) {
        throw std::invalid_argument(
            "volume frame reports a grid over the requested voxel budget");
    }
    if (metrics.coveredVoxels > voxels) {
        throw std::invalid_argument(
            "volume frame reports more covered voxels than the grid holds");
    }
    const auto highest = std::min(request.maximumLevel, metadata.finestLevel);
    if (metrics.sampledMaximumLevel < 0
        || metrics.sampledMaximumLevel > highest) {
        throw std::invalid_argument(
            "volume frame reports a sampled level the request did not allow");
    }
    const auto from = frame.cacheFallbackFromLevel;
    const auto to = frame.cacheFallbackToLevel;
    if ((from < 0) != (to < 0)) {
        throw std::invalid_argument(
            "volume frame reports a half-specified cache fallback");
    }
    // No `to < 0` here: the half-specified test above already threw for
    // exactly that pair, so from >= 0 implies to >= 0 by this point.
    if (from >= 0
        && (from > highest || to >= from || metrics.sampledMaximumLevel > to)) {
        // The last clause: a frame that says it fell back to `to` was
        // rendered with maximumLevel == to, so nothing finer than `to` can
        // have put a value in its grid. A peer claiming otherwise is
        // describing a render that cannot have happened.
        throw std::invalid_argument(
            "volume frame reports an impossible cache fallback");
    }
}

void validateSessionViewResult(const DatasetMetadata& metadata,
    const ViewDataRequest& request, const ViewDataResult& result)
{
    if (request.index() != result.index()) {
        throw std::invalid_argument(
            "view result does not answer the request that was made");
    }
    if (const auto* slice = std::get_if<SliceQueryResult>(&result)) {
        const auto& query = std::get<SliceRequest>(request);
        const auto& plane = slice->plane;
        if (plane.width < 1 || plane.height < 1) {
            throw std::invalid_argument("slice result has an empty raster");
        }
        // The raster and the window it covers are what the caller asked for,
        // exactly: a slice too large for one frame is refused rather than
        // reduced, and the query copies the requested region into its result.
        // A raster of another shape, or over another window, would be displayed
        // as if it answered the request.
        if (plane.width != query.outputSize[0]
            || plane.height != query.outputSize[1]) {
            throw std::invalid_argument(
                "slice result raster is not the size that was requested");
        }
        if (!(plane.physicalRegion == query.visibleRegion)) {
            throw std::invalid_argument(
                "slice result covers a different region than was requested");
        }
        const auto samples = static_cast<std::size_t>(plane.width)
            * static_cast<std::size_t>(plane.height);
        if (plane.values.size() != samples || plane.valid.size() != samples
            || plane.sourceLevel.size() != samples) {
            throw std::invalid_argument(
                "slice result raster and sample vectors disagree");
        }
        requirePlaneRegion(plane.physicalRegion, metadata.dimension,
            query.normalDirection, "slice result region");
        // Provenance is bounded by what the request allowed, not merely by what
        // the dataset has: a sample composited from a finer level than the
        // caller asked for is not the answer to that question.
        requireSourceLevels(plane.sourceLevel,
            std::min(query.maximumLevel, metadata.finestLevel),
            "slice result");
        // The overlay is switched by the request: the query copies the flag and
        // collects nothing when it is off, and the window installs whatever the
        // flag says arrived.
        if (slice->gridBoxesIncluded != query.includeGridBoxes) {
            // The query copies the flag from the request, so a disagreement is
            // never an answer to it -- and the window installs the overlay only
            // when the flag is set, keeping the previous one otherwise.
            throw std::invalid_argument(
                "slice result disagrees with the request about grid boxes");
        }
        if (!query.includeGridBoxes
            && (!slice->gridBoxes.empty() || slice->gridBoxesTruncated)) {
            throw std::invalid_argument(
                "slice result carries grid boxes that were not requested");
        }
        // Expected boxes per level, built once and reused: each catalog box of
        // that level, in physical space, clipped to the visible region on the
        // plane axes -- exactly the geometry the query derives. The
        // slice-intersection and non-degeneracy filters it also applies are
        // deliberately *not* mirrored, so this stays a superset: it can only
        // reject a box that corresponds to no catalog box at all, never one the
        // query legitimately chose to keep or drop.
        std::vector<std::pair<int, std::vector<RealBox>>> expectedByLevel;
        const auto expectedFor = [&](int level) -> const std::vector<RealBox>& {
            const auto known = std::find_if(expectedByLevel.begin(),
                expectedByLevel.end(),
                [level](const auto& entry) { return entry.first == level; });
            if (known != expectedByLevel.end()) {
                return known->second;
            }
            std::vector<RealBox> boxes;
            const auto& source
                = metadata.levels[static_cast<std::size_t>(level)];
            boxes.reserve(source.boxes.size());
            for (const auto& indexBox : source.boxes) {
                auto physical
                    = sampleBounds(source, indexBox, metadata.dimension);
                for (const auto axis : planeAxes(
                         metadata.dimension, query.normalDirection)) {
                    const auto entry = static_cast<std::size_t>(axis);
                    physical.lower[entry] = std::max(physical.lower[entry],
                        query.visibleRegion.lower[entry]);
                    physical.upper[entry] = std::min(physical.upper[entry],
                        query.visibleRegion.upper[entry]);
                }
                boxes.push_back(physical);
            }
            std::sort(boxes.begin(), boxes.end(),
                [](const RealBox& left, const RealBox& right) {
                    return left.lower.values < right.lower.values
                        || (left.lower.values == right.lower.values
                            && left.upper.values < right.upper.values);
                });
            expectedByLevel.emplace_back(level, std::move(boxes));
            return expectedByLevel.back().second;
        };
        for (const auto& box : slice->gridBoxes) {
            // A grid box, unlike a sample, always comes from a real level:
            // there is no sentinel for "no level drew this box".
            if (box.level < 0
                || box.level > std::min(
                       query.maximumLevel, metadata.finestLevel)) {
                throw std::invalid_argument(
                    "slice result grid box names a level the dataset does "
                    "not have");
            }
            requirePlaneRegion(box.physicalRegion, metadata.dimension,
                query.normalDirection, "slice result grid box");
            const auto& expected = expectedFor(box.level);
            const auto found = std::lower_bound(expected.begin(),
                expected.end(), box.physicalRegion,
                [](const RealBox& left, const RealBox& right) {
                    return left.lower.values < right.lower.values
                        || (left.lower.values == right.lower.values
                            && left.upper.values < right.upper.values);
                });
            if (found == expected.end()
                || !(*found == box.physicalRegion)) {
                throw std::invalid_argument(
                    "slice result grid box matches no box in the catalog");
            }
        }
        return;
    }
    const auto& view = std::get<LineViewRequest>(request);
    const auto& line = std::get<LineQueryResult>(result).line;
    const auto& query = view.query;
    if (line.axis != query.axis) {
        throw std::invalid_argument("line result is along the wrong axis");
    }
    if (line.values.size() != line.positions.size()
        || line.valid.size() != line.positions.size()
        || line.sourceLevel.size() != line.positions.size()) {
        throw std::invalid_argument("line result vectors disagree");
    }
    // boundLineToViewport's contract: at most two samples per output pixel, the
    // extrema of each bucket. A longer line would be plotted at a density the
    // caller never asked to receive.
    if (view.outputWidth >= 1
        && line.positions.size()
            > static_cast<std::size_t>(view.outputWidth) * 2) {
        throw std::invalid_argument(
            "line result carries more samples than its viewport allows");
    }
    requireSourceLevels(line.sourceLevel,
        std::min(query.maximumLevel, metadata.finestLevel), "line result");
    // Whether the horizontal axis is physical or an index is a property of the
    // dataset, not of the answer: the plot labels and scales its axis from this.
    if (line.positionsAreIndices == metadata.hasPhysicalGeometry) {
        throw std::invalid_argument(
            "line result disagrees with the dataset about index positions");
    }
    if (metadata.levels.empty()) {
        return;
    }
    // Sorted, not strictly sorted. The walk emits one sample per cell in
    // increasing order and boundLineToViewport appends each bucket's picks in
    // index order, so non-decreasing is certain; strict increase across a level
    // transition, where a coarse cell can share a centre with a fine one under
    // an odd refinement ratio, is not something the producer guarantees on paper.
    // A duplicated position plots as a vertical step, which is cosmetic; a false
    // rejection here retires the connection, which is not.
    if (!std::is_sorted(line.positions.begin(), line.positions.end())) {
        throw std::invalid_argument("line result positions are not ordered");
    }
    // Positions lie in the extent LineQuery walks, which is the requested region
    // when there is one and the sampling level's span otherwise -- with the
    // producer's own end tolerance. Note that a region is free to reach outside
    // the domain: the walk then emits invalid samples out there, and bounding
    // this by the domain instead would reject a legitimate answer.
    const auto& level = metadata.levels[static_cast<std::size_t>(
        std::clamp(query.maximumLevel, 0, metadata.finestLevel))];
    const auto axis = static_cast<std::size_t>(
        std::clamp(query.axis, 0, metadata.dimension - 1));
    const auto bounds = sampleBounds(level, level.domain, metadata.dimension);
    const auto lowest = query.region ? query.region->lower[axis]
                                     : bounds.lower[axis];
    const auto highest = query.region ? query.region->upper[axis]
                                      : bounds.upper[axis];
    const auto tolerance = 1.0e-9 * level.cellSize[axis];
    for (const auto position : line.positions) {
        if (!std::isfinite(position)) {
            throw std::invalid_argument("line result position is not finite");
        }
        auto centre = position;
        if (line.positionsAreIndices) {
            // The producer reports an int index widened to a double, so a value
            // that is not integral is not an index at all -- 1.5 would otherwise
            // be read as index 1 -- and one outside int's range cannot be
            // converted back at all: narrowing 1e100 to int is undefined, which
            // is the opposite of what a validation layer is for. Both bounds are
            // exactly representable as doubles, so these comparisons are exact.
            if (position != std::floor(position)
                || position
                    < static_cast<double>(std::numeric_limits<int>::min())
                || position
                    > static_cast<double>(std::numeric_limits<int>::max())) {
                throw std::invalid_argument(
                    "line result index position is not an index");
            }
            // An index position is in range when the sample it names is: the
            // producer tests the centre, not the index. Index positions only
            // occur on a single-level dataset, so this level is the one that
            // emitted it. The axis is the clamped one, since samplePosition
            // indexes fixed three-element geometry with it.
            centre = samplePosition(level, static_cast<int>(axis),
                static_cast<int>(position));
        }
        if (centre < lowest - tolerance || centre > highest + tolerance) {
            throw std::invalid_argument(
                "line result position lies outside the requested extent");
        }
    }
}

void validateSessionDatasetPageResult(const DatasetMetadata& metadata,
    const DatasetPageRequest& request, const DatasetPage& page)
{
    if (page.nx < 0 || page.ny < 0) {
        throw std::invalid_argument("dataset page extent is negative");
    }
    // The client sizes its table from the extent it asked for; a page larger
    // than that would overrun what the caller prepared for.
    if (page.nx > request.maximumExtent || page.ny > request.maximumExtent) {
        throw std::invalid_argument(
            "dataset page is larger than the requested extent");
    }
    // An empty page is the default-constructed one, whose bounds are
    // deliberately inverted (lower {0, 0}, upper {-1, -1}) to say "no cells".
    // One empty axis and one populated one is neither shape.
    if ((page.nx == 0) != (page.ny == 0)) {
        throw std::invalid_argument(
            "dataset page is empty on one axis only");
    }
    if (request.level < 0
        || static_cast<std::size_t>(request.level) >= metadata.levels.size()) {
        throw std::invalid_argument(
            "dataset page names a level the dataset does not have");
    }
    const auto& level
        = metadata.levels[static_cast<std::size_t>(request.level)];
    const auto axes = planeAxes(metadata.dimension, request.normalAxis);
    // What extractDatasetPage would have produced for this request. Mirroring it
    // is the point: the window, the slice index, and the emptiness of a page are
    // all determined by the region, the slice position, the level domain, and
    // the extent limit, so anything else is not an answer to this request. Keep
    // this in step with extractDatasetPage.
    //
    // sampleIndex throws when a coordinate is too far out to be an index. The
    // server's own builder would have thrown first and answered with an error,
    // so a response that exists at all implies the derivation succeeds; if it
    // does not, leave the geometry unchecked rather than blame the peer.
    struct Window {
        std::int64_t lower = 0;
        std::int64_t upper = 0;
    };
    std::array<Window, 2> expected{};
    bool expectEmpty = false;
    try {
        for (std::size_t entry = 0; entry < 2; ++entry) {
            const auto axis = static_cast<std::size_t>(axes[entry]);
            const auto domainLower
                = static_cast<std::int64_t>(level.domain.lower[axis]);
            const auto domainUpper
                = static_cast<std::int64_t>(level.domain.upper[axis]);
            const auto rawLower = static_cast<std::int64_t>(
                sampleIndex(level, axes[entry], request.region.lower[axis]));
            const auto rawUpper = static_cast<std::int64_t>(
                sampleIndex(level, axes[entry],
                    std::nextafter(request.region.upper[axis],
                        -std::numeric_limits<double>::infinity())));
            if (rawUpper < domainLower || rawLower > domainUpper) {
                expectEmpty = true;
                break;
            }
            expected[entry].lower = std::max(rawLower, domainLower);
            expected[entry].upper = std::min(rawUpper, domainUpper);
        }
    } catch (const std::out_of_range&) {
        // The request validator refuses a region this far out, so the builder
        // could only have answered with an error: a page is impossible here.
        throw std::invalid_argument(
            "dataset page answers a request that cannot be indexed");
    }
    constexpr bool derived = true;
    if (derived && expectEmpty && (page.nx != 0 || page.ny != 0)) {
        throw std::invalid_argument(
            "dataset page is not empty although the request misses the level");
    }
    if (derived && !expectEmpty && page.nx == 0 && page.ny == 0) {
        throw std::invalid_argument(
            "dataset page is empty although the request meets the level");
    }
    if (page.nx > 0 && page.ny > 0) {
        for (std::size_t entry = 0; entry < page.lower.size(); ++entry) {
            // Widen before subtracting: both bounds come straight off the wire,
            // where INT_MIN and INT_MAX are reachable and int arithmetic on them
            // is undefined.
            const auto lower = static_cast<std::int64_t>(page.lower[entry]);
            const auto upper = static_cast<std::int64_t>(page.upper[entry]);
            const auto extent = static_cast<std::int64_t>(
                entry == 0 ? page.nx : page.ny);
            const auto truncated = entry == 0 ? page.truncatedX
                                              : page.truncatedY;
            if (lower > upper) {
                throw std::invalid_argument(
                    "dataset page index bounds are reversed");
            }
            if (upper - lower + 1 != extent) {
                throw std::invalid_argument(
                    "dataset page extent does not match its index bounds");
            }
            // The page indexes the level it named, so its cells have to be
            // inside that level's domain.
            const auto axis = static_cast<std::size_t>(axes[entry]);
            if (lower < static_cast<std::int64_t>(level.domain.lower[axis])
                || upper
                    > static_cast<std::int64_t>(level.domain.upper[axis])) {
                throw std::invalid_argument(
                    "dataset page lies outside the level domain");
            }
            // Truncation only ever moves the upper edge in, so the lower edge is
            // exactly the requested one and the extent stops at the limit.
            if (derived && !expectEmpty) {
                if (lower != expected[entry].lower) {
                    throw std::invalid_argument(
                        "dataset page does not start where the request does");
                }
                if (upper > expected[entry].upper) {
                    throw std::invalid_argument(
                        "dataset page reaches past the requested region");
                }
                const auto requestedSpan = expected[entry].upper
                    - expected[entry].lower + 1;
                const auto expectTruncation = requestedSpan
                    > static_cast<std::int64_t>(request.maximumExtent);
                if (truncated != expectTruncation) {
                    throw std::invalid_argument(
                        "dataset page truncation flag does not match the "
                        "requested span");
                }
                if (truncated
                    && extent
                        != static_cast<std::int64_t>(request.maximumExtent)) {
                    throw std::invalid_argument(
                        "dataset page claims truncation without filling the "
                        "extent limit");
                }
                if (!truncated && upper != expected[entry].upper) {
                    throw std::invalid_argument(
                        "dataset page is short of the requested region without "
                        "claiming truncation");
                }
            }
        }
        if (metadata.dimension == 3) {
            const auto normal = static_cast<std::size_t>(request.normalAxis);
            if (normal >= 3) {
                throw std::invalid_argument(
                    "dataset page normal axis is invalid");
            }
            if (page.sliceIndex < level.domain.lower[normal]
                || page.sliceIndex > level.domain.upper[normal]) {
                throw std::invalid_argument(
                    "dataset page slice index is outside the level domain");
            }
            // The slice index is the requested position clamped to the domain,
            // so it is determined rather than merely plausible.
            try {
                const auto raw = static_cast<std::int64_t>(sampleIndex(
                    level, request.normalAxis, request.slicePosition));
                const auto clamped = std::clamp(raw,
                    static_cast<std::int64_t>(level.domain.lower[normal]),
                    static_cast<std::int64_t>(level.domain.upper[normal]));
                if (static_cast<std::int64_t>(page.sliceIndex) != clamped) {
                    throw std::invalid_argument(
                        "dataset page slices at a different position than was "
                        "requested");
                }
            } catch (const std::out_of_range&) {
                // Unreachable through the server, which would have failed the
                // same conversion first.
            }
        }
    }
    const auto samples = static_cast<std::size_t>(page.nx)
        * static_cast<std::size_t>(page.ny);
    if (page.values.size() != samples || page.covered.size() != samples) {
        throw std::invalid_argument(
            "dataset page extent and sample vectors disagree");
    }
}

void validateSessionParticleSampleResult(
    const std::vector<ParticleSpeciesMetadata>& species,
    const std::string& requested, const ParticleSample& sample)
{
    if (sample.species.name != requested) {
        throw std::invalid_argument(
            "particle sample describes the wrong species");
    }
    const auto known = std::find_if(species.begin(), species.end(),
        [&](const auto& entry) { return entry.name == requested; });
    if (known == species.end()) {
        throw std::invalid_argument(
            "particle sample describes a species the catalog does not list");
    }
    if (sample.species.dimension < 1 || sample.species.dimension > 3) {
        throw std::invalid_argument(
            "particle sample dimension is outside [1, 3]");
    }
    // The catalog published this species when the dataset opened; a response
    // describing the same name with a different shape contradicts it, and the
    // client would then report the response's numbers as the species' own.
    if (!(sample.species == *known)) {
        throw std::invalid_argument(
            "particle sample metadata contradicts the catalog");
    }
    // A sample is a subset of the species, so it cannot hold more points than
    // the catalog says exist.
    if (sample.points.size() > known->particleCount) {
        throw std::invalid_argument(
            "particle sample holds more points than the species contains");
    }
}

void validateSessionOpenedDerivedFields(
    const SessionOpenedDerivedFields& reply)
{
    const auto derived = reply.derivedFieldCount;
    const auto requested = reply.requestedCount;
    if (derived > reply.fieldCount) {
        throw std::invalid_argument(
            "dataset catalog claims more derived fields than it has fields");
    }
    if (derived > requested) {
        throw std::invalid_argument(
            "dataset catalog reports more derived fields than were requested");
    }
    if (reply.skips.size() > requested) {
        throw std::invalid_argument(
            "dataset catalog skips more definitions than were requested");
    }
    // Every definition was either installed or skipped, so the two cannot add
    // up to more than were sent. Fewer is legal: nothing says a server must
    // account for each one separately, only that it cannot invent them.
    if (derived + reply.skips.size() > requested) {
        throw std::invalid_argument(
            "dataset catalog installs and skips more definitions than were "
            "requested");
    }
    for (const auto& skip : reply.skips) {
        if (skip.definitionIndex >= requested) {
            throw std::invalid_argument(
                "dataset catalog skips a definition that was not requested");
        }
    }
}

} // namespace amrvis
