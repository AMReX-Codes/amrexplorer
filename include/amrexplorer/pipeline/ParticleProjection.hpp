#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/io/ParticleReader.hpp>

#include <span>
#include <vector>

namespace amrvis {

struct ParticlePixel {
    double x = 0.0;
    double y = 0.0;
};

// The half-open physical span the slice plane's cell occupies on the plane
// normal, at one AMR level. lower == upper means the plane cuts no cell at
// that level, so nothing drawn from it can hold a particle.
struct SliceCellSlab {
    double lower = 0.0;
    double upper = 0.0;
};

// The cell the plane cuts at every level, indexed the way a plane's
// sourceLevel is: positionally into metadata.levels. Empty for anything but a
// 3-D dataset, which is the only case where a slice has a normal to filter on.
[[nodiscard]] std::vector<SliceCellSlab> sliceCellSlabs(
    const DatasetMetadata& metadata, int normalAxis, double slicePosition);

// Projects physical particle positions into a slice plane's raster scene
// coordinates. The in-plane physical bounds map inclusively to the raster
// edges: x increases from 0 to width, while y is flipped from height to 0 to
// match the Qt image scene.
//
// With no levelSlabs the normal coordinate is ignored and particles are
// projected through the full volume -- the default. Given one slab per level,
// a particle is kept only when its normal coordinate lies in the slab of the
// level that supplied the pixel it projects onto (plane.sourceLevel), so the
// thickness always matches the cell actually drawn there; a particle over an
// uncovered pixel is dropped, there being no cell for it to be in.
[[nodiscard]] std::vector<ParticlePixel>
projectParticlePoints(std::span<const ParticlePoint> particles,
                      const ScalarPlane& plane, int dimension,
                      int normalDirection,
                      std::span<const SliceCellSlab> levelSlabs = {});

} // namespace amrvis
