#pragma once

// The parts of the original MainWindow.cpp anonymous namespaces that more than
// one of the five translation units needs; everything used by a single unit
// stayed in that unit's own anonymous namespace. The free functions are inline
// so every unit that needs one gets the same definition.

#include "MainWindow.hpp"
#include "QtErrorText.hpp"
#include "AnimationExporter.hpp"
#include "PaletteController.hpp"
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
#include "RemoteFileDialog.hpp"
#include "ScientificDoubleSpinBox.hpp"
#include "SshRemoteSession.hpp"
#include "SetContoursDialog.hpp"
#include "Theme.hpp"
#include "UserGuideDialog.hpp"

#include <amrexplorer/io/FabCatalog.hpp>
#include <amrexplorer/io/FitsWriter.hpp>
#include <amrexplorer/io/detail/FabHeaderParsing.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>
#include <amrexplorer/data/DatasetSession.hpp>
#include <amrexplorer/core/CoordinateSystem.hpp>
#include <amrexplorer/core/Statistics.hpp>
#include <amrexplorer/pipeline/ParticleProjection.hpp>
#include <amrexplorer/pipeline/SlicePipeline.hpp>
#include <amrexplorer/pipeline/SliceRangeResolver.hpp>
#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/RemoteDatasetSession.hpp>
#include <amrexplorer/render2d/Contours.hpp>
#include <amrexplorer/render2d/ImageBuffer.hpp>
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
#include <QObject>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
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
#include <QPainterPath>
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
#include <QString>
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
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <string>
#include <utility>
#include <vector>

#ifdef AMREXPLORER_QT_TEST_ACCESS
// Used only by the visible_sync_test gate below (test-access builds only).
#include <chrono>
#include <thread>
#endif

namespace amrvis::qt {

// Fed from the project version through a CMake compile definition; the
// fallback covers builds that do not set it (e.g. some IDE integrations).
// Defined once in MainWindow.cpp rather than inline here: AMREXPLORER_VERSION
// is PRIVATE to amrexplorer_qt, so an inline variable would acquire a second,
// conflicting definition in any other target that included this header.
extern const char* const kVersion;

// The single conversion from a rendered ImageBuffer to the QImage the views
// display: ARGB32 over the buffer's rgba, mirrored vertically because plane
// row 0 is the bottom row. Returns a detached copy, so it outlives the buffer.
// Every setImage caller goes through here so the transform has one definition
// (see showSlice, syncVisibleRanges, and
// activeViewRasterMatchesDisplayRangeForTest).
inline QImage displayImageFor(const ImageBuffer& image)
{
    const QImage wrapped(
        reinterpret_cast<const uchar*>(image.rgba.data()),
        image.width, image.height, image.strideBytes, QImage::Format_ARGB32);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    return wrapped.flipped(Qt::Vertical).copy();
#else
    return wrapped.mirrored(false, true).copy();
#endif
}

inline QSettings makeSettings()
{
    return QSettings(QStringLiteral("amrex-codes"), QStringLiteral("amrexplorer"));
}

// An AMReX plotfile directory holds a Header file plus one Level_N
// subdirectory per refinement level (Level_0, Level_1, ...). Detecting by
// structure rather than by a "plt" name prefix avoids false matches.
//
// Every filesystem call here reports through an error_code, including the
// iterator's advance. A range-for cannot: it steps with operator++(), which has
// no error_code overload, so a readdir that fails *after* a successful open --
// an unmounted directory, an NFS mount dropping mid-scan -- threw
// filesystem_error out of a function whose signature promises a bool. Both
// callers -- chooseDataset and openSequence -- run on the GUI thread inside a
// Qt event handler, where an escaping exception terminates the process instead
// of raising a dialog: the user picks a directory and the application dies. A
// directory that cannot be walked is simply not a plotfile.
inline bool isAmrexPlotfile(const std::filesystem::path& directory)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)
        || !std::filesystem::is_regular_file(directory / "Header", ec)) {
        return false;
    }
    std::filesystem::directory_iterator entry(directory, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::directory_iterator end;
    while (entry != end) {
        if (entry->is_directory(ec)
            && entry->path().filename().string().starts_with("Level_")) {
            return true;
        }
        // Checked before the iterator is used again: after a failed advance its
        // value is not something to dereference or compare against end.
        entry.increment(ec);
        if (ec) {
            return false;
        }
    }
    return false;
}

// QString face of the pipeline's formatter, for the GUI-side messages. Named
// apart from amrvis::cacheBudgetDescription so an unqualified call in this
// namespace cannot silently pick the wrong return type.
inline QString cacheBudgetText(std::uint64_t bytes)
{
    return QString::fromStdString(amrvis::cacheBudgetDescription(bytes));
}

inline QString cacheFallbackMessage(
    const DatasetSession& dataset, int fromLevel, int toLevel)
{
    const auto budget = cacheBudgetText(
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

#ifdef AMREXPLORER_QT_TEST_ACCESS
// A gate the visible-range sync worker waits on when armed, so the staleness
// regression test can hold a sync mid-flight, invalidate a panel, then release
// workers one at a time -- first the now-stale sync (to observe it dropped),
// then the self-healing rerun -- and check the transient in between. Grants are
// counted, not a single boolean, precisely so the sync and its rerun can be
// released independently. Compiled only into the test-access build.
namespace visible_sync_test {

inline std::atomic<bool> gateArmed{false};
inline std::atomic<int> releaseGrants{0};  // # of "proceed" grants issued
inline std::atomic<int> passed{0};         // # of workers that have proceeded
inline std::atomic<int> waiting{0};        // # of workers currently parked

inline void waitAtGate()
{
    if (!gateArmed.load()) {
        return;
    }
    waiting.fetch_add(1);
    // Park until a grant is free (passed < grants) -- or the gate is disarmed,
    // or the bounded wait elapses. The bound matters: if the test dies without
    // releasing, the worker must still return so QThreadPool's destructor can
    // join it, rather than hanging into a ctest TIMEOUT that masks the exit code.
    for (int waited = 0; waited < 10000; ++waited) {
        if (!gateArmed.load()
            || passed.load() < releaseGrants.load()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    passed.fetch_add(1);
    waiting.fetch_sub(1);
}

} // namespace visible_sync_test
#endif

} // namespace amrvis::qt
