#pragma once

#include <amrexplorer/core/DerivedField.hpp>
#include <amrexplorer/data/DatasetPage.hpp>
#include <amrexplorer/data/DatasetSession.hpp>

#include <cstddef>
#include <span>
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
//
// Gathered into one named argument rather than passed as four: fieldCount and
// requestedCount are both counts of the same type, and positionally a caller
// that swapped them would still compile and still satisfy every check on any
// reply whose catalog is larger than its request -- which is every reply that
// installed anything. The siblings above take the request and result objects
// themselves, which is what makes them non-transposable; this one cannot,
// because the reply it checks is a remote:: type and this header does not
// depend on that layer. Naming them does not by itself make leaving one out an
// error: the counts have default initializers, which is what suppresses the
// missing-initializer diagnostic, so an omitted count silently reads as zero
// and passes every check below. Only skips, which has no default, is caught
// that way. What the struct buys is that a swap has to be written down.
struct SessionOpenedDerivedFields {
    // The reply's whole catalog, computed tail included.
    std::size_t fieldCount = 0;
    // How many of that tail the reply claims it installed.
    std::size_t derivedFieldCount = 0;
    // The definitions it reports it could not.
    std::span<const DerivedFieldSkip> skips;
    // How many definitions the request carried.
    std::size_t requestedCount = 0;
};
void validateSessionOpenedDerivedFields(
    const SessionOpenedDerivedFields& reply);

} // namespace amrvis
