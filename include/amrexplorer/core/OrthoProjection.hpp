#pragma once

#include <amrexplorer/core/Geometry.hpp>

namespace amrvis {

// The orthographic camera of the 3-D overview (the iso quadrant) and of the
// volume renderer: the domain is normalised about its centre by its largest
// extent, rotated by `azimuth` about z and then by `elevation` about the
// rotated x axis, and projected along the view depth. Both the Qt view that
// draws the wireframe and the ray caster that draws the volume use these
// functions, so the two can never disagree about where a point lands --
// provided both are given the same `domain` box, since the normalisation is
// about that box's own centre and largest extent.
struct OrthoCamera {
    double azimuth = 0.0;    // radians, about z
    double elevation = 0.0;  // radians, about the rotated x axis
    double zoom = 1.0;
    friend bool operator==(const OrthoCamera&, const OrthoCamera&) = default;
};

// The camera angles the XY / XZ / YZ preset buttons select: XY looks down -z
// with +y up, XZ looks along +y with +z up, YZ looks along -x with +z up.
inline constexpr OrthoCamera orthoPresetXY{0.0, 0.0, 1.0};
inline constexpr OrthoCamera orthoPresetXZ{0.0, -1.5707963267948966, 1.0};
inline constexpr OrthoCamera orthoPresetYZ{
    -1.5707963267948966, -1.5707963267948966, 1.0};

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
