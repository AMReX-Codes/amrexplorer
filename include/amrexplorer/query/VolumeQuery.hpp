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
struct VolumeSampleRequest {
    DatasetId dataset;
    FieldId field;
    int component = 0;
    int maximumLevel = 0;
    CompositionPolicy composition = CompositionPolicy::FinestAvailable;
    RealBox region;
    std::uint64_t maximumVoxels = defaultVolumeVoxelBudget;
};

struct VolumeQueryResult {
    VolumeGrid grid;
    SliceQueryMetrics metrics;
};

// The grid dimensions a request resolves to: the native cell counts of
// `maximumLevel` (clamped to the finest level) across `region`, each at
// least one, scaled by cbrt(budget / product) and trimmed until the product
// fits `maximumVoxels`. Pure, so the server can bound a request with it.
[[nodiscard]] std::array<int, 3> volumeGridDims(const DatasetMetadata& metadata,
    const RealBox& region, int maximumLevel, std::uint64_t maximumVoxels);

class VolumeQuery {
public:
    explicit VolumeQuery(PlotfileDataset& dataset)
        : m_dataset(dataset)
    {
    }

    // Reads every block that intersects the region at each participating
    // level -- coarse to fine, each pinned only while it is painted -- and
    // writes each block's cells into the voxels whose centres they contain,
    // so a finer level overwrites a coarser one. Voxels no level covers, and
    // non-finite values, are NaN. Throws std::invalid_argument for a request
    // this dataset cannot serve and ReadCancelled when the token stops.
    [[nodiscard]] VolumeQueryResult execute(
        const VolumeSampleRequest& request, StopToken cancellation = {});

private:
    PlotfileDataset& m_dataset;
};

} // namespace amrvis
