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
#include <optional>
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

// A VisMF v2 _H body for one box of `componentCount` components stored in
// `fileName`: the shape of a mapped-grid Nu_nd_H (nodal box, two components
// in 2-D), or of anything else a test wants to put beside the Cell_H.
std::string visMfHeaderBody(
    const std::string& box, int componentCount, const std::string& fileName)
{
    return
        "2\n"
        "1\n"
        + std::to_string(componentCount) + "\n"
        "0\n"
        "(1 0\n"
        + box + "\n"
        + ")\n"
          "1\n"
          "FabOnDisk: " + fileName + " 0\n"
          "\n"
          "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))\n";
}

// The trailing extra-MultiFab block ERF writes: one set, one nodal
// displacement component per dimension, one path per level.
std::string mappedGridBlock()
{
    return
        "1\n"
        "2\n"
        "amrexvec_nu_x\n"
        "amrexvec_nu_y\n"
        "Level_0/Nu_nd\n"
        "Level_1/Nu_nd\n";
}

// Writes both levels' Nu_nd_H beside an existing valid plotfile.
void writeMappedGridIndex(const std::filesystem::path& dir, bool nodal = true)
{
    const std::string type = nodal ? "(1,1)" : "(0,0)";
    const std::string upper0 = nodal ? "(8,8)" : "(7,7)";
    const std::string upper1 = nodal ? "(16,16)" : "(15,15)";
    writeFile(dir / "Level_0" / "Nu_nd_H",
        visMfHeaderBody("((0,0) " + upper0 + " " + type + ")", 2, "Nu_nd_D_00000"));
    writeFile(dir / "Level_1" / "Nu_nd_H",
        visMfHeaderBody("((0,0) " + upper1 + " " + type + ")", 2, "Nu_nd_D_00000"));
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

    // Forward compatibility meets the new ceilings: an AMReX Header may carry
    // content past what this reader consumes, and that content is not the
    // reader's to validate. A trailing section holding a line far longer than
    // maximumHeaderLineBytes must therefore still parse -- the bounds apply to
    // fields the parser actually reads, not to the whole file.
    {
        const auto dir = scratch / "valid_extra_long";
        writeValidPlotfile(dir);
        writeFile(dir / "Header",
            validHeaderBody() + std::string(30000, 'z') + "\n");
        try {
            const auto result = amrvis::PlotfileMetadataReader{}.read(dir);
            require(result.metadata->levels.size() == 2,
                "a Header with an over-long trailing line parsed to the wrong shape");
        } catch (const std::exception& error) {
            std::cerr << "FAILED: an over-long line in trailing content the "
                         "reader never consumes was rejected: "
                      << error.what() << '\n';
            ++g_failures;
        }
    }

    // Mapped grids: the extra-MultiFab block ERF and REMORA append after the
    // level data paths. A well-formed block yields the nodal Nu_nd hierarchy
    // as a second metadata; anything wrong with it leaves hasMappedGrid false
    // and the open untouched, since this part of the Header is optional and
    // was never the reader's to reject.
    const auto readMapped = [](const std::filesystem::path& dir, const char* what)
        -> std::optional<amrvis::PlotfileMetadataResult> {
        try {
            return amrvis::PlotfileMetadataReader{}.read(dir);
        } catch (const std::exception& error) {
            std::cerr << "FAILED: " << what << " did not open: " << error.what()
                      << '\n';
            ++g_failures;
            return std::nullopt;
        }
    };
    {
        const auto dir = scratch / "mapped_erf";
        writeValidPlotfile(dir);
        writeMappedGridIndex(dir);
        writeFile(dir / "Header", validHeaderBody() + mappedGridBlock());
        if (const auto result = readMapped(dir, "an ERF-style mapped-grid Header")) {
            require(result->metadata->hasMappedGrid,
                "an ERF-style Nu_nd block did not set hasMappedGrid");
            require(result->mappedGrid != nullptr,
                "an ERF-style Nu_nd block produced no grid metadata");
            require(result->metrics.filesRead == 5,
                "the mapped grid's _H files were not counted in the metrics");
            if (result->mappedGrid) {
                const auto& grid = *result->mappedGrid;
                require(!grid.hasMappedGrid,
                    "the grid metadata must not itself claim a mapped grid");
                require(grid.dimension == 2 && grid.levels.size() == 2,
                    "the grid metadata lost the plotfile's shape");
                require(grid.fields.size() == 2
                        && grid.fields[0].name == "amrexvec_nu_x"
                        && grid.fields[1].name == "amrexvec_nu_y"
                        && grid.fields[0].centering == amrvis::Centering::Node
                        && grid.fields[1].centering == amrvis::Centering::Node,
                    "the grid fields are not the two nodal displacement components");
                const amrvis::IntBox nodalDomain0{{{0, 0, 0}}, {{8, 8, 0}}, {{1, 1, 0}}};
                const amrvis::IntBox nodalDomain1{{{0, 0, 0}}, {{16, 16, 0}}, {{1, 1, 0}}};
                require(grid.levels[0].domain == nodalDomain0
                        && grid.levels[1].domain == nodalDomain1,
                    "the grid level domains are not the nodal domains");
                require(grid.levels[0].dataPath == "Level_0/Nu_nd"
                        && grid.levels[1].dataPath == "Level_1/Nu_nd",
                    "the grid level data paths are not the Nu_nd paths");
                require(grid.levels[0].boxes.size() == 1
                        && grid.levels[0].blocks.size() == 1
                        && grid.levels[0].boxes[0] == nodalDomain0
                        && grid.levels[0].blocks[0].filePath == "Level_0/Nu_nd_D_00000",
                    "the grid blocks were not indexed from Nu_nd_H");
                require(grid.physicalDomain == result->metadata->physicalDomain
                        && grid.levels[0].cellSize == result->metadata->levels[0].cellSize
                        && grid.levels[0].indexOrigin
                            == result->metadata->levels[0].indexOrigin,
                    "the grid geometry differs from the plotfile's");
                require(amrvis::validateMetadata(grid).empty(),
                    "the grid metadata did not validate");
            }
        }
    }
    {
        // REMORA writes a count of 1 and then appends further sets; the Nu_nd
        // set need not come first, and the sets after it are never read.
        const auto dir = scratch / "mapped_remora";
        writeValidPlotfile(dir);
        writeMappedGridIndex(dir);
        writeFile(dir / "Header",
            validHeaderBody()
            + "1\n"
              "6\nh\nstflux_temp\nlrflux\nlhflux\nsrflux\nshflux\n"
              "Level_0/rho2d\nLevel_1/rho2d\n"
              "2\namrexvec_nu_x\namrexvec_nu_y\n"
              "Level_0/Nu_nd\nLevel_1/Nu_nd\n"
              "1\nsustr\nLevel_0/u2d\nLevel_1/u2d\n"
              "1\nsvstr\nLevel_0/v2d\nLevel_1/v2d\n");
        if (const auto result = readMapped(dir, "a REMORA-style mapped-grid Header")) {
            require(result->metadata->hasMappedGrid && result->mappedGrid != nullptr,
                "a REMORA-style Header with several extra sets lost its mapped grid");
        }
    }
    const auto expectNoMappedGrid = [&](const std::filesystem::path& dir,
                                        const std::string& header, bool nodal,
                                        bool writeIndex, const char* what) {
        writeValidPlotfile(dir);
        if (writeIndex) {
            writeMappedGridIndex(dir, nodal);
        }
        writeFile(dir / "Header", header);
        if (const auto result = readMapped(dir, what)) {
            require(!result->metadata->hasMappedGrid && result->mappedGrid == nullptr,
                what);
            require(result->metadata->levels.size() == 2,
                "a rejected mapped-grid block changed the plotfile's own shape");
        }
    };
    expectNoMappedGrid(scratch / "mapped_truncated",
        validHeaderBody() + "1\n2\namrexvec_nu_x\n", true, true,
        "a truncated Nu_nd block must leave hasMappedGrid false");
    expectNoMappedGrid(scratch / "mapped_missing_index",
        validHeaderBody() + mappedGridBlock(), true, false,
        "a Nu_nd block without its _H files must leave hasMappedGrid false");
    expectNoMappedGrid(scratch / "mapped_wrong_count",
        validHeaderBody()
            + "1\n3\namrexvec_nu_x\namrexvec_nu_y\namrexvec_nu_z\n"
              "Level_0/Nu_nd\nLevel_1/Nu_nd\n",
        true, true,
        "a Nu_nd block whose component count is not the dimension must be ignored");
    expectNoMappedGrid(scratch / "mapped_cell_centered",
        validHeaderBody() + mappedGridBlock(), false, true,
        "a Nu_nd_H with cell-centered boxes must be rejected as a mapped grid");
    expectNoMappedGrid(scratch / "mapped_escaping_path",
        validHeaderBody()
            + "1\n2\namrexvec_nu_x\namrexvec_nu_y\n"
              "../Nu_nd\nLevel_1/Nu_nd\n",
        true, true,
        "a Nu_nd path outside the plotfile must be rejected");
    expectNoMappedGrid(scratch / "mapped_other_names",
        validHeaderBody() + "1\n2\nfoo\nbar\nLevel_0/Nu_nd\nLevel_1/Nu_nd\n",
        true, true,
        "an extra set that is not the displacement set must not become a mapped grid");

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

    // Integer fields are bounded too, and stop where they always did. The
    // ceiling matters on libc++, which buffers a digit run rather than
    // accumulating a value, so an enormous one allocates in full there.
    expectHeaderRejected(scratch / "long_component_count",
        "HyperCLaw-V1.1\n" + std::string(5000, '9') + "\ndensity\n",
        "an over-long component count was not rejected",
        "exceeds the supported length");
    // ...and the delimiters are unchanged: a Box packs its integers against
    // commas and parens, which a whitespace-delimited read would swallow. The
    // baseline plotfile above already proves the positive case; this pins the
    // negative one, where the value is followed by punctuation it must not eat.
    expectHeaderRejected(scratch / "negative_component_count",
        "HyperCLaw-V1.1\n-3\ndensity\n",
        "a negative component count was not rejected",
        "component count");

    // Bounding the numeric fields must not widen what they accept. strtod
    // takes "nan", "inf" and an overflowing exponent where operator>> refuses
    // all three, and `time` is not among the fields validateMetadata checks
    // for finiteness -- so without an explicit rejection a Header declaring
    // time = nan would open and carry a NaN into the metadata, the wire
    // catalog and the UI. Non-finite values stay legal only in VisMF block
    // statistics, where AMReX genuinely writes them.
    for (const auto* value : {"nan", "inf", "-inf", "1e999"}) {
        const auto dir = scratch / (std::string("nonfinite_time_") + value);
        expectHeaderRejected(dir,
            "HyperCLaw-V1.1\n1\ndensity\n2\n" + std::string(value) + "\n0\n",
            "a non-finite time field was not rejected",
            "time");
    }
    // The other direction: a denormal is finite and must still parse, which is
    // why the check is isfinite rather than errno -- strtod reports ERANGE for
    // underflow, where the extraction this replaced accepted the value.
    {
        const auto dir = scratch / "denormal_time";
        writeValidPlotfile(dir);
        writeFile(dir / "Header",
            "HyperCLaw-V1.1\n1\ndensity\n2\n4.9e-324\n1\n"
            "0.0\n0.0\n1.0\n1.0\n2\n"
            "((0,0) (7,7) (0,0))\n((0,0) (15,15) (0,0))\n0\n0\n"
            "0.125 0.125\n0.0625 0.0625\n0\n0\n"
            "0 1 0.0 0\n0.0 1.0 0.0 1.0\nLevel_0/Cell\n"
            "1 1 0.0 0\n0.0 1.0 0.0 1.0\nLevel_1/Cell\n");
        try {
            const auto result = amrvis::PlotfileMetadataReader{}.read(dir);
            require(result.metadata->levels.size() == 2,
                "a denormal time field parsed to the wrong shape");
        } catch (const std::exception& error) {
            std::cerr << "FAILED: a denormal time field was rejected: "
                      << error.what() << '\n';
            ++g_failures;
        }
    }

    // Crafted input, bounded rather than allocated: the reader walks this file
    // as text, so without a ceiling one enormous line or token is accumulated
    // whole before any parse can reject it. Kilobyte-scale input then forces an
    // allocation limited only by the file's length -- and in the long-lived
    // server that cost lands on every session, not one process. Each case must
    // fault with a MetadataReadError, never a std::bad_alloc.
    expectHeaderRejected(scratch / "long_version",
        std::string(5000, 'V') + "\n1\ndensity\n2\n0.0\n0\n",
        "an over-long version token was not rejected",
        "exceeds the supported length");
    expectHeaderRejected(scratch / "long_component_name",
        "HyperCLaw-V1.1\n1\n" + std::string(20000, 'c') + "\n",
        "an over-long component-name line was not rejected",
        "header line exceeds the supported length");
    expectHeaderRejected(scratch / "long_data_path",
        headerThroughBoundaryWidth()
            + "0 1 0.0 0\n0.0 1.0 0.0 1.0\n" + std::string(5000, 'p') + "\n",
        "an over-long level data path was not rejected",
        "exceeds the supported length");
    // The unterminated variant, which is the shape the line bound exists for:
    // a line that never ends makes plain getline accumulate the whole rest of
    // the file before the parse can look at it. This trips the ceiling at
    // 16 KiB and never reaches end of input, so it does *not* cover the
    // bound's end-of-input path -- see the unterminated-descriptor case in
    // test_vismf_index for that.
    expectHeaderRejected(scratch / "no_newline",
        "HyperCLaw-V1.1\n1\n" + std::string(30000, 'z'),
        "an unterminated over-long line was not rejected",
        "header line exceeds the supported length");

    // A corrupt cell size is named as such. Every grid record is divided by
    // it, so without the check at the read site a zero was reported as
    // out-of-range grid bounds; inf and nan are already refused by the
    // double reader, and stay refused as the cell-size field.
    const auto withCellSizes = [](const char* level0) {
        return
            "HyperCLaw-V1.1\n1\ndensity\n2\n0.0\n1\n0.0\n0.0\n1.0\n1.0\n2\n"
            "((0,0) (7,7) (0,0))\n((0,0) (15,15) (0,0))\n0\n0\n"
            + std::string(level0) + "\n0.0625 0.0625\n0\n0\n"
            "0 1 0.0 0\n0.0 1.0 0.0 1.0\nLevel_0/Cell\n"
            "1 1 0.0 0\n0.0 1.0 0.0 1.0\nLevel_1/Cell\n";
    };
    expectHeaderRejected(scratch / "zero_cell_size",
        withCellSizes("0.125 0"),
        "a zero cell size was not rejected",
        "cell size must be positive");
    expectHeaderRejected(scratch / "negative_cell_size",
        withCellSizes("-0.125 0.125"),
        "a negative cell size was not rejected",
        "cell size must be positive");
    expectHeaderRejected(scratch / "infinite_cell_size",
        withCellSizes("inf 0.125"),
        "an infinite cell size was not rejected",
        "while reading level cell size");
    expectHeaderRejected(scratch / "nan_cell_size",
        withCellSizes("nan 0.125"),
        "a NaN cell size was not rejected",
        "while reading level cell size");

    std::error_code removeError;
    std::filesystem::remove_all(scratch, removeError);

    if (g_failures != 0) {
        std::cerr << g_failures << " plotfile_header test failure(s)\n";
        return 1;
    }
    return 0;
}
