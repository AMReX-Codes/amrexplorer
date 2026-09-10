// A field whose values differ only far below float's resolution still
// renders. The reproducer is a WarpX `mu` field holding the two doubles
// below: they are 1e-17 apart, which is about one nine-thousandth of a float
// ulp at that magnitude, so narrowing the plane to float collapsed every cell
// to one value and the image came out flat.

#include <amrexplorer/data/DatasetPage.hpp>
#include <amrexplorer/data/LocalDatasetSession.hpp>
#include <amrexplorer/data/SessionValidation.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/pipeline/SliceRangeResolver.hpp>
#include <amrexplorer/query/LineQuery.hpp>
#include <amrexplorer/query/SliceQuery.hpp>
#include <amrexplorer/query/VolumeQuery.hpp>
#include <amrexplorer/render2d/Palette.hpp>
#include <amrexplorer/render2d/ScalarRenderer.hpp>
#include <amrexplorer/render2d/Contours.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view doubleDescriptor =
    "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))";

// The two values the reporting plotfile actually holds.
constexpr double muLow = 1.2566370621199999e-06;
constexpr double muHigh = 1.25663706213e-06;

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
    require(static_cast<bool>(output),
        "could not create near-degenerate fixture text");
    output << text;
}

void writeFab(const std::filesystem::path& path, std::string_view box,
    int dimension, std::span<const double> values)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output),
        "could not create near-degenerate fixture FAB");
    output << "FAB " << doubleDescriptor << box << " 1\n";
    static_cast<void>(dimension);
    for (const auto value : values) {
        std::array<unsigned char, sizeof(double)> bytes{};
        std::memcpy(bytes.data(), &value, sizeof(value));
        if constexpr (std::endian::native == std::endian::big) {
            std::reverse(bytes.begin(), bytes.end());
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
}

// 2 x 2 cells over [0,1]^2. The low value fills the bottom row, the high one
// the top, so a slice, a line along y and a page all see both.
std::filesystem::path write2dFixture(const std::filesystem::path& root,
    double low = muLow, double high = muHigh)
{
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nmu\n"
        "2\n0.0\n0\n"
        "0.0 0.0\n1.0 1.0\n\n"
        "((0,0) (1,1) (0,0))\n"
        "0\n"
        "0.5 0.5\n"
        "0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((0,0) (1,1) (0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n1.25663706212e-06,\n\n"
        "1,1\n1.25663706213e-06,\n\n");
    const std::array<double, 4> values{low, low, high, high};
    writeFab(root / "Level_0" / "Cell_D_00000", "((0,0) (1,1) (0,0))", 2,
        std::span<const double>(values));
    return root;
}

// The same pair over 2 x 2 x 2 cells, for the volume path.
std::filesystem::path write3dFixture(const std::filesystem::path& root,
    double low = muLow, double high = muHigh)
{
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nmu\n"
        "3\n0.0\n0\n"
        "0.0 0.0 0.0\n1.0 1.0 1.0\n\n"
        "((0,0,0) (1,1,1) (0,0,0))\n"
        "0\n"
        "0.5 0.5 0.5\n"
        "0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n0.0 1.0\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((0,0,0) (1,1,1) (0,0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n1.25663706212e-06,\n\n"
        "1,1\n1.25663706213e-06,\n\n");
    std::array<double, 8> values{};
    for (std::size_t index = 0; index < values.size(); ++index) {
        values[index] = index < 4 ? low : high;
    }
    writeFab(root / "Level_0" / "Cell_D_00000", "((0,0,0) (1,1,1) (0,0,0))", 3,
        std::span<const double>(values));
    return root;
}

} // namespace

int main()
{
    // Pin the premise: distinct as doubles, identical as floats. Without this
    // the rest could pass on a fixture that never exercised the collapse.
    require(muLow != muHigh, "the fixture values are not distinct doubles");
    require(static_cast<float>(muLow) == static_cast<float>(muHigh),
        "the fixture no longer exercises the float collapse");

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto scratch = std::filesystem::temp_directory_path()
        / ("amrexplorer-near-degenerate-" + std::to_string(unique));

    // --- slice: both values survive sampling, bit for bit ------------------
    const auto root2d = write2dFixture(scratch / "plane");
    amrvis::PlotfileDataset dataset(root2d, amrvis::DatasetId{1}, 1024 * 1024);
    amrvis::SliceRequest sliceRequest;
    sliceRequest.dataset = dataset.id();
    sliceRequest.field = amrvis::FieldId{0};
    sliceRequest.normalDirection = 1;
    sliceRequest.visibleRegion = dataset.metadata().physicalDomain;
    sliceRequest.outputSize = {2, 2};
    const auto slice = amrvis::SliceQuery(dataset).execute(sliceRequest);
    require(slice.plane.values.size() == 4,
        "the near-degenerate slice has the wrong shape");
    require(slice.plane.values[0] == muLow && slice.plane.values[3] == muHigh,
        "a sample differing at the 12th significant digit was narrowed away");

    // --- range: the true bounds, not a padded degenerate pair --------------
    const auto range = amrvis::finiteRange(slice.plane, false);
    require(range.has_value(), "the near-degenerate plane produced no range");
    require(range->first == muLow && range->second == muHigh,
        "the visible range did not span the two distinct values");

    // --- the load-bearing one: distinct colours in the raster --------------
    const auto& palette = amrvis::builtinPalette(amrvis::BuiltinPalette::Rainbow);
    amrvis::ScalarRenderSettings settings;
    settings.minimum = range->first;
    settings.maximum = range->second;
    settings.palette = &palette;
    const auto image = amrvis::renderScalarPlane(slice.plane, settings);
    require(image.rgba[0] != image.rgba[3],
        "two doubles differing at the 12th significant digit rendered as one colour");
    // Not merely two adjacent slots: the pair spans the whole colour map, so
    // the picture shows the structure rather than a barely-perceptible tint.
    require(image.rgba[0] == palette.slotArgb(amrvis::Palette::paletteStart)
            && image.rgba[3] == palette.slotArgb(
                   amrvis::Palette::paletteStart
                   + amrvis::Palette::colorSlots - 1),
        "the near-degenerate pair did not span the palette");

    // --- line ---------------------------------------------------------------
    amrvis::LineRequest lineRequest;
    lineRequest.dataset = dataset.id();
    lineRequest.field = amrvis::FieldId{0};
    lineRequest.axis = 1;
    lineRequest.fixedCoordinates = {0.25, 0.0, 0.0};
    lineRequest.region = amrvis::datasetSampleBounds(dataset.metadata());
    const auto line = amrvis::LineQuery(dataset).execute(lineRequest);
    require(line.line.values.size() == 2,
        "the near-degenerate line has the wrong shape");
    require(line.line.values[0] == muLow && line.line.values[1] == muHigh,
        "a line sample was narrowed away");

    // --- dataset page: values now agree with the extrema beside them -------
    amrvis::DatasetPageRequest pageRequest;
    pageRequest.dataset = dataset.id();
    pageRequest.field = amrvis::FieldId{0};
    pageRequest.level = 0;
    pageRequest.region = dataset.metadata().physicalDomain;
    pageRequest.normalAxis = 1;
    pageRequest.maximumExtent = 16;
    const auto page = amrvis::extractDatasetPage(dataset, pageRequest);
    require(page.values.size() == 4, "the near-degenerate page has the wrong shape");
    require(page.values[0] != page.values[3],
        "the page collapsed two distinct values into one");
    // page.minimum/maximum were always double, so before the widening they
    // reported a span the flat value array could not account for.
    require(page.minimum == muLow && page.maximum == muHigh,
        "the page extrema do not match the values they describe");

    // --- volume -------------------------------------------------------------
    const auto root3d = write3dFixture(scratch / "volume");
    amrvis::PlotfileDataset volumeDataset(
        root3d, amrvis::DatasetId{2}, 1024 * 1024);
    amrvis::VolumeSampleRequest volumeRequest;
    volumeRequest.dataset = volumeDataset.id();
    volumeRequest.field = amrvis::FieldId{0};
    volumeRequest.maximumLevel = 0;
    volumeRequest.region = volumeDataset.metadata().physicalDomain;
    volumeRequest.maximumVoxels = 64;
    const auto sampled = amrvis::VolumeQuery(volumeDataset).execute(volumeRequest);
    const auto lowest = *std::min_element(
        sampled.grid.values.begin(), sampled.grid.values.end());
    const auto highest = *std::max_element(
        sampled.grid.values.begin(), sampled.grid.values.end());
    require(lowest == muLow && highest == muHigh,
        "the volume grid narrowed the pair to one value");

    // Visible ranges must remain renderable even when their span or the
    // padding of a constant would overflow. Exercise real slice/volume reads.
    const auto huge = std::numeric_limits<double>::max();
    int caseIndex = 0;
    for (const auto& [low, high] : {std::pair{-1.0e308, 1.0e308},
             std::pair{-huge, huge}, std::pair{huge, huge},
             std::pair{-huge, -huge}}) {
        const auto caseRoot = scratch / std::to_string(caseIndex++);
        amrvis::PlotfileDataset extreme2d(
            write2dFixture(caseRoot / "plane", low, high),
            amrvis::DatasetId{3}, 1024 * 1024);
        auto extremeSliceRequest = sliceRequest;
        extremeSliceRequest.dataset = extreme2d.id();
        const auto extremeSlice
            = amrvis::SliceQuery(extreme2d).execute(extremeSliceRequest);
        auto extreme3d = std::make_shared<amrvis::PlotfileDataset>(
            write3dFixture(caseRoot / "volume", low, high),
            amrvis::DatasetId{4}, 1024 * 1024);
        amrvis::LocalDatasetSession session(extreme3d);
        for (const bool logarithmic : {false, true}) {
            const auto visible = amrvis::resolveDisplayRange({}, amrvis::FieldId{0},
                0, amrvis::CompositionPolicy::FinestAvailable,
                amrvis::RangeMode::Visible, std::nullopt, logarithmic,
                extremeSlice.plane);
            require(std::isfinite(visible.minimum) && std::isfinite(visible.maximum)
                    && visible.minimum < visible.maximum
                    && visible.minimum <= low && visible.maximum >= high,
                "an extreme Visible range lost its finite bounds or clipped the data");
            auto extremeSettings = settings;
            extremeSettings.minimum = visible.minimum;
            extremeSettings.maximum = visible.maximum;
            extremeSettings.logarithmic = visible.logarithmic;
            const auto rendered
                = amrvis::renderScalarPlane(extremeSlice.plane, extremeSettings);
            require(rendered.rgba.size() == 4,
                "an extreme Visible range failed to render a slice");
            if (low < high) {
                require(rendered.rgba.front() == palette.slotArgb(amrvis::Palette::paletteStart)
                        && rendered.rgba.back() == palette.slotArgb(
                            amrvis::Palette::paletteStart + amrvis::Palette::colorSlots - 1),
                    "an extreme Visible range did not span the palette");
            }
            const auto levels = amrvis::contourValues(
                visible.minimum, visible.maximum, 4, visible.logarithmic);
            require(std::all_of(levels.begin(), levels.end(),
                        [](double value) { return std::isfinite(value); }),
                "an extreme Visible range produced non-finite contour levels");

            amrvis::VolumeRenderRequest renderRequest;
            renderRequest.dataset = session.id();
            renderRequest.field = amrvis::FieldId{0};
            renderRequest.region = extreme3d->metadata().physicalDomain;
            renderRequest.maximumVoxels = 64;
            renderRequest.outputSize = {16, 16};
            renderRequest.logarithmic = logarithmic;
            renderRequest.transfer.colors = {0xFF0000U, 0x0000FFU};
            renderRequest.transfer.opacities = {1.0F, 1.0F};
            const auto frame = session.renderVolume(renderRequest);
            amrvis::validateSessionVolumeResult(
                session.metadata(), renderRequest, frame);
            require(std::any_of(frame.pixels.begin(), frame.pixels.end(),
                        [](std::uint32_t pixel) { return (pixel >> 24U) != 0; }),
                "an extreme Visible range rendered a transparent volume");
            require(frame.usedRange.minimum == visible.minimum
                    && frame.usedRange.maximum == visible.maximum
                    && frame.usedRange.logarithmic == visible.logarithmic,
                "slice and volume resolved different extreme Visible ranges");
            renderRequest.range = frame.usedRange;
            require(session.renderVolume(renderRequest).pixels == frame.pixels,
                "reusing an extreme volume range changed the rendered frame");
        }
    }

    std::filesystem::remove_all(scratch);
    return 0;
}
