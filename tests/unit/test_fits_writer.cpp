#include <amrexplorer/io/FitsWriter.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

double readBigEndianDouble(
    const std::vector<unsigned char>& bytes, std::size_t offset)
{
    std::uint64_t bits = 0;
    for (std::size_t index = 0; index < sizeof(bits); ++index) {
        bits = (bits << 8U) | bytes[offset + index];
    }
    return std::bit_cast<double>(bits);
}

} // namespace

int main()
{
    try {
        const auto path = std::filesystem::temp_directory_path()
            / "amrexplorer-float64-writer-test.fits";
        amrvis::ScalarPlane plane;
        plane.width = 2;
        plane.height = 2;
        plane.values = {1.25F, -2.5F, 3.75F, 99.0F};
        plane.valid = {1, 1, 1, 0};
        plane.sourceLevel = {0, 0, 1, -1};

        amrvis::writeFloat64Fits(path, plane);
        std::vector<unsigned char> bytes;
        {
            std::ifstream input(path, std::ios::binary);
            bytes = {
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
        }
        std::filesystem::remove(path);

        require(bytes.size() == 2 * 2880, "FITS file is not block padded");
        const std::string header(bytes.begin(), bytes.begin() + 2880);
        require(header.substr(0, 30) == "SIMPLE  =                    T",
            "missing SIMPLE card");
        require(header.find("BITPIX  =                  -64") != std::string::npos,
                "BITPIX is not float64");
        require(header.find("NAXIS1  =                    2") != std::string::npos,
                "incorrect NAXIS1");
        require(header.find("NAXIS2  =                    2") != std::string::npos,
                "incorrect NAXIS2");
        require(readBigEndianDouble(bytes, 2880) == 1.25,
            "first sample is incorrect");
        require(readBigEndianDouble(bytes, 2888) == -2.5,
            "second sample is incorrect");
        require(readBigEndianDouble(bytes, 2896) == 3.75,
            "third sample is incorrect");
        require(std::isnan(readBigEndianDouble(bytes, 2904)),
            "invalid sample is not NaN");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
