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

} // namespace amrvis
