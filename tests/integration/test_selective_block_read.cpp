#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
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

// A minimal 2-D single-component plotfile Header with one small grid. The
// level data path is supplied so a crafted (escaping) path can be exercised;
// the index-space domain box is supplied so a large domain (that can contain
// an oversized _H box) can be exercised. The domain box is a raw index box,
// independent of the grid's physical-bounds -> cell mapping, so the grid stays
// small and valid regardless.
std::string minimalHeader(const std::string& dataPath,
    const std::string& domainBox = "((0,0) (1,1) (0,0))")
{
    return
        "HyperCLaw-V1.1\n"
        "1\nphi\n"
        "2\n0.0\n0\n"
        "0.0 0.0\n1.0 1.0\n\n"
        + domainBox + "\n"
        "0\n0.5 0.5\n0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n"
        + dataPath + "\n";
}

// Writes each value in big-endian (MSB-first) byte order, portably: on a
// little-endian host the native bytes are reversed, on a big-endian host they
// are already MSB-first.
void writeBigEndianDoubles(std::ofstream& out, std::span<const double> values)
{
    for (const double value : values) {
        std::array<unsigned char, sizeof(double)> bytes{};
        std::memcpy(bytes.data(), &value, sizeof(double));
        if constexpr (std::endian::native == std::endian::little) {
            std::reverse(bytes.begin(), bytes.end());
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
}

// A cross-endian (big-endian) FAB must decode to the same values a native-
// endian one does: exercises the byte-swap path of the block decoder, which
// every little-endian test fixture otherwise skips.
void testBigEndianDecode(const std::filesystem::path& base)
{
    const auto root = base / "big_endian";
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header", minimalHeader("Level_0/Cell"));
    // Version-1 _H: one 2x2 box, one component; the FAB header in the data
    // file carries the (big-endian) RealDescriptor read by readFabHeader.
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n"
        "(1 0\n((0,0) (1,1) (0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,1\n1.0,\n\n1,1\n4.0,\n");
    const std::array<double, 4> values{1.0, 2.0, 3.0, 4.0};
    {
        std::ofstream data(root / "Level_0" / "Cell_D_00000", std::ios::binary);
        require(static_cast<bool>(data), "could not create big-endian payload");
        // Ascending byte order (1 2 3 4 5 6 7 8) => big-endian in AMReX's
        // descriptor; parseRealDescriptor maps it to littleEndian == false.
        data << "FAB ((8, (64 11 52 0 1 12 0 1023)),"
                "(8, (1 2 3 4 5 6 7 8)))((0,0) (1,1) (0,0)) 1\n";
        writeBigEndianDoubles(data, values);
    }

    const auto metadata = amrvis::PlotfileMetadataReader{}.read(root);
    amrvis::PlotfileBlockReader reader(root, metadata.metadata);
    amrvis::BlockRequest request;
    request.dataset.value = 1;
    request.field.value = 0;
    const auto result = reader.readBlock(request);
    require(result.block->values.size() == values.size(),
        "big-endian block value count mismatch");
    for (std::size_t i = 0; i < values.size(); ++i) {
        require(result.block->values[i] == values[i],
            "big-endian FAB value decoded incorrectly");
    }
}

// A path that walks above the plotfile directory must be rejected at metadata
// read, not silently followed.
void testRejectsEscapingDataPath(const std::filesystem::path& base)
{
    const auto root = base / "escaping_datapath";
    std::filesystem::create_directories(root);
    writeText(root / "Header", minimalHeader("../escape/Cell"));
    bool threw = false;
    try {
        (void)amrvis::PlotfileMetadataReader{}.read(root);
    } catch (const amrvis::MetadataReadError& error) {
        // Pin the rejection to the path guard (not an incidental "cannot open
        // the _H at that location" failure, which would also throw).
        threw = std::string(error.what()).find("parent-directory")
            != std::string::npos;
    }
    require(threw, "an escaping level data path was not rejected by the path guard");
}

// A box extent larger than the FAB data file must be caught before the read
// buffer is sized, as a clean BlockReadError rather than std::bad_alloc from
// a multi-gigabyte allocation.
void testRejectsOversizedBox(const std::filesystem::path& base)
{
    const auto root = base / "oversized_box";
    std::filesystem::create_directories(root / "Level_0");
    // The index-space domain is large enough to contain the oversized box so
    // the metadata itself validates; only the box-vs-file check should fire.
    writeText(root / "Header",
        minimalHeader("Level_0/Cell", "((0,0) (80000,80000) (0,0))"));
    // Version-2 (NoFabHeader) _H: the box claims 80001 x 80001 cells (~51 GB
    // of doubles), while the data file below holds 16 bytes.
    writeText(root / "Level_0" / "Cell_H",
        "2\n1\n1\n0\n"
        "(1 0\n((0,0) (80000,80000) (0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))\n");
    writeText(root / "Level_0" / "Cell_D_00000", std::string(16, '\0'));

    const auto metadata = amrvis::PlotfileMetadataReader{}.read(root);
    amrvis::PlotfileBlockReader reader(root, metadata.metadata);
    amrvis::BlockRequest request;
    request.dataset.value = 1;
    request.field.value = 0;
    bool threw = false;
    try {
        (void)reader.readBlock(request);
    } catch (const amrvis::BlockReadError& error) {
        // Pin to the up-front size guard's message: the post-read gcount
        // "truncated" check would only fire after the (multi-GB) allocation,
        // so requiring this message proves the allocation was never attempted.
        threw = std::string(error.what()).find("past the end of the data file")
            != std::string::npos;
    }
    require(threw, "an oversized FAB box was not rejected before allocation");
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
    // One cold read is one logical lookup: exactly one miss (the in-lock
    // re-check must not double-count it), and the warm read is exactly one hit.
    require(dataset.cacheMetrics().misses == 1,
        "a cold block read recorded more than one cache miss");
    require(dataset.cacheMetrics().hits == 1,
        "a warm block read did not record exactly one cache hit");

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

    testBigEndianDecode(root);
    testRejectsEscapingDataPath(root);
    testRejectsOversizedBox(root);

    std::filesystem::remove_all(root);
    return 0;
}
