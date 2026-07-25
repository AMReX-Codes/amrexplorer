#include <amrexplorer/render2d/ImageBuffer.hpp>

#include <cstddef>
#include <limits>

namespace amrvis {

bool ImageBuffer::valid() const noexcept
{
    if (width < 0 || height < 0 || strideBytes < 0) {
        return false;
    }
    const auto pixelCount = static_cast<std::uint64_t>(width)
        * static_cast<std::uint64_t>(height);
    if (pixelCount > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    const auto minimumStride = static_cast<std::uint64_t>(width)
        * static_cast<std::uint64_t>(sizeof(std::uint32_t));
    return rgba.size() == static_cast<std::size_t>(pixelCount)
        && static_cast<std::uint64_t>(strideBytes) >= minimumStride;
}

} // namespace amrvis
