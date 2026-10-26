#include <amrexplorer/io/PlotfileSource.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>

#include <stdexcept>
#include <utility>

namespace amrvis {
namespace {

std::filesystem::path sourceDataRoot(const std::filesystem::path& path)
{
    if (std::filesystem::is_directory(path)
        && std::filesystem::is_regular_file(path / "Header")) {
        return path;
    }
    const auto parent = path.parent_path();
    return parent.empty() ? std::filesystem::path(".") : parent;
}

std::vector<ParticleSpeciesMetadata> discoverParticlesForPlotfileRoot(
    const std::filesystem::path& root, StopToken cancellation)
{
    if (std::filesystem::is_directory(root)
        && std::filesystem::is_regular_file(root / "Header")) {
        return discoverParticleSpecies(root, cancellation);
    }
    return {};
}

// The metadata result, refused when it carries no metadata -- before particle
// discovery or the block reader would otherwise run on it.
PlotfileMetadataResult requireMetadata(PlotfileMetadataResult result)
{
    if (!result.metadata) {
        throw std::invalid_argument("dataset metadata is unavailable");
    }
    return result;
}

} // namespace

PlotfileSource::PlotfileSource(
    const std::filesystem::path& path, StopToken cancellation)
    : m_root(sourceDataRoot(path))
    , m_metadataResult(
          requireMetadata(readDatasetMetadata(path, cancellation)))
    , m_particleSpecies(std::filesystem::is_directory(path)
            ? discoverParticleSpecies(m_root, cancellation)
            : std::vector<ParticleSpeciesMetadata>{})
    , m_blockReader(m_root, m_metadataResult.metadata)
    , m_mappedGrid(makeMappedGrid(m_root, m_metadataResult))
{
}

PlotfileSource::PlotfileSource(std::filesystem::path dataRoot,
    PlotfileMetadataResult metadata, StopToken cancellation)
    : m_root(std::move(dataRoot))
    , m_metadataResult(requireMetadata(std::move(metadata)))
    , m_particleSpecies(discoverParticlesForPlotfileRoot(m_root, cancellation))
    , m_blockReader(m_root, m_metadataResult.metadata)
    , m_mappedGrid(makeMappedGrid(m_root, m_metadataResult))
{
}

PlotfileSource::PlotfileSource(MappedGridTag, std::filesystem::path root,
    std::shared_ptr<const DatasetMetadata> metadata)
    : m_root(std::move(root))
    , m_metadataResult{std::move(metadata), {}, {}, {}}
    , m_blockReader(m_root, m_metadataResult.metadata)
{
}

std::shared_ptr<const BlockSource> PlotfileSource::makeMappedGrid(
    const std::filesystem::path& root, const PlotfileMetadataResult& source)
{
    if (!source.mappedGrid) {
        return nullptr;
    }
    return std::shared_ptr<const BlockSource>(
        new PlotfileSource(MappedGridTag{}, root, source.mappedGrid));
}

std::shared_ptr<const DatasetMetadata> PlotfileSource::metadata()
    const noexcept
{
    return m_metadataResult.metadata;
}

const MetadataReadMetrics& PlotfileSource::metadataReadMetrics() const noexcept
{
    return m_metadataResult.metrics;
}

const std::string& PlotfileSource::fileVersion() const noexcept
{
    return m_metadataResult.fileVersion;
}

const std::vector<ParticleSpeciesMetadata>& PlotfileSource::particleSpecies()
    const noexcept
{
    return m_particleSpecies;
}

ParticleSample PlotfileSource::readParticleSample(const std::string& species,
    double fraction, std::uint64_t seed, StopToken cancellation,
    std::size_t maximumPoints, std::optional<ParticleAttribute> attribute) const
{
    return amrvis::readParticleSample(m_root, species, fraction, seed,
        cancellation, maximumPoints, attribute);
}

BlockReadResult PlotfileSource::readBlock(
    const BlockRequest& request, StopToken cancellation) const
{
    return m_blockReader.readBlock(request, cancellation);
}

std::shared_ptr<const BlockSource> PlotfileSource::mappedGrid() const noexcept
{
    return m_mappedGrid;
}

} // namespace amrvis
