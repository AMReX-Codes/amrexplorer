// Exact leaf-cell traversal (src/analysis), without the Python binding: the
// plan partitions the leaf cells of a two-level fixture exactly, chunk values
// are the stored ones, regions select by cell center (half-open), and the
// planner refuses items over the cache budget. The Python suite under
// tests/python covers the same contracts more broadly; this keeps them tested
// in builds without Python.
#include <amrexplorer/analysis/LeafChunks.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace {

constexpr std::string_view realDescriptor =
    "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))";
constexpr int coarseCells = 8;

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
    require(static_cast<bool>(output), "could not create fixture text");
    output << text;
}

double valueAt(int level, int i, int j)
{
    return 1.0 + i + 100.0 * j + 10000.0 * level;
}

std::string box(int lowI, int lowJ, int highI, int highJ)
{
    return "((" + std::to_string(lowI) + "," + std::to_string(lowJ) + ") ("
        + std::to_string(highI) + "," + std::to_string(highJ) + ") (0,0))";
}

// One FAB per grid, each in its own data file.
void writeLevel(const std::filesystem::path& directory, int level,
    const std::vector<std::array<int, 4>>& boxes)
{
    std::filesystem::create_directories(directory);
    std::string header = "1\n1\n1\n0\n(" + std::to_string(boxes.size()) + " 0\n";
    for (const auto& b : boxes) {
        header += box(b[0], b[1], b[2], b[3]) + "\n";
    }
    header += ")\n" + std::to_string(boxes.size()) + "\n";
    for (std::size_t g = 0; g < boxes.size(); ++g) {
        header += "FabOnDisk: Cell_D_0000" + std::to_string(g) + " 0\n";
    }
    header += "\n" + std::to_string(boxes.size()) + ",1\n";
    for (std::size_t g = 0; g < boxes.size(); ++g) {
        header += "-1e9,\n";
    }
    header += "\n" + std::to_string(boxes.size()) + ",1\n";
    for (std::size_t g = 0; g < boxes.size(); ++g) {
        header += "1e9,\n";
    }
    writeText(directory / "Cell_H", header);
    for (std::size_t g = 0; g < boxes.size(); ++g) {
        const auto& b = boxes[g];
        std::ofstream output(directory / ("Cell_D_0000" + std::to_string(g)),
            std::ios::binary);
        output << "FAB " << realDescriptor << box(b[0], b[1], b[2], b[3]) << " 1\n";
        for (int j = b[1]; j <= b[3]; ++j) {
            for (int i = b[0]; i <= b[2]; ++i) {
                const auto value = valueAt(level, i, j);
                output.write(reinterpret_cast<const char*>(&value), sizeof(value));
            }
        }
    }
}

} // namespace

int main()
{
    using namespace amrvis;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("amrexplorer-leaf-chunks-" + std::to_string(unique));
    std::filesystem::create_directories(root);

    // Level 0: two grids side by side over 8x8. Level 1: one grid over
    // coarse cells (2..5, 2..5), straddling the level-0 grid boundary.
    const std::vector<std::array<int, 4>> coarse{{0, 0, 3, 7}, {4, 0, 7, 7}};
    const std::vector<std::array<int, 4>> fine{{4, 4, 11, 11}};
    writeLevel(root / "Level_0", 0, coarse);
    writeLevel(root / "Level_1", 1, fine);
    const auto dx = 1.0 / coarseCells;
    const auto bounds = [&](const std::array<int, 4>& b, double h) {
        return std::to_string(b[0] * h) + " " + std::to_string((b[2] + 1) * h) + "\n"
            + std::to_string(b[1] * h) + " " + std::to_string((b[3] + 1) * h) + "\n";
    };
    writeText(root / "Header",
        "HyperCLaw-V1.1\n1\nu\n2\n0.0\n1\n0.0 0.0\n1.0 1.0\n2\n"
        + box(0, 0, 7, 7) + " " + box(0, 0, 15, 15) + "\n0 0\n"
        + std::to_string(dx) + " " + std::to_string(dx) + "\n"
        + std::to_string(dx / 2) + " " + std::to_string(dx / 2) + "\n0\n0\n"
        "0 2 0.0\n0\n" + bounds(coarse[0], dx) + bounds(coarse[1], dx)
        + "Level_0/Cell\n1 1 0.0\n0\n" + bounds(fine[0], dx / 2) + "Level_1/Cell\n");

    PlotfileDataset dataset(root, DatasetId{3}, 1U << 24U);
    const auto plan = planLeafChunks(dataset, {{"u"}, std::nullopt, std::nullopt});
    require(plan.items.size() == 3, "one item per grid");
    require(plan.totalCells == 64 - 16 + 64, "leaf cells: 48 coarse + 64 fine");

    std::set<std::tuple<int, int, int>> seen;
    double area = 0.0;
    for (const auto& item : plan.items) {
        const auto chunk = readLeafChunk(dataset, item);
        const auto& values = std::get<std::vector<double>>(chunk->field("u"));
        const auto& i = chunk->index(0);
        const auto& j = chunk->index(1);
        for (std::size_t c = 0; c < chunk->size(); ++c) {
            require(seen.insert({item.level, i[c], j[c]}).second,
                "a leaf cell appears once");
            require(values[c] == valueAt(item.level, i[c], j[c]),
                "chunk values are the stored values");
            if (item.level == 0) {
                const bool covered = i[c] >= 2 && i[c] <= 5 && j[c] >= 2 && j[c] <= 5;
                require(!covered, "no covered coarse cell appears");
            }
            area += chunk->cellVolume()[c];
        }
    }
    require(std::abs(area - 1.0) < 1e-14, "leaf cells tile the domain");

    // A region bound on a level-1 cell center: half-open, so the cell whose
    // center is at 0.40625 belongs to the region above it.
    const RealBox lowerHalf{{{0.0, 0.0, 0.0}}, {{0.40625, 1.0, 0.0}}};
    const RealBox upperHalf{{{0.40625, 0.0, 0.0}}, {{1.0, 1.0, 0.0}}};
    const auto low = planLeafChunks(dataset, {{"u"}, lowerHalf, std::nullopt});
    const auto high = planLeafChunks(dataset, {{"u"}, upperHalf, std::nullopt});
    require(low.totalCells + high.totalCells == plan.totalCells,
        "adjacent regions partition the leaf cells");

    bool refused = false;
    try {
        PlotfileDataset small(root, DatasetId{4}, 600);
        static_cast<void>(planLeafChunks(small, {{"u"}, std::nullopt, std::nullopt}));
    } catch (const AnalysisError&) {
        refused = true;
    }
    require(refused, "items over the cache budget are refused at planning");

    bool mismatched = false;
    try {
        PlotfileDataset other(root, DatasetId{5}, 1U << 24U);
        static_cast<void>(readLeafChunk(other, plan.items.front()));
    } catch (const AnalysisError&) {
        mismatched = true;
    }
    require(mismatched, "items are bound to their dataset");

    std::filesystem::remove_all(root);
    std::cout << "leaf chunks: ok\n";
    return 0;
}
