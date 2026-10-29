#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/StopToken.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The plotfile's own grid structure, rather than a partition of its leaf
// cells. Blocks are listed by level and then native FAB index, the order the
// level header lists them, and each says its FAB index so output and
// selection match tools that address FABs by plotfile position.

namespace amrvis {

class PlotfileDataset;

struct BlockInfo {
    int level = 0;
    // The native FAB index on its level.
    int grid = 0;
    // The valid index box and its physical bounds.
    IntBox box;
    RealBox bounds;
    std::uint64_t cells = 0;
    // Valid cells the next finer level covers: not leaves.
    std::uint64_t coveredCells = 0;
    // Where the payload is: the FAB's data file, relative to the plotfile,
    // and its byte offset.
    std::string file;
    std::uint64_t offset = 0;
};

// Every block, or one level's. Throws AnalysisError for a missing level.
[[nodiscard]] std::vector<BlockInfo> listBlocks(
    const PlotfileDataset& dataset, std::optional<int> level = std::nullopt);

struct BlockExtrema {
    int level = 0;
    int grid = 0;
    // The box the extrema cover, per field: a stored field's whole on-disk
    // FAB, stored ghost cells included; a derived field's valid box, since
    // derived blocks have no stored ghost cells.
    std::vector<IntBox> boxes;
    std::vector<bool> includesGhostCells;
    // NaN is ignored; a block whose values are all NaN reports NaN for both.
    std::vector<double> minimum;
    std::vector<double> maximum;
};

// Extrema computed from the decoded data, not from the MultiFab header's
// per-FAB minima and maxima: AMReX computes those before converting to the
// stored precision, writes them as text at limited precision, and lets NaN
// propagate. Covered cells count like any other.
[[nodiscard]] std::vector<BlockExtrema> blockStatistics(PlotfileDataset& dataset,
    const std::vector<std::string>& fields, std::optional<int> level = std::nullopt,
    std::optional<int> grid = std::nullopt, StopToken cancellation = {});

} // namespace amrvis
