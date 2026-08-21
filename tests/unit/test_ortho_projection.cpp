// The shared orthographic camera: the projection the iso view draws with and
// the ray caster renders with. Pins the numbers the iso view produced before
// the math moved here (so the quadrant cannot drift), the preset views, and
// that pixelRay inverts projectPoint.

#include <amrexplorer/core/OrthoProjection.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool near(double actual, double expected, double tolerance = 1e-6)
{
    return std::abs(actual - expected) <= tolerance;
}

amrvis::Real3 point(double x, double y, double z)
{
    amrvis::Real3 result;
    result[0] = x;
    result[1] = y;
    result[2] = z;
    return result;
}

} // namespace

int main()
{
    constexpr double pi = 3.14159265358979323846;
    // An anisotropic domain, so the normalisation by the largest extent is
    // exercised: [0,2] x [0,1] x [0,1], centre (1, 0.5, 0.5), extent 2.
    const amrvis::RealBox domain{point(0.0, 0.0, 0.0), point(2.0, 1.0, 1.0)};

    // The viewport frame is IsoWidget's: centre of the widget, scale = the
    // shorter half-side minus the margin, never below one pixel.
    const auto frame = amrvis::viewportFrame(400, 300);
    require(near(frame.centerX, 200.0) && near(frame.centerY, 150.0)
            && near(frame.scale, 138.0),
        "the viewport frame is not the iso view's");
    require(near(amrvis::viewportFrame(10, 10).scale, 1.0),
        "a tiny viewport did not clamp the scale to one pixel");

    // Pinned from the iso view's projection formula: the upper corner under
    // azimuth 30 deg, elevation 30 deg, zoom 1.
    const amrvis::OrthoCamera oblique{30.0 * pi / 180.0, 30.0 * pi / 180.0, 1.0};
    const auto corner = amrvis::projectPoint(
        oblique, frame, domain, point(2.0, 1.0, 1.0));
    require(near(corner.x, 242.505752861) && near(corner.y, 111.497123569)
            && near(corner.depth, 0.449759526),
        "the oblique projection does not match the iso view's numbers");
    // Zoom scales about the viewport centre.
    const amrvis::OrthoCamera zoomed{oblique.azimuth, oblique.elevation, 2.0};
    const auto zoomedCorner = amrvis::projectPoint(
        zoomed, frame, domain, point(2.0, 1.0, 1.0));
    require(near(zoomedCorner.x - 200.0, 2.0 * (corner.x - 200.0))
            && near(zoomedCorner.y - 150.0, 2.0 * (corner.y - 150.0))
            && near(zoomedCorner.depth, corner.depth),
        "zoom did not scale about the viewport centre");
    // The domain centre projects to the viewport centre at zero depth.
    const auto centre = amrvis::projectPoint(
        oblique, frame, domain, point(1.0, 0.5, 0.5));
    require(near(centre.x, 200.0) && near(centre.y, 150.0)
            && near(centre.depth, 0.0),
        "the domain centre did not project to the viewport centre");

    // Presets. XY: screen right = +x, up = +y, depth = +z (the eye is above).
    {
        const auto p = amrvis::projectPoint(
            amrvis::orthoPresetXY, frame, domain, point(2.0, 1.0, 1.0));
        require(near(p.x, 200.0 + 138.0 * 0.5) && near(p.y, 150.0 - 138.0 * 0.25)
                && near(p.depth, 0.25),
            "the XY preset is not a top-down view");
    }
    // XZ: right = +x, up = +z, the eye on the -y side (depth = -y).
    {
        const auto p = amrvis::projectPoint(
            amrvis::orthoPresetXZ, frame, domain, point(2.0, 1.0, 1.0));
        require(near(p.x, 200.0 + 138.0 * 0.5) && near(p.y, 150.0 - 138.0 * 0.25)
                && near(p.depth, -0.25),
            "the XZ preset is not a front view");
    }
    // YZ: right = +y, up = +z, the eye on the +x side (depth = +x).
    {
        const auto p = amrvis::projectPoint(
            amrvis::orthoPresetYZ, frame, domain, point(2.0, 1.0, 1.0));
        require(near(p.x, 200.0 + 138.0 * 0.25) && near(p.y, 150.0 - 138.0 * 0.25)
                && near(p.depth, 0.5),
            "the YZ preset is not a side view");
    }

    // pixelRay inverts projectPoint: the ray through a point's projection
    // passes through the point, starts on the viewer's side outside the
    // domain, and points away from the viewer (toward decreasing depth).
    for (const auto& camera : {oblique, zoomed, amrvis::orthoPresetXY,
             amrvis::orthoPresetXZ, amrvis::orthoPresetYZ,
             amrvis::OrthoCamera{-2.3, 1.1, 0.7}}) {
        for (const auto& target : {point(2.0, 1.0, 1.0), point(0.0, 0.0, 0.0),
                 point(1.3, 0.2, 0.9), point(1.0, 0.5, 0.5)}) {
            const auto projected = amrvis::projectPoint(
                camera, frame, domain, target);
            const auto ray = amrvis::pixelRay(
                camera, frame, domain, projected.x, projected.y);
            // Unit direction.
            const auto length = std::sqrt(ray.direction[0] * ray.direction[0]
                + ray.direction[1] * ray.direction[1]
                + ray.direction[2] * ray.direction[2]);
            require(near(length, 1.0, 1e-12), "the ray direction is not unit");
            // The point lies on the ray at t = distance from the origin, and
            // ahead of the origin.
            double t = 0.0;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                t += (target[axis] - ray.origin[axis]) * ray.direction[axis];
            }
            require(t > 0.0, "the ray origin is not on the viewer's side");
            double miss = 0.0;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const auto onRay = ray.origin[axis] + t * ray.direction[axis];
                miss = std::max(miss, std::abs(onRay - target[axis]));
            }
            require(miss <= 1e-9, "the pixel ray does not pass through the point");
            // Marching along the ray lowers the depth.
            amrvis::Real3 ahead;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                ahead[axis] = target[axis] + 0.1 * ray.direction[axis];
            }
            const auto aheadDepth = amrvis::projectPoint(
                camera, frame, domain, ahead).depth;
            require(aheadDepth < projected.depth,
                "the ray direction does not point away from the viewer");
            // The origin is outside the domain.
            bool outside = false;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                outside = outside || ray.origin[axis] < domain.lower[axis]
                    || ray.origin[axis] > domain.upper[axis];
            }
            require(outside, "the ray origin starts inside the domain");
        }
    }
    // Domains at the extremes of the double range. The centre of a box is
    // taken as half of each bound rather than half of their sum or the lower
    // bound plus half the span: the first overflows for a box spanning most
    // of the range, the second for a box whose own span overflows, and either
    // way the projection comes back NaN and reaches QPainter.
    {
        constexpr auto huge = std::numeric_limits<double>::max();
        const amrvis::RealBox spanning{point(-huge, -1.0, -1.0),
            point(huge, 1.0, 1.0)};
        const amrvis::RealBox offset{point(1.0e308, 0.0, 0.0),
            point(1.1e308, 1.0, 1.0)};
        for (const auto& box : {spanning, offset}) {
            require(box.valid(3), "the extreme domain is not a valid box");
            const auto boxCentre = box.center();
            for (std::size_t axis = 0; axis < 3; ++axis) {
                require(std::isfinite(boxCentre[axis]),
                    "the centre of an extreme box is not finite");
            }
            const auto p = amrvis::projectPoint(
                oblique, frame, box, point(0.0, 0.0, 0.0));
            require(std::isfinite(p.x) && std::isfinite(p.y)
                    && std::isfinite(p.depth),
                "an extreme domain projected to a non-finite point");
        }
    }

    // The preset rays are axis-aligned with the expected sign.
    {
        const auto xy = amrvis::pixelRay(
            amrvis::orthoPresetXY, frame, domain, 200.0, 150.0);
        const auto xz = amrvis::pixelRay(
            amrvis::orthoPresetXZ, frame, domain, 200.0, 150.0);
        const auto yz = amrvis::pixelRay(
            amrvis::orthoPresetYZ, frame, domain, 200.0, 150.0);
        require(near(xy.direction[2], -1.0, 1e-12)
                && near(xz.direction[1], 1.0, 1e-12)
                && near(yz.direction[0], -1.0, 1e-12),
            "the preset rays are not axis-aligned as labelled");
    }
    return 0;
}
