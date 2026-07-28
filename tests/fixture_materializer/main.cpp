// Materializes FAB payloads for the metadata-only fixtures under tests/data/
// so the Qt smoke tests can open real data. This duplicates the
// payload-synthesis recipe of tests/integration/test_line_query.cpp (FAB
// on-disk header, little-endian doubles, analytic field values) as a
// standalone tool; keep the two in sync when the fixtures change.
//
// Usage:
//   fixture_materializer <sourceFixtureDir> <destDir>
//       [newTime] [--no-statistics] [--non-finite] [--reverse-fields]
//       [--rename-last-field <name>]
//
// Copies the fixture into destDir and writes each level's Cell_D_* payloads
// at the FabOnDisk offsets its Cell_H records. An optional time value replaces
// the Header time line, giving plotfile-sequence frames distinct times.
// --no-statistics rewrites the VisMF headers as legal version 2 headers whose
// FabOnDisk records point directly to the generated binary payloads.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
};

// Plotfile Header layout: version, field count, one line per field name,
// dimension, time, ...
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
    require(header.dimension == 2 || header.dimension == 3,
        "the fixture is neither 2-D nor 3-D");
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

// Analytic cell values, component-major with i fastest — the recipe of
// test_line_query.cpp: density(i, j) = (i + j) / 2, temperature = 100 +
// density, 3-D q(i, j, k) = (i + j + k) / 9, where i/j/k are cell indices at
// the level storing the grid.
std::vector<double> blockValues(
    const BlockRecord& block, int dimension, int fieldCount,
    bool nonFiniteValues, double scale)
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
                    const auto base = dimension == 2
                        ? 0.5 * static_cast<double>(i + j)
                        : static_cast<double>(i + j + k) / 9.0;
                    if (nonFiniteValues) {
                        switch (values.size() % 3) {
                        case 0:
                            values.push_back(
                                std::numeric_limits<double>::quiet_NaN());
                            break;
                        case 1:
                            values.push_back(
                                std::numeric_limits<double>::infinity());
                            break;
                        default:
                            values.push_back(
                                -std::numeric_limits<double>::infinity());
                            break;
                        }
                    } else {
                        values.push_back(
                            scale * (component == 0 ? base : 100.0 + base));
                    }
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
    int dimension, int fieldCount, bool nonFiniteValues, double scale)
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
    const auto values = blockValues(
        block, dimension, fieldCount, nonFiniteValues, scale);
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
    require(argc >= 3 && argc <= 11,
        "usage: fixture_materializer <sourceFixtureDir> <destDir> "
        "[newTime] [--no-statistics] [--non-finite] [--reverse-fields] "
        "[--scale <factor>] [--rename-last-field <name>]");
    const std::filesystem::path source(argv[1]);
    const std::filesystem::path destination(argv[2]);
    std::optional<std::string> newTime;
    bool omitStatistics = false;
    bool nonFiniteValues = false;
    bool reverseFields = false;
    std::optional<std::string> renamedLastField;
    // Multiplies the synthesized field values so successive frames of a
    // sequence can carry different ranges (used by the range-cache test).
    double scale = 1.0;
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
        } else if (value == "--reverse-fields") {
            require(!reverseFields,
                "--reverse-fields was specified more than once");
            reverseFields = true;
        } else if (value == "--scale") {
            require(argument + 1 < argc, "--scale requires a factor argument");
            const std::string factor(argv[++argument]);
            try {
                scale = std::stod(factor);
            } catch (const std::exception&) {
                require(false, "--scale factor is not a number");
            }
        } else if (value == "--rename-last-field") {
            require(argument + 1 < argc,
                "--rename-last-field requires a name argument");
            require(!renamedLastField.has_value(),
                "--rename-last-field was specified more than once");
            renamedLastField = argv[++argument];
            require(!renamedLastField->empty(),
                "--rename-last-field requires a non-empty name");
        } else {
            require(!newTime.has_value(), "more than one new time was specified");
            newTime = value;
        }
    }
    require(std::filesystem::is_directory(source),
        "the source fixture directory is missing");
    const auto header = readHeader(source / "Header");

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

    if (newTime.has_value() || reverseFields
        || renamedLastField.has_value()) {
        auto lines = header.lines;
        if (newTime.has_value()) {
            lines[header.timeLine] = *newTime;
        }
        if (reverseFields) {
            std::reverse(lines.begin() + 2,
                lines.begin() + 2 + header.fieldCount);
        }
        if (renamedLastField) {
            lines[static_cast<std::size_t>(
                1 + header.fieldCount)] = *renamedLastField;
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
        for (auto& block : blocks) {
            writeFab(levelDir / block.fileName, block,
                header.dimension, header.fieldCount, nonFiniteValues, scale);
        }
        if (omitStatistics) {
            writeHeaderWithoutStatistics(
                levelDir / "Cell_H", blocks, header.fieldCount);
        }
    }
    writeParticles(destination, header.dimension);
    return 0;
}
