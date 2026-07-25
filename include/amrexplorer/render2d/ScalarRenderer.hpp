#pragma once

#include <amrexplorer/core/Result.hpp>
#include <amrexplorer/render2d/ImageBuffer.hpp>
#include <amrexplorer/render2d/Palette.hpp>

#include <array>
#include <cstdint>

namespace amrvis {

struct ScalarRenderSettings {
    double minimum = 0.0;
    double maximum = 1.0;
    bool logarithmic = false;
    std::uint32_t invalidColor = 0xFF303030U;
    std::uint32_t nanColor = 0xFFFF00FFU;
    // When null, the built-in legacy rainbow palette is used.
    const Palette* palette = nullptr;
};

[[nodiscard]] std::array<std::uint8_t, 3> sampleViridis(double normalized) noexcept;
[[nodiscard]] ImageBuffer renderScalarPlane(
    const ScalarPlane& plane, const ScalarRenderSettings& settings);

} // namespace amrvis

