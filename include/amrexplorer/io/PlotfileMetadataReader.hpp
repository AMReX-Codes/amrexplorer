#pragma once

#include <amrexplorer/core/Metadata.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace amrvis {

struct MetadataReadMetrics {
    std::uint64_t filesRead = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t payloadFilesRead = 0;
    std::uint64_t payloadBytesRead = 0;
};

struct PlotfileMetadataResult {
    std::shared_ptr<const DatasetMetadata> metadata;
    MetadataReadMetrics metrics;
    std::string fileVersion;
};

class MetadataReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class PlotfileMetadataReader {
public:
    [[nodiscard]] PlotfileMetadataResult read(const std::filesystem::path& plotfile) const;
};

} // namespace amrvis
