#include <amrexplorer/pipeline/SlicePipeline.hpp>

#include <amrexplorer/cache/ByteLruCache.hpp>
#include <amrexplorer/data/LocalDatasetSession.hpp>
#include <amrexplorer/pipeline/DisplayCoordinator.hpp>
#include <amrexplorer/render2d/ScalarRenderer.hpp>

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
        logarithmic, result.slice.plane);
    result.minimum = range.minimum;
    result.maximum = range.maximum;
    result.logarithmic = range.logarithmic;
    result.fieldName = dataset->metadata().fields[request.field.value].name;
    result.image = renderScalarPlane(result.slice.plane,
        ScalarRenderSettings{
            .minimum = range.minimum,
            .maximum = range.maximum,
            .logarithmic = range.logarithmic,
            .palette = &palette
        });
    return result;
}

void appendVectorGlyphs(const std::shared_ptr<DatasetSession>& dataset,
    SliceRequest request, FieldId uField, FieldId vField, int count,
    StopToken cancellation, SliceDisplayResult& result)
{
    request.field = uField;
    auto uSlice = requestSlice(*dataset, request, cancellation);
    request.field = vField;
    auto vSlice = requestSlice(*dataset, request, cancellation);
    result.vectors = generateVectorGlyphs(uSlice.plane, vSlice.plane, count);
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
    contourRequest.outputSize = {
        std::min(std::max(dataWidth, 512), 1024),
        std::min(std::max(dataHeight, 512), 1024)};
    contourRequest.sampling = SamplingPolicy::Linear;
    auto contour = requestSlice(*dataset, contourRequest, cancellation);
    result.contourPlane = std::move(contour.plane);
    result.slice.metrics.candidateBlocks += contour.metrics.candidateBlocks;
    result.slice.metrics.blocksRead += contour.metrics.blocksRead;
    result.slice.metrics.cacheHits += contour.metrics.cacheHits;
    result.slice.metrics.payloadBytesRead += contour.metrics.payloadBytesRead;

    // No supersampling (#8 removed it): the linear plane is already smooth, so
    // store it as the fine plane too (factor 1) for refreshCachedSlice to
    // reuse. The copy and the factor retire together when the "stop
    // round-tripping planes through SliceDisplayResult" cleanup lands.
    result.contourFinePlane = result.contourPlane;
    result.contourFineFactor = 1;
    const auto values = contourValues(
        minimum, maximum, contourCount, logarithmic);
    result.contourPolylines = contourPolylinesForDisplay(
        result.contourFinePlane, 1, values, displayWidth, displayHeight);
}

SliceDisplayResult refreshCachedSlice(
    const std::shared_ptr<DatasetSession>& dataset,
    const SliceRequest& request, ScalarPlane displayPlane,
    ScalarPlane contourPlane, ScalarPlane contourFinePlane, int contourFineFactor,
    std::vector<VectorSegment> vectors,
    RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, const Palette& palette, DisplayMode displayMode,
    std::uint32_t vectorUField, std::uint32_t vectorVField,
    int contourCount, bool rasterDirty)
{
    SliceDisplayResult result;
    result.request = request;
    result.mode = displayMode;
    result.vectorUField = vectorUField;
    result.vectorVField = vectorVField;
    result.contourCount = contourCount;
    result.slice.plane = std::move(displayPlane);
    const auto range = resolveDisplayRange(dataset, request.field,
        request.maximumLevel, request.composition, rangeMode, userRange,
        logarithmic, result.slice.plane);
    result.minimum = range.minimum;
    result.maximum = range.maximum;
    result.logarithmic = range.logarithmic;
    result.fieldName = dataset->metadata().fields[request.field.value].name;
    result.rasterUnchanged = !rasterDirty;
    if (rasterDirty) {
        result.image = renderScalarPlane(result.slice.plane,
            ScalarRenderSettings{
                .minimum = range.minimum,
                .maximum = range.maximum,
                .logarithmic = range.logarithmic,
                .palette = &palette
            });
    }
    if (isContourMode(displayMode)) {
        result.contourPlane = std::move(contourPlane);
        result.contourFinePlane = std::move(contourFinePlane);
        result.contourFineFactor = contourFineFactor;
        const auto values = contourValues(
            range.minimum, range.maximum, contourCount, range.logarithmic);
        result.contourPolylines = contourPolylinesForDisplay(
            result.contourFinePlane, contourFineFactor, values,
            request.outputSize[0], request.outputSize[1]);
    }
    if (displayMode == DisplayMode::VelocityVectors) {
        result.vectors = std::move(vectors);
    }
    return result;
}

std::vector<ContourPolyline> recomputeContourPolylines(
    const ScalarPlane& finePlane, int fineFactor, double minimum,
    double maximum, bool logarithmic, int contourCount,
    int displayWidth, int displayHeight)
{
    if (finePlane.width <= 0 || finePlane.height <= 0 || contourCount < 1
        || !(minimum < maximum) || (logarithmic && !(minimum > 0.0))) {
        return {};
    }
    try {
        const auto values = contourValues(
            minimum, maximum, contourCount, logarithmic);
        return contourPolylinesForDisplay(
            finePlane, fineFactor, values, displayWidth, displayHeight);
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
        result.contourFinePlane, result.contourFineFactor, result.minimum,
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
                request.outputSize = entry < spec.outputSizes.size()
                    ? spec.outputSizes[entry]
                    : finestNativeOutputSize(metadata, request.visibleRegion,
                        request.normalDirection);
                request.outputSize[0] = std::clamp(
                    request.outputSize[0], 1, maxSliceOutputDimension);
                request.outputSize[1] = std::clamp(
                    request.outputSize[1], 1, maxSliceOutputDimension);
                request.composition = selectedLevel.composition;
                request.includeGridBoxes = spec.includeGridBoxes;
                request.maximumLevel = attemptMaximumLevel;
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
                    &result.displays[0].slice.plane,
                    &result.displays[1].slice.plane,
                    &result.displays[2].slice.plane};
                const auto shared = DisplayCoordinator::sharedVisibleRange(
                    planes, spec.logarithmic);
                // No finite samples anywhere: fall back to a neutral range so
                // the frame still renders.
                const auto [globalMin, globalMax] = shared.value_or(
                    spec.logarithmic ? std::pair{1.0, 10.0}
                                     : std::pair{0.0, 1.0});
                for (auto& d : result.displays) {
                    d.minimum = globalMin;
                    d.maximum = globalMax;
                    d.image = renderScalarPlane(d.slice.plane,
                        ScalarRenderSettings{
                            .minimum = globalMin,
                            .maximum = globalMax,
                            .logarithmic = d.logarithmic,
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
            std::move(*preparedMetadata));
    } else {
        dataset = std::make_shared<LocalDatasetSession>(
            path, datasetId, cacheBudgetBytes);
    }
    return executeSessionFrameLoad(
        std::move(dataset), spec, cancellation);
}

} // namespace amrvis
