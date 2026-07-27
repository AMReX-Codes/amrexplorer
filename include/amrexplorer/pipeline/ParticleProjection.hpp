#pragma once

#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/io/ParticleReader.hpp>

#include <span>
#include <vector>

namespace amrvis {

struct ParticlePixel {
    double x = 0.0;
    double y = 0.0;
};

// Projects physical particle positions into a slice plane's raster scene
// coordinates. The in-plane physical bounds map inclusively to the raster
// edges: x increases from 0 to width, while y is flipped from height to 0 to
// match the Qt image scene. The normal coordinate is deliberately ignored;
// particles are projected through the full volume rather than filtered to a
// slice thickness.
[[nodiscard]] std::vector<ParticlePixel>
projectParticlePoints(std::span<const ParticlePoint> particles,
                      const ScalarPlane& plane, int dimension, int normalDirection);

} // namespace amrvis
