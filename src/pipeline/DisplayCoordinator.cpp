#include <amrexplorer/pipeline/DisplayCoordinator.hpp>

#include <amrexplorer/render2d/ScalarRenderer.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace amrvis {

std::optional<std::pair<double, double>>
DisplayCoordinator::cachedFullDomainRange(const RangeKey& key) const
{
    if (m_rangeKey && *m_rangeKey == key) {
        return m_range;
    }
    return std::nullopt;
}

void DisplayCoordinator::storeFullDomainRange(
    const RangeKey& key, std::pair<double, double> range)
{
    m_rangeKey = key;
    m_range = range;
}

void DisplayCoordinator::invalidateRangeCache()
{
    m_rangeKey.reset();
}

std::optional<std::pair<double, double>> DisplayCoordinator::sharedVisibleRange(
    std::span<const ScalarPlane* const> planes, bool logarithmic)
{
    auto minimum = std::numeric_limits<double>::infinity();
    auto maximum = -std::numeric_limits<double>::infinity();
    for (const auto* plane : planes) {
        if (plane == nullptr || plane->width <= 0 || plane->height <= 0) {
            continue;
        }
        for (std::size_t i = 0; i < plane->values.size(); ++i) {
            if (plane->valid[i] == 0 || !std::isfinite(plane->values[i])) {
                continue;
            }
            const auto value = static_cast<double>(plane->values[i]);
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
    }
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
        return std::nullopt;
    }
    return paddedIfDegenerate(minimum, maximum, logarithmic);
}

ImageTransformPolicy DisplayCoordinator::rasterTransformPolicy(
    const std::optional<RasterGeometry>& cached,
    const RasterGeometry& incoming)
{
    // Dataset ids are connection/session-local ownership, not display
    // geometry. A sequence frame with a fresh id remains compatible when its
    // physical domain and raster-to-data orientation are unchanged. Pixel
    // density is also allowed to change: MainWindow remaps Custom mode's
    // visible physical window through the old and new planes.
    if (!cached.has_value()) {
        return ImageTransformPolicy::GeometryAware;
    }
    if (*cached != incoming) {
        return ImageTransformPolicy::Refit;
    }
    return ImageTransformPolicy::Preserve;
}

void DisplayCoordinator::realignArrivalToRange(SliceDisplayResult& result,
    std::pair<double, double> range, const Palette& palette,
    bool realignRasterAndContours)
{
    result.minimum = range.first;
    result.maximum = range.second;
    // The reused full-domain range is a superset of this arrival's own range,
    // so it can cross zero even when the arrival was all-positive (log resolved
    // true per panel). Log is only viable when the shared minimum is positive;
    // degrade to linear otherwise so renderScalarPlane does not reject the
    // non-positive minimum (see shared-log-range-render-throw-fails-load).
    result.logarithmic = result.logarithmic && range.first > 0.0;
    if (!realignRasterAndContours) {
        return;
    }
    if (!result.rasterUnchanged) {
        result.image = renderScalarPlane(result.slice.plane,
            ScalarRenderSettings{
                .minimum = result.minimum,
                .maximum = result.maximum,
                .logarithmic = result.logarithmic,
                .palette = &palette
            });
    }
    recomputeContourPolylines(result);
}

std::optional<DisplayCoordinator::SharedRangeSync>
DisplayCoordinator::syncPanelsToSharedRange(
    const RangeKey& key, std::span<const PanelSyncInput> panels,
    bool logarithmic, bool contourMode, int contourCount,
    const Palette& palette) const
{
    return renderPanelsToSharedRange(cachedFullDomainRange(key), panels,
        logarithmic, contourMode, contourCount, palette);
}

std::optional<DisplayCoordinator::SharedRangeSync>
DisplayCoordinator::renderPanelsToSharedRange(
    std::optional<std::pair<double, double>> sharedRange,
    std::span<const PanelSyncInput> panels, bool logarithmic,
    bool contourMode, int contourCount, const Palette& palette)
{
    auto shared = std::move(sharedRange);
    if (!shared) {
        std::vector<const ScalarPlane*> planes;
        planes.reserve(panels.size());
        for (const auto& panel : panels) {
            planes.push_back(panel.plane);
        }
        shared = sharedVisibleRange(planes, logarithmic);
    }
    if (!shared) {
        return std::nullopt;
    }
    SharedRangeSync sync;
    sync.range = *shared;
    // One log flag for every panel: log only when the shared minimum is
    // positive, matching how a single panel degrades to linear. Keeping it
    // per-panel would render an all-positive plane logarithmically against a
    // union that crosses zero, and renderScalarPlane rejects a non-positive
    // log minimum -- which threw and failed the whole load
    // (see shared-log-range-render-throw-fails-load).
    sync.logarithmic = logarithmic && sync.range.first > 0.0;
    sync.panels.resize(panels.size());
    for (std::size_t index = 0; index < panels.size(); ++index) {
        const auto& panel = panels[index];
        auto& update = sync.panels[index];
        if (panel.plane == nullptr || panel.plane->width <= 0
            || panel.plane->height <= 0) {
            continue;
        }
        update.applies = true;
        // Contours before the raster, mirroring the original inline order.
        if (contourMode && panel.contourFinePlane != nullptr
            && panel.contourFinePlane->width > 0) {
            update.contourPolylines = recomputeContourPolylines(
                *panel.contourFinePlane, panel.contourFineFactor,
                sync.range.first, sync.range.second, sync.logarithmic,
                contourCount, panel.outputSize[0], panel.outputSize[1]);
            update.contoursRecomputed = true;
        }
        update.image = renderScalarPlane(*panel.plane, ScalarRenderSettings{
            .minimum = sync.range.first,
            .maximum = sync.range.second,
            .logarithmic = sync.logarithmic,
            .palette = &palette
        });
    }
    return sync;
}

bool DisplayCoordinator::planeDensitiesDiffer(
    const ScalarPlane& before, const ScalarPlane& after,
    std::array<int, 2> axes)
{
    if (before.width <= 0 || before.height <= 0
        || after.width <= 0 || after.height <= 0) {
        return false;
    }
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto beforeExtentX
        = before.physicalRegion.upper[xAxis] - before.physicalRegion.lower[xAxis];
    const auto beforeExtentY
        = before.physicalRegion.upper[yAxis] - before.physicalRegion.lower[yAxis];
    const auto afterExtentX
        = after.physicalRegion.upper[xAxis] - after.physicalRegion.lower[xAxis];
    const auto afterExtentY
        = after.physicalRegion.upper[yAxis] - after.physicalRegion.lower[yAxis];
    if (!(beforeExtentX > 0.0) || !(beforeExtentY > 0.0)
        || !(afterExtentX > 0.0) || !(afterExtentY > 0.0)) {
        return false;
    }
    const auto matches = [](double a, double b) {
        return std::abs(a - b) <= 1.0e-9 * std::max(std::abs(a), std::abs(b));
    };
    return !matches(before.width / beforeExtentX, after.width / afterExtentX)
        || !matches(before.height / beforeExtentY,
            after.height / afterExtentY);
}

} // namespace amrvis
