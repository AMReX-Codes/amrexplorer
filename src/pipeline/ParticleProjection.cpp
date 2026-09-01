#include <amrexplorer/pipeline/ParticleProjection.hpp>

#include <amrexplorer/pipeline/SlicePipeline.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace amrvis {

std::vector<SliceCellSlab> sliceCellSlabs(
    const DatasetMetadata& metadata, int normalAxis, double slicePosition)
{
    std::vector<SliceCellSlab> slabs;
    if (metadata.dimension != 3 || normalAxis < 0
        || normalAxis >= metadata.dimension) {
        return slabs;
    }
    const auto axis = static_cast<std::size_t>(normalAxis);
    slabs.reserve(metadata.levels.size());
    for (const auto& level : metadata.levels) {
        // An empty slab (lower == upper) says the plane cuts no cell at this
        // level. Two ways to get one, and neither is sampleIndex returning a
        // sentinel: it throws only for a non-finite or int-range-exceeding
        // coordinate, and for a position off the end of a level it returns an
        // extrapolated index that sampleBounds would turn into a cell that is
        // not there. So the index is range-checked against the level's own
        // domain as well.
        SliceCellSlab slab;
        try {
            const auto index = sampleIndex(level, normalAxis, slicePosition);
            if (index >= level.domain.lower[axis]
                && index <= level.domain.upper[axis]) {
                // The level's own domain box collapsed to that one index on
                // the normal, so it carries the level's centering and
                // sampleBounds places nodal and cell-centered levels alike.
                auto cell = level.domain;
                cell.lower[axis] = index;
                cell.upper[axis] = index;
                const auto bounds
                    = sampleBounds(level, cell, metadata.dimension);
                slab.lower = bounds.lower[axis];
                slab.upper = bounds.upper[axis];
            }
        } catch (const std::out_of_range&) {
            // The default empty slab stands: sampleIndex is the only call
            // above that throws, and it throws before slab is touched.
        }
        slabs.push_back(slab);
    }
    return slabs;
}

std::vector<ParticlePixel>
projectParticlePoints(std::span<const ParticlePoint> particles,
                      const ScalarPlane& plane, int dimension,
                      int normalDirection,
                      std::span<const SliceCellSlab> levelSlabs)
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

    // Filtering to the cells the plane cuts needs the per-pixel level, and a
    // query sizes values/valid/sourceLevel together -- so a plane whose
    // sourceLevel does not match its raster carries no answer to give. Only
    // 3-D has a normal to filter on: in 2-D the slice is the domain, and
    // normalDirection is not even constrained to be an axis.
    const bool filtering = !levelSlabs.empty() && dimension == 3;
    const auto pixelCount = static_cast<std::size_t>(plane.width)
        * static_cast<std::size_t>(plane.height);
    if (filtering && plane.sourceLevel.size() != pixelCount) {
        return {};
    }
    // Only read when filtering, which implies a 3-D normal in range; below
    // three dimensions normalDirection is not constrained to be an axis.
    const auto normalAxis
        = static_cast<std::size_t>(filtering ? normalDirection : 0);

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
        if (filtering) {
            // The pixel this point falls in, and the inverse of the
            // sampleCentre the query binned that pixel with: the extent is
            // divided last there and has to be here too, or a coordinate a
            // ULP from a pixel edge lands on the other side of it -- across
            // a level jump, that is the wrong slab. The row counts the way
            // sourceLevel is written -- increasing physical y -- not the way
            // the emitted scene y is flipped. The inclusive clip above lands
            // the upper edge exactly on width/height, hence the clamp.
            // Clamped as a double, before the cast. Multiplying first
            // is what keeps the binning exact, but it also overflows to
            // infinity for a region whose extent approaches the double
            // range -- and casting that to int is undefined, where clamping
            // it lands on the last column the way any coordinate at the
            // upper edge should. Non-negative throughout (x >= lower), so
            // truncation is the floor.
            const auto scaled
                = (x - region.lower[xAxis]) * plane.width / xExtent;
            const auto column = static_cast<int>(std::clamp(
                scaled, 0.0, static_cast<double>(plane.width - 1)));
            const auto scaledRow
                = (y - region.lower[yAxis]) * plane.height / yExtent;
            const auto row = static_cast<int>(std::clamp(
                scaledRow, 0.0, static_cast<double>(plane.height - 1)));
            const auto level = plane.sourceLevel[static_cast<std::size_t>(column)
                + static_cast<std::size_t>(plane.width)
                    * static_cast<std::size_t>(row)];
            if (level < 0
                || static_cast<std::size_t>(level) >= levelSlabs.size()) {
                continue;
            }
            const auto& slab = levelSlabs[static_cast<std::size_t>(level)];
            const auto normal = particle.position[normalAxis];
            // Half-open, the rule the slice itself uses to pick its cell and
            // its grid boxes: a position on the upper face belongs to the
            // next cell up.
            if (!(normal >= slab.lower) || !(normal < slab.upper)) {
                continue;
            }
        }
        projected.push_back(
            {.x = (x - region.lower[xAxis]) / xExtent * plane.width,
             .y = plane.height
                  - (y - region.lower[yAxis]) / yExtent * plane.height});
    }
    return projected;
}

} // namespace amrvis
