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
    // Ray samples per voxel: the step is the mean distance a view ray spends
    // crossing one voxel divided by this, and each sample's opacity is
    // corrected so a voxel contributes its entry's opacity once whatever the
    // step. Measuring the step along the view rather than by the smallest
    // pitch is what keeps that true of an anisotropic grid, where a coarse
    // axis would otherwise be sampled many times per voxel and come out far
    // more opaque than its entry asks for.
    int samplesPerVoxel = 2;
    // 0 = std::thread::hardware_concurrency(); bounded above by the row count
    // and by a small multiple of the hardware's, so an outsized request costs
    // no more than a sensible one.
    unsigned threadCount = 0;
};

// Renders the grid; the frame's usedRange is settings.range and its metrics
// carry the render time and what the grid reports about itself (its dims,
// covered voxels and finest sampled level) -- the sampling and cache fields
// belong to whoever produced the grid. Throws std::invalid_argument for
// inconsistent settings or a malformed grid, ReadCancelled when the token
// stops.
[[nodiscard]] VolumeFrame raycastVolume(const VolumeGrid& grid,
    const RaycastSettings& settings, StopToken cancellation = {});

// The finite extrema of the grid's values (of its positive values when
// logarithmic), for resolving a "Visible" range; nullopt when there are
// none. Possibly degenerate (minimum == maximum): the caller pads. Scans the
// whole grid, so it takes a token and throws ReadCancelled like the render.
[[nodiscard]] std::optional<std::pair<double, double>> volumeGridRange(
    const VolumeGrid& grid, bool logarithmic, StopToken cancellation = {});

// The threads raycastVolume splits the rows across for a frame of this
// height, given settings.threadCount (0 = hardware_concurrency): bounded by
// the row count and by a small multiple of the hardware's, so an outsized
// request costs no more than a sensible one. Exposed so a caller reporting
// its own timings names the count the render actually used.
[[nodiscard]] int raycastThreadCount(unsigned requested, int height) noexcept;

// The transfer-function entry a value maps to under a range: entry 0 at or
// below the minimum, the last at or above the maximum, truncation between
// (the mapping renderScalarPlane uses for its palette slots, so volume
// colours agree with the slices'); nullopt for a value the range cannot map
// (non-finite, or non-positive under a logarithmic range) and for a range
// that can map nothing (a non-finite bound, an empty or infinite span, or a
// logarithmic range reaching to zero).
[[nodiscard]] std::optional<int> transferEntryFor(double value,
    const VolumeRange& range, int entryCount) noexcept;

} // namespace amrvis
