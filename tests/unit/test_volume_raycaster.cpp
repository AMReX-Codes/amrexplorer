// The ray caster over synthetic grids: a lone opaque voxel lands where the
// shared camera projects it and nowhere else; a uniform slab composites to
// the analytic opacity whatever the sampling rate; the presets and a quarter
// turn map the axes the way the labels promise; the output does not depend
// on the thread split; the value mapping matches the 2-D renderer's; and
// cancellation and the range helper behave.

#include <amrexplorer/render3d/VolumeRaycaster.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

amrvis::RealBox unitBox()
{
    amrvis::RealBox box;
    box.lower = {{0.0, 0.0, 0.0}};
    box.upper = {{1.0, 1.0, 1.0}};
    return box;
}

amrvis::VolumeGrid uniformGrid(int n, float value)
{
    amrvis::VolumeGrid grid;
    grid.dims = {n, n, n};
    grid.region = unitBox();
    grid.values.assign(static_cast<std::size_t>(n * n * n), value);
    grid.coveredVoxels = static_cast<std::uint64_t>(n * n * n);
    return grid;
}

std::size_t voxel(const amrvis::VolumeGrid& grid, int i, int j, int k)
{
    return static_cast<std::size_t>(i)
        + static_cast<std::size_t>(grid.dims[0])
            * (static_cast<std::size_t>(j)
                + static_cast<std::size_t>(grid.dims[1]) * static_cast<std::size_t>(k));
}

// Two entries: value 0 -> transparent black, value 1 -> `color` at `opacity`.
amrvis::VolumeTransferFunction twoEntries(std::uint32_t color, float opacity)
{
    amrvis::VolumeTransferFunction transfer;
    transfer.colors = {0x000000U, color};
    transfer.opacities = {0.0F, opacity};
    return transfer;
}

amrvis::RaycastSettings settingsFor(const amrvis::OrthoCamera& camera,
    int size, const amrvis::VolumeTransferFunction& transfer,
    int samplesPerVoxel = 2)
{
    amrvis::RaycastSettings settings;
    settings.camera = camera;
    settings.domain = unitBox();
    settings.outputSize = {size, size};
    settings.range = {0.0, 1.0, false};
    settings.transfer = transfer;
    settings.samplesPerVoxel = samplesPerVoxel;
    settings.threadCount = 1;
    return settings;
}

unsigned alphaOf(std::uint32_t pixel) { return pixel >> 24U; }
unsigned redOf(std::uint32_t pixel) { return (pixel >> 16U) & 0xFFU; }
unsigned greenOf(std::uint32_t pixel) { return (pixel >> 8U) & 0xFFU; }
unsigned blueOf(std::uint32_t pixel) { return pixel & 0xFFU; }

std::uint32_t pixelAt(const amrvis::VolumeFrame& frame, int x, int y)
{
    return frame.pixels[static_cast<std::size_t>(y)
        * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x)];
}

} // namespace

int main()
{
    // --- a lone opaque voxel projects to its footprint and nowhere else -----
    {
        auto grid = uniformGrid(8, 0.0F);
        // Voxel (5, 2, 6): x in [0.625, 0.75), y in [0.25, 0.375), z in
        // [0.75, 0.875).
        grid.values[voxel(grid, 5, 2, 6)] = 1.0F;
        const auto settings = settingsFor(
            amrvis::orthoPresetXY, 200, twoEntries(0xFF8000U, 1.0F));
        const auto frame = amrvis::raycastVolume(grid, settings);
        require(frame.width == 200 && frame.height == 200
                && frame.pixels.size() == 40000 && frame.usedRange == settings.range
                && frame.metrics.gridDims == grid.dims,
            "the frame does not describe the request");
        // The footprint from the shared projection: the voxel's eight corners.
        const auto viewport = amrvis::viewportFrame(200, 200);
        double left = 1e9;
        double right = -1e9;
        double top = 1e9;
        double bottom = -1e9;
        for (const double x : {0.625, 0.75}) {
            for (const double y : {0.25, 0.375}) {
                for (const double z : {0.75, 0.875}) {
                    amrvis::Real3 corner;
                    corner[0] = x;
                    corner[1] = y;
                    corner[2] = z;
                    const auto p = amrvis::projectPoint(
                        settings.camera, viewport, settings.domain, corner);
                    left = std::min(left, p.x);
                    right = std::max(right, p.x);
                    top = std::min(top, p.y);
                    bottom = std::max(bottom, p.y);
                }
            }
        }
        std::size_t lit = 0;
        for (int y = 0; y < 200; ++y) {
            for (int x = 0; x < 200; ++x) {
                const auto pixel = pixelAt(frame, x, y);
                const auto centreX = static_cast<double>(x) + 0.5;
                const auto centreY = static_cast<double>(y) + 0.5;
                const bool inside = centreX > left && centreX < right
                    && centreY > top && centreY < bottom;
                if (inside) {
                    require(alphaOf(pixel) == 255 && redOf(pixel) == 255
                            && greenOf(pixel) == 128 && blueOf(pixel) == 0,
                        "a pixel inside the voxel's footprint is not the voxel");
                    ++lit;
                } else {
                    // Pixels whose centre is within half a pixel of the edge
                    // are allowed either way; anything further out is empty.
                    const bool nearEdge = std::abs(centreX - left) < 1.0
                        || std::abs(centreX - right) < 1.0
                        || std::abs(centreY - top) < 1.0
                        || std::abs(centreY - bottom) < 1.0;
                    require(nearEdge || pixel == 0U,
                        "a pixel outside the voxel's footprint was lit");
                }
            }
        }
        require(lit > 100, "the voxel lit implausibly few pixels");
        // The voxel's footprint sits where XY puts it: right of centre (x >
        // 0.5), below centre (y < 0.5 draws lower on screen).
        require(left > 100.0 && top > 100.0,
            "the XY preset did not place +x right and +y up");
    }

    // --- a uniform slab composites to 1 - (1 - a)^N whatever the sampling ---
    for (const int samplesPerVoxel : {1, 2, 3, 5, 8}) {
        const auto grid = uniformGrid(8, 1.0F);   // eight voxels deep
        const auto settings = settingsFor(amrvis::orthoPresetXY, 64,
            twoEntries(0x0000FFU, 0.3F), samplesPerVoxel);
        const auto frame = amrvis::raycastVolume(grid, settings);
        const auto centre = pixelAt(frame, 32, 32);
        const auto expectedAlpha = 1.0 - std::pow(0.7, 8.0);   // 0.9424
        require(std::abs(static_cast<double>(alphaOf(centre)) / 255.0 - expectedAlpha)
                <= 1.5 / 255.0,
            "the slab's alpha does not match the analytic composite");
        // Premultiplied blue: the colour channel equals the alpha.
        require(blueOf(centre) == alphaOf(centre) && redOf(centre) == 0
                && greenOf(centre) == 0,
            "the slab's colour is not the premultiplied entry colour");
        // Corners are outside the domain's projection: empty.
        require(pixelAt(frame, 0, 0) == 0U && pixelAt(frame, 63, 63) == 0U,
            "pixels outside the domain were lit");
    }
    // A fully opaque entry stops the ray at the first voxel: alpha 255.
    {
        const auto grid = uniformGrid(4, 1.0F);
        const auto frame = amrvis::raycastVolume(grid,
            settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0x00FF00U, 1.0F)));
        require(alphaOf(pixelAt(frame, 16, 16)) == 255
                && greenOf(pixelAt(frame, 16, 16)) == 255,
            "an opaque entry did not saturate the pixel");
    }

    // --- presets map the axes as labelled --------------------------------
    // A grid whose value increases along +x only: entries 0 (transparent)
    // for x < 0.5 and 1 (opaque red) for x >= 0.5.
    {
        auto grid = uniformGrid(8, 0.0F);
        for (int k = 0; k < 8; ++k) {
            for (int j = 0; j < 8; ++j) {
                for (int i = 4; i < 8; ++i) {
                    grid.values[voxel(grid, i, j, k)] = 1.0F;
                }
            }
        }
        const auto transfer = twoEntries(0xFF0000U, 1.0F);
        // XY and XZ: +x is screen right, so the right half is lit, the left
        // half empty.
        for (const auto& camera : {amrvis::orthoPresetXY, amrvis::orthoPresetXZ}) {
            const auto frame = amrvis::raycastVolume(grid,
                settingsFor(camera, 64, transfer));
            require(alphaOf(pixelAt(frame, 40, 32)) == 255
                    && pixelAt(frame, 24, 32) == 0U,
                "XY/XZ did not put +x on the right");
        }
        // YZ: screen right is +y and the view runs along x, so every pixel
        // over the domain sees the lit half somewhere along its ray.
        {
            const auto frame = amrvis::raycastVolume(grid,
                settingsFor(amrvis::orthoPresetYZ, 64, transfer));
            require(alphaOf(pixelAt(frame, 40, 32)) == 255
                    && alphaOf(pixelAt(frame, 24, 32)) == 255,
                "YZ did not look along x");
        }
        // A quarter turn of azimuth from XY puts +x at the top of the screen
        // (x1 = -ny, y2 = nx): the upper half lit, the lower half empty.
        {
            const amrvis::OrthoCamera turned{1.5707963267948966, 0.0, 1.0};
            const auto frame = amrvis::raycastVolume(grid,
                settingsFor(turned, 64, transfer));
            require(alphaOf(pixelAt(frame, 32, 24)) == 255
                    && pixelAt(frame, 32, 40) == 0U,
                "a quarter turn did not rotate the picture");
        }
    }

    // --- thread invariance -------------------------------------------------
    {
        auto grid = uniformGrid(16, 0.0F);
        for (std::size_t index = 0; index < grid.values.size(); ++index) {
            grid.values[index] = static_cast<float>((index * 7919U) % 101U) / 100.0F;
        }
        amrvis::VolumeTransferFunction transfer;
        for (int entry = 0; entry < 16; ++entry) {
            transfer.colors.push_back(static_cast<std::uint32_t>(entry * 16)
                | (static_cast<std::uint32_t>(255 - entry * 16) << 16U));
            transfer.opacities.push_back(static_cast<float>(entry) / 40.0F);
        }
        const amrvis::OrthoCamera oblique{0.7, -0.4, 1.3};
        auto single = settingsFor(oblique, 97, transfer, 3);
        auto many = single;
        many.threadCount = 7;
        auto oversubscribed = single;
        oversubscribed.threadCount = 500;   // more threads than rows: clamped
        const auto one = amrvis::raycastVolume(grid, single);
        const auto seven = amrvis::raycastVolume(grid, many);
        const auto clamped = amrvis::raycastVolume(grid, oversubscribed);
        require(one.pixels == seven.pixels && one.pixels == clamped.pixels,
            "the thread split changed the picture");
        std::size_t lit = 0;
        for (const auto pixel : one.pixels) {
            lit += pixel != 0U;
        }
        require(lit > 0, "the oblique render lit nothing");
    }

    // --- the value mapping is the 2-D renderer's --------------------------
    {
        const amrvis::VolumeRange linear{0.0, 49.0, false};
        require(amrvis::transferEntryFor(24.5, linear, 253) == 126
                && amrvis::transferEntryFor(49.0, linear, 253) == 252
                && amrvis::transferEntryFor(-1.0, linear, 253) == 0
                && amrvis::transferEntryFor(0.0, linear, 253) == 0
                && amrvis::transferEntryFor(60.0, linear, 253) == 252,
            "the linear mapping does not truncate like the palette");
        const amrvis::VolumeRange logarithmic{1.0, 100.0, true};
        require(amrvis::transferEntryFor(10.0, logarithmic, 253) == 126
                && amrvis::transferEntryFor(1.0, logarithmic, 253) == 0
                && !amrvis::transferEntryFor(0.0, logarithmic, 253).has_value()
                && !amrvis::transferEntryFor(-3.0, logarithmic, 253).has_value(),
            "the logarithmic mapping is wrong");
        require(!amrvis::transferEntryFor(
                    std::numeric_limits<double>::quiet_NaN(), linear, 253).has_value()
                && !amrvis::transferEntryFor(
                    std::numeric_limits<double>::infinity(), linear, 253).has_value(),
            "a non-finite value mapped to an entry");
        // The renderer honours a logarithmic range: value 10 in [1, 100]
        // takes the middle entry's colour.
        auto grid = uniformGrid(4, 10.0F);
        amrvis::VolumeTransferFunction transfer;
        transfer.colors = {0xFF0000U, 0x00FF00U, 0x0000FFU};
        transfer.opacities = {1.0F, 1.0F, 1.0F};
        auto settings = settingsFor(amrvis::orthoPresetXY, 32, transfer);
        settings.range = logarithmic;
        const auto frame = amrvis::raycastVolume(grid, settings);
        require(greenOf(pixelAt(frame, 16, 16)) == 255
                && redOf(pixelAt(frame, 16, 16)) == 0,
            "the logarithmic range did not select the middle entry");
    }

    // --- NaN voxels are transparent ---------------------------------------
    {
        auto grid = uniformGrid(4, std::numeric_limits<float>::quiet_NaN());
        grid.coveredVoxels = 0;
        const auto frame = amrvis::raycastVolume(grid,
            settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0xFFFFFFU, 1.0F)));
        require(std::all_of(frame.pixels.begin(), frame.pixels.end(),
                    [](std::uint32_t pixel) { return pixel == 0U; }),
            "uncovered voxels were painted");
    }

    // --- volumeGridRange ---------------------------------------------------
    {
        auto grid = uniformGrid(2, 0.0F);
        grid.values = {-2.0F, 0.0F, 3.0F, std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(), 5.0F, 0.5F, 1.0F};
        const auto linear = amrvis::volumeGridRange(grid, false);
        const auto logarithmic = amrvis::volumeGridRange(grid, true);
        require(linear && linear->first == -2.0 && linear->second == 5.0,
            "the linear grid range is not the finite extrema");
        require(logarithmic && logarithmic->first == 0.5 && logarithmic->second == 5.0,
            "the logarithmic grid range did not skip non-positive values");
        grid.values.assign(8, std::numeric_limits<float>::quiet_NaN());
        require(!amrvis::volumeGridRange(grid, false).has_value(),
            "an all-NaN grid reported a range");
        grid.values.assign(8, -1.0F);
        require(!amrvis::volumeGridRange(grid, true).has_value()
                && amrvis::volumeGridRange(grid, false)->first == -1.0,
            "a non-positive grid reported a logarithmic range");
    }

    // --- refusals and cancellation ----------------------------------------
    {
        const auto grid = uniformGrid(4, 1.0F);
        auto settings = settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0xFFU, 1.0F));
        amrvis::StopSource stop;
        stop.request_stop();
        bool threw = false;
        try {
            (void)amrvis::raycastVolume(grid, settings, stop.get_token());
        } catch (const amrvis::ReadCancelled&) {
            threw = true;
        }
        require(threw, "a cancelled render did not throw ReadCancelled");
        settings.threadCount = 4;
        threw = false;
        try {
            (void)amrvis::raycastVolume(grid, settings, stop.get_token());
        } catch (const amrvis::ReadCancelled&) {
            threw = true;
        }
        require(threw, "a cancelled threaded render did not throw ReadCancelled");

        const auto rejects = [&grid](const amrvis::RaycastSettings& bad) {
            try {
                (void)amrvis::raycastVolume(grid, bad);
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };
        auto bad = settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0xFFU, 1.0F));
        bad.range = {1.0, 1.0, false};
        require(rejects(bad), "an empty range was accepted");
        bad = settingsFor(amrvis::orthoPresetXY, 0, twoEntries(0xFFU, 1.0F));
        require(rejects(bad), "a zero-size output was accepted");
        bad = settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0xFFU, 1.0F));
        bad.samplesPerVoxel = 0;
        require(rejects(bad), "zero samples per voxel was accepted");
        bad = settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0xFFU, 1.0F));
        bad.transfer.opacities.pop_back();
        require(rejects(bad), "a malformed transfer function was accepted");
        auto malformed = grid;
        malformed.values.pop_back();
        bool threwGrid = false;
        try {
            (void)amrvis::raycastVolume(malformed,
                settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0xFFU, 1.0F)));
        } catch (const std::invalid_argument&) {
            threwGrid = true;
        }
        require(threwGrid, "a grid whose storage mismatches its dims was accepted");
    }
    return 0;
}
