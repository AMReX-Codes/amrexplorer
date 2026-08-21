#include <amrexplorer/pipeline/VolumePipeline.hpp>

#include <amrexplorer/cache/ByteLruCache.hpp>
#include <amrexplorer/core/Statistics.hpp>
#include <amrexplorer/pipeline/SlicePipeline.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <tuple>

namespace amrvis {

VolumeTransferFunction makeVolumeTransferFunction(
    const Palette& palette, const OpacityRamp& ramp)
{
    VolumeTransferFunction transfer;
    constexpr int entryCount = Palette::colorSlots;
    transfer.colors.reserve(static_cast<std::size_t>(entryCount));
    transfer.opacities.reserve(static_cast<std::size_t>(entryCount));
    // Refused rather than clamped: std::clamp returns a NaN unchanged (it
    // compares, and every comparison against a NaN is false), and the casts
    // to int below are undefined for one. A control that is not a number is
    // a caller error, not a value to guess at.
    if (!std::isfinite(ramp.lowThreshold) || !std::isfinite(ramp.highThreshold)
        || !std::isfinite(ramp.maximumOpacity)) {
        throw std::invalid_argument(
            "opacity ramp thresholds and maximum must be finite");
    }
    // An inverted window is refused rather than treated as the sub-pitch
    // case. The two are indistinguishable once the entries are computed --
    // both land in the highEntry < lowEntry branch -- so a caller that wired
    // its two sliders the wrong way round would get one opaque shell at the
    // midpoint and a picture that looks deliberate, with 60% of the range
    // silently discarded and nothing to tell them.
    if (ramp.lowThreshold > ramp.highThreshold) {
        throw std::invalid_argument(
            "the opacity ramp's low threshold is above its high threshold");
    }
    const auto low = std::clamp(ramp.lowThreshold, 0.0, 1.0);
    const auto high = std::clamp(ramp.highThreshold, 0.0, 1.0);
    const auto maximum = std::clamp(ramp.maximumOpacity, 0.0, 1.0);
    const bool paletteAlpha = ramp.usePaletteAlpha && palette.hasAlphaRamp();
    // The window in entries: those whose position entry / (n - 1) lies in
    // [low, high], ramping over that span so the top entry inside always
    // reaches the maximum. A window narrower than the entry pitch (the
    // coupled sliders make low == high reachable) would select nothing and
    // render a blank volume; it selects the entry nearest its centre
    // instead, fully opaque -- a thin shell at that value.
    const auto last = static_cast<double>(entryCount - 1);
    constexpr double slack = 1.0e-9;
    int lowEntry = static_cast<int>(std::ceil(low * last - slack));
    int highEntry = static_cast<int>(std::floor(high * last + slack));
    if (highEntry < lowEntry) {
        lowEntry = std::clamp(
            static_cast<int>(std::lround(0.5 * (low + high) * last)), 0,
            entryCount - 1);
        highEntry = lowEntry;
    }
    for (int entry = 0; entry < entryCount; ++entry) {
        const auto slot = Palette::paletteStart + entry;
        transfer.colors.push_back(palette.slotArgb(slot) & 0x00FFFFFFU);
        double opacity = 0.0;
        if (entry >= lowEntry && entry <= highEntry) {
            if (highEntry == lowEntry) {
                // The sub-pitch window, whatever the alpha source. Taking the
                // palette's stored alpha here would make the single selected
                // entry as faint as that slot happens to be -- 5% on a typical
                // ramp -- so the "thin shell at that value" the header
                // promises would be the only visible entry and invisible.
                opacity = 1.0;
            } else if (paletteAlpha) {
                opacity = palette.opacity(slot);
            } else {
                opacity = static_cast<double>(entry - lowEntry)
                    / static_cast<double>(highEntry - lowEntry);
            }
        }
        transfer.opacities.push_back(
            static_cast<float>(std::clamp(opacity * maximum, 0.0, 1.0)));
    }
    return transfer;
}

std::optional<VolumeRange> resolveVolumeRange(
    const std::shared_ptr<DatasetSession>& dataset, FieldId field,
    int maximumLevel, CompositionPolicy composition, RangeMode rangeMode,
    const std::optional<std::pair<double, double>>& userRange,
    bool logarithmic, StopToken cancellation)
{
    std::optional<std::pair<double, double>> selected;
    if (rangeMode == RangeMode::User) {
        selected = userRange;
    } else if (rangeMode == RangeMode::File || rangeMode == RangeMode::Level) {
        // No fabDataRange fallback here, unlike the slice path: a FAB has no
        // volume to render (supportsVolumeRendering requires !isFab), so the
        // call would return nullopt for every dataset that can reach this.
        const auto statistics = dataset->requestRange(
            RangeRequest{.field = field,
                .maximumLevel = maximumLevel,
                .composition = composition,
                .scope = rangeMode == RangeMode::File ? RangeScope::File
                                                      : RangeScope::Level},
            cancellation);
        if (statistics) {
            selected = std::pair{statistics->minimum, statistics->maximum};
        }
    }
    if (!selected) {
        return std::nullopt;   // Visible, or statistics unavailable: the renderer decides
    }
    // Named for where it came from: saying "user" for a range read out of
    // level statistics sends the reader to a control that is not the cause.
    const auto* source = rangeMode == RangeMode::User ? "user"
        : rangeMode == RangeMode::File ? "file" : "level";
    auto [minimum, maximum]
        = paddedIfDegenerate(selected->first, selected->second, logarithmic);
    if (!(minimum < maximum)) {
        throw std::runtime_error(
            std::string(source) + " scalar range must have positive extent");
    }
    // Checked here rather than left to the renderer: this function decides
    // the range, so a range the renderer must refuse -- a span so wide it is
    // infinite, which validateVolumeRenderRequest rejects -- is this
    // function's error to report, not an invalid_argument out of the middle
    // of a render.
    if (!std::isfinite(minimum) || !std::isfinite(maximum)
        || !std::isfinite(maximum - minimum)) {
        throw std::runtime_error(
            std::string(source) + " scalar range must be finite with a finite span");
    }
    if (logarithmic && minimum > 0.0) {
        return VolumeRange{minimum, maximum, true};
    }
    if (logarithmic) {
        // Not viable in log: linear, re-padded without the log rule.
        std::tie(minimum, maximum)
            = paddedIfDegenerate(selected->first, selected->second, false);
    }
    return VolumeRange{minimum, maximum, false};
}

std::uint64_t volumeResponseBytes(std::array<int, 2> outputSize)
{
    return static_cast<std::uint64_t>(std::max(0, outputSize[0]))
        * static_cast<std::uint64_t>(std::max(0, outputSize[1]))
        * sizeof(std::uint32_t) + volumeResponseOverheadBytes;
}

std::array<int, 2> frameBudgetBoundedVolumeSize(
    std::array<int, 2> outputSize,
    std::optional<std::uint32_t> maximumResponseBytes)
{
    if (!maximumResponseBytes) {
        return outputSize;
    }
    const auto budget = static_cast<std::uint64_t>(*maximumResponseBytes);
    // Refused, not silently bounded to {1, 1}: a budget that cannot hold one
    // pixel would produce a request the far side rejects anyway, and saying
    // so here names the real problem instead of failing later as an
    // oversized frame.
    if (budget < volumeResponseBytes({1, 1})) {
        throw std::invalid_argument(
            "the negotiated response budget cannot hold a single volume pixel");
    }
    const auto maximumPixels = (budget - volumeResponseOverheadBytes)
        / sizeof(std::uint32_t);
    auto bounded = outputSize;
    const auto pixels = static_cast<std::uint64_t>(std::max(1, bounded[0]))
        * static_cast<std::uint64_t>(std::max(1, bounded[1]));
    if (pixels <= maximumPixels) {
        return bounded;
    }
    const auto scale = std::sqrt(static_cast<double>(maximumPixels)
        / static_cast<double>(pixels));
    bounded = {std::max(1, static_cast<int>(std::floor(bounded[0] * scale))),
        std::max(1, static_cast<int>(std::floor(bounded[1] * scale)))};
    while (static_cast<std::uint64_t>(bounded[0])
            * static_cast<std::uint64_t>(bounded[1]) > maximumPixels) {
        auto& larger = bounded[0] >= bounded[1] ? bounded[0] : bounded[1];
        if (larger <= 1) {
            break;
        }
        --larger;
    }
    return bounded;
}

namespace {

VolumeDisplayResult renderWithFallback(
    const std::shared_ptr<DatasetSession>& dataset, VolumeRenderRequest request,
    const std::optional<VolumeRangeChoice>& choice, StopToken cancellation)
{
    request.outputSize = frameBudgetBoundedVolumeSize(
        request.outputSize, dataset->maximumResponseBytes());
    int fallbackFrom = -1;
    int fallbackTo = -1;
    for (;;) {
        try {
            if (choice) {
                // Per attempt, not once: request.maximumLevel is what the
                // loop lowers, and a File or Level range is a property of it.
                request.logarithmic = choice->logarithmic;
                request.range = resolveVolumeRange(dataset, request.field,
                    request.maximumLevel, request.composition, choice->mode,
                    choice->userRange, choice->logarithmic, cancellation);
            }
            auto frame = dataset->renderVolume(request, cancellation);
            if (fallbackFrom >= 0) {
                // Only when this loop drove one. The frame arrives carrying
                // whatever fallback the session itself made -- a remote
                // server under its own cache pressure reports one -- and
                // stamping -1 over it would hide from the user that their
                // volume was rendered coarser than they asked. When both
                // happened, the span runs from where this loop started to
                // the coarsest level actually used.
                const auto sessionTo = frame.cacheFallbackToLevel;
                frame.cacheFallbackFromLevel = fallbackFrom;
                frame.cacheFallbackToLevel = sessionTo >= 0
                    ? std::min(sessionTo, fallbackTo) : fallbackTo;
            }
            // VolumeDisplayResult::request is documented as the request "as
            // rendered, after any fallback", and the session may have fallen
            // back inside an attempt this loop never retried -- a remote
            // server under its own cache pressure does exactly that. Without
            // this the caller's own state stays at the level it asked for
            // while the pixels came from a coarser one.
            if (frame.cacheFallbackToLevel >= 0) {
                request.maximumLevel = frame.cacheFallbackToLevel;
            }
            return {request, std::move(frame)};
        } catch (const CacheBudgetExceeded&) {
            const auto budget = cacheBudgetDescription(
                dataset->cacheMetrics().budgetBytes);
            if (request.composition != CompositionPolicy::FinestAvailable) {
                throw std::runtime_error(
                    "The selected volume level cannot fit in the " + budget
                    + " cache. Choose a lower level or increase "
                      "AMREXPLORER_CACHE_SIZE_MB.");
            }
            if (request.maximumLevel == 0) {
                throw std::runtime_error(
                    "The volume cannot fit in the " + budget
                    + " cache, even at level 0. Try a smaller plotfile or "
                      "increase AMREXPLORER_CACHE_SIZE_MB.");
            }
            dataset->clearUnpinnedCache();
            if (fallbackFrom < 0) {
                fallbackFrom = request.maximumLevel;
            }
            fallbackTo = --request.maximumLevel;
        }
    }
}

} // namespace

VolumeDisplayResult executeVolumeRenderWithFallback(
    const std::shared_ptr<DatasetSession>& dataset, VolumeRenderRequest request,
    const VolumeRangeChoice& range, StopToken cancellation)
{
    return renderWithFallback(dataset, std::move(request), range, cancellation);
}

VolumeDisplayResult executeVolumeRenderWithFallback(
    const std::shared_ptr<DatasetSession>& dataset, VolumeRenderRequest request,
    StopToken cancellation)
{
    return renderWithFallback(
        dataset, std::move(request), std::nullopt, cancellation);
}

} // namespace amrvis
