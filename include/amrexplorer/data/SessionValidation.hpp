#pragma once

#include <amrexplorer/data/DatasetPage.hpp>
#include <amrexplorer/data/DatasetSession.hpp>

#include <string>

namespace amrvis {

void validateSessionViewRequest(const DatasetMetadata& metadata,
    DatasetId dataset, const ViewDataRequest& request);
void validateSessionDatasetPageRequest(const DatasetMetadata& metadata,
    DatasetId dataset, const DatasetPageRequest& request);
void validateSessionRangeRequest(
    const DatasetMetadata& metadata, const RangeRequest& request);
void validateSessionParticleRequest(const DatasetMetadata& metadata,
    const std::vector<ParticleSpeciesMetadata>& species,
    const std::string& name, double fraction);
void validateSessionVolumeRequest(const DatasetMetadata& metadata,
    DatasetId dataset, const VolumeRenderRequest& request);

// Results, checked against the catalog they claim to describe. A local session
// produces its results with our own query code and needs none of this; a remote
// one is reading a peer's answer across a trust boundary, and provenance that is
// impossible for the catalog -- a source level no level has, a grid box on a
// nonexistent level, a plane region with no extent, more sampled particles than
// the species holds -- must be refused rather than stored as trusted result
// state. Each throws std::invalid_argument describing the first violation.
void validateSessionViewResult(const DatasetMetadata& metadata,
    const ViewDataRequest& request, const ViewDataResult& result);
void validateSessionDatasetPageResult(const DatasetMetadata& metadata,
    const DatasetPageRequest& request, const DatasetPage& page);
void validateSessionParticleSampleResult(
    const std::vector<ParticleSpeciesMetadata>& species,
    const std::string& requested, const ParticleSample& sample);
// A rendered frame: exactly the requested size, a range the request allows,
// and sampling metrics the dataset and the request's budget could produce.
void validateSessionVolumeResult(const DatasetMetadata& metadata,
    const VolumeRenderRequest& request, const VolumeFrame& frame);
// The derived-field half of an open reply, which the wire decoder cannot check
// on its own: it does not know how many definitions the request carried. A
// catalog cannot hold more derived fields than fields, cannot report more of
// them than were asked for, and cannot skip a definition that was never sent
// or report more skips than there were definitions -- and a definition cannot
// be both installed and skipped, which is what the counts add up to.
void validateSessionOpenedDerivedFields(std::size_t fieldCount,
    std::uint32_t derivedFieldCount,
    const std::vector<DerivedFieldSkip>& skips, std::size_t requestedCount);

} // namespace amrvis
