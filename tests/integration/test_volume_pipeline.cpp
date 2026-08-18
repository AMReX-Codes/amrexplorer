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
#include <memory>
#include <span>
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
        // A window: transparent outside [0.25, 0.75], ramping inside to the
        // maximum opacity at the top.
        amrvis::OpacityRamp windowed{0.25, 0.75, 0.5, false};
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
        for (const amrvis::OpacityRamp narrow :
            {amrvis::OpacityRamp{0.3, 0.3, 1.0, false},
                amrvis::OpacityRamp{0.299, 0.301, 1.0, false}}) {
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
            palette, amrvis::OpacityRamp{0.30, 0.31, 0.8, false});
        require(few.opacities[75] == 0.0F && few.opacities[76] == 0.0F
                && std::abs(few.opacities[77] - 0.4F) <= 1.0e-6F
                && std::abs(few.opacities[78] - 0.8F) <= 1.0e-6F
                && few.opacities[79] == 0.0F,
            "a few-entry window did not ramp over its entries to the maximum");
        // The palette's alpha ramp, when asked for and present; the plain
        // ramp when the palette has none.
        amrvis::OpacityRamp fromPalette{0.0, 1.0, 1.0, true};
        const auto alphaPalette = syntheticPalette(true);
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
        require(amrvis::frameBudgetBoundedVolumeSize({800, 600}, 100U)
                == (std::array<int, 2>{1, 1}),
            "a hopeless budget did not collapse to one pixel");
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
        const auto degenerate = amrvis::resolveVolumeRange(dataset, field, 1,
            amrvis::CompositionPolicy::FinestAvailable, amrvis::RangeMode::User,
            std::pair{2.0, 2.0}, false);
        require(degenerate && degenerate->minimum < 2.0 && degenerate->maximum > 2.0,
            "a degenerate User range was not padded");
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
            amrvis::OpacityRamp{0.5, 1.0, 1.0, false});
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
