#include <amrexplorer/pipeline/VolumePipeline.hpp>

#include <amrexplorer/cache/ByteLruCache.hpp>
#include <amrexplorer/core/Statistics.hpp>
#include <amrexplorer/core/ValueMapping.hpp>
#include <amrexplorer/pipeline/SlicePipeline.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <tuple>

namespace amrvis {

std::vector<OpacityPoint> defaultOpacityCurve()
{
    return {OpacityPoint{0.0, 0.0}, OpacityPoint{1.0, 1.0}};
}

double opacityCurveValue(
    const std::vector<OpacityPoint>& curve, double position)
{
    if (curve.empty()) {
        return 0.0;
    }
    if (!(position > curve.front().position)) {
        return curve.front().opacity;
    }
    if (!(position < curve.back().position)) {
        return curve.back().opacity;
    }
    for (std::size_t index = 1; index < curve.size(); ++index) {
        const auto& upper = curve[index];
        if (position > upper.position) {
            continue;
        }
        const auto& lower = curve[index - 1];
        const auto span = upper.position - lower.position;
        if (!(span > 0.0)) {
            // Guards the division, nothing more. Two points at one position
            // form a step, and the segment *ending* at that position is found
            // first, so a query exactly there returns through the branch below
            // with span > 0 and this is not reached from any curve the editing
            // functions can build. Left in because the alternative is a
            // division by zero if those early returns ever change.
            return upper.opacity;
        }
        const auto fraction = (position - lower.position) / span;
        return lower.opacity + fraction * (upper.opacity - lower.opacity);
    }
    return curve.back().opacity;
}

std::size_t insertOpacityPoint(
    std::vector<OpacityPoint>& curve, double position, double opacity)
{
    const OpacityPoint point{std::clamp(position, 0.0, 1.0),
        std::clamp(opacity, 0.0, 1.0)};
    const auto at = std::upper_bound(curve.begin(), curve.end(), point,
        [](const OpacityPoint& left, const OpacityPoint& right) {
            return left.position < right.position;
        });
    const auto index = static_cast<std::size_t>(at - curve.begin());
    curve.insert(at, point);
    return index;
}

void moveOpacityPoint(std::vector<OpacityPoint>& curve, std::size_t index,
    double position, double opacity)
{
    if (index >= curve.size()) {
        return;
    }
    curve[index].opacity = std::clamp(opacity, 0.0, 1.0);
    // The end points anchor the curve to the ends of the range, so they give
    // up their position: a curve that stopped short of either end would have
    // to invent a value out there, and opacityCurveValue holds the outermost
    // one flat precisely so it never has to.
    if (index == 0 || index + 1 == curve.size()) {
        return;
    }
    curve[index].position = std::clamp(position,
        curve[index - 1].position, curve[index + 1].position);
}

bool removeOpacityPoint(std::vector<OpacityPoint>& curve, std::size_t index)
{
    if (curve.size() <= 2 || index == 0 || index + 1 >= curve.size()) {
        return false;
    }
    curve.erase(curve.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

VolumeTransferFunction makeVolumeTransferFunction(
    const Palette& palette, const OpacityRamp& ramp)
{
    VolumeTransferFunction transfer;
    constexpr int entryCount = Palette::colorSlots;
    transfer.colors.reserve(static_cast<std::size_t>(entryCount));
    transfer.opacities.reserve(static_cast<std::size_t>(entryCount));
    if (!ramp.curve.empty()) {
        // The curve is the opacity, entry by entry. usePaletteAlpha still
        // substitutes the palette's own per-slot alpha, as it does for the
        // window below, and the curve then says only where opacity is zero --
        // so a palette ramp is shaped by where the curve has been pulled to
        // nothing, not multiplied into a different ramp.
        const bool paletteAlpha = ramp.usePaletteAlpha && palette.hasAlphaRamp();
        const auto last = static_cast<double>(entryCount - 1);
        for (int entry = 0; entry < entryCount; ++entry) {
            const auto slot = Palette::paletteStart + entry;
            transfer.colors.push_back(palette.slotArgb(slot) & 0x00FFFFFFU);
            const auto shaped = opacityCurveValue(
                ramp.curve, static_cast<double>(entry) / last);
            const auto opacity = paletteAlpha
                ? (shaped > 0.0 ? palette.opacity(slot) : 0.0)
                : shaped;
            transfer.opacities.push_back(
                static_cast<float>(std::clamp(opacity, 0.0, 1.0)));
        }
        return transfer;
    }
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
    VolumeRange resolved{minimum, maximum, false};
    if (logarithmic && minimum > 0.0) {
        resolved = VolumeRange{minimum, maximum, true};
    } else if (logarithmic) {
        // Not viable in log: linear, re-padded without the log rule.
        std::tie(resolved.minimum, resolved.maximum)
            = paddedIfDegenerate(selected->first, selected->second, false);
    }
    // The last thing the renderers do with a range is resolveValueRange, so
    // one it cannot resolve is this function's error to report. Ordering and
    // finiteness are already checked above; what is left is a logarithmic
    // range whose two bounds share a logarithm -- adjacent doubles up at
    // 1e300 do -- which has no span to spread the entries over and which
    // validateVolumeRenderRequest would otherwise refuse from inside the
    // render, as an invalid_argument rather than a named range error.
    if (!resolveValueRange(
            resolved.minimum, resolved.maximum, resolved.logarithmic)) {
        throw std::runtime_error(std::string(source)
            + " scalar range is too narrow to map: its bounds share a logarithm");
    }
    return resolved;
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
    // pixel would produce a request the far side rejects anyway. The slice
    // path returns {1, 1} for its own hopeless budget and gets away with it
    // because its overhead is 512 bytes, so a one-pixel slice still fits
    // inside a small budget; a one-pixel volume frame costs 4100 and does
    // not. A runtime_error with the numbers in it, because this is the same
    // kind of "your configuration cannot do this" message the cache-pressure
    // refusals below carry, and it reaches the user the same way.
    if (budget < volumeResponseBytes({1, 1})) {
        throw std::runtime_error("The negotiated response budget of "
            + std::to_string(budget) + " bytes cannot hold a single volume "
            "pixel; volume rendering needs at least "
            + std::to_string(volumeResponseBytes({1, 1}))
            + " bytes per frame. Raise the frame-bytes limit or render "
              "slices instead.");
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
    // Whether the range moves with the level, i.e. whether a fallback makes
    // the resolved range wrong and the render worth repeating. Only Level
    // does: Visible is resolved by the renderer from the grid it sampled, so
    // it follows on its own; User does not depend on the level at all; and
    // RangeScope::File asks metadataValueRange for the whole file
    // (LocalDatasetSession.cpp passes std::nullopt for the level), so it
    // returns the same bounds whatever the fallback did. Repeating for File
    // would cost a second render, and a second round trip to a server, to
    // arrive at the range it already had.
    const bool levelDependentRange = choice && choice->mode == RangeMode::Level;

    // Work done by an attempt whose frame is then discarded. The sampling and
    // the payload reads happened and cost what they cost; the repeat runs
    // against the grid the discarded attempt just cached, so it reports none
    // of it, and the caller would be told a render that read the plotfile
    // read nothing. Structural fields come from the attempt that produced the
    // pixels; only the counters and the timers accumulate.
    VolumeRenderMetrics carried;
    const auto addWork = [](VolumeRenderMetrics& into,
                             const VolumeRenderMetrics& from) {
        into.sampleMicroseconds += from.sampleMicroseconds;
        into.renderMicroseconds += from.renderMicroseconds;
        into.candidateBlocks += from.candidateBlocks;
        into.blocksRead += from.blocksRead;
        into.cacheHits += from.cacheHits;
        into.payloadBytesRead += from.payloadBytesRead;
    };
    for (;;) {
        try {
            if (choice) {
                // Per attempt, not once: request.maximumLevel is what the
                // loop lowers, and a Level range is read per level. The other
                // modes resolve to the same range every time round, which
                // costs a statistics lookup and nothing else -- only the
                // repeat below is worth gating on the mode.
                request.logarithmic = choice->logarithmic;
                request.range = resolveVolumeRange(dataset, request.field,
                    request.maximumLevel, request.composition, choice->mode,
                    choice->userRange, choice->logarithmic, cancellation);
            }
            auto frame = dataset->renderVolume(request, cancellation);
            // A fallback the session made inside this attempt counts the same
            // as one this loop drove. A remote server under its own cache
            // pressure renders coarser and says so, and the range that was
            // resolved a few lines above belongs to the level we asked for,
            // not the one it used -- level 1's statistics are not level 0's.
            // So the level is taken from the frame and, when the range
            // depends on it, the render is repeated for the level that
            // actually happened. Each repeat is strictly coarser, so this
            // walks down to level 0 at worst.
            const auto sessionTo = frame.cacheFallbackToLevel;
            if (sessionTo >= 0 && sessionTo < request.maximumLevel) {
                if (fallbackFrom < 0) {
                    fallbackFrom = frame.cacheFallbackFromLevel >= 0
                        ? frame.cacheFallbackFromLevel : request.maximumLevel;
                }
                fallbackTo = sessionTo;
                request.maximumLevel = sessionTo;
                if (levelDependentRange) {
                    addWork(carried, frame.metrics);
                    continue;
                }
            }
            addWork(frame.metrics, carried);
            if (fallbackFrom >= 0) {
                // The span runs from where the first fallback started to the
                // coarsest level anything actually rendered at.
                frame.cacheFallbackFromLevel = fallbackFrom;
                frame.cacheFallbackToLevel = fallbackTo;
                // VolumeDisplayResult::request is documented as the request
                // "as rendered, after any fallback".
                request.maximumLevel = fallbackTo;
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
