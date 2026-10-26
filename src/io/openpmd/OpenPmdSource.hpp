#pragma once

// Private to amrexplorer_io: the one header that includes openPMD-api, so
// nothing above src/io (and nothing in a build without openPMD) sees it.

#include <amrexplorer/io/BlockSource.hpp>

#include "OpenPmdSeries.hpp"

#include <openPMD/openPMD.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace amrvis {

// One iteration of an openPMD series as a BlockSource (see OpenPmd.hpp for
// how meshes map onto levels and fields).
class OpenPmdSource final : public BlockSource {
public:
    // Reads `iteration`, or the series' first when there is none.
    OpenPmdSource(const std::filesystem::path& path,
        std::optional<std::uint64_t> iteration = std::nullopt,
        StopToken cancellation = {});

    // Every iteration in the series, in order.
    [[nodiscard]] const std::vector<std::uint64_t>& iterations() const noexcept;

    [[nodiscard]] std::shared_ptr<const DatasetMetadata> metadata()
        const noexcept override;
    [[nodiscard]] const MetadataReadMetrics& metadataReadMetrics()
        const noexcept override;
    [[nodiscard]] const std::string& fileVersion() const noexcept override;
    [[nodiscard]] std::vector<std::uint64_t> seriesIterations() const override
    {
        return m_iterations;
    }
    // The iteration's particle species that have a position along every axis
    // of its meshes. Each record component other than position,
    // positionOffset and id is a component: NAME for a scalar record,
    // NAME_KEY otherwise, integer-typed ones among the ints.
    [[nodiscard]] const std::vector<ParticleSpeciesMetadata>& particleSpecies()
        const noexcept override;
    // Sampled by the `id` record (by index when there is none) with the
    // plotfile reader's hash, so a smaller fraction is a subset of a larger
    // one. Positions are position + positionOffset, in SI.
    [[nodiscard]] ParticleSample readParticleSample(const std::string& species,
        double fraction, std::uint64_t seed, StopToken cancellation,
        std::size_t maximumPoints,
        std::optional<ParticleAttribute> attribute) const override;
    [[nodiscard]] BlockReadResult readBlock(
        const BlockRequest& request, StopToken cancellation) const override;

    // Where one field's samples are at one level: the record component to
    // read, and how its array maps onto the level's cells.
    struct Component {
        std::string mesh;
        // The key in the mesh, or RecordComponent::SCALAR.
        std::string key;
        // The spatial axis each array dimension holds (C order: the last
        // dimension varies fastest).
        std::array<int, 3> axisOfDimension{0, 1, 2};
        // By spatial axis: where in a cell the samples sit (0.5 is the
        // centre, 0 the lower face) and how many there are.
        std::array<double, 3> position{0.5, 0.5, 0.5};
        std::array<std::uint64_t, 3> extent{1, 1, 1};
        double unitSI = 1.0;
    };

    // One particle component: a record and its key in it.
    struct ParticleRecord {
        std::string record;
        std::string key;
    };
    // Where a species' data is, beside the metadata it reports.
    struct SpeciesLayout {
        // By spatial axis: the position (and positionOffset) component.
        std::array<std::string, 3> axisKey;
        bool hasOffset = false;
        bool hasId = false;
        std::vector<ParticleRecord> reals;
        std::vector<ParticleRecord> ints;
    };

private:
    void readParticleLayouts(StopToken cancellation);

    // Every Series call goes through this: openPMD-api is not thread-safe,
    // and readBlock is called under the owning dataset's IO lock, which a
    // mapped grid or a second dataset on the same source would not share.
    // Shared with every other source on the same series (OpenPmdSeries).
    std::shared_ptr<OpenPmdSeries> m_open;
    std::uint64_t m_iteration = 0;
    std::vector<std::uint64_t> m_iterations;
    // By spatial axis: the label ("x", "y" or "z") the meshes give it, which
    // is the key of the particle position component along it.
    std::array<std::string, 3> m_axisLabels{"x", "y", "z"};
    std::filesystem::path m_path;
    // By level, then by field id.
    std::vector<std::vector<Component>> m_components;
    std::shared_ptr<const DatasetMetadata> m_metadata;
    MetadataReadMetrics m_metrics;
    std::string m_fileVersion;
    std::vector<ParticleSpeciesMetadata> m_particleSpecies;
    // Parallel to m_particleSpecies.
    std::vector<SpeciesLayout> m_speciesLayouts;
};

} // namespace amrvis
