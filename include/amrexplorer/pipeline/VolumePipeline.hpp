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

namespace amrvis {

// The GUI's side of volume rendering, the analogue of SlicePipeline: it
// turns the palette and the opacity controls into the request's transfer
// function, resolves the range the same way a slice would, keeps a remote
// frame under the negotiated size, and retries a render that overran the
// block cache at a coarser level. Qt-free.

// The opacity controls: a window over the colour range (in normalised
// [0, 1] of the range) that ramps linearly from transparent at the low
// threshold to maximumOpacity at the high one -- or, with usePaletteAlpha
// and a palette that carries an alpha ramp, the palette's own per-slot
// opacity, windowed and scaled the same way. A window too narrow to hold
// an entry (the thresholds may be equal) selects the one nearest it, at
// full opacity.
struct OpacityRamp {
    double lowThreshold = 0.0;
    double highThreshold = 1.0;
    double maximumOpacity = 1.0;
    bool usePaletteAlpha = false;
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

// The bytes a rendered frame of this size costs on the wire, and the size
// shrunk (uniformly, aspect kept) until that fits the session's response
// budget; unchanged for a local session.
inline constexpr std::uint64_t volumeResponseOverheadBytes = 4096;
[[nodiscard]] std::uint64_t volumeResponseBytes(std::array<int, 2> outputSize);
[[nodiscard]] std::array<int, 2> frameBudgetBoundedVolumeSize(
    std::array<int, 2> outputSize,
    std::optional<std::uint32_t> maximumResponseBytes);

struct VolumeDisplayResult {
    VolumeRenderRequest request;   // as rendered, after any fallback
    VolumeFrame frame;
};

// renderVolume with the slice pipeline's cache-pressure fallback: a
// CacheBudgetExceeded from the block cache lowers the composite level and
// retries (finest-available only), reporting the fallback in the frame; an
// exact level or level 0 that cannot fit throws an actionable message.
[[nodiscard]] VolumeDisplayResult executeVolumeRenderWithFallback(
    const std::shared_ptr<DatasetSession>& dataset, VolumeRenderRequest request,
    StopToken cancellation = {});

} // namespace amrvis
