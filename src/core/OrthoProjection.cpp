#include <amrexplorer/core/OrthoProjection.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace amrvis {
namespace {

struct Normalisation {
    Real3 center;
    double extent = 1.0;
};

// The domain's centre and its largest extent (1 for a degenerate box), the
// frame every camera quantity is measured in.
Normalisation normalisation(const RealBox& domain) noexcept
{
    Normalisation result;
    double extent = 0.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        result.center[axis] = 0.5 * (domain.lower[axis] + domain.upper[axis]);
        extent = std::max(extent, domain.upper[axis] - domain.lower[axis]);
    }
    result.extent = extent > 0.0 ? extent : 1.0;
    return result;
}

// View coordinates of a normalised point: x1 right, y2 up, depth toward the
// viewer -- Rz(azimuth) followed by the elevation rotation about x.
struct ViewPoint {
    double x1 = 0.0;
    double y2 = 0.0;
    double depth = 0.0;
};

// The camera's two rotations, resolved once. Every conversion below needs
// all four of these, so a caller applying the rotation more than once -- a
// ray field resolves four vectors -- takes the trig once, not per vector.
struct Rotation {
    double cosAz = 1.0;
    double sinAz = 0.0;
    double cosEl = 1.0;
    double sinEl = 0.0;
};

Rotation rotationOf(const OrthoCamera& camera) noexcept
{
    return {std::cos(camera.azimuth), std::sin(camera.azimuth),
        std::cos(camera.elevation), std::sin(camera.elevation)};
}

ViewPoint toView(const Rotation& rotation, const Real3& normalised) noexcept
{
    const auto x1 = normalised[0] * rotation.cosAz - normalised[1] * rotation.sinAz;
    const auto y1 = normalised[0] * rotation.sinAz + normalised[1] * rotation.cosAz;
    return {x1, y1 * rotation.cosEl - normalised[2] * rotation.sinEl,
        y1 * rotation.sinEl + normalised[2] * rotation.cosEl};
}

// The inverse rotation: a view-space vector back to normalised space.
Real3 toWorld(const Rotation& rotation, const ViewPoint& view) noexcept
{
    // Undo the elevation (transpose of the x rotation), then the azimuth.
    const auto y1 = view.y2 * rotation.cosEl + view.depth * rotation.sinEl;
    const auto z = -view.y2 * rotation.sinEl + view.depth * rotation.cosEl;
    Real3 world;
    world[0] = view.x1 * rotation.cosAz + y1 * rotation.sinAz;
    world[1] = -view.x1 * rotation.sinAz + y1 * rotation.cosAz;
    world[2] = z;
    return world;
}

} // namespace

ViewportFrame viewportFrame(int width, int height, double margin) noexcept
{
    ViewportFrame frame;
    frame.centerX = static_cast<double>(width) / 2.0;
    frame.centerY = static_cast<double>(height) / 2.0;
    frame.scale = std::max(
        std::min(frame.centerX, frame.centerY) - margin, 1.0);
    return frame;
}

ProjectedPoint projectPoint(const OrthoCamera& camera,
    const ViewportFrame& frame, const RealBox& domain,
    const Real3& point) noexcept
{
    const auto norm = normalisation(domain);
    Real3 normalised;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        normalised[axis] = (point[axis] - norm.center[axis]) / norm.extent;
    }
    const auto view = toView(rotationOf(camera), normalised);
    const auto pixels = frame.scale * camera.zoom;
    return {frame.centerX + pixels * view.x1, frame.centerY - pixels * view.y2,
        view.depth};
}

RayField rayField(const OrthoCamera& camera, const ViewportFrame& frame,
    const RealBox& domain) noexcept
{
    const auto norm = normalisation(domain);
    const auto pixels = frame.scale * camera.zoom;
    // The normalised domain lies within [-0.5, 0.5]^3, so a depth of 2 is
    // outside it in every orientation; the rays start there and run toward
    // negative depth.
    constexpr double startDepth = 2.0;
    // toWorld is a rotation, linear in its argument, so the origin is affine
    // in the pixel position: evaluate it at the viewport's (0, 0) and take
    // the two per-pixel steps from the view-space basis vectors. A pixel's
    // view x1 is (pixelX - centerX) / pixels and its y2 is (centerY - pixelY)
    // / pixels, hence the sign on the y step.
    const auto rotation = rotationOf(camera);
    const auto atZero = toWorld(rotation, ViewPoint{-frame.centerX / pixels,
        frame.centerY / pixels, startDepth});
    const auto alongX = toWorld(rotation, ViewPoint{1.0 / pixels, 0.0, 0.0});
    const auto alongY = toWorld(rotation, ViewPoint{0.0, -1.0 / pixels, 0.0});
    const auto direction = toWorld(rotation, ViewPoint{0.0, 0.0, -1.0});
    RayField field;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        field.origin[axis] = norm.center[axis] + atZero[axis] * norm.extent;
        field.perPixelX[axis] = alongX[axis] * norm.extent;
        field.perPixelY[axis] = alongY[axis] * norm.extent;
        field.direction[axis] = direction[axis];
    }
    return field;
}

Ray rayAt(const RayField& field, double pixelX, double pixelY) noexcept
{
    Ray ray;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        ray.origin[axis] = field.origin[axis]
            + pixelX * field.perPixelX[axis] + pixelY * field.perPixelY[axis];
        ray.direction[axis] = field.direction[axis];
    }
    return ray;
}

Ray pixelRay(const OrthoCamera& camera, const ViewportFrame& frame,
    const RealBox& domain, double pixelX, double pixelY) noexcept
{
    return rayAt(rayField(camera, frame, domain), pixelX, pixelY);
}

} // namespace amrvis
