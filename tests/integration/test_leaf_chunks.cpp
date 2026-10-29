// Exact leaf-cell traversal (src/analysis), without the Python binding: the
// plan partitions the leaf cells of a two-level fixture exactly, chunk values
// are the stored ones, regions select by cell center (half-open), requests
// and geometries the planner cannot honour are refused, dtypes and derived
// fields follow the plan, chunks hold no cache pins and outlive the dataset,
// and the cache budget is enforced at planning and again at reading.
#include <amrexplorer/analysis/LeafChunks.hpp>
#include <amrexplorer/cache/ByteLruCache.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
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

// Level 0: two grids side by side over 8x8. Level 1: one grid over
// coarse cells (2..5, 2..5), straddling the level-0 grid boundary.
const std::vector<std::array<int, 4>> coarse{{0, 0, 3, 7}, {4, 0, 7, 7}};
const std::vector<std::array<int, 4>> fine{{4, 4, 11, 11}};

void writePlotfile(const std::filesystem::path& root, int coordinateSystem)
{
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
        + std::to_string(dx / 2) + " " + std::to_string(dx / 2) + "\n"
        + std::to_string(coordinateSystem) + "\n0\n"
        "0 2 0.0\n0\n" + bounds(coarse[0], dx) + bounds(coarse[1], dx)
        + "Level_0/Cell\n1 1 0.0\n0\n" + bounds(fine[0], dx / 2) + "Level_1/Cell\n");
}

template <typename Error, typename Action>
bool throws(Action&& action)
{
    try {
        action();
    } catch (const Error&) {
        return true;
    }
    return false;
}

} // namespace

int main()
{
    using namespace amrvis;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("amrexplorer-leaf-chunks-" + std::to_string(unique));
    std::filesystem::create_directories(root);

    writePlotfile(root, 0);

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

    // Requests the planner cannot honour.
    require(throws<AnalysisError>([&] {
        static_cast<void>(planLeafChunks(dataset, {{"w"}, std::nullopt, std::nullopt}));
    }), "an unknown field is refused");
    require(throws<AnalysisError>([&] {
        static_cast<void>(planLeafChunks(dataset, {{"u", "u"}, std::nullopt, std::nullopt}));
    }), "a field requested twice is refused");
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    require(throws<AnalysisError>([&] {
        static_cast<void>(planLeafChunks(dataset,
            {{"u"}, RealBox{{{0.0, nan, 0.0}}, {{1.0, 1.0, 0.0}}}, std::nullopt}));
    }), "a region with a non-finite bound is refused");
    require(throws<AnalysisError>([&] {
        static_cast<void>(planLeafChunks(dataset,
            {{"u"}, RealBox{{{0.6, 0.0, 0.0}}, {{0.4, 1.0, 0.0}}}, std::nullopt}));
    }), "a region with unordered bounds is refused");
    const auto outside = planLeafChunks(dataset,
        {{"u"}, RealBox{{{2.0, 2.0, 0.0}}, {{3.0, 3.0, 0.0}}}, std::nullopt});
    require(outside.items.empty() && outside.totalCells == 0,
        "a region outside the domain plans nothing");

    // Reading is repeatable and does not depend on order.
    for (auto item = plan.items.rbegin(); item != plan.items.rend(); ++item) {
        const auto again = readLeafChunk(dataset, *item);
        const auto& values = std::get<std::vector<double>>(again->field("u"));
        const auto& i = again->index(0);
        const auto& j = again->index(1);
        require(again->size() == item->selectedCells, "an item reads its planned cells");
        for (std::size_t c = 0; c < again->size(); ++c) {
            require(values[c] == valueAt(item->level, i[c], j[c]),
                "a re-read in reverse order gives the same values");
        }
    }

    // The stored precision is double, so that is the default dtype; asking for
    // float32 narrows field values but not geometry.
    require(plan.selection->dtype == AnalysisValueType::Float64,
        "the default dtype is the widest stored precision");
    const auto narrow = planLeafChunks(dataset, {{"u"}, std::nullopt, AnalysisValueType::Float32});
    const auto narrowChunk = readLeafChunk(dataset, narrow.items.front());
    const auto& floats = std::get<std::vector<float>>(narrowChunk->field("u"));
    for (std::size_t c = 0; c < narrowChunk->size(); ++c) {
        require(floats[c] == static_cast<float>(valueAt(0, narrowChunk->index(0)[c],
                    narrowChunk->index(1)[c])),
            "a float32 plan narrows the stored values");
    }

    // Derived fields are evaluated per block and follow the plan's dtype.
    {
        PlotfileDataset derived(root, DatasetId{6}, 1U << 24U, StopToken{},
            {{"twice", "2*u"}}, DerivedFieldPolicy::Strict);
        const auto both = planLeafChunks(derived, {{"u", "twice"}, std::nullopt, std::nullopt});
        for (const auto& item : both.items) {
            const auto chunk = readLeafChunk(derived, item);
            const auto& u = std::get<std::vector<double>>(chunk->field("u"));
            const auto& twice = std::get<std::vector<double>>(chunk->field(1));
            for (std::size_t c = 0; c < chunk->size(); ++c) {
                require(twice[c] == 2.0 * u[c], "a derived field is computed from its inputs");
            }
        }
    }

    // A chunk is a copy: reading leaves nothing pinned, and the chunk outlives
    // the dataset it came from.
    std::shared_ptr<const LeafChunk> survivor;
    {
        PlotfileDataset scoped(root, DatasetId{7}, 1U << 24U);
        const auto scopedPlan = planLeafChunks(scoped, {{"u"}, std::nullopt, std::nullopt});
        survivor = readLeafChunk(scoped, scopedPlan.items.back());
        require(scoped.cacheMetrics().pinnedBytes == 0, "a read chunk holds no cache pins");
    }
    const auto& kept = std::get<std::vector<double>>(survivor->field("u"));
    for (std::size_t c = 0; c < survivor->size(); ++c) {
        require(kept[c] == valueAt(survivor->item().level, survivor->index(0)[c],
                    survivor->index(1)[c]),
            "a chunk outlives its dataset");
    }

    // A budget lowered after planning is enforced again when reading, and the
    // error names the item.
    {
        PlotfileDataset lowered(root, DatasetId{8}, 1U << 24U);
        const auto loweredPlan = planLeafChunks(lowered, {{"u"}, std::nullopt, std::nullopt});
        // Smaller than one coarse block of this fixture.
        require(lowered.setCacheBudget(64), "nothing is pinned, so the budget can drop");
        bool named = false;
        try {
            static_cast<void>(readLeafChunk(lowered, loweredPlan.items.front()));
        } catch (const CacheBudgetExceeded& error) {
            named = std::string(error.what()).find("plan item 0") != std::string::npos;
        }
        require(named, "a read over the lowered budget is refused, naming the item");
    }

    // A cancelled read stops.
    StopSource stop;
    static_cast<void>(stop.request_stop());
    require(throws<ReadCancelled>([&] {
        static_cast<void>(readLeafChunk(dataset, plan.items.front(), stop.get_token()));
    }), "a cancelled read raises ReadCancelled");

    // Leaf traversal is Cartesian only; other geometries fail explicitly.
    const auto cylindrical = root / "cylindrical";
    writePlotfile(cylindrical, 1);
    require(throws<AnalysisError>([&] {
        PlotfileDataset other(cylindrical, DatasetId{9}, 1U << 24U);
        static_cast<void>(planLeafChunks(other, {{"u"}, std::nullopt, std::nullopt}));
    }), "a non-Cartesian plotfile is refused");

    std::filesystem::remove_all(root);
    std::cout << "leaf chunks: ok\n";
    return 0;
}
