// Regression test for VisMF _H header parsing across header versions 1-3.
// AMReX writes a blank separator line before the RealDescriptor in versions 2
// and 3 (a trailing '\n' after the FabOnDisk list, and after each per-block
// min/max matrix). The reader must skip those blanks and still find the
// descriptor; otherwise v2/v3 plotfiles are unopenable.
#include <amrexplorer/io/detail/VisMfIndex.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr const char* kRealDescriptor =
    "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))";

int g_failures = 0;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

void writeHeader(const std::filesystem::path& path, const std::string& body)
{
    std::ofstream stream(path, std::ios::binary);
    stream << body;
}

// Exercises readVisMfIndex on a synthetic header, returning the parsed index.
// Fails the test (and rethrows) if the reader throws, so a regression that
// reintroduces the missing-RealDescriptor error is caught loudly.
amrvis::detail::VisMfIndex readHeader(
    const std::filesystem::path& path, int dimension, const char* versionLabel)
{
    try {
        return amrvis::detail::readVisMfIndex(path, dimension);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: readVisMfIndex threw for version " << versionLabel
                  << ": " << error.what() << '\n';
        ++g_failures;
        throw;
    }
}

void testVersion1(const std::filesystem::path& path)
{
    // Mirrors the real tests/data plotfile_2d Cell_H: per-block min/max, no
    // RealDescriptor (version 1 does not write one to the _H header).
    writeHeader(path,
        "1\n"                                    // version
        "1\n"                                    // file layout
        "2\n"                                    // ncomp
        "0\n"                                    // ghost width
        "(2 0\n"                                 // BoxArray header (2 boxes)
        "((0,0) (1,3) (0,0))\n"                  // box 0
        "((2,0) (3,3) (0,0))\n"                  // box 1
        ")\n"                                    // BoxArray terminator
        "2\n"                                    // location count
        "FabOnDisk: Cell_D_00000 0\n"
        "FabOnDisk: Cell_D_00000 4096\n"
        "\n"                                     // blank after FabOnDisk list
        "2,2\n"                                  // per-block minima shape
        "0.00000000000000000e+00,1.00000000000000000e+02,\n"
        "2.00000000000000000e+00,1.02000000000000000e+02,\n"
        "\n"                                     // blank after minima
        "2,2\n"                                  // per-block maxima shape
        "1.00000000000000000e+00,1.01000000000000000e+02,\n"
        "3.00000000000000000e+00,1.03000000000000000e+02,\n");
    const auto index = readHeader(path, 2, "1");
    require(index.version == 1, "v1 version mismatch");
    require(index.realDescriptor.empty(), "v1 must not read a RealDescriptor");
    require(index.boxes.size() == 2, "v1 box count mismatch");
    require(index.hasPerBlockStatistics, "v1 should carry per-block statistics");
}

void testVersion2(const std::filesystem::path& path)
{
    // NoFabHeader, no per-block statistics: a single blank separator line sits
    // between the FabOnDisk list and the RealDescriptor.
    writeHeader(path,
        "2\n"
        "1\n"
        "2\n"
        "0\n"
        "(2 0\n"
        "((0,0) (1,3) (0,0))\n"
        "((2,0) (3,3) (0,0))\n"
        ")\n"
        "2\n"
        "FabOnDisk: Cell_D_00000 0\n"
        "FabOnDisk: Cell_D_00000 4096\n"
        "\n"                                     // AMReX separator before descriptor
        "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))\n");
    const auto index = readHeader(path, 2, "2");
    require(index.version == 2, "v2 version mismatch");
    require(index.realDescriptor == kRealDescriptor,
        "v2 RealDescriptor not parsed across the blank separator");
    require(index.boxes.size() == 2, "v2 box count mismatch");
    require(!index.hasPerBlockStatistics, "v2 should not carry per-block statistics");
    const auto metadata = amrvis::StandaloneMetadataReader{}.readMultiFab(path);
    require(!metadata.metadata->hasPhysicalGeometry,
        "standalone MultiFab incorrectly claims physical geometry");
}

void testVersion3(const std::filesystem::path& path)
{
    // NoFabHeader with per-block min/max: a blank separator also follows the
    // maxima matrix, immediately before the RealDescriptor.
    writeHeader(path,
        "3\n"
        "1\n"
        "2\n"
        "0\n"
        "(2 0\n"
        "((0,0) (1,3) (0,0))\n"
        "((2,0) (3,3) (0,0))\n"
        ")\n"
        "2\n"
        "FabOnDisk: Cell_D_00000 0\n"
        "FabOnDisk: Cell_D_00000 4096\n"
        "\n"                                     // blank after FabOnDisk list
        "2,2\n"                                  // per-block minima shape
        "0.00000000000000000e+00,1.00000000000000000e+02,\n"
        "2.00000000000000000e+00,1.02000000000000000e+02,\n"
        "\n"                                     // blank after minima
        "2,2\n"                                  // per-block maxima shape
        "1.00000000000000000e+00,1.01000000000000000e+02,\n"
        "3.00000000000000000e+00,1.03000000000000000e+02,\n"
        "\n"                                     // AMReX separator before descriptor
        "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))\n");
    const auto index = readHeader(path, 2, "3");
    require(index.version == 3, "v3 version mismatch");
    require(index.realDescriptor == kRealDescriptor,
        "v3 RealDescriptor not parsed across the blank separator");
    require(index.boxes.size() == 2, "v3 box count mismatch");
    require(index.hasPerBlockStatistics, "v3 should carry per-block statistics");
    require(index.minimum.size() == 2 && index.maximum.size() == 2,
        "v3 per-block statistics count mismatch");
}

// A crafted header must be rejected with MetadataReadError specifically (not
// std::bad_alloc from over-allocating, and not some other exception). When
// expectedMessage is set, the error text must contain it — this pins the
// rejection to the intended check rather than an incidental parse failure
// (e.g. a truncated body or a missing file).
void expectRejected(const std::filesystem::path& path, const std::string& body,
    int dimension, const char* what, const char* expectedMessage = nullptr)
{
    writeHeader(path, body);
    bool threw = false;
    try {
        (void)amrvis::detail::readVisMfIndex(path, dimension);
    } catch (const amrvis::MetadataReadError& error) {
        threw = true;
        if (expectedMessage != nullptr
            && std::string(error.what()).find(expectedMessage) == std::string::npos) {
            std::cerr << "FAILED: " << what
                      << " was rejected for the wrong reason: " << error.what()
                      << '\n';
            ++g_failures;
            return;
        }
    } catch (const std::exception& other) {
        std::cerr << "FAILED: " << what << " threw the wrong exception: "
                  << other.what() << '\n';
        ++g_failures;
        return;
    }
    require(threw, what);
}

// A well-formed version-1 header prefix (through the FabOnDisk list) with a
// two-box, two-component BoxArray, so only the crafted tail below varies.
constexpr const char* kV1Prefix =
    "1\n1\n2\n0\n"
    "(2 0\n"
    "((0,0) (1,3) (0,0))\n"
    "((2,0) (3,3) (0,0))\n"
    ")\n"
    "2\n"
    "FabOnDisk: Cell_D_00000 0\n"
    "FabOnDisk: Cell_D_00000 4096\n"
    "\n";

void testRejectsOversizedMatrix(const std::filesystem::path& path)
{
    // The per-block minima matrix claims 10^12 entries. The dimensions must
    // be checked against the BoxArray/component count (2 x 2) before the
    // matrix is allocated, so this is a clean MetadataReadError rather than
    // an OOM kill.
    expectRejected(path, std::string(kV1Prefix) + "10000000,100000\n", 2,
        "oversized VisMF statistics matrix was not rejected",
        "do not match");
}

void testRejectsMatrixShapeMismatch(const std::filesystem::path& path)
{
    // A merely wrong (but small) shape is still rejected: AMReX always writes
    // exactly boxCount x components.
    expectRejected(path, std::string(kV1Prefix) + "2,1\n0.0,\n2.0,\n", 2,
        "mismatched VisMF statistics shape was not rejected",
        "do not match");
}

// A well-formed version-2 (NoFabHeader, no statistics matrix) body, whose only
// defect is the crafted first FAB filename: without the path guard it parses
// cleanly, so these tests fail loudly if the guard is removed rather than
// passing on an incidental parse error.
std::string version2Body(const std::string& firstFabName)
{
    return
        "2\n1\n2\n0\n"
        "(2 0\n"
        "((0,0) (1,3) (0,0))\n"
        "((2,0) (3,3) (0,0))\n"
        ")\n"
        "2\n"
        "FabOnDisk: " + firstFabName + " 0\n"
        "FabOnDisk: Cell_D_00000 4096\n"
        "\n"
        "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))\n";
}

void testRejectsRelativeTraversal(const std::filesystem::path& path)
{
    // A FabOnDisk name that walks above the plotfile directory is rejected at
    // parse time (an otherwise-valid v2 header).
    expectRejected(path, version2Body("../escape"), 2,
        "a parent-directory FAB name was not rejected",
        "parent-directory");
}

void testRejectsAbsolutePath(const std::filesystem::path& path)
{
    // An absolute FabOnDisk name would replace the plotfile root entirely.
    expectRejected(path, version2Body("/etc/passwd"), 2,
        "an absolute FAB name was not rejected",
        "relative path");
}

} // namespace

int main()
{
    const auto scratch = std::filesystem::temp_directory_path()
        / "amrexplorer_vismf_index_test";
    std::filesystem::create_directories(scratch);

    testVersion1(scratch / "v1_H");
    testVersion2(scratch / "v2_H");
    testVersion3(scratch / "v3_H");
    testRejectsOversizedMatrix(scratch / "bad_matrix_H");
    testRejectsMatrixShapeMismatch(scratch / "bad_shape_H");
    testRejectsRelativeTraversal(scratch / "traversal_H");
    testRejectsAbsolutePath(scratch / "absolute_H");

    std::error_code removeError;
    std::filesystem::remove_all(scratch, removeError);

    if (g_failures != 0) {
        std::cerr << g_failures << " vismf_index test failure(s)\n";
        return 1;
    }
    return 0;
}
