#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool nearlyEqual(double a, double b)
{
    return std::fabs(a - b) <= 1.0e-9 * std::max({1.0, std::fabs(a), std::fabs(b)});
}

} // namespace

int main(int argc, char* argv[])
{
    require(argc == 5, "four test data path arguments are required");
    const std::filesystem::path plotfile(argv[1]);
    require(!std::filesystem::exists(plotfile / "Level_0" / "Cell"),
        "metadata-only fixture must not contain field data");

    const auto result = amrvis::PlotfileMetadataReader{}.read(plotfile);
    const auto& metadata = *result.metadata;

    require(result.fileVersion == "HyperCLaw-V1.1", "file version mismatch");
    require(result.metrics.filesRead == 3, "opening metadata read an unexpected file count");
    const auto expectedBytes = std::filesystem::file_size(plotfile / "Header")
        + std::filesystem::file_size(plotfile / "Level_0" / "Cell_H")
        + std::filesystem::file_size(plotfile / "Level_1" / "Cell_H");
    require(result.metrics.bytesRead == expectedBytes,
        "metadata byte accounting mismatch");
    require(result.metrics.payloadFilesRead == 0, "opening metadata read a FAB payload file");
    require(result.metrics.payloadBytesRead == 0, "opening metadata read FAB payload bytes");
    require(metadata.dimension == 2, "dimension mismatch");
    require(metadata.finestLevel == 1, "finest level mismatch");
    require(metadata.fields.size() == 2, "field count mismatch");
    require(metadata.fields[0].name == "density", "first field mismatch");
    require(metadata.levels.size() == 2, "level count mismatch");
    require(metadata.levels[0].boxes.size() == 2, "coarse grid count mismatch");
    require(metadata.levels[1].boxes.size() == 1, "fine grid count mismatch");
    require(metadata.levels[0].blocks.size() == 2, "coarse block index mismatch");
    require(metadata.levels[0].blocks[1].fileOffset == 4096, "block offset mismatch");
    require(metadata.levels[0].blocks[0].statistics.has_value(), "block statistics missing");
    require(metadata.levels[0].blocks[0].statistics->minimum[1] == 100.0,
        "block minimum mismatch");
    require(metadata.levels[0].boxes[1].lower.values[0] == 2,
        "physical-to-index conversion mismatch");
    require(metadata.levels[1].domain.upper.values[1] == 7, "fine domain mismatch");
    require(amrvis::validateMetadata(metadata).empty(), "parsed metadata is invalid");

    const auto nonuniform = amrvis::PlotfileMetadataReader{}.read(argv[2]);
    // Anisotropic 2:3 refinement between levels 0 and 1. Visualization relates
    // levels through per-level cell geometry (see SliceQuery), so the finer
    // level's cell size is the coarser's divided by the per-axis ratio; that
    // is what the reader must get right.
    require(nonuniform.metadata->levels.size() == 2,
        "nonuniform plotfile should have two levels");
    const auto& coarse = nonuniform.metadata->levels[0];
    const auto& fine = nonuniform.metadata->levels[1];
    require(nearlyEqual(coarse.cellSize.values[0], 2.0 * fine.cellSize.values[0]),
        "x refinement (cell-size ratio) mismatch");
    require(nearlyEqual(coarse.cellSize.values[1], 3.0 * fine.cellSize.values[1]),
        "y refinement (cell-size ratio) mismatch");
    require(nonuniform.metrics.payloadFilesRead == 0,
        "nonuniform metadata read a FAB payload file");

    const auto standaloneMultiFab = amrvis::StandaloneMetadataReader{}.readMultiFab(
        plotfile / "Level_0" / "Cell");
    require(standaloneMultiFab.metadata->dimension == 2,
        "standalone MultiFab dimension mismatch");
    require(standaloneMultiFab.metadata->fields.size() == 2,
        "standalone MultiFab component mapping mismatch");
    require(standaloneMultiFab.metadata->levels[0].boxes.size() == 2,
        "standalone MultiFab BoxArray mismatch");
    require(standaloneMultiFab.metrics.payloadFilesRead == 0,
        "standalone MultiFab metadata read payload data");

    const auto threeDimensional = amrvis::PlotfileMetadataReader{}.read(argv[3]);
    require(threeDimensional.metadata->dimension == 3,
        "the same metadata reader did not accept a 3-D dataset");
    require(threeDimensional.metadata->levels[0].domain.upper.values[2] == 3,
        "3-D domain mismatch");
    require(threeDimensional.metrics.payloadFilesRead == 0,
        "3-D metadata read a FAB payload file");
    require(!threeDimensional.metadata->hasMappedGrid
            && threeDimensional.mappedGrid == nullptr,
        "a plotfile without a Nu_nd block reports a mapped grid");

    // The mapped-grid fixture: the same 3-D plotfile with the ERF-style Nu_nd
    // block. The grid hierarchy is nodal, spans the domain with the two boxes
    // of its own _H, and shares the plotfile's geometry.
    const auto mapped = amrvis::PlotfileMetadataReader{}.read(argv[4]);
    require(mapped.metadata->hasMappedGrid && mapped.mappedGrid != nullptr,
        "the mapped-grid fixture did not report its mapped grid");
    require(mapped.metrics.filesRead == 3,
        "the mapped-grid fixture's Nu_nd_H was not counted in the metrics");
    const auto& grid = *mapped.mappedGrid;
    require(grid.dimension == 3 && grid.levels.size() == 1 && grid.fields.size() == 3,
        "mapped-grid fixture grid shape mismatch");
    require(grid.fields[2].name == "amrexvec_nu_z"
            && grid.fields[2].centering == amrvis::Centering::Node,
        "mapped-grid fixture field mismatch");
    const auto& gridLevel = grid.levels[0];
    require(gridLevel.domain.upper.values[0] == 4
            && gridLevel.domain.upper.values[2] == 4
            && gridLevel.domain.centering.values[0] == 1
            && gridLevel.domain.centering.values[2] == 1,
        "mapped-grid fixture nodal domain mismatch");
    require(gridLevel.boxes.size() == 2 && gridLevel.blocks.size() == 2
            && gridLevel.boxes[1].lower.values[0] == 2
            && gridLevel.blocks[1].filePath == "Level_0/Nu_nd_D_00001",
        "mapped-grid fixture block index mismatch");
    require(gridLevel.blocks[1].statistics.has_value()
            && gridLevel.blocks[1].statistics->maximum.size() == 3
            && gridLevel.blocks[1].statistics->maximum[2] == 0.125,
        "mapped-grid fixture block statistics mismatch");
    require(gridLevel.cellSize == mapped.metadata->levels[0].cellSize
            && grid.physicalDomain == mapped.metadata->physicalDomain,
        "mapped-grid fixture geometry differs from the plotfile's");
    require(amrvis::validateMetadata(grid).empty(),
        "mapped-grid fixture grid metadata is invalid");
    return 0;
}
