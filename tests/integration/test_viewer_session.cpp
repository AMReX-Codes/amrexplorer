#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/viewer/ViewerSession.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

amrvis::ViewerDatasetSource sourceFor(const std::filesystem::path& path, amrvis::DatasetId id,
                                      amrvis::PlotfileMetadataResult metadata) {
    return amrvis::ViewerDatasetSource{.path = path,
                                       .dataRoot = path,
                                       .datasetId = id,
                                       .cacheBudgetBytes = 64U * 1024U * 1024U,
                                       .preparedMetadata = std::move(metadata)};
}

amrvis::PlotfileMetadataResult withoutDensity(amrvis::PlotfileMetadataResult metadata) {
    auto changed = std::make_shared<amrvis::DatasetMetadata>(*metadata.metadata);
    const auto density =
        std::find_if(changed->fields.begin(), changed->fields.end(),
                     [](const amrvis::FieldMetadata& field) { return field.name == "density"; });
    require(density != changed->fields.end(), "fixture has no density field");
    density->name = "rho";
    metadata.metadata = std::move(changed);
    return metadata;
}

} // namespace

int main(int argc, char* argv[]) {
    require(argc == 2, "usage: test_viewer_session <plotfile>");
    const std::filesystem::path path(argv[1]);
    const auto readMetadata = [&] { return amrvis::PlotfileMetadataReader{}.read(path); };

    amrvis::ViewerSession session;
    auto draft = session.beginExpressionEdit();
    const auto twice = draft.append("twice-density", "2*density");
    const auto threeTimes = draft.append("three-times", "twice-density + density");
    require(twice != threeTimes, "new expressions did not receive unique ids");

    std::swap(draft.definitions()[0], draft.definitions()[1]);
    require(draft.definitions()[0].id == threeTimes && draft.definitions()[1].id == twice,
            "reordering expressions changed stable identity");
    std::swap(draft.definitions()[0], draft.definitions()[1]);
    draft.definitions()[0].name = "double-density";
    draft.definitions()[1].source = "double-density + density";
    require(draft.definitions()[0].id == twice, "renaming an expression changed stable identity");

    auto install = amrvis::prepareViewerSnapshot(
        sourceFor(path, amrvis::DatasetId{1}, readMetadata()), session.planExpressions(draft));
    require(session.accept(std::move(install)), "valid expression transaction was rejected");
    require(session.expressions().size() == 2, "accepted catalog has the wrong size");
    require(session.expressions()[0].id == twice &&
                session.expressions()[0].name == "double-density",
            "accepted catalog did not preserve renamed identity");
    require(session.installedExpressionPairs().size() == 2, "valid expressions were not installed");

    const amrvis::FieldKey requested = amrvis::DerivedFieldKey{twice};
    session.setRequestedField(requested);
    session.setFieldRange(requested, amrvis::FieldRange{.mode = amrvis::RangeMode::User,
                                                        .userRange = std::pair{10.0, 20.0}});
    const auto missingPlan = session.planCurrent();
    auto missing = amrvis::prepareViewerSnapshot(
        sourceFor(path, amrvis::DatasetId{2}, withoutDensity(readMetadata())), missingPlan);
    require(missing.resolve(requested) == std::nullopt,
            "expression with a missing native input was installed");
    require(missing.requestedField == requested,
            "unavailable requested field intent was discarded");
    require(missing.resolvedField != requested,
            "unavailable requested field did not receive a fallback");
    require(missing.warnings.size() == 2,
            "unavailable dependency chain did not produce two warnings");
    require(session.accept(std::move(missing)), "compatible best-effort snapshot was rejected");
    require(session.requestedField() == requested, "session lost unavailable field intent");
    require(session.fieldRange(requested) == amrvis::FieldRange{.mode = amrvis::RangeMode::User,
                                                                .userRange = std::pair{10.0, 20.0}},
            "session lost the range of an unavailable field");
    require(session.expressions().size() == 2, "best-effort frame replaced the desired catalog");

    const auto restoredPlan = session.planCurrent();
    auto restored = amrvis::prepareViewerSnapshot(
        sourceFor(path, amrvis::DatasetId{3}, readMetadata()), restoredPlan);
    require(restored.resolve(requested).has_value(),
            "desired expression did not return on a compatible frame");
    require(restored.resolvedField == requested,
            "requested field was not restored on a compatible frame");
    require(session.accept(std::move(restored)), "restored compatible snapshot was rejected");

    const auto stalePlan = session.planCurrent();
    auto staleDraft = session.beginExpressionEdit();
    session.touch();
    auto stale = amrvis::prepareViewerSnapshot(
        sourceFor(path, amrvis::DatasetId{4}, readMetadata()), stalePlan);
    const auto acceptedDataset = session.dataset();
    require(!session.accept(std::move(stale)), "stale prepared snapshot was accepted");
    require(session.dataset() == acceptedDataset, "stale rejection changed the accepted dataset");
    bool staleDraftRejected = false;
    try {
        [[maybe_unused]] const auto ignored = session.planExpressions(staleDraft);
    } catch (const std::invalid_argument&) {
        staleDraftRejected = true;
    }
    require(staleDraftRejected, "stale expression draft was accepted");

    auto invalid = session.beginExpressionEdit();
    invalid.append("", "density");
    bool invalidRejected = false;
    try {
        [[maybe_unused]] const auto ignored = session.planExpressions(invalid);
    } catch (const std::invalid_argument&) {
        invalidRejected = true;
    }
    require(invalidRejected, "invalid expression draft was accepted");
    require(session.expressions().size() == 2, "invalid draft changed the accepted catalog");

    return 0;
}
