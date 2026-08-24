// A derived field end to end: installed by the dataset, evaluated per block,
// and read back through the ordinary slice, line and range paths. The point of
// the whole design is that nothing above PlotfileDataset knows the difference,
// so the assertions compare a derived field's results against the same query
// run on the fields it is computed from.
#include <amrexplorer/data/LocalDatasetSession.hpp>
#include <amrexplorer/query/LineQuery.hpp>
#include <amrexplorer/query/SliceQuery.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

constexpr std::string_view realDescriptor =
    "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))";

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

// Samples travel as float, and the derived field evaluates in double before
// narrowing, so an expectation rebuilt from the narrowed inputs agrees only to
// float precision. Everything compared here has been through a plane or a line
// result, so this is the tolerance throughout.
bool close(double left, double right)
{
    const auto scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= 1.0e-6 * scale;
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create fixture text");
    output << text;
}

// One multi-component FAB: the components follow one another, each laid out
// i-fastest over the box.
void writeFab(const std::filesystem::path& path, std::string_view box,
    const std::vector<std::vector<double>>& components)
{
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create fixture FAB");
    output << "FAB " << realDescriptor << box << " " << components.size()
           << '\n';
    for (const auto& values : components) {
        output.write(reinterpret_cast<const char*>(values.data()),
            static_cast<std::streamsize>(values.size() * sizeof(double)));
    }
}

// Cells per axis on the coarse level. Deliberately more than one evaluation
// chunk holds (derivedChunkPoints in PlotfileDataset.cpp), so a derived block
// is computed over several passes and the seams between them are covered; a
// block that fits in one pass hides every indexing mistake across them. The
// count keeps both levels' cell sizes exact in binary.
constexpr int coarseCells = 32;

// The fixture's three stored fields, as functions of the cell index, so the
// expectations below are written in the same terms.
double densityAt(int i, int j)
{
    return 1.0 + static_cast<double>(i)
        + static_cast<double>(coarseCells) * static_cast<double>(j);
}
double temperatureAt(int i, int j)
{
    return 10.0 + static_cast<double>(i) - static_cast<double>(j);
}
double momentumAt(int i, int j)
{
    return -(static_cast<double>(i) + static_cast<double>(j)) - 0.5;
}

std::vector<std::vector<double>> fabComponents(
    int lowI, int lowJ, double offset)
{
    std::vector<std::vector<double>> components(3);
    for (int j = lowJ; j < lowJ + coarseCells; ++j) {
        for (int i = lowI; i < lowI + coarseCells; ++i) {
            components[0].push_back(densityAt(i, j) + offset);
            components[1].push_back(temperatureAt(i, j) + offset);
            components[2].push_back(momentumAt(i, j) + offset);
        }
    }
    return components;
}

std::string visMfHeader(std::string_view box)
{
    return std::string("1\n1\n3\n0\n(1 0\n") + std::string(box)
        + "\n)\n1\nFabOnDisk: Cell_D_00000 0\n\n"
        // Statistics for the three stored components. Deliberately wide
        // enough to cover the payload; the derived fields have none, which is
        // what makes their File range unavailable.
        "1,3\n-10000.0,-10000.0,-10000.0,\n\n"
        "1,3\n10000.0,10000.0,10000.0,\n\n";
}

amrvis::SliceRequest sliceRequest(std::uint32_t field, int maximumLevel)
{
    amrvis::SliceRequest request;
    request.dataset.value = 7;
    request.field.value = field;
    request.normalDirection = 1;
    request.visibleRegion = {{{0.0, 0.0, 0.0}}, {{1.0, 1.0, 0.0}}};
    request.maximumLevel = maximumLevel;
    // One output sample per coarse cell, so a level-0 plane index maps
    // straight onto a cell index (which the coordinate check below relies on).
    request.outputSize = {coarseCells, coarseCells};
    return request;
}

// The derived-field definitions the fixture is opened with. Their ids follow
// the three stored fields, in this order.
const std::vector<amrvis::DerivedFieldDefinition>& definitions()
{
    static const std::vector<amrvis::DerivedFieldDefinition> list{
        {"speed", "sqrt(density**2 + temperature**2)"},
        {"drag", "-${x-momentum} / density"},
        {"doubled", "2*speed"},
        {"radius", "sqrt(x**2 + y**2)"},
    };
    return list;
}

constexpr std::uint32_t speedField = 3;
constexpr std::uint32_t dragField = 4;
constexpr std::uint32_t doubledField = 5;
constexpr std::uint32_t radiusField = 6;

} // namespace

int main()
{
    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("amrexplorer-derived-field-" + std::to_string(unique));
    std::filesystem::create_directories(root / "Level_0");
    std::filesystem::create_directories(root / "Level_1");

    // Two levels over the unit square: level 0 is one grid of coarseCells^2
    // cells, level 1 one grid of the same count over the middle quarter, so a
    // composite slice mixes both and every block spans several evaluation
    // chunks. The fine level's values are offset so which level a sample came
    // from is visible in the value itself.
    const auto box = [](int low, int high) {
        return "((" + std::to_string(low) + "," + std::to_string(low) + ") ("
            + std::to_string(high) + "," + std::to_string(high) + ") (0,0))";
    };
    const auto coarseBox = box(0, coarseCells - 1);
    const auto fineBox = box(coarseCells / 2, 3 * coarseCells / 2 - 1);
    const auto coarseCellSize = 1.0 / static_cast<double>(coarseCells);
    const auto cellSizeLine = [](double size) {
        return std::to_string(size) + " " + std::to_string(size) + "\n";
    };
    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "3\ndensity\ntemperature\nx-momentum\n"
        "2\n0.0\n1\n"
        "0.0 0.0\n1.0 1.0\n2\n"
        + coarseBox + "\n"
        + box(0, 2 * coarseCells - 1) + "\n"
        "0 0\n"
        + cellSizeLine(coarseCellSize) + cellSizeLine(0.5 * coarseCellSize)
        + "0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n"
        "Level_0/Cell\n"
        "1 1 0.0\n0\n"
        "0.25 0.75\n0.25 0.75\n"
        "Level_1/Cell\n");
    writeText(root / "Level_0" / "Cell_H", visMfHeader(coarseBox));
    writeText(root / "Level_1" / "Cell_H", visMfHeader(fineBox));
    writeFab(root / "Level_0" / "Cell_D_00000", coarseBox,
        fabComponents(0, 0, 0.0));
    writeFab(root / "Level_1" / "Cell_D_00000", fineBox,
        fabComponents(coarseCells / 2, coarseCells / 2, 1000.0));

    // A definition that does not resolve is left out and reported, and the
    // dataset opens: a definition written for another plotfile must not be
    // able to stop this one from opening.
    {
        amrvis::PlotfileDataset partial(root, amrvis::DatasetId{7},
            1024 * 1024, {},
            {{"speed", "sqrt(dens**2)"}, {"twice", "2*density"}});
        require(partial.metadata().fields.size() == 4,
            "the resolvable definition was not installed");
        require(partial.metadata().fields[3].name == "twice",
            "the installed definition is not the resolvable one");
        require(partial.storedFieldCount() == 3,
            "the stored field count moved");
        const auto& skipped = partial.skippedDerivedFields();
        require(skipped.size() == 1 && skipped[0].definitionIndex == 0
                && skipped[0].name == "speed"
                && skipped[0].reason.find("dens") != std::string::npos,
            "the skipped definition was not reported with its reason");
    }

    amrvis::PlotfileDataset dataset(
        root, amrvis::DatasetId{7}, 8 * 1024 * 1024, {}, definitions());
    const auto& metadata = dataset.metadata();
    require(metadata.fields.size() == 7, "derived fields were not appended");
    require(metadata.fields[speedField].name == "speed"
            && metadata.fields[radiusField].name == "radius",
        "derived fields are not in definition order");
    require(!dataset.isDerivedField(amrvis::FieldId{2})
            && dataset.isDerivedField(amrvis::FieldId{speedField}),
        "isDerivedField does not separate stored from derived");

    amrvis::SliceQuery query(dataset);

    // The whole contract, on the composite path that mixes both levels: the
    // derived plane is the expression applied to the stored planes, sample for
    // sample, including where the fine level covers the coarse one.
    {
        const auto density = query.execute(sliceRequest(0, 1));
        const auto temperature = query.execute(sliceRequest(1, 1));
        const auto momentum = query.execute(sliceRequest(2, 1));
        const auto speed = query.execute(sliceRequest(speedField, 1));
        const auto drag = query.execute(sliceRequest(dragField, 1));
        const auto doubled = query.execute(sliceRequest(doubledField, 1));
        require(speed.plane.values.size() == density.plane.values.size(),
            "a derived plane is not the size of a stored one");
        bool sawFineLevel = false;
        bool sawCoarseLevel = false;
        std::size_t compared = 0;
        for (std::size_t index = 0; index < speed.plane.values.size(); ++index) {
            require(speed.plane.valid[index] == density.plane.valid[index],
                "a derived plane's coverage differs from its inputs'");
            require(speed.plane.sourceLevel[index]
                    == density.plane.sourceLevel[index],
                "a derived plane's source levels differ from its inputs'");
            if (speed.plane.valid[index] == 0) {
                continue;
            }
            sawFineLevel = sawFineLevel || speed.plane.sourceLevel[index] == 1;
            sawCoarseLevel
                = sawCoarseLevel || speed.plane.sourceLevel[index] == 0;
            const auto d = static_cast<double>(density.plane.values[index]);
            const auto t = static_cast<double>(temperature.plane.values[index]);
            const auto m = static_cast<double>(momentum.plane.values[index]);
            require(close(static_cast<double>(speed.plane.values[index]),
                        std::sqrt(d * d + t * t)),
                "derived slice does not match its expression");
            require(close(static_cast<double>(drag.plane.values[index]),
                        -m / d),
                "derived slice with a braced field name does not match");
            require(close(static_cast<double>(doubled.plane.values[index]),
                        2.0 * std::sqrt(d * d + t * t)),
                "a derived field reading another derived field does not match");
            ++compared;
        }
        require(compared > 0, "no covered samples were compared");
        require(sawFineLevel && sawCoarseLevel,
            "the comparison did not cover both levels");
    }

    // Coordinates come from the level the sample was taken on, which is what
    // samplePosition answers for a cell-centered axis.
    {
        auto request = sliceRequest(radiusField, 0);
        request.composition = amrvis::CompositionPolicy::ExactLevel;
        const auto radius = query.execute(request);
        const auto& level = metadata.levels[0];
        std::size_t compared = 0;
        for (int j = 0; j < coarseCells; ++j) {
            for (int i = 0; i < coarseCells; ++i) {
                const auto index =
                    static_cast<std::size_t>(i + coarseCells * j);
                if (radius.plane.valid[index] == 0) {
                    continue;
                }
                const auto x = amrvis::samplePosition(level, 0, i);
                const auto y = amrvis::samplePosition(level, 1, j);
                require(close(static_cast<double>(radius.plane.values[index]),
                            std::sqrt(x * x + y * y)),
                    "a coordinate expression does not match samplePosition");
                ++compared;
            }
        }
        require(compared == static_cast<std::size_t>(coarseCells)
                    * static_cast<std::size_t>(coarseCells),
            "the coordinate plane was not fully covered");
    }

    // A derived block is cached like any other: the second identical slice
    // reads nothing. On its own dataset, so the queries above have not already
    // warmed the block this measures.
    {
        amrvis::PlotfileDataset cold(
            root, amrvis::DatasetId{7}, 8 * 1024 * 1024, {}, definitions());
        amrvis::SliceQuery coldQuery(cold);
        const auto first = coldQuery.execute(sliceRequest(speedField, 0));
        const auto again = coldQuery.execute(sliceRequest(speedField, 0));
        require(first.metrics.blocksRead >= 1,
            "the first derived slice read no block");
        require(again.metrics.blocksRead == 0
                && again.metrics.cacheHits == first.metrics.blocksRead,
            "a repeated derived slice did not reuse the cached block");
        require(again.metrics.payloadBytesRead == 0,
            "a cached derived slice performed payload I/O");
    }

    // The line path, which composites blocks of its own.
    {
        amrvis::LineQuery line(dataset);
        amrvis::LineRequest request;
        request.dataset.value = 7;
        request.axis = 0;
        request.maximumLevel = 0;
        request.composition = amrvis::CompositionPolicy::ExactLevel;
        request.fixedCoordinates = {0.0, 0.375, 0.0};
        request.field.value = 0;
        const auto density = line.execute(request);
        request.field.value = 1;
        const auto temperature = line.execute(request);
        request.field.value = speedField;
        const auto speed = line.execute(request);
        require(speed.line.values.size() == density.line.values.size()
                && !speed.line.values.empty(),
            "a derived line is not the shape of a stored one");
        for (std::size_t index = 0; index < speed.line.values.size(); ++index) {
            const auto d = density.line.values[index];
            const auto t = temperature.line.values[index];
            require(close(speed.line.values[index], std::sqrt(d * d + t * t)),
                "derived line does not match its expression");
        }
    }

    // Cancellation is observed while evaluating, not only while reading. Its
    // own dataset, with the inputs warmed and the derived block not: a cold
    // input would report the cancellation from the read below it and leave the
    // evaluation loop's own check untested, while a warm derived block would
    // be served from the cache without reaching either.
    {
        amrvis::PlotfileDataset warm(
            root, amrvis::DatasetId{7}, 8 * 1024 * 1024, {}, definitions());
        amrvis::SliceQuery warmQuery(warm);
        for (const std::uint32_t field : {0U, 1U}) {
            auto request = sliceRequest(field, 0);
            request.composition = amrvis::CompositionPolicy::ExactLevel;
            static_cast<void>(warmQuery.execute(request));
        }
        amrvis::StopSource stop;
        stop.request_stop();
        bool cancelled = false;
        try {
            amrvis::BlockRequest request;
            request.dataset.value = 7;
            request.field.value = speedField;
            static_cast<void>(warm.requestBlock(request, stop.get_token()));
        } catch (const amrvis::ReadCancelled&) {
            cancelled = true;
        }
        require(cancelled, "a cancelled derived block read was not abandoned");
    }

    // Ranges: a derived field has no stored statistic, so the File and Level
    // scopes cannot answer for it while they still answer for stored fields.
    // (The resolver's fallback to Visible is what the GUI then shows.)
    {
        amrvis::LocalDatasetSession session(
            root, amrvis::DatasetId{7}, 8 * 1024 * 1024, {}, definitions());
        require(session.supportsDerivedFields(),
            "a local session does not offer derived fields");
        require(session.metadata().fields.size() == 7,
            "the session does not see the derived fields");
        require(session.storedFieldCount() == 3
                && session.skippedDerivedFields().empty(),
            "the session misreports which fields are stored");
        const auto available = [&session](std::uint32_t field,
                                   amrvis::RangeScope scope) {
            return session.rangeAvailable(
                amrvis::RangeRequest{amrvis::FieldId{field}, 0,
                    amrvis::CompositionPolicy::ExactLevel, scope});
        };
        require(available(0, amrvis::RangeScope::File)
                && available(0, amrvis::RangeScope::Level),
            "a stored field's metadata ranges went missing");
        require(!available(speedField, amrvis::RangeScope::File)
                && !available(speedField, amrvis::RangeScope::Level),
            "a derived field claimed a range no statistic can answer");
        require(!session.requestRange(
                        amrvis::RangeRequest{amrvis::FieldId{speedField}, 0,
                            amrvis::CompositionPolicy::ExactLevel,
                            amrvis::RangeScope::File})
                     .has_value(),
            "a derived field returned a File range");

        // The session's own read path (what the slice pipeline calls) sees the
        // derived field as an ordinary one.
        auto slice = sliceRequest(speedField, 0);
        slice.composition = amrvis::CompositionPolicy::ExactLevel;
        const auto result = session.requestView(amrvis::ViewDataRequest{slice});
        require(!std::get<amrvis::SliceQueryResult>(result)
                     .plane.values.empty(),
            "a session view of a derived field produced no samples");
    }

    // Ghost cells: a stored block's box is the valid box grown by the level's
    // ghost width, so a derived field has to read its inputs through their own
    // boxes rather than assume they are laid out like its own. The ghost cells
    // here hold a value no expectation below can produce, so an offset that
    // slips into them is not a near miss but an obvious wrong answer -- and an
    // expression mixing a stored field with a coordinate, whose block has no
    // ghosts to inherit, is exactly what used to disagree.
    {
        constexpr int cells = 8;
        constexpr double poison = -999.0;
        const auto ghostRoot = std::filesystem::temp_directory_path()
            / ("amrexplorer-derived-ghost-" + std::to_string(unique));
        std::filesystem::create_directories(ghostRoot / "Level_0");
        writeText(ghostRoot / "Header",
            "HyperCLaw-V1.1\n"
            "2\ndensity\ntemperature\n"
            "2\n0.0\n0\n"
            "0.0 0.0\n1.0 1.0\n"
            "\n"
            "((0,0) (7,7) (0,0))\n"
            "0\n"
            "0.125 0.125\n"
            "0\n0\n"
            "0 1 0.0\n0\n"
            "0.0 1.0\n0.0 1.0\n"
            "Level_0/Cell\n");
        // One ghost cell all round: the box array carries the valid box, the
        // FAB its own grown one.
        writeText(ghostRoot / "Level_0" / "Cell_H",
            "1\n1\n2\n1\n(1 0\n((0,0) (7,7) (0,0))\n)\n"
            "1\nFabOnDisk: Cell_D_00000 0\n\n"
            "1,2\n-10000.0,-10000.0,\n\n"
            "1,2\n10000.0,10000.0,\n\n");
        std::vector<std::vector<double>> components(2);
        for (int j = -1; j <= cells; ++j) {
            for (int i = -1; i <= cells; ++i) {
                const auto ghost =
                    i < 0 || j < 0 || i >= cells || j >= cells;
                components[0].push_back(ghost ? poison : densityAt(i, j));
                components[1].push_back(ghost ? poison : temperatureAt(i, j));
            }
        }
        writeFab(ghostRoot / "Level_0" / "Cell_D_00000",
            "((-1,-1) (8,8) (0,0))", components);

        amrvis::PlotfileDataset ghosted(ghostRoot, amrvis::DatasetId{11},
            8 * 1024 * 1024, {},
            {{"sum", "density + temperature"}, {"weighted", "density*x"}});
        require(ghosted.metadata().fields.size() == 4,
            "the ghosted fixture did not install both derived fields");
        amrvis::SliceQuery ghostQuery(ghosted);
        const auto sliceOf = [&ghostQuery](std::uint32_t field) {
            amrvis::SliceRequest request;
            request.dataset.value = 11;
            request.field.value = field;
            request.normalDirection = 1;
            request.visibleRegion = {{{0.0, 0.0, 0.0}}, {{1.0, 1.0, 0.0}}};
            request.maximumLevel = 0;
            request.composition = amrvis::CompositionPolicy::ExactLevel;
            request.outputSize = {cells, cells};
            return ghostQuery.execute(request);
        };
        const auto sum = sliceOf(2);
        const auto weighted = sliceOf(3);
        const auto& level = ghosted.metadata().levels[0];
        std::size_t compared = 0;
        for (int j = 0; j < cells; ++j) {
            for (int i = 0; i < cells; ++i) {
                const auto index = static_cast<std::size_t>(i + cells * j);
                require(sum.plane.valid[index] == 1
                        && weighted.plane.valid[index] == 1,
                    "a ghosted derived plane left a hole");
                require(close(static_cast<double>(sum.plane.values[index]),
                            densityAt(i, j) + temperatureAt(i, j)),
                    "a derived field over ghost-grown blocks read the wrong "
                    "samples");
                require(close(static_cast<double>(weighted.plane.values[index]),
                            densityAt(i, j)
                                * amrvis::samplePosition(level, 0, i)),
                    "a stored field and a coordinate did not line up over "
                    "ghost-grown blocks");
                ++compared;
            }
        }
        require(compared == static_cast<std::size_t>(cells * cells),
            "the ghosted planes were not fully covered");
        std::filesystem::remove_all(ghostRoot);
    }

    std::cout << "derived field query tests passed\n";
    return EXIT_SUCCESS;
}
