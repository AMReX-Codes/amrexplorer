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

ImageBuffer transposeImage(const ImageBuffer& src)
{
    ImageBuffer dst;
    if (src.width <= 0 || src.height <= 0 || src.rgba.empty()) {
        return src;
    }
    dst.width = src.height;
    dst.height = src.width;
    dst.strideBytes = dst.width * static_cast<int>(sizeof(std::uint32_t));
    dst.rgba.resize(src.rgba.size());
    const auto srcW = static_cast<std::size_t>(src.width);
    const auto srcH = static_cast<std::size_t>(src.height);
    for (std::size_t row = 0; row < srcH; ++row) {
        for (std::size_t col = 0; col < srcW; ++col) {
            // dst is srcH wide: dst(row=col, col=row).
            dst.rgba[col * srcH + row] = src.rgba[row * srcW + col];
        }
    }
    return dst;
}

} // namespace amrvis
