#include <amrexplorer/core/OrthoProjection.hpp>

#include <algorithm>
#include <array>
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
    result.center = domain.center();
    double extent = 0.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        extent = std::max(extent, domain.upper[axis] - domain.lower[axis]);
    }
    result.extent = extent > 0.0 ? extent : 1.0;
    return result;
}

// View coordinates of a normalised point: x1 right, y2 up, depth toward the
// viewer -- the camera's rotation applied to the world point.
struct ViewPoint {
    double x1 = 0.0;
    double y2 = 0.0;
    double depth = 0.0;
};

// The camera's rotation as a matrix, resolved once from the unit quaternion:
// row 0 is the view's x1 axis in world terms, row 1 its y2, row 2 its depth.
// A caller applying the rotation more than once -- a ray field resolves four
// vectors -- builds it once, not per vector.
struct Rotation {
    std::array<Real3, 3> rows;
};

Rotation rotationOf(const OrthoCamera& camera) noexcept
{
    const auto q = normalized(camera.rotation);
    Rotation rotation;
    rotation.rows[0] = Real3{{1.0 - 2.0 * (q.y * q.y + q.z * q.z),
        2.0 * (q.x * q.y - q.w * q.z), 2.0 * (q.x * q.z + q.w * q.y)}};
    rotation.rows[1] = Real3{{2.0 * (q.x * q.y + q.w * q.z),
        1.0 - 2.0 * (q.x * q.x + q.z * q.z), 2.0 * (q.y * q.z - q.w * q.x)}};
    rotation.rows[2] = Real3{{2.0 * (q.x * q.z - q.w * q.y),
        2.0 * (q.y * q.z + q.w * q.x), 1.0 - 2.0 * (q.x * q.x + q.y * q.y)}};
    return rotation;
}

double dot(const Real3& a, const Real3& b) noexcept
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

ViewPoint toView(const Rotation& rotation, const Real3& normalised) noexcept
{
    return {dot(rotation.rows[0], normalised), dot(rotation.rows[1], normalised),
        dot(rotation.rows[2], normalised)};
}

// The inverse rotation (the transpose): a view-space vector back to
// normalised space.
Real3 toWorld(const Rotation& rotation, const ViewPoint& view) noexcept
{
    Real3 world;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        world[axis] = rotation.rows[0][axis] * view.x1
            + rotation.rows[1][axis] * view.y2 + rotation.rows[2][axis] * view.depth;
    }
    return world;
}

} // namespace

double norm(const Quaternion& q) noexcept
{
    return std::sqrt(normSquared(q));
}

Quaternion normalized(const Quaternion& q) noexcept
{
    const auto length = norm(q);
    if (!std::isfinite(length) || !(length > 0.0)) {
        return {};
    }
    return {q.w / length, q.x / length, q.y / length, q.z / length};
}

bool nearUnit(const Quaternion& q, double tolerance) noexcept
{
    const auto length = norm(q);
    return std::isfinite(length) && std::abs(length - 1.0) <= tolerance;
}

Quaternion axisAngle(const Real3& axis, double angle) noexcept
{
    const auto length = std::sqrt(dot(axis, axis));
    if (!std::isfinite(length) || !(length > 0.0)) {
        return {};
    }
    const auto s = std::sin(angle / 2.0) / length;
    return {std::cos(angle / 2.0), axis[0] * s, axis[1] * s, axis[2] * s};
}

Real3 rotate(const Quaternion& q, const Real3& vector) noexcept
{
    const auto rotation = rotationOf(OrthoCamera{q, 1.0});
    return {{dot(rotation.rows[0], vector), dot(rotation.rows[1], vector),
        dot(rotation.rows[2], vector)}};
}

OrthoCamera orthoCameraFromAngles(double azimuth, double elevation, double zoom) noexcept
{
    // The turn about z first, then the tilt about the turned x axis: the
    // tilt's quaternion on the left.
    return {aboutX(std::cos(elevation / 2.0), std::sin(elevation / 2.0))
            * aboutZ(std::cos(azimuth / 2.0), std::sin(azimuth / 2.0)),
        zoom};
}

OrthoAngles nearestOrthoAngles(const OrthoCamera& camera) noexcept
{
    // From the matrix of a roll-free rotation, Rx(el) * Rz(az): its top row
    // is (cos az, -sin az, 0) and its last column (0, -sin el, cos el).
    const auto q = normalized(camera.rotation);
    return {std::atan2(2.0 * (q.w * q.z - q.x * q.y), 1.0 - 2.0 * (q.y * q.y + q.z * q.z)),
        std::atan2(2.0 * (q.w * q.x - q.y * q.z), 1.0 - 2.0 * (q.x * q.x + q.y * q.y))};
}

std::optional<OrthoAngles> orthoAnglesOf(const OrthoCamera& camera) noexcept
{
    // Without roll, world z has no sideways component on screen: the matrix
    // entry that carries it is 2 (xz + wy).
    const auto q = normalized(camera.rotation);
    if (std::abs(q.x * q.z + q.w * q.y) > 1.0e-9) {
        return std::nullopt;
    }
    return nearestOrthoAngles(camera);
}

ViewportFrame viewportFrame(int width, int height, double margin) noexcept
{
    ViewportFrame frame;
    frame.centerX = static_cast<double>(width) / 2.0;
    frame.centerY = static_cast<double>(height) / 2.0;
    frame.scale = std::max(
        std::min(frame.centerX, frame.centerY) - margin, 1.0);
    return frame;
}

double backdropScale(const ViewportFrame& frame, double zoom,
    const ViewportFrame& rendered, double renderedZoom) noexcept
{
    const auto pixels = frame.scale * zoom;
    const auto renderedPixels = rendered.scale * renderedZoom;
    if (!(pixels > 0.0) || !(renderedPixels > 0.0)) {
        return 1.0;
    }
    return pixels / renderedPixels;
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

ViewDirection projectDirection(
    const OrthoCamera& camera, const Real3& direction) noexcept
{
    const auto view = toView(rotationOf(camera), direction);
    // Negated the way projectPoint negates it: screen y counts downward.
    return {view.x1, -view.y2};
}

} // namespace amrvis
