#include <amrexplorer/data/LocalDatasetSession.hpp>
#include <amrexplorer/io/DatasetOpen.hpp>
#include <amrexplorer/io/OpenPmd.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>
#include <amrexplorer/query/LineQuery.hpp>

#include "OpenPmdFixtures.hpp"

#include <openPMD/openPMD.hpp>

#include <algorithm>
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
using namespace amrvis::test::openpmd;

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

void testIterations(const std::filesystem::path& file)
{
    const auto first = amrvis::readDatasetMetadata(file);
    require(first.iterations
            == std::vector<std::uint64_t>(
                seriesIterations.begin(), seriesIterations.end()),
        "the metadata lists every iteration");
    require(first.metadata->levels[0].step == 10,
        "a series opened without an iteration shows its first");

    const amrvis::DatasetId id{3};
    for (const auto iteration : seriesIterations) {
        auto dataset = amrvis::openDataset({file, iteration}, id, 64ULL << 20U);
        const auto& metadata = dataset->metadata();
        require(metadata.levels[0].step == static_cast<int>(iteration),
            "an iteration opens as itself");
        require(near(metadata.time, 0.1 * static_cast<double>(iteration)),
            "each iteration has its own time");
        const auto access = dataset->requestBlock({id, 0, 0, amrvis::FieldId{0}, 0, 1});
        require(near(access.handle->values[0],
                    seriesRhoAt(iteration, 0.5, 0.5, 0.5)),
            "each iteration reads its own values");
    }

    bool threw = false;
    try {
        static_cast<void>(amrvis::readDatasetMetadata({file, 99}));
    } catch (const amrvis::MetadataReadError&) {
        threw = true;
    }
    require(threw, "an iteration the series lacks is refused");

    threw = false;
    try {
        static_cast<void>(amrvis::readDatasetMetadata(
            {file.parent_path() / "plt00000", 10}));
    } catch (const amrvis::MetadataReadError& error) {
        threw = std::string_view(error.what()).find("no iterations")
            != std::string_view::npos;
    }
    require(threw, "an iteration given with a plotfile is refused");

    amrvis::LocalDatasetSession session({file, 20}, id, 64ULL << 20U);
    require(session.seriesIterations().size() == 3,
        "the session reports the series' iterations");
}

void testParticles(const std::filesystem::path& file)
{
    const amrvis::DatasetId id{4};
    amrvis::LocalDatasetSession session({file, 30}, id, 64ULL << 20U);
    require(session.particleSpecies().size() == 1, "one species");
    const auto& species = session.particleSpecies().front();
    require(species.name == "electrons", "species name");
    require(species.particleCount == seriesParticles, "particle count");
    require(species.dimension == 3, "particle dimension");
    require(species.realComponentNames
            == std::vector<std::string>{
                "momentum_x", "momentum_y", "momentum_z", "weighting"},
        "real components exclude position, positionOffset and id");
    require(species.intComponentNames == std::vector<std::string>{"level"},
        "integer records are int components");

    using Kind = amrvis::ParticleAttribute::Kind;
    const auto all = session.requestParticleSample("electrons", 1.0, 0, {},
        amrvis::ParticleAttribute{Kind::Real, 1});
    require(all.points.size() == seriesParticles, "fraction 1 is every particle");
    for (std::uint64_t p = 0; p < seriesParticles; ++p) {
        const auto& point = all.points[static_cast<std::size_t>(p)];
        require(point.id == seriesParticleId(p), "ids come from the id record");
        for (int axis = 0; axis < 3; ++axis) {
            require(near(point.position[static_cast<std::size_t>(axis)],
                        seriesParticlePosition(30, p, axis)),
                "positions are position + positionOffset");
        }
        require(near(point.value, seriesMomentum(p, 1)),
            "attribute values have unitSI applied");
    }
    const auto levels = session.requestParticleSample("electrons", 1.0, 0, {},
        amrvis::ParticleAttribute{Kind::Int, 0});
    require(near(levels.points[5].value, seriesLevel(5)), "int attribute");

    // Nested and seed-stable, as plotfile sampling is.
    const auto half = session.requestParticleSample("electrons", 0.5, 42);
    const auto most = session.requestParticleSample("electrons", 0.8, 42);
    require(!half.points.empty() && half.points.size() < most.points.size()
            && most.points.size() < seriesParticles,
        "fractions select proportionate samples");
    for (const auto& point : half.points) {
        require(std::any_of(most.points.begin(), most.points.end(),
                    [&point](const auto& other) { return other.id == point.id; }),
            "a smaller fraction is a subset of a larger one");
        require(amrvis::particleSelected(point.id, 0.5, 42),
            "sampling uses the shared hash");
    }

    bool limited = false;
    try {
        static_cast<void>(session.requestParticleSample(
            "electrons", 1.0, 0, std::size_t{10}));
    } catch (const amrvis::ParticleSampleLimitExceeded&) {
        limited = true;
    }
    require(limited, "the point limit is enforced");
}

// Frames of one series share its open handle (OpenPmdSeries) while any of
// them is alive, and nothing keeps it open after. Moving the file away tells
// the two apart: only a series already open can still serve another
// iteration, and once every frame is gone the next open has to find the file.
void testSharedSeries(const std::filesystem::path& source)
{
    const auto file = source.parent_path() / "shared.json";
    const auto moved = source.parent_path() / "moved.json";
    std::filesystem::copy_file(source, file);
    const amrvis::DatasetId id{5};
    {
        auto first = amrvis::openDataset({file, 10}, id, 64ULL << 20U);
        std::filesystem::rename(file, moved);
        auto second = amrvis::openDataset({file, 20}, id, 64ULL << 20U);
        require(second->metadata().levels[0].step == 20,
            "a frame opens through the series another frame holds");
        const auto access = second->requestBlock({id, 0, 0, amrvis::FieldId{0}, 0, 1});
        require(near(access.handle->values[0], seriesRhoAt(20, 0.5, 0.5, 0.5)),
            "the shared series reads the frame's own iteration");
    }
    bool reopened = true;
    try {
        static_cast<void>(amrvis::openDataset({file, 30}, id, 64ULL << 20U));
    } catch (const amrvis::MetadataReadError&) {
        reopened = false;
    }
    require(!reopened, "a series no frame holds is closed, not cached");
}

} // namespace

// With --write-fixture PATH (the 3-D AMR series) or --write-series-fixture
// PATH (three iterations), writes that series to PATH and exits: the
// fixtures the Qt smoke tests open in the real window.
int main(int argc, char* argv[])
{
    if (argc == 3 && std::string_view(argv[1]) == "--write-fixture") {
        std::filesystem::create_directories(
            std::filesystem::path(argv[2]).parent_path());
        writeAmr(argv[2]);
        return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--write-series-fixture") {
        std::filesystem::create_directories(
            std::filesystem::path(argv[2]).parent_path());
        writeSeries(argv[2]);
        return 0;
    }
    const auto directory = std::filesystem::temp_directory_path()
        / ("amrexplorer-openpmd-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(directory);
    const auto amr = directory / "amr.json";
    const auto transposed = directory / "transposed.json";
    writeAmr(amr);
    writeTransposed(transposed);
    const auto seriesFile = directory / "series.json";
    writeSeries(seriesFile);

    testPathDetection();
    testMetadata(amr);
    testBlocks(amr);
    testSessionAndLine(amr);
    testTransposed(transposed);
    testRejection(directory);
    testIterations(seriesFile);
    testParticles(seriesFile);
    testSharedSeries(seriesFile);

    std::filesystem::remove_all(directory);
    std::cout << "openPMD reader tests passed\n";
    return 0;
}
