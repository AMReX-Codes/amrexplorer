#pragma once

#include <amrexplorer/core/Geometry.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace amrvis::detail {

struct VisMfIndex {
    int version = 0;
    int components = 0;
    Int3 ghostWidth;
    std::vector<IntBox> boxes;
    std::vector<std::string> fileNames;
    std::vector<std::uint64_t> fileOffsets;
    std::vector<std::vector<double>> minimum;
    std::vector<std::vector<double>> maximum;
    bool hasPerBlockStatistics = false;
    std::string realDescriptor;
    std::uint64_t bytesRead = 0;
};

[[nodiscard]] VisMfIndex readVisMfIndex(
    const std::filesystem::path& headerPath, int dimension);

} // namespace amrvis::detail
