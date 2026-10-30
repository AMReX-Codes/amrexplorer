// What PlotfileDataset does with a derived-field definition that does not
// resolve, under each policy: Skip (the viewer) opens anyway and reports it;
// Strict (unattended analysis) fails the open with DerivedFieldError. Also
// that derivedFieldProgram() exposes a derived field's compiled program and
// nothing for a stored field.
#include <amrexplorer/core/DerivedField.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view realDescriptor =
    "((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))";
constexpr int cells = 4;

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

double valueAt(int i, int j)
{
    return 1.0 + i + 10.0 * j;
}

// One level, one 4x4 grid of a single field "u".
std::filesystem::path writePlotfile(const std::filesystem::path& root)
{
    const std::string box = "((0,0) (3,3) (0,0))";
    std::filesystem::create_directories(root / "Level_0");
    writeText(root / "Header",
        "HyperCLaw-V1.1\n1\nu\n2\n0.0\n0\n0.0 0.0\n1.0 1.0\n\n" + box
            + "\n0\n0.25 0.25\n0\n0\n0 1 0.0\n0\n0.0 1.0\n0.0 1.0\n"
              "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n1\n0\n(1 0\n" + box
            + "\n)\n1\nFabOnDisk: Cell_D_00000 0\n\n1,1\n-1e9,\n\n1,1\n1e9,\n");
    std::ofstream output(root / "Level_0" / "Cell_D_00000", std::ios::binary);
    output << "FAB " << realDescriptor << box << " 1\n";
    for (int j = 0; j < cells; ++j) {
        for (int i = 0; i < cells; ++i) {
            const auto value = valueAt(i, j);
            output.write(reinterpret_cast<const char*>(&value), sizeof(value));
        }
    }
    return root;
}

} // namespace

int main()
{
    using namespace amrvis;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = writePlotfile(std::filesystem::temp_directory_path()
        / ("amrexplorer-derived-policy-" + std::to_string(unique)));
    const std::vector<DerivedFieldDefinition> definitions{
        {"twice", "2*u"}, {"broken", "missing + 1"}};

    {
        PlotfileDataset dataset(root, DatasetId{1}, 1U << 20U, StopToken{},
            definitions, DerivedFieldPolicy::Skip);
        require(dataset.metadata().fields.size() == 2,
            "Skip installs the definition that resolves");
        require(dataset.skippedDerivedFields().size() == 1
                && dataset.skippedDerivedFields().front().name == "broken",
            "Skip reports the definition that does not resolve");
    }

    bool failed = false;
    try {
        PlotfileDataset dataset(root, DatasetId{2}, 1U << 20U, StopToken{},
            definitions, DerivedFieldPolicy::Strict);
    } catch (const DerivedFieldError& error) {
        failed = error.definitionIndex() == 1;
    }
    require(failed, "Strict fails the open, naming the broken definition");

    PlotfileDataset dataset(root, DatasetId{3}, 1U << 20U, StopToken{},
        {definitions.front()}, DerivedFieldPolicy::Strict);
    require(dataset.skippedDerivedFields().empty(),
        "Strict with resolvable definitions skips nothing");
    const FieldId stored{0};
    const FieldId twice{1};
    require(dataset.derivedFieldProgram(stored) == nullptr,
        "a stored field has no derived program");
    const auto* program = dataset.derivedFieldProgram(twice);
    require(program != nullptr && program->inputs.size() == 1
            && !program->inputs.front().isCoordinate()
            && program->inputs.front().field == stored,
        "a derived field's program reads the stored field it names");
    require(dataset.derivedFieldProgram(FieldId{2}) == nullptr,
        "a field id past the list has no program");

    BlockRequest request;
    request.dataset = dataset.id();
    request.field = twice;
    const auto access = dataset.requestBlock(request);
    for (int j = 0; j < cells; ++j) {
        for (int i = 0; i < cells; ++i) {
            require(access.handle->values[static_cast<std::size_t>(i + cells * j)]
                    == 2.0 * valueAt(i, j),
                "the derived block evaluates its program");
        }
    }

    std::filesystem::remove_all(root);
    std::cout << "derived field policy: ok\n";
    return 0;
}
