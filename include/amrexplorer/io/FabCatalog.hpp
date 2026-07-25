#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/io/PlotfileBlockReader.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace amrvis {

struct FabRecord {
    std::size_t ordinal = 0;
    std::filesystem::path filePath;
    std::uint64_t headerOffset = 0;
    std::uint64_t payloadOffset = 0;
    IntBox storedBox;
    int dimension = 0;
    int components = 0;
    FabRealPrecision precision = FabRealPrecision::Double;
    std::string realDescriptor;
    int visMfHeaderVersion = 1;
};

[[nodiscard]] FabRecord inspectFabRecord(
    const std::filesystem::path& path, std::uint64_t offset = 0);
[[nodiscard]] FabRealPrecision fabPrecisionFromDescriptor(
    std::string_view descriptor);
[[nodiscard]] std::vector<FabRecord> scanFabFile(
    const std::filesystem::path& path);

} // namespace amrvis
