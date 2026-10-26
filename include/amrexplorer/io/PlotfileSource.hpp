#pragma once

#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/BlockSource.hpp>
#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>

#include <filesystem>
#include <memory>
#include <vector>

namespace amrvis {

// An AMReX plotfile, MultiFab or standalone FAB as a BlockSource.
class PlotfileSource final : public BlockSource {
public:
    // Reads the metadata of whatever `path` names: a plotfile directory, a
    // MultiFab or a FAB.
    explicit PlotfileSource(
        const std::filesystem::path& path, StopToken cancellation = {});
    // Metadata already read (a FAB selected from a catalog), with the
    // directory its block paths are relative to.
    PlotfileSource(std::filesystem::path dataRoot,
        PlotfileMetadataResult metadata, StopToken cancellation = {});

    [[nodiscard]] std::shared_ptr<const DatasetMetadata> metadata()
        const noexcept override;
    [[nodiscard]] const MetadataReadMetrics& metadataReadMetrics()
        const noexcept override;
    // The Header's version *token* -- its first whitespace-delimited word --
    // as the bounded metadata read already parsed it, so no caller has to
    // re-read the Header for it. AMReX writes the version alone on that line
    // ("HyperCLaw-V1.1"), so this is the whole first line of every real
    // plotfile.
    [[nodiscard]] const std::string& fileVersion() const noexcept override;
    [[nodiscard]] const std::vector<ParticleSpeciesMetadata>& particleSpecies()
        const noexcept override;
    [[nodiscard]] ParticleSample readParticleSample(const std::string& species,
        double fraction, std::uint64_t seed, StopToken cancellation,
        std::size_t maximumPoints,
        std::optional<ParticleAttribute> attribute) const override;
    [[nodiscard]] BlockReadResult readBlock(
        const BlockRequest& request, StopToken cancellation) const override;
    // The Nu_nd hierarchy, when the Header lists it and every level checks
    // out.
    [[nodiscard]] std::shared_ptr<const BlockSource> mappedGrid()
        const noexcept override;

private:
    struct MappedGridTag {};
    // The mapped-grid sub-source: metadata only, no particle discovery.
    PlotfileSource(MappedGridTag, std::filesystem::path root,
        std::shared_ptr<const DatasetMetadata> metadata);
    [[nodiscard]] static std::shared_ptr<const BlockSource> makeMappedGrid(
        const std::filesystem::path& root,
        const PlotfileMetadataResult& source);

    std::filesystem::path m_root;
    PlotfileMetadataResult m_metadataResult;
    std::vector<ParticleSpeciesMetadata> m_particleSpecies;
    PlotfileBlockReader m_blockReader;
    std::shared_ptr<const BlockSource> m_mappedGrid;
};

} // namespace amrvis
