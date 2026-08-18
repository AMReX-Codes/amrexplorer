#pragma once

#include <amrexplorer/cache/CacheMetrics.hpp>
#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/Statistics.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/core/Volume.hpp>
#include <amrexplorer/data/DatasetPage.hpp>
#include <amrexplorer/data/ViewData.hpp>
#include <amrexplorer/io/ParticleReader.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace amrvis {

enum class RangeScope : std::uint8_t {
    File,
    Level
};

struct RangeRequest {
    FieldId field;
    int maximumLevel = 0;
    CompositionPolicy composition = CompositionPolicy::FinestAvailable;
    RangeScope scope = RangeScope::File;
};

class DatasetSession {
public:
    virtual ~DatasetSession() = default;

    [[nodiscard]] virtual DatasetId id() const noexcept = 0;
    [[nodiscard]] virtual const DatasetMetadata& metadata() const noexcept = 0;
    [[nodiscard]] virtual const MetadataReadMetrics& metadataReadMetrics()
        const noexcept = 0;
    [[nodiscard]] virtual const std::string& fileVersion() const noexcept = 0;
    [[nodiscard]] virtual const std::vector<ParticleSpeciesMetadata>&
    particleSpecies() const noexcept = 0;
    // The largest encoded response accepted by this session. Local sessions
    // have no transport frame budget; remote sessions expose the negotiated
    // value so request planning can stay below it.
    [[nodiscard]] virtual std::optional<std::uint32_t> maximumResponseBytes()
        const noexcept
    {
        return std::nullopt;
    }

    [[nodiscard]] virtual ViewDataResult requestView(
        const ViewDataRequest& request, StopToken cancellation = {}) = 0;
    [[nodiscard]] virtual DatasetPage requestDatasetPage(
        const DatasetPageRequest& request, StopToken cancellation = {}) = 0;
    [[nodiscard]] virtual std::optional<ValueRange> requestRange(
        const RangeRequest& request, StopToken cancellation = {}) = 0;
    [[nodiscard]] virtual bool rangeAvailable(
        const RangeRequest& request) const noexcept = 0;
    [[nodiscard]] virtual ParticleSample requestParticleSample(
        const std::string& species, double fraction, std::uint64_t seed,
        StopToken cancellation = {}) = 0;

    // Direct volume rendering of a 3-D field (core/Volume.hpp): the session
    // samples the field into a bounded grid, caches it, and ray-casts it to a
    // viewport-sized frame -- locally, or on the server for a remote session,
    // which never receives the sampled field. Not every session can: a
    // standalone FAB, a 2-D dataset, or a peer speaking an older protocol
    // cannot, and supportsVolumeRendering says so before a request is built.
    // The defaults keep every other implementation (and every test fake)
    // untouched.
    [[nodiscard]] virtual bool supportsVolumeRendering() const noexcept
    {
        return false;
    }
    [[nodiscard]] virtual VolumeFrame renderVolume(
        const VolumeRenderRequest& request, StopToken cancellation = {})
    {
        static_cast<void>(request);
        static_cast<void>(cancellation);
        throw std::runtime_error(
            "volume rendering is not supported by this session");
    }

    [[nodiscard]] virtual CacheMetrics cacheMetrics() const = 0;
    [[nodiscard]] virtual bool setCacheBudget(std::uint64_t bytes) = 0;
    virtual void clearUnpinnedCache() = 0;
    virtual void close() noexcept = 0;
};

} // namespace amrvis
