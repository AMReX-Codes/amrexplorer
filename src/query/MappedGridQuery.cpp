#include <amrexplorer/query/MappedGridQuery.hpp>

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/query/SliceQuery.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace amrvis {

MappedGridPlane queryMappedGridPlane(PlotfileDataset& data,
    PlotfileDataset& grid, const MappedGridPlaneRequest& request,
    StopToken cancellation)
{
    const auto& metadata = data.metadata();
    const auto& gridMetadata = grid.metadata();
    const auto errors = validateMappedGridPlaneRequest(request, metadata.dimension);
    if (!errors.empty()) {
        throw std::invalid_argument(errors.front());
    }
    if (request.dataset != data.id() || request.dataset != grid.id()) {
        throw std::invalid_argument(
            "mapped-grid request targets a different dataset");
    }
    if (gridMetadata.dimension != metadata.dimension
        || gridMetadata.levels.size() != metadata.levels.size()
        || gridMetadata.fields.size()
            < static_cast<std::size_t>(metadata.dimension)) {
        throw std::invalid_argument(
            "mapped-grid dataset does not match the plotfile");
    }
    const auto maximumLevel = std::min(request.maximumLevel, metadata.finestLevel);

    const auto axes = planeAxes(metadata.dimension, request.normalDirection);
    const auto& region = request.visibleRegion;
    const auto width = static_cast<std::uint64_t>(request.outputSize[0]) + 1;
    const auto height = static_cast<std::uint64_t>(request.outputSize[1]) + 1;
    if (width > std::numeric_limits<std::size_t>::max() / height) {
        throw std::overflow_error(
            "mapped-grid plane dimensions exceed addressable memory");
    }
    const auto nodeCount = static_cast<std::size_t>(width * height);

    MappedGridPlane plane;
    plane.width = static_cast<int>(width);
    plane.height = static_cast<int>(height);
    plane.physicalRegion = region;
    plane.a.assign(nodeCount, 0.0);
    plane.b.assign(nodeCount, 0.0);

    // The node layers along the normal whose displacements are averaged: the
    // two bounding the slice's cell in 3-D, the plane itself in 2-D. Indices
    // are clamped into the nodal domain so a slice through the last cell still
    // finds its upper layer.
    std::vector<double> layerPositions;
    if (metadata.dimension == 3) {
        const auto normal = static_cast<std::size_t>(request.normalDirection);
        const auto& dataLevel
            = metadata.levels[static_cast<std::size_t>(maximumLevel)];
        const auto& gridLevel
            = gridMetadata.levels[static_cast<std::size_t>(maximumLevel)];
        const auto cell = sampleIndex(
            dataLevel, request.normalDirection, request.physicalPosition);
        for (const auto node : {cell, cell + 1}) {
            const auto clamped = std::clamp(
                node, gridLevel.domain.lower[normal], gridLevel.domain.upper[normal]);
            layerPositions.push_back(
                samplePosition(gridLevel, request.normalDirection, clamped));
        }
    } else {
        layerPositions.push_back(request.physicalPosition);
    }

    // One node more than the raster per axis, over a region grown by half a
    // raster pitch: the sample centres then fall on lower + i*pitch, the
    // nodes themselves at native resolution. Linear sampling, because the
    // raster pitch is the finest level's while the nodes shown may be a
    // coarser level's: a sample between two stored nodes takes the
    // interpolated displacement, so a straight coarse edge stays straight
    // instead of stepping. Where a sample lands on a node it is exact.
    SliceRequest nodeRequest;
    nodeRequest.dataset = request.dataset;
    nodeRequest.normalDirection = request.normalDirection;
    nodeRequest.visibleRegion = region;
    nodeRequest.maximumLevel = request.maximumLevel;
    nodeRequest.outputSize = {plane.width, plane.height};
    nodeRequest.sampling = SamplingPolicy::Linear;
    nodeRequest.composition = request.composition;
    std::array<double, 2> pitch{};
    for (std::size_t inPlane = 0; inPlane < 2; ++inPlane) {
        const auto axis = static_cast<std::size_t>(axes[inPlane]);
        pitch[inPlane] = (region.upper[axis] - region.lower[axis])
            / static_cast<double>(request.outputSize[inPlane]);
        nodeRequest.visibleRegion.lower[axis] -= 0.5 * pitch[inPlane];
        nodeRequest.visibleRegion.upper[axis] += 0.5 * pitch[inPlane];
    }

    // The node query interpolates in-plane only. A layer position is a node
    // layer of the display level; where a coarser level is all that covers a
    // node, the layer falls between that level's node layers, and the
    // sample would snap to one of them -- both of the slice's layers can
    // snap to the same one, which places the cell by a single node. So a
    // node answered by a coarser level is interpolated along the normal
    // between the two coarse layers bracketing the position. Layer queries
    // are cached per position within this call; a single-level dataset never
    // needs a second one. A deque, not a vector: the loop below holds
    // references to cached layers across further queryLayer calls, and a
    // vector's push would move them out from under those references.
    std::deque<std::pair<double, SliceQueryResult>> layers;
    const auto queryLayer = [&](double position) -> const SliceQueryResult& {
        for (const auto& [at, result] : layers) {
            if (at == position) {
                return result;
            }
        }
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        nodeRequest.physicalPosition = position;
        layers.emplace_back(
            position, SliceQuery(grid).execute(nodeRequest, cancellation));
        return layers.back().second;
    };
    const auto normal = request.normalDirection;
    const auto nodalDomainOf = [&](int level) -> const IntBox& {
        return gridMetadata.levels[static_cast<std::size_t>(level)].domain;
    };

    std::vector<double> displacement(nodeCount);
    std::vector<std::uint8_t> covered(nodeCount);
    for (std::size_t inPlane = 0; inPlane < 2; ++inPlane) {
        const auto axis = static_cast<std::size_t>(axes[inPlane]);
        auto& coordinates = inPlane == 0 ? plane.a : plane.b;
        std::fill(displacement.begin(), displacement.end(), 0.0);
        std::fill(covered.begin(), covered.end(), std::uint8_t{0});
        layers.clear();
        nodeRequest.field = FieldId{static_cast<std::uint32_t>(axis)};
        for (const auto position : layerPositions) {
            const auto& direct = queryLayer(position);
            for (std::size_t node = 0; node < nodeCount; ++node) {
                if (direct.plane.valid[node] == 0) {
                    continue;
                }
                auto value = direct.plane.values[node];
                const int level = direct.plane.sourceLevel[node];
                if (metadata.dimension == 3 && level >= 0
                    && level < maximumLevel) {
                    const auto& gridLevel
                        = gridMetadata.levels[static_cast<std::size_t>(level)];
                    const auto n = static_cast<std::size_t>(normal);
                    const auto spacing = gridLevel.cellSize[n];
                    const auto offset
                        = (position - gridLevel.indexOrigin[n]) / spacing;
                    const auto lowerLayer = std::floor(offset);
                    const auto weight = offset - lowerLayer;
                    constexpr double onLayer = 1e-9;
                    if (weight > onLayer && weight < 1.0 - onLayer) {
                        const auto& domain = nodalDomainOf(level);
                        const auto low = std::clamp(static_cast<int>(lowerLayer),
                            domain.lower[n], domain.upper[n]);
                        const auto high = std::clamp(static_cast<int>(lowerLayer) + 1,
                            domain.lower[n], domain.upper[n]);
                        const auto& below = queryLayer(
                            samplePosition(gridLevel, normal, low));
                        const auto& above = queryLayer(
                            samplePosition(gridLevel, normal, high));
                        const auto valueBelow = below.plane.valid[node] != 0
                            ? below.plane.values[node] : value;
                        const auto valueAbove = above.plane.valid[node] != 0
                            ? above.plane.values[node] : value;
                        value = (1.0 - weight) * valueBelow + weight * valueAbove;
                    }
                }
                displacement[node] += value;
                ++covered[node];
            }
        }
        for (std::size_t row = 0; row < height; ++row) {
            for (std::size_t column = 0; column < width; ++column) {
                const auto node = row * static_cast<std::size_t>(width) + column;
                const auto step = inPlane == 0 ? column : row;
                auto value = region.lower[axis]
                    + static_cast<double>(step) * pitch[inPlane];
                if (covered[node] != 0) {
                    value += displacement[node] / static_cast<double>(covered[node]);
                }
                coordinates[node] = value;
            }
        }
    }
    return plane;
}

} // namespace amrvis
