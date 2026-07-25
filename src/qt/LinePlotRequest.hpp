#pragma once

#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/Result.hpp>

#include <array>
#include <cstddef>

namespace amrvis::qt {

// Maps an image-space middle/right-button click onto a LineRequest. The image
// shows the display plane mirrored vertically (image row 0 is the top row),
// so the plane row is height - 1 - imageY, the same mapping the probe code
// uses. For a 2-D display the in-plane axes are x and y; for a 3-D display
// with normal n they are the two axes != n in increasing order. A horizontal
// (middle-button) click produces a line along the first in-plane axis with the
// cursor's coordinate on the second in-plane axis fixed; a vertical
// (right-button) click swaps the roles. In 3-D the normal coordinate is fixed
// at the current slice position.
[[nodiscard]] inline LineRequest makeLineRequest(const RealBox& planeRegion,
    int imageWidth, int imageHeight, int imageX, int imageY, bool horizontalDrag,
    int dimension, int normalDirection, double slicePosition,
    DatasetId dataset, FieldId field, int maximumLevel,
    CompositionPolicy composition) noexcept
{
    std::array<int, 2> axes{0, 1};
    if (dimension == 3) {
        std::size_t next = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (axis != normalDirection) {
                axes[next++] = axis;
            }
        }
    }
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto planeY = imageHeight - 1 - imageY;
    const auto physicalX = planeRegion.lower[xAxis]
        + (static_cast<double>(imageX) + 0.5) / static_cast<double>(imageWidth)
            * (planeRegion.upper[xAxis] - planeRegion.lower[xAxis]);
    const auto physicalY = planeRegion.lower[yAxis]
        + (static_cast<double>(planeY) + 0.5) / static_cast<double>(imageHeight)
            * (planeRegion.upper[yAxis] - planeRegion.lower[yAxis]);

    LineRequest request;
    request.dataset = dataset;
    request.field = field;
    request.maximumLevel = maximumLevel;
    request.composition = composition;
    if (horizontalDrag) {
        request.axis = axes[0];
        request.fixedCoordinates[yAxis] = physicalY;
    } else {
        request.axis = axes[1];
        request.fixedCoordinates[xAxis] = physicalX;
    }
    if (dimension == 3) {
        request.fixedCoordinates[static_cast<std::size_t>(normalDirection)]
            = slicePosition;
    }
    // Bound the line to the viewport's visible region so a line plot started
    // from a zoomed subregion only spans that subregion.
    request.region = planeRegion;
    return request;
}

} // namespace amrvis::qt
