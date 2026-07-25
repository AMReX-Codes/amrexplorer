#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create selective-read fixture text");
    output << text;
}

} // namespace

int main()
{
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("amrexplorer-selective-read-" + std::to_string(unique));
    std::filesystem::create_directories(root / "Level_0");

    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "2\nfirst\nsecond\n"
        "2\n0.0\n0\n"
        "0.0 0.0\n1.0 1.0\n\n"
        "((0,0) (1,1) (0,0))\n"
        "0\n0.5 0.5\n0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n2\n0\n"
        "(1 0\n((0,0) (1,1) (0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,2\n1.0,10.0,\n\n"
        "1,2\n4.0,40.0,\n\n");

    constexpr std::string_view fabHeader =
        "FAB ((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))"
        "((0,0) (1,1) (0,0)) 2\n";
    const std::array<double, 4> first{1.0, 2.0, 3.0, 4.0};
    const std::array<double, 4> second{10.0, 20.0, 30.0, 40.0};
    {
        std::ofstream output(root / "Level_0" / "Cell_D_00000", std::ios::binary);
        require(static_cast<bool>(output), "could not create selective-read payload");
        output.write(fabHeader.data(), static_cast<std::streamsize>(fabHeader.size()));
        output.write(reinterpret_cast<const char*>(first.data()),
            static_cast<std::streamsize>(sizeof(first)));
        output.write(reinterpret_cast<const char*>(second.data()),
            static_cast<std::streamsize>(sizeof(second)));
    }

    const auto standaloneFab = amrvis::StandaloneMetadataReader{}.readFab(
        root / "Level_0" / "Cell_D_00000");
    require(standaloneFab.metadata->dimension == 2,
        "standalone FAB dimension mismatch");
    require(standaloneFab.metadata->fields.size() == 2,
        "standalone FAB component mapping mismatch");
    require(standaloneFab.metrics.payloadFilesRead == 0,
        "standalone FAB metadata read payload values");

    const auto metadataResult = amrvis::PlotfileMetadataReader{}.read(root);
    amrvis::PlotfileBlockReader reader(root, metadataResult.metadata);
    amrvis::BlockRequest request;
    request.dataset.value = 1;
    request.field.value = 1;
    const auto result = reader.readBlock(request);

    require(result.block->values.size() == second.size(), "selective value count mismatch");
    require(result.block->values[0] == 10.0 && result.block->values[3] == 40.0,
        "selective component values mismatch");
    require(result.metrics.filesRead == 1, "selective read opened an unexpected payload count");
    require(result.metrics.valuesRead == 4, "selective value accounting mismatch");
    require(result.metrics.bytesRead == fabHeader.size() + sizeof(second),
        "selective byte accounting mismatch");
    require(result.metrics.bytesRead
            < std::filesystem::file_size(root / "Level_0" / "Cell_D_00000"),
        "selective read accounted for unrelated component bytes");

    amrvis::StopSource stopped;
    stopped.request_stop();
    bool cancelled = false;
    try {
        [[maybe_unused]] auto ignored = reader.readBlock(request, stopped.get_token());
    } catch (const amrvis::ReadCancelled&) {
        cancelled = true;
    }
    require(cancelled, "pre-cancelled block read proceeded");

    amrvis::PlotfileDataset dataset(root, amrvis::DatasetId{7}, 1024 * 1024);
    request.dataset.value = 7;
    auto firstAccess = dataset.requestBlock(request);
    require(!firstAccess.cacheHit && firstAccess.io.bytesRead > 0,
        "first dataset access did not read the block");
    firstAccess.handle = {};
    auto secondAccess = dataset.requestBlock(request);
    require(secondAccess.cacheHit && secondAccess.io.bytesRead == 0,
        "second dataset access did not reuse the cached block");
    require(secondAccess.handle->values[2] == 30.0, "cached block value mismatch");
    require(dataset.cacheMetrics().residentBytes > 0, "dataset cache did not account bytes");

    amrvis::PlotfileDataset fabDataset(
        root / "Level_0" / "Cell_D_00000", amrvis::DatasetId{8}, 1024 * 1024);
    request.dataset.value = 8;
    const auto fabAccess = fabDataset.requestBlock(request);
    require(fabAccess.handle->values[1] == 20.0,
        "standalone FAB selective read value mismatch");

    amrvis::PlotfileDataset multiFabDataset(
        root / "Level_0" / "Cell", amrvis::DatasetId{9}, 1024 * 1024);
    request.dataset.value = 9;
    const auto multiFabAccess = multiFabDataset.requestBlock(request);
    require(multiFabAccess.handle->values[2] == 30.0,
        "standalone MultiFab selective read value mismatch");

    std::filesystem::remove_all(root);
    return 0;
}
