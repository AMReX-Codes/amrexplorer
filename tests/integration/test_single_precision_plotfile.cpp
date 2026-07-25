#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/query/LineQuery.hpp>
#include <amrexplorer/query/SliceQuery.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view singleDescriptor =
    "((8, (32 8 23 0 1 9 0 127)),(4, (4 3 2 1)))";
constexpr std::string_view doubleDescriptor =
    "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))";

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create single-precision fixture text");
    output << text;
}

template <typename T>
void writeLittleEndian(std::ofstream& output, std::span<const T> values)
{
    for (const auto value : values) {
        std::array<unsigned char, sizeof(T)> bytes{};
        std::memcpy(bytes.data(), &value, sizeof(value));
        if constexpr (std::endian::native == std::endian::big) {
            std::reverse(bytes.begin(), bytes.end());
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
}

template <typename T>
void writeFab(const std::filesystem::path& path, std::string_view descriptor,
    std::span<const T> values)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create single-precision fixture FAB");
    output << "FAB " << descriptor << "((0,0) (1,1) (0,0)) 1\n";
    writeLittleEndian(output, values);
}

} // namespace

int main()
{
    constexpr double time = 1.23456789012345;
    constexpr double lowerX = 0.123456789012345;
    constexpr double lowerY = 0.234567890123456;
    constexpr double upperX = 1.123456789012347;
    constexpr double upperY = 1.234567890123460;
    constexpr double cellX = 0.500000000000001;
    constexpr double cellY = 0.500000000000002;
    constexpr std::array<float, 4> singleValues{1.25F, -2.5F, 3.75F, 4.5F};
    constexpr std::array<double, 4> doubleValues{1.25, -2.5, 3.75, 4.5};

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("amrexplorer-single-precision-" + std::to_string(unique));
    std::filesystem::create_directories(root / "Level_0");

    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "1\nphi\n"
        "2\n1.23456789012345\n0\n"
        "0.123456789012345 0.234567890123456\n"
        "1.123456789012347 1.234567890123460\n\n"
        "((0,0) (1,1) (0,0))\n"
        "0\n0.500000000000001 0.500000000000002\n0\n0\n"
        "0 1 1.23456789012345\n0\n"
        "0.123456789012345 1.123456789012347\n"
        "0.234567890123456 1.234567890123460\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((0,0) (1,1) (0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n-2.5,\n\n"
        "1,1\n4.5,\n\n");
    writeFab(root / "Level_0" / "Cell_D_00000",
        singleDescriptor, std::span<const float>(singleValues));
    writeFab(root / "DoubleFab", doubleDescriptor,
        std::span<const double>(doubleValues));

    amrvis::PlotfileDataset dataset(root, amrvis::DatasetId{1}, 1024 * 1024);
    const auto& metadata = dataset.metadata();
    require(metadata.time == time, "single plotfile time was narrowed to float");
    require(metadata.physicalDomain.lower[0] == lowerX
            && metadata.physicalDomain.lower[1] == lowerY,
        "single plotfile lower bounds were narrowed to float");
    require(metadata.physicalDomain.upper[0] == upperX
            && metadata.physicalDomain.upper[1] == upperY,
        "single plotfile upper bounds were narrowed to float");
    require(metadata.levels[0].cellSize[0] == cellX
            && metadata.levels[0].cellSize[1] == cellY,
        "single plotfile cell sizes were narrowed to float");

    amrvis::BlockRequest blockRequest;
    blockRequest.dataset = dataset.id();
    auto firstAccess = dataset.requestBlock(blockRequest);
    require(!firstAccess.cacheHit, "first single-precision block access was cached");
    require(firstAccess.handle->values.precision() == amrvis::FabRealPrecision::Single,
        "IEEE-32 FAB was not retained in float storage");
    require(firstAccess.handle->values.elementBytes() == sizeof(float),
        "single-precision FAB reports the wrong element size");
    require(firstAccess.handle->values.size() == singleValues.size(),
        "single-precision FAB value count mismatch");
    for (std::size_t index = 0; index < singleValues.size(); ++index) {
        require(firstAccess.handle->values[index]
                == static_cast<double>(singleValues[index]),
            "single-precision FAB value mismatch");
    }
    const auto expectedPayloadBytes = singleValues.size() * sizeof(float);
    require(firstAccess.io.bytesRead > expectedPayloadBytes,
        "single-precision read omitted its FAB header bytes");
    firstAccess.handle = {};
    const auto cachedAccess = dataset.requestBlock(blockRequest);
    require(cachedAccess.cacheHit && cachedAccess.io.bytesRead == 0,
        "single-precision block was not reused from the cache");
    require(cachedAccess.handle.bytes()
            == sizeof(amrvis::FabBlock)
                + cachedAccess.handle->values.residentBytes(),
        "single-precision cache resident-byte accounting mismatch");

    amrvis::SliceRequest sliceRequest;
    sliceRequest.dataset = dataset.id();
    sliceRequest.normalDirection = 1;
    sliceRequest.visibleRegion = metadata.physicalDomain;
    sliceRequest.outputSize = {2, 2};
    const auto slice = amrvis::SliceQuery(dataset).execute(sliceRequest);
    require(slice.plane.values == std::vector<float>(
            singleValues.begin(), singleValues.end()),
        "single-precision slice values mismatch");
    require(std::all_of(slice.plane.valid.begin(), slice.plane.valid.end(),
            [](std::uint8_t valid) { return valid == 1; }),
        "single-precision slice lost covered cells");

    amrvis::LineRequest lineRequest;
    lineRequest.dataset = dataset.id();
    lineRequest.axis = 0;
    lineRequest.fixedCoordinates[1] = lowerY + 0.25;
    const auto line = amrvis::LineQuery(dataset).execute(lineRequest);
    require(line.line.positions.size() == 2 && line.line.values.size() == 2,
        "single-precision line sample count mismatch");
    require(line.line.values[0] == singleValues[0]
            && line.line.values[1] == singleValues[1],
        "single-precision line values mismatch");

    amrvis::PlotfileDataset multiFabDataset(
        root / "Level_0" / "Cell", amrvis::DatasetId{2}, 1024 * 1024);
    blockRequest.dataset = multiFabDataset.id();
    const auto multiFabAccess = multiFabDataset.requestBlock(blockRequest);
    require(multiFabAccess.handle->values.precision()
            == amrvis::FabRealPrecision::Single,
        "standalone single-precision MultiFab was not retained as float");

    amrvis::PlotfileDataset fabDataset(
        root / "Level_0" / "Cell_D_00000", amrvis::DatasetId{3}, 1024 * 1024);
    blockRequest.dataset = fabDataset.id();
    const auto fabAccess = fabDataset.requestBlock(blockRequest);
    require(fabAccess.handle->values.precision() == amrvis::FabRealPrecision::Single,
        "standalone single-precision FAB was not retained as float");

    amrvis::PlotfileDataset doubleDataset(
        root / "DoubleFab", amrvis::DatasetId{4}, 1024 * 1024);
    blockRequest.dataset = doubleDataset.id();
    const auto doubleAccess = doubleDataset.requestBlock(blockRequest);
    require(doubleAccess.handle->values.precision() == amrvis::FabRealPrecision::Double,
        "IEEE-64 FAB was not retained in double storage");
    require(doubleAccess.handle->values.elementBytes() == sizeof(double),
        "double-precision FAB reports the wrong element size");
    for (std::size_t index = 0; index < doubleValues.size(); ++index) {
        require(doubleAccess.handle->values[index] == doubleValues[index],
            "double-precision FAB value mismatch");
    }

    std::filesystem::remove_all(root);
    return 0;
}
