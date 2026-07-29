#pragma once

#include <amrexplorer/core/Result.hpp>

#include <filesystem>

namespace amrvis {

// Writes a two-dimensional primary FITS image with BITPIX=-64. ScalarPlane
// stores samples as floats for the interactive rendering pipeline; the writer
// promotes valid samples to IEEE binary64 and writes invalid samples as NaN.
// Plane storage already has the FITS-friendly orientation: x varies fastest
// and y increases from the physical lower edge.
void writeFloat64Fits(
    const std::filesystem::path& path, const ScalarPlane& plane);

} // namespace amrvis
