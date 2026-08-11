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

} // namespace amrvis
