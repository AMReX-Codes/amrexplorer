#include <amrexplorer/io/FitsWriter.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace amrvis {
namespace {

constexpr std::size_t fitsBlockSize = 2880;
constexpr std::size_t fitsCardSize = 80;

std::string card(std::string_view keyword, std::string_view value)
{
    std::string result(fitsCardSize, ' ');
    result.replace(0, keyword.size(), keyword);
    result.replace(8, 2, "= ");
    const auto valueStart = 30 - std::min<std::size_t>(value.size(), 20);
    result.replace(valueStart, value.size(), value);
    return result;
}

void writeHeader(std::ostream& output, int width, int height)
{
    std::string header;
    header.reserve(fitsBlockSize);
    header += card("SIMPLE", "T");
    header += card("BITPIX", "-64");
    header += card("NAXIS", "2");
    header += card("NAXIS1", std::to_string(width));
    header += card("NAXIS2", std::to_string(height));
    header += card("EXTEND", "T");
    header += std::string("END") + std::string(fitsCardSize - 3, ' ');
    header.resize(fitsBlockSize, ' ');
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
}

void writeBigEndianDouble(std::ostream& output, double value)
{
    const auto bits = std::bit_cast<std::uint64_t>(value);
    std::array<char, sizeof(bits)> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = 8U * static_cast<unsigned>(bytes.size() - 1 - index);
        bytes[index] = static_cast<char>((bits >> shift) & 0xffU);
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

void writeFloat64Fits(
    const std::filesystem::path& path, const ScalarPlane& plane)
{
    if (plane.width <= 0 || plane.height <= 0) {
        throw std::invalid_argument("FITS image dimensions must be positive");
    }
    const auto width = static_cast<std::size_t>(plane.width);
    const auto height = static_cast<std::size_t>(plane.height);
    if (width > std::numeric_limits<std::size_t>::max() / height) {
        throw std::overflow_error("FITS image dimensions exceed addressable memory");
    }
    const auto pixelCount = width * height;
    if (plane.values.size() != pixelCount
        || plane.valid.size() != pixelCount
        || plane.sourceLevel.size() != pixelCount) {
        throw std::invalid_argument(
            "FITS image storage does not match its dimensions");
    }
    if (pixelCount > std::numeric_limits<std::size_t>::max() / sizeof(double)) {
        throw std::overflow_error("FITS image byte count exceeds addressable memory");
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not open FITS image for writing");
    }

    writeHeader(output, plane.width, plane.height);
    const auto invalid = std::numeric_limits<double>::quiet_NaN();
    for (std::size_t index = 0; index < plane.values.size(); ++index) {
        writeBigEndianDouble(output,
            plane.valid[index] != 0
                ? static_cast<double>(plane.values[index]) : invalid);
    }

    const auto dataBytes = plane.values.size() * sizeof(double);
    const auto padding = (fitsBlockSize - dataBytes % fitsBlockSize)
        % fitsBlockSize;
    const std::array<char, fitsBlockSize> zeros{};
    output.write(zeros.data(), static_cast<std::streamsize>(padding));
    output.close();
    if (!output) {
        throw std::runtime_error("could not write FITS image");
    }
}

} // namespace amrvis
