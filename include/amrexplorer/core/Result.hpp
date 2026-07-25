#pragma once

#include <amrexplorer/core/Geometry.hpp>

#include <cstdint>
#include <vector>

namespace amrvis {

struct ScalarPlane {
    int width = 0;
    int height = 0;
    RealBox physicalRegion;
    std::vector<float> values;
    std::vector<std::uint8_t> valid;
    std::vector<std::int16_t> sourceLevel;
};

struct LineResult {
    int axis = 0;
    // Physical sample coordinates for plotfiles, integer indices (stored as
    // doubles for plotting) for standalone FABs and MultiFabs.
    std::vector<double> positions;
    bool positionsAreIndices = false;
    std::vector<float> values;
    std::vector<std::uint8_t> valid;
    std::vector<std::int16_t> sourceLevel;
};

} // namespace amrvis
