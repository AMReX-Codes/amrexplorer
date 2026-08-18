#include <amrexplorer/pipeline/SlicePipeline.hpp>

#include <amrexplorer/cache/ByteLruCache.hpp>
#include <amrexplorer/data/LocalDatasetSession.hpp>
#include <amrexplorer/core/CoordinateSystem.hpp>
#include <amrexplorer/pipeline/DisplayCoordinator.hpp>
#include <amrexplorer/render2d/ScalarRenderer.hpp>
#include <amrexplorer/render2d/SphericalWarp.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace amrvis {
namespace {

SliceQueryResult requestSlice(DatasetSession& dataset,
    const SliceRequest& request, StopToken cancellation)
{
    return std::get<SliceQueryResult>(
        dataset.requestView(ViewDataRequest{request}, cancellation));
}

} // namespace

LevelSelection decodeLevelData(int data, int finestLevel)
{
    if (data >= kUpdateToLevelOffset) {
        return {CompositionPolicy::FinestAvailable, data - kUpdateToLevelOffset};
    }
    if (data < 0) {
        return {CompositionPolicy::FinestAvailable, finestLevel};
    }
    return {CompositionPolicy::ExactLevel, data};
}

std::array<int, 2> finestNativeOutputSize(
    const DatasetMetadata& metadata, const RealBox& region, int normal)
{
    const auto& finest = metadata.levels[static_cast<std::size_t>(
        std::max(0, metadata.finestLevel))];
    std::array<int, 2> axes{0, 1};
    if (metadata.dimension == 3) {
        std::size_t next = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (axis != normal) {
                axes[next++] = axis;
            }
        }
    }
    const auto cells = [&](int axis) {
        const auto i = static_cast<std::size_t>(axis);
        const auto extent = region.upper[i] - region.lower[i];
        return std::clamp(
            static_cast<int>(std::round(extent / finest.cellSize[i])),
            1, maxSliceOutputDimension);
    };
    return {cells(axes[0]), cells(axes[1])};
}

std::array<int, 2> viewportBoundedOutputSize(
    const DatasetMetadata& metadata, const RealBox& region, int normal,
    std::array<int, 2> viewportSize)
{
    viewportSize[0] = std::clamp(
        viewportSize[0], 1, maxSliceOutputDimension);
    viewportSize[1] = std::clamp(
        viewportSize[1], 1, maxSliceOutputDimension);
    const auto axes = slicePlaneAxes(metadata.dimension, normal);
    // The aspect is the region's extent in finest cells, not in physical
    // units: the display draws one square pixel per finest cell (that is
    // what finestNativeOutputSize and the view's logical size use), so a
    // raster fitted to the physical aspect would be squeezed whenever the
    // cells are not square -- to a one-pixel strip on a domain whose dy is
    // a hundred times its dx -- and, for spherical data, radius and angle
    // do not even share units. Fractional cell edges are kept: a rubber-band
    // region's exact aspect survives.
    const auto& finest = metadata.levels[static_cast<std::size_t>(
        std::max(0, metadata.finestLevel))];
    const auto extentX = (region.upper[static_cast<std::size_t>(axes[0])]
        - region.lower[static_cast<std::size_t>(axes[0])])
        / finest.cellSize[static_cast<std::size_t>(axes[0])];
    const auto extentY = (region.upper[static_cast<std::size_t>(axes[1])]
        - region.lower[static_cast<std::size_t>(axes[1])])
        / finest.cellSize[static_cast<std::size_t>(axes[1])];
    if (!(extentX > 0.0) || !(extentY > 0.0)) {
        return {1, 1};
    }
    const auto fit = std::min(
        static_cast<double>(viewportSize[0]) / extentX,
        static_cast<double>(viewportSize[1]) / extentY);
    return {
        std::clamp(static_cast<int>(std::lround(extentX * fit)),
            1, viewportSize[0]),
        std::clamp(static_cast<int>(std::lround(extentY * fit)),
            1, viewportSize[1])};
}

std::array<int, 2> nativeBoundedViewportOutputSize(
    const DatasetMetadata& metadata, const RealBox& region, int normal,
    std::array<int, 2> viewportSize)
{
    const auto viewport = viewportBoundedOutputSize(
        metadata, region, normal, viewportSize);
    const auto native = finestNativeOutputSize(metadata, region, normal);
    const auto scale = std::min({1.0,
        static_cast<double>(native[0]) / viewport[0],
        static_cast<double>(native[1]) / viewport[1]});
    return {
        std::clamp(static_cast<int>(std::lround(viewport[0] * scale)),
            1, native[0]),
        std::clamp(static_cast<int>(std::lround(viewport[1] * scale)),
            1, native[1])};
}

std::array<int, 2> frameBudgetBoundedOutputSize(
    std::array<int, 2> outputSize,
    std::optional<std::uint32_t> maximumResponseBytes)
{
    if (!maximumResponseBytes) {
        return outputSize;
    }
    const auto frameBytes = static_cast<std::uint64_t>(*maximumResponseBytes);
    const auto maximumCells = frameBytes > sliceResponseOverheadBytes
        ? (frameBytes - sliceResponseOverheadBytes) / sliceResponseBytesPerCell
        : 0;
    const auto requestedCells = static_cast<std::uint64_t>(outputSize[0])
        * static_cast<std::uint64_t>(outputSize[1]);
    if (maximumCells == 0) {
        return {1, 1};
    }
    if (requestedCells <= maximumCells) {
        return outputSize;
    }
    const auto scale = std::sqrt(static_cast<double>(maximumCells)
        / static_cast<double>(requestedCells));
    auto bounded = std::array<int, 2>{
        std::max(1, static_cast<int>(std::floor(outputSize[0] * scale))),
        std::max(1, static_cast<int>(std::floor(outputSize[1] * scale)))};
    while (static_cast<std::uint64_t>(bounded[0])
            * static_cast<std::uint64_t>(bounded[1]) > maximumCells) {
        if (bounded[0] >= bounded[1] && bounded[0] > 1) {
            --bounded[0];
        } else if (bounded[1] > 1) {
            --bounded[1];
        } else {
            break;
        }
    }
    return bounded;
}

bool sameSliceSpec(const SliceRequest& lhs, const SliceRequest& rhs)
{
    return lhs.dataset == rhs.dataset && lhs.field == rhs.field
        && lhs.component == rhs.component
        && lhs.normalDirection == rhs.normalDirection
        && lhs.physicalPosition == rhs.physicalPosition
        && lhs.visibleRegion == rhs.visibleRegion
        && lhs.maximumLevel == rhs.maximumLevel
        && lhs.outputSize == rhs.outputSize
        && lhs.sampling == rhs.sampling
        && lhs.composition == rhs.composition
        && lhs.includeGridBoxes == rhs.includeGridBoxes;
}

std::array<int, 2> slicePlaneAxes(int dimension, int normalDirection)
{
    if (dimension == 2) {
        return {0, 1};
    }
    std::array<int, 2> axes{};
    std::size_t next = 0;
    for (int axis = 0; axis < 3; ++axis) {
        if (axis != normalDirection) {
            axes[next++] = axis;
        }
    }
    return axes;
}

int coveredCells(const DatasetMetadata& metadata, int level,
    int axis, double lower, double upper)
{
    const auto& levelMetadata = metadata.levels[static_cast<std::size_t>(level)];
    const auto index = static_cast<std::size_t>(axis);
    const auto bounds = sampleBounds(
        levelMetadata, levelMetadata.domain, metadata.dimension);
    const auto lo = std::max(lower, bounds.lower[index]);
    const auto hi = std::min(upper, bounds.upper[index]);
    if (!(lo < hi)) {
        return 1;
    }
    const auto domainCells = static_cast<std::int64_t>(
        levelMetadata.domain.upper[index]) - levelMetadata.domain.lower[index];
    const auto first = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(sampleIndex(levelMetadata, axis, lo))
            - levelMetadata.domain.lower[index],
        0, domainCells);
    const auto last = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(sampleIndex(
            levelMetadata, axis, std::nextafter(hi, lo)))
            - levelMetadata.domain.lower[index],
        0, domainCells);
    return static_cast<int>(last - first + 1);
}

int sliceIndexForPosition(const DatasetMetadata& md, int level, int axis,
    double position)
{
    const auto& levelMd = md.levels[static_cast<std::size_t>(level)];
    return sampleIndex(levelMd, axis, position);
}

double positionForSliceIndex(const DatasetMetadata& md, int level, int axis,
    int index)
{
    const auto& levelMd = md.levels[static_cast<std::size_t>(level)];
    return samplePosition(levelMd, axis, index);
}

std::string cacheBudgetDescription(std::uint64_t bytes)
{
    constexpr std::uint64_t kibibyte = 1024;
    constexpr std::uint64_t mebibyte = 1024 * kibibyte;
    constexpr std::uint64_t gibibyte = 1024 * mebibyte;
    if (bytes % gibibyte == 0) {
        return std::to_string(bytes / gibibyte) + " GiB";
    }
    if (bytes % mebibyte == 0) {
        return std::to_string(bytes / mebibyte) + " MiB";
    }
    if (bytes % kibibyte == 0) {
        return std::to_string(bytes / kibibyte) + " KiB";
    }
    return std::to_string(bytes) + " bytes";
}

namespace {

// Records the dataset's coordinate system on the result and, for 2-D spherical
// data, replaces the logical (r, theta) raster with one warped into physical
// (R, Z) display space. Non-spherical data keeps its raster untouched and its
// display region equal to the plane's logical bounds. Safe to call when the
// raster was intentionally not rendered (contour-only refresh): the display
// region still updates from the plane's bounds.
void applyDisplayCoordinates(
    const DatasetMetadata& metadata, SliceDisplayResult& result)
{
    result.coordinateSystem = metadata.coordinateSystem;
    const auto& logical = result.displayPlane().physicalRegion;  // (r, theta)
    if (!isSpherical2D(metadata)) {
        result.displayRegion = logical;
        return;
    }
    result.sphericalDisplay = result.request.sphericalDisplay;
    const bool haveImage = result.image.width > 0 && result.image.height > 0
        && !result.image.rgba.empty();
    switch (result.request.sphericalDisplay) {
    case SphericalDisplay::RTheta:
        // Logical grid as-is: r horizontal, theta vertical (no warp).
        result.displayRegion = logical;
        break;
    case SphericalDisplay::ThetaR:
        // Logical grid transposed: theta horizontal, r vertical.
        result.displayRegion.lower[0] = logical.lower[1];
        result.displayRegion.upper[0] = logical.upper[1];
        result.displayRegion.lower[1] = logical.lower[0];
        result.displayRegion.upper[1] = logical.upper[0];
        if (haveImage) {
            result.image = transposeImage(result.image);
        }
        break;
    case SphericalDisplay::RZ:
    default:
        // Warped physical wedge (R, Z); the supersample factor applies here.
        result.displayRegion = sphericalDisplayBounds(logical);
        if (haveImage) {
            auto warped = warpSpherical(result.image, logical,
                maxSliceOutputDimension, result.request.sphericalSupersample);
            result.image = std::move(warped.image);
            result.displayRegion = warped.displayRegion;
        }
        break;
    }
}

} // namespace

SliceDisplayResult executeSlice(const std::shared_ptr<DatasetSession>& dataset,
    const SliceRequest& request,
    RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, const Palette& palette, StopToken cancellation)
{
    SliceDisplayResult result;
    result.request = request;
    result.slice = requestSlice(*dataset, request, cancellation);
    const auto range = resolveDisplayRange(dataset, request.field,
        request.maximumLevel, request.composition, rangeMode, userRange,
        logarithmic, result.displayPlane(), cancellation);
    result.minimum = range.minimum;
    result.maximum = range.maximum;
    result.logarithmic = range.logarithmic;
    result.fieldName = dataset->metadata().fields[request.field.value].name;
    result.image = renderScalarPlane(result.displayPlane(),
        ScalarRenderSettings{
            .minimum = range.minimum,
            .maximum = range.maximum,
            .logarithmic = range.logarithmic,
            .palette = &palette
        });
    applyDisplayCoordinates(dataset->metadata(), result);
    return result;
}

void appendVectorGlyphs(const std::shared_ptr<DatasetSession>& dataset,
    SliceRequest request, FieldId uField, FieldId vField, int count,
    StopToken cancellation, SliceDisplayResult& result)
{
    // The primary scalar request already carries the optional box list.
    // Auxiliary vector-component planes contribute values only.
    request.includeGridBoxes = false;
    request.field = uField;
    auto uSlice = requestSlice(*dataset, request, cancellation);
    request.field = vField;
    auto vSlice = requestSlice(*dataset, request, cancellation);
    // The warped R-Z spherical view anchors each glyph at its physical (R, Z)
    // position and rotates the components into display directions; the
    // executeSlice call preceding this one already populated
    // result.displayRegion with the sector bounds the segments map through.
    const bool sphericalRZ = isSpherical2D(dataset->metadata())
        && request.sphericalDisplay == SphericalDisplay::RZ;
    result.vectors = sphericalRZ
        ? generateSphericalRZVectorGlyphs(
              uSlice.plane, vSlice.plane, count, result.displayRegion)
        : generateVectorGlyphs(uSlice.plane, vSlice.plane, count);
    result.slice.metrics.candidateBlocks += uSlice.metrics.candidateBlocks
        + vSlice.metrics.candidateBlocks;
    result.slice.metrics.blocksRead += uSlice.metrics.blocksRead
        + vSlice.metrics.blocksRead;
    result.slice.metrics.cacheHits += uSlice.metrics.cacheHits
        + vSlice.metrics.cacheHits;
    result.slice.metrics.payloadBytesRead += uSlice.metrics.payloadBytesRead
        + vSlice.metrics.payloadBytesRead;
}

SliceDisplayResult executeSliceWithFallback(
    const std::shared_ptr<DatasetSession>& dataset, SliceRequest request,
    RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, const Palette& palette, DisplayMode displayMode,
    std::uint32_t vectorUField, std::uint32_t vectorVField, int contourCount,
    StopToken cancellation)
{
    request.outputSize = frameBudgetBoundedOutputSize(
        request.outputSize, dataset->maximumResponseBytes());
    int fallbackFrom = -1;
    int fallbackTo = -1;
    for (;;) {
        try {
            auto result = executeSlice(dataset, request, rangeMode,
                userRange, logarithmic, palette, cancellation);
            result.mode = displayMode;
            result.vectorUField = vectorUField;
            result.vectorVField = vectorVField;
            result.contourCount = contourCount;
            if (isContourMode(displayMode)) {
                appendContours(dataset, request, contourCount,
                    result.minimum, result.maximum, result.logarithmic,
                    cancellation, result);
            }
            if (displayMode == DisplayMode::VelocityVectors) {
                appendVectorGlyphs(dataset, request,
                    FieldId{vectorUField}, FieldId{vectorVField},
                    contourCount, cancellation, result);
            }
            result.cacheFallbackFromLevel = fallbackFrom;
            result.cacheFallbackToLevel = fallbackTo;
            return result;
        } catch (const CacheBudgetExceeded&) {
            const auto budget = cacheBudgetDescription(
                dataset->cacheMetrics().budgetBytes);
            if (request.composition != CompositionPolicy::FinestAvailable) {
                throw std::runtime_error(
                    "The selected slice level cannot fit in the " + budget
                    + " cache. Choose a lower level or increase "
                      "AMREXPLORER_CACHE_SIZE_MB.");
            }
            if (request.maximumLevel == 0) {
                throw std::runtime_error(
                    "The slice cannot fit in the " + budget
                    + " cache, even at level 0. Try a smaller plotfile or "
                      "increase AMREXPLORER_CACHE_SIZE_MB.");
            }
            dataset->clearUnpinnedCache();
            if (fallbackFrom < 0) {
                fallbackFrom = request.maximumLevel;
            }
            fallbackTo = --request.maximumLevel;
        }
    }
}

// Contour overlays are extracted from a dedicated linearly-sampled slice
// queried at a resolution fine enough that the cell-scale staircase is
// invisible: at least 512 samples on the shorter axis, capped at 1024 (see
// contourRequest below). Because this plane already carries the smooth
// interpolant, contour extraction runs on it directly with no further
// refinement, and two Chaikin passes finish the polylines. Extraction runs
// on the slice worker, with the output mapped to display-plane pixel space;
// the GUI thread only converts the polylines to painter paths. The contour
// plane is cached by the GUI, so range and contour-count changes re-run only
// this cheap extraction (see refreshCachedSlice).
void appendContours(const std::shared_ptr<DatasetSession>& dataset,
    const SliceRequest& request, int contourCount, double minimum, double maximum,
    bool logarithmic, StopToken cancellation, SliceDisplayResult& result)
{
    const auto& metadata = dataset->metadata();
    const auto level = std::min(request.maximumLevel, metadata.finestLevel);
    const auto axes = slicePlaneAxes(metadata.dimension, request.normalDirection);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto dataWidth = coveredCells(metadata, level, axes[0],
        request.visibleRegion.lower[xAxis], request.visibleRegion.upper[xAxis]);
    const auto dataHeight = coveredCells(metadata, level, axes[1],
        request.visibleRegion.lower[yAxis], request.visibleRegion.upper[yAxis]);
    const auto displayWidth = request.outputSize[0];
    const auto displayHeight = request.outputSize[1];

    // Legacy Amrvis draws contours at each FAB's native grid resolution,
    // producing smooth curves at any display scale. We match that by
    // querying a linearly interpolated plane at a resolution fine enough
    // that contour staircases are invisible — at least 512 samples on the
    // shorter axis, capped at 1024. Two Chaikin passes finish the polylines.
    auto contourRequest = request;
    // The primary scalar request already carries the optional box list.
    // The higher-resolution contour plane contributes values only.
    contourRequest.includeGridBoxes = false;
    contourRequest.outputSize = {
        std::min(std::max(dataWidth, 512), 1024),
        std::min(std::max(dataHeight, 512), 1024)};
    contourRequest.outputSize = frameBudgetBoundedOutputSize(
        contourRequest.outputSize, dataset->maximumResponseBytes());
    contourRequest.sampling = SamplingPolicy::Linear;
    auto contour = requestSlice(*dataset, contourRequest, cancellation);
    result.contourPlane = std::move(contour.plane);
    result.slice.metrics.candidateBlocks += contour.metrics.candidateBlocks;
    result.slice.metrics.blocksRead += contour.metrics.blocksRead;
    result.slice.metrics.cacheHits += contour.metrics.cacheHits;
    result.slice.metrics.payloadBytesRead += contour.metrics.payloadBytesRead;

    // No supersampling (#56 removed it): the linear plane is already at contour
    // resolution, so contours are extracted from contourPlane directly.
    const auto values = contourValues(
        minimum, maximum, contourCount, logarithmic);
    result.contourPolylines = contourPolylinesForDisplay(
        result.contourPlane, values, displayWidth, displayHeight);
}

SliceDisplayResult refreshCachedSlice(
    const std::shared_ptr<DatasetSession>& dataset,
    const SliceRequest& request,
    std::shared_ptr<const ScalarPlane> displayPlanePtr,
    ScalarPlane contourPlane, std::vector<VectorSegment> vectors,
    RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, const Palette& palette, DisplayMode displayMode,
    std::uint32_t vectorUField, std::uint32_t vectorVField,
    int contourCount, bool rasterDirty, StopToken cancellation)
{
    // The cache path exists to reuse an existing display plane; a null one is a
    // caller bug. Reject it here so displayPlane() never silently substitutes an
    // empty plane (the old by-value parameter made null unrepresentable).
    if (!displayPlanePtr) {
        throw std::invalid_argument(
            "refreshCachedSlice requires a non-null display plane");
    }
    SliceDisplayResult result;
    result.request = request;
    result.mode = displayMode;
    result.vectorUField = vectorUField;
    result.vectorVField = vectorVField;
    result.contourCount = contourCount;
    // Adopt the cached plane by shared_ptr instead of deep-copying it into
    // slice.plane (up to ~110 MB); every reader goes through displayPlane().
    // `plane` binds to the shared pointee (stable, external to `result`), not
    // into `result`'s own storage, so populating `result` below can't dangle it.
    result.reusedPlane = std::move(displayPlanePtr);
    const auto& plane = *result.reusedPlane;
    const auto range = resolveDisplayRange(dataset, request.field,
        request.maximumLevel, request.composition, rangeMode, userRange,
        logarithmic, plane, cancellation);
    result.minimum = range.minimum;
    result.maximum = range.maximum;
    result.logarithmic = range.logarithmic;
    result.fieldName = dataset->metadata().fields[request.field.value].name;
    result.rasterUnchanged = !rasterDirty;
    if (rasterDirty) {
        result.image = renderScalarPlane(plane,
            ScalarRenderSettings{
                .minimum = range.minimum,
                .maximum = range.maximum,
                .logarithmic = range.logarithmic,
                .palette = &palette
            });
    }
    if (isContourMode(displayMode)) {
        result.contourPlane = std::move(contourPlane);
        const auto values = contourValues(
            range.minimum, range.maximum, contourCount, range.logarithmic);
        result.contourPolylines = contourPolylinesForDisplay(
            result.contourPlane, values,
            request.outputSize[0], request.outputSize[1]);
    }
    if (displayMode == DisplayMode::VelocityVectors) {
        result.vectors = std::move(vectors);
    }
    applyDisplayCoordinates(dataset->metadata(), result);
    return result;
}

std::vector<ContourPolyline> recomputeContourPolylines(
    const ScalarPlane& plane, double minimum,
    double maximum, bool logarithmic, int contourCount,
    int displayWidth, int displayHeight)
{
    if (plane.width <= 0 || plane.height <= 0 || contourCount < 1
        || !(minimum < maximum) || (logarithmic && !(minimum > 0.0))) {
        return {};
    }
    try {
        const auto values = contourValues(
            minimum, maximum, contourCount, logarithmic);
        return contourPolylinesForDisplay(
            plane, values, displayWidth, displayHeight);
    } catch (const std::exception&) {
        return {};
    }
}

void recomputeContourPolylines(SliceDisplayResult& result)
{
    if (!isContourMode(result.mode)) {
        return;
    }
    result.contourPolylines = recomputeContourPolylines(
        result.contourPlane, result.minimum,
        result.maximum, result.logarithmic, result.contourCount,
        result.request.outputSize[0], result.request.outputSize[1]);
}

std::vector<ParticleSample> loadParticleSamples(
    DatasetSession& dataset,
    std::span<const std::string> selectedSpecies, double fraction,
    std::uint64_t seed, StopToken cancellation)
{
    std::vector<ParticleSample> samples;
    samples.reserve(std::min(
        dataset.particleSpecies().size(), selectedSpecies.size()));
    for (const auto& species : dataset.particleSpecies()) {
        if (std::find(selectedSpecies.begin(), selectedSpecies.end(),
                species.name) == selectedSpecies.end()) {
            continue;
        }
        samples.push_back(dataset.requestParticleSample(
            species.name, fraction, seed, cancellation));
    }
    return samples;
}

InitialSliceResult executeSessionFrameLoad(
    std::shared_ptr<DatasetSession> dataset, const FrameSliceSpec& spec,
    StopToken cancellation)
{
    InitialSliceResult result;
    result.dataset = std::move(dataset);
    if (!result.dataset) {
        throw std::invalid_argument("frame load requires a dataset session");
    }
    const auto datasetId = result.dataset->id();
    const auto cacheBudgetBytes
        = result.dataset->cacheMetrics().budgetBytes;
    const auto& metadata = result.dataset->metadata();
    if (metadata.fields.empty()) {
        throw std::runtime_error("dataset has no scalar fields to display");
    }
    result.fileVersion = result.dataset->fileVersion();

    const auto fieldCount = static_cast<std::uint32_t>(metadata.fields.size());
    const auto field = std::min(spec.field, fieldCount - 1);
    // An out-of-range exact level falls back to finest-available, matching
    // the level combo's behavior when a frame has fewer levels.
    // Combo data encoding: -1=finest, N=level N only, 1000+N=update to N.
    const auto levelSelection = spec.levelSelection >= -1
        && spec.levelSelection <= metadata.finestLevel
            ? spec.levelSelection
            : (spec.levelSelection >= kUpdateToLevelOffset
                && spec.levelSelection - kUpdateToLevelOffset
                    <= metadata.finestLevel)
            ? spec.levelSelection
            : -1;
    const auto selectedLevel = decodeLevelData(
        levelSelection, metadata.finestLevel);
    auto attemptMaximumLevel = selectedLevel.maximumLevel;
    const auto rangeMode = effectiveRangeMode(result.dataset, FieldId{field},
        attemptMaximumLevel, selectedLevel.composition, spec.rangeMode);
    std::array<double, 3> positions{0.0, 0.0, 0.0};
    const auto dataBounds = datasetSampleBounds(metadata);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto lower = dataBounds.lower[axis];
        const auto upper = dataBounds.upper[axis];
        positions[axis] = spec.defaultPositions
            ? lower + 0.5 * (upper - lower)
            : std::clamp(spec.slicePositions[axis], lower,
                std::nextafter(upper, lower));
    }

    // 3-D datasets display all three orthogonal planes at once; the slices
    // share the dataset cache, so the sequential queries are bounded. 2-D
    // keeps its single y-normal display plane.
    const std::vector<int> normals = metadata.dimension == 3
        ? std::vector<int>{0, 1, 2} : std::vector<int>{1};
    for (;;) {
        try {
            result.displays.clear();
            result.displays.reserve(normals.size());
            for (std::size_t entry = 0; entry < normals.size(); ++entry) {
                const auto normal = normals[entry];
                SliceRequest request;
                request.dataset = datasetId;
                request.field = FieldId{field};
                request.normalDirection = normal;
                // A preserved zoom region is clipped to the new frame's domain; if
                // it no longer intersects at all, fall back to the whole domain.
                auto region = entry < spec.visibleRegions.size()
                    && spec.visibleRegions[entry].has_value()
                        ? *spec.visibleRegions[entry] : dataBounds;
                for (int axis = 0; axis < metadata.dimension; ++axis) {
                    const auto index = static_cast<std::size_t>(axis);
                    auto lower = std::max(region.lower[index],
                        dataBounds.lower[index]);
                    auto upper = std::min(region.upper[index],
                        dataBounds.upper[index]);
                    if (!(lower < upper)) {
                        lower = dataBounds.lower[index];
                        upper = dataBounds.upper[index];
                    }
                    region.lower[index] = lower;
                    region.upper[index] = upper;
                }
                request.visibleRegion = region;
                const auto hasOutputSize = !spec.outputSizes.empty();
                request.outputSize = entry < spec.outputSizes.size()
                    ? spec.outputSizes[entry]
                    : (spec.outputSizesAreViewportBounds && hasOutputSize
                              ? spec.outputSizes.back()
                              : finestNativeOutputSize(metadata,
                                    request.visibleRegion,
                                    request.normalDirection));
                if (spec.outputSizesAreViewportBounds && hasOutputSize) {
                    request.outputSize = nativeBoundedViewportOutputSize(metadata,
                        request.visibleRegion, request.normalDirection,
                        request.outputSize);
                }
                request.outputSize[0] = std::clamp(
                    request.outputSize[0], 1, maxSliceOutputDimension);
                request.outputSize[1] = std::clamp(
                    request.outputSize[1], 1, maxSliceOutputDimension);
                request.outputSize = frameBudgetBoundedOutputSize(
                    request.outputSize, result.dataset->maximumResponseBytes());
                request.composition = selectedLevel.composition;
                request.includeGridBoxes = spec.includeGridBoxes;
                request.maximumLevel = attemptMaximumLevel;
                request.sphericalSupersample = spec.sphericalSupersample;
                request.sphericalDisplay = spec.sphericalDisplay;
                if (metadata.dimension == 3) {
                    request.physicalPosition = positions[static_cast<std::size_t>(normal)];
                }
                auto display = executeSlice(result.dataset, request, rangeMode,
                    spec.userRange, spec.logarithmic, spec.palette, cancellation);
                display.mode = spec.displayMode;
                display.contourCount = spec.contourCount;
                if (isContourMode(spec.displayMode)) {
                    appendContours(result.dataset, request, spec.contourCount,
                        display.minimum, display.maximum, display.logarithmic,
                        cancellation, display);
                }
                if (spec.displayMode == DisplayMode::VelocityVectors) {
                    const auto u = std::min(spec.vectorUField, fieldCount - 1);
                    const auto v = std::min(spec.vectorVField, fieldCount - 1);
                    const auto w = std::min(spec.vectorWField, fieldCount - 1);
                    auto [f1, f2] = (metadata.dimension == 3)
                        ? (normal == 0 ? std::pair{v, w}
                           : normal == 1 ? std::pair{u, w}
                           : std::pair{u, v})
                        : std::pair{u, v};
                    display.vectorUField = f1;
                    display.vectorVField = f2;
                    appendVectorGlyphs(result.dataset, request,
                        FieldId{f1}, FieldId{f2},
                        spec.contourCount, cancellation, display);
                }
                result.displays.push_back(std::move(display));
            }
            // In 3-D, every views' "Visible" range must agree so the single color bar
            // maps all three panels consistently. Compute the union of finite extrema
            // across all three planes and re-render each display with the shared range.
            if (result.displays.size() == 3 && rangeMode == RangeMode::Visible) {
                const std::array<const ScalarPlane*, 3> planes{
                    &result.displays[0].displayPlane(),
                    &result.displays[1].displayPlane(),
                    &result.displays[2].displayPlane()};
                const auto shared = DisplayCoordinator::sharedVisibleRange(
                    planes, spec.logarithmic);
                // No finite samples anywhere: fall back to a neutral range so
                // the frame still renders.
                const auto [globalMin, globalMax] = shared.value_or(
                    spec.logarithmic ? std::pair{1.0, 10.0}
                                     : std::pair{0.0, 1.0});
                // One log flag for all three panels: log only when the shared
                // minimum is positive, matching how a single panel degrades to
                // linear. A per-panel flag kept an all-positive plane
                // logarithmic against a union that crosses zero, and
                // renderScalarPlane rejects a non-positive log minimum -- which
                // failed the whole frame load
                // (see shared-log-range-render-throw-fails-load).
                const bool sharedLog = spec.logarithmic && globalMin > 0.0;
                for (auto& d : result.displays) {
                    d.minimum = globalMin;
                    d.maximum = globalMax;
                    d.logarithmic = sharedLog;
                    d.image = renderScalarPlane(d.displayPlane(),
                        ScalarRenderSettings{
                            .minimum = globalMin,
                            .maximum = globalMax,
                            .logarithmic = sharedLog,
                            .palette = &spec.palette
                        });
                    // Contours were extracted per view before the shared range
                    // was known; re-extract them so their levels match the
                    // shared colorbar (see contours-stale-after-visible-range).
                    recomputeContourPolylines(d);
                }
            }
            break;
        } catch (const CacheBudgetExceeded&) {
            result.displays.clear();
            result.dataset->clearUnpinnedCache();
            // Plain (untranslated) messages: this runs in the Qt-free pipeline;
            // the GUI wraps failures in its own translated "Cannot load" text.
            if (selectedLevel.composition != CompositionPolicy::FinestAvailable) {
                throw std::runtime_error(
                    "The selected slice level cannot fit in the "
                    + cacheBudgetDescription(cacheBudgetBytes)
                    + " cache. Choose a lower level or increase "
                      "AMREXPLORER_CACHE_SIZE_MB.");
            }
            if (attemptMaximumLevel == 0) {
                throw std::runtime_error(
                    "The slice cannot fit in the "
                    + cacheBudgetDescription(cacheBudgetBytes)
                    + " cache, even at level 0. Try a smaller plotfile or "
                      "increase AMREXPLORER_CACHE_SIZE_MB.");
            }
            if (result.cacheFallbackFromLevel < 0) {
                result.cacheFallbackFromLevel = attemptMaximumLevel;
            }
            result.cacheFallbackToLevel = --attemptMaximumLevel;
        }
    }
    result.particles = loadParticleSamples(*result.dataset,
        spec.particleSpecies, spec.particleFraction, spec.particleSeed,
        cancellation);
    return result;
}

InitialSliceResult executeFrameLoad(const std::filesystem::path& path,
    DatasetId datasetId, const FrameSliceSpec& spec,
    std::uint64_t cacheBudgetBytes, StopToken cancellation,
    std::optional<PlotfileMetadataResult> preparedMetadata,
    std::filesystem::path dataRoot)
{
    std::shared_ptr<DatasetSession> dataset;
    if (preparedMetadata) {
        dataset = std::make_shared<LocalDatasetSession>(
            std::move(dataRoot), datasetId, cacheBudgetBytes,
            std::move(*preparedMetadata), cancellation);
    } else {
        dataset = std::make_shared<LocalDatasetSession>(
            path, datasetId, cacheBudgetBytes, cancellation);
    }
    return executeSessionFrameLoad(
        std::move(dataset), spec, cancellation);
}

} // namespace amrvis
