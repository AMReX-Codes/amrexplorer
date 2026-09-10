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
// same request agree pixel for pixel -- for the same build. The compositing
// runs through std::pow and std::log, which no standard requires to be
// correctly rounded, so two libm implementations (or two versions of one)
// can differ by an ulp, and an ulp in a step opacity can compound over a few
// hundred composites into one 8-bit level. Pixel equality across a
// heterogeneous deployment is not a promise this can keep. The isosurface
// shading adds no second such term: its shininess is an integer power taken
// by repeated multiplication, and std::hypot and std::sqrt are correctly
// rounded.
//
// An isosurface, when asked for, is marched in the same pass: each ray
// watches where a second grid's trilinear value crosses the iso-value between
// consecutive samples, shades the crossing from the field's gradient under a
// fixed headlight, and composites it in depth order with the volume samples.
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
    // How a sample reads the grid. Linear is trilinear over the eight voxel
    // centres bracketing the sample, which is what stops a ray from fetching
    // one voxel repeatedly and terracing the picture; Nearest and
    // PiecewiseConstant both take the voxel the sample lands in, the rule the
    // 2-D slice shows cells with. The names are the slice's, so one vocabulary
    // covers both -- see SamplingPolicy in core/Request.hpp.
    SamplingPolicy sampling = SamplingPolicy::Linear;
    // 0 = std::thread::hardware_concurrency(); bounded above by the row count
    // and by a small multiple of the hardware's, so an outsized request costs
    // no more than a sensible one.
    unsigned threadCount = 0;
    // Whether the volume grid is composited at all; off, only the isosurface
    // draws, and the range and transfer above merely have to be valid.
    bool showVolume = true;
    // The surface to draw, if any: its value, colour and opacity. Its field
    // and component name the grid handed in as RaycastGrids::isosurface and
    // are the caller's business; the march never reads them. Set exactly when
    // that grid is.
    std::optional<VolumeIsosurface> isosurface;
};

// Fixed headlight: light, viewer and half vector coincide, so every term is a
// power of the cosine between the normal and the view. Exposed so a test or a
// swatch can compute the colour a hit composites.
inline constexpr double isosurfaceAmbient = 0.25;
inline constexpr double isosurfaceDiffuse = 0.65;
inline constexpr double isosurfaceSpecular = 0.20;   // white
inline constexpr int isosurfaceShininess = 32;
// Bisections of the trilinear field between the two samples bracketing a
// crossing before the hit is placed by one secant step within the bracket.
inline constexpr int isosurfaceRefinementSteps = 2;

// The grids a render reads. Non-owning: the caller keeps both alive for the
// call. `volume` is required exactly when settings.showVolume, `isosurface`
// exactly when settings.isosurface is set; when both are given their dims
// and region must be identical, which a session guarantees by sampling both
// through one VolumeSampleRequest geometry. They may be the same object.
struct RaycastGrids {
    const VolumeGrid* volume = nullptr;
    const VolumeGrid* isosurface = nullptr;
};

// Renders the grids; the frame's usedRange is settings.range and its metrics
// carry the render time and what the reference grid (the volume when shown,
// else the isosurface's) reports about itself -- its dims, covered voxels and
// finest sampled level; the sampling and cache fields belong to whoever
// produced the grid. Throws std::invalid_argument for inconsistent settings
// or a malformed grid, ReadCancelled when the token stops.
[[nodiscard]] VolumeFrame raycastVolume(const RaycastGrids& grids,
    const RaycastSettings& settings, StopToken cancellation = {});

// The volume alone, with no isosurface: what every caller before isosurfaces
// existed means, and what settings default to.
[[nodiscard]] inline VolumeFrame raycastVolume(const VolumeGrid& grid,
    const RaycastSettings& settings, StopToken cancellation = {})
{
    return raycastVolume(
        RaycastGrids{&grid, nullptr}, settings, std::move(cancellation));
}

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

// The transfer-function entry a value maps to under a range, for callers
// mapping a single value -- a colour bar, a readout, a test. The march does
// not use it: it resolves the range once and calls valueSlot per sample,
// which is what this wraps (core/ValueMapping.hpp owns the mapping, so the
// volume and the slice cannot disagree). Resolving the range takes the
// logarithm of both bounds, so this is the slow form: hold a
// ResolvedValueRange yourself if you are mapping more than a few values.
//
// Entry 0 at or below the minimum, the last at or above the maximum,
// truncation between; nullopt for a value the range cannot map
// (non-finite, or non-positive under a logarithmic range) and for a range
// that can map nothing (a non-finite bound, an empty span, or a
// logarithmic range reaching to zero).
[[nodiscard]] std::optional<int> transferEntryFor(double value,
    const VolumeRange& range, int entryCount) noexcept;

} // namespace amrvis
