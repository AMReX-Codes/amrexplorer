#include <amrexplorer/core/CoordinateSystem.hpp>
#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/render2d/ImageBuffer.hpp>
#include <amrexplorer/render2d/SphericalWarp.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool approx(double a, double b, double tol = 1e-6)
{
    return std::abs(a - b) <= tol;
}

amrvis::RealBox logicalBox(double r0, double r1, double t0, double t1)
{
    amrvis::RealBox box;
    box.lower[0] = r0;
    box.upper[0] = r1;
    box.lower[1] = t0;
    box.upper[1] = t1;
    return box;
}

} // namespace

int main()
{
    using namespace amrvis;

    // Forward/inverse round-trip over a spread of (r, theta).
    for (double r : {0.5, 1.5, 2.5, 10.0}) {
        for (double theta : {0.0, 0.1, 0.25, 0.7854, 1.2, 2.5}) {
            const auto display = sphericalToDisplay(r, theta);
            const auto back = displayToSpherical(display[0], display[1]);
            require(approx(back[0], r), "round-trip r");
            require(approx(back[1], theta), "round-trip theta");
        }
    }

    // Known display mapping: theta = 0 lies on the +Z axis (R = 0, Z = r).
    {
        const auto onAxis = sphericalToDisplay(2.0, 0.0);
        require(approx(onAxis[0], 0.0) && approx(onAxis[1], 2.0), "theta=0 on Z axis");
    }

    // Bounding box of the reference sedov sector: r in [1.5, 2.5], theta in
    // [0, 0.25]. R in [0, 2.5*sin(0.25)], Z in [1.5*cos(0.25), 2.5].
    {
        const auto bounds = sphericalDisplayBounds(logicalBox(1.5, 2.5, 0.0, 0.25));
        require(approx(bounds.lower[0], 0.0), "sedov R lower");
        require(approx(bounds.upper[0], 2.5 * std::sin(0.25)), "sedov R upper");
        require(approx(bounds.lower[1], 1.5 * std::cos(0.25)), "sedov Z lower");
        require(approx(bounds.upper[1], 2.5), "sedov Z upper");
    }

    // A sector straddling theta = pi/2: R must reach the outer radius there,
    // not just at the corners.
    {
        const auto bounds = sphericalDisplayBounds(logicalBox(1.0, 2.0, 0.0, 2.0));
        require(approx(bounds.upper[0], 2.0), "half-pi sector R upper = r1");
        require(approx(bounds.upper[1], 2.0), "half-pi sector Z upper");
        require(approx(bounds.lower[1], 2.0 * std::cos(2.0)), "half-pi sector Z lower");
    }

    // isSpherical2D: only coord 2 + dimension 2 + real geometry qualifies.
    {
        DatasetMetadata md;
        md.coordinateSystem = 2;
        md.dimension = 2;
        md.hasPhysicalGeometry = true;
        require(isSpherical2D(md), "coord2 dim2 -> spherical");
        md.dimension = 3;
        require(!isSpherical2D(md), "coord2 dim3 -> not spherical");
        md.dimension = 2;
        md.coordinateSystem = 0;
        require(!isSpherical2D(md), "cartesian -> not spherical");
        md.coordinateSystem = 2;
        md.hasPhysicalGeometry = false;
        require(!isSpherical2D(md), "no geometry -> not spherical");
    }

    // warpSpherical: a uniform opaque source produces a raster whose sector
    // interior keeps the source color and whose exterior is transparent.
    {
        constexpr std::uint32_t kColor = 0xFF112233U;  // opaque
        ImageBuffer src;
        src.width = 8;
        src.height = 8;
        src.strideBytes = src.width * static_cast<int>(sizeof(std::uint32_t));
        src.rgba.assign(
            static_cast<std::size_t>(src.width) * static_cast<std::size_t>(src.height),
            kColor);

        const auto region = logicalBox(1.0, 2.0, 0.0, 0.4);
        const auto warped = warpSpherical(src, region, 128, 4);
        require(warped.image.width > 0 && warped.image.height > 0, "warp dims positive");
        require(approx(warped.displayRegion.upper[1],
                    sphericalDisplayBounds(region).upper[1]),
            "warp display region");

        std::size_t opaque = 0;
        std::size_t transparent = 0;
        for (const auto pixel : warped.image.rgba) {
            if (pixel == 0U) {
                ++transparent;
            } else {
                require(pixel == kColor, "warp keeps source color inside sector");
                ++opaque;
            }
        }
        require(opaque > 0, "warp has sector interior");
        // The sector never fills its axis-aligned bounding box, so some output
        // pixels must fall outside it and stay transparent.
        require(transparent > 0, "warp has transparent exterior");
    }

    std::cout << "spherical_coords OK\n";
    return 0;
}
