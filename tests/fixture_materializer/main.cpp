// Materializes FAB payloads for the metadata-only fixtures under tests/data/
// so the Qt smoke tests can open real data. This duplicates the
// payload-synthesis recipe of tests/integration/test_line_query.cpp (FAB
// on-disk header, little-endian doubles, analytic field values) as a
// standalone tool; keep the two in sync when the fixtures change.
//
// Usage:
//   fixture_materializer <sourceFixtureDir> <destDir>
//       [newTime] [--no-statistics] [--non-finite] [--drop-field <name>]
//
// Copies the fixture into destDir and writes each level's Cell_D_* payloads
// at the FabOnDisk offsets its Cell_H records; a level with a Nu_nd_H (a
// mapped-grid fixture) also gets its Nu_nd_D_* node displacements, from
// mappedGridRecipe below. An optional time value replaces the Header time
// line, giving plotfile-sequence frames distinct times.
// --no-statistics rewrites the VisMF headers as legal version 2 headers whose
// FabOnDisk records point directly to the generated binary payloads.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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

struct FixtureHeader {
    std::vector<std::string> lines;
    int fieldCount = 0;
    int dimension = 0;
    std::size_t timeLine = 0;
    std::size_t physicalUpperLine = 0;
    // The physical domain and each level's cell size, which place a node
    // index in space for the mapped-grid recipe.
    std::array<double, 3> physicalLower{0.0, 0.0, 0.0};
    std::array<double, 3> physicalUpper{1.0, 1.0, 1.0};
    std::vector<std::array<double, 3>> cellSize;
};

std::array<double, 3> readTriple(const std::string& line, int dimension,
    const char* what)
{
    std::istringstream input(line);
    std::array<double, 3> values{0.0, 0.0, 0.0};
    for (int axis = 0; axis < dimension; ++axis) {
        input >> values[static_cast<std::size_t>(axis)];
    }
    require(static_cast<bool>(input), what);
    return values;
}

// Plotfile Header layout: version, field count, one line per field name,
// dimension, time, finest level, physical lower, physical upper, refinement
// ratios, level domains, level steps, one cell-size line per level, ...
FixtureHeader readHeader(const std::filesystem::path& path)
{
    std::ifstream input(path);
    require(static_cast<bool>(input), "could not open the fixture Header");
    FixtureHeader header;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        header.lines.push_back(line);
    }
    require(header.lines.size() > 2, "the fixture Header is too short");
    header.fieldCount = std::stoi(header.lines[1]);
    const auto dimensionLine = static_cast<std::size_t>(2 + header.fieldCount);
    require(header.lines.size() > dimensionLine + 1,
        "the fixture Header is missing its dimension/time lines");
    header.dimension = std::stoi(header.lines[dimensionLine]);
    header.timeLine = dimensionLine + 1;
    header.physicalUpperLine = header.timeLine + 3;
    require(header.dimension == 2 || header.dimension == 3,
        "the fixture is neither 2-D nor 3-D");
    const auto finestLevel = std::stoi(header.lines[header.timeLine + 1]);
    const auto cellSizeLine = header.timeLine + 7;
    require(finestLevel >= 0
            && header.lines.size() > cellSizeLine + static_cast<std::size_t>(finestLevel),
        "the fixture Header is missing its cell-size lines");
    header.physicalLower = readTriple(header.lines[header.timeLine + 2],
        header.dimension, "could not parse the Header physical lower bound");
    header.physicalUpper = readTriple(header.lines[header.physicalUpperLine],
        header.dimension, "could not parse the Header physical upper bound");
    for (int level = 0; level <= finestLevel; ++level) {
        header.cellSize.push_back(readTriple(
            header.lines[cellSizeLine + static_cast<std::size_t>(level)],
            header.dimension, "could not parse a Header cell-size line"));
    }
    return header;
}

// Every integer in a box record such as "((0,0) (1,3) (0,0))", in order:
// lower per axis, upper per axis, index type.
std::vector<int> parseIntegers(const std::string& text)
{
    std::vector<int> values;
    std::size_t position = 0;
    while (position < text.size()) {
        const auto start = text.find_first_of("-0123456789", position);
        if (start == std::string::npos) {
            break;
        }
        auto end = start + 1;
        while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
            ++end;
        }
        values.push_back(std::stoi(text.substr(start, end - start)));
        position = end;
    }
    return values;
}

struct BlockRecord {
    std::string boxText;        // reused verbatim as the FAB header box
    std::vector<int> indices;   // parsed lower/upper per axis
    std::string fileName;
    std::uint64_t offset = 0;
    std::uint64_t payloadOffset = 0;
};

// Scans a level's Cell_H for its box records and FabOnDisk entries; record n
// stores the FAB of box n.
std::vector<BlockRecord> readCellHeader(
    const std::filesystem::path& path, int dimension)
{
    std::ifstream input(path);
    require(static_cast<bool>(input), "could not open a level Cell_H");
    std::vector<BlockRecord> blocks;
    std::size_t nextFab = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("((", 0) == 0) {
            BlockRecord block;
            block.boxText = line;
            block.indices = parseIntegers(line);
            require(block.indices.size()
                    == static_cast<std::size_t>(dimension * 3),
                "a Cell_H box record has the wrong shape");
            blocks.push_back(std::move(block));
        } else if (line.rfind("FabOnDisk:", 0) == 0) {
            require(nextFab < blocks.size(),
                "more FabOnDisk records than boxes in a Cell_H");
            std::istringstream record(line);
            std::string prefix;
            record >> prefix >> blocks[nextFab].fileName
                >> blocks[nextFab].offset;
            require(static_cast<bool>(record), "malformed FabOnDisk record");
            ++nextFab;
        }
    }
    require(!blocks.empty() && nextFab == blocks.size(),
        "a Cell_H's boxes and FabOnDisk records disagree");
    return blocks;
}

// The value of one component at one index of a block; `ordinal` counts the
// values written so far in the block (component-major, i fastest).
using ValueRecipe = std::function<double(
    int component, int i, int j, int k, std::size_t ordinal)>;

// The recipe of test_line_query.cpp: density(i, j) = (i + j) / 2,
// temperature = 100 + density, 3-D q(i, j, k) = (i + j + k) / 9, where i/j/k
// are cell indices at the level storing the grid.
ValueRecipe fieldRecipe(int dimension, bool nonFiniteValues, double scale)
{
    return [dimension, nonFiniteValues, scale](
               int component, int i, int j, int k, std::size_t ordinal) {
        if (nonFiniteValues) {
            switch (ordinal % 3) {
            case 0:
                return std::numeric_limits<double>::quiet_NaN();
            case 1:
                return std::numeric_limits<double>::infinity();
            default:
                return -std::numeric_limits<double>::infinity();
            }
        }
        const auto base = dimension == 2
            ? 0.5 * static_cast<double>(i + j)
            : static_cast<double>(i + j + k) / 9.0;
        return scale * (component == 0 ? base : 100.0 + base);
    };
}

// The mapped-grid (Nu_nd) recipe: a terrain that is highest at the far
// (x, y) corner and flattens out towards the top of the domain, with no
// horizontal displacement. A function of the node's physical position
// (lower + index * cell size of the level storing it), so every level of a
// refined fixture describes the same terrain. With the domain [lo, hi] and
// fractions X = (x - xlo) / (xhi - xlo) etc.:
//
//   nu_x = nu_y = 0
//   nu_z(x, y, z) = 0.125 * (1 - Z) * (X + Y) / 2      (3-D)
//   nu_y(x, y)    = 0.125 * (1 - Y) * X                (2-D)
//
// On the unit-domain fixtures every factor is exact in binary, so a test
// can recompute the value and compare exactly; bilinear in each in-plane
// pair, so linear interpolation between stored nodes reproduces it. Keep in
// sync with test_mapped_grid_query.cpp.
ValueRecipe mappedGridRecipe(const FixtureHeader& header,
    const std::array<double, 3>& cellSize)
{
    const auto dimension = header.dimension;
    const auto lower = header.physicalLower;
    const auto upper = header.physicalUpper;
    return [dimension, lower, upper, cellSize](
               int component, int i, int j, int k, std::size_t) {
        constexpr double amplitude = 0.125;
        const auto fraction = [&](int axis, int index) {
            const auto a = static_cast<std::size_t>(axis);
            return static_cast<double>(index) * cellSize[a] / (upper[a] - lower[a]);
        };
        if (dimension == 3) {
            if (component != 2) {
                return 0.0;
            }
            return amplitude * (1.0 - fraction(2, k))
                * (fraction(0, i) + fraction(1, j)) / 2.0;
        }
        if (component != 1) {
            return 0.0;
        }
        return amplitude * (1.0 - fraction(1, j)) * fraction(0, i);
    };
}

// Analytic values of one block, component-major with i fastest.
std::vector<double> blockValues(
    const BlockRecord& block, int dimension, int fieldCount,
    const ValueRecipe& recipe)
{
    const auto lower = [&block](int axis) {
        return block.indices[static_cast<std::size_t>(axis)];
    };
    const auto upper = [&block, dimension](int axis) {
        return block.indices[static_cast<std::size_t>(dimension + axis)];
    };
    std::vector<double> values;
    for (int component = 0; component < fieldCount; ++component) {
        for (int k = dimension == 3 ? lower(2) : 0;
            k <= (dimension == 3 ? upper(2) : 0); ++k) {
            for (int j = lower(1); j <= upper(1); ++j) {
                for (int i = lower(0); i <= upper(0); ++i) {
                    values.push_back(
                        recipe(component, i, j, k, values.size()));
                }
            }
        }
    }
    return values;
}

// Writes one FAB payload at its recorded FabOnDisk offset, exactly like
// test_line_query.cpp's writeFab: an ASCII header line followed by
// little-endian doubles.
void writeFab(const std::filesystem::path& path, BlockRecord& block,
    int dimension, int fieldCount, const ValueRecipe& recipe)
{
    if (block.offset == 0) {
        std::ofstream create(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(create), "could not create a fixture FAB");
    }
    std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
    require(static_cast<bool>(output), "could not open a fixture FAB");
    output.seekp(static_cast<std::streamoff>(block.offset), std::ios::beg);
    const auto header = std::string("FAB ") + std::string(realDescriptor)
        + block.boxText + " " + std::to_string(fieldCount) + "\n";
    output << header;
    block.payloadOffset = block.offset + header.size();
    const auto values = blockValues(block, dimension, fieldCount, recipe);
    output.write(reinterpret_cast<const char*>(values.data()),
        static_cast<std::streamsize>(values.size() * sizeof(double)));
    require(static_cast<bool>(output), "could not write a fixture FAB payload");
}

// Adds one small native AMReX particle species so the Qt slice and sequence
// smoke tests exercise particle discovery, binary reads, and point overlays.
void writeParticles(const std::filesystem::path& root, int dimension)
{
    constexpr int particleCount = 8;
    const auto species = root / "Tracer";
    std::filesystem::create_directories(species / "Level_0");
    {
        std::ofstream header(species / "Header");
        require(static_cast<bool>(header),
            "could not create the fixture particle Header");
        header << "Version_Two_Dot_Zero_double\n"
               << dimension << '\n'
               << "0\n"
               << "0\n"
               << "1\n"
               << particleCount << '\n'
               << "100\n"
               << "0\n"
               << "1\n"
               << "0 " << particleCount << " 0\n";
    }
    std::ofstream data(species / "Level_0" / "DATA_00000",
        std::ios::binary);
    require(static_cast<bool>(data),
        "could not create the fixture particle data");
    for (int id = 1; id <= particleCount; ++id) {
        const std::int32_t words[2]{id, 0};
        data.write(reinterpret_cast<const char*>(words), sizeof(words));
    }
    for (int id = 1; id <= particleCount; ++id) {
        const auto fraction = static_cast<double>(id)
            / static_cast<double>(particleCount + 1);
        const double positions[3]{
            fraction, 1.0 - fraction, 0.25 + 0.5 * fraction};
        data.write(reinterpret_cast<const char*>(positions),
            static_cast<std::streamsize>(
                static_cast<std::size_t>(dimension) * sizeof(double)));
    }
    require(static_cast<bool>(data),
        "could not write the fixture particle data");
}

void writeHeaderWithoutStatistics(const std::filesystem::path& path,
    const std::vector<BlockRecord>& blocks, int fieldCount)
{
    std::ofstream output(path, std::ios::trunc);
    require(static_cast<bool>(output),
        "could not create the no-statistics VisMF header");
    output << "2\n1\n" << fieldCount << "\n0\n"
        << "(" << blocks.size() << " 0\n";
    for (const auto& block : blocks) {
        output << block.boxText << '\n';
    }
    output << ")\n" << blocks.size() << '\n';
    for (const auto& block : blocks) {
        output << "FabOnDisk: " << block.fileName << ' '
            << block.payloadOffset << '\n';
    }
    output << realDescriptor << '\n';
    require(static_cast<bool>(output),
        "could not write the no-statistics VisMF header");
}

} // namespace

int main(int argc, char* argv[])
{
    require(argc >= 3 && argc <= 12,
        "usage: fixture_materializer <sourceFixtureDir> <destDir> "
        "[newTime] [--no-statistics] [--non-finite] [--scale <factor>] "
        "[--domain-upper-x <value>] [--drop-field <name>]");
    const std::filesystem::path source(argv[1]);
    const std::filesystem::path destination(argv[2]);
    std::optional<std::string> newTime;
    bool omitStatistics = false;
    bool nonFiniteValues = false;
    // Multiplies the synthesized field values so successive frames of a
    // sequence can carry different ranges (used by the range-cache test).
    double scale = 1.0;
    std::optional<double> domainUpperX;
    // Takes a field out of the Header's list, leaving the stored components
    // alone: what a frame that simply does not carry that field looks like.
    // A sequence of two such copies is the one shape that makes a field id
    // mean different things from one frame to the next.
    std::optional<std::string> droppedField;
    for (int argument = 3; argument < argc; ++argument) {
        const std::string value(argv[argument]);
        if (value == "--no-statistics") {
            require(!omitStatistics,
                "--no-statistics was specified more than once");
            omitStatistics = true;
        } else if (value == "--non-finite") {
            require(!nonFiniteValues,
                "--non-finite was specified more than once");
            nonFiniteValues = true;
        } else if (value == "--scale") {
            require(argument + 1 < argc, "--scale requires a factor argument");
            const std::string factor(argv[++argument]);
            try {
                scale = std::stod(factor);
            } catch (const std::exception&) {
                require(false, "--scale factor is not a number");
            }
        } else if (value == "--drop-field") {
            require(argument + 1 < argc, "--drop-field requires a name");
            require(!droppedField.has_value(),
                "--drop-field was specified more than once");
            droppedField = argv[++argument];
        } else if (value == "--domain-upper-x") {
            require(argument + 1 < argc,
                "--domain-upper-x requires a value");
            try {
                domainUpperX = std::stod(argv[++argument]);
            } catch (const std::exception&) {
                require(false, "--domain-upper-x value is not a number");
            }
        } else {
            require(!newTime.has_value(), "more than one new time was specified");
            newTime = value;
        }
    }
    require(std::filesystem::is_directory(source),
        "the source fixture directory is missing");
    auto header = readHeader(source / "Header");

    std::error_code error;
    std::filesystem::remove_all(destination, error);
    require(!error, "could not clear the destination directory");
    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path(), error);
        require(!error, "could not create the destination parent");
    }
    std::filesystem::copy(
        source, destination, std::filesystem::copy_options::recursive, error);
    require(!error, "could not copy the fixture");

    if (newTime.has_value() || domainUpperX.has_value()
        || droppedField.has_value()) {
        auto lines = header.lines;
        if (newTime.has_value()) {
            lines[header.timeLine] = *newTime;
        }
        if (domainUpperX.has_value()) {
            std::istringstream input(lines[header.physicalUpperLine]);
            std::vector<double> upper(
                static_cast<std::size_t>(header.dimension));
            for (auto& coordinate : upper) {
                input >> coordinate;
            }
            require(static_cast<bool>(input),
                "could not parse the Header physical upper bound");
            upper[0] = *domainUpperX;
            header.physicalUpper[0] = *domainUpperX;
            std::ostringstream output;
            output << std::setprecision(17);
            for (std::size_t axis = 0; axis < upper.size(); ++axis) {
                if (axis != 0) {
                    output << ' ';
                }
                output << upper[axis];
            }
            lines[header.physicalUpperLine] = output.str();
        }
        if (droppedField) {
            const auto first = lines.begin() + 2;
            const auto last = first + header.fieldCount;
            const auto found = std::find(first, last, *droppedField);
            require(found != last, "the fixture has no such field to drop");
            lines.erase(found);
            --header.fieldCount;
            lines[1] = std::to_string(header.fieldCount);
            // The VisMF header carries the component count too, and the two
            // are cross-checked when the plotfile is read, so its statistics
            // (one column per component) go with the dropped field.
            omitStatistics = true;
        }
        std::ofstream output(destination / "Header", std::ios::trunc);
        require(static_cast<bool>(output), "could not rewrite the Header");
        for (const auto& line : lines) {
            output << line << '\n';
        }
        require(static_cast<bool>(output), "could not write the Header");
    }

    for (int level = 0;; ++level) {
        const auto levelDir = destination / ("Level_" + std::to_string(level));
        if (!std::filesystem::is_directory(levelDir)) {
            break;
        }
        auto blocks = readCellHeader(levelDir / "Cell_H", header.dimension);
        const auto recipe = fieldRecipe(header.dimension, nonFiniteValues, scale);
        for (auto& block : blocks) {
            writeFab(levelDir / block.fileName, block,
                header.dimension, header.fieldCount, recipe);
        }
        if (omitStatistics) {
            writeHeaderWithoutStatistics(
                levelDir / "Cell_H", blocks, header.fieldCount);
        }
        // A mapped-grid fixture also carries the nodal Nu_nd MultiFab, one
        // component per space dimension, filled from mappedGridRecipe.
        const auto nodalHeader = levelDir / "Nu_nd_H";
        if (std::filesystem::is_regular_file(nodalHeader)) {
            auto nodalBlocks = readCellHeader(nodalHeader, header.dimension);
            require(static_cast<std::size_t>(level) < header.cellSize.size(),
                "the Header lists no cell size for a Nu_nd level");
            const auto nodalRecipe = mappedGridRecipe(
                header, header.cellSize[static_cast<std::size_t>(level)]);
            for (auto& block : nodalBlocks) {
                writeFab(levelDir / block.fileName, block,
                    header.dimension, header.dimension, nodalRecipe);
            }
            if (omitStatistics) {
                writeHeaderWithoutStatistics(
                    nodalHeader, nodalBlocks, header.dimension);
            }
        }
    }
    writeParticles(destination, header.dimension);
    return 0;
}
