#pragma once

namespace amrvis {

// What the slice panels draw. Lives in the Qt-free pipeline layer because the
// worker-side slice functions branch on it; the Set Contours dialog re-exports
// it into amrvis::qt for the GUI.
enum class DisplayMode {
    Raster,
    RasterContours,
    VelocityVectors
};

[[nodiscard]] inline bool isContourMode(DisplayMode mode)
{
    return mode == DisplayMode::RasterContours;
}

// How a slice raster was drawn: flat (one pixel per plane sample over the
// logical region), or warped into physical display space for the window a
// view shows at its device pixels -- on the plotfile's mapped (stretched)
// grid, or on the (R, Z) wedge of a 2-D spherical plane.
enum class DisplayWarp {
    None,
    MappedGrid,
    SphericalRZ
};

[[nodiscard]] constexpr bool isWarped(DisplayWarp warp) noexcept
{
    return warp != DisplayWarp::None;
}

} // namespace amrvis
