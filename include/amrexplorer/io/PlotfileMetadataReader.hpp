#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/StopToken.hpp>

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
    // The mapped-grid (nodal "Nu_nd") hierarchy when the plotfile Header
    // declares one and every level's _H checks out: the same geometry as
    // `metadata` with nodal level domains, one Node field per space
    // dimension (amrexvec_nu_x, _y, _z) and the Nu_nd boxes and blocks. Null
    // otherwise; metadata->hasMappedGrid says which.
    std::shared_ptr<const DatasetMetadata> mappedGrid;
};

// Component names that identify the mapped-grid block among a Header's
// trailing extra-MultiFab sets.
inline constexpr const char* mappedGridComponentNames[3]
    = {"amrexvec_nu_x", "amrexvec_nu_y", "amrexvec_nu_z"};

class MetadataReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class PlotfileMetadataReader {
public:
    [[nodiscard]] PlotfileMetadataResult read(const std::filesystem::path& plotfile, StopToken cancellation = {}) const;
};

} // namespace amrvis
