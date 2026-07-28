// Negative- and forward-compat tests for the plotfile Header parser
// (PlotfileMetadataReader). A Header truncated before a required field, or
// with its per-level grid records out of order, must be rejected with a
// MetadataReadError naming the offending field. Conversely, a Header *longer*
// than the reader consumes must still parse: AMReX evolves the format by
// appending fields, and an older reader ignoring the tail is how new writers
// stay readable by old tools.
#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

void writeFile(const std::filesystem::path& path, const std::string& body)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
}

// A well-formed 2-D, single-component, two-level Header through the
// boundary-width line — everything the reader consumes before the per-level
// grid records begin. Truncation cases cut this short; the grid-record cases
// append to it.
std::string headerThroughBoundaryWidth()
{
    return
        "HyperCLaw-V1.1\n"          // file version
        "1\n"                       // component count
        "density\n"                 // component name
        "2\n"                       // dimension
        "0.0\n"                     // time
        "1\n"                       // finest level -> two levels
        "0.0\n0.0\n"                // physical lower bounds (x, y)
        "1.0\n1.0\n"                // physical upper bounds (x, y)
        "2\n"                       // level 0 -> 1 refinement ratio
        "((0,0) (7,7) (0,0))\n"     // level 0 domain
        "((0,0) (15,15) (0,0))\n"   // level 1 domain
        "0\n0\n"                    // per-level steps
        "0.125 0.125\n"             // level 0 cell sizes
        "0.0625 0.0625\n"           // level 1 cell sizes
        "0\n"                       // coordinate system
        "0\n";                      // boundary width
}

// The grid records that complete a valid Header: one grid per level, each
// spanning the whole level domain (so physicalBoundsToCellBox reproduces the
// domain box).
std::string validHeaderBody()
{
    return headerThroughBoundaryWidth()
        + "0 1 0.0 0\n"             // level 0: number, grid count, time, step
          "0.0 1.0 0.0 1.0\n"       // grid 0 bounds: lo_x hi_x lo_y hi_y
          "Level_0/Cell\n"          // level 0 data path
          "1 1 0.0 0\n"             // level 1 record
          "0.0 1.0 0.0 1.0\n"
          "Level_1/Cell\n";
}

// A VisMF v2 _H body describing a single box with one component, matching one
// level of the Header above. Metadata-only reads never touch the _D payload.
std::string cellHeaderBody(const std::string& box)
{
    return
        "2\n"                       // version
        "1\n"                       // file layout
        "1\n"                       // component count
        "0\n"                       // ghost width
        "(1 0\n"                    // BoxArray: one box
        + box + "\n"
        + ")\n"
          "1\n"                     // location count
          "FabOnDisk: Cell_D_00000 0\n"
          "\n"                      // AMReX separator before the descriptor
          "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))\n";
}

// Materializes a complete, metadata-readable plotfile (Header + both levels'
// _H files, no _D payload).
void writeValidPlotfile(const std::filesystem::path& dir)
{
    std::filesystem::create_directories(dir / "Level_0");
    std::filesystem::create_directories(dir / "Level_1");
    writeFile(dir / "Header", validHeaderBody());
    writeFile(dir / "Level_0" / "Cell_H", cellHeaderBody("((0,0) (7,7) (0,0))"));
    writeFile(dir / "Level_1" / "Cell_H", cellHeaderBody("((0,0) (15,15) (0,0))"));
}

// Writes just a (crafted) Header into a fresh directory and asserts the reader
// rejects it with a MetadataReadError whose text contains expectedMessage,
// pinning the rejection to the intended field rather than an incidental
// earlier failure.
void expectHeaderRejected(const std::filesystem::path& dir,
    const std::string& body, const char* what, const char* expectedMessage)
{
    std::filesystem::create_directories(dir);
    writeFile(dir / "Header", body);
    bool threw = false;
    try {
        (void)amrvis::PlotfileMetadataReader{}.read(dir);
    } catch (const amrvis::MetadataReadError& error) {
        threw = true;
        if (std::string(error.what()).find(expectedMessage) == std::string::npos) {
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

} // namespace

int main()
{
    const auto scratch = std::filesystem::temp_directory_path()
        / "amrexplorer_plotfile_header_test";
    std::filesystem::remove_all(scratch);

    // Baseline: the synthetic plotfile the negative and forward-compat cases
    // are built from must itself read cleanly, or nothing below proves anything.
    {
        const auto dir = scratch / "valid";
        writeValidPlotfile(dir);
        const auto result = amrvis::PlotfileMetadataReader{}.read(dir);
        require(result.metadata->dimension == 2, "valid plotfile dimension mismatch");
        require(result.metadata->finestLevel == 1, "valid plotfile finest-level mismatch");
        require(result.metadata->levels.size() == 2, "valid plotfile level count mismatch");
        require(amrvis::validateMetadata(*result.metadata).empty(),
            "the synthetic baseline plotfile did not validate");
    }

    // Forward compatibility: a Header carrying extra trailing content the reader
    // never consumes must still parse. This locks in the leniency that lets new
    // AMReX writers stay readable by old tools; a strict end-of-file check would
    // regress it.
    {
        const auto dir = scratch / "valid_extra";
        writeValidPlotfile(dir);
        writeFile(dir / "Header",
            validHeaderBody()
            + "1 2 3\n"
            + "a future section this reader does not know about\n"
            + "42\n");
        try {
            const auto result = amrvis::PlotfileMetadataReader{}.read(dir);
            require(result.metadata->levels.size() == 2,
                "a Header with extra trailing content parsed to the wrong shape");
        } catch (const std::exception& error) {
            std::cerr << "FAILED: a Header longer than the reader consumes was "
                         "rejected: " << error.what() << '\n';
            ++g_failures;
        }
    }

    // Truncation: the Header ends before a required field. Each case names the
    // first field the reader cannot read.
    expectHeaderRejected(scratch / "trunc_version",
        "HyperCLaw-V1.1\n",
        "a Header truncated after the version was not rejected",
        "component count");
    expectHeaderRejected(scratch / "trunc_finest",
        "HyperCLaw-V1.1\n1\ndensity\n2\n0.0\n1\n",
        "a Header truncated after the finest level was not rejected",
        "physical lower bound");
    expectHeaderRejected(scratch / "trunc_domain",
        "HyperCLaw-V1.1\n1\ndensity\n2\n0.0\n1\n"
        "0.0\n0.0\n1.0\n1.0\n2\n"
        "((0,0) (7,7)",                          // level 0 domain cut mid-box
        "a Header truncated inside a level domain box was not rejected",
        "level domain");
    expectHeaderRejected(scratch / "trunc_grids",
        headerThroughBoundaryWidth(),            // nothing after boundary width
        "a Header truncated before the grid records was not rejected",
        "level number");

    // Out-of-order: the first per-level grid record announces a level number
    // that disagrees with its position.
    expectHeaderRejected(scratch / "out_of_order",
        headerThroughBoundaryWidth() + "1 1 0.0 0\n",  // level 1 record first
        "out-of-order grid records were not rejected",
        "out of order");

    std::error_code removeError;
    std::filesystem::remove_all(scratch, removeError);

    if (g_failures != 0) {
        std::cerr << g_failures << " plotfile_header test failure(s)\n";
        return 1;
    }
    return 0;
}
