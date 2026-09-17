#pragma once

#include <amrexplorer/core/CoordinateSystem.hpp>
#include <amrexplorer/core/Geometry.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace amrvis {

struct DatasetId {
    std::uint64_t value = 0;
    auto operator<=>(const DatasetId&) const = default;
};

struct FieldId {
    std::uint32_t value = 0;
    auto operator<=>(const FieldId&) const = default;
};

enum class SamplingPolicy : std::uint8_t {
    Nearest,
    PiecewiseConstant,
    Linear
};

enum class CompositionPolicy : std::uint8_t {
    FinestAvailable,
    ExactLevel
};

struct BlockRequest {
    DatasetId dataset;
    int level = 0;
    int gridIndex = 0;
    FieldId field;
    int firstComponent = 0;
    int componentCount = 1;
};

struct SliceRequest {
    DatasetId dataset;
    FieldId field;
    int component = 0;
    int normalDirection = 2;
    double physicalPosition = 0.0;
    RealBox visibleRegion;
    int maximumLevel = 0;
    std::array<int, 2> outputSize{0, 0};
    SamplingPolicy sampling = SamplingPolicy::PiecewiseConstant;
    CompositionPolicy composition = CompositionPolicy::FinestAvailable;
    bool includeGridBoxes = false;
    // Collection bound supplied by the execution boundary. Local queries keep
    // the default; the remote server replaces it with a frame-derived limit.
    std::size_t maximumGridBoxes = std::numeric_limits<std::size_t>::max();
    // 2-D spherical display layout (R-Z warp, r-theta, or theta-r). A pure
    // display parameter -- deliberately excluded from sameSliceSpec so changing
    // it re-warps from the cached planes without a new query.
    SphericalDisplay sphericalDisplay = SphericalDisplay::RZ;
    // Mapped-grid display only: draw the raster on the plotfile's stretched
    // node positions (core/MappedGrid.hpp) when the session has them. Pure
    // display parameters like the spherical ones above, excluded from
    // sameSliceSpec so a toggle re-warps the cached planes without a new
    // query. Ignored by a session without a mapped grid.
    bool mappedGrid = false;
    // The part of physical display space the warp is drawn for, on the
    // slice's in-plane axes, and the device-pixel size it is drawn at: the
    // view's visible window at the screen's own resolution, so cell edges
    // are rasterized where they are seen rather than resampled from a
    // fixed-pitch image. An invalid window (the default) means the whole
    // node bounding box (the sector's, for the spherical R-Z warp); zero
    // pixels mean the raster's own size.
    RealBox displayWindow{};
    std::array<int, 2> displayPixels{0, 0};
    // Ask for the node bounding box of the whole domain on this slice's
    // plane (SliceDisplayResult::mappedDomainBounds): what a view anchors
    // its canvas to, wanted once per view rather than with every warp.
    bool wantMappedDomainBounds = false;
};

struct LineRequest {
    DatasetId dataset;
    FieldId field;
    int component = 0;
    int axis = 0;                              // line direction: 0=x, 1=y, 2=z
    std::array<double, 3> fixedCoordinates{};  // physical coords of the other axes
    int maximumLevel = 0;
    CompositionPolicy composition = CompositionPolicy::FinestAvailable;
    // Optional extent along the line axis (from the viewport's visible region).
    // When unset the line spans the full level domain.
    std::optional<RealBox> region;
};

[[nodiscard]] std::vector<std::string> validateBlockRequest(const BlockRequest& request);
[[nodiscard]] std::vector<std::string> validateSliceRequest(
    const SliceRequest& request, int datasetDimension);
[[nodiscard]] std::vector<std::string> validateLineRequest(
    const LineRequest& request, int datasetDimension);

} // namespace amrvis
