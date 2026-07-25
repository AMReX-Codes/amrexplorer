#pragma once

namespace amrvis {

// How the view treats its existing transform when a replacement raster
// arrives. Preserve keeps the current panel-local transform (rubber-band zoom
// or pan refresh). GeometryAware refits only when the raster dimensions
// change. Refit discards the transform even for equal-size rasters whose
// data regions are incompatible. Lives in the Qt-free pipeline layer so the
// DisplayCoordinator can decide it from pure request data; ImageView
// re-exports it for the GUI.
enum class ImageTransformPolicy {
    GeometryAware,
    Preserve,
    Refit
};

} // namespace amrvis
