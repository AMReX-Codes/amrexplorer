#include <amrexplorer/pipeline/ParticleProjection.hpp>

#include <amrexplorer/pipeline/SlicePipeline.hpp>

#include <cmath>

namespace amrvis {

std::vector<ParticlePixel>
projectParticlePoints(std::span<const ParticlePoint> particles,
                      const ScalarPlane& plane, int dimension, int normalDirection)
{
    if ((dimension != 2 && dimension != 3)
        || (dimension == 3 && (normalDirection < 0 || normalDirection >= dimension))
        || plane.width <= 0 || plane.height <= 0) {
        return {};
    }

    const auto axes = slicePlaneAxes(dimension, normalDirection);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto& region = plane.physicalRegion;
    const auto xExtent = region.upper[xAxis] - region.lower[xAxis];
    const auto yExtent = region.upper[yAxis] - region.lower[yAxis];
    if (!(xExtent > 0.0) || !(yExtent > 0.0) || !std::isfinite(xExtent)
        || !std::isfinite(yExtent)) {
        return {};
    }

    std::vector<ParticlePixel> projected;
    projected.reserve(particles.size());
    for (const auto& particle : particles) {
        const auto x = particle.position[xAxis];
        const auto y = particle.position[yAxis];
        if (!std::isfinite(x) || !std::isfinite(y) || x < region.lower[xAxis]
            || x > region.upper[xAxis] || y < region.lower[yAxis]
            || y > region.upper[yAxis]) {
            continue;
        }
        projected.push_back(
            {.x = (x - region.lower[xAxis]) / xExtent * plane.width,
             .y = plane.height
                  - (y - region.lower[yAxis]) / yExtent * plane.height});
    }
    return projected;
}

} // namespace amrvis
