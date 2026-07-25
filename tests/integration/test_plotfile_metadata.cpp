#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>

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

} // namespace

int main(int argc, char* argv[])
{
    require(argc == 4, "three test data path arguments are required");
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
    require(nonuniform.metadata->levels[0].refinementRatioToNext.values[0] == 2,
        "x refinement ratio mismatch");
    require(nonuniform.metadata->levels[0].refinementRatioToNext.values[1] == 3,
        "y refinement ratio mismatch");
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
    return 0;
}
