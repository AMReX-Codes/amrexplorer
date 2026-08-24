#pragma once

#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/core/Volume.hpp>
#include <amrexplorer/data/DatasetSession.hpp>
#include <amrexplorer/pipeline/SliceRangeResolver.hpp>
#include <amrexplorer/render2d/Palette.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace amrvis {

// The GUI's side of volume rendering, the analogue of SlicePipeline: it
// turns the palette and the opacity controls into the request's transfer
// function, resolves the range the same way a slice would, keeps a remote
// frame under the negotiated size, and retries a render that overran the
// block cache at a coarser level. Qt-free.

// One control point of an opacity curve: `opacity` at `position`, both in
// [0, 1] -- position over the colour range, opacity from transparent to
// opaque.
struct OpacityPoint {
    double position = 0.0;
    double opacity = 0.0;
    friend bool operator==(const OpacityPoint&, const OpacityPoint&) = default;
};

// The two points that reproduce the default window: transparent at the bottom
// of the range, opaque at the top.
[[nodiscard]] std::vector<OpacityPoint> defaultOpacityCurve();

// The curve's value at `position`, linearly between the points either side of
// it and flat outside the outermost ones. An empty curve is transparent
// everywhere; the points are assumed sorted, which every editing function
// below maintains.
[[nodiscard]] double opacityCurveValue(
    const std::vector<OpacityPoint>& curve, double position);

// Editing, as pure functions on the point list, because this is where the
// mistakes live: a point dragged past its neighbour, an end point dragged off
// the range, a curve left unsorted for opacityCurveValue to read.
//
// insert: a new point, keeping the list sorted; returns its index. On a curve
//   with two ends it always lands between them -- its position is held to the
//   span they cover and the returned index is never 0 or the last -- so the
//   ends stay the ends: they are what makes the curve span the range, and an
//   end that arrived by insertion could not be removed again.
// move: point `index` to (position, opacity), clamped into [0, 1] and, for an
//   interior point, between its neighbours. The two end points keep their
//   positions -- the curve has to span the range -- and move only in opacity.
// remove: point `index`, unless it is an end point or the curve would be left
//   with fewer than two.
[[nodiscard]] std::size_t insertOpacityPoint(
    std::vector<OpacityPoint>& curve, double position, double opacity);
void moveOpacityPoint(std::vector<OpacityPoint>& curve, std::size_t index,
    double position, double opacity);
bool removeOpacityPoint(std::vector<OpacityPoint>& curve, std::size_t index);

// The opacity controls: a window over the colour range (in normalised
// [0, 1] of the range) that ramps linearly from transparent at the low
// threshold to maximumOpacity at the high one -- or, with usePaletteAlpha
// and a palette that carries an alpha ramp, the palette's own per-slot
// opacity, windowed and scaled the same way. A window too narrow to hold
// an entry (the thresholds may be equal) selects the one nearest it, at
// full opacity.
//
// A shaped curve replaces the window when one is given: see `curve` below.
struct OpacityRamp {
    double lowThreshold = 0.0;
    double highThreshold = 1.0;
    double maximumOpacity = 1.0;
    bool usePaletteAlpha = false;
    // Control points of a piecewise-linear opacity curve over the same
    // normalised range, in ascending position. Non-empty replaces the window
    // and the maximum above -- it says the same kind of thing with more than
    // three numbers -- and empty leaves that path exactly as it was, which is
    // what keeps a caller that never touches a curve rendering what it did.
    //
    // usePaletteAlpha wins over it: the two are alternative sources rather
    // than layers, and a palette's authored ramp is handed back as authored.
    std::vector<OpacityPoint> curve;
    friend bool operator==(const OpacityRamp&, const OpacityRamp&) = default;
};

// One entry per palette data slot (Palette::colorSlots), so a value maps to
// the same colour the slices and the colour bar show for it.
[[nodiscard]] VolumeTransferFunction makeVolumeTransferFunction(
    const Palette& palette, const OpacityRamp& ramp);

// The range for the request: File/Level statistics or the User range,
// padded if degenerate and downgraded from logarithmic when the range is
// not strictly positive (as resolveDisplayRange does); nullopt for the
// Visible mode, which the renderer resolves from the sampled grid itself
// (a remote session's grid never leaves the server).
[[nodiscard]] std::optional<VolumeRange> resolveVolumeRange(
    const std::shared_ptr<DatasetSession>& dataset, FieldId field,
    int maximumLevel, CompositionPolicy composition, RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, StopToken cancellation = {});

// How the range is resolved, for the fallback path below, which resolves it
// afresh for whatever level each attempt is about to render.
//
// This matters for one mode: RangeMode::Level, whose statistics are read per
// level. Level 2's are not level 0's, so a range resolved once up front
// would colour a fallen-back frame, and label its colour bar, with numbers no
// part of the volume it drew came from. The slice path avoids the same trap
// by resolving inside its own retry loop.
//
// The other modes do not move with the level and are never worth a repeat:
// File asks metadataValueRange for the whole file and ignores the level
// entirely, User is the caller's own pair, and Visible is resolved by the
// renderer from the grid it actually sampled, so it follows a fallback on its
// own.
//
// A fallback the *session* made counts as well as one the loop drove: a
// remote server that renders coarser under its own cache pressure reports the
// level it used, and that level is always carried into the result. For a
// Level range the render is then repeated for it, so the range belongs to the
// pixels -- an extra render, on a path that only runs under cache pressure.
struct VolumeRangeChoice {
    RangeMode mode = RangeMode::Visible;
    std::optional<std::pair<double, double>> userRange;
    bool logarithmic = false;
};

// The bytes a rendered frame of this size costs on the wire, and the size
// shrunk (uniformly, aspect kept) until that fits the session's response
// budget; unchanged for a local session.
[[nodiscard]] std::uint64_t volumeResponseBytes(std::array<int, 2> outputSize);
[[nodiscard]] std::array<int, 2> frameBudgetBoundedVolumeSize(
    std::array<int, 2> outputSize,
    std::optional<std::uint32_t> maximumResponseBytes);

struct VolumeDisplayResult {
    // As rendered, after any fallback -- its maximumLevel is the level the
    // pixels came from, not the one asked for.
    //
    // Which means it is NOT the request to hand validateSessionVolumeResult.
    // That validator checks a peer's frame against the request that was sent
    // it, and derives the finest allowed level from request.maximumLevel; a
    // frame reporting a fallback from 2 to 0 paired with this request (level
    // 0) fails its `from > highest` test, because from is 2. The two carry
    // the same name and mean different things: the validator wants the
    // original, this is the outcome.
    VolumeRenderRequest request;
    VolumeFrame frame;
};

// renderVolume with the slice pipeline's cache-pressure fallback: a
// CacheBudgetExceeded from the block cache lowers the composite level and
// retries (finest-available only), reporting the fallback in the frame; an
// exact level or level 0 that cannot fit throws an actionable message.
//
// The overload taking a VolumeRangeChoice resolves the range for each attempt
// and is the one to use for a Level range. The overload without one takes
// request.range as final, which is correct for every other mode -- see
// VolumeRangeChoice above for why none of them moves with the level.
[[nodiscard]] VolumeDisplayResult executeVolumeRenderWithFallback(
    const std::shared_ptr<DatasetSession>& dataset, VolumeRenderRequest request,
    const VolumeRangeChoice& range, StopToken cancellation = {});
[[nodiscard]] VolumeDisplayResult executeVolumeRenderWithFallback(
    const std::shared_ptr<DatasetSession>& dataset, VolumeRenderRequest request,
    StopToken cancellation = {});

} // namespace amrvis
