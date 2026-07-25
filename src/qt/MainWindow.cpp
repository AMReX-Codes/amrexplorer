#include "MainWindow.hpp"
#include "AnimationExporter.hpp"
#include "AnimationPanel.hpp"
#include "CacheConfig.hpp"
#include "ColorBarWidget.hpp"
#include "DatasetWindow.hpp"
#include "FabSelectorDock.hpp"
#include "ImageView.hpp"
#include "IsoWidget.hpp"
#include "LinePlotRequest.hpp"
#include "LinePlotWindow.hpp"
#include "ScientificDoubleSpinBox.hpp"
#include "SetContoursDialog.hpp"
#include "Theme.hpp"
#include "UserGuideDialog.hpp"

#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/io/FabCatalog.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>
#include <amrexplorer/core/Statistics.hpp>
#include <amrexplorer/pipeline/SliceRangeResolver.hpp>
#include <amrexplorer/query/LineQuery.hpp>
#include <amrexplorer/query/SliceQuery.hpp>
#include <amrexplorer/render2d/Contours.hpp>
#include <amrexplorer/render2d/Palette.hpp>
#include <amrexplorer/render2d/ScalarRenderer.hpp>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
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
#include <QLabel>
#include <QListView>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRect>
#include <QSettings>
#include <QShortcut>
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
namespace {

// Sequence frame loads and prefetches get dataset ids from a dedicated range
// so they never collide with the ids openDataset derives from m_generation.
constexpr std::uint64_t sequenceDatasetIdBase = 0x4000000000000000ULL;

constexpr std::array<BuiltinPalette, 7> builtinPalettes{
    BuiltinPalette::Rainbow, BuiltinPalette::Turbo, BuiltinPalette::Viridis,
    BuiltinPalette::Plasma, BuiltinPalette::Parula, BuiltinPalette::Coolwarm,
    BuiltinPalette::Blackbody};
// Menu labels and QSettings keys; kept in sync with builtinPaletteName().
constexpr std::array<const char*, 7> builtinPaletteNames{
    "rainbow", "turbo", "viridis", "plasma", "parula", "coolwarm", "blackbody"};

QImage verticallyFlippedCopy(const QImage& image)
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
QImage displayImageFor(const ImageBuffer& image)
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
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        // Reserve the marker column horizontally and add vertical padding so
        // the names have breathing room; keeps the closed combo unaffected.
        size.setWidth(size.width() + kMarkerColumn);
        size.setHeight(size.height() + kRowVerticalPadding);
        return size;
    }

private:
    static constexpr int kMarkerColumn = 16;
    static constexpr int kRowVerticalPadding = 6;
    QPointer<QComboBox> m_combo;
};

QSettings makeSettings()
{
    return QSettings(QStringLiteral("amrex-codes"), QStringLiteral("amrexplorer"));
}

// An AMReX plotfile directory holds a Header file plus one Level_N
// subdirectory per refinement level (Level_0, Level_1, ...). Detecting by
// structure rather than by a "plt" name prefix avoids false matches.
bool isAmrexPlotfile(const std::filesystem::path& directory)
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
QString exceptionMessage(const std::exception& error)
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

// QString face of the pipeline's formatter, for the GUI-side messages; hides
// amrvis::cacheBudgetDescription for unqualified calls in this namespace.
QString cacheBudgetDescription(std::uint64_t bytes)
{
    return QString::fromStdString(amrvis::cacheBudgetDescription(bytes));
}

QString cacheFallbackMessage(
    const PlotfileDataset& dataset, int fromLevel, int toLevel)
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

bool selectCacheFallbackLevel(QComboBox* selector, int toLevel)
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

void populateLevelCombo(QComboBox* combo, int finestLevel)
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


} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("AMReXplorer"));
    resize(960, 720);

    // The plot area is a stacked widget: page 0 holds the single 2-D view,
    // page 1 the 3-D grid (XY top-left, XZ top-right, YZ bottom-left, iso
    // wireframe bottom-right).
    m_stack = new QStackedWidget(this);

    m_view2d.normal = 1;
    m_view2d.label = QStringLiteral("2-D");
    m_view2d.view = new ImageView(m_stack);
    m_view2d.view->setMinimumSize(320, 240);
    m_view2d.view->setPlaceholder(tr("Open an AMReX dataset to display a slice"));
    m_stack->addWidget(m_view2d.view);

    auto* gridPage = new QWidget(m_stack);
    auto* gridLayout = new QGridLayout(gridPage);
    gridLayout->setSpacing(2);
    gridLayout->setContentsMargins(2, 2, 2, 2);
    constexpr std::array<const char*, 3> viewLabels{"YZ", "XZ", "XY"};
    // Per-panel L-shaped axis indicator in the lower-left corner.
    constexpr std::array<const char*, 3> hAxis{"Y", "X", "X"};
    constexpr std::array<const char*, 3> vAxis{"Z", "Z", "Y"};
    for (int normal = 0; normal < 3; ++normal) {
        const auto idx = static_cast<std::size_t>(normal);
        auto& state = m_planeViews[idx];
        state.normal = normal;
        state.label = QString::fromLatin1(viewLabels[idx]);
        state.view = new ImageView(gridPage);
        state.view->setMinimumSize(200, 150);
        state.view->setSliceMoveEnabled(true);
        state.view->setPlaceholder(tr("%1 view").arg(state.label));
        state.view->setAxisIndicator(
            QString::fromLatin1(hAxis[idx]),
            QString::fromLatin1(vAxis[idx]));
    }
    m_isoWidget = new IsoWidget(gridPage);
    m_isoWidget->setColorPalette(&m_palette);
    gridLayout->addWidget(m_planeViews[2].view, 0, 0);  // XY: plane normal to Z
    gridLayout->addWidget(m_planeViews[1].view, 0, 1);  // XZ: plane normal to Y
    gridLayout->addWidget(m_planeViews[0].view, 1, 0);  // YZ: plane normal to X
    gridLayout->addWidget(m_isoWidget, 1, 1);
    gridLayout->setColumnStretch(0, 1);
    gridLayout->setColumnStretch(1, 1);
    gridLayout->setRowStretch(0, 1);
    gridLayout->setRowStretch(1, 1);
    m_stack->addWidget(gridPage);
    m_stack->setCurrentIndex(0);
    setCentralWidget(m_stack);

    m_sliceToolbar = addToolBar(tr("Slice Controls"));
    auto* sliceToolbar = m_sliceToolbar;
    sliceToolbar->setMovable(false);
    sliceToolbar->addWidget(new QLabel(tr("Field:"), sliceToolbar));
    m_fieldSelector = new QComboBox(sliceToolbar);
    m_fieldSelector->setObjectName(QStringLiteral("fieldSelector"));
    m_fieldSelector->setMinimumContentsLength(10);
    m_fieldSelector->view()->setItemDelegate(new CurrentRowBulletDelegate(
        m_fieldSelector, m_fieldSelector->view()));
    sliceToolbar->addWidget(m_fieldSelector);
    sliceToolbar->addSeparator();
    sliceToolbar->addWidget(new QLabel(tr("Level:"), sliceToolbar));
    m_levelSelector = new QComboBox(sliceToolbar);
    m_levelSelector->setObjectName(QStringLiteral("levelSelector"));
    m_levelSelector->setMinimumContentsLength(8);
    m_levelSelector->view()->setItemDelegate(new CurrentRowBulletDelegate(
        m_levelSelector, m_levelSelector->view()));
    sliceToolbar->addWidget(m_levelSelector);
    sliceToolbar->addSeparator();
    // 3-D shared slice positions: one compact spinbox per axis. The whole
    // group stays hidden for 2-D datasets.
    m_slicePositionControls = new QWidget(sliceToolbar);
    auto* positionLayout = new QHBoxLayout(m_slicePositionControls);
    positionLayout->setContentsMargins(0, 0, 0, 0);
    positionLayout->setSpacing(4);
    positionLayout->addWidget(new QLabel(tr("Position:"), m_slicePositionControls));
    constexpr std::array<const char*, 3> axisLabels{"X:", "Y:", "Z:"};
    for (int axis = 0; axis < 3; ++axis) {
        positionLayout->addWidget(new QLabel(
            QString::fromLatin1(axisLabels[static_cast<std::size_t>(axis)]),
            m_slicePositionControls));
        auto* spin = new QSpinBox(m_slicePositionControls);
        spin->setMinimumWidth(110);
        positionLayout->addWidget(spin);
        m_sliceSpinboxes[static_cast<std::size_t>(axis)] = spin;
        connect(spin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this, axis](int index) {
                if (!m_controlsReady || !m_dataset
                    || m_dataset->metadata().dimension != 3) {
                    return;
                }
                const auto level = sliceIndexLevel();
                if (level < 0 || static_cast<std::size_t>(level)
                    >= m_dataset->metadata().levels.size()) {
                    return;
                }
                setSlicePosition(axis, positionForSliceIndex(
                    m_dataset->metadata(), level, axis, index));
            });
    }
    sliceToolbar->addWidget(m_slicePositionControls);
    m_slicePositionControls->setVisible(false);

    m_scaleButton = new QPushButton(tr("Fit"), sliceToolbar);
    m_scaleButton->setToolTip(
        tr("Zoom scale and rubber-band synchronization for panels"));
    m_scaleButton->setFocusPolicy(Qt::NoFocus);
    auto* scaleMenu = new QMenu(m_scaleButton);
    // "Reset Zoom" restores the whole domain and refits (issue #45 renamed it
    // from "Fit", which read as fit-the-current-region). The button label
    // stays "Fit" as the *scale state*: auto-fit also holds for a panned
    // crop (applyPanStep), where the region is not the whole domain.
    auto* resetZoomAction = scaleMenu->addAction(tr("Reset Zoom"));
    connect(resetZoomAction, &QAction::triggered, this, [this] {
        m_scaleButton->setText(tr("Fit"));
        resetZoomAllViews();
    });
    constexpr std::array<int, 6> scaleFactors{1, 2, 4, 8, 16, 32};
    for (const auto factor : scaleFactors) {
        auto* action = scaleMenu->addAction(tr("%1x").arg(factor));
        connect(action, &QAction::triggered, this, [this, factor] {
            m_scaleButton->setText(tr("%1x").arg(factor));
            for (auto* state : currentViews()) {
                state->view->setFixedScale(factor);
            }
        });
    }
    m_syncRubberBandZoomAction =
        new QAction(tr("Sync Rubber-band Zoom"), this);
    m_syncRubberBandZoomAction->setObjectName(
        QStringLiteral("syncRubberBandZoomAction"));
    m_syncRubberBandZoomAction->setCheckable(true);
    m_syncRubberBandZoomAction->setChecked(true);
    m_syncRubberBandZoomAction->setVisible(false);
    m_syncRubberBandZoomAction->setStatusTip(
        tr("Apply rubber-band selections to every 3-D panel; "
           "mouse-wheel zoom remains panel-specific"));
    connect(m_syncRubberBandZoomAction, &QAction::toggled,
        this, [this](bool) { saveSettings(); });
    scaleMenu->addSeparator();
    scaleMenu->addAction(m_syncRubberBandZoomAction);
    m_scaleButton->setMenu(scaleMenu);
    sliceToolbar->addWidget(m_scaleButton);

    addToolBarBreak(Qt::TopToolBarArea);
    m_rangeToolbar = addToolBar(tr("Color and Overlay Controls"));
    auto* rangeToolbar = m_rangeToolbar;
    rangeToolbar->setMovable(false);
    rangeToolbar->addWidget(new QLabel(tr("Range:"), rangeToolbar));
    m_rangeMode = new QComboBox(rangeToolbar);
    m_rangeMode->setObjectName(QStringLiteral("rangeModeSelector"));
    m_rangeMode->addItem(tr("File"), static_cast<int>(RangeMode::File));
    m_rangeMode->addItem(tr("Level"), static_cast<int>(RangeMode::Level));
    m_rangeMode->addItem(tr("Visible"), static_cast<int>(RangeMode::Visible));
    m_rangeMode->addItem(tr("User"), static_cast<int>(RangeMode::User));
    rangeToolbar->addWidget(m_rangeMode);
    m_rangeMinimum = new ScientificDoubleSpinBox(rangeToolbar);
    m_rangeMaximum = new ScientificDoubleSpinBox(rangeToolbar);
    for (auto* range : {m_rangeMinimum, m_rangeMaximum}) {
        range->setRange(-std::numeric_limits<double>::max(),
            std::numeric_limits<double>::max());
        range->setMinimumWidth(110);
        range->setEnabled(false);
        rangeToolbar->addWidget(range);
    }
    m_rangeMinimum->setPrefix(tr("min "));
    m_rangeMaximum->setPrefix(tr("max "));
    m_rangeMaximum->setValue(1.0);
    m_logarithmic = new QCheckBox(tr("Log"), rangeToolbar);
    m_logarithmic->setLayoutDirection(Qt::RightToLeft);
    rangeToolbar->addWidget(m_logarithmic);
    auto* paletteSpacer = new QWidget(rangeToolbar);
    paletteSpacer->setFixedWidth(12);
    rangeToolbar->addWidget(paletteSpacer);
    rangeToolbar->addWidget(new QLabel(tr("Palette:"), rangeToolbar));
    m_paletteSelector = new QComboBox(rangeToolbar);
    const QFontMetrics paletteFm(m_paletteSelector->font());
    int widestBuiltin = 0;
    for (std::size_t index = 0; index < builtinPalettes.size(); ++index) {
        const auto raw = builtinPaletteName(builtinPalettes[index]);
        auto label = QString::fromLatin1(raw.data(),
            static_cast<qsizetype>(raw.size()));
        if (!label.isEmpty()) {
            label[0] = label[0].toUpper();
        }
        widestBuiltin = std::max(widestBuiltin, paletteFm.horizontalAdvance(label));
        m_paletteSelector->addItem(label, static_cast<int>(index));
    }
    // Size the closed combo to exactly fit the longest builtin name (the popup
    // expands independently, so the "Load Palette File..." / custom entries are
    // never truncated there). Any custom entry shows elided when closed.
    QStyleOptionComboBox comboBoxOption;
    comboBoxOption.initFrom(m_paletteSelector);
    const QSize content(widestBuiltin + 4, paletteFm.height());
    m_paletteSelector->setFixedWidth(m_paletteSelector->style()->sizeFromContents(
        QStyle::CT_ComboBox, &comboBoxOption, content, m_paletteSelector).width());
    m_paletteSelector->view()->setItemDelegate(new CurrentRowBulletDelegate(
        m_paletteSelector, m_paletteSelector->view()));
    connect(m_paletteSelector, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) {
            const auto selection = m_paletteSelector->currentData().toInt();
            if (selection >= 0) {
                selectBuiltinPalette(selection);
            }
            // selection == -2 is the transient "Custom: <file>" entry added by
            // syncPaletteSelector(); selecting it is a no-op.
        });
    rangeToolbar->addWidget(m_paletteSelector);

    m_sliceDebounce = new QTimer(this);
    m_sliceDebounce->setSingleShot(true);
    m_sliceDebounce->setInterval(100);
    connect(m_sliceDebounce, &QTimer::timeout, this, [this] { flushSliceRequests(); });
    m_panDebounce = new QTimer(this);
    m_panDebounce->setSingleShot(true);
    m_panDebounce->setInterval(120);
    connect(m_panDebounce, &QTimer::timeout, this, [this] { flushPanDrag(false); });
    connect(m_fieldSelector, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int index) {
            // Swap the per-field range snapshot before re-slicing. This only
            // fires on a real user selection -- per-frame repopulation during
            // animation blocks signals and preserves the index, so the range
            // stays constant across frames.
            if (m_controlsReady && index >= 0) {
                const auto newField = m_fieldSelector->itemData(index).toUInt();
                if (newField != m_trackedField) {
                    commitFieldRange(m_trackedField);
                    m_trackedField = newField;
                    applyFieldRange(newField);
                }
            }
            updateRangeModeAvailability();
            scheduleSliceRequest();
        });
    connect(m_levelSelector, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) {
            configureSlicePositionControls();
            updateRangeModeAvailability();
            scheduleSliceRequest();
        });
    connect(m_rangeMode, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) {
            updateRangeModeAvailability();
            const auto userRange = static_cast<RangeMode>(
                m_rangeMode->currentData().toInt()) == RangeMode::User;
            m_rangeMinimum->setEnabled(userRange && m_controlsReady);
            m_rangeMaximum->setEnabled(userRange && m_controlsReady);
            scheduleSliceRequest();
        });
    connect(m_rangeMinimum, qOverload<double>(&QDoubleSpinBox::valueChanged),
        this, [this](double) {
            if (static_cast<RangeMode>(m_rangeMode->currentData().toInt())
                == RangeMode::User) {
                scheduleSliceRequest();
            }
        });
    connect(m_rangeMaximum, qOverload<double>(&QDoubleSpinBox::valueChanged),
        this, [this](double) {
            if (static_cast<RangeMode>(m_rangeMode->currentData().toInt())
                == RangeMode::User) {
                scheduleSliceRequest();
            }
        });
    connect(m_logarithmic, &QCheckBox::toggled,
        this, [this](bool) { scheduleSliceRequest(); });
    m_fieldSelector->setEnabled(false);
    m_levelSelector->setEnabled(false);
    m_rangeMode->setEnabled(false);
    m_logarithmic->setEnabled(false);

    m_metadataDock = new QDockWidget(tr("Dataset Metadata"), this);
    m_metadataTree = new QTreeWidget(m_metadataDock);
    m_metadataTree->setColumnCount(2);
    m_metadataTree->setHeaderLabels({tr("Property"), tr("Value")});
    m_metadataTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_metadataTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_metadataDock->setWidget(m_metadataTree);
    addDockWidget(Qt::LeftDockWidgetArea, m_metadataDock);
    m_metadataDock->setVisible(false);

    m_diagnosticsDock = new QDockWidget(tr("Diagnostics"), this);
    m_diagnostics = new QPlainTextEdit(m_diagnosticsDock);
    m_diagnostics->setReadOnly(true);
    m_diagnosticsDock->setWidget(m_diagnostics);
    addDockWidget(Qt::BottomDockWidgetArea, m_diagnosticsDock);
    m_diagnosticsDock->setVisible(false);

    m_colorBarDock = new QDockWidget(tr("Color Scale"), this);
    m_colorBar = new ColorBarWidget(m_colorBarDock);
    m_colorBarDock->setWidget(m_colorBar);
    addDockWidget(Qt::RightDockWidgetArea, m_colorBarDock);

    m_animationDock = new QDockWidget(tr("Animation"), this);
    m_animationPanel = new AnimationPanel(m_animationDock);
    m_animationDock->setWidget(m_animationPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_animationDock);
    // Shown only for 3-D datasets (slice sweep) or plotfile sequences; hidden
    // until updateAnimationDockVisibility() decides otherwise.
    m_animationDock->setVisible(false);

    m_fabSelectorDock = new FabSelectorDock(this);
    addDockWidget(Qt::LeftDockWidgetArea, m_fabSelectorDock);
    m_fabSelectorDock->setVisible(false);
    connect(m_fabSelectorDock, &FabSelectorDock::viewRequested,
        this, [this](std::size_t entry) { viewFab(entry); });
    connect(m_fabSelectorDock, &FabSelectorDock::backRequested,
        this, &MainWindow::backToMultiFab);

    // One playback timer drives either animation mode; starting one mode
    // stops the other (see setPlaybackMode).
    m_playbackTimer = new QTimer(this);
    connect(m_playbackTimer, &QTimer::timeout, this, [this] { playbackTick(); });
    // Animation export advances one frame at a time as each renders. The
    // exporter owns the whole export state machine; this window supplies
    // frame rendering and navigation, and restores its UI on finished().
    m_animationExporter = new AnimationExporter(
        [this](bool includeColorBar, qreal scale) {
            std::vector<std::pair<QString, QImage>> frames;
            if (m_viewDimension == 3) {
                constexpr std::array<const char*, 3> suffixes{
                    "_yz", "_xz", "_xy"};
                for (int normal = 0; normal < 3; ++normal) {
                    const auto idx = static_cast<std::size_t>(normal);
                    auto* panelView = m_planeViews[idx].view;
                    if (panelView == nullptr || !panelView->hasImage()) {
                        continue;
                    }
                    frames.emplace_back(QString::fromLatin1(suffixes[idx]),
                        composeExportFrame(panelView, includeColorBar, scale));
                }
            } else {
                frames.emplace_back(QString(), composeExportFrame(
                    m_activeView != nullptr ? m_activeView->view : nullptr,
                    includeColorBar, scale));
            }
            return frames;
        },
        [this](int index) { goToSequenceFrame(index); },
        this);
    connect(m_animationExporter, &AnimationExporter::encodingStarted,
        this, &MainWindow::exportEncodingStarted);
    connect(m_animationExporter, &AnimationExporter::finished, this,
        [this](bool success, const QString& message, int restoreIndex) {
            // Return the user to the frame they were viewing (unless we are
            // closing, which would launch a new frame load mid-shutdown).
            if (!m_closing && !m_sequenceFrames.empty()) {
                goToSequenceFrame(restoreIndex < 0 ? 0 : restoreIndex);
            }
            m_exportAnimationAction->setEnabled(!m_sequenceFrames.empty());
            if (!m_closing) {
                if (success) {
                    QMessageBox::information(
                        this, tr("Export Animation"), message);
                } else {
                    reportBackgroundError(message);
                }
            }
        });
    connect(this, &MainWindow::sequenceFrameDisplayed,
        m_animationExporter, &AnimationExporter::onFrameDisplayed);
    connect(this, &MainWindow::sequenceFrameFailed,
        m_animationExporter, &AnimationExporter::onFrameFailed);
    applySpeed();
    connect(m_animationPanel, &AnimationPanel::sweepStepRequested, this,
        [this](int direction) { stepSweep(direction); });
    connect(m_animationPanel, &AnimationPanel::sweepPlayToggled, this,
        [this] { toggleSweepPlayback(); });
    connect(m_animationPanel, &AnimationPanel::sequenceStepRequested, this,
        [this](int direction) { stepSequence(direction); });
    connect(m_animationPanel, &AnimationPanel::sequencePlayToggled, this,
        [this] { toggleSequencePlayback(); });
    connect(m_animationPanel, &AnimationPanel::sequenceFrameRequested, this,
        [this](int index) { goToSequenceFrame(index); });
    connect(m_animationPanel, &AnimationPanel::speedChanged, this,
        [this](int) {
            applySpeed();
            saveSettings();
        });

    createMenus();

    connect(m_fieldSelector, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) {
            syncMenuChecks();
            syncVariableMenu();
        });
    connect(m_levelSelector, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) { syncMenuChecks(); });
    connect(m_rangeMode, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) { saveSettings(); });
    connect(m_logarithmic, &QCheckBox::toggled,
        this, [this](bool) { saveSettings(); });

    wireView(m_view2d);
    for (auto& state : m_planeViews) {
        wireView(state);
    }
    setupPanShortcuts();

    m_probeLabel = new QLabel(statusBar());
    statusBar()->addPermanentWidget(m_probeLabel);
    statusBar()->showMessage(tr("No dataset open"));
    updateDiagnostics();
    restoreSettings();
    // Cancel in-flight async work on any quit path (last-window close, Cmd-Q,
    // menu Quit) so QThreadPool teardown does not block on an outstanding read
    // and the process can exit promptly.
    connect(qApp, &QCoreApplication::aboutToQuit, this,
        &MainWindow::cancelInFlight);
}

void MainWindow::wireView(PlaneViewState& state)
{
    auto* view = state.view;
    view->setFocusPolicy(Qt::StrongFocus);
    connect(view, &ImageView::probeClicked, this,
        [this, &state](int x, int displayY) { probeClicked(state, x, displayY); });
    connect(view, &ImageView::probeMoved, this,
        [this, &state](int x, int displayY) { probeMoved(state, x, displayY); });
    connect(view, &ImageView::rubberBandSelected, this,
        [this, &state](const QRectF& sceneRect) { rubberBandZoom(state, sceneRect); });
    connect(view, &ImageView::panDragBegan, this,
        [this, &state] { beginPanDrag(state); });
    connect(view, &ImageView::panDragMoved, this,
        [this, &state](const QPointF& totalSceneDelta, const QPoint& viewportDelta) {
            updatePanDrag(state, totalSceneDelta, viewportDelta);
        });
    connect(view, &ImageView::panDragEnded, this,
        [this, &state](const QPointF& totalSceneDelta) {
            endPanDrag(state, totalSceneDelta);
        });
    connect(view, &ImageView::linePlotRequested, this,
        [this, &state](int x, int y, Qt::MouseButton button) {
            linePlotRequested(state, x, y, button);
        });
    connect(view, &ImageView::sliceMoveRequested, this,
        [this, &state](int x, int y, Qt::MouseButton button) {
            sliceMoveRequested(state, x, y, button);
        });
    connect(view, &ImageView::fitRequested, this,
        [this, &state] { resetViewZoom(state); });
}

std::vector<MainWindow::PlaneViewState*> MainWindow::currentViews()
{
    if (m_viewDimension == 3) {
        return {&m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
    }
    if (m_viewDimension == 2) {
        return {&m_view2d};
    }
    return {};
}

void MainWindow::setActiveView(PlaneViewState& state)
{
    if (m_activeView == &state) {
        return;
    }
    if (m_activeView != nullptr && m_viewDimension == 3) {
        m_activeView->view->setActiveBorder(false);
    }
    m_activeView = &state;
    if (m_viewDimension == 3) {
        state.view->setActiveBorder(true);
    }
    if (state.plane.width <= 0 || state.plane.height <= 0) {
        return;
    }
    // The color scale and range boxes track the active view.
    m_colorBar->setLogarithmic(state.displayLogarithmic);
    m_colorBar->setFieldRange(state.displayLogarithmic
        ? state.fieldName + tr(" (log)") : state.fieldName,
        state.displayMinimum, state.displayMaximum);
    if (m_logarithmic->isChecked() != state.displayLogarithmic) {
        const QSignalBlocker logarithmicBlocker(m_logarithmic);
        m_logarithmic->setChecked(state.displayLogarithmic);
    }
    if (static_cast<RangeMode>(m_rangeMode->currentData().toInt())
        != RangeMode::User) {
        const QSignalBlocker minimumBlocker(m_rangeMinimum);
        const QSignalBlocker maximumBlocker(m_rangeMaximum);
        m_rangeMinimum->setValue(state.displayMinimum);
        m_rangeMaximum->setValue(state.displayMaximum);
    }
}

std::array<int, 2> MainWindow::displayAxes(int normal) const
{
    std::array<int, 2> axes{0, 1};
    if (m_dataset && m_dataset->metadata().dimension == 3) {
        std::size_t next = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (axis != normal) {
                axes[next++] = axis;
            }
        }
    }
    return axes;
}

void MainWindow::createMenus()
{
    auto* newWindowAction = new QAction(tr("Open &New Window"), this);
    newWindowAction->setShortcut(QKeySequence::New);
    connect(newWindowAction, &QAction::triggered, this, [this] { createNewWindow(); });

    auto* openAction = new QAction(tr("&Open Plotfile Directory..."), this);
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, [this] { chooseDataset(); });

    auto* openSequenceAction = new QAction(tr("Open Plotfile &Sequence..."), this);
    connect(openSequenceAction, &QAction::triggered, this,
        [this] { choosePlotfileSequence(); });

    auto* openFabAction = new QAction(tr("Open &FAB..."), this);
    connect(openFabAction, &QAction::triggered, this,
        [this] { chooseStandaloneDataset(tr("Open AMReX FAB"), true); });

    auto* openMultiFabAction = new QAction(tr("Open &MultiFab..."), this);
    connect(openMultiFabAction, &QAction::triggered, this,
        [this] {
            chooseStandaloneDataset(tr("Open AMReX MultiFab header"), false);
        });

    m_paletteGroup = new QActionGroup(this);
    auto* paletteMenu = new QMenu(tr("&Palette"), this);
    for (std::size_t index = 0; index < builtinPalettes.size(); ++index) {
        const auto fileName = builtinPaletteName(builtinPalettes[index]);
        auto* action = new QAction(QString::fromLatin1(fileName.data(),
            static_cast<qsizetype>(fileName.size())), paletteMenu);
        action->setCheckable(true);
        action->setActionGroup(m_paletteGroup);
        connect(action, &QAction::triggered, this,
            [this, index] { selectBuiltinPalette(static_cast<int>(index)); });
        paletteMenu->addAction(action);
    }
    paletteMenu->addSeparator();
    auto* loadPaletteAction = paletteMenu->addAction(tr("&Load Palette File..."));
    connect(loadPaletteAction, &QAction::triggered, this, [this] { loadPaletteFile(); });

    auto* exportAction = new QAction(tr("&Export Image..."), this);
    connect(exportAction, &QAction::triggered, this, [this] { exportImage(); });

    m_exportAnimationAction = new QAction(tr("Export &Animation..."), this);
    m_exportAnimationAction->setEnabled(false);
    connect(m_exportAnimationAction, &QAction::triggered,
        this, [this] { exportAnimation(); });

    auto* quitAction = new QAction(tr("&Quit"), this);
    quitAction->setShortcut(QKeySequence::Quit);
    // Application-wide: close every main window (each runs its own close
    // handling) rather than just this one.
    connect(quitAction, &QAction::triggered, qApp, &QApplication::closeAllWindows);

    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(newWindowAction);
    fileMenu->addSeparator();
    fileMenu->addAction(openAction);
    fileMenu->addAction(openSequenceAction);
    fileMenu->addAction(openFabAction);
    fileMenu->addAction(openMultiFabAction);
    fileMenu->addSeparator();
    fileMenu->addAction(exportAction);
    fileMenu->addAction(m_exportAnimationAction);
    fileMenu->addSeparator();
    fileMenu->addAction(quitAction);

    m_scaleGroup = new QActionGroup(this);
    auto* scaleMenu = new QMenu(tr("&Scale"), this);
    m_resetZoomAction = new QAction(tr("&Reset Zoom"), scaleMenu);
    m_resetZoomAction->setCheckable(true);
    m_resetZoomAction->setActionGroup(m_scaleGroup);
    m_resetZoomAction->setChecked(true);
    m_resetZoomAction->setShortcut(QKeySequence(Qt::Key_0));
    connect(m_resetZoomAction, &QAction::triggered,
        this, [this] { resetZoomAllViews(); });
    scaleMenu->addAction(m_resetZoomAction);
    constexpr std::array<int, 6> fixedScales{1, 2, 4, 8, 16, 32};
    for (std::size_t index = 0; index < fixedScales.size(); ++index) {
        const auto factor = fixedScales[index];
        auto* action = new QAction(tr("%1x").arg(factor), scaleMenu);
        action->setCheckable(true);
        action->setActionGroup(m_scaleGroup);
        action->setShortcut(QKeySequence(Qt::Key_1 + static_cast<int>(index)));
        connect(action, &QAction::triggered, this, [this, factor] {
            if (m_scaleButton != nullptr) {
                m_scaleButton->setText(tr("%1x").arg(factor));
            }
            for (auto* state : currentViews()) {
                state->view->setFixedScale(factor);
            }
        });
        scaleMenu->addAction(action);
    }
    scaleMenu->addSeparator();
    scaleMenu->addAction(m_syncRubberBandZoomAction);

    m_levelMenu = new QMenu(tr("&Level"), this);
    m_levelGroup = new QActionGroup(this);
    m_levelMenu->setEnabled(false);

    m_boxesAction = new QAction(tr("&Boxes"), this);
    m_boxesAction->setCheckable(true);
    m_boxesAction->setShortcuts(
        {QKeySequence(Qt::Key_B), QKeySequence(Qt::SHIFT | Qt::Key_B)});
    m_boxesAction->setEnabled(false);
    connect(m_boxesAction, &QAction::toggled, this, [this](bool) {
        updateGridBoxes();
        saveSettings();
    });
    m_slicePlanesAction = new QAction(tr("&Slice Planes"), this);
    m_slicePlanesAction->setCheckable(true);
    m_slicePlanesAction->setEnabled(false);
    connect(m_slicePlanesAction, &QAction::toggled, this,
        [this](bool visible) { m_isoWidget->setSlicePlanesVisible(visible); });

    m_contoursAction = new QAction(tr("&Contours..."), this);
    m_contoursAction->setEnabled(false);
    connect(m_contoursAction, &QAction::triggered,
        this, [this] { showContoursDialog(); });

    m_datasetAction = new QAction(tr("&Dataset..."), this);
    m_datasetAction->setEnabled(false);
    m_datasetAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    connect(m_datasetAction, &QAction::triggered,
        this, [this] { showDatasetWindow(); });

    // Legacy View menu order: Contours..., Range..., Dataset..., Number
    // Format... (the range lives in the toolbar here, not in a dialog).
    auto* numberFormatAction = new QAction(tr("&Number Format..."), this);
    connect(numberFormatAction, &QAction::triggered,
        this, [this] { showNumberFormatDialog(); });

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addMenu(scaleMenu);
    viewMenu->addMenu(m_levelMenu);
    viewMenu->addAction(m_boxesAction);
    viewMenu->addAction(m_slicePlanesAction);
    viewMenu->addMenu(paletteMenu);
    viewMenu->addSeparator();
    viewMenu->addAction(m_contoursAction);
    viewMenu->addAction(m_datasetAction);
    viewMenu->addAction(numberFormatAction);
    viewMenu->addSeparator();
    // Toolbar visibility toggles.
    viewMenu->addAction(m_sliceToolbar->toggleViewAction());
    viewMenu->addAction(m_rangeToolbar->toggleViewAction());
    viewMenu->addSeparator();
    // Panel visibility toggles. Color Scale is visible by default; Dataset
    // Metadata and Diagnostics start hidden, and Animation is auto-shown for
    // 3-D datasets and plotfile sequences.
    viewMenu->addAction(m_metadataDock->toggleViewAction());
    viewMenu->addAction(m_colorBarDock->toggleViewAction());
    viewMenu->addAction(m_diagnosticsDock->toggleViewAction());
    viewMenu->addAction(m_animationDock->toggleViewAction());
    viewMenu->addAction(m_fabSelectorDock->toggleViewAction());

    // Variable menu: lists all fields with a bullet on the active one.
    m_variableMenu = menuBar()->addMenu(tr("&Variable"));
    m_variableGroup = new QActionGroup(this);

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    auto* guideAction = new QAction(tr("&User Guide..."), this);
    guideAction->setShortcut(QKeySequence::HelpContents);
    connect(guideAction, &QAction::triggered,
        this, [this] { showUserGuide(); });
    auto* referenceAction = new QAction(tr("&Keyboard && Mouse..."), this);
    connect(referenceAction, &QAction::triggered,
        this, [this] { showKeyboardMouseReference(); });
    auto* aboutAction = new QAction(tr("&About AMReXplorer..."), this);
    connect(aboutAction, &QAction::triggered, this, [this] { showAboutDialog(); });
    helpMenu->addAction(guideAction);
    helpMenu->addAction(referenceAction);
    helpMenu->addSeparator();
    helpMenu->addAction(aboutAction);
}

void MainWindow::rebuildLevelMenu()
{
    m_levelMenu->clear();
    if (!m_dataset) {
        return;
    }
    const auto& metadata = m_dataset->metadata();
    auto* finest = new QAction(tr("Finest available"), m_levelMenu);
    finest->setCheckable(true);
    finest->setActionGroup(m_levelGroup);
    finest->setData(-1);
    {
        QList<QKeySequence> finestShortcuts{QKeySequence(Qt::CTRL | Qt::Key_0)};
        if (metadata.finestLevel >= 1 && metadata.finestLevel <= 9) {
            finestShortcuts.append(QKeySequence(
                Qt::CTRL | static_cast<Qt::Key>(Qt::Key_0 + metadata.finestLevel)));
        }
        finest->setShortcuts(finestShortcuts);
    }
    connect(finest, &QAction::triggered, this, [this] {
        const auto index = m_levelSelector->findData(-1);
        if (index >= 0) {
            m_levelSelector->setCurrentIndex(index);
        }
    });
    m_levelMenu->addAction(finest);
    // "Levs 0-N" entries, descending, only when there are at least three levels.
    for (int level = metadata.finestLevel - 1; level >= 1; --level) {
        const auto comboData = kUpdateToLevelOffset + level;
        auto* action = new QAction(tr("Levs 0-%1").arg(level), m_levelMenu);
        action->setCheckable(true);
        action->setActionGroup(m_levelGroup);
        action->setData(comboData);
        if (level < 10) {
            action->setShortcut(QKeySequence(
                Qt::CTRL | static_cast<Qt::Key>(Qt::Key_0 + level)));
        }
        connect(action, &QAction::triggered, this, [this, comboData] {
            const auto index = m_levelSelector->findData(comboData);
            if (index >= 0) {
                m_levelSelector->setCurrentIndex(index);
            }
        });
        m_levelMenu->addAction(action);
    }
    // "Level N only" is redundant for a single-level dataset (mirrors
    // populateLevelCombo, which also drops these for finestLevel == 0).
    if (metadata.finestLevel > 0) {
        for (int level = 0; level <= metadata.finestLevel; ++level) {
            auto* action = new QAction(tr("Level %1 only").arg(level), m_levelMenu);
            action->setCheckable(true);
            action->setActionGroup(m_levelGroup);
            action->setData(level);
            if (level < 10) {
                action->setShortcut(QKeySequence(
                    Qt::ALT | static_cast<Qt::Key>(Qt::Key_0 + level)));
            }
            connect(action, &QAction::triggered, this, [this, level] {
                const auto index = m_levelSelector->findData(level);
                if (index >= 0) {
                    m_levelSelector->setCurrentIndex(index);
                }
            });
            m_levelMenu->addAction(action);
        }
    }
    syncMenuChecks();
}

void MainWindow::syncMenuChecks()
{
    const auto currentData = m_levelSelector->currentData().toInt();
    const auto levelActions = m_levelMenu->actions();
    for (auto* action : levelActions) {
        action->setChecked(action->data().toInt() == currentData);
    }
}

void MainWindow::rebuildVariableMenu()
{
    m_variableMenu->clear();
    if (!m_dataset) {
        m_variableMenu->setEnabled(false);
        return;
    }
    m_variableMenu->setEnabled(true);
    const auto& metadata = m_dataset->metadata();
    const auto currentField = m_fieldSelector->currentIndex() >= 0
        ? m_fieldSelector->currentData().toUInt() : 0;
    for (std::size_t field = 0; field < metadata.fields.size(); ++field) {
        const auto name = QString::fromStdString(metadata.fields[field].name);
        auto* action = m_variableMenu->addAction(name);
        action->setCheckable(true);
        action->setActionGroup(m_variableGroup);
        action->setChecked(static_cast<std::uint32_t>(field) == currentField);
        action->setData(static_cast<unsigned int>(field));
        connect(action, &QAction::triggered, this, [this, field] {
            const auto index = m_fieldSelector->findData(
                static_cast<unsigned int>(field));
            if (index >= 0) {
                m_fieldSelector->setCurrentIndex(index);
            }
        });
    }
}

void MainWindow::syncVariableMenu()
{
    if (!m_dataset) {
        return;
    }
    const auto currentField = m_fieldSelector->currentIndex() >= 0
        ? m_fieldSelector->currentData().toUInt() : 0;
    const auto actions = m_variableMenu->actions();
    for (int i = 0; i < actions.size(); ++i) {
        actions[i]->setChecked(
            static_cast<std::uint32_t>(i) == currentField);
    }
}

void MainWindow::syncPaletteChecks()
{
    const auto actions = m_paletteGroup->actions();
    for (int index = 0; index < actions.size(); ++index) {
        actions[index]->setChecked(!m_paletteFromFile && index == m_builtinIndex);
    }
}

void MainWindow::syncPaletteSelector()
{
    const QSignalBlocker blocker(m_paletteSelector);
    // Drop any stale "custom palette file" entry before reconciling.
    const int custom = m_paletteSelector->findData(-2);
    if (custom >= 0) {
        m_paletteSelector->removeItem(custom);
    }
    if (m_paletteFromFile) {
        const auto label =
            tr("Custom: %1").arg(QFileInfo(m_paletteFilePath).fileName());
        // Insert just after the builtins (and before the separator) so the
        // "Load Palette File..." entry stays anchored at the bottom.
        m_paletteSelector->insertItem(
            static_cast<int>(builtinPalettes.size()), label, -2);
        m_paletteSelector->setCurrentIndex(m_paletteSelector->findData(-2));
    } else {
        m_paletteSelector->setCurrentIndex(
            m_paletteSelector->findData(m_builtinIndex));
    }
}

void MainWindow::selectBuiltinPalette(int index)
{
    if (index < 0 || index >= static_cast<int>(builtinPalettes.size())) {
        return;
    }
    applyPalette(builtinPalette(builtinPalettes[static_cast<std::size_t>(index)]),
        index, QString());
}

void MainWindow::loadPaletteFile()
{
    const auto settings = makeSettings();
    const auto filename = QFileDialog::getOpenFileName(this,
        tr("Load Palette File"),
        settings.value(QStringLiteral("lastOpenDirectory")).toString(),
        tr("Legacy palette files (*.pal);;All files (*)"));
    if (filename.isEmpty()) {
        return;
    }
    try {
        applyPalette(Palette::load(filename.toStdString()), std::nullopt, filename);
        auto writableSettings = makeSettings();
        writableSettings.setValue(QStringLiteral("lastOpenDirectory"),
            QFileInfo(filename).absolutePath());
    } catch (const std::exception& error) {
        QMessageBox::critical(this, tr("Cannot load palette"),
            QString::fromUtf8(error.what()));
    }
}

void MainWindow::applyPalette(const Palette& palette, std::optional<int> builtinIndex,
    const QString& filePath)
{
    m_palette = palette;
    m_paletteFromFile = !builtinIndex.has_value();
    if (builtinIndex.has_value()) {
        m_builtinIndex = *builtinIndex;
        m_paletteFilePath.clear();
    } else {
        m_paletteFilePath = filePath;
    }
    m_colorBar->setPalette(&m_palette);
    syncPaletteChecks();
    syncPaletteSelector();
    saveSettings();
    scheduleSliceRequest();
    updateGridBoxes();
    updateOverlays();
    updateCrosshairs();
    m_isoWidget->update();
}

void MainWindow::showContoursDialog()
{
    if (!m_dataset) {
        return;
    }
    if (m_contoursDialog != nullptr) {
        m_contoursDialog->raise();
        m_contoursDialog->activateWindow();
        return;
    }
    const auto& fields = m_dataset->metadata().fields;
    std::vector<std::string> fieldNames;
    fieldNames.reserve(fields.size());
    for (const auto& field : fields) {
        fieldNames.push_back(field.name);
    }
    auto* dialog = new SetContoursDialog(fieldNames,
        m_viewDimension == 3, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMode(m_displayMode);
    dialog->setContourCount(m_contourCount);
    dialog->setVectorFields(m_vectorUField, m_vectorVField, m_vectorWField);
    dialog->setContourColor(m_contourColor);
    connect(dialog, &SetContoursDialog::applied, this, [this, dialog] {
        applyContourSettings(dialog->mode(), dialog->contourCount(),
            dialog->uField(), dialog->vField(), dialog->wField(),
            dialog->contourColor());
    });
    connect(dialog, &QDialog::finished, this, [this] {
        m_contoursDialog = nullptr;
    });
    m_contoursDialog = dialog;
    dialog->show();
}

void MainWindow::applyContourSettings(
    DisplayMode mode, int count, int uField, int vField, int wField,
    int contourColor)
{
    const auto previousMode = m_displayMode;
    const auto previousCount = m_contourCount;
    const auto previousUField = m_vectorUField;
    const auto previousVField = m_vectorVField;
    const auto previousWField = m_vectorWField;
    m_displayMode = mode;
    m_contourCount = count;
    m_vectorUField = uField;
    m_vectorVField = vField;
    m_vectorWField = wField;
    m_contourColor = contourColor;
    if (mode == DisplayMode::VelocityVectors) {
        ensureVectorFieldDefaults();
    }
    saveSettings();
    const auto involvesVectors = mode == DisplayMode::VelocityVectors
        || previousMode == DisplayMode::VelocityVectors;
    const auto inputsChanged = mode != previousMode || count != previousCount
        || uField != previousUField || vField != previousVField
        || wField != previousWField;
    if (inputsChanged) {
        if (involvesVectors) {
            for (auto* state : currentViews()) {
                state->vectorSegments.clear();
                state->view->setOverlaySegments({});
            }
        }
        scheduleSliceRequest(false);
    } else {
        updateOverlays();
    }
}

void MainWindow::configureContourSyncForTest(
    int count, bool logarithmic, std::array<double, 3> slicePositions)
{
    if (!m_dataset) {
        return;
    }
    m_slicePosition3d = slicePositions;
    // Set range/log through the widgets (requestSlice reads them) but block
    // their signals so only the single scheduleSliceRequest below re-slices.
    {
        const QSignalBlocker rangeBlocker(m_rangeMode);
        const auto index = m_rangeMode->findData(
            static_cast<int>(RangeMode::Visible));
        if (index >= 0) {
            m_rangeMode->setCurrentIndex(index);
        }
    }
    {
        const QSignalBlocker logBlocker(m_logarithmic);
        m_logarithmic->setChecked(logarithmic);
    }
    m_displayMode = DisplayMode::RasterContours;
    m_contourCount = count;
    scheduleSliceRequest(false);
}

std::vector<MainWindow::ContourViewProbe>
MainWindow::contourViewProbesForTest()
{
    std::vector<ContourViewProbe> probes;
    for (const auto* state : currentViews()) {
        ContourViewProbe probe;
        probe.displayMinimum = state->displayMinimum;
        probe.displayMaximum = state->displayMaximum;
        probe.logarithmic = state->displayLogarithmic;
        for (const auto& polyline : state->contourPolylines) {
            probe.contourLevels.push_back(polyline.value);
        }
        std::sort(probe.contourLevels.begin(), probe.contourLevels.end());
        probe.contourLevels.erase(
            std::unique(probe.contourLevels.begin(), probe.contourLevels.end()),
            probe.contourLevels.end());
        probes.push_back(std::move(probe));
    }
    return probes;
}

void MainWindow::enableVisibleRasterForTest()
{
    if (!m_dataset) {
        return;
    }
    {
        const QSignalBlocker rangeBlocker(m_rangeMode);
        const auto index = m_rangeMode->findData(
            static_cast<int>(RangeMode::Visible));
        if (index >= 0) {
            m_rangeMode->setCurrentIndex(index);
        }
    }
    m_displayMode = DisplayMode::Raster;
    scheduleSliceRequest(false);
}

void MainWindow::zoomActiveViewForTest()
{
    if (!m_dataset || m_activeView == nullptr) {
        return;
    }
    const auto bounds = datasetSampleBounds(m_dataset->metadata());
    auto subregion = bounds;
    const auto axes = displayAxes(m_activeView->normal);
    for (std::size_t k = 0; k < 2; ++k) {
        const auto axis = static_cast<std::size_t>(axes[k]);
        subregion.lower[axis] = 0.5 * (bounds.lower[axis] + bounds.upper[axis]);
        subregion.upper[axis] = bounds.upper[axis];
    }
    m_activeView->visibleRegion = subregion;
    scheduleSliceRequest(*m_activeView, true);
}

bool MainWindow::activeViewRasterMatchesDisplayRangeForTest()
{
    if (m_activeView == nullptr) {
        return false;
    }
    const auto& state = *m_activeView;
    if (state.plane.width <= 0 || state.plane.height <= 0
        || !state.view->hasImage()) {
        return false;
    }
    const auto reference = renderScalarPlane(state.plane, ScalarRenderSettings{
        .minimum = state.displayMinimum,
        .maximum = state.displayMaximum,
        .logarithmic = state.displayLogarithmic,
        .palette = &m_palette
    });
    if (!reference.valid()) {
        return false;
    }
    // Same buffer->view transform showSlice uses, so this stays in lockstep
    // with however the raster is actually displayed.
    return displayImageFor(reference) == state.view->image();
}

void MainWindow::rubberBandZoomActiveViewForTest()
{
    if (m_activeView == nullptr || m_activeView->plane.width <= 0
        || m_activeView->plane.height <= 0) {
        return;
    }
    const auto width = static_cast<double>(m_activeView->plane.width);
    const auto height = static_cast<double>(m_activeView->plane.height);
    rubberBandZoom(*m_activeView,
        QRectF(0.25 * width, 0.25 * height, 0.5 * width, 0.5 * height));
}

bool MainWindow::allViewsRubberBandZoomedForTest()
{
    const auto views = currentViews();
    return views.size() > 1
        && rubberBandZoomedViewCountForTest() == views.size();
}

std::size_t MainWindow::rubberBandZoomedViewCountForTest()
{
    const auto views = currentViews();
    return static_cast<std::size_t>(
        std::count_if(views.begin(), views.end(), [](const auto* state) {
            return state->visibleRegion.has_value();
        }));
}

void MainWindow::setActiveViewScaleForTest(int factor)
{
    if (m_activeView != nullptr) {
        m_activeView->view->setFixedScale(factor);
    }
}

void MainWindow::panActiveViewForTest(
    double sceneDeltaX, double sceneDeltaY)
{
    if (m_activeView == nullptr) {
        return;
    }
    const QPointF delta(sceneDeltaX, sceneDeltaY);
    beginPanDrag(*m_activeView);
    updatePanDrag(*m_activeView, delta, QPoint());
    endPanDrag(*m_activeView, delta);
}

qreal MainWindow::activeViewScaleForTest() const
{
    return m_activeView != nullptr ? m_activeView->view->transform().m11() : 0.0;
}

bool MainWindow::activeViewIsFitToWindowForTest()
{
    if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
        return false;
    }
    const auto before = m_activeView->view->transform();
    m_activeView->view->fitToWindow();
    return before == m_activeView->view->transform();
}

bool MainWindow::activeViewShowsWholeImageForTest() const
{
    if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
        return false;
    }
    auto* view = m_activeView->view;
    const auto visible = view->mapToScene(
        view->viewport()->rect()).boundingRect();
    const auto image = view->image();
    // Half-a-scene-pixel slack absorbs fitInView rounding at the borders.
    const QRectF imageRect(QPointF(0.0, 0.0), QSizeF(image.size()));
    return visible.adjusted(-0.5, -0.5, 0.5, 0.5).contains(imageRect);
}

void MainWindow::viewFabForTest(std::size_t index)
{
    viewFab(index);
}

bool MainWindow::activeViewIsZoomedForTest() const
{
    return m_activeView != nullptr && m_activeView->visibleRegion.has_value();
}

void MainWindow::setCacheBudgetForTest(std::uint64_t bytes)
{
    if (m_dataset) {
        // The return (whether resident already fits) is irrelevant here; the
        // next non-cache slice re-pins and triggers the fallback.
        static_cast<void>(m_dataset->setCacheBudget(bytes));
    }
}

std::uint64_t MainWindow::cacheResidentBytesForTest() const
{
    return m_dataset ? m_dataset->cacheMetrics().residentBytes : 0;
}

void MainWindow::showNumberFormatDialog()
{
    if (m_numberFormatDialog != nullptr) {
        m_numberFormatDialog->raise();
        m_numberFormatDialog->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Number Format"));
    dialog->setWindowFlags(Qt::Window);

    auto* edit = new QLineEdit(m_numberFormat, dialog);
    edit->setMinimumWidth(160);
    auto* syntaxLabel = new QLabel(
        tr("C printf format, e.g. %1").arg(defaultNumberFormat()), dialog);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok
        | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, dialog);
    auto* defaultButton = buttons->addButton(
        tr("Default"), QDialogButtonBox::ResetRole);
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(syntaxLabel);
    layout->addWidget(edit);
    layout->addWidget(buttons);

    connect(defaultButton, &QPushButton::clicked, dialog, [this, edit] {
        edit->setText(defaultNumberFormat());
        applyNumberFormat(defaultNumberFormat());
    });
    connect(buttons, &QDialogButtonBox::clicked, dialog,
        [this, dialog, edit, buttons](QAbstractButton* button) {
            const auto role = buttons->buttonRole(button);
            if (role == QDialogButtonBox::AcceptRole
                || role == QDialogButtonBox::ApplyRole) {
                const auto format = edit->text();
                if (!isValidNumberFormat(format)) {
                    QMessageBox::warning(dialog, tr("Invalid number format"),
                        tr("\"%1\" is not a usable number format.\n"
                           "Use a printf-style format with exactly one floating "
                           "conversion, e.g. %2.")
                            .arg(format, defaultNumberFormat()));
                    return;
                }
                applyNumberFormat(format);
                if (role == QDialogButtonBox::AcceptRole) {
                    dialog->accept();
                }
            } else if (role == QDialogButtonBox::RejectRole) {
                dialog->reject();
            }
        });
    connect(dialog, &QDialog::finished, this, [this] {
        m_numberFormatDialog = nullptr;
    });
    m_numberFormatDialog = dialog;
    dialog->show();
}

void MainWindow::applyNumberFormat(const QString& format)
{
    if (!isValidNumberFormat(format) || m_numberFormat == format) {
        return;
    }
    m_numberFormat = format;
    m_rangeMinimum->setNumberFormat(format);
    m_rangeMaximum->setNumberFormat(format);
    m_colorBar->setNumberFormat(format);
    // Open child windows repaint against the stored format; a null pointer
    // means the window picks the format up when it is next created.
    if (m_datasetWindow != nullptr) {
        m_datasetWindow->setNumberFormat(format);
    }
    if (m_linePlotWindow != nullptr) {
        m_linePlotWindow->setNumberFormat(format);
    }
    saveSettings();
}

void MainWindow::validateVectorMode()
{
    if (m_displayMode != DisplayMode::VelocityVectors) {
        return;
    }
    const auto fieldCount = m_openMetadata ? m_openMetadata->fields.size() : 0;
    if (fieldCount < 2) {
        statusBar()->showMessage(
            tr("Velocity Vectors requires at least two fields"));
        m_displayMode = DisplayMode::Raster;
        return;
    }
    ensureVectorFieldDefaults();
}

void MainWindow::ensureVectorFieldDefaults()
{
    if (!m_openMetadata) {
        return;
    }
    const auto& fields = m_openMetadata->fields;
    const auto count = static_cast<int>(fields.size());
    if (m_vectorUField >= 0 && m_vectorUField < count
        && m_vectorVField >= 0 && m_vectorVField < count
        && m_vectorWField >= 0 && m_vectorWField < count) {
        return;
    }
    std::vector<std::string> fieldNames;
    fieldNames.reserve(fields.size());
    for (const auto& field : fields) {
        fieldNames.push_back(field.name);
    }
    auto [uField, vField, wField] = detectVectorFields(fieldNames);
    if (uField == vField && count > 1) {
        vField = (uField == 0) ? 1 : 0;
    }
    m_vectorUField = uField;
    m_vectorVField = vField;
    m_vectorWField = wField;
}

QLineF MainWindow::planeSegmentToScene(const PlaneViewState& state,
    float x0, float y0, float x1, float y1) const
{
    // Plane row 0 is the bottom row; the displayed image is mirrored
    // vertically, so scene y runs opposite to plane y (see showSlice).
    const auto top = static_cast<double>(state.plane.height) - 1.0;
    return QLineF(QPointF(x0, top - y0), QPointF(x1, top - y1));
}

QColor MainWindow::overlayColor() const
{
    if (m_contourColor == contourColorWhite) {
        return QColor(255, 255, 255);
    }
    if (m_contourColor >= 0 && m_contourColor < Palette::slotCount) {
        return QColor::fromRgba(static_cast<QRgb>(
            m_palette.slotArgb(m_contourColor)));
    }
    return QColor(0, 0, 0);
}

QColor MainWindow::sliceAxisColor(int axis) const
{
    // Legacy Amrvis draws each slice plane's guides in a fixed palette slot:
    // x -> slot 65, y -> slot 220, z -> slot 255.
    constexpr std::array<int, 3> paletteSlots{65, 220, 255};
    return QColor::fromRgba(static_cast<QRgb>(
        m_palette.slotArgb(paletteSlots[static_cast<std::size_t>(axis)])));
}

void MainWindow::updateOverlay(PlaneViewState& state)
{
    std::vector<OverlaySegment> overlays;
    std::vector<OverlayPath> paths;
    const auto planeReady = state.plane.width > 1 && state.plane.height > 1;
    if (!planeReady || m_displayMode == DisplayMode::Raster) {
        state.view->setOverlaySegments(overlays);
        state.view->setOverlayPaths(paths);
        return;
    }

    if (m_displayMode == DisplayMode::VelocityVectors) {
        overlays.reserve(state.vectorSegments.size());
        const auto vectorColor = overlayColor();
        for (const auto& segment : state.vectorSegments) {
            overlays.push_back({planeSegmentToScene(state,
                segment.x0, segment.y0, segment.x1, segment.y1),
                vectorColor, 1.0F});
        }
        state.view->setOverlaySegments(overlays);
        state.view->setOverlayPaths(paths);
        return;
    }

    if (!(state.displayMinimum < state.displayMaximum)) {
        state.view->setOverlaySegments(overlays);
        state.view->setOverlayPaths(paths);
        return;
    }
    try {
        // The polylines were extracted from the refined data-resolution
        // contour plane on the slice worker and are already in display-plane
        // pixel space (see appendContours); this thread only converts them
        // to painter paths. Plane row 0 is the bottom row; the displayed
        // image is mirrored vertically, so scene y runs opposite to plane y
        // (see showSlice).
        const auto contourColor = overlayColor();
        const auto top = static_cast<double>(state.plane.height) - 1.0;
        std::map<double, QPainterPath> pathsByValue;
        for (const auto& polyline : state.contourPolylines) {
            if (polyline.points.empty()) {
                continue;
            }
            auto& path = pathsByValue[polyline.value];
            const auto& first = polyline.points.front();
            path.moveTo(QPointF(first[0], top - first[1]));
            for (std::size_t i = 1; i < polyline.points.size(); ++i) {
                const auto& point = polyline.points[i];
                path.lineTo(QPointF(point[0], top - point[1]));
            }
            if (polyline.closed) {
                path.closeSubpath();
            }
        }
        paths.reserve(pathsByValue.size());
        for (auto& [value, path] : pathsByValue) {
            const auto color = contourColor;
            paths.push_back({std::move(path), color, 1.0F});
        }
    } catch (const std::exception&) {
        paths.clear();
    }
    state.view->setOverlaySegments(overlays);
    state.view->setOverlayPaths(paths);
}

void MainWindow::updateOverlays()
{
    for (auto* state : currentViews()) {
        updateOverlay(*state);
    }
}

void MainWindow::showKeyboardMouseReference()
{
    QString rows;
    const auto add = [&rows](const QString& action, const QString& description) {
        rows += QStringLiteral(
            "<tr><td style='padding-right:14px;vertical-align:top;'><b>%1</b></td>"
            "<td>%2</td></tr>").arg(action, description);
    };
    add(tr("Left click"), tr("Probe the value under the cursor"));
    add(tr("Left drag"),
        tr("Zoom to the rubber-band subregion; Scale controls panel sync"));
    add(tr("Shift+left drag"), tr("Pan the view"));
    add(tr("Arrow keys"), tr("Pan the active panel (5% of the view per step)"));
    add(tr("Shift+middle click"), tr("Line plot along the horizontal axis"));
    add(tr("Shift+right click"), tr("Line plot along the vertical axis"));
    add(tr("Right drag"), tr("Line plot (drag direction picks orientation)"));
    add(tr("Right click (3-D)"),
        tr("Move both slice planes to intersect at the clicked point"));
    add(tr("Wheel / double click"),
        tr("Zoom this panel in or out / reset the zoom"));
    add(tr("B"), tr("Toggle AMR grid boxes"));
    add(tr("0"), tr("Reset the zoom to the whole domain"));
    add(tr("1-6"), tr("Fixed zoom scales (1x-32x)"));
    add(tr("Ctrl+0"), tr("Composite the finest available level"));
    add(tr("Ctrl+1-9"), tr("Composite levels 0 through N (Levs 0-N)"));
    add(tr("Alt+0-9"), tr("Show one exact AMR level"));
    add(tr("Ctrl+D"), tr("Open the Dataset window (raw cell values per level)"));

    QMessageBox box(this);
    box.setWindowTitle(tr("Keyboard & Mouse"));
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral("<table>%1</table>").arg(rows));
    box.setInformativeText(
        tr("View \xE2\x86\x92 Number Format... sets the readout format; "
           "the View menu shows or hides the panels."));
    box.setIcon(QMessageBox::NoIcon);
    box.exec();
}

void MainWindow::showUserGuide()
{
    if (m_userGuideDialog == nullptr) {
        m_userGuideDialog = new UserGuideDialog(this);
    }
    m_userGuideDialog->show();
    m_userGuideDialog->raise();
    m_userGuideDialog->activateWindow();
}

void MainWindow::showAboutDialog()
{
    QMessageBox::about(this, tr("About AMReXplorer"),
        tr("<h3>AMReXplorer</h3>"
           "<p>Demand-driven AMR visualization.</p>"
           "<p>Version %1</p>"
           "<p>A C++20 / Qt 6 application for inspecting AMReX plotfiles.</p>")
            .arg(QStringLiteral(AMREXPLORER_VERSION)));
}

void MainWindow::resetViewZoom(PlaneViewState& state)
{
    state.visibleRegion.reset();
    state.view->fitToWindow();
    m_resetZoomAction->setChecked(true);
    if (m_scaleButton != nullptr) {
        m_scaleButton->setText(tr("Fit"));
    }
    scheduleSliceRequest(state);
}

void MainWindow::resetZoomAllViews()
{
    for (auto* state : currentViews()) {
        resetViewZoom(*state);
    }
}

QString MainWindow::probeReadout(
    const PlaneViewState& state, int x, int displayY) const
{
    const auto& plane = state.plane;
    if (!m_dataset || plane.width <= 0 || plane.height <= 0) {
        return tr("no data");
    }
    const auto y = plane.height - 1 - displayY;
    const auto offset = static_cast<std::size_t>(x)
        + static_cast<std::size_t>(plane.width) * static_cast<std::size_t>(y);
    if (offset >= plane.values.size() || plane.valid[offset] == 0) {
        return tr("no data");
    }
    const auto& metadata = m_dataset->metadata();
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    std::array<double, 3> position{0.0, 0.0, 0.0};
    position[xAxis] = plane.physicalRegion.lower[xAxis]
        + (static_cast<double>(x) + 0.5)
            / static_cast<double>(plane.width)
            * (plane.physicalRegion.upper[xAxis]
                - plane.physicalRegion.lower[xAxis]);
    position[yAxis] = plane.physicalRegion.lower[yAxis]
        + (static_cast<double>(y) + 0.5)
            / static_cast<double>(plane.height)
            * (plane.physicalRegion.upper[yAxis]
                - plane.physicalRegion.lower[yAxis]);
    if (metadata.dimension == 3) {
        position[static_cast<std::size_t>(state.normal)]
            = m_slicePosition3d[static_cast<std::size_t>(state.normal)];
    }
    const auto level = std::clamp(
        static_cast<int>(plane.sourceLevel[offset]), 0, metadata.finestLevel);
    const auto& levelMetadata = metadata.levels[static_cast<std::size_t>(level)];

    // Integer index of the cell/face/edge/node. Nodes sit on integer
    // positions so they round; everything else floors into its cell.
    const auto centering = (state.hasCachedRequest
            && state.cachedRequest.field.value < metadata.fields.size())
        ? metadata.fields[state.cachedRequest.field.value].centering
        : amrvis::Centering::Cell;
    std::array<int, 3> cell{0, 0, 0};
    for (int axis = 0; axis < metadata.dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        cell[i] = sampleIndex(levelMetadata, axis, position[i]);
    }

    // The AMR box (grid) at this level that contains the cell.
    int boxIndex = -1;
    for (int box = 0; box < static_cast<int>(levelMetadata.boxes.size()); ++box) {
        const auto& candidate = levelMetadata.boxes[static_cast<std::size_t>(box)];
        bool contains = true;
        for (int axis = 0; axis < metadata.dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            if (cell[i] < candidate.lower[i] || cell[i] > candidate.upper[i]) {
                contains = false;
                break;
            }
        }
        if (contains) {
            boxIndex = box;
            break;
        }
    }

    auto join = [&](const auto& triple) {
        QString text;
        for (int axis = 0; axis < metadata.dimension; ++axis) {
            if (axis != 0) {
                text += ',';
            }
            text += QString::number(triple[static_cast<std::size_t>(axis)]);
        }
        return text;
    };

    constexpr std::array<const char*, 3> axisNames{"x", "y", "z"};
    const char* indexKind = "cell";
    if (centering == amrvis::Centering::Node) {
        indexKind = "node";
    } else if (centering == amrvis::Centering::FaceX
        || centering == amrvis::Centering::FaceY
        || centering == amrvis::Centering::FaceZ) {
        indexKind = "face";
    } else if (centering == amrvis::Centering::EdgeX
        || centering == amrvis::Centering::EdgeY
        || centering == amrvis::Centering::EdgeZ) {
        indexKind = "edge";
    }

    QString boxText;
    if (boxIndex >= 0) {
        const auto& box = levelMetadata.boxes[static_cast<std::size_t>(boxIndex)];
        // Axis-major: ((xlo,xhi),(ylo,yhi),...,(index-type per axis)). The
        // trailing list is the box's AMReX IndexType (0 = cell, 1 = node).
        QString bounds;
        for (int axis = 0; axis < metadata.dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            if (axis != 0) {
                bounds += ',';
            }
            bounds += QStringLiteral("(%1,%2)").arg(box.lower[i]).arg(box.upper[i]);
        }
        QString indexType;
        for (int axis = 0; axis < metadata.dimension; ++axis) {
            const auto i = static_cast<std::size_t>(axis);
            if (axis != 0) {
                indexType += ',';
            }
            indexType += QString::number(box.centering[i]);
        }
        boxText = tr("box #%1 (%2,(%3))").arg(boxIndex).arg(bounds, indexType);
    } else {
        boxText = tr("box=none");
    }

    return tr("%1=%2 %3=%4 value=%5 level=%6 %7=(%8) %9")
        .arg(QString::fromLatin1(axisNames[xAxis]))
        .arg(formatNumber(position[xAxis], m_numberFormat))
        .arg(QString::fromLatin1(axisNames[yAxis]))
        .arg(formatNumber(position[yAxis], m_numberFormat))
        .arg(formatNumber(static_cast<double>(plane.values[offset]),
            m_numberFormat))
        .arg(level)
        .arg(QString::fromLatin1(indexKind))
        .arg(join(cell))
        .arg(boxText);
}

void MainWindow::probeMoved(PlaneViewState& state, int x, int displayY)
{
    m_probeLabel->setText(probeReadout(state, x, displayY));
}

void MainWindow::probeClicked(PlaneViewState& state, int x, int displayY)
{
    setActiveView(state);
    const auto line = probeReadout(state, x, displayY);
    m_probeLabel->setText(line);
    constexpr int maximumProbeLines = 100;
    m_probeLines.append(line);
    while (m_probeLines.size() > maximumProbeLines) {
        m_probeLines.removeFirst();
    }
    updateDiagnostics();
}

void MainWindow::rubberBandZoom(PlaneViewState& state, const QRectF& sceneRect)
{
    setActiveView(state);
    const auto& plane = state.plane;
    if (!m_dataset || plane.width <= 0 || plane.height <= 0) {
        return;
    }
    const auto clamped = sceneRect.normalized().intersected(
        QRectF(0.0, 0.0, static_cast<double>(plane.width),
            static_cast<double>(plane.height)));
    if (clamped.width() < 1.0 || clamped.height() < 1.0) {
        return;
    }
    const QRectF normalizedRect(
        clamped.left() / static_cast<double>(plane.width),
        clamped.top() / static_cast<double>(plane.height),
        clamped.width() / static_cast<double>(plane.width),
        clamped.height() / static_cast<double>(plane.height));
    const auto views = currentViews();
    const bool synchronize = m_syncRubberBandZoomAction != nullptr
        && m_syncRubberBandZoomAction->isChecked()
        && views.size() > 1;
    if (synchronize) {
        for (auto* target : views) {
            applyRubberBandZoom(*target, normalizedRect);
        }
    } else {
        applyRubberBandZoom(state, normalizedRect);
    }
    if (m_scaleGroup != nullptr) {
        if (auto* checked = m_scaleGroup->checkedAction()) {
            checked->setChecked(false);
        }
    }
    if (m_scaleButton != nullptr) {
        m_scaleButton->setText(
            views.size() > 1 && !synchronize ? tr("Mixed") : tr("Custom"));
    }
}

void MainWindow::applyRubberBandZoom(
    PlaneViewState& state, const QRectF& normalizedRect)
{
    const auto& plane = state.plane;
    if (!m_dataset || plane.width <= 0 || plane.height <= 0) {
        return;
    }
    const auto normalized = normalizedRect.normalized().intersected(
        QRectF(0.0, 0.0, 1.0, 1.0));
    if (normalized.isEmpty()) {
        return;
    }
    const auto width = static_cast<double>(plane.width);
    const auto height = static_cast<double>(plane.height);
    const QRectF clamped(
        normalized.left() * width, normalized.top() * height,
        normalized.width() * width, normalized.height() * height);
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto& region = plane.physicalRegion;
    const auto xExtent = region.upper[xAxis] - region.lower[xAxis];
    const auto yExtent = region.upper[yAxis] - region.lower[yAxis];
    auto visible = region;
    visible.lower[xAxis] = region.lower[xAxis] + clamped.left() / width * xExtent;
    visible.upper[xAxis] = region.lower[xAxis] + clamped.right() / width * xExtent;
    visible.lower[yAxis] = region.lower[yAxis]
        + (height - clamped.bottom()) / height * yExtent;
    visible.upper[yAxis] = region.lower[yAxis]
        + (height - clamped.top()) / height * yExtent;
    // The edges above land mid-cell. Snap them outward to finest-level cell
    // boundaries so the slice output (one pixel per finest cell, see
    // finestNativeOutputSize) samples exactly at cell centers; fractional
    // edges make the sampling pitch differ from the cell size and produce
    // duplicated or skipped rows/columns of cells.
    const auto& metadata = m_dataset->metadata();
    const auto& finest = metadata.levels[static_cast<std::size_t>(
        std::max(0, metadata.finestLevel))];
    visible = snapToCellBoundaries(
        visible, datasetSampleBounds(metadata), finest.cellSize, axes);
    state.visibleRegion = visible;
    // Zoom to the snapped region mapped back to scene pixels, so the view
    // transform matches the region the requested slice will actually cover.
    const QRectF snappedScene(
        QPointF((visible.lower[xAxis] - region.lower[xAxis]) / xExtent * width,
            (region.upper[yAxis] - visible.upper[yAxis]) / yExtent * height),
        QPointF((visible.upper[xAxis] - region.lower[xAxis]) / xExtent * width,
            (region.upper[yAxis] - visible.lower[yAxis]) / yExtent * height));
    state.view->zoomToRect(snappedScene.normalized());
    scheduleSliceRequest(state);
}

void MainWindow::beginPanDrag(PlaneViewState& state)
{
    setActiveView(state);
    m_panView = &state;
    m_panSceneDelta = QPointF();
    m_panLastScheduledDelta = QPointF();
    m_panDataRefresh = state.visibleRegion.has_value();
    if (m_panDataRefresh) {
        m_panStartRegion = *state.visibleRegion;
        m_panPlaneWidth = state.plane.width;
        m_panPlaneHeight = state.plane.height;
    }
}

void MainWindow::updatePanDrag(PlaneViewState& state,
    const QPointF& totalSceneDelta, const QPoint& viewportDelta)
{
    if (m_panView != &state) {
        return;
    }
    m_panSceneDelta = totalSceneDelta;
    constexpr int minimumDrag = 4;
    if (std::max(std::abs(totalSceneDelta.x()),
            std::abs(totalSceneDelta.y())) < minimumDrag) {
        return;
    }
    if (m_panDataRefresh) {
        if (!m_panDebounce->isActive()) {
            flushPanDrag(false);
            m_panDebounce->start();
        }
    } else {
        state.view->panViewport(viewportDelta);
    }
}

void MainWindow::endPanDrag(PlaneViewState& state, const QPointF& totalSceneDelta)
{
    m_panDebounce->stop();
    if (m_panView != &state) {
        return;
    }
    m_panSceneDelta = totalSceneDelta;
    constexpr int minimumDrag = 4;
    if (std::max(std::abs(totalSceneDelta.x()),
            std::abs(totalSceneDelta.y())) >= minimumDrag
        && m_panDataRefresh) {
        flushPanDrag(true);
    }
    m_panView = nullptr;
    m_panDataRefresh = false;
}

void MainWindow::flushPanDrag(bool finalize)
{
    if (!m_panView || !m_panDataRefresh || !m_dataset) {
        return;
    }
    if (!finalize && m_panSceneDelta == m_panLastScheduledDelta) {
        return;
    }
    const auto region = shiftedPanRegion(*m_panView, m_panStartRegion,
        m_panPlaneWidth, m_panPlaneHeight, m_panSceneDelta);
    if (!region.has_value()) {
        return;
    }
    m_panView->visibleRegion = *region;
    m_panLastScheduledDelta = m_panSceneDelta;
    scheduleSliceRequest(*m_panView, false);
}

void MainWindow::setupPanShortcuts()
{
    const auto bind = [this](Qt::Key key, double x, double y) {
        auto* shortcut = new QShortcut(QKeySequence(key), this);
        shortcut->setContext(Qt::WindowShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, x, y] {
            if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
                return;
            }
            applyPanStep(*m_activeView, QPointF(x, y));
        });
    };
    bind(Qt::Key_Left, 1.0, 0.0);
    bind(Qt::Key_Right, -1.0, 0.0);
    bind(Qt::Key_Up, 0.0, 1.0);
    bind(Qt::Key_Down, 0.0, -1.0);
}

void MainWindow::applyPanStep(PlaneViewState& state, const QPointF& direction)
{
    if (!state.view->hasImage() || state.plane.width <= 0 || state.plane.height <= 0) {
        return;
    }
    setActiveView(state);
    const auto stepX = std::max(1.0, static_cast<double>(state.plane.width) * 0.05);
    const auto stepY = std::max(1.0, static_cast<double>(state.plane.height) * 0.05);
    const QPointF sceneDelta(direction.x() * stepX, direction.y() * stepY);

    if (state.visibleRegion.has_value() && m_dataset) {
        const auto region = shiftedPanRegion(state, *state.visibleRegion,
            state.plane.width, state.plane.height, sceneDelta);
        if (!region.has_value()) {
            return;
        }
        state.visibleRegion = *region;
        state.view->fitToWindow();
        m_resetZoomAction->setChecked(true);
        if (m_scaleButton != nullptr) {
            m_scaleButton->setText(tr("Fit"));
        }
        scheduleSliceRequest(state, false);
        return;
    }

    const auto scale = state.view->transform().m11();
    state.view->panViewport(QPoint(
        static_cast<int>(std::round(-sceneDelta.x() * scale)),
        static_cast<int>(std::round(-sceneDelta.y() * scale))));
}

std::optional<RealBox> MainWindow::shiftedPanRegion(
    const PlaneViewState& state, const RealBox& baseRegion,
    int planeWidth, int planeHeight, const QPointF& sceneDelta) const
{
    if (!m_dataset || planeWidth <= 0 || planeHeight <= 0) {
        return std::nullopt;
    }
    auto visible = baseRegion;
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto domain = datasetSampleBounds(m_dataset->metadata());
    const auto width = static_cast<double>(planeWidth);
    const auto height = static_cast<double>(planeHeight);
    const auto xExtent = visible.upper[xAxis] - visible.lower[xAxis];
    const auto yExtent = visible.upper[yAxis] - visible.lower[yAxis];
    auto deltaX = -sceneDelta.x() / width * xExtent;
    auto deltaY = sceneDelta.y() / height * yExtent;

    if (visible.lower[xAxis] + deltaX < domain.lower[xAxis]) {
        deltaX = domain.lower[xAxis] - visible.lower[xAxis];
    }
    if (visible.upper[xAxis] + deltaX > domain.upper[xAxis]) {
        deltaX = domain.upper[xAxis] - visible.upper[xAxis];
    }
    if (visible.lower[yAxis] + deltaY < domain.lower[yAxis]) {
        deltaY = domain.lower[yAxis] - visible.lower[yAxis];
    }
    if (visible.upper[yAxis] + deltaY > domain.upper[yAxis]) {
        deltaY = domain.upper[yAxis] - visible.upper[yAxis];
    }
    if (deltaX == 0.0 && deltaY == 0.0) {
        return std::nullopt;
    }

    visible.lower[xAxis] += deltaX;
    visible.upper[xAxis] += deltaX;
    visible.lower[yAxis] += deltaY;
    visible.upper[yAxis] += deltaY;
    // Snap the translated region back onto the finest-level cell grid,
    // preserving its span. Fractional edges let the slice sampler's pixel
    // centers land on cell boundaries whenever the phase approaches half a
    // cell (arrow-key steps of 0.05*N cells hit exactly x.5 within a few
    // presses), and the floor in physicalToIndex then rounds either way —
    // the duplicated/skipped rows and columns this prevents.
    const auto& metadata = m_dataset->metadata();
    const auto& finest = metadata.levels[static_cast<std::size_t>(
        std::max(0, metadata.finestLevel))];
    const auto snapped = snapToNearestCellGrid(
        visible, domain, finest.cellSize, axes);
    if (snapped == baseRegion) {
        return std::nullopt;
    }
    return snapped;
}

void MainWindow::linePlotRequested(PlaneViewState& state, int imageX, int imageY,
    Qt::MouseButton button)
{
    setActiveView(state);
    const auto& plane = state.plane;
    if (!m_controlsReady || !m_dataset || plane.width <= 0 || plane.height <= 0) {
        return;
    }
    const auto dataset = m_dataset;
    const auto& metadata = dataset->metadata();
    const auto horizontal = button == Qt::MiddleButton;
    const auto level = m_levelSelector->currentData().toInt();
    const auto [composition, maximumLevel] = decodeLevelData(
        level, metadata.finestLevel);
    const auto field = m_fieldSelector->currentData().toUInt();
    const auto slicePosition = metadata.dimension == 3
        ? m_slicePosition3d[static_cast<std::size_t>(state.normal)] : 0.0;
    const auto request = makeLineRequest(plane.physicalRegion,
        plane.width, plane.height, imageX, imageY, horizontal,
        metadata.dimension, state.normal, slicePosition,
        dataset->id(), FieldId{field}, maximumLevel, composition);
    const auto fieldName = metadata.fields[field].name;
    const auto dimension = metadata.dimension;
    // The other in-plane axis carries the cursor's fixed coordinate.
    const auto axes = displayAxes(state.normal);
    const auto primaryFixedAxis = request.axis == axes[0] ? axes[1] : axes[0];
    const auto generation = m_generation;
    // Renew the line-plot stop source only if a dataset switch or window close
    // stopped it, so concurrent line requests can still complete and stack
    // their curves in the shared window.
    if (m_linePlotStopSource.stop_requested()) {
        m_linePlotStopSource = StopSource{};
    }
    const auto cancellation = m_linePlotStopSource.get_token();
    ++m_activeRequests;
    statusBar()->showMessage(tr("Loading line plot for %1...").arg(
        QString::fromStdString(fieldName)));
    updateDiagnostics();

    auto* watcher = new QFutureWatcher<LineQueryResult>(this);
    auto* view = state.view;
    connect(watcher, &QFutureWatcher<LineQueryResult>::finished, this,
        [this, watcher, dataset, generation, cancellation, request, fieldName,
            dimension, primaryFixedAxis, maximumLevel, composition, view] {
            --m_activeRequests;
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            try {
                auto result = watcher->result();
                if (generation != m_generation || cancellation.stop_requested()) {
                    ++m_staleResults;
                } else {
                    view->clearLineGuide();
                    appendLinePlotCurve(result.line, fieldName, dimension,
                        primaryFixedAxis, request.axis,
                        request.fixedCoordinates,
                        maximumLevel, composition);
                    const auto cache = dataset->cacheMetrics();
                    m_cacheBudgetBytes = cache.budgetBytes;
                    m_cacheResidentBytes = cache.residentBytes;
                    m_cachePinnedBytes = cache.pinnedBytes;
                    m_cacheEvictions = cache.evictions;
                    m_lastBlocksRead = result.metrics.blocksRead;
                    m_lastCacheHits = result.metrics.cacheHits;
                    m_lastPayloadBytesRead = result.metrics.payloadBytesRead;
                    statusBar()->showMessage(tr("Added line plot curve for %1")
                        .arg(QString::fromStdString(fieldName)));
                }
            } catch (const CacheBudgetExceeded&) {
                // A line plot cannot shed resolution the way a slice can, so
                // translate the raw pinned-budget error into actionable advice
                // instead of degrading (see
                // cache-budget-exceeded-hard-fails-after-load).
                if (generation == m_generation && !cancellation.stop_requested()) {
                    reportBackgroundError(tr(
                        "The line plot cannot fit in the %1 cache. Choose a "
                        "lower level or increase AMREXPLORER_CACHE_SIZE_MB.")
                        .arg(cacheBudgetDescription(
                            dataset->cacheMetrics().budgetBytes)));
                } else {
                    ++m_staleResults;
                }
            } catch (const std::exception& error) {
                if (generation == m_generation && !cancellation.stop_requested()) {
                    reportBackgroundError(
                        tr("Cannot load line plot: %1").arg(exceptionMessage(error)));
                } else {
                    ++m_staleResults;
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [dataset, request, cancellation] {
            return LineQuery(*dataset).execute(request, cancellation);
        }));
}

void MainWindow::sliceMoveRequested(PlaneViewState& state, int imageX, int imageY,
    Qt::MouseButton /*button*/)
{
    setActiveView(state);
    if (!m_dataset || m_dataset->metadata().dimension != 3
        || state.plane.width <= 0 || state.plane.height <= 0) {
        return;
    }
    // Move both in-plane axes so the three slices intersect at the clicked
    // point. A single right-click replaces the old middle=x / right=y split,
    // which was inaccessible on Mac (no middle button).
    const auto axes = displayAxes(state.normal);
    const auto& region = state.plane.physicalRegion;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto axis = axes[i];
        const auto fraction = (i == 0)
            ? (static_cast<double>(imageX) + 0.5)
                / static_cast<double>(state.plane.width)
            : (static_cast<double>(state.plane.height - 1 - imageY) + 0.5)
                / static_cast<double>(state.plane.height);
        const auto index = static_cast<std::size_t>(axis);
        setSlicePosition(axis, region.lower[index]
            + fraction * (region.upper[index] - region.lower[index]));
    }
}

void MainWindow::appendLinePlotCurve(const LineResult& line,
    const std::string& fieldName, int dimension, int primaryFixedAxis,
    int lineAxis, const std::array<double, 3>& fixedCoordinates,
    int maximumLevel, CompositionPolicy composition)
{
    if (m_linePlotWindow == nullptr) {
        auto name = QString::fromStdString(m_datasetPath.filename().string());
        if (name.isEmpty()) {
            name = QString::fromStdString(m_datasetPath.string());
        }
        auto* window = new LinePlotWindow(name);
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->setNumberFormat(m_numberFormat);
        connect(window, &QObject::destroyed, this, [this, window] {
            if (m_linePlotWindow == window) {
                m_linePlotWindow = nullptr;
            }
            // Stop in-flight line queries so a late result cannot reopen the
            // window the user just closed.
            m_linePlotStopSource.request_stop();
        });
        m_linePlotWindow = window;
    }
    LinePlotCurve curve;
    curve.line = line;
    curve.fieldName = fieldName;
    curve.primaryFixedAxis = primaryFixedAxis;
    curve.lineAxis = lineAxis;
    curve.fixedCoordinates = fixedCoordinates;
    curve.dimension = dimension;
    curve.maximumLevel = maximumLevel;
    curve.composition = composition;
    m_linePlotWindow->addCurve(std::move(curve));
    m_linePlotWindow->show();
    m_linePlotWindow->raise();
    m_linePlotWindow->activateWindow();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Mark this window closing so asynchronous completion handlers that fire
    // during or after shutdown do not pop modal dialogs or reopen windows.
    m_closing = true;
    // Stop resubmit timers, request cancellation on every async task, and
    // clear queued global-pool jobs. Running tasks re-check their stop token
    // and bail promptly; without this a task mid-read can leave the process
    // lingering at quit. (aboutToQuit also calls this, as a fallback.)
    cancelInFlight();
    // Secondary top-level windows are parentless or non-modal; close them with
    // the main window so none lingers and keeps the process alive.
    if (m_linePlotWindow != nullptr) {
        auto* linePlotWindow = m_linePlotWindow;
        m_linePlotWindow = nullptr;
        linePlotWindow->close();
    }
    closeDatasetWindow();
    if (m_contoursDialog != nullptr) {
        auto* dialog = m_contoursDialog;
        m_contoursDialog = nullptr;
        dialog->close();
    }
    if (m_numberFormatDialog != nullptr) {
        auto* dialog = m_numberFormatDialog;
        m_numberFormatDialog = nullptr;
        dialog->close();
    }
    if (m_userGuideDialog != nullptr) {
        auto* dialog = m_userGuideDialog;
        m_userGuideDialog = nullptr;
        dialog->close();
    }
    // Dismiss any export progress dialog and signal the encoder workers to
    // terminate their FFmpeg processes (see AnimationExporter).
    m_animationExporter->cancelForShutdown();
    saveSettings();
    auto settings = makeSettings();
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    QMainWindow::closeEvent(event);
}

void MainWindow::cancelInFlight()
{
    // Application shutdown: stop the timers that resubmit work, request stop on
    // every async task this window can launch, and clear the queued jobs from
    // the global thread pool. The slice/prefetch/line-query/initial-load tasks
    // run on QThreadPool::globalInstance() via QtConcurrent::run, and that
    // pool's destructor calls waitForDone() with no timeout. A task caught
    // mid-read holds the global AMReX I/O mutex and only re-checks its
    // cancellation token at the next chunk boundary (PlotfileBlockReader
    // checks every 1 MiB / 4096 values), so unless it is told to stop the process lingers
    // "not responding" at quit -- which is what made closing the window look
    // like a hang. request_stop is the cooperative signal those tasks poll, so
    // once set a running task bails promptly and teardown unblocks.
    if (m_sliceDebounce != nullptr) {
        m_sliceDebounce->stop();
    }
    if (m_playbackTimer != nullptr) {
        m_playbackTimer->stop();
    }
    m_initialStopSource.request_stop();
    m_metadataStopSource.request_stop();
    m_prefetchStopSource.request_stop();
    m_linePlotStopSource.request_stop();
    m_view2d.stopSource.request_stop();
    for (auto& state : m_planeViews) {
        state.stopSource.request_stop();
    }
    if (auto* pool = QThreadPool::globalInstance()) {
        pool->clear();
    }
}

void MainWindow::restoreSettings()
{
    const auto settings = makeSettings();

    auto paletteRestored = false;
    if (settings.value(QStringLiteral("palette/fromFile"), false).toBool()) {
        const auto path = settings.value(QStringLiteral("palette/filePath")).toString();
        if (!path.isEmpty()) {
            try {
                m_palette = Palette::load(path.toStdString());
                m_paletteFromFile = true;
                m_paletteFilePath = path;
                paletteRestored = true;
            } catch (const std::exception&) {
                paletteRestored = false;
            }
        }
    }
    if (!paletteRestored) {
        const auto name = settings.value(QStringLiteral("palette/builtin"),
            QStringLiteral("rainbow")).toString();
        m_builtinIndex = 0;
        for (std::size_t index = 0; index < builtinPaletteNames.size(); ++index) {
            if (name == QLatin1String(builtinPaletteNames[index])) {
                m_builtinIndex = static_cast<int>(index);
                break;
            }
        }
        m_palette = builtinPalette(
            builtinPalettes[static_cast<std::size_t>(m_builtinIndex)]);
        m_paletteFromFile = false;
        m_paletteFilePath.clear();
    }
    m_colorBar->setPalette(&m_palette);
    syncPaletteChecks();
    syncPaletteSelector();

    {
        const QSignalBlocker logarithmicBlocker(m_logarithmic);
        m_logarithmic->setChecked(
            settings.value(QStringLiteral("range/logarithmic"), false).toBool());
    }
    {
        // A stored format that no longer validates falls back to the default.
        const auto format = settings.value(QStringLiteral("numberFormat"),
            defaultNumberFormat()).toString();
        m_numberFormat = isValidNumberFormat(format) ? format
            : defaultNumberFormat();
        m_rangeMinimum->setNumberFormat(m_numberFormat);
        m_rangeMaximum->setNumberFormat(m_numberFormat);
        m_colorBar->setNumberFormat(m_numberFormat);
    }
    m_animationPanel->setSpeedValue(
        settings.value(QStringLiteral("animation/speed"), 300).toInt());
    {
        const QSignalBlocker syncZoomBlocker(m_syncRubberBandZoomAction);
        m_syncRubberBandZoomAction->setChecked(
            settings.value(QStringLiteral("zoom/syncRubberBand"), true).toBool());
    }
    applySpeed();
    const auto geometry = settings.value(QStringLiteral("geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
}

void MainWindow::saveSettings()
{
    auto settings = makeSettings();
    // Range mode is deliberately not persisted: the correct default (File)
    // depends on the dataset and restoring a different mode from a previous
    // session would produce unexpected color bars.
    settings.setValue(QStringLiteral("range/logarithmic"), m_logarithmic->isChecked());
    settings.setValue(QStringLiteral("palette/fromFile"), m_paletteFromFile);
    settings.setValue(QStringLiteral("palette/filePath"), m_paletteFilePath);
    settings.setValue(QStringLiteral("palette/builtin"),
        QLatin1String(builtinPaletteNames[static_cast<std::size_t>(m_builtinIndex)]));
    settings.setValue(QStringLiteral("numberFormat"), m_numberFormat);
    settings.setValue(QStringLiteral("animation/speed"),
        m_animationPanel->speedValue());
    settings.setValue(QStringLiteral("zoom/syncRubberBand"),
        m_syncRubberBandZoomAction->isChecked());
}

void MainWindow::updateWindowTitle()
{
    if (!m_openMetadata) {
        setWindowTitle(tr("AMReXplorer"));
        return;
    }
    const auto& metadata = *m_openMetadata;
    auto name = QString::fromStdString(m_datasetPath.filename().string());
    if (name.isEmpty()) {
        name = QString::fromStdString(m_datasetPath.string());
    }
    if (m_fabMode) {
        setWindowTitle(tr("AMReXplorer — %1 — FAB  T = %2")
            .arg(name)
            .arg(metadata.time, 0, 'g', 12));
    } else {
        setWindowTitle(
            tr("AMReXplorer — %1  T = %2  Levels: 0..%3  Finest Level: %3")
                .arg(name)
                .arg(metadata.time, 0, 'g', 12)
                .arg(metadata.finestLevel));
    }
}

MainWindow* MainWindow::createNewWindow()
{
    auto* window = new MainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
    return window;
}

void MainWindow::chooseDataset()
{
    const auto settings = makeSettings();
    const auto directory = QFileDialog::getExistingDirectory(
        this, tr("Open AMReX plotfile"),
        settings.value(QStringLiteral("lastOpenDirectory")).toString());
    if (directory.isEmpty()) {
        return;
    }
    openDataset(directory.toStdString());
}

void MainWindow::chooseStandaloneDataset(const QString& caption, bool rawFab)
{
    const auto settings = makeSettings();
    const auto filename = QFileDialog::getOpenFileName(this,
        caption,
        settings.value(QStringLiteral("lastOpenDirectory")).toString(),
        tr("AMReX data (*)"));
    if (!filename.isEmpty()) {
        if (rawFab) {
            try {
                const auto path = std::filesystem::path(filename.toStdString());
                auto metadata = StandaloneMetadataReader{}.readFab(path);
                auto root = path.parent_path();
                if (root.empty()) {
                    root = ".";
                }
                openDatasetImpl(path, false, std::move(metadata),
                    std::move(root), false, std::nullopt);
            } catch (const std::exception& error) {
                QMessageBox::critical(this, tr("Cannot open FAB"),
                    exceptionMessage(error));
            }
        } else {
            openDataset(filename.toStdString());
        }
    }
}

void MainWindow::configureFabSelector(
    const PlotfileMetadataResult& result, const std::filesystem::path& path)
{
    std::vector<FabSelectorEntry> entries;
    auto root = path.parent_path();
    if (root.empty()) {
        root = ".";
    }

    if (result.fileVersion == "FAB") {
        const auto records = scanFabFile(path);
        entries.reserve(records.size());
        for (const auto& record : records) {
            entries.push_back({
                .ordinal = record.ordinal,
                .level = 0,
                .blockIndex = record.ordinal,
                .filePath = path,
                .fileOffset = record.headerOffset,
                .validBox = record.storedBox,
                .storedBox = record.storedBox,
                .dimension = record.dimension,
                .components = record.components,
                .precision = record.precision == FabRealPrecision::Single
                    ? tr("IEEE-32") : tr("IEEE-64"),
                .rawRecord = true
            });
        }
        m_fabMode = true;
        m_fabSourceMetadata.reset();
    } else if (result.fileVersion.starts_with("VisMF-")
        && result.metadata->levels.size() == 1) {
        const auto& metadata = *result.metadata;
        const auto& level = metadata.levels.front();
        entries.reserve(level.blocks.size());
        for (std::size_t index = 0; index < level.blocks.size(); ++index) {
            const auto& block = level.blocks[index];
            auto storedBox = block.box;
            for (int axis = 0; axis < metadata.dimension; ++axis) {
                const auto coordinate = static_cast<std::size_t>(axis);
                storedBox.lower[coordinate] -= level.ghostWidth[coordinate];
                storedBox.upper[coordinate] += level.ghostWidth[coordinate];
            }
            auto precision = FabRealPrecision::Double;
            if (level.visMfHeaderVersion == 1) {
                const auto record = inspectFabRecord(
                    root / block.filePath, block.fileOffset);
                storedBox = record.storedBox;
                precision = record.precision;
            } else {
                precision = fabPrecisionFromDescriptor(level.realDescriptor);
            }
            entries.push_back({
                .ordinal = index,
                .level = level.level,
                .blockIndex = index,
                .filePath = root / block.filePath,
                .fileOffset = block.fileOffset,
                .validBox = block.box,
                .storedBox = storedBox,
                .dimension = metadata.dimension,
                .components = level.storedComponents,
                .precision = precision == FabRealPrecision::Single
                    ? tr("IEEE-32") : tr("IEEE-64"),
                .rawRecord = false
            });
        }
        m_fabMode = false;
        m_fabSourceMetadata = result;
    }

    if (entries.empty()) {
        m_fabSelectorDock->setVisible(false);
        return;
    }
    m_fabSourcePath = path;
    m_fabDataRoot = root;
    m_fabSelectorDock->setEntries(std::move(entries));
    m_fabSelectorDock->setBackAvailable(false);
    m_fabSelectorDock->setVisible(true);
    m_fabSelectorDock->raise();
    updateWindowTitle();
}

void MainWindow::viewFab(std::size_t entryIndex)
{
    const auto& entries = m_fabSelectorDock->entries();
    if (entryIndex >= entries.size()) {
        return;
    }
    const auto entry = entries[entryIndex];
    try {
        auto selectedSpec = m_dataset
            ? std::optional<FrameSliceSpec>{buildFrameSpec()}
            : std::nullopt;
        if (selectedSpec) {
            selectedSpec->levelSelection = -1;
            selectedSpec->rangeMode = RangeMode::File;
            selectedSpec->userRange.reset();
        }
        PlotfileMetadataResult selected;
        if (entry.rawRecord) {
            selected = StandaloneMetadataReader{}.readFab(
                entry.filePath, entry.fileOffset);
        } else {
            if (!m_fabSourceMetadata) {
                throw std::runtime_error(
                    "the source MultiFab is no longer available");
            }
            if (!m_multifabReturn) {
                m_multifabReturn = MultiFabReturnState{
                    m_fabSourcePath, m_fabDataRoot,
                    *m_fabSourceMetadata, buildFrameSpec()};
            }
            selected = makeSelectedFabMetadata(*m_fabSourceMetadata->metadata,
                entry.level, entry.blockIndex, m_fabDataRoot);
        }
        m_fabMode = true;
        m_fabSelectorDock->setBackAvailable(m_multifabReturn.has_value());
        m_fabSelectorDock->selectEntry(entry.ordinal);
        openDatasetImpl(m_fabSourcePath, false, std::move(selected),
            m_fabDataRoot, true, std::move(selectedSpec));
    } catch (const std::exception& error) {
        QMessageBox::critical(this, tr("Cannot view FAB"),
            exceptionMessage(error));
    }
}

void MainWindow::backToMultiFab()
{
    if (!m_multifabReturn) {
        return;
    }
    auto state = std::move(*m_multifabReturn);
    m_multifabReturn.reset();
    m_fabMode = false;
    m_fabSelectorDock->setBackAvailable(false);
    openDatasetImpl(state.path, false, std::move(state.metadata),
        std::move(state.dataRoot), true, std::move(state.spec));
}

void MainWindow::exportImage()
{
    auto* view = m_activeView != nullptr ? m_activeView->view : nullptr;
    if (view == nullptr || !view->hasImage()) {
        QMessageBox::information(this, tr("No image"),
            tr("Open a dataset before exporting an image."));
        return;
    }

    QMessageBox choice(this);
    choice.setIcon(QMessageBox::Question);
    choice.setWindowTitle(tr("Export Image"));
    choice.setText(tr("Include the color bar in the exported image?"));
    auto* withBar = choice.addButton(tr("&With color bar"),
        QMessageBox::AcceptRole);
    auto* withoutBar = choice.addButton(tr("With&out color bar"),
        QMessageBox::AcceptRole);
    choice.addButton(QMessageBox::Cancel);
    choice.exec();
    if (choice.clickedButton() != withBar && choice.clickedButton() != withoutBar) {
        return;
    }
    const bool includeColorBar = choice.clickedButton() == withBar;

    auto filename = QFileDialog::getSaveFileName(
        this, tr("Export scalar image"), QString(), tr("PNG image (*.png)"));
    if (filename.isEmpty()) {
        return;
    }

    // Strip a trailing ".png" (case-insensitive) to get the base name; the
    // per-panel suffix is inserted before the extension is re-appended.
    QString base = filename;
    if (base.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
        base.chop(4);
    } else {
        // The dialog does not auto-append on Linux; ensure we don't double it.
        filename += QStringLiteral(".png");
    }

    if (m_viewDimension == 3) {
        // Export all three panels: foo_xy.png, foo_xz.png, foo_yz.png.
        constexpr std::array<const char*, 3> suffixes{"_yz", "_xz", "_xy"};
        for (int normal = 0; normal < 3; ++normal) {
            const auto idx = static_cast<std::size_t>(normal);
            auto* panelView = m_planeViews[idx].view;
            if (panelView == nullptr || !panelView->hasImage()) {
                continue;
            }
            const auto outPath = base
                + QString::fromLatin1(suffixes[idx]) + QStringLiteral(".png");
            const qreal scale = std::max(1.0,
                panelView->transform().m11());
            const QImage composite = composeExportFrame(
                panelView, includeColorBar, scale);
            if (composite.isNull() || !composite.save(outPath, "PNG")) {
                QMessageBox::critical(this, tr("Cannot export image"),
                    tr("Could not write %1.").arg(outPath));
            }
        }
    } else {
        const qreal exportScale = std::max(1.0, view->transform().m11());
        const QImage composite = composeExportFrame(
            view, includeColorBar, exportScale);
        if (composite.isNull()) {
            QMessageBox::critical(this, tr("Cannot export image"),
                tr("The image could not be composited."));
            return;
        }
        if (!composite.save(filename, "PNG")) {
            QMessageBox::critical(this, tr("Cannot export image"),
                tr("The image could not be written to %1.").arg(filename));
        }
    }
}

QImage MainWindow::composeExportFrame(const ImageView* view,
    bool includeColorBar, qreal scaleFactor) const
{
    if (view == nullptr) {
        return {};
    }
    const QImage scalar = view->composedImage(scaleFactor);
    if (scalar.isNull() || !includeColorBar) {
        return scalar;
    }
    constexpr int gap = 8;
    const int barWidth = m_colorBar->preferredWidth();
    QImage composite(QSize(scalar.width() + gap + barWidth, scalar.height()),
        QImage::Format_ARGB32_Premultiplied);
    {
        QPainter painter(&composite);
        painter.setFont(m_colorBar->font());
        painter.fillRect(composite.rect(), viewportBackground());
        painter.drawImage(0, 0, scalar);
        m_colorBar->paintBar(&painter,
            QRect(scalar.width() + gap, 0, barWidth, composite.height()));
    }
    return composite;
}

void MainWindow::exportAnimation()
{
    if (m_animationExporter->active()) {
        return;
    }
    if (m_sequenceFrames.empty()) {
        QMessageBox::information(this, tr("No animation"),
            tr("Open a plotfile sequence before exporting an animation."));
        return;
    }
    auto* view = m_activeView != nullptr ? m_activeView->view : nullptr;
    if (view == nullptr || !view->hasImage()) {
        QMessageBox::information(this, tr("No image"),
            tr("Open a dataset before exporting an animation."));
        return;
    }

    // Color-bar choice (same options as single-image export); applies to all.
    QMessageBox choice(this);
    choice.setIcon(QMessageBox::Question);
    choice.setWindowTitle(tr("Export Animation"));
    choice.setText(tr("Include the color bar in every frame?"));
    auto* withBar = choice.addButton(tr("&With color bar"), QMessageBox::AcceptRole);
    auto* withoutBar = choice.addButton(tr("With&out color bar"), QMessageBox::AcceptRole);
    choice.addButton(QMessageBox::Cancel);
    choice.exec();
    if (choice.clickedButton() != withBar && choice.clickedButton() != withoutBar) {
        return;
    }
    const bool includeColorBar = choice.clickedButton() == withBar;

    // The chosen file's directory and basename (minus extension) become the
    // output location and the PNG/MP4 stem, e.g. "runs/anim.png" ->
    // runs/anim_0000.png ... runs/anim.mp4.
    const auto settings = makeSettings();
    const auto path = QFileDialog::getSaveFileName(this,
        tr("Export animation"),
        settings.value(QStringLiteral("lastOpenDirectory")).toString(),
        tr("PNG image (*.png)"));
    if (path.isEmpty()) {
        return;
    }
    beginAnimationExport(path, includeColorBar);
}

void MainWindow::startAnimationExportForTest(const QString& path,
    bool includeColorBar)
{
    if (m_animationExporter->active()) {
        return;
    }
    beginAnimationExport(path, includeColorBar);
}

void MainWindow::beginAnimationExport(const QString& path, bool includeColorBar)
{
    auto* view = m_activeView != nullptr ? m_activeView->view : nullptr;
    if (view == nullptr || !view->hasImage() || m_sequenceFrames.empty()) {
        return;
    }
    // Freeze the export zoom from the current view so every frame renders at the
    // same dimensions even if a frame's image size changes and refits the view.
    // In 3-D this single scale is shared by all three panels, so a panel whose
    // fitted zoom differs from the active view exports at the active view's
    // scale -- constant across frames, which is the goal.
    const auto scale = std::max(1.0, view->transform().m11());
    std::vector<QString> suffixes;
    if (m_viewDimension == 3) {
        suffixes = {QStringLiteral("_yz"), QStringLiteral("_xz"),
            QStringLiteral("_xy")};
    } else {
        suffixes = {QString()};
    }
    if (!m_animationExporter->begin(path, includeColorBar,
            static_cast<int>(m_sequenceFrames.size()), m_sequenceIndex, scale,
            std::move(suffixes), this)) {
        return;
    }

    // Freeze the action and stop playback while exporting.
    m_exportAnimationAction->setEnabled(false);
    setPlaybackMode(PlaybackMode::None);

    goToSequenceFrame(0);
}

std::optional<DatasetRequest> MainWindow::buildDatasetRequest() const
{
    if (!m_dataset || m_activeView == nullptr
        || m_activeView->plane.width <= 0 || m_activeView->plane.height <= 0
        || m_fieldSelector->currentIndex() < 0) {
        return std::nullopt;
    }
    const auto& metadata = m_dataset->metadata();
    DatasetRequest request;
    request.dataset = m_dataset;
    request.field.value = m_fieldSelector->currentData().toUInt();
    request.fieldName = tr("%1 — %2").arg(m_activeView->label)
        .arg(QString::fromStdString(
            metadata.fields[request.field.value].name));
    // The "selected region" is the active view's visible region: the
    // rubber-band zoom, or the whole domain when fitted.
    request.region = m_activeView->plane.physicalRegion;
    request.normalAxis = m_activeView->normal;
    if (metadata.dimension == 3) {
        request.slicePosition
            = m_slicePosition3d[static_cast<std::size_t>(m_activeView->normal)];
    }
    return request;
}

void MainWindow::showDatasetWindow()
{
    auto request = buildDatasetRequest();
    if (!request.has_value()) {
        return;
    }
    // One instance at a time: a new window replaces the old one.
    closeDatasetWindow();
    auto* window = new DatasetWindow(*request);
    window->setNumberFormat(m_numberFormat);
    m_datasetWindow = window;
    connect(window, &QObject::destroyed, this, [this, window] {
        if (m_datasetWindow == window) {
            m_datasetWindow = nullptr;
        }
        for (auto* state : currentViews()) {
            state->view->setCellHighlight(std::nullopt);
        }
    });
    connect(window, &DatasetWindow::extractionFailed, this,
        &MainWindow::reportBackgroundError);
    connect(window, &DatasetWindow::cellActivated, this,
        [this](const RealBox& physicalCell) {
            datasetCellActivated(physicalCell);
        });
    connect(window, &DatasetWindow::refreshRequested, this,
        [this] { refreshDatasetWindow(); });
    window->show();
    window->raise();
    window->activateWindow();
}

void MainWindow::closeDatasetWindow()
{
    auto* window = m_datasetWindow;
    m_datasetWindow = nullptr;
    if (window != nullptr) {
        window->close();
    }
}

void MainWindow::refreshDatasetWindow()
{
    if (m_datasetWindow == nullptr) {
        return;
    }
    auto request = buildDatasetRequest();
    if (!request.has_value()) {
        closeDatasetWindow();
        return;
    }
    m_datasetWindow->reload(*request);
}

void MainWindow::datasetCellActivated(const RealBox& physicalCell)
{
    if (m_activeView == nullptr) {
        return;
    }
    const auto& plane = m_activeView->plane;
    if (plane.width <= 0 || plane.height <= 0) {
        return;
    }
    const auto axes = displayAxes(m_activeView->normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto& region = plane.physicalRegion;
    const auto xExtent = region.upper[xAxis] - region.lower[xAxis];
    const auto yExtent = region.upper[yAxis] - region.lower[yAxis];
    // Same physical-to-scene mapping updateGridBoxes applies; plane row 0 is
    // the image bottom, so scene y runs opposite to physical y.
    const auto pixelX0 = (physicalCell.lower[xAxis] - region.lower[xAxis])
        / xExtent * plane.width;
    const auto pixelX1 = (physicalCell.upper[xAxis] - region.lower[xAxis])
        / xExtent * plane.width;
    const auto pixelY0 = plane.height
        - (physicalCell.upper[yAxis] - region.lower[yAxis])
            / yExtent * plane.height;
    const auto pixelY1 = plane.height
        - (physicalCell.lower[yAxis] - region.lower[yAxis])
            / yExtent * plane.height;
    QRectF rectangle(QPointF(pixelX0, pixelY0), QPointF(pixelX1, pixelY1));
    rectangle = rectangle.normalized().intersected(
        QRectF(0.0, 0.0, plane.width, plane.height));
    std::optional<QRectF> highlight;
    if (!rectangle.isEmpty()) {
        highlight = rectangle;
    }
    m_activeView->view->setCellHighlight(highlight);
}

void MainWindow::openDataset(
    const std::filesystem::path& path, bool metadataOnly)
{
    openDatasetImpl(
        path, metadataOnly, std::nullopt, {}, false, std::nullopt);
}

void MainWindow::openDatasetImpl(const std::filesystem::path& path,
    bool metadataOnly,
    std::optional<PlotfileMetadataResult> preparedMetadata,
    std::filesystem::path dataRoot, bool preserveFabSelector,
    std::optional<FrameSliceSpec> initialSpec)
{
    if (!preserveFabSelector) {
        m_fabMode = false;
        m_multifabReturn.reset();
        m_fabSourceMetadata.reset();
        m_fabSourcePath.clear();
        m_fabDataRoot.clear();
        m_fabSelectorDock->setEntries({});
        m_fabSelectorDock->setBackAvailable(false);
        m_fabSelectorDock->setVisible(false);
    }
    // Opening a single dataset ends any plotfile sequence and stops playback
    // of either animation mode.
    setPlaybackMode(PlaybackMode::None);
    closeSequence();
    resetRangeState();
    // Invalidate every in-flight per-view slice and reset the view states.
    const std::array<PlaneViewState*, 4> states{
        &m_view2d, &m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
    for (auto* state : states) {
        state->stopSource.request_stop();
        ++state->sliceGeneration;
        state->view->setPlaceholder(tr("Loading dataset..."));
        state->plane = {};
        state->contourPlane = {};
        state->contourFinePlane = {};
        state->contourFineFactor = 1;
        state->contourPolylines.clear();
        state->fieldName.clear();
        state->visibleRegion.reset();
        state->vectorSegments.clear();
        state->cachedRequest = {};
        state->hasCachedRequest = false;
        state->cachedMode = DisplayMode::Raster;
        state->cachedVectorUField = 0;
        state->cachedVectorVField = 0;
        state->cachedContourCount = 0;
    }
    m_initialStopSource.request_stop();
    m_linePlotStopSource.request_stop();
    m_pendingAllViews = false;
    m_pendingViews.clear();
    m_sliceDebounce->stop();
    m_controlsReady = false;
    m_viewDimension = 0;
    if (m_activeView != nullptr) {
        m_activeView->view->setActiveBorder(false);
    }
    m_activeView = nullptr;
    m_dataset.reset();
    // Line plot curves are snapshots of this dataset; drop the window.
    auto* linePlotWindow = m_linePlotWindow;
    m_linePlotWindow = nullptr;
    if (linePlotWindow != nullptr) {
        linePlotWindow->close();
    }
    // The dataset window shows this dataset's raw values; drop it too.
    closeDatasetWindow();
    if (m_contoursDialog != nullptr) {
        auto* dialog = m_contoursDialog;
        m_contoursDialog = nullptr;
        dialog->close();
    }
    if (m_numberFormatDialog != nullptr) {
        auto* dialog = m_numberFormatDialog;
        m_numberFormatDialog = nullptr;
        dialog->close();
    }
    m_datasetPath = path;
    m_lastBlocksRead = 0;
    m_lastCacheHits = 0;
    m_lastPayloadBytesRead = 0;
    m_cacheBudgetBytes = 0;
    m_cacheResidentBytes = 0;
    m_cachePinnedBytes = 0;
    m_cacheEvictions = 0;
    m_fieldSelector->setEnabled(false);
    m_levelSelector->setEnabled(false);
    m_rangeMode->setEnabled(false);
    m_logarithmic->setEnabled(false);
    m_boxesAction->setEnabled(false);
    m_slicePlanesAction->setEnabled(false);
    m_rangeMinimum->setEnabled(false);
    m_rangeMaximum->setEnabled(false);
    m_slicePositionControls->setVisible(false);
    m_animationPanel->setSweepVisible(false);
    m_levelMenu->setEnabled(false);
    m_contoursAction->setEnabled(false);
    m_datasetAction->setEnabled(false);
    m_exportAnimationAction->setEnabled(false);
    m_openMetadata.reset();
    m_fileVersion.clear();
    m_probeLines.clear();
    m_vectorUField = -1;
    m_vectorVField = -1;
    m_vectorWField = -1;
    setWindowTitle(tr("AMReXplorer"));
    {
        auto settings = makeSettings();
        settings.setValue(QStringLiteral("lastOpenDirectory"),
            QString::fromStdString(path.parent_path().string()));
    }
    m_probeLabel->clear();
    m_colorBar->clearRange();
    const auto generation = ++m_generation;
    m_metadataStopSource.request_stop();
    m_metadataStopSource = StopSource{};
    const auto metadataCancellation = m_metadataStopSource.get_token();
    ++m_activeRequests;
    statusBar()->showMessage(tr("Reading metadata for %1...").arg(
        QString::fromStdString(path.string())));
    updateDiagnostics();

    auto* watcher = new QFutureWatcher<PlotfileMetadataResult>(this);
    connect(watcher, &QFutureWatcher<PlotfileMetadataResult>::finished, this,
        [this, watcher, generation, path, metadataOnly,
            dataRoot = std::move(dataRoot), preserveFabSelector,
            initialSpec = std::move(initialSpec)]() mutable {
            --m_activeRequests;
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            try {
                auto result = watcher->result();
                if (generation == m_generation) {
                    showMetadata(result, path);
                    if (!preserveFabSelector) {
                        configureFabSelector(result, path);
                    }
                    emit datasetOpenFinished(true);
                    if (!metadataOnly) {
                        auto root = std::move(dataRoot);
                        if (root.empty()) {
                            root = std::filesystem::is_directory(path)
                                ? path : path.parent_path();
                            if (root.empty()) {
                                root = ".";
                            }
                        }
                        requestInitialSlice(path, generation, result,
                            std::move(root), std::move(initialSpec));
                    }
                } else {
                    ++m_staleResults;
                }
            } catch (const std::exception& error) {
                if (generation == m_generation) {
                    reportBackgroundError(
                        tr("Cannot open dataset: %1").arg(exceptionMessage(error)));
                    emit datasetOpenFinished(false);
                } else {
                    ++m_staleResults;
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [path, preparedMetadata = std::move(preparedMetadata),
            cancellation = metadataCancellation]() mutable {
        if (preparedMetadata) {
            return std::move(*preparedMetadata);
        }
        return readDatasetMetadata(path, cancellation);
    }));
}

void MainWindow::requestInitialSlice(
    const std::filesystem::path& path, std::uint64_t generation,
    std::optional<PlotfileMetadataResult> preparedMetadata,
    std::filesystem::path dataRoot,
    std::optional<FrameSliceSpec> initialSpec)
{
    validateVectorMode();
    const auto& metadata = *m_openMetadata;
    m_viewDimension = metadata.dimension;
    const auto views = currentViews();
    // The XY view starts out as the active one in 3-D.
    setActiveView(m_viewDimension == 3
        ? m_planeViews[2] : m_view2d);
    // Slice positions start at the domain midpoints unless a reversible FAB
    // transition is restoring the previous MultiFab view.
    const auto dataBounds = datasetSampleBounds(metadata);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto lower = dataBounds.lower[axis];
        const auto upper = dataBounds.upper[axis];
        m_slicePosition3d[axis] = initialSpec
            ? std::clamp(initialSpec->slicePositions[axis], lower,
                std::nextafter(upper, lower))
            : lower + 0.5 * (upper - lower);
    }
    m_initialStopSource.request_stop();
    m_linePlotStopSource.request_stop();
    m_initialStopSource = StopSource{};
    const auto cancellation = m_initialStopSource.get_token();
    // The initial open uses default slice state: field 0, finest available,
    // file range (falling back to Visible when metadata statistics are
    // unavailable), linear scale, whole domain, midpoint positions.
    FrameSliceSpec spec = initialSpec.value_or(FrameSliceSpec{});
    if (!initialSpec) {
        spec.palette = m_palette;
        spec.displayMode = m_displayMode;
        spec.vectorUField =
            static_cast<std::uint32_t>(std::max(m_vectorUField, 0));
        spec.vectorVField =
            static_cast<std::uint32_t>(std::max(m_vectorVField, 0));
        spec.vectorWField =
            static_cast<std::uint32_t>(std::max(m_vectorWField, 0));
        spec.contourCount = m_contourCount;
    }
    const auto restoredSpec = initialSpec;
    // Per-view generations captured now: a view that gets a newer request
    // before the initial slices land keeps its newer data.
    std::vector<std::uint64_t> viewGenerations;
    viewGenerations.reserve(views.size());
    for (const auto* state : views) {
        viewGenerations.push_back(state->sliceGeneration);
    }
    ++m_activeRequests;
    statusBar()->showMessage(tr("Loading initial slice..."));
    updateDiagnostics();

    auto* watcher = new QFutureWatcher<InitialSliceResult>(this);
    connect(watcher, &QFutureWatcher<InitialSliceResult>::finished, this,
        [this, watcher, generation, cancellation, views, viewGenerations,
            restoredSpec] {
            --m_activeRequests;
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            try {
                auto result = watcher->result();
                if (generation == m_generation) {
                    m_dataset = result.dataset;
                    configureSliceControls();
                    if (restoredSpec) {
                        const QSignalBlocker fieldBlocker(m_fieldSelector);
                        const QSignalBlocker levelBlocker(m_levelSelector);
                        const QSignalBlocker rangeBlocker(m_rangeMode);
                        const QSignalBlocker logBlocker(m_logarithmic);
                        const auto fieldIndex = m_fieldSelector->findData(
                            restoredSpec->field);
                        if (fieldIndex >= 0) {
                            m_fieldSelector->setCurrentIndex(fieldIndex);
                        }
                        const auto levelIndex = m_levelSelector->findData(
                            restoredSpec->levelSelection);
                        if (levelIndex >= 0) {
                            m_levelSelector->setCurrentIndex(levelIndex);
                        }
                        m_rangeMode->setCurrentIndex(
                            m_rangeMode->findData(
                                static_cast<int>(restoredSpec->rangeMode)));
                        m_logarithmic->setChecked(restoredSpec->logarithmic);
                        m_trackedField =
                            m_fieldSelector->currentData().toUInt();
                        m_fieldRanges[m_trackedField] = {
                            restoredSpec->rangeMode, restoredSpec->userRange};
                        if (restoredSpec->userRange) {
                            m_rangeMinimum->setValue(
                                restoredSpec->userRange->first);
                            m_rangeMaximum->setValue(
                                restoredSpec->userRange->second);
                        }
                        updateRangeModeAvailability();
                        const auto userRange =
                            static_cast<RangeMode>(
                                m_rangeMode->currentData().toInt())
                            == RangeMode::User;
                        m_rangeMinimum->setEnabled(userRange);
                        m_rangeMaximum->setEnabled(userRange);
                        configureSlicePositionControls();
                        syncMenuChecks();
                    }
                    if (selectCacheFallbackLevel(
                            m_levelSelector, result.cacheFallbackToLevel)) {
                        configureSlicePositionControls();
                        updateRangeModeAvailability();
                        syncMenuChecks();
                    }
                    if (result.displays.size() != views.size()) {
                        throw std::runtime_error(
                            "initial slice count does not match the view set");
                    }
                    for (std::size_t index = 0; index < views.size(); ++index) {
                        if (views[index]->sliceGeneration
                            != viewGenerations[index]) {
                            continue;
                        }
                        // A FAB round-trip preserved the zoom in restoredSpec and
                        // executeFrameLoad rendered the slice against it, but
                        // openDatasetImpl reset the view states. Restore the zoom
                        // from the region that actually produced this plane (so it
                        // stays in step with the slice cache key), gated on
                        // whether the spec recorded a zoom for this view — a
                        // full-domain view stays nullopt, without float-comparing
                        // regions. See fab-round-trip-loses-visible-region.
                        if (restoredSpec) {
                            const bool wasZoomed =
                                index < restoredSpec->visibleRegions.size()
                                && restoredSpec->visibleRegions[index]
                                    .has_value();
                            views[index]->visibleRegion = wasZoomed
                                ? std::optional<RealBox>{
                                    result.displays[index].request.visibleRegion}
                                : std::optional<RealBox>{};
                        }
                        showSlice(*views[index], result.displays[index]);
                    }
                    const auto cache = m_dataset->cacheMetrics();
                    m_cacheBudgetBytes = cache.budgetBytes;
                    m_cacheResidentBytes = cache.residentBytes;
                    m_cachePinnedBytes = cache.pinnedBytes;
                    m_cacheEvictions = cache.evictions;
                    if (result.cacheFallbackToLevel >= 0) {
                        // Non-modal: an informational cache-fallback notice must
                        // not pop a modal dialog that would block the quit path.
                        statusBar()->showMessage(cacheFallbackMessage(
                            *result.dataset, result.cacheFallbackFromLevel,
                            result.cacheFallbackToLevel));
                    }
                    emit initialSliceFinished(true);
                } else {
                    ++m_staleResults;
                }
            } catch (const std::exception& error) {
                if (generation == m_generation && !cancellation.stop_requested()) {
                    reportBackgroundError(
                        tr("Cannot load slice: %1").arg(exceptionMessage(error)));
                    emit initialSliceFinished(false);
                } else {
                    ++m_staleResults;
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [path, generation, spec = std::move(spec), cancellation,
            preparedMetadata = std::move(preparedMetadata),
            dataRoot = std::move(dataRoot)]() mutable {
        return executeFrameLoad(path, DatasetId{generation}, spec,
            initialCacheBudget(), cancellation,
            std::move(preparedMetadata), std::move(dataRoot));
    }));
}

void MainWindow::configureSliceControls()
{
    if (!m_dataset) {
        return;
    }
    const QSignalBlocker fieldBlocker(m_fieldSelector);
    const QSignalBlocker levelBlocker(m_levelSelector);
    const auto& metadata = m_dataset->metadata();

    m_fieldSelector->clear();
    for (std::size_t field = 0; field < metadata.fields.size(); ++field) {
        m_fieldSelector->addItem(QString::fromStdString(metadata.fields[field].name),
            static_cast<unsigned int>(field));
    }
    m_fieldSelector->setCurrentIndex(0);

    populateLevelCombo(m_levelSelector, metadata.finestLevel);
    m_levelSelector->setCurrentIndex(0);

    m_controlsReady = true;
    m_fieldSelector->setEnabled(true);
    m_levelSelector->setEnabled(true);
    m_rangeMode->setEnabled(true);
    m_logarithmic->setEnabled(true);
    m_boxesAction->setEnabled(true);
    m_slicePlanesAction->setEnabled(metadata.dimension == 3);
    const auto userRange = static_cast<RangeMode>(
        m_rangeMode->currentData().toInt()) == RangeMode::User;
    m_rangeMinimum->setEnabled(userRange);
    m_rangeMaximum->setEnabled(userRange);
    rebuildLevelMenu();
    m_levelMenu->setEnabled(true);
    m_contoursAction->setEnabled(true);
    m_datasetAction->setEnabled(true);

    rebuildVariableMenu();
    updateRangeModeAvailability();

    // Switch the stacked page to match the dataset dimension and, for 3-D,
    // reveal the shared slice position controls and the iso wireframe.
    const auto isThreeDimensional = metadata.dimension == 3;
    m_syncRubberBandZoomAction->setVisible(isThreeDimensional);
    m_stack->setCurrentIndex(isThreeDimensional ? 1 : 0);
    m_animationPanel->setSweepVisible(isThreeDimensional);
    updateAnimationDockVisibility();
    configureSlicePositionControls();
    if (isThreeDimensional) {
        m_isoWidget->setGeometry(metadata);
        m_isoWidget->setSlicePositions(m_slicePosition3d[0], m_slicePosition3d[1],
            m_slicePosition3d[2]);
    }
    ensureVectorFieldDefaults();
}

void MainWindow::configureSlicePositionControls()
{
    if (!m_dataset) {
        m_slicePositionControls->setVisible(false);
        return;
    }
    m_slicePositionControls->setVisible(true);
    const auto& md = m_dataset->metadata();

    if (md.dimension != 3) {
        // 2-D: dim rather than hide — there is no slice depth to control,
        // but the user can see Position is a 3-D-only concept.
        m_slicePositionControls->setEnabled(false);
        return;
    }

    const auto level = sliceIndexLevel();
    if (level < 0 || static_cast<std::size_t>(level) >= md.levels.size()) {
        m_slicePositionControls->setEnabled(false);
        return;
    }

    m_slicePositionControls->setEnabled(true);
    const auto& levelMd = md.levels[static_cast<std::size_t>(level)];
    for (std::size_t axis = 0; axis < 3; ++axis) {
        auto* spin = m_sliceSpinboxes[axis];
        const QSignalBlocker blocker(spin);
        // Cell-centered: indices from domain.lower to domain.upper inclusive.
        // Nodal data would have one extra node at the upper end: domain.upper+1.
        const auto iMin = levelMd.domain.lower[axis];
        const auto iMax = levelMd.domain.upper[axis];
        spin->setRange(iMin, iMax);
        spin->setSingleStep(1);
        spin->setValue(sliceIndexForPosition(md, level,
            static_cast<int>(axis), m_slicePosition3d[axis]));
    }
}

int MainWindow::sliceIndexLevel() const
{
    if (!m_dataset || m_dataset->metadata().dimension != 3) {
        return -1;
    }
    const auto levelData = m_levelSelector->currentData().toInt();
    return decodeLevelData(levelData, m_dataset->metadata().finestLevel).maximumLevel;
}

void MainWindow::setSlicePosition(int axis, double value)
{
    if (!m_dataset || m_dataset->metadata().dimension != 3) {
        return;
    }
    const auto ax = static_cast<std::size_t>(axis);
    const auto domain = datasetSampleBounds(m_dataset->metadata());
    const auto position = std::clamp(value, domain.lower[ax],
        std::nextafter(domain.upper[ax], domain.lower[ax]));
    m_slicePosition3d[ax] = position;
    {
        const QSignalBlocker blocker(m_sliceSpinboxes[ax]);
        const auto level = sliceIndexLevel();
        if (level >= 0 && static_cast<std::size_t>(level)
            < m_dataset->metadata().levels.size()) {
            m_sliceSpinboxes[ax]->setValue(sliceIndexForPosition(
                m_dataset->metadata(), level, axis, position));
        }
    }
    m_isoWidget->setSlicePositions(m_slicePosition3d[0], m_slicePosition3d[1],
        m_slicePosition3d[2]);
    // The cached full-domain Visible range is now stale.
    m_displayCoordinator.invalidateRangeCache();
    // The other two views only need their crosshair guides redrawn; the view
    // normal to the moved axis gets a fresh (debounced) slice.
    updateCrosshairs();
    scheduleSliceRequest(m_planeViews[ax]);
}

void MainWindow::scheduleSliceRequest(bool rasterDirty)
{
    if (m_controlsReady && m_dataset) {
        // Any slice-affecting UI change funnels through here; a prefetched
        // frame rendered against the old spec is obsolete.
        ++m_specGeneration;
        discardPrefetch();
        m_pendingRasterDirty = m_pendingRasterDirty || rasterDirty;
        m_pendingAllViews = true;
        m_sliceDebounce->start();
    }
}

void MainWindow::scheduleSliceRequest(PlaneViewState& state, bool rasterDirty)
{
    if (m_controlsReady && m_dataset) {
        ++m_specGeneration;
        discardPrefetch();
        m_pendingRasterDirty = m_pendingRasterDirty || rasterDirty;
        if (std::find(m_pendingViews.begin(), m_pendingViews.end(), &state)
            == m_pendingViews.end()) {
            m_pendingViews.push_back(&state);
        }
        m_sliceDebounce->start();
    }
}

void MainWindow::flushSliceRequests()
{
    std::vector<PlaneViewState*> targets;
    if (m_pendingAllViews) {
        targets = currentViews();
    } else {
        targets = m_pendingViews;
    }
    m_pendingAllViews = false;
    m_pendingViews.clear();
    const auto rasterDirty = m_pendingRasterDirty;
    m_pendingRasterDirty = false;
    for (auto* state : targets) {
        requestSlice(*state, rasterDirty);
    }
}

void MainWindow::requestSlice(PlaneViewState& state, bool rasterDirty)
{
    if (!m_controlsReady || !m_dataset
        || m_fieldSelector->currentIndex() < 0
        || m_levelSelector->currentIndex() < 0) {
        return;
    }
    updateRangeModeAvailability();

    const auto dataset = m_dataset;
    const auto& metadata = dataset->metadata();
    SliceRequest request;
    request.dataset = dataset->id();
    request.field.value = m_fieldSelector->currentData().toUInt();
    request.normalDirection = state.normal;
    if (metadata.dimension == 3) {
        request.physicalPosition
            = m_slicePosition3d[static_cast<std::size_t>(state.normal)];
    }
    request.visibleRegion = state.visibleRegion.value_or(
        datasetSampleBounds(metadata));
    request.outputSize = finestNativeOutputSize(
        metadata, request.visibleRegion, state.normal);
    const auto level = m_levelSelector->currentData().toInt();
    const auto [composition, maximumLevel] = decodeLevelData(
        level, metadata.finestLevel);
    request.composition = composition;
    request.maximumLevel = maximumLevel;

    const auto requestedRangeMode = static_cast<RangeMode>(
        m_rangeMode->currentData().toInt());
    const auto rangeMode = effectiveRangeMode(metadata, request.field,
        maximumLevel, composition, requestedRangeMode);
    std::optional<std::pair<double, double>> userRange;
    if (rangeMode == RangeMode::User) {
        userRange = std::pair{m_rangeMinimum->value(), m_rangeMaximum->value()};
    }
    const auto logarithmic = m_logarithmic->isChecked();
    const auto palette = m_palette;
    const auto displayMode = m_displayMode;
    // Each 3-D panel uses a different pair of vector components:
    //   XY (normal=2) → U,V   XZ (normal=1) → U,W   YZ (normal=0) → V,W
    // 2-D always uses U,V.
    const auto u = static_cast<std::uint32_t>(std::max(m_vectorUField, 0));
    const auto v = static_cast<std::uint32_t>(std::max(m_vectorVField, 0));
    const auto w = static_cast<std::uint32_t>(std::max(m_vectorWField, 0));
    const auto vectorUField = (metadata.dimension == 3 && state.normal == 0) ? v : u;
    const auto vectorVField = (metadata.dimension == 3)
        ? (state.normal == 2 ? v : w) : v;
    const auto contourCount = m_contourCount;

    const auto fromCache = state.hasCachedRequest
        && state.plane.width > 0
        && sameSliceSpec(state.cachedRequest, request)
        && state.cachedVectorVField == vectorVField
        && state.cachedVectorUField == vectorUField
        && displayMode == state.cachedMode
        && (!isContourMode(displayMode) || state.contourFinePlane.width > 0)
        && (displayMode != DisplayMode::VelocityVectors
            || (!state.vectorSegments.empty()
                && contourCount == state.cachedContourCount));

    state.stopSource.request_stop();
    state.stopSource = StopSource{};
    const auto cancellation = state.stopSource.get_token();
    const auto generation = m_generation;
    const auto sliceGeneration = ++state.sliceGeneration;
    ++state.pendingRequests;
    ++m_activeRequests;
    const auto tag = m_viewDimension == 3
        ? tr(" (%1)").arg(state.label) : QString();
    statusBar()->showMessage(tr("Loading %1%2...").arg(
        m_fieldSelector->currentText(), tag));
    updateDiagnostics();

    QFuture<SliceDisplayResult> future;
    if (fromCache) {
        // Cheap path: re-range, re-render, and re-contour the cached planes
        // on a worker; no SliceQuery runs at all.
        future = QtConcurrent::run([dataset, request,
            displayPlane = state.plane,
            contourPlane = state.contourPlane,
            contourFinePlane = state.contourFinePlane,
            contourFineFactor = state.contourFineFactor,
            vectors = state.vectorSegments,
            rangeMode, userRange, logarithmic, palette, displayMode,
            vectorUField, vectorVField, contourCount, rasterDirty]() mutable {
            return refreshCachedSlice(dataset, request, std::move(displayPlane),
                std::move(contourPlane), std::move(contourFinePlane),
                contourFineFactor, std::move(vectors), rangeMode, userRange,
                logarithmic, palette, displayMode, vectorUField, vectorVField,
                contourCount, rasterDirty);
        });
    } else {
        future = QtConcurrent::run(
            [dataset, request, rangeMode, userRange, logarithmic, palette,
                cancellation, displayMode, vectorUField, vectorVField,
                contourCount]() mutable {
            // The pipeline owns the whole non-cached slice worker, including
            // the cache-pressure level fallback (see
            // cache-budget-exceeded-hard-fails-after-load).
            return executeSliceWithFallback(dataset, request, rangeMode,
                userRange, logarithmic, palette, displayMode, vectorUField,
                vectorVField, contourCount, cancellation);
        });
    }

    auto* watcher = new QFutureWatcher<SliceDisplayResult>(this);
    connect(watcher, &QFutureWatcher<SliceDisplayResult>::finished, this,
        [this, watcher, dataset, generation, sliceGeneration, cancellation,
         &state, rangeMode] {
            --state.pendingRequests;
            --m_activeRequests;
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            try {
                auto result = watcher->result();
                if (generation == m_generation
                    && sliceGeneration == state.sliceGeneration) {
                    // Cache the full-domain range whenever we get a non-zoomed
                    // Visible-range slice; reuse it for zoomed (subregion)
                    // slices so the color bar stays stable during pan and zoom.
                    const bool isFullDomain = !state.visibleRegion.has_value();
                    const DisplayCoordinator::RangeKey rangeKey{
                        result.request.dataset, result.request.field,
                        result.request.maximumLevel,
                        result.request.composition};
                    const auto cachedRange = !isFullDomain
                        && rangeMode == RangeMode::Visible
                            ? m_displayCoordinator.cachedFullDomainRange(
                                rangeKey)
                            : std::nullopt;
                    if (cachedRange) {
                        // The subregion result was produced against its own
                        // range; realign it to the reused full-domain range
                        // so it matches the colorbar. In 3-D the shared-range
                        // sync below realigns every panel, so only 2-D (which
                        // it skips) realigns the raster and contours here —
                        // that also avoids rendering each 3-D panel twice.
                        DisplayCoordinator::realignArrivalToRange(result,
                            *cachedRange, m_palette, m_viewDimension != 3);
                    }
                    showSlice(state, result);
                    syncVisibleRanges();
                    // Refresh the cache after syncVisibleRanges so the 3-D
                    // union across all panels is captured.
                    if (isFullDomain && rangeMode == RangeMode::Visible
                        && state.plane.width > 0) {
                        m_displayCoordinator.storeFullDomainRange(rangeKey,
                            {state.displayMinimum, state.displayMaximum});
                    }
                    const auto cache = dataset->cacheMetrics();
                    m_cacheBudgetBytes = cache.budgetBytes;
                    m_cacheResidentBytes = cache.residentBytes;
                    m_cachePinnedBytes = cache.pinnedBytes;
                    m_cacheEvictions = cache.evictions;
                    // A cache-pressure fallback lowered the composite level;
                    // reflect it in the level combo (no re-slice) and inform the
                    // user, matching the initial-load handling.
                    if (result.cacheFallbackToLevel >= 0) {
                        if (selectCacheFallbackLevel(
                                m_levelSelector,
                                result.cacheFallbackToLevel)) {
                            configureSlicePositionControls();
                            updateRangeModeAvailability();
                            syncMenuChecks();
                        }
                        statusBar()->showMessage(cacheFallbackMessage(
                            *dataset, result.cacheFallbackFromLevel,
                            result.cacheFallbackToLevel));
                    }
                } else {
                    ++m_staleResults;
                }
            } catch (const std::exception& error) {
                if (generation == m_generation
                    && sliceGeneration == state.sliceGeneration
                    && !cancellation.stop_requested()) {
                    reportBackgroundError(
                        tr("Cannot load slice: %1").arg(exceptionMessage(error)));
                } else {
                    ++m_staleResults;
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
            // The interactive re-slice batch has drained once no view has work
            // in flight; the smoke test waits on this to read settled state.
            if (m_activeRequests == 0) {
                emit interactiveSlicesSettled();
            }
        });
    watcher->setFuture(future);
}

void MainWindow::updateGridBoxes(PlaneViewState& state)
{
    std::vector<GridBoxOverlay> overlays;
    if (!m_boxesAction->isChecked() || !m_dataset || !state.view->hasImage()
        || state.plane.width <= 0 || state.plane.height <= 0) {
        state.view->setGridBoxes(overlays);
        return;
    }

    const auto& metadata = m_dataset->metadata();
    const auto& plane = state.plane;
    const auto normal = metadata.dimension == 3 ? state.normal : -1;
    const auto axes = displayAxes(state.normal);
    const auto rawLevel = m_levelSelector->currentData().toInt();
    const auto [composition, maximumLevel] = decodeLevelData(
        rawLevel, metadata.finestLevel);
    const auto firstLevel = composition == CompositionPolicy::ExactLevel
        ? maximumLevel : 0;
    const auto lastLevel = maximumLevel;

    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto xExtent = plane.physicalRegion.upper[xAxis]
        - plane.physicalRegion.lower[xAxis];
    const auto yExtent = plane.physicalRegion.upper[yAxis]
        - plane.physicalRegion.lower[yAxis];
    for (int levelIndex = firstLevel; levelIndex <= lastLevel; ++levelIndex) {
        const auto& level = metadata.levels[static_cast<std::size_t>(levelIndex)];
        for (const auto& box : level.boxes) {
            const auto physicalBox = sampleBounds(
                level, box, metadata.dimension);
            if (normal >= 0) {
                // Only boxes intersecting this view's slice position show.
                const auto direction = static_cast<std::size_t>(normal);
                const auto normalLower = physicalBox.lower[direction];
                const auto normalUpper = physicalBox.upper[direction];
                const auto slicePosition
                    = m_slicePosition3d[static_cast<std::size_t>(normal)];
                if (slicePosition < normalLower || slicePosition >= normalUpper) {
                    continue;
                }
            }

            const auto xLower = physicalBox.lower[xAxis];
            const auto xUpper = physicalBox.upper[xAxis];
            const auto yLower = physicalBox.lower[yAxis];
            const auto yUpper = physicalBox.upper[yAxis];
            const auto pixelX0 = std::round(
                (xLower - plane.physicalRegion.lower[xAxis])
                    / xExtent * plane.width);
            const auto pixelX1 = std::round(
                (xUpper - plane.physicalRegion.lower[xAxis])
                    / xExtent * plane.width);
            const auto pixelY0 = std::round(plane.height
                - (yUpper - plane.physicalRegion.lower[yAxis])
                    / yExtent * plane.height);
            const auto pixelY1 = std::round(plane.height
                - (yLower - plane.physicalRegion.lower[yAxis])
                    / yExtent * plane.height);
            if (pixelX0 == pixelX1 || pixelY0 == pixelY1) {
                continue;
            }
            QRectF rectangle(QPointF(pixelX0, pixelY0), QPointF(pixelX1, pixelY1));
            rectangle = rectangle.normalized().intersected(
                QRectF(0.0, 0.0, plane.width, plane.height));
            if (!rectangle.isEmpty()) {
                const auto color = levelIndex == firstLevel
                    ? QColor(Qt::white)
                    : QColor::fromRgb(static_cast<QRgb>(
                        m_palette.levelColor(levelIndex, lastLevel)));
                overlays.push_back({rectangle, color});
            }
        }
    }
    state.view->setGridBoxes(overlays);
}

void MainWindow::updateGridBoxes()
{
    for (auto* state : currentViews()) {
        updateGridBoxes(*state);
    }
}

void MainWindow::updateCrosshairs(PlaneViewState& state)
{
    std::optional<QLineF> vertical;
    std::optional<QLineF> horizontal;
    QColor verticalColor;
    QColor horizontalColor;
    if (m_dataset && m_dataset->metadata().dimension == 3
        && state.plane.width > 0 && state.plane.height > 0) {
        const auto axes = displayAxes(state.normal);
        const auto xAxis = static_cast<std::size_t>(axes[0]);
        const auto yAxis = static_cast<std::size_t>(axes[1]);
        const auto& region = state.plane.physicalRegion;
        const auto width = static_cast<double>(state.plane.width);
        const auto height = static_cast<double>(state.plane.height);
        // The vertical guide marks the slice position of the axis pointing
        // horizontally in this view, and vice versa; each guide takes that
        // axis' legacy palette color and hides outside the displayed region.
        const auto xPosition = m_slicePosition3d[xAxis];
        if (xPosition >= region.lower[xAxis] && xPosition <= region.upper[xAxis]) {
            const auto t = (xPosition - region.lower[xAxis])
                / (region.upper[xAxis] - region.lower[xAxis]);
            vertical = QLineF(t * width, 0.0, t * width, height);
            verticalColor = sliceAxisColor(axes[0]);
        }
        const auto yPosition = m_slicePosition3d[yAxis];
        if (yPosition >= region.lower[yAxis] && yPosition <= region.upper[yAxis]) {
            const auto t = (yPosition - region.lower[yAxis])
                / (region.upper[yAxis] - region.lower[yAxis]);
            const auto sceneY = height * (1.0 - t);
            horizontal = QLineF(0.0, sceneY, width, sceneY);
            horizontalColor = sliceAxisColor(axes[1]);
        }
    }
    state.view->setCrosshairs(vertical, horizontal, verticalColor,
        horizontalColor);
}

void MainWindow::updateCrosshairs()
{
    for (auto* state : currentViews()) {
        updateCrosshairs(*state);
    }
}

void MainWindow::showMetadata(
    const PlotfileMetadataResult& result, const std::filesystem::path& path)
{
    m_metadataTree->clear();
    const auto& metadata = *result.metadata;
    const auto addValue = [this](const QString& name, const QString& value) {
        new QTreeWidgetItem(m_metadataTree, {name, value});
    };

    addValue(tr("Dataset"), QString::fromStdString(path.string()));
    addValue(tr("Format"), QString::fromStdString(result.fileVersion));
    addValue(tr("Dimension"), QString::number(metadata.dimension));
    addValue(tr("Time"), QString::number(metadata.time, 'g', 17));
    addValue(tr("Finest level"), QString::number(metadata.finestLevel));

    auto* fields = new QTreeWidgetItem(
        m_metadataTree, {tr("Fields"), QString::number(metadata.fields.size())});
    for (const auto& field : metadata.fields) {
        new QTreeWidgetItem(fields, {
            QString::fromStdString(field.name),
            tr("%1 component(s)").arg(field.componentCount)
        });
    }

    auto* levels = new QTreeWidgetItem(
        m_metadataTree, {tr("Levels"), QString::number(metadata.levels.size())});
    for (const auto& level : metadata.levels) {
        new QTreeWidgetItem(levels, {
            tr("Level %1").arg(level.level),
            tr("%1 grid(s), %2").arg(level.boxes.size()).arg(
                QString::fromStdString(level.dataPath))
        });
    }
    m_metadataTree->expandAll();

    m_openMetadata = result.metadata;
    m_fileVersion = result.fileVersion;
    updateWindowTitle();

    m_lastFilesRead = result.metrics.filesRead;
    m_lastBytesRead = result.metrics.bytesRead;
    statusBar()->showMessage(tr("Metadata loaded: %1 field(s), %2 level(s)")
        .arg(metadata.fields.size()).arg(metadata.levels.size()));
}

std::optional<QRectF> MainWindow::preservedDataWindow(
    const PlaneViewState& state, const ScalarPlane& incoming) const
{
    const auto& cached = state.plane;
    const auto axes = displayAxes(state.normal);
    // Equal densities (or degenerate geometry) mean the preserved scene
    // transform already preserves the on-screen data, so leave it alone; the
    // coordinator owns that decision.
    if (!DisplayCoordinator::planeDensitiesDiffer(cached, incoming, axes)) {
        return std::nullopt;
    }
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto& oldRegion = cached.physicalRegion;
    const auto& newRegion = incoming.physicalRegion;
    const auto oldExtentX = oldRegion.upper[xAxis] - oldRegion.lower[xAxis];
    const auto oldExtentY = oldRegion.upper[yAxis] - oldRegion.lower[yAxis];
    const auto newExtentX = newRegion.upper[xAxis] - newRegion.lower[xAxis];
    const auto newExtentY = newRegion.upper[yAxis] - newRegion.lower[yAxis];
    // Viewport -> old scene -> physical -> new scene. Scene y runs opposite
    // to physical y: plane row 0 is the bottom row and the displayed raster
    // is mirrored vertically (see displayImageFor), for both planes alike.
    const auto visible = state.view->mapToScene(
        state.view->viewport()->rect()).boundingRect();
    const auto dataX = [&](double sceneX) {
        return oldRegion.lower[xAxis] + sceneX / cached.width * oldExtentX;
    };
    const auto dataY = [&](double sceneY) {
        return oldRegion.upper[yAxis] - sceneY / cached.height * oldExtentY;
    };
    const auto newSceneX = [&](double x) {
        return (x - newRegion.lower[xAxis]) / newExtentX * incoming.width;
    };
    const auto newSceneY = [&](double y) {
        return (newRegion.upper[yAxis] - y) / newExtentY * incoming.height;
    };
    const QRectF window(
        QPointF(newSceneX(dataX(visible.left())),
            newSceneY(dataY(visible.top()))),
        QPointF(newSceneX(dataX(visible.right())),
            newSceneY(dataY(visible.bottom()))));
    if (window.isEmpty()) {
        return std::nullopt;
    }
    return window;
}

void MainWindow::showSlice(PlaneViewState& state, const SliceDisplayResult& display)
{
    if (!display.rasterUnchanged) {
        if (!display.image.valid()) {
            throw std::runtime_error("renderer produced an invalid image");
        }
        // Preserve/Refit/GeometryAware from the cached-vs-incoming request
        // pair; the rationale lives with the decision in the coordinator.
        const auto transformPolicy = DisplayCoordinator::rasterTransformPolicy(
            state.hasCachedRequest, state.cachedRequest, display.request,
            state.visibleRegion.has_value());
        // Preserve keeps the scene transform, which is only equivalent to
        // keeping what the user sees while the raster's pixels-per-data
        // density is unchanged. A zoomed re-slice can arrive denser: the
        // full-domain raster is capped at maxSliceOutputDimension while a
        // subregion fits under the cap, so preserving the scene transform
        // would show the crop over-zoomed with part of it off screen
        // (issue #45). When the density changes, preserve the visible *data*
        // window instead: capture the viewport in physical coordinates
        // through the old plane's geometry before the swap, then re-frame
        // that window through the new plane's geometry after it. Equal
        // densities (pan, uncapped zoom) keep the plain Preserve behavior.
        std::optional<QRectF> dataWindowInNewScene;
        if (transformPolicy == ImageTransformPolicy::Preserve) {
            dataWindowInNewScene = preservedDataWindow(
                state, display.slice.plane);
        }
        state.view->setImage(
            displayImageFor(display.image), transformPolicy);
        if (dataWindowInNewScene) {
            state.view->zoomToRect(*dataWindowInNewScene);
        }
    }
    state.plane = display.slice.plane;
    state.contourPlane = display.contourPlane;
    state.contourFinePlane = display.contourFinePlane;
    state.contourFineFactor = display.contourFineFactor;
    state.contourPolylines = display.contourPolylines;
    const auto fieldName = QString::fromStdString(display.fieldName);
    state.fieldName = fieldName;
    state.displayMinimum = display.minimum;
    state.displayMaximum = display.maximum;
    state.displayLogarithmic = display.logarithmic;
    state.vectorSegments = display.vectors;
    // Cache key for the re-render-from-cache path (see requestSlice).
    state.cachedRequest = display.request;
    state.hasCachedRequest = true;
    state.cachedMode = display.mode;
    state.cachedVectorUField = display.vectorUField;
    state.cachedVectorVField = display.vectorVField;
    state.cachedContourCount = display.contourCount;
    if (m_activeView == &state) {
        m_colorBar->setLogarithmic(display.logarithmic);
        m_colorBar->setFieldRange(
            display.logarithmic ? fieldName + tr(" (log)") : fieldName,
            display.minimum, display.maximum);
        // If log was requested but fell back to linear, reflect that in the
        // checkbox so the user sees log did not apply.
        if (m_logarithmic->isChecked() != display.logarithmic) {
            const QSignalBlocker logarithmicBlocker(m_logarithmic);
            m_logarithmic->setChecked(display.logarithmic);
        }
        if (static_cast<RangeMode>(m_rangeMode->currentData().toInt())
            != RangeMode::User) {
            const QSignalBlocker minimumBlocker(m_rangeMinimum);
            const QSignalBlocker maximumBlocker(m_rangeMaximum);
            m_rangeMinimum->setValue(display.minimum);
            m_rangeMaximum->setValue(display.maximum);
        }
    }
    updateGridBoxes(state);
    updateOverlay(state);
    // This view's region may have changed; refresh every view's guides.
    updateCrosshairs();

    m_lastBlocksRead = display.slice.metrics.blocksRead;
    m_lastCacheHits = display.slice.metrics.cacheHits;
    m_lastPayloadBytesRead = display.slice.metrics.payloadBytesRead;
    statusBar()->clearMessage();
}

void MainWindow::syncVisibleRanges()
{
    if (m_viewDimension != 3 || !m_dataset) {
        return;
    }
    const auto rangeMode = static_cast<RangeMode>(
        m_rangeMode->currentData().toInt());
    if (rangeMode != RangeMode::Visible) {
        return;
    }
    std::array<PlaneViewState*, 3> views{
        &m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
    const bool logarithmic = m_logarithmic->isChecked();

    // The coordinator resolves the shared range (the cached full-domain
    // range when current, so the color bar stays stable during zoom and pan;
    // else the union of the panels' finite extrema) and produces every
    // panel's raster and contours realigned to it. This method only blits
    // the updates into the views.
    const FieldId currentField{m_fieldSelector->currentData().toUInt()};
    const auto rawLevel = m_levelSelector->currentData().toInt();
    const auto [composition, maximumLevel] = decodeLevelData(
        rawLevel, m_dataset->metadata().finestLevel);
    std::array<DisplayCoordinator::PanelSyncInput, 3> inputs;
    for (std::size_t index = 0; index < views.size(); ++index) {
        const auto* state = views[index];
        inputs[index] = {&state->plane, &state->contourFinePlane,
            state->contourFineFactor, state->displayLogarithmic,
            state->cachedRequest.outputSize};
    }
    auto sync = m_displayCoordinator.syncPanelsToSharedRange(
        {m_dataset->id(), currentField, maximumLevel, composition}, inputs,
        logarithmic, isContourMode(m_displayMode), m_contourCount, m_palette);
    if (!sync) {
        return;
    }
    const auto [globalMin, globalMax] = sync->range;
    for (std::size_t index = 0; index < views.size(); ++index) {
        auto* state = views[index];
        auto& update = sync->panels[index];
        if (!update.applies) {
            continue;
        }
        state->displayMinimum = globalMin;
        state->displayMaximum = globalMax;
        if (update.contoursRecomputed) {
            state->contourPolylines = std::move(update.contourPolylines);
        }
        if (update.image.valid()) {
            state->view->setImage(displayImageFor(update.image));
        }
    }
    // setImage clears grid boxes and vector/contour overlays; restore them.
    for (auto* state : views) {
        if (state->plane.width > 0 && state->plane.height > 0) {
            updateGridBoxes(*state);
            updateOverlay(*state);
        }
    }
    if (m_activeView && m_activeView->plane.width > 0) {
        const auto fieldName = m_fieldSelector->currentText();
        const auto label = m_activeView->displayLogarithmic
            ? fieldName + tr(" (log)") : fieldName;
        m_colorBar->setLogarithmic(m_activeView->displayLogarithmic);
        m_colorBar->setFieldRange(label, globalMin, globalMax);
        if (rangeMode != RangeMode::User) {
            const QSignalBlocker minBlocker(m_rangeMinimum);
            const QSignalBlocker maxBlocker(m_rangeMaximum);
            m_rangeMinimum->setValue(globalMin);
            m_rangeMaximum->setValue(globalMax);
        }
    }
}

void MainWindow::choosePlotfileSequence()
{
    const auto settings = makeSettings();
    // Select the plotfile directories directly with click / Ctrl-click /
    // Shift-click. QFileDialog::Directory only permits selecting more than one
    // directory on the non-native dialog, so disable the native one and force
    // extended selection on every file-list view (both the icon/list view and
    // the detail/tree view). The selected directories are validated as AMReX
    // plotfiles (Header + Level_N) by openSequence.
    QFileDialog dialog(this,
        tr("Open Plotfile Sequence — select two or more plotfile directories"),
        settings.value(QStringLiteral("lastOpenDirectory")).toString());
    dialog.setFileMode(QFileDialog::Directory);
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);
    for (auto* view : dialog.findChildren<QListView*>()) {
        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    }
    for (auto* view : dialog.findChildren<QTreeView*>()) {
        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    }
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const auto selected = dialog.selectedFiles();
    if (selected.isEmpty()) {
        return;
    }
    std::vector<std::filesystem::path> frames;
    frames.reserve(static_cast<std::size_t>(selected.size()));
    for (const auto& directory : selected) {
        frames.push_back(std::filesystem::path(directory.toStdString()));
    }
    auto writableSettings = makeSettings();
    writableSettings.setValue(QStringLiteral("lastOpenDirectory"),
        QFileInfo(selected.first()).absolutePath());
    openSequence(frames);
}

void MainWindow::openSequence(const std::vector<std::filesystem::path>& frames)
{
    // Sweep and sequence playback are mutually exclusive.
    setPlaybackMode(PlaybackMode::None);
    closeSequence();
    resetRangeState();

    auto sorted = frames;
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.filename() < rhs.filename();
        });
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    const auto valid = std::all_of(sorted.begin(), sorted.end(),
        [](const auto& frame) { return isAmrexPlotfile(frame); });
    if (sorted.size() < 2 || !valid) {
        emit sequenceFrameFailed();
        QMessageBox::warning(this, tr("Cannot open sequence"),
            tr("Select two or more plotfile Header files, each inside its own "
               "plotfile directory."));
        return;
    }

    m_sequenceFrames = std::move(sorted);
    m_animationPanel->setSequenceFrameCount(
        static_cast<int>(m_sequenceFrames.size()));
    m_animationPanel->setSequenceVisible(true);
    updateAnimationDockVisibility();
    // Line plot curves are snapshots of the previous dataset; drop the window.
    auto* linePlotWindow = m_linePlotWindow;
    m_linePlotWindow = nullptr;
    if (linePlotWindow != nullptr) {
        linePlotWindow->close();
    }
    goToSequenceFrame(0);
}

void MainWindow::closeSequence()
{
    if (m_playbackMode == PlaybackMode::Sequence) {
        setPlaybackMode(PlaybackMode::None);
    }
    discardPrefetch();
    m_sequenceFrames.clear();
    m_sequenceIndex = -1;
    m_sequenceInFlight = false;
    m_animationPanel->setSequenceVisible(false);
    updateAnimationDockVisibility();
}

void MainWindow::updateAnimationDockVisibility()
{
    // The Animation panel hosts the 3-D slice-sweep controls and the
    // plotfile-sequence controls. Keep it visible only when one of those
    // applies; otherwise it is dead space.
    const auto sequenceActive = !m_sequenceFrames.empty();
    const auto threeD = m_dataset != nullptr
        && m_dataset->metadata().dimension == 3;
    m_animationDock->setVisible(sequenceActive || threeD);
}

void MainWindow::stepSequence(int direction)
{
    if (m_sequenceFrames.empty()) {
        return;
    }
    goToSequenceFrame(m_sequenceIndex + direction);
}

void MainWindow::goToSequenceFrame(int index)
{
    if (m_sequenceFrames.empty()) {
        return;
    }
    const auto count = static_cast<int>(m_sequenceFrames.size());
    // Both steps and playback wrap around the ends of the sequence.
    index = ((index % count) + count) % count;
    if (m_sequenceInFlight && index == m_sequenceIndex) {
        return;
    }
    // Cancel the current dataset's in-flight work, exactly like opening a
    // fresh dataset does, but keep the view state (field, level, range,
    // log, palette, zoom, slice positions) for the next frame.
    const std::array<PlaneViewState*, 4> states{
        &m_view2d, &m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
    for (auto* state : states) {
        state->stopSource.request_stop();
        ++state->sliceGeneration;
    }
    m_initialStopSource.request_stop();
    m_linePlotStopSource.request_stop();
    m_pendingAllViews = false;
    m_pendingViews.clear();
    m_sliceDebounce->stop();
    // The dataset window shows the previous frame's raw values; drop it.
    closeDatasetWindow();
    // Line plot curves are snapshots of this dataset; drop the window so its
    // title and curves do not go stale across the frame switch.
    auto* linePlotWindow = m_linePlotWindow;
    m_linePlotWindow = nullptr;
    if (linePlotWindow != nullptr) {
        linePlotWindow->close();
    }
    const auto generation = ++m_generation;
    m_sequenceIndex = index;
    m_sequenceInFlight = true;
    m_frameTimer.start();
    m_datasetPath = m_sequenceFrames[static_cast<std::size_t>(index)];
    m_animationPanel->setSequenceFrame(index);

    // A still-valid prefetch of this frame is consumed instead of loading
    // again; anything else in the slot is cancelled and dropped.
    if (m_prefetched && m_prefetched->frameIndex == index
        && m_prefetched->specGeneration == m_specGeneration) {
        auto prefetched = std::move(*m_prefetched);
        m_prefetched.reset();
        discardPrefetch();
        finishFrameLoad(std::move(prefetched.result), prefetched.defaultPositions);
        return;
    }
    discardPrefetch();
    startFrameLoad(index, generation);
}

void MainWindow::startFrameLoad(int index, std::uint64_t generation)
{
    auto spec = buildFrameSpec();
    const auto defaultPositions = spec.defaultPositions;
    const auto path = m_sequenceFrames[static_cast<std::size_t>(index)];
    const auto datasetId = DatasetId{
        sequenceDatasetIdBase + ++m_sequenceDatasetCounter};
    m_initialStopSource = StopSource{};
    const auto cancellation = m_initialStopSource.get_token();
    ++m_activeRequests;
    statusBar()->showMessage(tr("Loading frame %1...").arg(
        QString::fromStdString(path.filename().string())));
    updateDiagnostics();

    auto* watcher = new QFutureWatcher<InitialSliceResult>(this);
    connect(watcher, &QFutureWatcher<InitialSliceResult>::finished, this,
        [this, watcher, generation, index, defaultPositions] {
            --m_activeRequests;
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            try {
                auto result = watcher->result();
                if (generation == m_generation && index == m_sequenceIndex) {
                    finishFrameLoad(std::move(result), defaultPositions);
                } else {
                    ++m_staleResults;
                }
            } catch (const std::exception& error) {
                if (generation == m_generation && index == m_sequenceIndex) {
                    m_sequenceInFlight = false;
                    statusBar()->showMessage(tr("Frame load failed"));
                    // During animation export the failure is reported by the
                    // export handler (endExportAnimation); avoid a second dialog.
                    const bool wasExporting = m_animationExporter->active();
                    emit sequenceFrameFailed();
                    if (!wasExporting) {
                        reportBackgroundError(
                            tr("Cannot load frame: %1").arg(exceptionMessage(error)));
                    }
                } else {
                    ++m_staleResults;
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [path, datasetId, spec = std::move(spec), cancellation] {
        return executeFrameLoad(path, datasetId, spec, initialCacheBudget(),
            cancellation);
    }));
}

void MainWindow::finishFrameLoad(InitialSliceResult result, bool defaultPositions)
{
    try {
        displayFrameResult(result, defaultPositions);
    } catch (const std::exception& error) {
        m_sequenceInFlight = false;
        statusBar()->showMessage(tr("Frame load failed"));
        // During animation export the failure is reported by the export
        // handler (endExportAnimation); avoid a second dialog.
        const bool wasExporting = m_animationExporter->active();
        emit sequenceFrameFailed();
        if (!wasExporting) {
            reportBackgroundError(
                tr("Cannot load frame: %1").arg(exceptionMessage(error)));
        }
        updateDiagnostics();
        return;
    }
    m_sequenceInFlight = false;
    m_lastFrameSwitchMs = m_frameTimer.elapsed();
    m_animationPanel->setSequenceFrame(m_sequenceIndex);
    m_animationPanel->setSequenceInfo(
        QString::fromStdString(m_datasetPath.filename().string()),
        m_openMetadata->time);
    updateDiagnostics();
    emit sequenceFrameDisplayed(m_sequenceIndex);
    // Bounded low-priority prefetch of the next frame: queued behind the
    // display update, and re-validated when it runs so a frame jump in the
    // meantime does not start obsolete I/O.
    const auto displayedIndex = m_sequenceIndex;
    QTimer::singleShot(0, this, [this, displayedIndex] {
        if (m_sequenceFrames.empty() || m_sequenceInFlight
            || m_sequenceIndex != displayedIndex) {
            return;
        }
        const auto count = static_cast<int>(m_sequenceFrames.size());
        startPrefetch((displayedIndex + 1) % count);
    });
}

void MainWindow::displayFrameResult(InitialSliceResult& result,
    bool defaultPositions)
{
    m_dataset = result.dataset;
    const auto& metadata = m_dataset->metadata();
    m_viewDimension = metadata.dimension;

    // Refresh the metadata dock and the window title (frame name + time).
    PlotfileMetadataResult frameMetadata;
    frameMetadata.metadata = std::make_shared<DatasetMetadata>(metadata);
    frameMetadata.metrics = result.dataset->metadataReadMetrics();
    frameMetadata.fileVersion = !result.fileVersion.empty()
        ? result.fileVersion : m_fileVersion;
    showMetadata(frameMetadata, m_datasetPath);

    configureSequenceControls(defaultPositions);
    if (selectCacheFallbackLevel(m_levelSelector, result.cacheFallbackToLevel)) {
        configureSlicePositionControls();
        updateRangeModeAvailability();
        syncMenuChecks();
    }
    const auto views = currentViews();
    if (result.displays.size() != views.size()) {
        throw std::runtime_error("frame slice count does not match the view set");
    }
    for (std::size_t index = 0; index < views.size(); ++index) {
        showSlice(*views[index], result.displays[index]);
    }
    const auto cache = m_dataset->cacheMetrics();
    m_cacheBudgetBytes = cache.budgetBytes;
    m_cacheResidentBytes = cache.residentBytes;
    m_cachePinnedBytes = cache.pinnedBytes;
    m_cacheEvictions = cache.evictions;
    validateVectorMode();
    if (result.cacheFallbackToLevel >= 0) {
        statusBar()->showMessage(cacheFallbackMessage(
            *result.dataset, result.cacheFallbackFromLevel,
            result.cacheFallbackToLevel));
    }
}

void MainWindow::configureSequenceControls(bool defaultPositions)
{
    if (!m_dataset) {
        return;
    }
    const auto& metadata = m_dataset->metadata();
    // Preserve the user's selections across frames: the field index if it
    // still exists, the level by its combo data (falling back to finest
    // available when this frame has fewer levels).
    const auto previousField = m_controlsReady && m_fieldSelector->count() > 0
        ? m_fieldSelector->currentIndex() : 0;
    const auto previousLevel = m_controlsReady
        && m_levelSelector->currentIndex() >= 0
            ? m_levelSelector->currentData().toInt() : -1;
    {
        const QSignalBlocker fieldBlocker(m_fieldSelector);
        const QSignalBlocker levelBlocker(m_levelSelector);
        m_fieldSelector->clear();
        for (std::size_t field = 0; field < metadata.fields.size(); ++field) {
            m_fieldSelector->addItem(
                QString::fromStdString(metadata.fields[field].name),
                static_cast<unsigned int>(field));
        }
        m_fieldSelector->setCurrentIndex(
            std::clamp(previousField, 0, m_fieldSelector->count() - 1));
        m_levelSelector->clear();
        populateLevelCombo(m_levelSelector, metadata.finestLevel);
        const auto levelIndex = m_levelSelector->findData(previousLevel);
        m_levelSelector->setCurrentIndex(levelIndex >= 0 ? levelIndex : 0);
    }

    // 3-D keeps the user's slice positions (clamped into the new domain);
    // the first 3-D frame of a session starts at the domain midpoints.
    const auto isThreeDimensional = metadata.dimension == 3;
    m_syncRubberBandZoomAction->setVisible(isThreeDimensional);
    if (isThreeDimensional) {
        const auto domain = datasetSampleBounds(metadata);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            m_slicePosition3d[axis] = defaultPositions
                ? domain.lower[axis]
                    + 0.5 * (domain.upper[axis] - domain.lower[axis])
                : std::clamp(m_slicePosition3d[axis], domain.lower[axis],
                    std::nextafter(domain.upper[axis], domain.lower[axis]));
        }
        m_isoWidget->setGeometry(metadata);
        m_isoWidget->setSlicePositions(m_slicePosition3d[0], m_slicePosition3d[1],
            m_slicePosition3d[2]);
    }
    m_stack->setCurrentIndex(isThreeDimensional ? 1 : 0);
    m_animationPanel->setSweepVisible(isThreeDimensional);
    updateAnimationDockVisibility();
    configureSlicePositionControls();

    // The active view must belong to the new dimension's view set.
    const auto views = currentViews();
    if (std::find(views.begin(), views.end(), m_activeView) == views.end()) {
        setActiveView(isThreeDimensional ? m_planeViews[2] : m_view2d);
    }

    m_controlsReady = true;
    m_fieldSelector->setEnabled(true);
    m_levelSelector->setEnabled(true);
    m_rangeMode->setEnabled(true);
    m_logarithmic->setEnabled(true);
    m_boxesAction->setEnabled(true);
    m_slicePlanesAction->setEnabled(metadata.dimension == 3);
    const auto userRange = static_cast<RangeMode>(
        m_rangeMode->currentData().toInt()) == RangeMode::User;
    m_rangeMinimum->setEnabled(userRange);
    m_rangeMaximum->setEnabled(userRange);
    rebuildLevelMenu();
    m_levelMenu->setEnabled(true);
    m_contoursAction->setEnabled(true);
    m_datasetAction->setEnabled(true);
    m_exportAnimationAction->setEnabled(true);
    rebuildVariableMenu();
    ensureVectorFieldDefaults();
    updateRangeModeAvailability();
}

void MainWindow::commitFieldRange(std::uint32_t field)
{
    FieldRange range;
    range.mode = static_cast<RangeMode>(m_rangeMode->currentData().toInt());
    if (range.mode == RangeMode::User) {
        range.userRange = std::pair{m_rangeMinimum->value(), m_rangeMaximum->value()};
    }
    m_fieldRanges[field] = std::move(range);
}

void MainWindow::applyFieldRange(std::uint32_t field)
{
    const auto it = m_fieldRanges.find(field);
    const auto range = (it != m_fieldRanges.end()) ? it->second : FieldRange{};
    {
        const QSignalBlocker modeBlocker(m_rangeMode);
        const QSignalBlocker minBlocker(m_rangeMinimum);
        const QSignalBlocker maxBlocker(m_rangeMaximum);
        m_rangeMode->setCurrentIndex(
            m_rangeMode->findData(static_cast<int>(range.mode)));
        if (range.userRange.has_value()) {
            m_rangeMinimum->setValue(range.userRange->first);
            m_rangeMaximum->setValue(range.userRange->second);
        }
    }
    const auto isUser = range.mode == RangeMode::User;
    m_rangeMinimum->setEnabled(isUser && m_controlsReady);
    m_rangeMaximum->setEnabled(isUser && m_controlsReady);
}

void MainWindow::resetRangeState()
{
    m_fieldRanges.clear();
    m_trackedField = 0;
    m_displayCoordinator.invalidateRangeCache();
    const QSignalBlocker modeBlocker(m_rangeMode);
    const QSignalBlocker minBlocker(m_rangeMinimum);
    const QSignalBlocker maxBlocker(m_rangeMaximum);
    m_rangeMode->setCurrentIndex(
        m_rangeMode->findData(static_cast<int>(RangeMode::File)));
    m_rangeMinimum->setValue(0.0);
    m_rangeMaximum->setValue(1.0);
    m_rangeMinimum->setEnabled(false);
    m_rangeMaximum->setEnabled(false);
}

void MainWindow::updateRangeModeAvailability()
{
    if (!m_dataset || m_fieldSelector->currentIndex() < 0
        || m_levelSelector->currentIndex() < 0) {
        return;
    }

    const auto& metadata = m_dataset->metadata();
    const FieldId field{m_fieldSelector->currentData().toUInt()};
    const auto [composition, maximumLevel] = decodeLevelData(
        m_levelSelector->currentData().toInt(), metadata.finestLevel);
    const auto fileAvailable = metadata.isFab
        || selectedMetadataRange(metadata, field,
            maximumLevel, composition, RangeMode::File).has_value();
    const auto levelAvailable = selectedMetadataRange(metadata, field,
        maximumLevel, composition, RangeMode::Level).has_value();

    auto* model = qobject_cast<QStandardItemModel*>(m_rangeMode->model());
    if (model == nullptr) {
        return;
    }
    const auto unavailableText = tr(
        "Unavailable because this data does not provide complete range statistics.");
    const auto setAvailable = [&](RangeMode mode, bool available) {
        const auto index = m_rangeMode->findData(static_cast<int>(mode));
        if (index < 0) {
            return;
        }
        if (auto* item = model->item(index)) {
            item->setEnabled(available);
            item->setToolTip(available ? QString() : unavailableText);
        }
    };
    setAvailable(RangeMode::File, fileAvailable);
    setAvailable(RangeMode::Level, levelAvailable);

    const auto current = static_cast<RangeMode>(
        m_rangeMode->currentData().toInt());
    const auto currentAvailable =
        (current != RangeMode::File || fileAvailable)
        && (current != RangeMode::Level || levelAvailable);
    if (currentAvailable) {
        return;
    }

    {
        const QSignalBlocker blocker(m_rangeMode);
        m_rangeMode->setCurrentIndex(
            m_rangeMode->findData(static_cast<int>(RangeMode::Visible)));
    }
    m_rangeMinimum->setEnabled(false);
    m_rangeMaximum->setEnabled(false);
    auto& fieldRange = m_fieldRanges[field.value];
    fieldRange.mode = RangeMode::Visible;
    statusBar()->showMessage(
        tr("Metadata range unavailable; using the visible-data range."));
}

FrameSliceSpec MainWindow::buildFrameSpec()
{
    FrameSliceSpec spec;
    spec.displayMode = m_displayMode;
    spec.palette = m_palette;
    spec.contourCount = m_contourCount;
    spec.logarithmic = m_logarithmic->isChecked();
    spec.rangeMode = static_cast<RangeMode>(m_rangeMode->currentData().toInt());
    if (spec.rangeMode == RangeMode::User) {
        spec.userRange = std::pair{m_rangeMinimum->value(),
            m_rangeMaximum->value()};
    }
    spec.field = m_controlsReady && m_fieldSelector->currentIndex() >= 0
        ? m_fieldSelector->currentData().toUInt() : 0U;
    spec.levelSelection = m_controlsReady && m_levelSelector->currentIndex() >= 0
        ? m_levelSelector->currentData().toInt() : -1;
    spec.vectorUField = static_cast<std::uint32_t>(std::max(m_vectorUField, 0));
    spec.vectorVField = static_cast<std::uint32_t>(std::max(m_vectorVField, 0));
    spec.vectorWField = static_cast<std::uint32_t>(std::max(m_vectorWField, 0));
    // Slice positions only carry over between 3-D frames; anything else
    // starts the new dataset at its domain midpoints.
    spec.defaultPositions = m_viewDimension != 3;
    spec.slicePositions = m_slicePosition3d;
    const auto views = currentViews();
    spec.visibleRegions.reserve(views.size());
    for (const auto* state : views) {
        spec.visibleRegions.push_back(state->visibleRegion);
    }
    return spec;
}

void MainWindow::startPrefetch(int frameIndex)
{
    // Single bounded slot: cancel and drop whatever prefetch came before.
    discardPrefetch();
    auto spec = buildFrameSpec();
    const auto defaultPositions = spec.defaultPositions;
    const auto specGeneration = m_specGeneration;
    const auto generation = m_prefetchGeneration;
    const auto path = m_sequenceFrames[static_cast<std::size_t>(frameIndex)];
    const auto datasetId = DatasetId{
        sequenceDatasetIdBase + ++m_sequenceDatasetCounter};
    m_prefetchStopSource = StopSource{};
    const auto cancellation = m_prefetchStopSource.get_token();
    ++m_activeRequests;
    updateDiagnostics();

    auto* watcher = new QFutureWatcher<InitialSliceResult>(this);
    connect(watcher, &QFutureWatcher<InitialSliceResult>::finished, this,
        [this, watcher, generation, frameIndex, specGeneration,
            defaultPositions] {
            --m_activeRequests;
            try {
                auto result = watcher->result();
                if (generation == m_prefetchGeneration
                    && !m_sequenceFrames.empty()) {
                    m_prefetched = PrefetchedFrame{frameIndex, specGeneration,
                        defaultPositions, std::move(result)};
                } else {
                    ++m_staleResults;
                }
            } catch (const std::exception&) {
                // Prefetch failures stay silent: reaching the frame loads it
                // through the normal path and reports any error then.
                if (generation != m_prefetchGeneration) {
                    ++m_staleResults;
                }
            }
            updateDiagnostics();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [path, datasetId, spec = std::move(spec), cancellation] {
        return executeFrameLoad(path, datasetId, spec, initialCacheBudget(),
            cancellation);
    }));
}

void MainWindow::discardPrefetch()
{
    m_prefetchStopSource.request_stop();
    ++m_prefetchGeneration;
    m_prefetched.reset();
}

void MainWindow::stepSweep(int direction)
{
    if (!m_dataset || m_dataset->metadata().dimension != 3) {
        return;
    }
    const auto axis = m_animationPanel->sweepAxis();
    const auto index = static_cast<std::size_t>(axis);
    const auto& metadata = m_dataset->metadata();
    const auto& level = metadata.levels.back();
    auto sample = sampleIndex(level, axis, m_slicePosition3d[index]) + direction;
    if (sample > level.domain.upper[index]) {
        sample = level.domain.lower[index];
    } else if (sample < level.domain.lower[index]) {
        sample = level.domain.upper[index];
    }
    setSlicePosition(axis, samplePosition(level, axis, sample));
}

void MainWindow::toggleSweepPlayback()
{
    if (m_playbackMode == PlaybackMode::Sweep) {
        setPlaybackMode(PlaybackMode::None);
        return;
    }
    if (!m_dataset || m_dataset->metadata().dimension != 3) {
        return;
    }
    setPlaybackMode(PlaybackMode::Sweep);
}

void MainWindow::toggleSequencePlayback()
{
    if (m_playbackMode == PlaybackMode::Sequence) {
        setPlaybackMode(PlaybackMode::None);
        return;
    }
    if (m_sequenceFrames.size() < 2) {
        return;
    }
    setPlaybackMode(PlaybackMode::Sequence);
}

void MainWindow::setPlaybackMode(PlaybackMode mode)
{
    m_playbackMode = mode;
    m_animationPanel->setSweepPlaying(mode == PlaybackMode::Sweep);
    m_animationPanel->setSequencePlaying(mode == PlaybackMode::Sequence);
    if (mode == PlaybackMode::None) {
        m_playbackTimer->stop();
    } else {
        m_playbackTimer->start(m_animationPanel->frameDelayMs());
    }
}

void MainWindow::playbackTick()
{
    if (m_playbackMode == PlaybackMode::Sweep) {
        if (!m_dataset || m_dataset->metadata().dimension != 3) {
            setPlaybackMode(PlaybackMode::None);
            return;
        }
        // Skip the tick while the previous slice is still on a worker, so a
        // fast Speed setting cannot pile up requests.
        const auto axis = m_animationPanel->sweepAxis();
        if (m_planeViews[static_cast<std::size_t>(axis)].pendingRequests > 0) {
            return;
        }
        stepSweep(1);
        // Bypass the debounce so each tick issues its slice immediately; the
        // in-flight check above is the throttle.
        flushSliceRequests();
        return;
    }
    if (m_playbackMode == PlaybackMode::Sequence) {
        if (m_sequenceFrames.size() < 2) {
            setPlaybackMode(PlaybackMode::None);
            return;
        }
        // Skip the tick while the previous frame is still loading.
        if (m_sequenceInFlight) {
            return;
        }
        goToSequenceFrame(m_sequenceIndex + 1);
    }
}

void MainWindow::applySpeed()
{
    m_playbackTimer->setInterval(m_animationPanel->frameDelayMs());
}

void MainWindow::reportBackgroundError(const QString& message)
{
    // Non-modal: background-operation failures append to the Diagnostics dock
    // and set a status-bar message instead of a modal dialog that disables the
    // window. Suppressed while closing (stage 1 also guards the handlers).
    if (m_closing) {
        return;
    }
    qWarning("%s", message.toUtf8().constData());
    m_backgroundErrors.append(message);
    constexpr int maximumErrors = 50;
    while (m_backgroundErrors.size() > maximumErrors) {
        m_backgroundErrors.removeFirst();
    }
    statusBar()->showMessage(message.section(QLatin1Char('\n'), 0, 0));
    m_diagnosticsDock->setVisible(true);
    updateDiagnostics();
}

void MainWindow::updateDiagnostics()
{
    auto text = tr("generation: %1\nactive background requests: %2\n"
           "stale results discarded: %3\nmetadata files read: %4\n"
           "metadata bytes read: %5\nblocks read: %6\ncache hits: %7\n"
           "payload bytes read: %8\ncache budget bytes: %9\n"
           "cache resident bytes: %10\ncache pinned bytes: %11\n"
           "cache evictions: %12\nlast frame switch: %13 ms")
            .arg(m_generation)
            .arg(m_activeRequests)
            .arg(m_staleResults)
            .arg(m_lastFilesRead)
            .arg(m_lastBytesRead)
            .arg(m_lastBlocksRead)
            .arg(m_lastCacheHits)
            .arg(m_lastPayloadBytesRead)
            .arg(m_cacheBudgetBytes)
            .arg(m_cacheResidentBytes)
            .arg(m_cachePinnedBytes)
            .arg(m_cacheEvictions)
            .arg(m_lastFrameSwitchMs);
    for (const auto& line : m_probeLines) {
        text += QLatin1Char('\n');
        text += line;
    }
    for (const auto& error : m_backgroundErrors) {
        text += QLatin1Char('\n');
        text += tr("background error: %1").arg(error);
    }
    m_diagnostics->setPlainText(text);
}

} // namespace amrvis::qt
