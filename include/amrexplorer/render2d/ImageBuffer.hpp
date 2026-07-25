#pragma once

#include <cstdint>
#include <vector>

namespace amrvis {

struct ImageBuffer {
    int width = 0;
    int height = 0;
    int strideBytes = 0;
    std::vector<std::uint32_t> rgba;

    [[nodiscard]] bool valid() const noexcept;
};

} // namespace amrvis

