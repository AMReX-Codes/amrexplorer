#include "ViewerState.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

amrvis::qt::ViewerStateDocument completeDocument(
    const std::filesystem::path& source)
{
    using namespace amrvis;
    using namespace amrvis::qt;
    ViewerStateDocument document;
    document.source.kind = ViewerSourceKind::PlotfileSequence;
    document.source.frames = {source / "plt00000", source / "plt00010"};
    document.source.currentFrame = 1;
    document.display.field = "density";
    document.display.level = {
        LevelSelectionMode::CompositeThroughLevel, 2};
    document.display.logarithmic = true;
    document.display.ranges.emplace("density",
        FieldRangeState{RangeMode::User, std::pair{1.0e-8, 1.0e-2}});
    document.display.slicePositions = {0.2, 0.4, 0.6};
    document.display.activePanel = "y";
    document.panels.emplace("x", PanelViewState{
        RealBox{{0.0, 0.1, 0.2}, {1.0, 0.9, 0.8}},
        ImageCameraState{CameraMode::Manual, 2.5, {0.25, 0.75}}});
    document.isoCamera = {0.4, -0.2, 1.7};
    document.rendering.palette.kind = ViewerPaletteKind::Embedded;
    for (int index = 0; index < Palette::slotCount; ++index) {
        document.rendering.palette.paletteSlots[
            static_cast<std::size_t>(index)] = {
            static_cast<std::uint8_t>(index),
            static_cast<std::uint8_t>(255 - index),
            static_cast<std::uint8_t>(index / 2)};
    }
    document.rendering.palette.provenancePath = "/old/palette.pal";
    document.rendering.mode = ViewerDisplayMode::VelocityVectors;
    document.rendering.contourCount = 23;
    document.rendering.contourColor = QColor::fromRgba(0xff7f803f);
    document.rendering.vectorFields = {"x_velocity", "y_velocity", "z_velocity"};
    document.rendering.boxes = true;
    document.rendering.slicePlanes = true;
    document.rendering.numberFormat = QStringLiteral("%.9e");
    document.particles.initialized = true;
    document.particles.species = {"dark_matter", "stars"};
    document.particles.fraction = 0.125;
    document.particles.seed = 123456789;
    document.particles.pointSize = 7;
    document.particles.colors.emplace(
        "stars", QColor::fromRgba(0xaacc5500));
    document.animation = {1, 511};
    document.synchronizeRubberBand = false;
    return document;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    using namespace amrvis::qt;

    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary directory unavailable");
    const auto directory =
        std::filesystem::path(temporary.path().toStdString());
    const auto statePath = directory / "views" / "saved.amrexplorer-state.json";
    std::filesystem::create_directories(statePath.parent_path());
    const auto source = statePath.parent_path() / "data";

    auto original = completeDocument(source);
    const auto bytes =
        QJsonDocument(toJson(original, statePath)).toJson();
    const auto parsed = fromJson(bytes, statePath);
    require(static_cast<bool>(parsed), "complete document did not parse");
    require(parsed.document->source.frames == original.source.frames,
        "relative sequence paths did not resolve against the state file");
    require(parsed.document->source.currentFrame == 1,
        "sequence current frame was lost");
    require(parsed.document->display.field == "density",
        "field name was lost");
    require(parsed.document->display.ranges.at("density").userRange
            == original.display.ranges.at("density").userRange,
        "field range was lost");
    require(parsed.document->panels.at("x").camera.center
            == original.panels.at("x").camera.center,
        "camera center was lost");
    require(parsed.document->rendering.palette.paletteSlots[127].green
            == original.rendering.palette.paletteSlots[127].green,
        "embedded palette was lost");
    require(parsed.document->particles.colors.at("stars").rgba()
            == original.particles.colors.at("stars").rgba(),
        "particle RGBA color was lost");

    original.particles.seed = std::numeric_limits<std::uint64_t>::max();
    const auto maximumSeedJson = toJson(original, statePath);
    require(maximumSeedJson[QStringLiteral("particles")].toObject()
                [QStringLiteral("seed")].isString(),
        "particle seed was not serialized losslessly");
    const auto maximumSeed = fromJson(
        QJsonDocument(maximumSeedJson).toJson(), statePath);
    require(maximumSeed
            && maximumSeed.document->particles.seed
                == std::numeric_limits<std::uint64_t>::max(),
        "maximum particle seed did not round-trip");
    auto legacyNumericSeed = maximumSeedJson;
    auto legacyParticles =
        legacyNumericSeed[QStringLiteral("particles")].toObject();
    legacyParticles[QStringLiteral("seed")] = 42.0;
    legacyNumericSeed[QStringLiteral("particles")] = legacyParticles;
    const auto legacySeed = fromJson(
        QJsonDocument(legacyNumericSeed).toJson(), statePath);
    require(legacySeed && legacySeed.document->particles.seed == 42,
        "legacy numeric particle seed was not accepted");

    require(writeViewerState(original, statePath).isEmpty(),
        "atomic state write failed");
    require(static_cast<bool>(readViewerState(statePath)),
        "state file could not be read after atomic write");

    for (const auto kind : {ViewerSourceKind::Plotfile,
             ViewerSourceKind::Fab, ViewerSourceKind::MultiFab}) {
        auto single = original;
        single.source = {};
        single.source.kind = kind;
        single.source.path = source / "single";
        if (kind == ViewerSourceKind::MultiFab) {
            single.source.selectedFab = 3;
        }
        require(static_cast<bool>(fromJson(
            QJsonDocument(toJson(single, statePath)).toJson(), statePath)),
            "single-source variant did not round-trip");
    }

    auto unknown = toJson(original, statePath);
    unknown.insert(QStringLiteral("futureMember"), 42);
    require(static_cast<bool>(
        fromJson(QJsonDocument(unknown).toJson(), statePath)),
        "unknown additive member was not ignored");

    const auto reject = [&](QByteArray json, const char* message) {
        require(!fromJson(json, statePath), message);
    };
    reject(R"({"format":"wrong","version":1})",
        "wrong discriminator was accepted");
    reject(R"({"format":"amrexplorer-viewer-state","version":2})",
        "future version was accepted");
    reject(R"({"format":"amrexplorer-viewer-state","format":"x","version":1})",
        "duplicate logical key was accepted");
    reject(
        R"({"format":"amrexplorer-viewer-state","\u0066ormat":"x","version":1})",
        "escaped duplicate logical key was accepted");
    reject(QByteArray(maximumViewerStateBytes + 1, ' '),
        "oversized input was accepted");

    auto invalidRange = toJson(original, statePath);
    auto display = invalidRange[QStringLiteral("display")].toObject();
    auto ranges = display[QStringLiteral("ranges")].toObject();
    auto density = ranges[QStringLiteral("density")].toObject();
    density[QStringLiteral("minimum")] = 2.0;
    density[QStringLiteral("maximum")] = 1.0;
    ranges[QStringLiteral("density")] = density;
    display[QStringLiteral("ranges")] = ranges;
    invalidRange[QStringLiteral("display")] = display;
    reject(QJsonDocument(invalidRange).toJson(),
        "inverted user range was accepted");

    auto invalidColor = toJson(original, statePath);
    auto rendering = invalidColor[QStringLiteral("rendering")].toObject();
    rendering[QStringLiteral("contourColor")] = QStringLiteral("red");
    invalidColor[QStringLiteral("rendering")] = rendering;
    reject(QJsonDocument(invalidColor).toJson(),
        "invalid color was accepted");

    return 0;
}
