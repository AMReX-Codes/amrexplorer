#pragma once

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/pipeline/SliceRangeResolver.hpp>
#include <amrexplorer/render2d/Palette.hpp>

#include <QByteArray>
#include <QColor>
#include <QJsonObject>
#include <QString>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace amrvis::qt {

inline constexpr qsizetype maximumViewerStateBytes = 4 * 1024 * 1024;
inline constexpr int viewerStateVersion = 1;

enum class ViewerSourceKind {
    Plotfile,
    PlotfileSequence,
    Fab,
    MultiFab
};

struct ViewerSourceState {
    ViewerSourceKind kind = ViewerSourceKind::Plotfile;
    std::filesystem::path path;
    std::vector<std::filesystem::path> frames;
    int currentFrame = 0;
    std::optional<std::size_t> selectedFab;
};

enum class LevelSelectionMode {
    FinestAvailable,
    ExactLevel,
    CompositeThroughLevel
};

struct LevelSelectionState {
    LevelSelectionMode mode = LevelSelectionMode::FinestAvailable;
    int level = 0;
};

struct FieldRangeState {
    RangeMode mode = RangeMode::File;
    std::optional<std::pair<double, double>> userRange;
};

struct FieldSelectionState {
    std::string field;
    LevelSelectionState level;
    bool logarithmic = false;
    std::map<std::string, FieldRangeState> ranges;
    std::vector<double> slicePositions;
    std::string activePanel = "2d";
};

enum class CameraMode {
    Fit,
    Manual
};

struct ImageCameraState {
    CameraMode mode = CameraMode::Fit;
    double zoom = 1.0;
    std::array<double, 2> center{0.5, 0.5};
};

struct PanelViewState {
    RealBox visibleRegion;
    ImageCameraState camera;
};

struct IsoCameraState {
    double azimuth = 0.0;
    double elevation = 0.0;
    double zoom = 1.0;
};

enum class ViewerPaletteKind {
    Builtin,
    Embedded
};

struct ViewerPaletteState {
    ViewerPaletteKind kind = ViewerPaletteKind::Builtin;
    std::string name = "rainbow";
    std::array<Palette::Rgb, Palette::slotCount> paletteSlots{};
    std::filesystem::path provenancePath;
};

enum class ViewerDisplayMode {
    Raster,
    Contours,
    RasterContours,
    VelocityVectors
};

struct RenderingState {
    ViewerPaletteState palette;
    ViewerDisplayMode mode = ViewerDisplayMode::Raster;
    int contourCount = 15;
    QColor contourColor = Qt::black;
    std::array<std::string, 3> vectorFields;
    bool boxes = false;
    bool slicePlanes = false;
    QString numberFormat = QStringLiteral("%.6g");
};

struct ParticleViewState {
    bool initialized = false;
    std::vector<std::string> species;
    double fraction = 1.0;
    std::uint64_t seed = 0;
    int pointSize = 3;
    std::map<std::string, QColor> colors;
};

struct AnimationViewState {
    int sweepAxis = 2;
    int speed = 300;
};

struct ViewerStateDocument {
    ViewerSourceState source;
    FieldSelectionState display;
    std::map<std::string, PanelViewState> panels;
    IsoCameraState isoCamera;
    RenderingState rendering;
    ParticleViewState particles;
    AnimationViewState animation;
    bool synchronizeRubberBand = true;
};

struct ViewerStateReadResult {
    std::optional<ViewerStateDocument> document;
    QString error;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return document.has_value();
    }
};

// Documents keep resolved absolute source paths in memory. Serialization makes
// paths below statePath's directory relative; parsing resolves them against
// that directory, never the process current working directory.
[[nodiscard]] std::filesystem::path portableViewerStatePath(
    const std::filesystem::path& source,
    const std::filesystem::path& statePath);
[[nodiscard]] std::filesystem::path resolveViewerStatePath(
    const std::filesystem::path& stored,
    const std::filesystem::path& statePath);

[[nodiscard]] QJsonObject toJson(const ViewerStateDocument& document,
    const std::filesystem::path& statePath);
[[nodiscard]] ViewerStateReadResult fromJson(const QByteArray& bytes,
    const std::filesystem::path& statePath);
[[nodiscard]] ViewerStateReadResult readViewerState(
    const std::filesystem::path& statePath);
[[nodiscard]] QString writeViewerState(const ViewerStateDocument& document,
    const std::filesystem::path& statePath);

} // namespace amrvis::qt
