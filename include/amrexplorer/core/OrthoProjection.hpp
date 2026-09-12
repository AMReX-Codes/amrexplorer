#pragma once

#include <amrexplorer/core/Geometry.hpp>

#include <optional>

namespace amrvis {

// A rotation as a unit quaternion (w, x, y, z): Hamilton product, a vector
// rotated by q v q*, the identity by default. q and -q are one rotation.
struct Quaternion {
    double w = 1.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    friend constexpr bool operator==(const Quaternion&, const Quaternion&) = default;
};

[[nodiscard]] constexpr Quaternion operator*(
    const Quaternion& a, const Quaternion& b) noexcept
{
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
[[nodiscard]] constexpr Quaternion conjugate(const Quaternion& q) noexcept
{
    return {q.w, -q.x, -q.y, -q.z};
}
[[nodiscard]] constexpr double normSquared(const Quaternion& q) noexcept
{
    return q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
}
[[nodiscard]] double norm(const Quaternion& q) noexcept;
// Unit length; the identity when the norm is not finite and positive.
[[nodiscard]] Quaternion normalized(const Quaternion& q) noexcept;
// Finite and within `tolerance` of unit length.
[[nodiscard]] bool nearUnit(const Quaternion& q, double tolerance) noexcept;
// The rotation by `angle` (radians, right-handed) about `axis`, which need
// not be unit; the identity for a zero axis.
[[nodiscard]] Quaternion axisAngle(const Real3& axis, double angle) noexcept;
[[nodiscard]] Real3 rotate(const Quaternion& q, const Real3& vector) noexcept;

// The orthographic camera of the 3-D overview (the iso quadrant) and of the
// volume renderer: the domain is normalised about its centre by its largest
// extent, rotated by `rotation` from world into view coordinates (x1 right,
// y2 up, depth toward the viewer), and projected along the view depth. Both
// the Qt view that draws the wireframe and the ray caster that draws the
// volume use these functions, so the two can never disagree about where a
// point lands -- provided both are given the same `domain` box, since the
// normalisation is about that box's own centre and largest extent. The
// constructor is what makes a stale `{azimuth, elevation, zoom}` a compile
// error rather than three quaternion components.
struct OrthoCamera {
    Quaternion rotation;
    double zoom = 1.0;
    constexpr OrthoCamera() = default;
    constexpr OrthoCamera(const Quaternion& rotation_, double zoom_) noexcept
        : rotation(rotation_)
        , zoom(zoom_)
    {
    }
    friend constexpr bool operator==(const OrthoCamera&, const OrthoCamera&) = default;
};

// The two-angle view of a camera: a turn by `azimuth` about world z, then by
// `elevation` about the turned x axis. Every rotation without roll -- world
// z drawn vertical on screen -- has exactly one such pair.
struct OrthoAngles {
    double azimuth = 0.0;
    double elevation = 0.0;
};
[[nodiscard]] OrthoCamera orthoCameraFromAngles(
    double azimuth, double elevation, double zoom = 1.0) noexcept;
// The angles of a camera without roll; nothing for one with.
[[nodiscard]] std::optional<OrthoAngles> orthoAnglesOf(
    const OrthoCamera& camera) noexcept;
// The same, for any camera: the angles of the roll-free camera nearest it,
// by the angle between rotations. At a quarter-turn roll a whole family is
// equally near and the level one is taken -- fixed, not the rounding's.
[[nodiscard]] OrthoAngles nearestOrthoAngles(const OrthoCamera& camera) noexcept;
// How far from unit length a camera's rotation may be and still be used.
inline constexpr double orthoRotationTolerance = 1.0e-3;

// Rotations about x and about z from the cosine and sine of the half angle,
// which the presets below need as literals: the trig functions are not
// constant expressions.
[[nodiscard]] constexpr Quaternion aboutX(double cosHalf, double sinHalf) noexcept
{
    return {cosHalf, sinHalf, 0.0, 0.0};
}
[[nodiscard]] constexpr Quaternion aboutZ(double cosHalf, double sinHalf) noexcept
{
    return {cosHalf, 0.0, 0.0, sinHalf};
}
inline constexpr double orthoHalfQuarterTurn = 0.70710678118654752;  // cos, sin of pi/4
inline constexpr double orthoCosPiTwelfth = 0.96592582628906829;     // cos(pi/12)
inline constexpr double orthoSinPiTwelfth = 0.25881904510252076;     // sin(pi/12)

// The cameras the XY / XZ / YZ preset buttons select: XY looks down -z with
// +y up (the identity), XZ looks along +y with +z up (elevation -pi/2), YZ
// looks along -x with +z up (azimuth -pi/2, elevation -pi/2).
inline constexpr OrthoCamera orthoPresetXY{Quaternion{}, 1.0};
inline constexpr OrthoCamera orthoPresetXZ{
    aboutX(orthoHalfQuarterTurn, -orthoHalfQuarterTurn), 1.0};
inline constexpr OrthoCamera orthoPresetYZ{
    aboutX(orthoHalfQuarterTurn, -orthoHalfQuarterTurn)
        * aboutZ(orthoHalfQuarterTurn, -orthoHalfQuarterTurn),
    1.0};

// The angle the 3-D views open at: a twelfth of a turn around the domain
// (azimuth pi/6) and tilted onto it from above (elevation -pi/6). The
// elevation is negative for the same reason the presets above are -- that
// is the sign that puts +z up. A positive one looks at the domain from
// underneath, so a plume hangs from the ceiling instead of rising from the
// floor, and the axis indicator, which shares the camera, agrees with it and
// looks equally wrong.
inline constexpr OrthoCamera orthoDefaultView{
    aboutX(orthoCosPiTwelfth, -orthoSinPiTwelfth)
        * aboutZ(orthoCosPiTwelfth, orthoSinPiTwelfth),
    1.0};

// Which way a direction in domain space points on screen, in screen sense
// (y down), as a unit-ish vector a caller scales for itself. The same rotation
// projectPoint applies, without the domain normalisation or the viewport
// scale: an axis indicator drawn from this agrees with the picture because it
// is the same arithmetic, not because someone transcribed it correctly.
struct ViewDirection {
    double x = 0.0;
    double y = 0.0;
};
[[nodiscard]] ViewDirection projectDirection(
    const OrthoCamera& camera, const Real3& direction) noexcept;

// Where the projection lands in a viewport of the given pixel size: the
// centre, and the pixel scale of one normalised unit (the normalised domain
// spans [-0.5, 0.5], so a scale of half the shorter side minus the margin
// keeps the whole domain inside the viewport at zoom 1 in every orientation).
struct ViewportFrame {
    double centerX = 0.0;
    double centerY = 0.0;
    double scale = 1.0;
};

inline constexpr double orthoViewportMargin = 12.0;

[[nodiscard]] ViewportFrame viewportFrame(
    int width, int height, double margin = orthoViewportMargin) noexcept;

// The scale at which an image rendered for `rendered` at `renderedZoom` has to
// be drawn, about the viewport centre, into `frame` at `zoom` for the two
// projections to agree.
//
// Both project about their own centre at scale * zoom, so the image's pixels
// already carry the zoom it was rendered at: the ratio of the frame scales
// alone lines up a frame rendered at another *size* (a half-size draft) but
// not one rendered at another *magnification*, and a wheel notch would then
// slide the image out of the wireframe drawn over it. Returns 1 when either
// magnification is not positive.
[[nodiscard]] double backdropScale(const ViewportFrame& frame, double zoom,
    const ViewportFrame& rendered, double renderedZoom) noexcept;

// A point projected into the viewport: pixel x (right), pixel y (down), and
// its depth along the view axis in normalised units, increasing toward the
// viewer.
struct ProjectedPoint {
    double x = 0.0;
    double y = 0.0;
    double depth = 0.0;
};

[[nodiscard]] ProjectedPoint projectPoint(const OrthoCamera& camera,
    const ViewportFrame& frame, const RealBox& domain,
    const Real3& point) noexcept;

// The view ray through a viewport position, in physical coordinates: the
// origin lies outside the domain on the viewer's side, and the unit
// direction points away from the viewer, so marching t >= 0 from the origin
// crosses the domain front to back. The inverse of projectPoint: the ray
// through a projected point's (x, y) passes through the point. The viewport
// position is continuous, with the domain centre at (width / 2, height / 2):
// a raster caster passes pixel centres, (column + 0.5, row + 0.5).
struct Ray {
    Real3 origin;
    Real3 direction;
};

[[nodiscard]] Ray pixelRay(const OrthoCamera& camera,
    const ViewportFrame& frame, const RealBox& domain, double pixelX,
    double pixelY) noexcept;

// The rays of a whole frame, resolved once. Nothing pixelRay computes from
// the camera and the domain -- the two rotations, the normalisation, and,
// because the projection is orthographic, the direction itself -- depends on
// the pixel, and the origin is affine in the pixel position. A raster caster
// resolves this once per frame and steps the origin per column and per row
// instead of rebuilding the rotation for every pixel; pixelRay is one call
// through it, so the two cannot disagree.
struct RayField {
    Real3 origin;      // the ray origin at viewport position (0, 0)
    Real3 perPixelX;   // the origin's change per pixel to the right
    Real3 perPixelY;   // the origin's change per pixel down
    Real3 direction;   // unit, away from the viewer, shared by every pixel
};

[[nodiscard]] RayField rayField(const OrthoCamera& camera,
    const ViewportFrame& frame, const RealBox& domain) noexcept;

[[nodiscard]] Ray rayAt(const RayField& field, double pixelX,
    double pixelY) noexcept;

} // namespace amrvis
