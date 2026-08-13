#pragma once

// Shared internals of the MainWindow translation units. These were the two
// anonymous namespaces of the original single-file MainWindow.cpp; the free
// functions are inline so every unit that needs one gets the same definition.

#include "MainWindow.hpp"
#include "AnimationExporter.hpp"
#include "SequenceController.hpp"
#include "AnimationPanel.hpp"
#include "CacheConfig.hpp"
#include "ColorBarWidget.hpp"
#include "DatasetWindow.hpp"
#include "FabSelectorDock.hpp"
#include "ImageView.hpp"
#include "IsoWidget.hpp"
#include "LinePlotRequest.hpp"
#include "LinePlotWindow.hpp"
#include "PlaneMapping.hpp"
#include "RemoteEndpoint.hpp"
#include "ScientificDoubleSpinBox.hpp"
#include "SetContoursDialog.hpp"
#include "Theme.hpp"
#include "UserGuideDialog.hpp"

#include <amrexplorer/io/FabCatalog.hpp>
#include <amrexplorer/io/FitsWriter.hpp>
#include <amrexplorer/io/detail/FabHeaderParsing.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>
#include <amrexplorer/core/CoordinateSystem.hpp>
#include <amrexplorer/core/Statistics.hpp>
#include <amrexplorer/pipeline/ParticleProjection.hpp>
#include <amrexplorer/pipeline/SliceRangeResolver.hpp>
#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/RemoteDatasetSession.hpp>
#include <amrexplorer/render2d/Contours.hpp>
#include <amrexplorer/render2d/Palette.hpp>
#include <amrexplorer/render2d/ScalarRenderer.hpp>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QWheelEvent>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QException>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QListView>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QProgressBar>
#include <QPushButton>
#include <QRect>
#include <QRegularExpressionValidator>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStringList>
#include <QStyleOptionComboBox>
#include <QStyledItemDelegate>
#include <QThreadPool>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <QtDebug>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Fed from the project version through a CMake compile definition; the
// fallback covers builds that do not set it (e.g. some IDE integrations).
#ifndef AMREXPLORER_VERSION
#define AMREXPLORER_VERSION "0.1.0-dev"
#endif

namespace amrvis::qt {

inline constexpr std::array<BuiltinPalette, 7> builtinPalettes{
    BuiltinPalette::Rainbow, BuiltinPalette::Turbo, BuiltinPalette::Viridis,
    BuiltinPalette::Plasma, BuiltinPalette::Parula, BuiltinPalette::Coolwarm,
    BuiltinPalette::Blackbody};
// Menu labels and QSettings keys; kept in sync with builtinPaletteName().
inline constexpr std::array<const char*, 7> builtinPaletteNames{
    "rainbow", "turbo", "viridis", "plasma", "parula", "coolwarm", "blackbody"};

inline constexpr std::array<Qt::GlobalColor, 7> particleDefaultColors{
    Qt::white, Qt::yellow, Qt::cyan, Qt::magenta,
    Qt::green, Qt::red, Qt::lightGray};

inline QColor defaultParticleColor(std::size_t speciesIndex)
{
    return QColor(
        particleDefaultColors[speciesIndex % particleDefaultColors.size()]);
}

inline void updateColorButton(QPushButton& button, const QColor& color)
{
    QPixmap swatch(18, 18);
    swatch.fill(color);
    button.setIcon(QIcon(swatch));
    button.setText(color.name(QColor::HexRgb).toUpper());
}

inline QImage verticallyFlippedCopy(const QImage& image)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    return image.flipped(Qt::Vertical).copy();
#else
    return image.mirrored(false, true).copy();
#endif
}

// The single conversion from a rendered ImageBuffer to the QImage the views
// display: ARGB32 over the buffer's rgba, mirrored vertically because plane
// row 0 is the bottom row. Returns a detached copy (verticallyFlippedCopy
// copies), so it outlives the buffer. Every setImage caller goes through here
// so the transform has one definition (see showSlice, syncVisibleRanges, and
// activeViewRasterMatchesDisplayRangeForTest).
inline QImage displayImageFor(const ImageBuffer& image)
{
    const QImage wrapped(
        reinterpret_cast<const uchar*>(image.rgba.data()),
        image.width, image.height, image.strideBytes, QImage::Format_ARGB32);
    return verticallyFlippedCopy(wrapped);
}

// Marks the active row in the palette dropdown with a bullet. The bullet lives
// in a reserved left column that every row's sizeHint accounts for, so names
// align and the indented text is never clipped. Installed only on the combo's
// popup view, so the closed combo still shows the clean palette name.
class CurrentRowBulletDelegate : public QStyledItemDelegate {
public:
    explicit CurrentRowBulletDelegate(QComboBox* combo, QObject* parent)
        : QStyledItemDelegate(parent), m_combo(combo) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        if (isSeparator(index)) {
            // The default combo delegate draws separators as a thin rule; this
            // custom delegate replaces it, so render the rule ourselves rather
            // than leaving a tall blank row. Paint the same item-view panel the
            // other rows use so the background matches, then draw the line.
            QStyleOptionViewItem sepOpt = option;
            initStyleOption(&sepOpt, index);
            auto* const sepStyle =
                sepOpt.widget != nullptr ? sepOpt.widget->style() : nullptr;
            if (sepStyle != nullptr) {
                sepStyle->drawPrimitive(
                    QStyle::PE_PanelItemViewItem, &sepOpt, painter, sepOpt.widget);
            }
            painter->save();
            painter->setPen(option.palette.color(QPalette::Mid));
            const int y = option.rect.center().y();
            painter->drawLine(option.rect.left() + kSeparatorMargin, y,
                option.rect.right() - kSeparatorMargin, y);
            painter->restore();
            return;
        }
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        auto* const style = opt.widget != nullptr ? opt.widget->style() : nullptr;

        // Full-width selection background, then the name indented past the
        // marker column so all rows line up at the same x.
        if (style != nullptr) {
            style->drawPrimitive(
                QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);
        }
        opt.rect.adjust(kMarkerColumn, 0, 0, 0);
        if (style != nullptr) {
            style->drawControl(
                QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        }

        if (m_combo != nullptr && index.row() == m_combo->currentIndex()) {
            const QPalette::ColorRole role =
                (opt.state & QStyle::State_Selected) != 0
                    ? QPalette::HighlightedText
                    : QPalette::WindowText;
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            painter->setBrush(opt.palette.brush(role));
            const QPointF center(option.rect.left() + kMarkerColumn / 2.0,
                option.rect.center().y());
            painter->drawEllipse(center, 2.5, 2.5);
            painter->restore();
        }
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        if (isSeparator(index)) {
            // A short row for the rule; the default sizeHint would give it a
            // full text-row height and read as a large gap.
            return QSize(0, kSeparatorHeight);
        }
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        // Reserve the marker column horizontally and add vertical padding so
        // the names have breathing room; keeps the closed combo unaffected.
        size.setWidth(size.width() + kMarkerColumn);
        size.setHeight(size.height() + kRowVerticalPadding);
        return size;
    }

private:
    static bool isSeparator(const QModelIndex& index)
    {
        return index.data(Qt::AccessibleDescriptionRole).toString()
            == QLatin1String("separator");
    }

    static constexpr int kMarkerColumn = 16;
    static constexpr int kRowVerticalPadding = 6;
    static constexpr int kSeparatorHeight = 9;
    static constexpr int kSeparatorMargin = 4;
    QPointer<QComboBox> m_combo;
};

inline QSettings makeSettings()
{
    return QSettings(QStringLiteral("amrex-codes"), QStringLiteral("amrexplorer"));
}

// An AMReX plotfile directory holds a Header file plus one Level_N
// subdirectory per refinement level (Level_0, Level_1, ...). Detecting by
// structure rather than by a "plt" name prefix avoids false matches.
inline bool isAmrexPlotfile(const std::filesystem::path& directory)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)
        || !std::filesystem::is_regular_file(directory / "Header", ec)) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.is_directory(ec)
            && entry.path().filename().string().starts_with("Level_")) {
            return true;
        }
    }
    return false;
}

// Qt Concurrent masks worker exceptions behind QUnhandledException, so the
// underlying library error text must be unwrapped before it is shown.
inline QString exceptionMessage(const std::exception& error)
{
    const auto* unhandled = dynamic_cast<const QUnhandledException*>(&error);
    if (unhandled != nullptr && unhandled->exception()) {
        try {
            std::rethrow_exception(unhandled->exception());
        } catch (const std::exception& inner) {
            return QString::fromUtf8(inner.what());
        } catch (...) {
            return QStringLiteral("unknown non-std exception");
        }
    }
    return QString::fromUtf8(error.what());
}

// Recovers a remote error code from a worker exception, unwrapping the
// QUnhandledException that Qt Concurrent wraps around a thrown std exception.
// Returns nullopt for local failures and any non-remote error.
inline std::optional<remote::ErrorCode> remoteErrorCode(const std::exception& error)
{
    const auto* unhandled = dynamic_cast<const QUnhandledException*>(&error);
    if (unhandled != nullptr && unhandled->exception()) {
        try {
            std::rethrow_exception(unhandled->exception());
        } catch (const remote::RemoteError& remoteError) {
            return remoteError.code();
        } catch (...) {
            return std::nullopt;
        }
    }
    if (const auto* remoteError
            = dynamic_cast<const remote::RemoteError*>(&error)) {
        return remoteError->code();
    }
    return std::nullopt;
}

// Result of the cheap connect-time handshake used to validate an endpoint and
// token before any dataset is opened.
struct RemoteVerifyOutcome {
    bool ok = false;
    bool unauthorized = false;
    QString message;
};

// QString face of the pipeline's formatter, for the GUI-side messages; hides
// amrvis::cacheBudgetDescription for unqualified calls in this namespace.
inline QString cacheBudgetDescription(std::uint64_t bytes)
{
    return QString::fromStdString(amrvis::cacheBudgetDescription(bytes));
}

inline QString cacheFallbackMessage(
    const DatasetSession& dataset, int fromLevel, int toLevel)
{
    const auto budget = cacheBudgetDescription(
        dataset.cacheMetrics().budgetBytes);
    return QObject::tr(
        "The finest slice exceeded the %1 cache budget. Showing levels 0 "
        "through %2 instead of levels 0 through %3; higher-resolution levels "
        "were omitted.")
        .arg(budget)
        .arg(toLevel)
        .arg(fromLevel);
}

inline bool selectCacheFallbackLevel(QComboBox* selector, int toLevel)
{
    if (toLevel < 0) {
        return false;
    }
    const auto data = toLevel == 0
        ? 0 : kUpdateToLevelOffset + toLevel;
    const auto index = selector->findData(data);
    if (index < 0) {
        return false;
    }
    const QSignalBlocker blocker(selector);
    selector->setCurrentIndex(index);
    return true;
}

inline void populateLevelCombo(QComboBox* combo, int finestLevel)
{
    combo->clear();
    combo->addItem(QObject::tr("Finest available"), -1);
    // "Level N only" is redundant when there is only one level; the whole
    // block is skipped for finestLevel == 0 so the combo shows just the
    // "Finest available" entry.
    if (finestLevel <= 0) {
        return;
    }
    // "Update to Level N" (composite 0..N) in reverse order, from
    // finestLevel-1 down to 1; only when there are at least three levels.
    for (int level = finestLevel - 1; level >= 1; --level) {
        combo->addItem(QObject::tr("Levs 0-%1").arg(level),
            kUpdateToLevelOffset + level);
    }
    for (int level = 0; level <= finestLevel; ++level) {
        combo->addItem(QObject::tr("Level %1 only").arg(level), level);
    }
}


// Scene-space annular-sector outline for a logical (r, theta) box: two
// straight radial edges (constant theta) and two subdivided arcs (constant r).
// Used for the spherical grid-box outlines and the picked-cell highlight.
inline QPainterPath sphericalSectorPath(const PlaneMapping& mapping,
    double r0, double r1, double t0, double t1)
{
    // ~1 degree per arc segment keeps the curve smooth; clamp so both tiny and
    // whole-domain sectors stay reasonable.
    const int steps = std::clamp(
        static_cast<int>(std::ceil((t1 - t0) / 0.02)), 8, 512);
    QPainterPath path;
    path.moveTo(mapping.sceneFromLogical(r0, t0));
    path.lineTo(mapping.sceneFromLogical(r1, t0));  // radial edge at theta0
    for (int i = 1; i <= steps; ++i) {              // outer arc at r1
        const double theta = t0 + (t1 - t0) * i / steps;
        path.lineTo(mapping.sceneFromLogical(r1, theta));
    }
    path.lineTo(mapping.sceneFromLogical(r0, t1));  // radial edge at theta1
    for (int i = 1; i <= steps; ++i) {              // inner arc at r0
        const double theta = t1 - (t1 - t0) * i / steps;
        path.lineTo(mapping.sceneFromLogical(r0, theta));
    }
    path.closeSubpath();
    return path;
}

// Everything the FAB selector dock needs for a source, computed off the GUI
// thread (see buildFabSelector) so its header scans / per-block preads never
// block the event loop. `matched` distinguishes a recognized FAB or
// single-level-VisMF source (whose m_fabMode/source state should be applied)
// from anything else (leave that state untouched, just hide the dock).
struct FabSelectorBuild {
    bool matched = false;
    bool fabMode = false;
    bool hasSourceMetadata = false;
    std::vector<FabSelectorEntry> entries;
    std::filesystem::path root;
};

// The result of a dataset open worker: the metadata plus, when the caller did
// not ask to preserve the existing selector, the FAB selector contents built
// alongside it (so the GUI-thread completion only blits, never reads files).
struct OpenedDataset {
    PlotfileMetadataResult metadata;
    std::optional<FabSelectorBuild> fabSelector;
    std::shared_ptr<DatasetSession> session;
};

// Reads FAB/MultiFab record headers and builds the selector entries. Runs on a
// worker thread; QCoreApplication::translate is thread-safe, and it touches no
// widgets or member state.
inline FabSelectorBuild buildFabSelector(
    const PlotfileMetadataResult& result, const std::filesystem::path& path)
{
    const auto precisionLabel = [](FabRealPrecision precision) {
        return precision == FabRealPrecision::Single
            ? QCoreApplication::translate("MainWindow", "IEEE-32")
            : QCoreApplication::translate("MainWindow", "IEEE-64");
    };

    FabSelectorBuild build;
    build.root = path.parent_path();
    if (build.root.empty()) {
        build.root = ".";
    }

    if (result.fileVersion == "FAB") {
        const auto records = scanFabFile(path);
        build.entries.reserve(records.size());
        for (const auto& record : records) {
            build.entries.push_back({
                .ordinal = record.ordinal,
                .level = 0,
                .blockIndex = record.ordinal,
                .filePath = path,
                .fileOffset = record.headerOffset,
                .validBox = record.storedBox,
                .storedBox = record.storedBox,
                .dimension = record.dimension,
                .components = record.components,
                .precision = precisionLabel(record.precision),
                .rawRecord = true
            });
        }
        build.matched = true;
        build.fabMode = true;
        build.hasSourceMetadata = false;
    } else if (result.fileVersion.starts_with("VisMF-")
        && result.metadata->levels.size() == 1) {
        const auto& metadata = *result.metadata;
        const auto& level = metadata.levels.front();
        build.entries.reserve(level.blocks.size());
        for (std::size_t index = 0; index < level.blocks.size(); ++index) {
            const auto& block = level.blocks[index];
            // Overflow-guarded shared grow (this copy previously used
            // plain int).
            auto storedBox = amrvis::detail::grownBox<MetadataReadError>(
                block.box, level.ghostWidth, metadata.dimension);
            auto precision = FabRealPrecision::Double;
            if (level.visMfHeaderVersion == 1) {
                const auto record = inspectFabRecord(
                    build.root / block.filePath, block.fileOffset);
                storedBox = record.storedBox;
                precision = record.precision;
            } else {
                precision = fabPrecisionFromDescriptor(level.realDescriptor);
            }
            build.entries.push_back({
                .ordinal = index,
                .level = level.level,
                .blockIndex = index,
                .filePath = build.root / block.filePath,
                .fileOffset = block.fileOffset,
                .validBox = block.box,
                .storedBox = storedBox,
                .dimension = metadata.dimension,
                .components = level.storedComponents,
                .precision = precisionLabel(precision),
                .rawRecord = false
            });
        }
        build.matched = true;
        build.fabMode = false;
        build.hasSourceMetadata = true;
    }
    return build;
}

} // namespace amrvis::qt
