#include <amrexplorer/data/LocalDatasetSession.hpp>
#include <amrexplorer/io/DatasetOpen.hpp>
#include <amrexplorer/io/OpenPmd.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>
#include <amrexplorer/query/LineQuery.hpp>

#include <openPMD/openPMD.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// The openPMD reader against series this test writes itself with the JSON
// backend, which every openPMD-api build has. Every field is linear in
// position, so a staggered component interpolated to the cell centres must
// reproduce the field there exactly (to rounding).

namespace {

namespace io = openPMD;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool near(double a, double b)
{
    return std::abs(a - b) <= 1e-9 * (1.0 + std::abs(b));
}

double rhoAt(double x, double y, double z)
{
    return 1.0 + 2.0 * x + 3.0 * y + 5.0 * z;
}

double eAt(int component, double x, double y, double z)
{
    return 7.0 * (component + 1) - x + 4.0 * y + 0.5 * z;
}

// The 3-D series: level 0 is 8^3 cells of size 1 from (-4, 0, 10); level 1
// (WarpX's _lvl1 meshes) is the same domain at half the spacing, written only
// over cells [4, 11]^3. rho is cell-centred and stored at half its value with
// unitSI 2; E's components each sit on the lower face along their own axis.
// Arrays are (z, y, x), x fastest, as WarpX writes them.
constexpr std::array<double, 3> origin{-4.0, 0.0, 10.0};

void writeMesh(io::Iteration& iteration, const std::string& name,
    double spacing, int cells, int lower, int upper, bool vector)
{
    auto mesh = iteration.meshes[name];
    mesh.setGeometry(io::Mesh::Geometry::cartesian);
    mesh.setDataOrder(io::Mesh::DataOrder::C);
    mesh.setAxisLabels({"z", "y", "x"});
    mesh.setGridSpacing(std::vector<double>{spacing, spacing, spacing});
    mesh.setGridGlobalOffset({origin[2], origin[1], origin[0]});
    mesh.setGridUnitSI(1.0);
    const std::vector<std::string> keys = vector
        ? std::vector<std::string>{"x", "y", "z"}
        : std::vector<std::string>{io::RecordComponent::SCALAR};
    for (std::size_t c = 0; c < keys.size(); ++c) {
        auto rc = mesh[keys[c]];
        // By array dimension (z, y, x): which axis this component is
        // staggered along, if any.
        std::array<double, 3> position{0.5, 0.5, 0.5};
        std::array<std::uint64_t, 3> extent{static_cast<std::uint64_t>(cells),
            static_cast<std::uint64_t>(cells), static_cast<std::uint64_t>(cells)};
        std::array<int, 3> last{upper, upper, upper};
        if (vector) {
            const auto dim = 2 - c;
            position[dim] = 0.0;
            extent[dim] += 1;
            last[dim] += 1;
        }
        rc.setPosition(std::vector<double>(position.begin(), position.end()));
        rc.setUnitSI(vector ? 1.0 : 2.0);
        rc.resetDataset({io::Datatype::DOUBLE, {extent[0], extent[1], extent[2]}});
        std::vector<double> values;
        for (int k = lower; k <= last[0]; ++k) {
            for (int j = lower; j <= last[1]; ++j) {
                for (int i = lower; i <= last[2]; ++i) {
                    const double x = origin[0] + (i + position[2]) * spacing;
                    const double y = origin[1] + (j + position[1]) * spacing;
                    const double z = origin[2] + (k + position[0]) * spacing;
                    values.push_back(vector ? eAt(static_cast<int>(c), x, y, z)
                                            : 0.5 * rhoAt(x, y, z));
                }
            }
        }
        const auto n = [&](std::size_t dim) {
            return static_cast<std::uint64_t>(last[dim] - lower + 1);
        };
        rc.storeChunk(values,
            {static_cast<std::uint64_t>(lower), static_cast<std::uint64_t>(lower),
                static_cast<std::uint64_t>(lower)},
            {n(0), n(1), n(2)});
        // storeChunk keeps only a view of `values` until the next flush.
        iteration.seriesFlush();
    }
}

void writeAmr(const std::filesystem::path& file)
{
    io::Series series(file.string(), io::Access::CREATE);
    auto iteration = series.iterations[100];
    iteration.setTime(2.0);
    iteration.setTimeUnitSI(0.5);
    writeMesh(iteration, "rho", 1.0, 8, 0, 7, false);
    writeMesh(iteration, "E", 1.0, 8, 0, 7, true);
    writeMesh(iteration, "rho_lvl1", 0.5, 16, 4, 11, false);
    writeMesh(iteration, "E_lvl1", 0.5, 16, 4, 11, true);
    series.flush();
}

// 2-D, single precision, labelled (x, z) with x the *slowest* array
// dimension -- the opposite of AMReX's layout, so the reader must transpose.
// x spacing 1 from 0, z spacing 2 from 100.
double phiAt(double x, double z)
{
    return x + 1000.0 * z;
}

void writeTransposed(const std::filesystem::path& file)
{
    io::Series series(file.string(), io::Access::CREATE);
    auto iteration = series.iterations[0];
    auto mesh = iteration.meshes["phi"];
    mesh.setGeometry(io::Mesh::Geometry::cartesian);
    mesh.setAxisLabels({"x", "z"});
    mesh.setGridSpacing(std::vector<double>{1.0, 2.0});
    mesh.setGridGlobalOffset({0.0, 100.0});
    auto rc = mesh[io::RecordComponent::SCALAR];
    rc.setPosition(std::vector<double>{0.5, 0.5});
    rc.resetDataset({io::Datatype::FLOAT, {4, 6}});
    std::vector<float> values;
    for (int i = 0; i < 4; ++i) {
        for (int k = 0; k < 6; ++k) {
            values.push_back(static_cast<float>(phiAt(i + 0.5, 100.0 + 2.0 * (k + 0.5))));
        }
    }
    rc.storeChunk(values, {0, 0}, {4, 6});
    series.flush();
}

double cellCentre(const amrvis::LevelMetadata& level, int axis, int index)
{
    const auto i = static_cast<std::size_t>(axis);
    return level.indexOrigin[i] + (index + 0.5) * level.cellSize[i];
}

void testPathDetection()
{
    using amrvis::isOpenPmdSeriesPath;
    require(isOpenPmdSeriesPath("run/diags.h5"), ".h5 is openPMD");
    require(isOpenPmdSeriesPath("run/diags.bp/"), "a .bp directory is openPMD");
    require(isOpenPmdSeriesPath("run/diag_%T.bp5"), "a %T pattern is openPMD");
    require(isOpenPmdSeriesPath("series.json"), ".json is openPMD");
    require(!isOpenPmdSeriesPath("run/plt00010"), "a plotfile is not openPMD");
    require(!isOpenPmdSeriesPath("run/plt00010/Level_0/Cell_H"),
        "a MultiFab header is not openPMD");
    require(amrvis::openPmdSupported(), "this test only builds with openPMD");
}

void testMetadata(const std::filesystem::path& file)
{
    const auto result = amrvis::readDatasetMetadata(file);
    require(result.source != nullptr, "the result carries the open series");
    require(result.fileVersion.starts_with("openPMD "), "file version names openPMD");
    const auto& metadata = *result.metadata;
    require(metadata.dimension == 3, "dimension");
    require(metadata.finestLevel == 1, "two levels from _lvl1");
    require(near(metadata.time, 1.0), "time is in SI");
    require(metadata.fields.size() == 4, "rho and three E components");
    const std::array<std::string, 4> names{"E_x", "E_y", "E_z", "rho"};
    for (std::size_t f = 0; f < names.size(); ++f) {
        require(metadata.fields[f].name == names[f], "field " + names[f]);
        require(metadata.fields[f].centering == amrvis::Centering::Cell,
            "fields are presented cell-centred");
    }
    const auto& coarse = metadata.levels[0];
    require(coarse.step == 100, "step is the iteration index");
    for (int axis = 0; axis < 3; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        require(coarse.domain.lower[i] == 0 && coarse.domain.upper[i] == 7,
            "level 0 has 8 cells per axis (not the staggered 9)");
        require(near(coarse.cellSize[i], 1.0), "level 0 spacing");
        require(near(coarse.indexOrigin[i], origin[i]),
            "axes are mapped from (z, y, x) labels");
        require(near(metadata.physicalDomain.lower[i], origin[i])
                && near(metadata.physicalDomain.upper[i], origin[i] + 8.0),
            "physical domain");
    }
    require(coarse.boxes.size() == 1, "level 0 is one written chunk");

    const auto& fine = metadata.levels[1];
    require(near(fine.cellSize[0], 0.5) && fine.domain.upper[0] == 15,
        "level 1 is the domain at half the spacing");
    std::uint64_t cells = 0;
    for (const auto& box : fine.boxes) {
        for (int axis = 0; axis < 3; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            require(box.lower[i] >= 4 && box.upper[i] <= 11,
                "level 1 boxes cover only what was written");
        }
        cells += static_cast<std::uint64_t>(box.upper[0] - box.lower[0] + 1)
            * static_cast<std::uint64_t>(box.upper[1] - box.lower[1] + 1)
            * static_cast<std::uint64_t>(box.upper[2] - box.lower[2] + 1);
    }
    require(cells == 8 * 8 * 8, "level 1 boxes cover the written region once");
}

void testBlocks(const std::filesystem::path& file)
{
    auto result = amrvis::readDatasetMetadata(file);
    const amrvis::DatasetId id{1};
    auto dataset = amrvis::openDataset(
        file.parent_path(), id, 64ULL << 20U, std::move(result));
    const auto& metadata = dataset->metadata();
    for (int levelIndex = 0; levelIndex <= 1; ++levelIndex) {
        const auto& level = metadata.levels[static_cast<std::size_t>(levelIndex)];
        for (std::uint32_t field = 0; field < 4; ++field) {
            for (std::size_t grid = 0; grid < level.blocks.size(); ++grid) {
                const auto access = dataset->requestBlock({id, levelIndex,
                    static_cast<int>(grid), amrvis::FieldId{field}, 0, 1});
                const auto& block = *access.handle;
                const auto& box = level.blocks[grid].box;
                require(block.box == box, "a block is its grid's valid box");
                std::size_t at = 0;
                for (int k = box.lower[2]; k <= box.upper[2]; ++k) {
                    for (int j = box.lower[1]; j <= box.upper[1]; ++j) {
                        for (int i = box.lower[0]; i <= box.upper[0]; ++i) {
                            const auto x = cellCentre(level, 0, i);
                            const auto y = cellCentre(level, 1, j);
                            const auto z = cellCentre(level, 2, k);
                            const auto expected = field < 3
                                ? eAt(static_cast<int>(field), x, y, z)
                                : rhoAt(x, y, z);
                            require(near(block.values[at++], expected),
                                "level " + std::to_string(levelIndex) + " "
                                    + metadata.fields[field].name
                                    + " matches the field at cell centres");
                        }
                    }
                }
            }
        }
    }
}

void testSessionAndLine(const std::filesystem::path& file)
{
    // The path constructor, as the server and a plain open use it.
    const amrvis::DatasetId id{7};
    amrvis::LocalDatasetSession session(file, id, 64ULL << 20U);
    require(session.metadata().fields.size() == 4, "the session sees the fields");
    require(session.fileVersion().starts_with("openPMD "), "session file version");
    require(session.particleSpecies().empty(), "no particles yet");

    // A line along x through cells that are level-1 centres, composing both
    // levels: the refined stretch (x in [-2, 2]) comes from level 1.
    auto dataset = amrvis::openDataset(file, id, 64ULL << 20U);
    amrvis::LineRequest request;
    request.dataset = id;
    request.field = amrvis::FieldId{3};
    request.axis = 0;
    request.fixedCoordinates = {0.0, 4.25, 14.25};
    request.maximumLevel = 1;
    const auto line = amrvis::LineQuery(*dataset).execute(request).line;
    bool sawFine = false;
    bool sawCoarse = false;
    for (std::size_t s = 0; s < line.values.size(); ++s) {
        require(line.valid[s] != 0, "every sample on the line is covered");
        const auto x = line.positions[s];
        const bool fine = line.sourceLevel[s] == 1;
        require(fine == (x > -2.0 && x < 2.0),
            "level 1 supplies exactly the refined stretch");
        sawFine = sawFine || fine;
        sawCoarse = sawCoarse || !fine;
        const auto expected = fine ? rhoAt(x, 4.25, 14.25) : rhoAt(x, 4.5, 14.5);
        require(near(line.values[s], expected), "line values");
    }
    require(sawFine && sawCoarse, "the line crosses both levels");
}

void testTransposed(const std::filesystem::path& file)
{
    const amrvis::DatasetId id{2};
    auto dataset = amrvis::openDataset(file, id, 64ULL << 20U);
    const auto& metadata = dataset->metadata();
    require(metadata.dimension == 2, "2-D series");
    const auto& level = metadata.levels[0];
    require(level.domain.upper[0] == 3 && level.domain.upper[1] == 5,
        "x has 4 cells and z has 6, whatever the array order");
    require(near(level.cellSize[1], 2.0) && near(level.indexOrigin[1], 100.0),
        "z keeps its own spacing and offset");
    const auto access = dataset->requestBlock({id, 0, 0, amrvis::FieldId{0}, 0, 1});
    require(access.handle->values.precision() == amrvis::FabRealPrecision::Single,
        "single precision stays single");
    std::size_t at = 0;
    for (int k = 0; k <= 5; ++k) {
        for (int i = 0; i <= 3; ++i) {
            const auto expected = phiAt(cellCentre(level, 0, i), cellCentre(level, 1, k));
            require(std::abs(access.handle->values[at++] - expected) <= 1e-6 * expected,
                "a block is x-fastest whatever the array order");
        }
    }
}

void testRejection(const std::filesystem::path& directory)
{
    const auto empty = directory / "empty.json";
    {
        io::Series series(empty.string(), io::Access::CREATE);
        series.iterations[0].setTime(0.0);
        series.flush();
    }
    bool threw = false;
    try {
        static_cast<void>(amrvis::readDatasetMetadata(empty));
    } catch (const amrvis::MetadataReadError&) {
        threw = true;
    }
    require(threw, "a series without meshes is refused as metadata");

    const auto garbage = directory / "garbage.json";
    std::ofstream(garbage) << "not json";
    threw = false;
    try {
        static_cast<void>(amrvis::readDatasetMetadata(garbage));
    } catch (const amrvis::MetadataReadError&) {
        threw = true;
    }
    require(threw, "an unreadable file is refused as metadata");
}

} // namespace

// With --write-fixture PATH, writes the 3-D AMR series to PATH and exits:
// the fixture the Qt smoke test (qt_openpmd_open) opens in the real window.
int main(int argc, char* argv[])
{
    if (argc == 3 && std::string_view(argv[1]) == "--write-fixture") {
        std::filesystem::create_directories(
            std::filesystem::path(argv[2]).parent_path());
        writeAmr(argv[2]);
        return 0;
    }
    const auto directory = std::filesystem::temp_directory_path()
        / ("amrexplorer-openpmd-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(directory);
    const auto amr = directory / "amr.json";
    const auto transposed = directory / "transposed.json";
    writeAmr(amr);
    writeTransposed(transposed);

    testPathDetection();
    testMetadata(amr);
    testBlocks(amr);
    testSessionAndLine(amr);
    testTransposed(transposed);
    testRejection(directory);

    std::filesystem::remove_all(directory);
    std::cout << "openPMD reader tests passed\n";
    return 0;
}
