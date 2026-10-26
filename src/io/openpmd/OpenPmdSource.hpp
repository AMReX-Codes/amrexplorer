#pragma once

// Private to amrexplorer_io: the one header that includes openPMD-api, so
// nothing above src/io (and nothing in a build without openPMD) sees it.

#include <amrexplorer/io/BlockSource.hpp>

#include <openPMD/openPMD.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace amrvis {

// One iteration of an openPMD series as a BlockSource (see OpenPmd.hpp for
// how meshes map onto levels and fields).
class OpenPmdSource final : public BlockSource {
public:
    explicit OpenPmdSource(
        const std::filesystem::path& path, StopToken cancellation = {});

    [[nodiscard]] std::shared_ptr<const DatasetMetadata> metadata()
        const noexcept override;
    [[nodiscard]] const MetadataReadMetrics& metadataReadMetrics()
        const noexcept override;
    [[nodiscard]] const std::string& fileVersion() const noexcept override;
    // Particles are not read yet (docs/openpmd-plan.md, phase 3).
    [[nodiscard]] const std::vector<ParticleSpeciesMetadata>& particleSpecies()
        const noexcept override;
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

private:
    // Every Series call goes through this: openPMD-api is not thread-safe,
    // and readBlock is called under the owning dataset's IO lock, which a
    // mapped grid or a second dataset on the same source would not share.
    mutable std::mutex m_mutex;
    mutable openPMD::Series m_series;
    std::uint64_t m_iteration = 0;
    std::filesystem::path m_path;
    // By level, then by field id.
    std::vector<std::vector<Component>> m_components;
    std::shared_ptr<const DatasetMetadata> m_metadata;
    MetadataReadMetrics m_metrics;
    std::string m_fileVersion;
    std::vector<ParticleSpeciesMetadata> m_particleSpecies;
};

} // namespace amrvis
