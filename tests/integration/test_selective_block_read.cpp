#include <amrexplorer/expression/Expression.hpp>
#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

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
    require(static_cast<bool>(output), "could not create selective-read fixture text");
    output << text;
}

} // namespace

int main()
{
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("amrexplorer-selective-read-" + std::to_string(unique));
    std::filesystem::create_directories(root / "Level_0");

    writeText(root / "Header",
        "HyperCLaw-V1.1\n"
        "2\nfirst\nsecond-field\n"
        "2\n0.0\n0\n"
        "0.0 0.0\n1.0 1.0\n\n"
        "((0,0) (1,1) (0,0))\n"
        "0\n0.5 0.5\n0\n0\n"
        "0 1 0.0\n0\n"
        "0.0 1.0\n0.0 1.0\n"
        "Level_0/Cell\n");
    writeText(root / "Level_0" / "Cell_H",
        "1\n1\n2\n1\n"
        "(1 0\n((0,0) (1,1) (0,0))\n)\n"
        "1\nFabOnDisk: Cell_D_00000 0\n\n"
        "1,2\n1.0,10.0,\n\n"
        "1,2\n4.0,40.0,\n\n");

    constexpr std::string_view fabHeader =
        "FAB ((8, (64 11 52 0 1 12 0 1023)),(8, (8 7 6 5 4 3 2 1)))"
        "((-1,-1) (2,2) (0,0)) 2\n";
    const std::array<double, 16> first{
        1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0,
        9.0, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0};
    const std::array<double, 16> second{
        10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0,
        90.0, 100.0, 110.0, 120.0, 130.0, 140.0, 150.0, 160.0};
    {
        std::ofstream output(root / "Level_0" / "Cell_D_00000", std::ios::binary);
        require(static_cast<bool>(output), "could not create selective-read payload");
        output.write(fabHeader.data(), static_cast<std::streamsize>(fabHeader.size()));
        output.write(reinterpret_cast<const char*>(first.data()),
            static_cast<std::streamsize>(sizeof(first)));
        output.write(reinterpret_cast<const char*>(second.data()),
            static_cast<std::streamsize>(sizeof(second)));
    }

    const auto standaloneFab = amrvis::StandaloneMetadataReader{}.readFab(
        root / "Level_0" / "Cell_D_00000");
    require(standaloneFab.metadata->dimension == 2,
        "standalone FAB dimension mismatch");
    require(standaloneFab.metadata->fields.size() == 2,
        "standalone FAB component mapping mismatch");
    require(standaloneFab.metrics.payloadFilesRead == 0,
        "standalone FAB metadata read payload values");

    const auto metadataResult = amrvis::PlotfileMetadataReader{}.read(root);
    amrvis::PlotfileBlockReader reader(root, metadataResult.metadata);
    amrvis::BlockRequest request;
    request.dataset.value = 1;
    request.field.value = 1;
    const auto result = reader.readBlock(request);

    require(result.block->values.size() == second.size(), "selective value count mismatch");
    require(result.block->values[0] == 10.0 && result.block->values[3] == 40.0,
        "selective component values mismatch");
    require(result.metrics.filesRead == 1, "selective read opened an unexpected payload count");
    require(result.metrics.valuesRead == second.size(),
        "selective value accounting mismatch");
    require(result.metrics.bytesRead == fabHeader.size() + sizeof(second),
        "selective byte accounting mismatch");
    require(result.metrics.bytesRead
            < std::filesystem::file_size(root / "Level_0" / "Cell_D_00000"),
        "selective read accounted for unrelated component bytes");

    amrvis::StopSource stopped;
    stopped.request_stop();
    bool cancelled = false;
    try {
        [[maybe_unused]] auto ignored = reader.readBlock(request, stopped.get_token());
    } catch (const amrvis::ReadCancelled&) {
        cancelled = true;
    }
    require(cancelled, "pre-cancelled block read proceeded");

    amrvis::PlotfileDataset dataset(root, amrvis::DatasetId{7}, 1024 * 1024);
    request.dataset.value = 7;
    auto firstAccess = dataset.requestBlock(request);
    require(!firstAccess.cacheHit && firstAccess.io.bytesRead > 0,
        "first dataset access did not read the block");
    firstAccess.handle = {};
    auto secondAccess = dataset.requestBlock(request);
    require(secondAccess.cacheHit && secondAccess.io.bytesRead == 0,
        "second dataset access did not reuse the cached block");
    require(secondAccess.handle->values[2] == 30.0, "cached block value mismatch");
    require(dataset.cacheMetrics().residentBytes > 0, "dataset cache did not account bytes");

    const auto derived = dataset.addDerivedField({
        .name = "magnitude",
        .expression = "sqrt(first**2 + second-field**2)"
    });
    require(derived.value == 2, "derived field id does not follow stored fields");
    require(dataset.metadata().fields.size() == 3,
        "derived field was not published in dataset metadata");
    require(dataset.isDerivedField(derived), "derived field was not classified");
    request.field = derived;
    const auto derivedAccess = dataset.requestBlock(request);
    require(derivedAccess.handle->values.size() == first.size(),
        "derived field value count mismatch");
    require(derivedAccess.handle->values[0] == std::sqrt(101.0)
            && derivedAccess.handle->values[3] == std::sqrt(1616.0),
        "derived field expression produced incorrect values");
    require(derivedAccess.io.filesRead == 1,
        "derived field did not reuse the already cached second input");

    const auto powerDifference = dataset.addDerivedField({
        .name = "power_difference",
        .expression = "pow(first, 2) - first**2"
    });
    request.field = powerDifference;
    const auto powerDifferenceAccess = dataset.requestBlock(request);
    require(powerDifferenceAccess.handle->values[0] == 0.0
            && powerDifferenceAccess.handle->values[3] == 0.0,
        "pow() and ** produced different derived-field values");

    const auto logarithmRoundTrip = dataset.addDerivedField({
        .name = "logarithm_round_trip",
        .expression = "log10(exp10(first)) + log(exp(0))"
    });
    request.field = logarithmRoundTrip;
    const auto logarithmAccess = dataset.requestBlock(request);
    require(logarithmAccess.handle->values[0] == first[0]
            && logarithmAccess.handle->values[3] == first[3],
        "exponential and logarithm functions produced incorrect values");

    const auto difference = dataset.addDerivedField({
        .name = "difference",
        .expression = "second-field - first"
    });
    request.field = difference;
    const auto differenceAccess = dataset.requestBlock(request);
    require(differenceAccess.handle->values[0] == 9.0
            && differenceAccess.handle->values[3] == 36.0,
        "dashed field name was confused with subtraction");

    const auto dashedDerived = dataset.addDerivedField({
        .name = "derived-speed",
        .expression = "first + 1"
    });
    const auto dashedDerivedChain = dataset.addDerivedField({
        .name = "dashed_derived_chain",
        .expression = "derived-speed + 1"
    });
    request.field = dashedDerivedChain;
    const auto dashedDerivedAccess = dataset.requestBlock(request);
    require(dataset.isDerivedField(dashedDerived)
            && dashedDerivedAccess.handle->values[3] == 6.0,
        "valid dashed derived-field name could not be chained");

    const auto chained = dataset.addDerivedField({
        .name = "scaled",
        .expression = "2*magnitude"
    });
    request.field = chained;
    const auto chainedAccess = dataset.requestBlock(request);
    require(chainedAccess.handle->values[1] == 2.0 * std::sqrt(404.0),
        "derived field could not reference an earlier derived field");

    bool badExpressionRejected = false;
    try {
        [[maybe_unused]] const auto ignored = dataset.addDerivedField({
            .name = "bad",
            .expression = "missing + 1"
        });
    } catch (const std::invalid_argument&) {
        badExpressionRejected = true;
    }
    require(badExpressionRejected, "unknown derived-field input was accepted");

    const auto constant = dataset.addDerivedField({
        .name = "constant",
        .expression = "3.5"
    });
    request.field = constant;
    const auto constantAccess = dataset.requestBlock(request);
    require(constantAccess.handle->values.size() == first.size()
            && constantAccess.handle->values[2] == 3.5,
        "constant derived field did not cover the grid cells");

    const auto constantChained = dataset.addDerivedField({
        .name = "constant_chained",
        .expression = "constant + first"
    });
    request.field = constantChained;
    const auto constantChainedAccess = dataset.requestBlock(request);
    require(constantChainedAccess.handle->values.size() == first.size()
            && constantChainedAccess.handle->values[15] == 19.5,
        "constant derived field did not preserve ghost cells when chained");

    constexpr std::string_view dashedSyntaxError = "second-field + )";
    const auto fieldCountBeforeSyntaxError = dataset.metadata().fields.size();
    bool syntaxErrorRejected = false;
    try {
        [[maybe_unused]] const auto ignored = dataset.addDerivedField({
            .name = "syntax_error",
            .expression = std::string(dashedSyntaxError)
        });
    } catch (const amrvis::ExpressionError& error) {
        syntaxErrorRejected = true;
        require(error.offset() == dashedSyntaxError.find(')'),
            "dashed-field alias changed the reported syntax-error offset");
    }
    require(syntaxErrorRejected, "unsupported parser syntax was accepted");
    require(dataset.metadata().fields.size() == fieldCountBeforeSyntaxError,
        "syntax error changed dataset metadata");

    for (const auto* invalidName : {"2speed", "my speed", "sqrt(x)"}) {
        const auto fieldCountBeforeInvalidName = dataset.metadata().fields.size();
        bool invalidNameRejected = false;
        try {
            [[maybe_unused]] const auto ignored = dataset.addDerivedField({
                .name = invalidName,
                .expression = "first"
            });
        } catch (const std::invalid_argument&) {
            invalidNameRejected = true;
        }
        require(invalidNameRejected,
            "unreferenceable derived-field name was accepted");
        require(dataset.metadata().fields.size() == fieldCountBeforeInvalidName,
            "invalid derived-field name changed dataset metadata");
    }

    while (dataset.metadata().fields.size() < 17) {
        const auto suffix = dataset.metadata().fields.size();
        [[maybe_unused]] const auto filler = dataset.addDerivedField({
            .name = "limit_input_" + std::to_string(suffix),
            .expression = std::to_string(suffix)
        });
    }
    std::string sixteenInputs;
    std::string seventeenInputs;
    for (std::size_t index = 0; index < 17; ++index) {
        if (index != 0) {
            seventeenInputs += '+';
        }
        seventeenInputs += dataset.metadata().fields[index].name;
        if (index < 16) {
            if (!sixteenInputs.empty()) {
                sixteenInputs += '+';
            }
            sixteenInputs += dataset.metadata().fields[index].name;
        }
    }
    [[maybe_unused]] const auto maximumInputs = dataset.addDerivedField({
        .name = "maximum_inputs",
        .expression = sixteenInputs
    });
    bool tooManyInputsRejected = false;
    try {
        [[maybe_unused]] const auto ignored = dataset.addDerivedField({
            .name = "too_many_inputs",
            .expression = seventeenInputs
        });
    } catch (const std::invalid_argument&) {
        tooManyInputsRejected = true;
    }
    require(tooManyInputsRejected,
        "derived expression with more than 16 inputs was accepted");

    request.field.value = 1;
    amrvis::PlotfileDataset fabDataset(
        root / "Level_0" / "Cell_D_00000", amrvis::DatasetId{8}, 1024 * 1024);
    request.dataset.value = 8;
    const auto fabAccess = fabDataset.requestBlock(request);
    require(fabAccess.handle->values[1] == 20.0,
        "standalone FAB selective read value mismatch");

    amrvis::PlotfileDataset multiFabDataset(
        root / "Level_0" / "Cell", amrvis::DatasetId{9}, 1024 * 1024);
    request.dataset.value = 9;
    const auto multiFabAccess = multiFabDataset.requestBlock(request);
    require(multiFabAccess.handle->values[2] == 30.0,
        "standalone MultiFab selective read value mismatch");

    auto nodalMetadata =
        std::make_shared<amrvis::DatasetMetadata>(*standaloneFab.metadata);
    for (auto& field : nodalMetadata->fields) {
        field.centering = amrvis::Centering::Node;
    }
    nodalMetadata->levels.front().domain.centering = {{1, 1, 0}};
    nodalMetadata->levels.front().boxes.front().centering = {{1, 1, 0}};
    nodalMetadata->levels.front().blocks.front().box.centering = {{1, 1, 0}};
    amrvis::PlotfileDataset nodalDataset(
        root / "Level_0", amrvis::DatasetId{10}, 1024 * 1024,
        {std::move(nodalMetadata), {}, "FAB"});
    bool nodalInputRejected = false;
    try {
        [[maybe_unused]] const auto ignored = nodalDataset.addDerivedField({
            .name = "nodal-derived",
            .expression = "Fab_0 + 1"
        });
    } catch (const std::invalid_argument&) {
        nodalInputRejected = true;
    }
    require(nodalInputRejected,
        "non-cell-centered derived-field input was accepted");

    std::filesystem::remove_all(root);
    return 0;
}
