// The ray caster over synthetic grids: a lone opaque voxel lands where the
// shared camera projects it and nowhere else; a uniform slab composites to
// the analytic opacity whatever the sampling rate; the presets and a quarter
// turn map the axes the way the labels promise; the output does not depend
// on the thread split; the value mapping matches the 2-D renderer's; and
// cancellation and the range helper behave.

#include <amrexplorer/render3d/VolumeRaycaster.hpp>

#include <amrexplorer/core/ValueMapping.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>
#include <utility>
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
    // In size_t, not int: n * n * n overflows a signed int past n = 1290,
    // and this is the helper a larger-grid test would reach for.
    const auto count = static_cast<std::size_t>(n)
        * static_cast<std::size_t>(n) * static_cast<std::size_t>(n);
    grid.values.assign(count, value);
    grid.coveredVoxels = count;
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
    // Nearest unless a case asks otherwise. Most of what is checked below is
    // the march -- step length, opacity correction, path length, threading --
    // and reading one voxel per sample keeps a case's expected value the
    // voxel's own, so those checks stay about the thing they name.
    settings.sampling = amrvis::SamplingPolicy::Nearest;
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

// A field equal to the coordinate along `axis`: voxel centres sit at
// (index + 0.5) / n, so the trilinear read reproduces the coordinate exactly
// between the outermost centres.
amrvis::VolumeGrid rampGrid(int n, int axis)
{
    auto grid = uniformGrid(n, 0.0F);
    for (int k = 0; k < n; ++k) {
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const int index = axis == 0 ? i : axis == 1 ? j : k;
                grid.values[voxel(grid, i, j, k)]
                    = (static_cast<double>(index) + 0.5) / static_cast<double>(n);
            }
        }
    }
    return grid;
}

// The distance from `centre`, so its iso-value r is a sphere of that radius.
amrvis::VolumeGrid sphereGrid(int n, const std::array<double, 3>& centre)
{
    auto grid = uniformGrid(n, 0.0F);
    for (int k = 0; k < n; ++k) {
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const auto dx = (static_cast<double>(i) + 0.5) / n - centre[0];
                const auto dy = (static_cast<double>(j) + 0.5) / n - centre[1];
                const auto dz = (static_cast<double>(k) + 0.5) / n - centre[2];
                grid.values[voxel(grid, i, j, k)] = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
        }
    }
    return grid;
}

amrvis::VolumeIsosurface isosurface(double value, std::uint32_t color, float opacity)
{
    amrvis::VolumeIsosurface iso;
    iso.field.value = 1;
    iso.value = value;
    iso.color = color;
    iso.opacity = opacity;
    return iso;
}

// Settings for an isosurface alone: the volume is off, its transfer function
// present only because the settings must still validate.
amrvis::RaycastSettings isoOnlySettings(const amrvis::OrthoCamera& camera, int size,
    const amrvis::VolumeIsosurface& iso, int samplesPerVoxel = 2)
{
    auto settings = settingsFor(camera, size, twoEntries(0xFFFFFFU, 1.0F), samplesPerVoxel);
    settings.showVolume = false;
    settings.isosurface = iso;
    return settings;
}

// The colour a hit composites for a channel value in [0, 1] and the cosine
// between the normal and the view: the header's headlight terms.
double shaded(double channel, double cosTheta)
{
    double highlight = 1.0;
    for (int power = 0; power < amrvis::isosurfaceShininess; ++power) {
        highlight *= cosTheta;
    }
    return channel * (amrvis::isosurfaceAmbient + amrvis::isosurfaceDiffuse * cosTheta)
        + amrvis::isosurfaceSpecular * highlight;
}

unsigned byteOf(double value)
{
    return static_cast<unsigned>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

std::size_t litCount(const amrvis::VolumeFrame& frame)
{
    std::size_t lit = 0;
    for (const auto pixel : frame.pixels) {
        lit += pixel != 0U;
    }
    return lit;
}

} // namespace

int main()
{
    // --- a lone opaque voxel projects to its footprint and nowhere else -----
    //
    // Nearest sampling, which this pins: the footprint is exactly the voxel's
    // own projection, with nothing outside it. Linear deliberately breaks
    // both halves -- it spreads the voxel half a voxel further and drops the
    // value below the entry threshold inside -- and is checked on its own
    // terms further down.
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
        // Both policies, because rows are handed out on demand and Linear
        // carries a cache that lives for one ray: a cache that outlived its
        // ray would make a pixel depend on which worker reached it and in
        // what order, and Nearest touches neither piece of machinery.
        for (const auto policy : {amrvis::SamplingPolicy::Nearest,
                 amrvis::SamplingPolicy::Linear}) {
            auto single = settingsFor(oblique, 97, transfer, 3);
            single.sampling = policy;
            auto many = single;
            many.threadCount = 7;
            auto oversubscribed = single;
            oversubscribed.threadCount = 500;   // more threads than rows
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
                && !amrvis::transferEntryFor(5.0, linear, 0).has_value(),
            "a range that can map nothing returned an entry");
        const amrvis::VolumeRange wide{-huge, huge, false};
        require(amrvis::transferEntryFor(-huge, wide, 253) == 0
                && amrvis::transferEntryFor(-huge / 2.0, wide, 253) == 63
                && amrvis::transferEntryFor(0.0, wide, 253) == 126
                && amrvis::transferEntryFor(huge / 2.0, wide, 253) == 189
                && amrvis::transferEntryFor(huge, wide, 253) == 252,
            "an overflowing span did not preserve transfer-function slots");
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

    // Trilinear interpolation must preserve the picture when finite corners
    // acquire a difference larger than DBL_MAX, in any of the three axes.
    for (int axis = 0; axis < 3; ++axis) {
        auto grid = uniformGrid(2, 0.0F);
        for (std::size_t i = 0; i < grid.values.size(); ++i) {
            grid.values[i] = ((i >> axis) & 1U) != 0 ? 1.0 : -1.0;
        }
        amrvis::VolumeTransferFunction transfer;
        transfer.colors = {0xFF0000U, 0x00FF00U, 0x0000FFU};
        transfer.opacities = {1.0F, 1.0F, 1.0F};
        auto settings = settingsFor(amrvis::orthoPresetXY, 64, transfer);
        settings.sampling = amrvis::SamplingPolicy::Linear;
        settings.range = {-1.0, 1.0, false};
        const auto reference = amrvis::raycastVolume(grid, settings);
        for (const double magnitude : {1.0e308, std::numeric_limits<double>::max()}) {
            auto scaled = grid;
            for (auto& value : scaled.values) {
                value *= magnitude;
            }
            settings.range = {-magnitude, magnitude, false};
            require(amrvis::raycastVolume(scaled, settings).pixels == reference.pixels,
                "extreme finite corners changed trilinear volume colors");
        }
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

    // --- linear sampling reads between the voxel centres --------------------
    //
    // A step from 0 to 1 between voxels 3 and 4 of an 8-voxel axis. Nearest
    // jumps at the face they share, x = 0.5. Linear ramps between their
    // centres, x = 0.4375 to x = 0.5625, and with two entries over [0, 1] only
    // the very top of that ramp selects the opaque entry -- so the picture
    // starts at the far centre, x = 0.5625, not at the face.
    //
    // This is the half-voxel offset, and it is the whole reason to write the
    // case: voxel i is centred at i + 0.5, so interpolating without shifting
    // by half a voxel moves the field half a voxel through the grid. That
    // renders perfectly plausibly -- it just shows the wrong place, and every
    // other check here would still pass.
    {
        constexpr int n = 8;
        auto grid = uniformGrid(n, 0.0F);
        for (int k = 0; k < n; ++k) {
            for (int j = 0; j < n; ++j) {
                for (int i = n / 2; i < n; ++i) {
                    grid.values[voxel(grid, i, j, k)] = 1.0F;
                }
            }
        }
        auto settings = settingsFor(
            amrvis::orthoPresetXY, 256, twoEntries(0x0000FFU, 1.0F), 4);
        const auto viewport = amrvis::viewportFrame(256, 256);
        const auto columnOf = [&](double worldX) {
            amrvis::Real3 point;
            point[0] = worldX;
            point[1] = 0.5;
            point[2] = 0.5;
            return amrvis::projectPoint(
                settings.camera, viewport, settings.domain, point)
                .x;
        };
        const auto firstLit = [&](const amrvis::VolumeFrame& frame) {
            for (int x = 0; x < 256; ++x) {
                if (alphaOf(pixelAt(frame, x, 128)) > 0U) {
                    return x;
                }
            }
            return -1;
        };

        settings.sampling = amrvis::SamplingPolicy::Nearest;
        const auto nearestFrame = amrvis::raycastVolume(grid, settings);
        require(std::abs(static_cast<double>(firstLit(nearestFrame))
                    - columnOf(0.5))
                <= 1.0,
            "nearest sampling did not step at the face between the voxels");

        settings.sampling = amrvis::SamplingPolicy::Linear;
        const auto linearFrame = amrvis::raycastVolume(grid, settings);
        require(std::abs(static_cast<double>(firstLit(linearFrame))
                    - columnOf(0.5625))
                <= 1.0,
            "linear sampling did not reach the top of its ramp at the far "
            "voxel centre, so the half-voxel offset is wrong");
    }

    // --- linear sampling reproduces a linear field --------------------------
    //
    // The property that says the weights are right rather than merely smooth:
    // over a field linear in the grid's own coordinates, interpolating between
    // the centres returns the field itself. Read through the transfer
    // function, which is the only way a value leaves a render: 253 entries,
    // each painting its own index into the blue channel, so the pixel names
    // the entry the sample chose and through valueSlot the value it read.
    //
    // The field varies along x and y, not one axis: with a single axis every
    // per-axis weight could be swapped for another and nothing would show it,
    // because the other axes' fractions never matter. It stays constant along
    // z so that a ray down z crosses one value and the pixel is unambiguous.
    {
        constexpr int n = 16;
        constexpr int entryCount = 253;
        auto grid = uniformGrid(n, 0.0F);
        for (int k = 0; k < n; ++k) {
            for (int j = 0; j < n; ++j) {
                for (int i = 0; i < n; ++i) {
                    grid.values[voxel(grid, i, j, k)]
                        = static_cast<float>(i + 3 * j)
                        / static_cast<float>(4 * (n - 1));
                }
            }
        }
        amrvis::VolumeTransferFunction transfer;
        for (int entry = 0; entry < entryCount; ++entry) {
            transfer.colors.push_back(static_cast<std::uint32_t>(entry));
            transfer.opacities.push_back(1.0F);
        }
        auto settings = settingsFor(amrvis::orthoPresetXY, 128, transfer, 1);
        settings.sampling = amrvis::SamplingPolicy::Linear;
        const auto frame = amrvis::raycastVolume(grid, settings);
        const auto viewport = amrvis::viewportFrame(128, 128);
        // Bisect the projection the renderer used rather than reinventing it,
        // so this cannot drift from the camera. The XY preset maps world x to
        // screen x and world y to screen y, so each axis inverts on its own.
        const auto worldOn = [&](std::size_t axis, double screen) {
            double low = 0.0;
            double high = 1.0;
            for (int step = 0; step < 60; ++step) {
                const auto mid = 0.5 * (low + high);
                amrvis::Real3 point;
                point[0] = 0.5;
                point[1] = 0.5;
                point[2] = 0.5;
                point[axis] = mid;
                const auto projected = amrvis::projectPoint(
                    settings.camera, viewport, settings.domain, point);
                const auto at = axis == 0 ? projected.x : projected.y;
                const auto rising = axis == 0;
                if ((at < screen) == rising) {
                    low = mid;
                } else {
                    high = mid;
                }
            }
            return 0.5 * (low + high);
        };
        // Down the diagonal, so both fractions vary and neither can stand in
        // for the other. Only pixels the domain actually covers: the camera
        // fits the whole box, so the outer ones render nothing at all.
        auto checked = 0;
        for (int step = 24; step < 104; ++step) {
            if (alphaOf(pixelAt(frame, step, step)) == 0U) {
                continue;
            }
            ++checked;
            // Voxel i is centred at i + 0.5 voxels, so this is the field the
            // grid holds, evaluated where the ray actually goes.
            // Clamped the way the sampler clamps: outside the outermost
            // centres the bracket collapses onto the edge voxel and the field
            // reads flat, so the half voxel at each end is not the linear
            // extrapolation of it.
            const auto onCentres = [](double world) {
                const auto centres = world * static_cast<double>(n) - 0.5;
                return std::clamp(centres, 0.0, static_cast<double>(n - 1));
            };
            const auto centreX = onCentres(worldOn(0, static_cast<double>(step) + 0.5));
            const auto centreY = onCentres(worldOn(1, static_cast<double>(step) + 0.5));
            // Weighted unequally on purpose: a field symmetric in x and y
            // reads the same with the two weights exchanged, so it cannot
            // tell a correct interpolation from one that swaps them.
            const auto expected
                = (centreX + 3.0 * centreY) / static_cast<double>(4 * (n - 1));
            const auto slot = amrvis::valueSlot(
                expected, amrvis::ResolvedValueRange{0.0, 1.0, false},
                entryCount);
            const auto seen
                = static_cast<int>(blueOf(pixelAt(frame, step, step)));
            require(std::abs(seen - slot) <= 1,
                "linear sampling did not reproduce a field linear in the "
                "grid's own coordinates");
        }
        require(checked > 40,
            "too few pixels landed on the domain to have checked the field");
    }

    // --- linear sampling interpolates along the ray as well -----------------
    //
    // Everything above varies across the ray, not along it, so a ray sees one
    // value the whole way and nothing checks the axis it travels down. That
    // axis is the one a per-ray cache can get wrong -- reuse the cell a ray
    // started in and the field looks constant along it, which those cases
    // cannot see -- and it is also where a mixed-up z weight hides.
    //
    // A step along z instead, read through the accumulated opacity. Nearest
    // turns opaque at the face between voxels 3 and 4 (z = 0.5, four voxels of
    // it); linear only where the ramp between their centres reaches the top
    // entry, at z = 0.5625, which is three and a half voxels. The opacity
    // correction makes a voxel contribute its entry's opacity once, so those
    // are 1 - 0.7^4 and 1 - 0.7^3.5 -- far enough apart to tell, and both far
    // from the 1 - 0.7^8 a ray would accumulate if it read one cell all the
    // way down.
    {
        constexpr int n = 8;
        auto grid = uniformGrid(n, 0.0F);
        for (int k = n / 2; k < n; ++k) {
            for (int j = 0; j < n; ++j) {
                for (int i = 0; i < n; ++i) {
                    grid.values[voxel(grid, i, j, k)] = 1.0F;
                }
            }
        }
        auto settings = settingsFor(
            amrvis::orthoPresetXY, 64, twoEntries(0x0000FFU, 0.3F), 4);
        settings.sampling = amrvis::SamplingPolicy::Linear;
        const auto linearFrame = amrvis::raycastVolume(grid, settings);
        settings.sampling = amrvis::SamplingPolicy::Nearest;
        const auto nearestFrame = amrvis::raycastVolume(grid, settings);
        const auto centreAlpha = [](const amrvis::VolumeFrame& frame) {
            return static_cast<double>(alphaOf(pixelAt(frame, 32, 32))) / 255.0;
        };
        require(std::abs(centreAlpha(nearestFrame) - (1.0 - std::pow(0.7, 4.0)))
                <= 4.0 / 255.0,
            "nearest sampling did not turn opaque at the face along the ray");
        require(std::abs(centreAlpha(linearFrame) - (1.0 - std::pow(0.7, 3.5)))
                <= 4.0 / 255.0,
            "linear sampling did not interpolate along the ray, which is the "
            "axis a reused cell would freeze");
    }

    // --- the ray's own axis is interpolated, whichever axis that is ---------
    //
    // The step case above proves the axis a ray travels down is interpolated;
    // it does not prove the right weight reaches it, because a step's corners
    // are 0 and 1 and any weight short of 1 leaves the value below the top
    // entry, so a substituted weight lands the transition in the same place.
    // A ramp exposes it: the value inside a cell then depends on the fraction,
    // and a wrong one -- another axis's, or a frozen one from a cell held over
    // from the previous sample -- moves where the ramp crosses the entry
    // threshold.
    //
    // Once per preset, since each looks down a different axis: the XY view
    // travels z, XZ travels y, YZ travels x. A cache that compares only some
    // of the three indices, or a weight taken from the wrong axis, survives
    // every check that exercises one axis alone.
    {
        constexpr int n = 8;
        const std::array<std::pair<amrvis::OrthoCamera, std::size_t>, 3> views{
            {{amrvis::orthoPresetXY, 2}, {amrvis::orthoPresetXZ, 1},
                {amrvis::orthoPresetYZ, 0}}};
        for (const auto& [camera, axis] : views) {
            auto grid = uniformGrid(n, 0.0F);
            for (int k = 0; k < n; ++k) {
                for (int j = 0; j < n; ++j) {
                    for (int i = 0; i < n; ++i) {
                        const std::array<int, 3> at{i, j, k};
                        grid.values[voxel(grid, i, j, k)]
                            = static_cast<float>(at[axis])
                            / static_cast<float>(n - 1);
                    }
                }
            }
            auto settings = settingsFor(
                camera, 64, twoEntries(0x0000FFU, 0.3F), 4);
            // Half the field's span, so the top entry begins halfway up the
            // ramp -- at the centre of voxel 3.5, which is the middle of the
            // grid -- instead of only at its very top.
            settings.range = {0.0, 0.5, false};
            settings.sampling = amrvis::SamplingPolicy::Linear;
            const auto frame = amrvis::raycastVolume(grid, settings);
            const auto alpha
                = static_cast<double>(alphaOf(pixelAt(frame, 32, 32))) / 255.0;
            // The ramp reaches the threshold at the middle of the grid, so
            // four of the eight voxels along the ray are opaque.
            require(std::abs(alpha - (1.0 - std::pow(0.7, 4.0))) <= 4.0 / 255.0,
                "linear sampling did not interpolate correctly along the axis "
                "the ray travels down");
        }
    }

    // --- linear sampling does not erode a coverage boundary -----------------
    //
    // The rule a plain eight-corner average gets wrong: a stencil touching an
    // uncovered voxel would average NaN and drop the sample, eating a
    // voxel-wide transparent rind inward from every boundary. A covered voxel
    // beside NaN keeps its own value instead, which is what the slice's
    // bilinear sampler does at a domain edge.
    {
        constexpr int n = 8;
        auto grid = uniformGrid(n, 1.0F);
        // Uncover the whole upper half in x, leaving a boundary mid-grid.
        for (int k = 0; k < n; ++k) {
            for (int j = 0; j < n; ++j) {
                for (int i = n / 2; i < n; ++i) {
                    grid.values[voxel(grid, i, j, k)]
                        = std::numeric_limits<float>::quiet_NaN();
                }
            }
        }
        grid.coveredVoxels = grid.values.size() / 2;
        auto settings = settingsFor(
            amrvis::orthoPresetXY, 128, twoEntries(0xFFFFFFU, 1.0F), 4);
        const auto countLit = [&](amrvis::SamplingPolicy policy) {
            settings.sampling = policy;
            const auto frame = amrvis::raycastVolume(grid, settings);
            auto lit = 0;
            for (int x = 0; x < 128; ++x) {
                if (alphaOf(pixelAt(frame, x, 64)) > 0U) {
                    ++lit;
                }
            }
            return lit;
        };
        require(countLit(amrvis::SamplingPolicy::Linear)
                == countLit(amrvis::SamplingPolicy::Nearest),
            "linear sampling lit a different width than nearest at a coverage "
            "boundary, so the stencil ate into the covered side");
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
        // Including when there is nothing to scan: the loop body is where the
        // poll lives, and an empty grid never enters it.
        amrvis::VolumeGrid empty;
        empty.dims = {1, 1, 1};
        empty.region = unitBox();
        threw = false;
        try {
            (void)amrvis::volumeGridRange(empty, false, stop.get_token());
        } catch (const amrvis::ReadCancelled&) {
            threw = true;
        }
        require(threw, "a cancelled scan of an empty grid did not throw");
    }

    // --- an isosurface seen face-on composites once, whatever the rate ------
    // f = z on the unit grid, iso-value 0.5: the plane z = 0.5, which the XY
    // view (rays along -z) meets head-on, so the normal is the view direction
    // and every headlight term is at its maximum. The volume is off. One hit
    // per ray, so the alpha is the surface's opacity exactly -- not corrected
    // per step like a volume entry -- at every samples-per-voxel.
    {
        const auto grid = rampGrid(8, 2);
        const auto opaque = amrvis::raycastVolume(
            amrvis::RaycastGrids{nullptr, &grid},
            isoOnlySettings(amrvis::orthoPresetXY, 64, isosurface(0.5, 0xCC0000U, 1.0F)));
        const auto centre = pixelAt(opaque, 32, 32);
        require(alphaOf(centre) == 255, "a face-on opaque isosurface is not opaque");
        require(redOf(centre) == byteOf(shaded(0.8, 1.0))
                && greenOf(centre) == byteOf(shaded(0.0, 1.0))
                && blueOf(centre) == byteOf(shaded(0.0, 1.0)),
            "a face-on isosurface is not lit by the headlight terms");
        require(redOf(centre) == 235 && greenOf(centre) == 51,
            "the headlight constants moved the expected colour");
        require(pixelAt(opaque, 0, 0) == 0U && pixelAt(opaque, 63, 63) == 0U,
            "an isosurface lit pixels outside the domain");
        // The same footprint as an opaque volume of the same grid: a plane
        // spanning the region is crossed by every ray through it.
        const auto slab = amrvis::raycastVolume(uniformGrid(8, 1.0F),
            settingsFor(amrvis::orthoPresetXY, 64, twoEntries(0xFFFFFFU, 1.0F)));
        for (std::size_t index = 0; index < slab.pixels.size(); ++index) {
            require((slab.pixels[index] != 0U) == (opaque.pixels[index] != 0U),
                "the isosurface footprint differs from the volume's");
        }
        for (const int samplesPerVoxel : {1, 2, 4, 8}) {
            const auto frame = amrvis::raycastVolume(
                amrvis::RaycastGrids{nullptr, &grid},
                isoOnlySettings(amrvis::orthoPresetXY, 64,
                    isosurface(0.5, 0xCC0000U, 0.3F), samplesPerVoxel));
            require(alphaOf(pixelAt(frame, 32, 32)) == 77,
                "a translucent isosurface's alpha depends on the sampling rate");
        }
        require(opaque.metrics.gridDims == grid.dims
                && opaque.metrics.coveredVoxels == grid.coveredVoxels,
            "an isosurface-only frame does not describe its grid");
        // The default white surface, half opaque: the headlight terms sum past
        // 1 head-on, and a premultiplied channel must not exceed its alpha.
        const auto white = amrvis::raycastVolume(
            amrvis::RaycastGrids{nullptr, &grid},
            isoOnlySettings(amrvis::orthoPresetXY, 64, isosurface(0.5, 0xFFFFFFU, 0.5F)));
        const auto half = pixelAt(white, 32, 32);
        require(alphaOf(half) == 128 && redOf(half) == 128 && greenOf(half) == 128
                && blueOf(half) == 128,
            "a half-opaque white surface wrote a channel above its alpha");
        // The same plane in a field near the top of the double range: f =
        // (4z - 2) * 1e308, whose values stay finite but whose slope is 4e308
        // per unit length. The gradient's per-voxel difference times the
        // reciprocal pitch overflows, halved or not; only normalising the
        // direction first keeps the surface.
        auto huge = grid;
        for (auto& value : huge.values) {
            value = (4.0 * value - 2.0) * 1.0e308;
        }
        const auto vast = amrvis::raycastVolume(
            amrvis::RaycastGrids{nullptr, &huge},
            isoOnlySettings(amrvis::orthoPresetXY, 64, isosurface(0.0, 0xCC0000U, 1.0F)));
        require(vast.pixels == opaque.pixels,
            "a field near the double range's top lost its isosurface");
        // And a step from -1.6e308 to +1.6e308 across the same plane: the two
        // sides of a central difference are then further apart than a double
        // can hold, so the difference has to be taken in halves.
        auto step = grid;
        for (int k = 0; k < 8; ++k) {
            for (int j = 0; j < 8; ++j) {
                for (int i = 0; i < 8; ++i) {
                    step.values[voxel(step, i, j, k)] = k < 4 ? -1.6e308 : 1.6e308;
                }
            }
        }
        const auto stepped = amrvis::raycastVolume(
            amrvis::RaycastGrids{nullptr, &step},
            isoOnlySettings(amrvis::orthoPresetXY, 64, isosurface(0.0, 0xCC0000U, 1.0F)));
        require(stepped.pixels == opaque.pixels,
            "a step spanning the double range lost its isosurface");
        // The same step with the iso-value near one end. Taken whole between
        // the two samples, both differences of the secant would overflow and
        // their NaN quotient would put the hit at voxel zero; the bisections
        // narrow the bracket fourfold first, so today the quotient stays
        // finite and this pins only that the surface is drawn. The halves and
        // the midpoint fallback in the march guard the day the refinement is
        // shortened, which no face-on picture can tell apart.
        const auto lopsided = amrvis::raycastVolume(
            amrvis::RaycastGrids{nullptr, &step},
            isoOnlySettings(amrvis::orthoPresetXY, 64, isosurface(1.5e308, 0xCC0000U, 1.0F)));
        require(lopsided.pixels == opaque.pixels,
            "a crossing with an extreme iso-value lost its isosurface");
    }

    // --- a plane seen edge-on is invisible -----------------------------------
    // f = x under the same view: constant along every ray, so no ray crosses
    // it and nothing is drawn. An infinitely thin surface has no edge to show.
    {
        const auto grid = rampGrid(8, 0);
        const auto frame = amrvis::raycastVolume(
            amrvis::RaycastGrids{nullptr, &grid},
            isoOnlySettings(amrvis::orthoPresetXY, 64, isosurface(0.5, 0xCC0000U, 1.0F)));
        require(litCount(frame) == 0, "an edge-on plane lit a pixel");
    }

    // --- a closed surface is hit going in and coming out ---------------------
    // A sphere of radius 0.3 about the centre, at half opacity: the centre ray
    // (odd viewport, so a pixel centre sits on the domain centre) crosses the
    // front and the back, both face-on by symmetry, and composites to exactly
    // 1 - 0.5^2. Pixels outside the projected disc stay empty: the domain
    // spans 20.5 pixels, so the disc is 6.15 pixels in radius.
    {
        const auto grid = sphereGrid(32, {0.5, 0.5, 0.5});
        const auto frame = amrvis::raycastVolume(
            amrvis::RaycastGrids{nullptr, &grid},
            isoOnlySettings(amrvis::orthoPresetXY, 65, isosurface(0.3, 0x00CC00U, 0.5F)));
        const auto centre = pixelAt(frame, 32, 32);
        require(alphaOf(centre) == 191, "two half-opaque hits did not composite to 0.75");
        require(greenOf(centre) == byteOf(0.75 * shaded(0.8, 1.0))
                && redOf(centre) == byteOf(0.75 * shaded(0.0, 1.0)),
            "the sphere's centre is not lit face-on twice");
        require(alphaOf(pixelAt(frame, 30, 32)) == 191,
            "a pixel inside the disc was not hit twice");
        require(pixelAt(frame, 40, 32) == 0U && pixelAt(frame, 32, 40) == 0U,
            "a pixel outside the disc but inside the domain was lit");
        // Off-centre the surface is oblique and darker, never brighter.
        require(greenOf(pixelAt(frame, 36, 32)) < greenOf(centre)
                && alphaOf(pixelAt(frame, 36, 32)) == 191,
            "an oblique hit is not darker than a face-on one");
    }

    // --- the surface composites in depth order with the volume ---------------
    // A blue slab over z in [0.25, 0.75] (four voxels at 0.3 each: alpha
    // 1 - 0.7^4) and an opaque red plane. The XY view marches from z = 1 down,
    // so a plane at z = 0.875 is met first and hides the slab -- the picture
    // is the isosurface alone -- and one at z = 0.125 is seen through it.
    {
        auto slab = uniformGrid(8, 0.0F);
        for (int k = 2; k <= 5; ++k) {
            for (int j = 0; j < 8; ++j) {
                for (int i = 0; i < 8; ++i) {
                    slab.values[voxel(slab, i, j, k)] = 1.0;
                }
            }
        }
        const auto ramp = rampGrid(8, 2);
        auto both = settingsFor(amrvis::orthoPresetXY, 64, twoEntries(0x0000FFU, 0.3F));
        both.isosurface = isosurface(0.875, 0xCC0000U, 1.0F);
        const auto nearer = amrvis::raycastVolume(
            amrvis::RaycastGrids{&slab, &ramp}, both);
        const auto alone = amrvis::raycastVolume(
            amrvis::RaycastGrids{nullptr, &ramp},
            isoOnlySettings(amrvis::orthoPresetXY, 64, *both.isosurface));
        require(nearer.pixels == alone.pixels,
            "a volume behind an opaque isosurface showed through");
        both.isosurface->value = 0.125;
        const auto behind = amrvis::raycastVolume(
            amrvis::RaycastGrids{&slab, &ramp}, both);
        const auto centre = pixelAt(behind, 32, 32);
        const auto slabAlpha = 1.0 - std::pow(0.7, 4.0);
        const auto tolerance = 1.5 / 255.0;
        require(alphaOf(centre) == 255, "a slab over an opaque surface is not opaque");
        require(std::abs(blueOf(centre) / 255.0 - (slabAlpha + (1.0 - slabAlpha) * shaded(0.0, 1.0)))
                    <= tolerance
                && std::abs(redOf(centre) / 255.0 - (1.0 - slabAlpha) * shaded(0.8, 1.0))
                    <= tolerance
                && std::abs(greenOf(centre) / 255.0 - (1.0 - slabAlpha) * shaded(0.0, 1.0))
                    <= tolerance,
            "a surface behind the slab is not blended under it");
        // The volume alone, for contrast: less than opaque, and pure blue.
        const auto volumeOnly = amrvis::raycastVolume(slab,
            settingsFor(amrvis::orthoPresetXY, 64, twoEntries(0x0000FFU, 0.3F)));
        require(alphaOf(pixelAt(volumeOnly, 32, 32)) < 255
                && redOf(pixelAt(volumeOnly, 32, 32)) == 0,
            "the slab alone is not the expected contrast");
    }

    // --- no surface is invented at a coverage boundary -----------------------
    // The plane's crossing cell has an uncovered corner: no hit anywhere. And
    // an uncovered half: the covered half is lit, the uncovered half is not,
    // with the one-voxel band between free either way.
    {
        constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
        auto gap = rampGrid(8, 2);
        for (int j = 0; j < 8; ++j) {
            for (int i = 0; i < 8; ++i) {
                gap.values[voxel(gap, i, j, 3)] = nan;
            }
        }
        const auto settings = isoOnlySettings(
            amrvis::orthoPresetXY, 64, isosurface(0.5, 0xCC0000U, 1.0F));
        require(litCount(amrvis::raycastVolume(amrvis::RaycastGrids{nullptr, &gap}, settings))
                == 0,
            "a crossing through an uncovered slab lit a pixel");
        auto half = rampGrid(8, 2);
        for (int k = 0; k < 8; ++k) {
            for (int j = 0; j < 8; ++j) {
                for (int i = 0; i < 4; ++i) {
                    half.values[voxel(half, i, j, k)] = nan;
                }
            }
        }
        const auto frame = amrvis::raycastVolume(
            amrvis::RaycastGrids{nullptr, &half}, settings);
        // The unit domain spans 20 pixels, columns 22 through 41: x = 0.275
        // is column 27 and x = 0.775 is column 37.
        require(pixelAt(frame, 27, 32) == 0U, "the uncovered half was lit");
        require(alphaOf(pixelAt(frame, 37, 32)) == 255, "the covered half was not lit");
        // The boundary column itself, x = 0.5, reads a cell with uncovered
        // corners and is left alone rather than given an invented surface.
        require(pixelAt(frame, 32, 32) == 0U, "the coverage boundary was lit");
    }

    // --- a field that never crosses the iso-value draws nothing --------------
    {
        const auto flat = uniformGrid(8, 0.5F);
        for (const double value : {0.5, 2.0, -1.0}) {
            const auto frame = amrvis::raycastVolume(
                amrvis::RaycastGrids{nullptr, &flat},
                isoOnlySettings(amrvis::orthoPresetXY, 32, isosurface(value, 0xCC0000U, 1.0F)));
            require(litCount(frame) == 0, "a flat field lit a pixel");
        }
        // A zero-opacity surface is accepted and changes nothing.
        const auto ramp = rampGrid(8, 2);
        const auto frame = amrvis::raycastVolume(
            amrvis::RaycastGrids{nullptr, &ramp},
            isoOnlySettings(amrvis::orthoPresetXY, 32, isosurface(0.5, 0xCC0000U, 0.0F)));
        require(litCount(frame) == 0, "a zero-opacity isosurface lit a pixel");
    }

    // --- an isosurface does not break thread invariance ----------------------
    // The sphere over the pseudo-random volume of the thread test above, seen
    // obliquely; and one grid passed as both volume and isosurface renders.
    {
        auto grid = uniformGrid(16, 0.0F);
        for (std::size_t index = 0; index < grid.values.size(); ++index) {
            grid.values[index] = static_cast<float>((index * 7919U) % 101U) / 100.0F;
        }
        const auto sphere = sphereGrid(16, {0.5, 0.5, 0.5});
        amrvis::VolumeTransferFunction transfer;
        for (int entry = 0; entry < 16; ++entry) {
            transfer.colors.push_back(static_cast<std::uint32_t>(entry * 16)
                | (static_cast<std::uint32_t>(255 - entry * 16) << 16U));
            transfer.opacities.push_back(static_cast<float>(entry) / 40.0F);
        }
        auto single = settingsFor(amrvis::OrthoCamera{0.7, -0.4, 1.3}, 97, transfer, 3);
        single.sampling = amrvis::SamplingPolicy::Linear;
        single.isosurface = isosurface(0.3, 0x40C0FFU, 0.6F);
        auto many = single;
        many.threadCount = 7;
        auto oversubscribed = single;
        oversubscribed.threadCount = 500;
        const amrvis::RaycastGrids grids{&grid, &sphere};
        const auto one = amrvis::raycastVolume(grids, single);
        const auto seven = amrvis::raycastVolume(grids, many);
        const auto clamped = amrvis::raycastVolume(grids, oversubscribed);
        require(one.pixels == seven.pixels && one.pixels == clamped.pixels,
            "the thread split changed an isosurface picture");
        require(litCount(one) > 0, "the oblique isosurface render lit nothing");
        const auto shared = amrvis::raycastVolume(
            amrvis::RaycastGrids{&sphere, &sphere}, single);
        require(litCount(shared) > 0, "one grid as both volume and isosurface lit nothing");
    }

    // --- isosurface refusals -------------------------------------------------
    {
        const auto grid = uniformGrid(4, 1.0F);
        const auto ramp = rampGrid(4, 2);
        const auto rejects = [](const amrvis::RaycastGrids& grids,
                                 const amrvis::RaycastSettings& settings) {
            try {
                (void)amrvis::raycastVolume(grids, settings);
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };
        const auto plain = settingsFor(amrvis::orthoPresetXY, 16, twoEntries(0xFFU, 1.0F));
        const auto isoOnly = isoOnlySettings(
            amrvis::orthoPresetXY, 16, isosurface(0.5, 0xCC0000U, 1.0F));
        require(rejects({nullptr, nullptr}, plain), "no grids at all was accepted");
        require(rejects({nullptr, nullptr}, isoOnly), "no grids at all was accepted");
        require(rejects({&grid, nullptr}, isoOnly),
            "isosurface settings without an isosurface grid were accepted");
        require(rejects({&grid, &ramp}, plain),
            "an isosurface grid without settings was accepted");
        require(rejects({nullptr, &ramp}, plain),
            "a shown volume without a volume grid was accepted");
        auto withIso = plain;
        withIso.isosurface = isosurface(0.5, 0xCC0000U, 1.0F);
        require(rejects({&grid, &ramp}, plain) && !rejects({&grid, &ramp}, withIso),
            "a well-formed two-grid render was refused");
        const auto smaller = rampGrid(2, 2);
        require(rejects({&grid, &smaller}, withIso), "mismatched grid dims were accepted");
        auto moved = ramp;
        moved.region.upper[0] = 2.0;
        require(rejects({&grid, &moved}, withIso), "mismatched grid regions were accepted");
        auto bad = withIso;
        bad.isosurface->value = std::numeric_limits<double>::quiet_NaN();
        require(rejects({&grid, &ramp}, bad), "a NaN iso-value was accepted");
        bad = withIso;
        bad.isosurface->opacity = 1.5F;
        require(rejects({&grid, &ramp}, bad), "an isosurface opacity above one was accepted");
        bad.isosurface->opacity = -0.1F;
        require(rejects({&grid, &ramp}, bad), "a negative isosurface opacity was accepted");
        bad.isosurface->opacity = std::numeric_limits<float>::quiet_NaN();
        require(rejects({&grid, &ramp}, bad), "a NaN isosurface opacity was accepted");
        bad = withIso;
        bad.isosurface->color = 0x01000000U;
        require(rejects({&grid, &ramp}, bad), "an isosurface colour with a high byte was accepted");
        auto malformed = ramp;
        malformed.values.pop_back();
        require(rejects({&grid, &malformed}, withIso),
            "a malformed isosurface grid was accepted");
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
        // A finite range may span more than DBL_MAX; mapping scales it safely.
        bad.range = {-std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max(), false};
        require(!rejects(bad), "a finite range with an overflowing span was refused");
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

    // --- a stop is answered part-way through one long ray -----------------
    // An elongated grid within the voxel budget puts millions of samples on a
    // single ray, so one pixel is a long stretch of work. Polling between
    // pixels does not reach into it: the stop has to be seen by the sample
    // loop itself.
    {
        amrvis::VolumeGrid grid;
        grid.dims = {1, 1, 2000000};
        grid.region = unitBox();
        grid.values.assign(2000000, 1.0F);
        grid.coveredVoxels = grid.values.size();
        auto settings = settingsFor(
            amrvis::orthoPresetXY, 1, twoEntries(0x0000FFU, 1.0e-7F), 8);
        const auto run = [&grid, &settings](amrvis::StopToken token) {
            const auto started = std::chrono::steady_clock::now();
            bool cancelled = false;
            try {
                (void)amrvis::raycastVolume(grid, settings, token);
            } catch (const amrvis::ReadCancelled&) {
                cancelled = true;
            }
            return std::pair{cancelled,
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count()};
        };
        const auto baseline = run({});
        require(!baseline.first, "an uncancelled render reported cancellation");
        // Only assert when the ray is long enough for "part-way through" to
        // mean something.
        if (baseline.second > 30.0) {
            // Whether the stop was *answered*, not how fast: a wall-clock
            // ratio is a flake on a loaded runner, where sleep_for overshoots
            // by tens of milliseconds. The boolean discriminates on its own --
            // this frame is one pixel, so the pixel-level poll runs once
            // before the ray starts, and a stop raised after that is seen
            // only if the sample loop looks. Retried because the one way a
            // sound implementation fails here is the asker losing its race
            // with a render that already finished.
            bool answered = false;
            for (int attempt = 0; attempt < 5 && !answered; ++attempt) {
                amrvis::StopSource stop;
                std::thread asker([&stop] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    stop.request_stop();
                });
                answered = run(stop.get_token()).first;
                asker.join();
            }
            require(answered,
                "a stop raised during one long ray was not answered");
        }
    }

    // --- a domain near the top of the double range -------------------------
    // Its bounds are finite and its span is ordinary, but the midpoint of the
    // two overflows, so a centre taken as half their sum is infinite and
    // every ray origin follows.
    {
        const auto low = 1.0e308;
        const auto span = 1.0e307;
        amrvis::VolumeGrid grid;
        grid.dims = {4, 4, 4};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            grid.region.lower[axis] = low;
            grid.region.upper[axis] = low + span;
        }
        grid.values.assign(64, 1.0F);
        grid.coveredVoxels = 64;
        auto settings = settingsFor(
            amrvis::orthoPresetXY, 33, twoEntries(0x00FF00U, 1.0F));
        settings.domain = grid.region;
        const auto frame = amrvis::raycastVolume(grid, settings);
        require(alphaOf(pixelAt(frame, 16, 16)) == 255
                && greenOf(pixelAt(frame, 16, 16)) == 255,
            "a domain near the top of the range rendered its centre empty");
        // The smallest zoom scales the same origins past what a double holds;
        // that is refused rather than rendered from infinities.
        settings.camera.zoom = amrvis::minVolumeZoom;
        bool threw = false;
        try {
            (void)amrvis::raycastVolume(grid, settings);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "a ray field of infinities was accepted");
    }

    // --- a region too small beside its domain is refused, not rendered empty
    // A unit region inside a 1e16-wide domain: the entry and exit parameters
    // round to the same double, so the slab test misses and every pixel comes
    // back transparent. That is indistinguishable from an empty dataset, so
    // it is a refusal. A merely deep zoom is many orders short of this and
    // still renders.
    {
        amrvis::VolumeGrid grid;
        grid.dims = {1, 1, 1};
        grid.region = unitBox();
        grid.values.assign(1, 1.0F);
        grid.coveredVoxels = 1;
        auto settings = settingsFor(
            amrvis::orthoPresetXY, 33, twoEntries(0x00FF00U, 1.0F));
        settings.domain.upper[0] = 1.0e16;
        bool threw = false;
        try {
            (void)amrvis::raycastVolume(grid, settings);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "a region the ray cannot resolve rendered empty instead");
        // The guard is far away from ordinary geometry: a region a tenth of
        // its domain still renders. (It has to be a *visible* fraction to
        // light anything at all -- the camera normalises to the domain, so a
        // region a millionth of it projects to well under a pixel and draws
        // nothing whatever the arithmetic does. That is the projection
        // working as documented, not the guard.)
        settings.domain.upper[0] = 10.0;
        settings.outputSize = {128, 128};
        const auto frame = amrvis::raycastVolume(grid, settings);
        std::size_t lit = 0;
        for (const auto pixel : frame.pixels) {
            lit += pixel != 0U;
        }
        require(lit > 0, "a region a tenth of its domain rendered nothing");
    }

    return 0;
}
