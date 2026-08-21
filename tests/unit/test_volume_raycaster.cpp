// The ray caster over synthetic grids: a lone opaque voxel lands where the
// shared camera projects it and nowhere else; a uniform slab composites to
// the analytic opacity whatever the sampling rate; the presets and a quarter
// turn map the axes the way the labels promise; the output does not depend
// on the thread split; the value mapping matches the 2-D renderer's; and
// cancellation and the range helper behave.

#include <amrexplorer/render3d/VolumeRaycaster.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
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

    // --- an anisotropic grid still spends samplesPerVoxel on a voxel -------
    // A grid four voxels deep and thirty-two wide: a step sized by the
    // smallest pitch would take eight times samplesPerVoxel samples in every
    // z voxel and composite an entry authored at 0.2 to 0.999.
    {
        amrvis::VolumeGrid grid;
        grid.dims = {32, 32, 4};
        grid.region = unitBox();
        grid.values.assign(32U * 32U * 4U, 1.0F);
        grid.coveredVoxels = grid.values.size();
        const auto frame = amrvis::raycastVolume(grid,
            settingsFor(amrvis::orthoPresetXY, 64, twoEntries(0x0000FFU, 0.2F)));
        const auto expected = 1.0 - std::pow(0.8, 4.0);   // 0.5904
        require(std::abs(static_cast<double>(alphaOf(pixelAt(frame, 32, 32))) / 255.0
                    - expected) <= 1.5 / 255.0,
            "a coarse view axis was sampled more than once per voxel");
    }
    // The other way round: a grid finely divided along an axis the view never
    // travels along. The step must follow the axes the ray actually crosses,
    // or a ray crossing two x voxels takes forty thousand samples and paints
    // a 0.5 entry opaque.
    {
        amrvis::VolumeGrid grid;
        grid.dims = {2, 2, 20000};
        grid.region = unitBox();
        grid.values.assign(2U * 2U * 20000U, 1.0F);
        grid.coveredVoxels = grid.values.size();
        const auto frame = amrvis::raycastVolume(grid,
            settingsFor(amrvis::orthoPresetYZ, 32, twoEntries(0x0000FFU, 0.5F)));
        const auto expected = 1.0 - std::pow(0.5, 2.0);   // two x voxels
        require(std::abs(static_cast<double>(alphaOf(pixelAt(frame, 16, 16))) / 255.0
                    - expected) <= 1.5 / 255.0,
            "a fine axis the ray does not travel along set the step");
    }

    // --- an oblique ray composites its physical path, not its voxel count --
    // A uniform cube seen down its body diagonal: the centre ray runs corner
    // to corner, a path of sqrt(3) against 1 face-on, so it must composite
    // sqrt(3) times the material -- not 3 times, which is what counting the
    // voxels the ray *enters* would give (it meets all three families of
    // voxel planes at once along that direction). An odd viewport puts a
    // pixel centre exactly on the domain centre, so the centre ray really is
    // the corner-to-corner one.
    {
        const auto grid = uniformGrid(8, 1.0F);
        const auto transfer = twoEntries(0x0000FFU, 0.1F);
        const auto faceOn = amrvis::raycastVolume(grid,
            settingsFor(amrvis::orthoPresetXY, 65, transfer));
        // Down the body diagonal: elevation acos(1/sqrt(3)), azimuth 45 deg.
        const amrvis::OrthoCamera diagonal{
            0.7853981633974483, 0.9553166181245093, 1.0};
        const auto oblique = amrvis::raycastVolume(grid,
            settingsFor(diagonal, 65, transfer));
        const auto alphaOfCentre = [](const amrvis::VolumeFrame& f) {
            return static_cast<double>(alphaOf(pixelAt(f, 32, 32))) / 255.0;
        };
        require(std::abs(alphaOfCentre(faceOn) - (1.0 - std::pow(0.9, 8.0)))
                <= 1.5 / 255.0,
            "the face-on slab is not the analytic composite");
        // The tolerance is wider than the face-on case because the sample
        // count is an integer: the diagonal path is 8*sqrt(3) voxels, not a
        // whole number of them, so the composite is quantised by up to half a
        // sample. It is still nowhere near the 235 that counting voxel
        // entries would give.
        require(std::abs(alphaOfCentre(oblique)
                    - (1.0 - std::pow(0.9, 8.0 * std::sqrt(3.0)))) <= 3.0 / 255.0,
            "the oblique slab does not composite its physical path length");
    }

    // --- a region reaching past the domain is not clipped at the origin ----
    // The camera is normalised to the domain and its rays start just outside
    // it, but the grid's region need not be the domain: here it reaches to
    // z = 10 while the domain is the unit box, so the whole lit slab sits
    // behind the ray origin at z = 2.5 and only a march that accepts a
    // negative entry parameter finds it.
    {
        amrvis::VolumeGrid grid;
        grid.dims = {4, 4, 20};
        grid.region.lower = {{0.0, 0.0, -10.0}};
        grid.region.upper = {{1.0, 1.0, 10.0}};
        grid.values.assign(4U * 4U * 20U, 0.0F);
        for (int j = 0; j < 4; ++j) {
            for (int i = 0; i < 4; ++i) {
                grid.values[voxel(grid, i, j, 19)] = 1.0F;   // z in [9, 10]
            }
        }
        grid.coveredVoxels = grid.values.size();
        const auto frame = amrvis::raycastVolume(grid,
            settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0xFF0000U, 1.0F)));
        require(alphaOf(pixelAt(frame, 16, 16)) == 255
                && redOf(pixelAt(frame, 16, 16)) == 255,
            "the part of the region in front of the ray origin was clipped away");
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
        // A range that can map nothing, which the helper is reachable with on
        // its own: entry 0 would look like an answer.
        const auto huge = std::numeric_limits<double>::max();
        require(!amrvis::transferEntryFor(
                    5.0, amrvis::VolumeRange{-1.0, 10.0, true}, 253).has_value()
                && !amrvis::transferEntryFor(
                    5.0, amrvis::VolumeRange{1.0, 1.0, false}, 253).has_value()
                && !amrvis::transferEntryFor(
                    5.0, amrvis::VolumeRange{-huge, huge, false}, 253).has_value()
                && !amrvis::transferEntryFor(5.0, linear, 0).has_value(),
            "a range that can map nothing returned an entry");
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
        // The scan runs over the whole grid, so it stops when the token does.
        amrvis::StopSource stop;
        stop.request_stop();
        bool threw = false;
        try {
            (void)amrvis::volumeGridRange(grid, false, stop.get_token());
        } catch (const amrvis::ReadCancelled&) {
            threw = true;
        }
        require(threw, "a cancelled grid scan did not throw ReadCancelled");
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
        // An infinite span would map every value to the bottom entry.
        bad.range = {-std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max(), false};
        require(rejects(bad), "a range with an infinite span was accepted");
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
        // Dims whose product overflows 64 bits: wrapped it is zero, which
        // matches an empty values vector and would let every ray index it.
        amrvis::VolumeGrid overflowing;
        overflowing.dims = {4194304, 2097152, 2097152};
        overflowing.region = unitBox();
        threwGrid = false;
        try {
            (void)amrvis::raycastVolume(overflowing,
                settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0xFFU, 1.0F)));
        } catch (const std::invalid_argument&) {
            threwGrid = true;
        }
        require(threwGrid, "dims whose voxel count overflows were accepted");
        // Over the sampler's budget: refused before the storage is looked at,
        // so a caller does not have to have allocated it to be told.
        amrvis::VolumeGrid oversized;
        oversized.dims = {512, 512, 513};
        oversized.region = unitBox();
        threwGrid = false;
        try {
            (void)amrvis::raycastVolume(oversized,
                settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0xFFU, 1.0F)));
        } catch (const std::invalid_argument&) {
            threwGrid = true;
        }
        require(threwGrid, "a grid over the voxel budget was accepted");

        // The guards the settings carry, each on its own.
        const auto huge = std::numeric_limits<double>::max();
        bad = settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0xFFU, 1.0F));
        bad.domain.lower[0] = -huge;
        bad.domain.upper[0] = huge;
        require(rejects(bad),
            "a domain whose span overflows to infinity was accepted");
        bad = settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0xFFU, 1.0F));
        bad.camera.azimuth = std::numeric_limits<double>::quiet_NaN();
        require(rejects(bad), "a non-finite camera angle was accepted");
        bad = settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0xFFU, 1.0F));
        bad.camera.zoom = 0.5 * amrvis::minVolumeZoom;
        require(rejects(bad), "a zoom below the minimum was accepted");
        bad = settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0xFFU, 1.0F));
        bad.outputSize = {amrvis::maxVolumeOutputDimension + 1, 32};
        require(rejects(bad), "an output dimension past the cap was accepted");

        // The pitch guard, for the two ways a box RealBox::valid accepts
        // still fails to give a usable pitch.
        const auto rejectsGrid = [](const amrvis::VolumeGrid& g) {
            try {
                (void)amrvis::raycastVolume(g,
                    settingsFor(amrvis::orthoPresetXY, 32, twoEntries(0xFFU, 1.0F)));
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };
        auto thin = uniformGrid(8, 1.0F);
        // A denormal span over eight voxels divides to exactly zero.
        thin.region.upper[2] = 4.0 * std::numeric_limits<double>::denorm_min();
        require(rejectsGrid(thin), "a region whose pitch underflows was accepted");
        auto wide = uniformGrid(8, 1.0F);
        wide.region.lower[2] = -huge;
        wide.region.upper[2] = huge;
        require(rejectsGrid(wide), "a region whose span is infinite was accepted");
    }

    // --- a single-row frame still honours the token ------------------------
    // The row loop is where cancellation used to be polled, so a frame with
    // one row is the shape that could bypass it entirely.
    {
        const auto grid = uniformGrid(16, 1.0F);
        auto settings = settingsFor(
            amrvis::orthoPresetXY, 64, twoEntries(0x0000FFU, 0.5F));
        settings.outputSize = {64, 1};
        amrvis::StopSource stop;
        stop.request_stop();
        bool threw = false;
        try {
            (void)amrvis::raycastVolume(grid, settings, stop.get_token());
        } catch (const amrvis::ReadCancelled&) {
            threw = true;
        }
        require(threw, "a cancelled single-row render did not throw ReadCancelled");
        // Uncancelled, the same frame renders: the poll is not swallowing it.
        const auto frame = amrvis::raycastVolume(grid, settings);
        require(frame.width == 64 && frame.height == 1
                && frame.pixels.size() == 64,
            "a single-row frame is not the requested size");
    }

    return 0;
}
