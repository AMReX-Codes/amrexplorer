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
    const auto low = std::clamp(ramp.lowThreshold, 0.0, 1.0);
    const auto high = std::clamp(ramp.highThreshold, 0.0, 1.0);
    const auto maximum = std::clamp(ramp.maximumOpacity, 0.0, 1.0);
    const bool paletteAlpha = ramp.usePaletteAlpha && palette.hasAlphaRamp();
    for (int entry = 0; entry < entryCount; ++entry) {
        const auto slot = Palette::paletteStart + entry;
        transfer.colors.push_back(palette.slotArgb(slot) & 0x00FFFFFFU);
        const auto t = static_cast<double>(entry) / static_cast<double>(entryCount - 1);
        double opacity = 0.0;
        if (t >= low && t <= high) {
            if (paletteAlpha) {
                opacity = palette.opacity(slot);
            } else if (high > low) {
                opacity = (t - low) / (high - low);
            } else {
                opacity = 1.0;   // a zero-width window is a step
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
        if (rangeMode == RangeMode::File) {
            selected = fabDataRange(dataset, field, cancellation);
        }
        if (!selected) {
            const auto statistics = dataset->requestRange(RangeRequest{
                .field = field,
                .maximumLevel = maximumLevel,
                .composition = composition,
                .scope = rangeMode == RangeMode::File
                    ? RangeScope::File : RangeScope::Level}, cancellation);
            if (statistics) {
                selected = std::pair{statistics->minimum, statistics->maximum};
            }
        }
    }
    if (!selected) {
        return std::nullopt;   // Visible, or statistics unavailable: the renderer decides
    }
    auto [minimum, maximum]
        = paddedIfDegenerate(selected->first, selected->second, logarithmic);
    if (!(minimum < maximum)) {
        throw std::runtime_error("user scalar range must have positive extent");
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
    if (budget <= volumeResponseOverheadBytes + sizeof(std::uint32_t)) {
        return {1, 1};
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

VolumeDisplayResult executeVolumeRenderWithFallback(
    const std::shared_ptr<DatasetSession>& dataset, VolumeRenderRequest request,
    StopToken cancellation)
{
    request.outputSize = frameBudgetBoundedVolumeSize(
        request.outputSize, dataset->maximumResponseBytes());
    int fallbackFrom = -1;
    int fallbackTo = -1;
    for (;;) {
        try {
            auto frame = dataset->renderVolume(request, cancellation);
            frame.cacheFallbackFromLevel = fallbackFrom;
            frame.cacheFallbackToLevel = fallbackTo;
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

} // namespace amrvis
