// The volume pipeline over a real local session: the transfer function
// built from a palette and the opacity controls, the range resolution, the
// frame-budget bound, and executeVolumeRenderWithFallback end to end --
// a frame with coverage, the grid cache serving the second render, a
// closed session refusing, and the cache-pressure fallback to a coarser
// level over a two-level fixture whose fine block outsizes the block cache.

#include <amrexplorer/pipeline/VolumePipeline.hpp>

#include <amrexplorer/data/LocalDatasetSession.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view realDescriptor =
    "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))";

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create fixture text");
    output << text;
}

void writeFab(const std::filesystem::path& path, std::string_view box,
    std::span<const double> values)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create fixture FAB");
    output << "FAB " << realDescriptor << box << " 1\n";
    output.write(reinterpret_cast<const char*>(values.data()),
        static_cast<std::streamsize>(values.size() * sizeof(double)));
}

// Two levels over [0,1]^3: coarse 4^3 = 1.0 (one 512-byte block), fine 8^3
// = 2.0 over the whole domain (one 4 KiB block). Block statistics: the
// coarse block [1, 1], the fine block [2, 2], so the File range is [1, 2].
std::filesystem::path writeTwoLevelFixture(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root / "Level_0");
    std::filesystem::create_directories(root / "Level_1");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nphi\n"
        "3\n0.0\n1\n"
        "0.0 0.0 0.0\n1.0 1.0 1.0\n2\n"
        "((0,0,0) (3,3,3) (0,0,0))\n"
        "((0,0,0) (7,7,7) (0,0,0))\n"
        "0 0\n"
        "0.25 0.25 0.25\n0.125 0.125 0.125\n"
        "0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n0.0 1.0\n"
        "Level_0/Cell\n"
        "1 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n0.0 1.0\n"
        "Level_1/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((0,0,0) (3,3,3) (0,0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n1.0,\n\n1,1\n1.0,\n\n");
    writeText(root / "Level_1" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((0,0,0) (7,7,7) (0,0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n2.0,\n\n1,1\n2.0,\n\n");
    std::array<double, 64> coarse{};
    std::array<double, 512> fine{};
    coarse.fill(1.0);
    fine.fill(2.0);
    writeFab(root / "Level_0" / "Cell_D_00000", "((0,0,0) (3,3,3) (0,0,0))",
        coarse);
    writeFab(root / "Level_1" / "Cell_D_00000", "((0,0,0) (7,7,7) (0,0,0))",
        fine);
    return root;
}

amrvis::Palette syntheticPalette(bool withAlpha)
{
    std::array<amrvis::Palette::Rgb, amrvis::Palette::slotCount> slots{};
    std::array<std::uint8_t, amrvis::Palette::slotCount> alpha{};
    for (int index = 0; index < amrvis::Palette::slotCount; ++index) {
        const auto i = static_cast<std::size_t>(index);
        slots[i] = {static_cast<std::uint8_t>(index),
            static_cast<std::uint8_t>(255 - index), 7};
        alpha[i] = static_cast<std::uint8_t>(index % 3 == 0 ? 100 : 20);
    }
    return withAlpha ? amrvis::Palette(slots, alpha) : amrvis::Palette(slots);
}

std::size_t litPixels(const amrvis::VolumeFrame& frame)
{
    std::size_t lit = 0;
    for (const auto pixel : frame.pixels) {
        lit += (pixel >> 24U) != 0U;
    }
    return lit;
}

// A session that renders locally but reports a fallback of its own, the way
// a remote server under its own cache pressure does. Everything but
// renderVolume delegates; LocalDatasetSession is final, so this wraps rather
// than derives.
class FallbackReportingSession final : public amrvis::DatasetSession {
public:
    explicit FallbackReportingSession(
        std::shared_ptr<amrvis::LocalDatasetSession> inner)
        : m_inner(std::move(inner))
    {
    }

    [[nodiscard]] amrvis::DatasetId id() const noexcept override
    {
        return m_inner->id();
    }
    [[nodiscard]] const amrvis::DatasetMetadata& metadata() const noexcept override
    {
        return m_inner->metadata();
    }
    [[nodiscard]] const amrvis::MetadataReadMetrics&
    metadataReadMetrics() const noexcept override
    {
        return m_inner->metadataReadMetrics();
    }
    [[nodiscard]] const std::string& fileVersion() const noexcept override
    {
        return m_inner->fileVersion();
    }
    [[nodiscard]] const std::vector<amrvis::ParticleSpeciesMetadata>&
    particleSpecies() const noexcept override
    {
        return m_inner->particleSpecies();
    }
    [[nodiscard]] amrvis::ViewDataResult requestView(
        const amrvis::ViewDataRequest& request,
        amrvis::StopToken cancellation = {}) override
    {
        return m_inner->requestView(request, cancellation);
    }
    [[nodiscard]] amrvis::DatasetPage requestDatasetPage(
        const amrvis::DatasetPageRequest& request,
        amrvis::StopToken cancellation = {}) override
    {
        return m_inner->requestDatasetPage(request, cancellation);
    }
    [[nodiscard]] std::optional<amrvis::ValueRange> requestRange(
        const amrvis::RangeRequest& request,
        amrvis::StopToken cancellation = {}) override
    {
        return m_inner->requestRange(request, cancellation);
    }
    [[nodiscard]] bool rangeAvailable(
        const amrvis::RangeRequest& request) const noexcept override
    {
        return m_inner->rangeAvailable(request);
    }
    [[nodiscard]] amrvis::ParticleSample requestParticleSample(
        const std::string& species, double fraction, std::uint64_t seed,
        amrvis::StopToken cancellation = {}) override
    {
        return m_inner->requestParticleSample(
            species, fraction, seed, cancellation);
    }
    [[nodiscard]] bool supportsVolumeRendering() const noexcept override
    {
        return m_inner->supportsVolumeRendering();
    }
    // Forwarded like everything else: the base class answers false, so a
    // wrapper that forgot this would report a session that samples in
    // process as one that cannot.
    [[nodiscard]] bool supportsVolumeSampling() const noexcept override
    {
        return m_inner->supportsVolumeSampling();
    }
    // The one that matters: render at one level coarser than asked and say
    // so, without ever throwing CacheBudgetExceeded at the pipeline.
    [[nodiscard]] amrvis::VolumeFrame renderVolume(
        const amrvis::VolumeRenderRequest& request,
        amrvis::StopToken cancellation = {}) override
    {
        auto coarser = request;
        coarser.maximumLevel = std::max(0, request.maximumLevel - 1);
        auto frame = m_inner->renderVolume(coarser, cancellation);
        frame.cacheFallbackFromLevel = request.maximumLevel;
        frame.cacheFallbackToLevel = coarser.maximumLevel;
        return frame;
    }
    [[nodiscard]] amrvis::CacheMetrics cacheMetrics() const override
    {
        return m_inner->cacheMetrics();
    }
    [[nodiscard]] bool setCacheBudget(std::uint64_t bytes) override
    {
        return m_inner->setCacheBudget(bytes);
    }
    void clearUnpinnedCache() override { m_inner->clearUnpinnedCache(); }
    void close() noexcept override { m_inner->close(); }

private:
    std::shared_ptr<amrvis::LocalDatasetSession> m_inner;
};

} // namespace

int main()
{
    // --- the transfer function -------------------------------------------
    {
        const auto palette = syntheticPalette(false);
        const auto plain = amrvis::makeVolumeTransferFunction(palette, {});
        require(plain.colors.size() == 253 && plain.opacities.size() == 253,
            "the transfer function does not have one entry per data slot");
        for (int entry = 0; entry < 253; ++entry) {
            const auto i = static_cast<std::size_t>(entry);
            require(plain.colors[i]
                    == (palette.slotArgb(amrvis::Palette::paletteStart + entry)
                        & 0x00FFFFFFU),
                "an entry's colour is not its palette slot");
            const auto expected = static_cast<float>(entry / 252.0);
            require(std::abs(plain.opacities[i] - expected) <= 1.0e-6F,
                "the default ramp is not linear over the whole range");
        }
        // --- the opacity curve ------------------------------------------
        // The default curve says what the default window says. This is the
        // whole reason the curve is allowed to replace the window: someone who
        // never touches it renders exactly what they rendered before.
        amrvis::OpacityRamp curved;
        curved.curve = amrvis::defaultOpacityCurve();
        const auto fromCurve
            = amrvis::makeVolumeTransferFunction(palette, curved);
        require(fromCurve.opacities == plain.opacities,
            "the default curve does not reproduce the default window");
        require(fromCurve.colors == plain.colors,
            "the curve path did not lay down the same colours");

        // Evaluation: between the points either side, flat outside the ends.
        {
            using amrvis::opacityCurveValue;
            const std::vector<amrvis::OpacityPoint> curve{
                {0.0, 0.0}, {0.5, 1.0}, {1.0, 0.25}};
            require(opacityCurveValue(curve, 0.0) == 0.0
                    && opacityCurveValue(curve, 0.5) == 1.0
                    && opacityCurveValue(curve, 1.0) == 0.25,
                "the curve does not pass through its own control points");
            require(std::abs(opacityCurveValue(curve, 0.25) - 0.5) <= 1.0e-12,
                "the curve is not linear between two points");
            require(std::abs(opacityCurveValue(curve, 0.75) - 0.625) <= 1.0e-12,
                "the curve is not linear on its falling side");
            // Outside the outermost points it holds, so nothing has to be
            // invented past the ends of the range.
            require(opacityCurveValue(curve, -1.0) == 0.0
                    && opacityCurveValue(curve, 2.0) == 0.25,
                "the curve did not hold its end values outside the range");
            require(opacityCurveValue({}, 0.5) == 0.0,
                "an empty curve is not transparent");
            // Two points at one position form a step. Exactly at it, the
            // side below wins -- the segment ending there is found first --
            // and just above it the upper value does. Pinned because either
            // is defensible and the widget lets a drag produce one.
            const std::vector<amrvis::OpacityPoint> step{
                {0.0, 0.0}, {0.5, 0.2}, {0.5, 0.9}, {1.0, 1.0}};
            require(opacityCurveValue(step, 0.5) == 0.2,
                "a step did not take the value below it at its own position");
            require(opacityCurveValue(step, 0.5 + 1.0e-9) > 0.85,
                "just above a step did not take the value above it");
        }

        // Editing. These are where the mistakes live, so they are pinned
        // rather than left to the widget to get right.
        {
            auto curve = amrvis::defaultOpacityCurve();
            const auto middle
                = amrvis::insertOpacityPoint(curve, 0.5, 0.75);
            require(middle == 1 && curve.size() == 3
                    && curve[1] == amrvis::OpacityPoint{0.5, 0.75},
                "an inserted point did not land in order");
            // Inserted out of order, and clamped.
            const auto low = amrvis::insertOpacityPoint(curve, 0.25, 4.0);
            require(low == 1 && curve[1].position == 0.25
                    && curve[1].opacity == 1.0,
                "an inserted point was not sorted and clamped");

            // The edges of the plot, where a press clamps to exactly 0.0 or
            // 1.0. Both have to land inside the two anchors: a point appended
            // past the last one would become the end, which cannot be removed
            // and which opacityCurveValue extends flat, so it would shape one
            // entry of the table and nothing else.
            {
                auto edge = amrvis::defaultOpacityCurve();
                const auto top = amrvis::insertOpacityPoint(edge, 1.0, 0.5);
                require(top == 1 && edge.size() == 3,
                    "a point inserted at the top of the range did not land "
                    "inside the curve's ends");
                require(edge.back() == amrvis::OpacityPoint{1.0, 1.0},
                    "inserting at the top of the range displaced the anchor");
                require(amrvis::removeOpacityPoint(edge, top),
                    "a point inserted at the top of the range could not be "
                    "removed again");
                // The same for the bottom, checked the same way: the index and
                // the anchor alone would miss a point that landed as an end
                // and could not be removed again.
                const auto bottom = amrvis::insertOpacityPoint(edge, 0.0, 0.5);
                require(bottom == 1 && edge.size() == 3
                        && edge.front() == amrvis::OpacityPoint{0.0, 0.0},
                    "inserting at the bottom of the range displaced the "
                    "anchor");
                require(amrvis::removeOpacityPoint(edge, bottom),
                    "a point inserted at the bottom of the range could not be "
                    "removed again");
            }

            // Keeping the new point inside the ends must not cost the sorting
            // that opacityCurveValue reads and that moveOpacityPoint clamps
            // between neighbours with. Nothing says the ends are at 0 and 1 --
            // setCurve takes any two -- so a position outside the span they
            // cover has to come to the span's edge, not to the near end of the
            // interior.
            {
                const auto isSorted = [](const std::vector<amrvis::OpacityPoint>&
                                              points) {
                    for (std::size_t index = 1; index < points.size(); ++index) {
                        if (points[index - 1].position > points[index].position) {
                            return false;
                        }
                    }
                    return true;
                };
                const std::vector<amrvis::OpacityPoint> inset{
                    {0.2, 0.0}, {0.5, 0.5}, {0.9, 1.0}};
                auto below = inset;
                const auto under = amrvis::insertOpacityPoint(below, 0.0, 0.5);
                require(isSorted(below) && under == 1
                        && below.front() == inset.front(),
                    "inserting below the low end left the curve unsorted");
                auto above = inset;
                const auto over = amrvis::insertOpacityPoint(above, 1.0, 0.5);
                require(isSorted(above) && over == above.size() - 2
                        && above.back() == inset.back(),
                    "inserting above the high end left the curve unsorted");

                // And a curve too short to have two ends is still sorted, even
                // though it has no ends to stay between.
                std::vector<amrvis::OpacityPoint> lone{{0.8, 1.0}};
                require(amrvis::insertOpacityPoint(lone, 0.2, 0.5) == 0
                        && isSorted(lone),
                    "inserting into a one-point curve did not keep it sorted");
            }

            // An interior point cannot pass its neighbours.
            amrvis::moveOpacityPoint(curve, 2, 5.0, 0.5);
            require(curve[2].position == curve[3].position,
                "an interior point moved past its neighbour");
            amrvis::moveOpacityPoint(curve, 2, -5.0, 0.5);
            require(curve[2].position == curve[1].position,
                "an interior point moved below its neighbour");

            // The ends keep their positions -- the curve spans the range --
            // and move only in opacity.
            amrvis::moveOpacityPoint(curve, 0, 0.9, 0.4);
            require(curve[0].position == 0.0 && curve[0].opacity == 0.4,
                "the low end point did not stay at the bottom of the range");
            const auto lastIndex = curve.size() - 1;
            amrvis::moveOpacityPoint(curve, lastIndex, 0.1, 0.2);
            require(curve[lastIndex].position == 1.0
                    && curve[lastIndex].opacity == 0.2,
                "the high end point did not stay at the top of the range");
            amrvis::moveOpacityPoint(curve, 99, 0.5, 0.5);   // no such point
            require(curve.size() == 4, "moving a point that is not there grew "
                                      "the curve");

            // Removal takes interior points only, and never the last two.
            require(!amrvis::removeOpacityPoint(curve, 0)
                    && !amrvis::removeOpacityPoint(curve, curve.size() - 1),
                "an end point was removed, leaving the curve short of the "
                "range");
            require(amrvis::removeOpacityPoint(curve, 1) && curve.size() == 3,
                "an interior point was not removed");
            auto pair = amrvis::defaultOpacityCurve();
            require(!amrvis::removeOpacityPoint(pair, 1) && pair.size() == 2,
                "a two-point curve was reduced below two points");
        }

        // A window: transparent outside [0.25, 0.75], ramping inside to the
        // maximum opacity at the top.
        amrvis::OpacityRamp windowed{0.25, 0.75, 0.5, false, {}};
        const auto window = amrvis::makeVolumeTransferFunction(palette, windowed);
        require(window.opacities[0] == 0.0F && window.opacities[252] == 0.0F
                && window.opacities[50] == 0.0F,
            "the window did not clear the entries outside the thresholds");
        require(std::abs(window.opacities[189] - 0.5F) <= 1.0e-5F,   // t = 0.75
            "the window's top is not the maximum opacity");
        require(std::abs(window.opacities[126] - 0.25F) <= 2.0e-3F,   // t = 0.5
            "the window's midpoint is not half the maximum opacity");
        // Equal thresholds (the coupled sliders allow them) and a window
        // narrower than the entry pitch select the single nearest entry,
        // opaque, rather than nothing: 0.3 * 252 = 75.6 -> entry 76.
        for (const amrvis::OpacityRamp& narrow :
            {amrvis::OpacityRamp{0.3, 0.3, 1.0, false, {}},
                amrvis::OpacityRamp{0.299, 0.301, 1.0, false, {}}}) {
            const auto shell = amrvis::makeVolumeTransferFunction(palette, narrow);
            for (int entry = 0; entry < 253; ++entry) {
                const auto i = static_cast<std::size_t>(entry);
                require(shell.opacities[i] == (entry == 76 ? 1.0F : 0.0F),
                    "a sub-pitch window did not select the nearest entry alone");
            }
        }
        // A window holding a few entries ramps over exactly those, the top
        // one at the maximum: [0.30, 0.31] * 252 = [75.6, 78.12] -> 76..78.
        const auto few = amrvis::makeVolumeTransferFunction(
            palette, amrvis::OpacityRamp{0.30, 0.31, 0.8, false, {}});
        require(few.opacities[75] == 0.0F && few.opacities[76] == 0.0F
                && std::abs(few.opacities[77] - 0.4F) <= 1.0e-6F
                && std::abs(few.opacities[78] - 0.8F) <= 1.0e-6F
                && few.opacities[79] == 0.0F,
            "a few-entry window did not ramp over its entries to the maximum");
        // The palette's alpha ramp, when asked for and present; the plain
        // ramp when the palette has none.
        amrvis::OpacityRamp fromPalette{0.0, 1.0, 1.0, true, {}};
        const auto alphaPalette = syntheticPalette(true);
        // A curve and the palette's own alpha are alternatives: with the box
        // in effect the palette's ramp comes back as authored, whatever the
        // curve says. The curve used to gate it -- zero where the curve was
        // zero -- which almost never bit, because the entries land at k / 252
        // and a curve touching zero at one point misses all of them. This
        // shape is exactly that case: a V with its floor at 0.501.
        {
            amrvis::OpacityRamp shaped;
            shaped.usePaletteAlpha = true;
            shaped.curve = {{0.0, 1.0}, {0.501, 0.0}, {1.0, 1.0}};
            const auto authored
                = amrvis::makeVolumeTransferFunction(alphaPalette, shaped);
            amrvis::OpacityRamp plainAlpha;
            plainAlpha.usePaletteAlpha = true;
            const auto reference
                = amrvis::makeVolumeTransferFunction(alphaPalette, plainAlpha);
            require(authored.opacities == reference.opacities,
                "the curve altered a palette's authored alpha ramp");
            // And it is that ramp, not a flat or empty one, so the comparison
            // above is not two identical mistakes.
            const auto slot = amrvis::Palette::paletteStart + 200;
            require(std::abs(authored.opacities[200]
                        - static_cast<float>(alphaPalette.opacity(slot)))
                    <= 1.0e-6F,
                "the palette-alpha path did not hand back the palette's own "
                "opacity");
            // Entries 0 and 1: the synthetic ramp is 100 % on every third
            // slot and 20 % elsewhere, and paletteStart is 3, so these two
            // differ where 10 and 200 happen to collide.
            require(authored.opacities[0] != authored.opacities[1],
                "the reference ramp is flat, so matching it proves nothing");

            // And a curve that gating *would* have bitten on: flat at zero
            // over the bottom half, so entries there landed on it. The V above
            // cannot tell the old rule from the new one -- gating left it
            // unchanged too -- so without this case the bug passes.
            amrvis::OpacityRamp flatBottom;
            flatBottom.usePaletteAlpha = true;
            flatBottom.curve = {{0.0, 0.0}, {0.5, 0.0}, {0.6, 1.0}, {1.0, 1.0}};
            const auto unshaped
                = amrvis::makeVolumeTransferFunction(alphaPalette, flatBottom);
            require(unshaped.opacities == reference.opacities,
                "a curve pulled to nothing still cleared the palette's own "
                "alpha, so the two are being layered rather than chosen "
                "between");
            require(reference.opacities[50] > 0.0F,
                "the reference ramp is already zero there, so the check above "
                "proves nothing");
        }
        const auto withAlpha = amrvis::makeVolumeTransferFunction(alphaPalette, fromPalette);
        require(withAlpha.opacities[0] == 1.0F   // slot 3: 3 % 3 == 0 -> 100 %
                && std::abs(withAlpha.opacities[1] - 0.2F) <= 1.0e-6F,
            "the palette alpha ramp was not used");
        const auto withoutAlpha = amrvis::makeVolumeTransferFunction(palette, fromPalette);
        require(withoutAlpha.opacities == plain.opacities,
            "a palette without a ramp did not fall back to the linear ramp");
        // Every transfer function the builder makes validates.
        require(amrvis::validateVolumeTransferFunction(withAlpha).empty()
                && amrvis::validateVolumeTransferFunction(window).empty(),
            "the builder produced an invalid transfer function");
    }

    // --- the frame-budget bound -------------------------------------------
    {
        require(amrvis::frameBudgetBoundedVolumeSize({800, 600}, std::nullopt)
                == (std::array<int, 2>{800, 600}),
            "a local session's frame was bounded");
        const auto plenty = static_cast<std::uint32_t>(
            amrvis::volumeResponseBytes({800, 600}));
        require(amrvis::frameBudgetBoundedVolumeSize({800, 600}, plenty)
                == (std::array<int, 2>{800, 600}),
            "a frame that exactly fits was shrunk");
        const auto quarter = static_cast<std::uint32_t>(
            amrvis::volumeResponseBytes({400, 300}));
        const auto shrunk = amrvis::frameBudgetBoundedVolumeSize({800, 600}, quarter);
        require(shrunk[0] <= 400 && shrunk[1] <= 300 && shrunk[0] >= 398
                && shrunk[1] >= 298
                && amrvis::volumeResponseBytes(shrunk) <= quarter,
            "an oversized frame was not shrunk to fit, aspect kept");
        // A budget too small for one pixel is refused rather than bounded to
        // {1, 1}: volumeResponseBytes({1, 1}) exceeds it, so the request that
        // came back would be rejected on arrival anyway, and the refusal here
        // names the budget instead of failing later as an oversized frame.
        std::string refusal;
        try {
            static_cast<void>(
                amrvis::frameBudgetBoundedVolumeSize({800, 600}, 100U));
        } catch (const std::runtime_error& error) {
            refusal = error.what();
        }
        // Actionable, and carrying both numbers: this escapes to the user the
        // way the cache-pressure refusals do, and a bare "invalid argument"
        // would tell them nothing about a budget they can raise.
        require(refusal.find("100") != std::string::npos
                && refusal.find("4100") != std::string::npos,
            "a hopeless budget did not refuse with the numbers");
        // An inverted window is refused rather than silently collapsing to a
        // shell at the midpoint, which is what the sub-pitch branch would do
        // with it and is indistinguishable from a deliberate thin shell.
        const auto ramps = syntheticPalette(false);
        const auto refusesRamp = [&ramps](const amrvis::OpacityRamp& ramp) {
            try {
                static_cast<void>(
                    amrvis::makeVolumeTransferFunction(ramps, ramp));
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };
        require(refusesRamp({0.8, 0.2, 1.0, false, {}}),
            "an inverted opacity window was accepted");
        require(refusesRamp({std::nan(""), 1.0, 1.0, false, {}})
                && refusesRamp({0.0, 1.0, std::nan(""), false, {}}),
            "a non-finite opacity control was accepted");
        // The smallest budget that does fit one pixel still yields one pixel.
        const auto minimum = static_cast<std::uint32_t>(
            amrvis::volumeResponseBytes({1, 1}));
        require(amrvis::frameBudgetBoundedVolumeSize({800, 600}, minimum)
                == (std::array<int, 2>{1, 1}),
            "the smallest workable budget did not collapse to one pixel");
    }

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto scratch = std::filesystem::temp_directory_path()
        / ("amrexplorer-volume-pipeline-" + std::to_string(unique));
    const auto root = writeTwoLevelFixture(scratch / "two-level");

    // --- range resolution and rendering over a local session ------------
    {
        auto session = std::make_shared<amrvis::LocalDatasetSession>(
            root, amrvis::DatasetId{4}, 1024 * 1024);
        require(session->supportsVolumeRendering(),
            "a 3-D plotfile session does not support volume rendering");
        std::shared_ptr<amrvis::DatasetSession> dataset = session;
        const amrvis::FieldId field{0};

        // File statistics: [1, 2]; Level 1 (finest available) also [1, 2];
        // User range as given, padded when degenerate; log downgrades on a
        // non-positive range; Visible leaves it to the renderer.
        const auto file = amrvis::resolveVolumeRange(dataset, field, 1,
            amrvis::CompositionPolicy::FinestAvailable, amrvis::RangeMode::File,
            std::nullopt, false);
        require(file && file->minimum == 1.0 && file->maximum == 2.0
                && !file->logarithmic,
            "the File range is not the metadata's");
        const auto logFile = amrvis::resolveVolumeRange(dataset, field, 1,
            amrvis::CompositionPolicy::FinestAvailable, amrvis::RangeMode::File,
            std::nullopt, true);
        require(logFile && logFile->logarithmic && logFile->minimum == 1.0,
            "a positive File range did not stay logarithmic");
        const auto user = amrvis::resolveVolumeRange(dataset, field, 1,
            amrvis::CompositionPolicy::FinestAvailable, amrvis::RangeMode::User,
            std::pair{-3.0, 5.0}, true);
        require(user && !user->logarithmic && user->minimum == -3.0
                && user->maximum == 5.0,
            "a non-positive User range did not fall back to linear");
        const auto symlog = amrvis::resolveVolumeRange(dataset, field, 1,
            amrvis::CompositionPolicy::FinestAvailable, amrvis::RangeMode::User,
            std::pair{-3.0, 5.0},
            {amrvis::ColorScale::SymLogarithmic, 0.25});
        require(symlog && !symlog->logarithmic
                && symlog->scale.scale == amrvis::ColorScale::SymLogarithmic
                && symlog->scale.linearThreshold == 0.25,
            "a symmetric-log User range did not preserve its mapping");
        const auto degenerate = amrvis::resolveVolumeRange(dataset, field, 1,
            amrvis::CompositionPolicy::FinestAvailable, amrvis::RangeMode::User,
            std::pair{2.0, 2.0}, false);
        require(degenerate && degenerate->minimum < 2.0 && degenerate->maximum > 2.0,
            "a degenerate User range was not padded");
        {
            const auto huge = 1.0e300;
            std::string narrow;
            try {
                static_cast<void>(amrvis::resolveVolumeRange(dataset, field, 1,
                    amrvis::CompositionPolicy::FinestAvailable,
                    amrvis::RangeMode::User,
                    std::pair{huge, std::nextafter(huge,
                        std::numeric_limits<double>::infinity())},
                    true));
            } catch (const std::runtime_error& error) {
                narrow = error.what();
            }
            require(narrow.find("user") != std::string::npos
                    && narrow.find("logarithm") != std::string::npos,
                "a logarithmic range too narrow to map was handed to the renderer");
        }
        require(!amrvis::resolveVolumeRange(dataset, field, 1,
                    amrvis::CompositionPolicy::FinestAvailable,
                    amrvis::RangeMode::Visible, std::nullopt, false)
                    .has_value(),
            "the Visible range was resolved on the client");

        // Render: an oblique view of a domain that is 2.0 in its fine
        // block and 1.0 in the coarse remainder, opaque above the middle.
        amrvis::VolumeRenderRequest request;
        request.dataset = amrvis::DatasetId{4};
        request.field = field;
        request.maximumLevel = 1;
        request.region = amrvis::datasetSampleBounds(session->metadata());
        request.camera = {0.6, 0.4, 1.0};
        request.outputSize = {64, 48};
        request.range = amrvis::VolumeRange{1.0, 2.0, false};
        request.transfer = amrvis::makeVolumeTransferFunction(
            amrvis::builtinPalette(amrvis::BuiltinPalette::Rainbow),
            amrvis::OpacityRamp{0.5, 1.0, 1.0, false, {}});
        request.maximumVoxels = 4096;
        const auto first = amrvis::executeVolumeRenderWithFallback(dataset, request);
        require(first.frame.width == 64 && first.frame.height == 48
                && litPixels(first.frame) > 0
                && first.frame.usedRange == *request.range
                && first.frame.metrics.gridDims == (std::array<int, 3>{8, 8, 8})
                && first.frame.metrics.coveredVoxels == 512
                && first.frame.metrics.sampledMaximumLevel == 1
                && !first.frame.metrics.gridFromCache
                && first.frame.metrics.blocksRead == 2
                && first.frame.cacheFallbackFromLevel == -1
                && first.request.maximumLevel == 1,
            "the first render did not sample and draw the fixture");
        // Everything is at least the middle of the range: the whole domain
        // silhouette is lit; the pixel at the frame centre is inside it.
        require((first.frame.pixels[static_cast<std::size_t>(24 * 64 + 32)] >> 24U) > 0,
            "the domain centre pixel is not lit");
        // The second render of the same sample comes from the grid cache and
        // draws the same picture.
        const auto second = amrvis::executeVolumeRenderWithFallback(dataset, request);
        require(second.frame.metrics.gridFromCache
                && second.frame.metrics.blocksRead == 0
                && second.frame.pixels == first.frame.pixels,
            "the second render did not reuse the cached grid");
        // Visible range: resolved by the renderer from the sampled grid and
        // reported back. The fine level covers the whole domain, so the grid
        // is uniformly 2.0 and the range is that value, padded either side.
        auto visible = request;
        visible.range.reset();
        const auto resolved = amrvis::executeVolumeRenderWithFallback(dataset, visible);
        require(resolved.frame.usedRange.minimum < 2.0
                && resolved.frame.usedRange.maximum > 2.0
                && resolved.frame.usedRange.maximum - resolved.frame.usedRange.minimum
                    < 1.0e-4
                && !resolved.frame.usedRange.logarithmic
                && resolved.frame.metrics.gridFromCache,
            "the Visible range was not resolved from the sampled grid");
        visible.logarithmic = true;
        const auto resolvedLog = amrvis::executeVolumeRenderWithFallback(dataset, visible);
        require(resolvedLog.frame.usedRange.logarithmic
                && resolvedLog.frame.usedRange.minimum > 0.0
                && resolvedLog.frame.usedRange.minimum < 2.0
                && resolvedLog.frame.usedRange.maximum > 2.0,
            "a positive Visible range did not go logarithmic when asked");
        // A Visible render at level 0 sees only the coarse 1.0.
        auto coarseVisible = visible;
        coarseVisible.logarithmic = false;
        coarseVisible.maximumLevel = 0;
        const auto coarseResolved
            = amrvis::executeVolumeRenderWithFallback(dataset, coarseVisible);
        require(coarseResolved.frame.usedRange.minimum < 1.0
                && coarseResolved.frame.usedRange.maximum > 1.0
                && coarseResolved.frame.usedRange.maximum < 1.001
                && coarseResolved.frame.metrics.sampledMaximumLevel == 0
                && !coarseResolved.frame.metrics.gridFromCache,
            "a coarser level did not resolve its own Visible range");
        // The budget a session was constructed with bounds the grid pool
        // too, not just the block pool. Nothing on the normal open paths
        // calls setCacheBudget, so a session built with a small budget would
        // otherwise keep the 256 MiB grid default.
        {
            auto small = std::make_shared<amrvis::LocalDatasetSession>(
                root, amrvis::DatasetId{9}, 4096);
            require(small->volumeGridCacheMetrics().budgetBytes == 4096,
                "the constructed budget did not reach the grid cache");
            require(small->setCacheBudget(8192)
                    && small->volumeGridCacheMetrics().budgetBytes == 8192,
                "setCacheBudget did not keep the grid cache in step");
            // setBlockCacheBudget leaves the grid pool where it is. The
            // server applies a client's requested budget through this one,
            // so a peer cannot raise the sampled-grid cache past the limit
            // --volume-cache-mib set for it.
            require(small->setVolumeGridCacheBudget(4096),
                "the grid cache budget could not be pinned");
            require(small->setBlockCacheBudget(64ULL * 1024ULL * 1024ULL)
                    && small->volumeGridCacheMetrics().budgetBytes == 4096,
                "a block-only budget moved the grid cache");
        }
        // The shape a server opens a session in: the client names a block
        // budget, the operator names the grid budget, and the operator's is
        // the one the grid pool gets. Taking the smaller of the two -- the
        // client's block number is not a ceiling for grids -- would let a
        // client with a modest AMREXPLORER_CACHE_SIZE_MB starve a pool the
        // operator sized generously, and a client that named none zero it.
        {
            constexpr std::uint64_t clientBlockBudget = 64ULL * 1024ULL;
            constexpr std::uint64_t operatorGridBudget = 8ULL * 1024ULL * 1024ULL;
            auto served = std::make_shared<amrvis::LocalDatasetSession>(
                root, amrvis::DatasetId{11}, clientBlockBudget);
            require(served->setVolumeGridCacheBudget(operatorGridBudget),
                "the operator's grid budget was refused");
            require(served->volumeGridCacheMetrics().budgetBytes
                    == operatorGridBudget,
                "the client's block budget capped the operator's grid budget");
            // And a later client budget change leaves it alone.
            require(served->setBlockCacheBudget(32ULL * 1024ULL)
                    && served->volumeGridCacheMetrics().budgetBytes
                        == operatorGridBudget,
                "a client budget change moved the operator's grid budget");
        }
        // A grid larger than the grid cache still renders, uncached.
        require(session->setVolumeGridCacheBudget(1024),
            "the grid cache budget could not be lowered");
        session->clearUnpinnedCache();
        const auto uncached = amrvis::executeVolumeRenderWithFallback(dataset, request);
        require(!uncached.frame.metrics.gridFromCache
                && uncached.frame.pixels == first.frame.pixels,
            "an over-budget grid was not rendered uncached");
        const auto uncachedAgain = amrvis::executeVolumeRenderWithFallback(dataset, request);
        require(!uncachedAgain.frame.metrics.gridFromCache,
            "an over-budget grid was cached anyway");
        // A closed session refuses.
        session->close();
        bool threw = false;
        try {
            (void)amrvis::executeVolumeRenderWithFallback(dataset, request);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        require(threw, "a closed session rendered a volume");
    }

    // --- cache-pressure fallback -----------------------------------------
    // A block cache too small for the 4 KiB fine block but large enough for
    // the 512-byte coarse one: the finest-available render falls back from
    // level 1 to level 0 and says so; an exact-level request fails with an
    // actionable message.
    {
        auto session = std::make_shared<amrvis::LocalDatasetSession>(
            root, amrvis::DatasetId{6}, 2048);
        std::shared_ptr<amrvis::DatasetSession> dataset = session;
        amrvis::VolumeRenderRequest request;
        request.dataset = amrvis::DatasetId{6};
        request.field = amrvis::FieldId{0};
        request.maximumLevel = 1;
        request.region = amrvis::datasetSampleBounds(session->metadata());
        request.camera = amrvis::orthoPresetXY;
        request.outputSize = {32, 32};
        // Range [0, 1]: the coarse 1.0 maps to the opaque top entry.
        request.range = amrvis::VolumeRange{0.0, 1.0, false};
        request.transfer.colors = {0x0U, 0xFFFFFFU};
        request.transfer.opacities = {0.0F, 1.0F};
        request.maximumVoxels = 4096;
        const auto fallen = amrvis::executeVolumeRenderWithFallback(dataset, request);
        require(fallen.frame.cacheFallbackFromLevel == 1
                && fallen.frame.cacheFallbackToLevel == 0
                && fallen.request.maximumLevel == 0
                && fallen.frame.metrics.sampledMaximumLevel == 0
                && fallen.frame.metrics.gridDims == (std::array<int, 3>{4, 4, 4})
                && litPixels(fallen.frame) > 0,
            "the render did not fall back to the level that fits");
        // The range follows the fallback when it is resolved per attempt:
        // level 1's statistics span [1, 2], level 0's are the coarse 1.0
        // alone, so a Level range resolved once up front would colour a
        // level-0 render with level-1's numbers.
        {
            const auto levelWide = amrvis::resolveVolumeRange(dataset,
                request.field, 1, request.composition, amrvis::RangeMode::Level,
                std::nullopt, false);
            require(levelWide && levelWide->maximum > 1.5,
                "the level-1 range is not the wider one");
            auto tracking = request;
            tracking.range.reset();
            const auto followed = amrvis::executeVolumeRenderWithFallback(
                dataset, tracking,
                amrvis::VolumeRangeChoice{amrvis::RangeMode::Level,
                    std::nullopt, false});
            require(followed.request.maximumLevel == 0
                    && followed.frame.usedRange.maximum < 1.5,
                "the range did not follow the fallback to level 0");
        }
        // A session that falls back inside an attempt the client never
        // retried: the result's request must describe what was rendered, not
        // what was asked for, or the caller's state stays finer than the
        // pixels it got.
        {
            auto reporting = std::make_shared<FallbackReportingSession>(session);
            std::shared_ptr<amrvis::DatasetSession> wrapped = reporting;
            auto asked = request;
            asked.maximumLevel = 1;
            const auto served
                = amrvis::executeVolumeRenderWithFallback(wrapped, asked);
            require(served.frame.cacheFallbackFromLevel == 1
                    && served.frame.cacheFallbackToLevel == 0,
                "the session's own fallback was not reported");
            require(served.request.maximumLevel == 0,
                "the result's request did not follow the session's fallback");
            // And a Level range follows it too: level 1's statistics span
            // [1, 2], level 0's are the coarse 1.0 alone. Resolving once
            // before the call would map level-0 pixels with level-1's range.
            // From cold: the render above already cached this grid, so
            // without clearing, no attempt reads anything and the assertion
            // below would pass for the wrong reason.
            session->clearUnpinnedCache();
            auto tracked = asked;
            tracked.range.reset();
            const auto followed = amrvis::executeVolumeRenderWithFallback(
                wrapped, tracked,
                amrvis::VolumeRangeChoice{amrvis::RangeMode::Level,
                    std::nullopt, false});
            require(followed.request.maximumLevel == 0
                    && followed.frame.cacheFallbackFromLevel == 1
                    && followed.frame.cacheFallbackToLevel == 0,
                "the session's fallback was not reported after the repeat");
            require(followed.frame.usedRange.maximum < 1.5,
                "a Level range did not follow the session's own fallback");
            // The repeat renders against the grid the discarded attempt
            // cached, so it reads nothing itself. The work still happened,
            // and a caller told this render read no blocks would be misled.
            require(followed.frame.metrics.blocksRead > 0
                    && followed.frame.metrics.candidateBlocks > 0,
                "the discarded attempt's work was dropped from the metrics");
            // A File range asks for the whole file's bounds whatever the
            // level, so a fallback cannot make it wrong and it is not worth a
            // second render. One render means the grid was still uncached.
            session->clearUnpinnedCache();
            auto byFile = asked;
            byFile.range.reset();
            const auto fileRange = amrvis::executeVolumeRenderWithFallback(
                wrapped, byFile,
                amrvis::VolumeRangeChoice{amrvis::RangeMode::File,
                    std::nullopt, false});
            require(!fileRange.frame.metrics.gridFromCache,
                "a File range was repeated even though it ignores the level");
        }
        auto exact = request;
        exact.composition = amrvis::CompositionPolicy::ExactLevel;
        bool threw = false;
        try {
            (void)amrvis::executeVolumeRenderWithFallback(dataset, exact);
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()).find("Choose a lower level")
                != std::string::npos;
        }
        require(threw, "an exact level that cannot fit did not fail actionably");
    }

    std::error_code removeError;
    std::filesystem::remove_all(scratch, removeError);
    return 0;
}
