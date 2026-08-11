#include <amrexplorer/data/SessionValidation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
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
        if (!query.includeGridBoxes
            && (slice->gridBoxesIncluded || !slice->gridBoxes.empty()
                || slice->gridBoxesTruncated)) {
            throw std::invalid_argument(
                "slice result carries grid boxes that were not requested");
        }
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
            // Every box is clipped to the visible region on the plane axes
            // before it is collected, so one reaching outside it never came
            // from this request.
            for (const auto axis : planeAxes(
                     metadata.dimension, query.normalDirection)) {
                const auto entry = static_cast<std::size_t>(axis);
                if (box.physicalRegion.lower[entry]
                        < query.visibleRegion.lower[entry]
                    || box.physicalRegion.upper[entry]
                        > query.visibleRegion.upper[entry]) {
                    throw std::invalid_argument(
                        "slice result grid box lies outside the visible "
                        "region");
                }
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
    // The walk advances along the axis and emits at most one sample per index,
    // in order, and boundLineToViewport appends each bucket's picks in index
    // order too, so the positions arrive sorted. Unsorted ones would plot as a
    // line folded back over itself.
    if (!std::is_sorted(line.positions.begin(), line.positions.end())) {
        throw std::invalid_argument("line result positions are not ordered");
    }
    // And they lie in the extent the sampling level spans: physical bounds when
    // the dataset has geometry, index bounds when its positions are indices.
    // A region narrows this further; the level extent is the outer limit either
    // way, which is what makes this check independent of the region's own rule.
    const auto& level = metadata.levels[static_cast<std::size_t>(
        std::clamp(query.maximumLevel, 0, metadata.finestLevel))];
    const auto axis = static_cast<std::size_t>(
        std::clamp(query.axis, 0, metadata.dimension - 1));
    double lowest = 0.0;
    double highest = 0.0;
    if (line.positionsAreIndices) {
        lowest = static_cast<double>(level.domain.lower[axis]);
        highest = static_cast<double>(level.domain.upper[axis]);
    } else {
        const auto bounds
            = sampleBounds(level, level.domain, metadata.dimension);
        lowest = bounds.lower[axis];
        highest = bounds.upper[axis];
    }
    for (const auto position : line.positions) {
        if (!std::isfinite(position) || position < lowest
            || position > highest) {
            throw std::invalid_argument(
                "line result position lies outside the sampled level");
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
    bool derived = false;
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
        derived = true;
    } catch (const std::exception&) {
        derived = false;
    }
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

} // namespace amrvis
