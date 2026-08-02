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

} // namespace amrvis
