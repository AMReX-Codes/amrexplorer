#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/StopToken.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace amrvis {

class BlockSource;

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
    // The open source this metadata was read from, for a format whose blocks
    // can only be read through the handle that read its metadata (an openPMD
    // series). A dataset built from this result reads through it; null for a
    // plotfile, MultiFab or FAB, which reopen their files per block.
    std::shared_ptr<const BlockSource> source;
    // Every iteration of the openPMD series this metadata is one iteration
    // of, in order; empty for a plotfile, MultiFab or FAB. More than one is
    // what makes the GUI open the series as a sequence.
    std::vector<std::uint64_t> iterations;
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
