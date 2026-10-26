#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/ParticleReader.hpp>
#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace amrvis {

// Where a dataset's stored data comes from: one on-disk format's metadata,
// block and particle reads, and nothing else. AmrDataset layers the block
// cache, the per-dataset IO lock and derived fields on top, so a new format
// implements only this.
//
// A source is immutable once constructed. readBlock is called under the
// owning dataset's IO lock, but particle reads and every accessor may run
// concurrently with it and with each other.
class BlockSource {
public:
    virtual ~BlockSource() = default;

    // The stored fields only -- derived fields are the dataset's, not the
    // source's. Never null.
    [[nodiscard]] virtual std::shared_ptr<const DatasetMetadata> metadata()
        const noexcept = 0;
    [[nodiscard]] virtual const MetadataReadMetrics& metadataReadMetrics()
        const noexcept = 0;
    // The format's own version string, as the UI's Format field shows it.
    [[nodiscard]] virtual const std::string& fileVersion() const noexcept = 0;
    // Every iteration of the series this source is one iteration of, in
    // order; empty for a format without iterations.
    [[nodiscard]] virtual std::vector<std::uint64_t> seriesIterations() const
    {
        return {};
    }
    [[nodiscard]] virtual const std::vector<ParticleSpeciesMetadata>&
    particleSpecies() const noexcept = 0;
    [[nodiscard]] virtual ParticleSample readParticleSample(
        const std::string& species, double fraction, std::uint64_t seed,
        StopToken cancellation, std::size_t maximumPoints,
        std::optional<ParticleAttribute> attribute) const = 0;

    // One stored field's block. The request's field is an index into
    // metadata()->fields; the dataset never asks for a derived one.
    [[nodiscard]] virtual BlockReadResult readBlock(
        const BlockRequest& request, StopToken cancellation) const = 0;

    // The mapped-grid (nodal displacement) hierarchy as a source of its own,
    // or null when the data carries none. It has no particles and no mapped
    // grid of its own.
    [[nodiscard]] virtual std::shared_ptr<const BlockSource> mappedGrid()
        const noexcept
    {
        return nullptr;
    }
};

} // namespace amrvis
