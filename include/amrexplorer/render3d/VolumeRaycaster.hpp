#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/OrthoProjection.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/core/Volume.hpp>

#include <array>
#include <optional>
#include <utility>

namespace amrvis {

// Direct volume rendering of a sampled grid: an orthographic ray per output
// pixel, marched front to back through the grid, each sample's value mapped
// through the range to a transfer-function entry whose colour and opacity
// are composited until the ray leaves the grid or turns opaque. Qt-free and
// deterministic: a pixel's result depends only on the inputs, never on how
// the rows are split across threads, so a local and a server render of the
// same request agree pixel for pixel.
struct RaycastSettings {
    OrthoCamera camera;
    // The box the camera is normalised to (the dataset's sample bounds), so
    // the wireframe drawn over the frame with the same camera lines up; it is
    // not necessarily the grid's region.
    RealBox domain;
    std::array<int, 2> outputSize{0, 0};
    VolumeRange range;
    VolumeTransferFunction transfer;
    // Ray samples per voxel: the step is the smallest voxel pitch divided by
    // this, and each sample's opacity is corrected so a voxel contributes its
    // entry's opacity once whatever the step.
    int samplesPerVoxel = 2;
    unsigned threadCount = 0;   // 0 = std::thread::hardware_concurrency()
};

// Renders the grid; the frame's usedRange is settings.range and its metrics
// carry only the render time. Throws std::invalid_argument for inconsistent
// settings or a malformed grid, ReadCancelled when the token stops.
[[nodiscard]] VolumeFrame raycastVolume(const VolumeGrid& grid,
    const RaycastSettings& settings, StopToken cancellation = {});

// The finite extrema of the grid's values (of its positive values when
// logarithmic), for resolving a "Visible" range; nullopt when there are
// none. Possibly degenerate (minimum == maximum): the caller pads.
[[nodiscard]] std::optional<std::pair<double, double>> volumeGridRange(
    const VolumeGrid& grid, bool logarithmic);

// The transfer-function entry a value maps to under a range: entry 0 at or
// below the minimum, the last at or above the maximum, truncation between
// (the mapping renderScalarPlane uses for its palette slots, so volume
// colours agree with the slices'); nullopt for a value the range cannot map
// (non-finite, or non-positive under a logarithmic range).
[[nodiscard]] std::optional<int> transferEntryFor(double value,
    const VolumeRange& range, int entryCount) noexcept;

} // namespace amrvis
