#pragma once

#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/core/StopToken.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace amrvis {

class StandaloneMetadataReader {
public:
    [[nodiscard]] PlotfileMetadataResult readFab(
        const std::filesystem::path& fabPath,
        std::uint64_t offset = 0, StopToken cancellation = {}) const;
    [[nodiscard]] PlotfileMetadataResult readMultiFab(
        const std::filesystem::path& prefixOrHeader, StopToken cancellation = {}) const;
};

[[nodiscard]] PlotfileMetadataResult makeSelectedFabMetadata(
    const DatasetMetadata& source, int levelIndex, std::size_t blockIndex,
    const std::filesystem::path& dataRoot);

// Inspect a plotfile directory, standalone FAB, or serialized MultiFab prefix/header.
[[nodiscard]] PlotfileMetadataResult readDatasetMetadata(
    const std::filesystem::path& path, StopToken cancellation = {});

} // namespace amrvis
