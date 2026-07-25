#include <amrexplorer/pipeline/DisplayCoordinator.hpp>

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
    if (minimum == maximum) {
        if (logarithmic && minimum > 0.0) {
            minimum /= 1.0 + 1.0e-6;
            maximum *= 1.0 + 1.0e-6;
        } else {
            const auto pad = std::max(std::abs(minimum), 1.0) * 1.0e-6;
            minimum -= pad;
            maximum += pad;
        }
    }
    return std::pair{minimum, maximum};
}

ImageTransformPolicy DisplayCoordinator::rasterTransformPolicy(
    bool hasCachedRequest, const SliceRequest& cached,
    const SliceRequest& incoming, bool zoomed)
{
    // A zoomed visibleRegion is physical data state, not evidence that the
    // old raster transform is compatible with the incoming image. Preserve
    // the transform only for a panel-local refresh in the same dataset and
    // orientation (rubber-band zoom or pan). If a dataset replacement
    // overtakes such a refresh, explicitly refit when the cached and
    // incoming rasters cover different regions: their dimensions can
    // coincide even though their pixel-to-data mappings do not.
    const bool samePanelRenderContext = hasCachedRequest
        && cached.dataset == incoming.dataset
        && cached.normalDirection == incoming.normalDirection;
    const bool incompatibleRasterContext = hasCachedRequest
        && !samePanelRenderContext
        && (cached.normalDirection != incoming.normalDirection
            || cached.visibleRegion != incoming.visibleRegion);
    if (incompatibleRasterContext) {
        return ImageTransformPolicy::Refit;
    }
    if (zoomed && samePanelRenderContext) {
        return ImageTransformPolicy::Preserve;
    }
    return ImageTransformPolicy::GeometryAware;
}

} // namespace amrvis
