#include "ViewerState.hpp"
#include "NumberFormat.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace amrvis::qt {
namespace {

using Object = QJsonObject;
using Array = QJsonArray;

[[nodiscard]] QString pathString(const std::filesystem::path& path)
{
    return QString::fromStdString(path.generic_string());
}

[[nodiscard]] bool finite(double value) noexcept
{
    return std::isfinite(value);
}

[[noreturn]] void invalid(const QString& message)
{
    throw std::runtime_error(message.toStdString());
}

[[nodiscard]] QJsonValue required(
    const Object& object, QStringView key)
{
    const auto it = object.constFind(key.toString());
    if (it == object.constEnd()) {
        invalid(QStringLiteral("missing required member '%1'").arg(key));
    }
    return *it;
}

[[nodiscard]] Object objectValue(
    const Object& object, QStringView key, bool requiredValue = true)
{
    const auto value = requiredValue ? required(object, key)
                                     : object.value(key.toString());
    if (!value.isObject()) {
        invalid(QStringLiteral("member '%1' must be an object").arg(key));
    }
    return value.toObject();
}

[[nodiscard]] Array arrayValue(
    const Object& object, QStringView key, bool requiredValue = true)
{
    const auto value = requiredValue ? required(object, key)
                                     : object.value(key.toString());
    if (!value.isArray()) {
        invalid(QStringLiteral("member '%1' must be an array").arg(key));
    }
    return value.toArray();
}

[[nodiscard]] QString stringValue(const Object& object, QStringView key)
{
    const auto value = required(object, key);
    if (!value.isString()) {
        invalid(QStringLiteral("member '%1' must be a string").arg(key));
    }
    return value.toString();
}

[[nodiscard]] bool boolValue(const Object& object, QStringView key)
{
    const auto value = required(object, key);
    if (!value.isBool()) {
        invalid(QStringLiteral("member '%1' must be a boolean").arg(key));
    }
    return value.toBool();
}

[[nodiscard]] double numberValue(const Object& object, QStringView key)
{
    const auto value = required(object, key);
    if (!value.isDouble() || !finite(value.toDouble())) {
        invalid(QStringLiteral("member '%1' must be a finite number").arg(key));
    }
    return value.toDouble();
}

[[nodiscard]] int integerValue(const Object& object, QStringView key)
{
    const auto number = numberValue(object, key);
    if (std::trunc(number) != number
        || number < static_cast<double>(std::numeric_limits<int>::min())
        || number > static_cast<double>(std::numeric_limits<int>::max())) {
        invalid(QStringLiteral("member '%1' must be an integer").arg(key));
    }
    return static_cast<int>(number);
}

[[nodiscard]] std::uint64_t unsignedIntegerValue(
    const Object& object, QStringView key)
{
    const auto number = numberValue(object, key);
    // JSON numbers cannot represent every uint64 exactly. Restrict the file
    // format to the exact integer range so seeds round-trip portably.
    constexpr double largestExactInteger = 9007199254740991.0;
    if (std::trunc(number) != number || number < 0.0
        || number > largestExactInteger) {
        invalid(QStringLiteral(
            "member '%1' must be a non-negative exact JSON integer").arg(key));
    }
    return static_cast<std::uint64_t>(number);
}

[[nodiscard]] QColor colorValue(const Object& object, QStringView key)
{
    const auto text = stringValue(object, key);
    if (text.size() != 9 || !text.startsWith(QLatin1Char('#'))) {
        invalid(QStringLiteral(
            "member '%1' must be a #AARRGGBB color").arg(key));
    }
    bool ok = false;
    text.sliced(1).toUInt(&ok, 16);
    if (!ok) {
        invalid(QStringLiteral(
            "member '%1' must be a #AARRGGBB color").arg(key));
    }
    return QColor::fromRgba(text.sliced(1).toUInt(nullptr, 16));
}

[[nodiscard]] QString colorString(const QColor& color)
{
    return QStringLiteral("#%1").arg(
        color.rgba(), 8, 16, QLatin1Char('0'));
}

[[nodiscard]] Array realBoxJson(const RealBox& box)
{
    Array lower;
    Array upper;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        lower.append(box.lower[axis]);
        upper.append(box.upper[axis]);
    }
    return {lower, upper};
}

[[nodiscard]] RealBox parseRealBox(const QJsonValue& value, QStringView name)
{
    if (!value.isArray()) {
        invalid(QStringLiteral("member '%1' must be a box").arg(name));
    }
    const auto box = value.toArray();
    if (box.size() != 2 || !box[0].isArray() || !box[1].isArray()
        || box[0].toArray().size() != 3 || box[1].toArray().size() != 3) {
        invalid(QStringLiteral(
            "member '%1' must contain two 3-D corners").arg(name));
    }
    RealBox result;
    const auto lower = box[0].toArray();
    const auto upper = box[1].toArray();
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto index = static_cast<qsizetype>(axis);
        if (!lower[index].isDouble() || !upper[index].isDouble()
            || !finite(lower[index].toDouble())
            || !finite(upper[index].toDouble())
            || lower[index].toDouble() > upper[index].toDouble()) {
            invalid(QStringLiteral(
                "member '%1' contains a non-finite or inverted box").arg(name));
        }
        result.lower[axis] = lower[index].toDouble();
        result.upper[axis] = upper[index].toDouble();
    }
    return result;
}

[[nodiscard]] std::string sourceKindString(ViewerSourceKind kind)
{
    switch (kind) {
    case ViewerSourceKind::Plotfile: return "plotfile";
    case ViewerSourceKind::PlotfileSequence: return "plotfile-sequence";
    case ViewerSourceKind::Fab: return "fab";
    case ViewerSourceKind::MultiFab: return "multifab";
    }
    return {};
}

[[nodiscard]] ViewerSourceKind parseSourceKind(const QString& value)
{
    if (value == QLatin1String("plotfile")) return ViewerSourceKind::Plotfile;
    if (value == QLatin1String("plotfile-sequence")) {
        return ViewerSourceKind::PlotfileSequence;
    }
    if (value == QLatin1String("fab")) return ViewerSourceKind::Fab;
    if (value == QLatin1String("multifab")) return ViewerSourceKind::MultiFab;
    invalid(QStringLiteral("invalid source kind '%1'").arg(value));
}

[[nodiscard]] QString levelModeString(LevelSelectionMode mode)
{
    switch (mode) {
    case LevelSelectionMode::FinestAvailable:
        return QStringLiteral("finest-available");
    case LevelSelectionMode::ExactLevel: return QStringLiteral("exact-level");
    case LevelSelectionMode::CompositeThroughLevel:
        return QStringLiteral("composite-through-level");
    }
    return {};
}

[[nodiscard]] LevelSelectionMode parseLevelMode(const QString& value)
{
    if (value == QLatin1String("finest-available")) {
        return LevelSelectionMode::FinestAvailable;
    }
    if (value == QLatin1String("exact-level")) {
        return LevelSelectionMode::ExactLevel;
    }
    if (value == QLatin1String("composite-through-level")) {
        return LevelSelectionMode::CompositeThroughLevel;
    }
    invalid(QStringLiteral("invalid level mode '%1'").arg(value));
}

[[nodiscard]] QString rangeModeString(RangeMode mode)
{
    switch (mode) {
    case RangeMode::File: return QStringLiteral("file");
    case RangeMode::Level: return QStringLiteral("level");
    case RangeMode::Visible: return QStringLiteral("visible");
    case RangeMode::User: return QStringLiteral("user");
    }
    return {};
}

[[nodiscard]] RangeMode parseRangeMode(const QString& value)
{
    if (value == QLatin1String("file")) return RangeMode::File;
    if (value == QLatin1String("level")) return RangeMode::Level;
    if (value == QLatin1String("visible")) return RangeMode::Visible;
    if (value == QLatin1String("user")) return RangeMode::User;
    invalid(QStringLiteral("invalid range mode '%1'").arg(value));
}

[[nodiscard]] QString displayModeString(ViewerDisplayMode mode)
{
    switch (mode) {
    case ViewerDisplayMode::Raster: return QStringLiteral("raster");
    case ViewerDisplayMode::Contours: return QStringLiteral("contours");
    case ViewerDisplayMode::RasterContours:
        return QStringLiteral("raster-contours");
    case ViewerDisplayMode::VelocityVectors:
        return QStringLiteral("velocity-vectors");
    }
    return {};
}

[[nodiscard]] ViewerDisplayMode parseDisplayMode(const QString& value)
{
    if (value == QLatin1String("raster")) return ViewerDisplayMode::Raster;
    if (value == QLatin1String("contours")) return ViewerDisplayMode::Contours;
    if (value == QLatin1String("raster-contours")) {
        return ViewerDisplayMode::RasterContours;
    }
    if (value == QLatin1String("velocity-vectors")) {
        return ViewerDisplayMode::VelocityVectors;
    }
    invalid(QStringLiteral("invalid rendering mode '%1'").arg(value));
}

// Qt's JSON parser keeps the last duplicate object member. State documents
// reject duplicates, so scan just enough JSON syntax to detect them before
// handing the bytes to QJsonDocument.
class DuplicateKeyScanner {
public:
    explicit DuplicateKeyScanner(QByteArrayView input) : m_input(input) {}

    void scan()
    {
        whitespace();
        value();
        whitespace();
        if (m_at != m_input.size()) invalid(QStringLiteral("invalid JSON"));
    }

private:
    void whitespace()
    {
        while (m_at < m_input.size()
            && (m_input[m_at] == ' ' || m_input[m_at] == '\t'
                || m_input[m_at] == '\r' || m_input[m_at] == '\n')) {
            ++m_at;
        }
    }

    [[nodiscard]] char take()
    {
        if (m_at >= m_input.size()) invalid(QStringLiteral("invalid JSON"));
        return m_input[m_at++];
    }

    [[nodiscard]] QByteArray string()
    {
        if (take() != '"') invalid(QStringLiteral("invalid JSON string"));
        QByteArray result;
        while (m_at < m_input.size()) {
            const auto ch = take();
            if (ch == '"') return result;
            if (ch == '\\') {
                result.append(ch);
                result.append(take());
            } else {
                if (static_cast<unsigned char>(ch) < 0x20) {
                    invalid(QStringLiteral("invalid JSON string"));
                }
                result.append(ch);
            }
        }
        invalid(QStringLiteral("unterminated JSON string"));
    }

    void value()
    {
        whitespace();
        if (m_at >= m_input.size()) invalid(QStringLiteral("invalid JSON"));
        if (m_input[m_at] == '{') return object();
        if (m_input[m_at] == '[') return array();
        if (m_input[m_at] == '"') {
            (void)string();
            return;
        }
        while (m_at < m_input.size()
            && m_input[m_at] != ',' && m_input[m_at] != ']'
            && m_input[m_at] != '}' && m_input[m_at] != ' '
            && m_input[m_at] != '\t' && m_input[m_at] != '\r'
            && m_input[m_at] != '\n') {
            ++m_at;
        }
    }

    void object()
    {
        ++m_at;
        whitespace();
        std::vector<QByteArray> keys;
        if (m_at < m_input.size() && m_input[m_at] == '}') {
            ++m_at;
            return;
        }
        for (;;) {
            whitespace();
            auto key = string();
            if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
                invalid(QStringLiteral("duplicate object member '%1'")
                    .arg(QString::fromUtf8(key)));
            }
            keys.push_back(std::move(key));
            whitespace();
            if (take() != ':') invalid(QStringLiteral("invalid JSON object"));
            value();
            whitespace();
            const auto separator = take();
            if (separator == '}') return;
            if (separator != ',') invalid(QStringLiteral("invalid JSON object"));
        }
    }

    void array()
    {
        ++m_at;
        whitespace();
        if (m_at < m_input.size() && m_input[m_at] == ']') {
            ++m_at;
            return;
        }
        for (;;) {
            value();
            whitespace();
            const auto separator = take();
            if (separator == ']') return;
            if (separator != ',') invalid(QStringLiteral("invalid JSON array"));
        }
    }

    QByteArrayView m_input;
    qsizetype m_at = 0;
};

} // namespace

std::filesystem::path portableViewerStatePath(
    const std::filesystem::path& source,
    const std::filesystem::path& statePath)
{
    const auto base = std::filesystem::absolute(statePath).parent_path()
        .lexically_normal();
    const auto normalized = std::filesystem::absolute(source).lexically_normal();
    const auto relative = normalized.lexically_relative(base);
    if (!relative.empty() && *relative.begin() != "..") {
        return relative;
    }
    return normalized;
}

std::filesystem::path resolveViewerStatePath(
    const std::filesystem::path& stored,
    const std::filesystem::path& statePath)
{
    if (stored.is_absolute()) return stored.lexically_normal();
    return (std::filesystem::absolute(statePath).parent_path() / stored)
        .lexically_normal();
}

QJsonObject toJson(const ViewerStateDocument& document,
    const std::filesystem::path& statePath)
{
    Object root{{QStringLiteral("format"),
                    QStringLiteral("amrexplorer-viewer-state")},
        {QStringLiteral("version"), viewerStateVersion}};

    Object source{{QStringLiteral("kind"),
        QString::fromStdString(sourceKindString(document.source.kind))}};
    if (document.source.kind == ViewerSourceKind::PlotfileSequence) {
        Array frames;
        for (const auto& frame : document.source.frames) {
            frames.append(pathString(portableViewerStatePath(frame, statePath)));
        }
        source.insert(QStringLiteral("frames"), frames);
        source.insert(QStringLiteral("currentFrame"),
            document.source.currentFrame);
    } else {
        source.insert(QStringLiteral("path"), pathString(
            portableViewerStatePath(document.source.path, statePath)));
        if (document.source.selectedFab) {
            source.insert(QStringLiteral("selectedFab"),
                static_cast<double>(*document.source.selectedFab));
        }
    }
    root.insert(QStringLiteral("source"), source);

    Object level{{QStringLiteral("mode"),
        levelModeString(document.display.level.mode)}};
    if (document.display.level.mode != LevelSelectionMode::FinestAvailable) {
        level.insert(QStringLiteral("level"), document.display.level.level);
    }
    Object ranges;
    for (const auto& [name, range] : document.display.ranges) {
        Object value{{QStringLiteral("mode"), rangeModeString(range.mode)}};
        if (range.userRange) {
            value.insert(QStringLiteral("minimum"), range.userRange->first);
            value.insert(QStringLiteral("maximum"), range.userRange->second);
        }
        ranges.insert(QString::fromStdString(name), value);
    }
    Object display{{QStringLiteral("field"),
                       QString::fromStdString(document.display.field)},
        {QStringLiteral("level"), level},
        {QStringLiteral("logarithmic"), document.display.logarithmic},
        {QStringLiteral("ranges"), ranges},
        {QStringLiteral("activePanel"),
            QString::fromStdString(document.display.activePanel)}};
    if (!document.display.slicePositions.empty()) {
        Array positions;
        for (const auto position : document.display.slicePositions) {
            positions.append(position);
        }
        display.insert(QStringLiteral("slicePositions"), positions);
    }
    root.insert(QStringLiteral("display"), display);

    Object panels;
    for (const auto& [name, panel] : document.panels) {
        Object camera{{QStringLiteral("mode"),
            panel.camera.mode == CameraMode::Fit
                ? QStringLiteral("fit") : QStringLiteral("manual")}};
        if (panel.camera.mode == CameraMode::Manual) {
            camera.insert(QStringLiteral("zoom"), panel.camera.zoom);
            camera.insert(QStringLiteral("center"),
                Array{panel.camera.center[0], panel.camera.center[1]});
        }
        panels.insert(QString::fromStdString(name),
            Object{{QStringLiteral("visibleRegion"),
                       realBoxJson(panel.visibleRegion)},
                {QStringLiteral("camera"), camera}});
    }
    root.insert(QStringLiteral("panels"), panels);
    root.insert(QStringLiteral("isoCamera"),
        Object{{QStringLiteral("azimuth"), document.isoCamera.azimuth},
            {QStringLiteral("elevation"), document.isoCamera.elevation},
            {QStringLiteral("zoom"), document.isoCamera.zoom}});

    Object palette{{QStringLiteral("kind"),
        document.rendering.palette.kind == ViewerPaletteKind::Builtin
            ? QStringLiteral("builtin") : QStringLiteral("embedded")}};
    if (document.rendering.palette.kind == ViewerPaletteKind::Builtin) {
        palette.insert(QStringLiteral("name"),
            QString::fromStdString(document.rendering.palette.name));
    } else {
        Array embeddedSlots;
        for (const auto& rgb : document.rendering.palette.paletteSlots) {
            embeddedSlots.append(Array{rgb.red, rgb.green, rgb.blue});
        }
        palette.insert(QStringLiteral("slots"), embeddedSlots);
        if (!document.rendering.palette.provenancePath.empty()) {
            palette.insert(QStringLiteral("provenancePath"),
                pathString(document.rendering.palette.provenancePath));
        }
    }
    Object vectors;
    constexpr std::array<const char*, 3> vectorNames{"u", "v", "w"};
    for (std::size_t index = 0; index < vectorNames.size(); ++index) {
        if (!document.rendering.vectorFields[index].empty()) {
            vectors.insert(QString::fromLatin1(vectorNames[index]),
                QString::fromStdString(document.rendering.vectorFields[index]));
        }
    }
    root.insert(QStringLiteral("rendering"),
        Object{{QStringLiteral("palette"), palette},
            {QStringLiteral("mode"), displayModeString(document.rendering.mode)},
            {QStringLiteral("contourCount"), document.rendering.contourCount},
            {QStringLiteral("contourColor"),
                colorString(document.rendering.contourColor)},
            {QStringLiteral("vectorFields"), vectors},
            {QStringLiteral("boxes"), document.rendering.boxes},
            {QStringLiteral("slicePlanes"), document.rendering.slicePlanes},
            {QStringLiteral("numberFormat"), document.rendering.numberFormat}});

    Array species;
    for (const auto& name : document.particles.species) {
        species.append(QString::fromStdString(name));
    }
    Object colors;
    for (const auto& [name, color] : document.particles.colors) {
        colors.insert(QString::fromStdString(name), colorString(color));
    }
    root.insert(QStringLiteral("particles"),
        Object{{QStringLiteral("initialized"), document.particles.initialized},
            {QStringLiteral("species"), species},
            {QStringLiteral("fraction"), document.particles.fraction},
            {QStringLiteral("seed"),
                static_cast<double>(document.particles.seed)},
            {QStringLiteral("pointSize"), document.particles.pointSize},
            {QStringLiteral("colors"), colors}});
    root.insert(QStringLiteral("animation"),
        Object{{QStringLiteral("sweepAxis"), document.animation.sweepAxis},
            {QStringLiteral("speed"), document.animation.speed}});
    root.insert(QStringLiteral("zoom"),
        Object{{QStringLiteral("synchronizeRubberBand"),
            document.synchronizeRubberBand}});
    return root;
}

ViewerStateReadResult fromJson(const QByteArray& bytes,
    const std::filesystem::path& statePath)
{
    try {
        if (bytes.size() > maximumViewerStateBytes) {
            invalid(QStringLiteral("viewer state exceeds the 4 MiB limit"));
        }
        DuplicateKeyScanner(QByteArrayView(bytes)).scan();
        QJsonParseError parseError;
        const auto json = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
            invalid(QStringLiteral("invalid viewer-state JSON: %1")
                .arg(parseError.errorString()));
        }
        const auto root = json.object();
        if (stringValue(root, u"format") !=
            QLatin1String("amrexplorer-viewer-state")) {
            invalid(QStringLiteral("unsupported viewer-state format"));
        }
        const auto version = integerValue(root, u"version");
        if (version < 1 || version > viewerStateVersion) {
            invalid(QStringLiteral("unsupported viewer-state version %1")
                .arg(version));
        }

        ViewerStateDocument document;
        const auto source = objectValue(root, u"source");
        document.source.kind = parseSourceKind(stringValue(source, u"kind"));
        if (document.source.kind == ViewerSourceKind::PlotfileSequence) {
            const auto frames = arrayValue(source, u"frames");
            if (frames.size() < 2) {
                invalid(QStringLiteral(
                    "a plotfile sequence requires at least two frames"));
            }
            for (const auto& value : frames) {
                if (!value.isString() || value.toString().isEmpty()) {
                    invalid(QStringLiteral("sequence frame paths must be strings"));
                }
                document.source.frames.push_back(resolveViewerStatePath(
                    value.toString().toStdString(), statePath));
            }
            document.source.currentFrame = integerValue(source, u"currentFrame");
            if (document.source.currentFrame < 0
                || document.source.currentFrame
                    >= static_cast<int>(document.source.frames.size())) {
                invalid(QStringLiteral("currentFrame is outside the frame list"));
            }
        } else {
            const auto path = stringValue(source, u"path");
            if (path.isEmpty()) invalid(QStringLiteral("source path is empty"));
            document.source.path = resolveViewerStatePath(
                path.toStdString(), statePath);
            if (source.contains(QStringLiteral("selectedFab"))) {
                const auto selected = unsignedIntegerValue(source, u"selectedFab");
                document.source.selectedFab =
                    static_cast<std::size_t>(selected);
            }
        }

        const auto display = objectValue(root, u"display");
        document.display.field = stringValue(display, u"field").toStdString();
        if (document.display.field.empty()) {
            invalid(QStringLiteral("display field is empty"));
        }
        const auto level = objectValue(display, u"level");
        document.display.level.mode =
            parseLevelMode(stringValue(level, u"mode"));
        if (document.display.level.mode !=
            LevelSelectionMode::FinestAvailable) {
            document.display.level.level = integerValue(level, u"level");
            if (document.display.level.level < 0) {
                invalid(QStringLiteral("selected level cannot be negative"));
            }
        }
        document.display.logarithmic = boolValue(display, u"logarithmic");
        const auto ranges = objectValue(display, u"ranges");
        for (auto it = ranges.constBegin(); it != ranges.constEnd(); ++it) {
            if (!it.value().isObject() || it.key().isEmpty()) {
                invalid(QStringLiteral("field ranges must be named objects"));
            }
            const auto object = it.value().toObject();
            FieldRangeState range;
            range.mode = parseRangeMode(stringValue(object, u"mode"));
            const auto hasMinimum = object.contains(QStringLiteral("minimum"));
            const auto hasMaximum = object.contains(QStringLiteral("maximum"));
            if (range.mode == RangeMode::User) {
                if (!hasMinimum || !hasMaximum) {
                    invalid(QStringLiteral(
                        "a user range requires minimum and maximum"));
                }
                const auto minimum = numberValue(object, u"minimum");
                const auto maximum = numberValue(object, u"maximum");
                if (!(minimum < maximum)) {
                    invalid(QStringLiteral("a user range must be increasing"));
                }
                range.userRange = std::pair{minimum, maximum};
            } else if (hasMinimum || hasMaximum) {
                invalid(QStringLiteral(
                    "minimum/maximum are valid only for a user range"));
            }
            document.display.ranges.emplace(
                it.key().toStdString(), std::move(range));
        }
        document.display.activePanel =
            stringValue(display, u"activePanel").toStdString();
        if (document.display.activePanel != "2d"
            && document.display.activePanel != "x"
            && document.display.activePanel != "y"
            && document.display.activePanel != "z") {
            invalid(QStringLiteral("invalid active panel"));
        }
        if (display.contains(QStringLiteral("slicePositions"))) {
            const auto positions = arrayValue(display, u"slicePositions");
            if (positions.size() != 3) {
                invalid(QStringLiteral("slicePositions must have three values"));
            }
            for (const auto& value : positions) {
                if (!value.isDouble() || !finite(value.toDouble())) {
                    invalid(QStringLiteral(
                        "slicePositions must contain finite numbers"));
                }
                document.display.slicePositions.push_back(value.toDouble());
            }
        }

        const auto panels = objectValue(root, u"panels");
        for (auto it = panels.constBegin(); it != panels.constEnd(); ++it) {
            if (it.key() != QLatin1String("2d")
                && it.key() != QLatin1String("x")
                && it.key() != QLatin1String("y")
                && it.key() != QLatin1String("z")) {
                invalid(QStringLiteral("invalid panel name '%1'").arg(it.key()));
            }
            if (!it.value().isObject()) {
                invalid(QStringLiteral("panel '%1' must be an object")
                    .arg(it.key()));
            }
            const auto object = it.value().toObject();
            PanelViewState panel;
            panel.visibleRegion = parseRealBox(
                required(object, u"visibleRegion"), u"visibleRegion");
            const auto camera = objectValue(object, u"camera");
            const auto mode = stringValue(camera, u"mode");
            if (mode == QLatin1String("fit")) {
                panel.camera.mode = CameraMode::Fit;
            } else if (mode == QLatin1String("manual")) {
                panel.camera.mode = CameraMode::Manual;
                panel.camera.zoom = numberValue(camera, u"zoom");
                if (panel.camera.zoom <= 0.0 || panel.camera.zoom > 1.0e6) {
                    invalid(QStringLiteral("camera zoom is outside usable limits"));
                }
                const auto center = arrayValue(camera, u"center");
                if (center.size() != 2) {
                    invalid(QStringLiteral("camera center must have two values"));
                }
                for (std::size_t axis = 0; axis < 2; ++axis) {
                    const auto value = center[static_cast<qsizetype>(axis)];
                    if (!value.isDouble() || !finite(value.toDouble())
                        || value.toDouble() < 0.0 || value.toDouble() > 1.0) {
                        invalid(QStringLiteral(
                            "camera center must be normalized to [0, 1]"));
                    }
                    panel.camera.center[axis] = value.toDouble();
                }
            } else {
                invalid(QStringLiteral("invalid camera mode '%1'").arg(mode));
            }
            document.panels.emplace(it.key().toStdString(), std::move(panel));
        }
        const auto iso = objectValue(root, u"isoCamera");
        document.isoCamera.azimuth = numberValue(iso, u"azimuth");
        document.isoCamera.elevation = numberValue(iso, u"elevation");
        document.isoCamera.zoom = numberValue(iso, u"zoom");
        if (document.isoCamera.zoom < 0.1 || document.isoCamera.zoom > 10.0) {
            invalid(QStringLiteral("ISO camera zoom must be between 0.1 and 10"));
        }

        const auto rendering = objectValue(root, u"rendering");
        const auto palette = objectValue(rendering, u"palette");
        const auto paletteKind = stringValue(palette, u"kind");
        if (paletteKind == QLatin1String("builtin")) {
            document.rendering.palette.kind = ViewerPaletteKind::Builtin;
            document.rendering.palette.name =
                stringValue(palette, u"name").toStdString();
            constexpr std::array<std::string_view, 7> names{
                "rainbow", "turbo", "viridis", "plasma", "parula",
                "coolwarm", "blackbody"};
            if (std::find(names.begin(), names.end(),
                    document.rendering.palette.name) == names.end()) {
                invalid(QStringLiteral("unknown built-in palette"));
            }
        } else if (paletteKind == QLatin1String("embedded")) {
            document.rendering.palette.kind = ViewerPaletteKind::Embedded;
            const auto embeddedSlots = arrayValue(palette, u"slots");
            if (embeddedSlots.size() != Palette::slotCount) {
                invalid(QStringLiteral(
                    "an embedded palette requires exactly 256 RGB slots"));
            }
            for (qsizetype index = 0; index < embeddedSlots.size(); ++index) {
                if (!embeddedSlots[index].isArray()
                    || embeddedSlots[index].toArray().size() != 3) {
                    invalid(QStringLiteral("invalid embedded palette slot"));
                }
                const auto rgb = embeddedSlots[index].toArray();
                auto& output = document.rendering.palette.paletteSlots[
                    static_cast<std::size_t>(index)];
                std::array<std::uint8_t*, 3> channels{
                    &output.red, &output.green, &output.blue};
                for (qsizetype channel = 0; channel < 3; ++channel) {
                    if (!rgb[channel].isDouble()
                        || std::trunc(rgb[channel].toDouble())
                            != rgb[channel].toDouble()
                        || rgb[channel].toDouble() < 0.0
                        || rgb[channel].toDouble() > 255.0) {
                        invalid(QStringLiteral("invalid embedded palette slot"));
                    }
                    *channels[static_cast<std::size_t>(channel)] =
                        static_cast<std::uint8_t>(rgb[channel].toInt());
                }
            }
            if (palette.contains(QStringLiteral("provenancePath"))) {
                document.rendering.palette.provenancePath =
                    stringValue(palette, u"provenancePath").toStdString();
            }
        } else {
            invalid(QStringLiteral("invalid palette kind"));
        }
        document.rendering.mode =
            parseDisplayMode(stringValue(rendering, u"mode"));
        document.rendering.contourCount =
            integerValue(rendering, u"contourCount");
        if (document.rendering.contourCount < 1
            || document.rendering.contourCount > 99) {
            invalid(QStringLiteral("contourCount must be between 1 and 99"));
        }
        document.rendering.contourColor =
            colorValue(rendering, u"contourColor");
        const auto contourArgb = document.rendering.contourColor.rgba();
        if (contourArgb != QColor(Qt::black).rgba()
            && contourArgb != QColor(Qt::white).rgba()) {
            const auto paletteForColor =
                document.rendering.palette.kind == ViewerPaletteKind::Builtin
                ? [&]() -> Palette {
                    constexpr std::array<BuiltinPalette, 7> palettes{
                        BuiltinPalette::Rainbow, BuiltinPalette::Turbo,
                        BuiltinPalette::Viridis, BuiltinPalette::Plasma,
                        BuiltinPalette::Parula, BuiltinPalette::Coolwarm,
                        BuiltinPalette::Blackbody};
                    const auto found = std::find_if(
                        palettes.begin(), palettes.end(),
                        [&](BuiltinPalette candidate) {
                            return builtinPaletteName(candidate)
                                == document.rendering.palette.name;
                        });
                    return builtinPalette(*found);
                }()
                : Palette(document.rendering.palette.paletteSlots);
            bool representable = false;
            for (int index = 0; index < Palette::slotCount; ++index) {
                representable = representable
                    || paletteForColor.slotArgb(index) == contourArgb;
            }
            if (!representable) {
                invalid(QStringLiteral(
                    "contourColor is not black, white, or a palette slot"));
            }
        }
        const auto vectors = objectValue(rendering, u"vectorFields");
        constexpr std::array<const char*, 3> vectorNames{"u", "v", "w"};
        for (std::size_t index = 0; index < vectorNames.size(); ++index) {
            const auto key = QString::fromLatin1(vectorNames[index]);
            if (vectors.contains(key)) {
                if (!vectors.value(key).isString()) {
                    invalid(QStringLiteral("vector field names must be strings"));
                }
                document.rendering.vectorFields[index] =
                    vectors.value(key).toString().toStdString();
            }
        }
        document.rendering.boxes = boolValue(rendering, u"boxes");
        document.rendering.slicePlanes =
            boolValue(rendering, u"slicePlanes");
        document.rendering.numberFormat =
            stringValue(rendering, u"numberFormat");
        if (document.rendering.numberFormat.size() > 128
            || !isValidNumberFormat(document.rendering.numberFormat)) {
            invalid(QStringLiteral("invalid number format"));
        }

        const auto particles = objectValue(root, u"particles");
        document.particles.initialized = boolValue(particles, u"initialized");
        for (const auto& value : arrayValue(particles, u"species")) {
            if (!value.isString() || value.toString().isEmpty()) {
                invalid(QStringLiteral("particle species must be named strings"));
            }
            document.particles.species.push_back(value.toString().toStdString());
        }
        document.particles.fraction = numberValue(particles, u"fraction");
        if (document.particles.fraction < 0.0001
            || document.particles.fraction > 1.0) {
            invalid(QStringLiteral("particle fraction must be in [0.0001, 1]"));
        }
        document.particles.seed = unsignedIntegerValue(particles, u"seed");
        document.particles.pointSize = integerValue(particles, u"pointSize");
        if (document.particles.pointSize < 1
            || document.particles.pointSize > 12) {
            invalid(QStringLiteral("particle pointSize must be between 1 and 12"));
        }
        const auto colors = objectValue(particles, u"colors");
        for (auto it = colors.constBegin(); it != colors.constEnd(); ++it) {
            Object wrapper{{QStringLiteral("value"), it.value()}};
            document.particles.colors.emplace(it.key().toStdString(),
                colorValue(wrapper, u"value"));
        }

        const auto animation = objectValue(root, u"animation");
        document.animation.sweepAxis = integerValue(animation, u"sweepAxis");
        document.animation.speed = integerValue(animation, u"speed");
        if (document.animation.sweepAxis < 0
            || document.animation.sweepAxis > 2
            || document.animation.speed < 1
            || document.animation.speed > 600) {
            invalid(QStringLiteral("animation controls are outside UI limits"));
        }
        document.synchronizeRubberBand =
            boolValue(objectValue(root, u"zoom"), u"synchronizeRubberBand");
        return {std::move(document), {}};
    } catch (const std::exception& error) {
        return {std::nullopt, QString::fromUtf8(error.what())};
    }
}

ViewerStateReadResult readViewerState(
    const std::filesystem::path& statePath)
{
    QFile file(pathString(statePath));
    if (!file.open(QIODevice::ReadOnly)) {
        return {std::nullopt,
            QStringLiteral("cannot open viewer state: %1").arg(file.errorString())};
    }
    if (file.size() > maximumViewerStateBytes) {
        return {std::nullopt,
            QStringLiteral("viewer state exceeds the 4 MiB limit")};
    }
    return fromJson(file.readAll(), statePath);
}

QString writeViewerState(const ViewerStateDocument& document,
    const std::filesystem::path& statePath)
{
    QSaveFile file(pathString(statePath));
    if (!file.open(QIODevice::WriteOnly)) return file.errorString();
    const auto bytes = QJsonDocument(toJson(document, statePath))
        .toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return file.errorString();
    }
    if (!file.commit()) return file.errorString();
    return {};
}

} // namespace amrvis::qt
