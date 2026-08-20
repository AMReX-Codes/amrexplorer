#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/core/Volume.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/query/SliceQuery.hpp>

#include <array>
#include <cstdint>

namespace amrvis {

// Samples a 3-D field over a physical region onto a uniform grid, composing
// the AMR levels the way a slice does (the finest participating level that
// covers a voxel's centre wins), for the volume renderer to ray-cast. The
// grid is bounded by a voxel budget: it is the finest requested level's
// native cell counts over the region, scaled down uniformly when those
// exceed the budget.
struct VolumeQueryResult {
    VolumeGrid grid;
    SliceQueryMetrics metrics;
};

// The grid dimensions a request resolves to: the native cell counts of
// `maximumLevel` (clamped to the finest level) across `region`, each at
// least one, scaled by cbrt(budget / product) and trimmed until the product
// fits `maximumVoxels`. Axes the dataset does not have are one.
//
// Pure and total, so a server can bound a request before validating it: the
// budget is clamped to [1, maxVolumeVoxelBudget], the level is clamped to
// the finest, a single axis is capped at 1e9 native cells (what the int
// conversion holds, and the only cap applied before the scaling), and a
// region that is empty, inverted or non-finite yields one voxel on the
// offending axis rather than a garbage count. Every result satisfies
// dims >= 1 and product(dims) <= maxVolumeVoxelBudget.
[[nodiscard]] std::array<int, 3> volumeGridDims(const DatasetMetadata& metadata,
    const RealBox& region, int maximumLevel, std::uint64_t maximumVoxels);

class VolumeQuery {
public:
    explicit VolumeQuery(PlotfileDataset& dataset)
        : m_dataset(dataset)
    {
    }

    // Reads every block that holds a voxel centre at each participating
    // level -- coarse to fine, each pinned only while it is painted -- and
    // writes each block's cells into the voxels whose centres they contain,
    // so a finer level overwrites a coarser one. A voxel is NaN when no level
    // covers it, and when the value there is not a finite float -- non-finite
    // in the data, or past the range float can represent.
    //
    // Throws std::invalid_argument for a request this dataset cannot serve
    // and ReadCancelled when the token stops. Propagates what reading the
    // data throws: BlockReadError and CacheBudgetExceeded from the dataset
    // (the pipeline's cache-pressure fallback keys on the latter),
    // std::out_of_range from an index outside the level, and
    // std::runtime_error or std::overflow_error from a malformed FAB.
    //
    // The out_of_range is the one a caller can provoke with a bad request:
    // a region is allowed past the domain, but only until a voxel centre
    // lands more than INT_MAX cells from the level's index origin, and past
    // that the throw comes out of sampleIndex rather than as an
    // invalid_argument. Refusing it up front would mean restoring the
    // containment test this deliberately dropped, so a caller that must
    // distinguish a bad request from an internal fault has to catch both.
    [[nodiscard]] VolumeQueryResult execute(
        const VolumeSampleRequest& request, StopToken cancellation = {});

private:
    PlotfileDataset& m_dataset;
};

} // namespace amrvis
