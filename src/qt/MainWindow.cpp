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
namespace {

constexpr std::array<BuiltinPalette, 7> builtinPalettes{
    BuiltinPalette::Rainbow, BuiltinPalette::Turbo, BuiltinPalette::Viridis,
    BuiltinPalette::Plasma, BuiltinPalette::Parula, BuiltinPalette::Coolwarm,
    BuiltinPalette::Blackbody};
// Menu labels and QSettings keys; kept in sync with builtinPaletteName().
constexpr std::array<const char*, 7> builtinPaletteNames{
    "rainbow", "turbo", "viridis", "plasma", "parula", "coolwarm", "blackbody"};

constexpr std::array<Qt::GlobalColor, 7> particleDefaultColors{
    Qt::white, Qt::yellow, Qt::cyan, Qt::magenta,
    Qt::green, Qt::red, Qt::lightGray};

QColor defaultParticleColor(std::size_t speciesIndex)
{
    return QColor(
        particleDefaultColors[speciesIndex % particleDefaultColors.size()]);
}

void updateColorButton(QPushButton& button, const QColor& color)
{
    QPixmap swatch(18, 18);
    swatch.fill(color);
    button.setIcon(QIcon(swatch));
    button.setText(color.name(QColor::HexRgb).toUpper());
}

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

// Recovers a remote error code from a worker exception, unwrapping the
// QUnhandledException that Qt Concurrent wraps around a thrown std exception.
// Returns nullopt for local failures and any non-remote error.
std::optional<remote::ErrorCode> remoteErrorCode(const std::exception& error)
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
QString cacheBudgetDescription(std::uint64_t bytes)
{
    return QString::fromStdString(amrvis::cacheBudgetDescription(bytes));
}

QString cacheFallbackMessage(
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
    // Separator between the Position group and Scale. It tracks the Position
    // group's visibility (see setSlicePositionControlsVisible) so it does not
    // dangle beside the Level separator when no dataset is loaded.
    m_positionSeparator = sliceToolbar->addSeparator();
    setSlicePositionControlsVisible(false);

    // A static "Scale:" label plus a state button, matching the Field:/Level:/
    // Range: label-and-widget pairs elsewhere on this toolbar (and the
    // View -> Scale menu name).
    sliceToolbar->addWidget(new QLabel(tr("Scale:"), sliceToolbar));
    m_scaleButton = new QPushButton(tr("Fit"), sliceToolbar);
    m_scaleButton->setToolTip(
        tr("Zoom scale and rubber-band synchronization for panels"));
    m_scaleButton->setFocusPolicy(Qt::NoFocus);
    auto* scaleMenu = new QMenu(m_scaleButton);
    // The clicked item stays "Reset Zoom" (the action verb): it restores the
    // whole domain and refits (issue #45 renamed it from "Fit", which read as
    // fit-the-current-region). The button shows just the *scale state* ("Fit"
    // for auto-fit, which also holds for a panned crop in applyPanStep where
    // the region is not the whole domain); the adjacent "Scale:" label names
    // the control.
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
            applyFixedScale(factor);
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
    // Separate the Range group (mode + min/max) from Log and Palette, matching
    // the per-group separators on the Slice Controls toolbar.
    rangeToolbar->addSeparator();
    m_logarithmic = new QCheckBox(tr("Log"), rangeToolbar);
    rangeToolbar->addWidget(m_logarithmic);
    rangeToolbar->addSeparator();
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
        // Reserve room for the reversed form ("Plasma_r") so the closed
        // selector never elides the "_r" suffix syncPaletteSelector appends.
        widestBuiltin = std::max(widestBuiltin,
            paletteFm.horizontalAdvance(label + QStringLiteral("_r")));
        m_paletteSelector->addItem(label, static_cast<int>(index));
    }
    // A toggle that reverses the selected palette (data -3), kept at the end of
    // the popup. Its label reflects the state (see syncPaletteSelector); it is
    // never the closed selection, so it does not affect the fixed width below.
    m_paletteSelector->insertSeparator(m_paletteSelector->count());
    m_paletteSelector->addItem(tr("Reverse Colormap"), -3);
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
            } else if (selection == -3) {
                // The "Reverse Colormap" toggle: flip the state, then
                // syncPaletteSelector restores the current index to the palette.
                setReversePalette(!m_reversePalette);
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
            // A deferred full-domain range store (m_pendingRangeStore) is keyed
            // to the field/level/mode in effect when it was queued. Changing any
            // of them means a completing 3-D Visible sync would store its union
            // under the wrong key, so drop the pending store here (the sync
            // completion also re-checks the key; see range-cache-staleness-races).
            m_pendingRangeStore.reset();
            updateRangeModeAvailability();
            scheduleSliceRequest();
        });
    connect(m_levelSelector, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) {
            configureSlicePositionControls();
            m_pendingRangeStore.reset();  // see the field selector above
            updateRangeModeAvailability();
            scheduleSliceRequest();
        });
    connect(m_rangeMode, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) {
            m_pendingRangeStore.reset();  // see the field selector above
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
    // The sequence controller owns the frame/prefetch state machine; this
    // window supplies the GUI-coupled hooks (spec snapshot, frame display,
    // shutdown flag) and reacts to its signals below.
    m_sequenceController = new SequenceController(
        SequenceController::Hooks{
            [this] { return buildFrameSpec(); },
            [this](InitialSliceResult& result, bool defaultPositions) {
                displayFrameResult(result, defaultPositions);
            },
            [this] { return m_closing; },
        },
        this);
    connect(m_sequenceController, &SequenceController::frameSwitchStarted,
        this, [this](int index) {
            // Cancel the current dataset's in-flight work, exactly like
            // opening a fresh dataset does, but keep the view state (field,
            // level, range, log, palette, zoom, slice positions) for the
            // next frame.
            const std::array<PlaneViewState*, 4> states{&m_view2d,
                &m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
            for (auto* state : states) {
                state->stopSource.request_stop();
                ++state->sliceGeneration;
            }
            m_initialStopSource.request_stop();
            m_linePlotStopSource.request_stop();
            m_particleStopSource.request_stop();
            m_particleLoading = false;
            m_particleProgress->setVisible(false);
            m_pendingAllViews = false;
            m_pendingViews.clear();
            m_sliceDebounce->stop();
            // The dataset window shows the previous frame's raw values;
            // drop it, and the line plot window whose curves are snapshots
            // of this dataset, so neither goes stale across the switch.
            closeDatasetWindow();
            auto* linePlotWindow = m_linePlotWindow;
            m_linePlotWindow = nullptr;
            if (linePlotWindow != nullptr) {
                linePlotWindow->close();
            }
            ++m_generation;
            m_datasetPath = m_sequenceController->framePath(index);
            m_animationPanel->setSequenceFrame(index);
        });
    connect(m_sequenceController, &SequenceController::loadActivityChanged,
        this, [this](int delta) {
            if (delta > 0) {
                m_activeRequests += static_cast<std::uint64_t>(delta);
            } else {
                m_activeRequests -= static_cast<std::uint64_t>(-delta);
            }
            updateDiagnostics();
        });
    connect(m_sequenceController, &SequenceController::staleResultDropped,
        this, [this] {
            ++m_staleResults;
            updateDiagnostics();
        });
    connect(m_sequenceController, &SequenceController::statusMessage,
        this, [this](const QString& message) {
            statusBar()->showMessage(message);
        });
    connect(m_sequenceController, &SequenceController::frameDisplayed,
        this, [this](int index) {
            m_animationPanel->setSequenceFrame(index);
            m_animationPanel->setSequenceInfo(
                QString::fromStdString(m_datasetPath.filename().string()),
                m_openMetadata->time);
            updateDiagnostics();
            emit sequenceFrameDisplayed(index);
        });
    connect(m_sequenceController, &SequenceController::frameLoadFailed,
        this, [this](const QString& message) {
            statusBar()->showMessage(tr("Frame load failed"));
            // Stop playing. Playback wraps, so without this it comes back to
            // the same unreadable frame -- or the same disconnected server --
            // every cycle, raising a diagnostic each time. The user is left on
            // the failed frame and can step or play again once they have dealt
            // with it. A sweep is left alone; only sequence playback can hit a
            // frame load.
            if (m_playbackMode == PlaybackMode::Sequence) {
                setPlaybackMode(PlaybackMode::None);
            }
            // During animation export the failure is reported by the export
            // handler; avoid a second dialog.
            const bool wasExporting = m_animationExporter->active();
            emit sequenceFrameFailed();
            if (!wasExporting) {
                reportBackgroundError(
                    tr("Cannot load frame: %1").arg(message));
            }
            updateDiagnostics();
        });

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
            if (!m_closing && m_sequenceController->hasSequence()) {
                goToSequenceFrame(restoreIndex < 0 ? 0 : restoreIndex);
            }
            m_exportAnimationAction->setEnabled(
                m_sequenceController->hasSequence());
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

    m_probeLabel = new QLabel(statusBar());
    m_particleProgress = new QProgressBar(statusBar());
    m_particleProgress->setRange(0, 0);
    m_particleProgress->setFormat(tr("Loading particles..."));
    m_particleProgress->setAccessibleName(tr("Loading particle sample"));
    m_particleProgress->setFixedWidth(170);
    m_particleProgress->setVisible(false);
    statusBar()->addPermanentWidget(m_particleProgress);
    statusBar()->addPermanentWidget(m_probeLabel);
    statusBar()->showMessage(tr("No dataset open"));
    updateDiagnostics();
    restoreSettings();
    // Cancel in-flight async work on any quit path (last-window close, Cmd-Q,
    // menu Quit) so QThreadPool teardown does not block on an outstanding read
    // and the process can exit promptly. Only here -- where every window is
    // going away -- is it safe to also drop the shared global pool's queued
    // jobs, so teardown skips starting work that would only observe its stop
    // token and exit; a per-window close must not (see cancelInFlight and
    // window-close-clears-shared-thread-pool).
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        cancelInFlight();
        if (auto* pool = QThreadPool::globalInstance()) {
            pool->clear();
        }
    });
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
    connect(view, &ImageView::viewportResized, this,
        [this, &state](const QSize&) {
            if (!m_dataset || !std::dynamic_pointer_cast<
                    remote::RemoteDatasetSession>(m_dataset)) {
                return;
            }
            if (remoteDemandCanvas(state)) {
                updateRemoteFixedScaleDemand(state);
                return;
            }
            if (!state.hasCachedRequest
                || state.cachedRequest.outputSize != sliceOutputSize(state)) {
                scheduleSliceRequest(state);
            }
        });
    connect(view, &ImageView::canvasScrolled, this,
        [this, &state] { updateRemoteFixedScaleDemand(state); });
    connect(view, &ImageView::panStepRequested, this,
        [this, &state](const QPointF& direction) {
            applyPanStep(state, direction);
        });
}

std::array<MainWindow::PlaneViewState*, 4> MainWindow::allViewStates()
{
    return {&m_view2d, &m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
}

void MainWindow::setAllViewPlaceholders(const QString& text)
{
    for (auto* state : allViewStates()) {
        state->view->setPlaceholder(text);
    }
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
    if (state.plane->width <= 0 || state.plane->height <= 0) {
        return;
    }
    syncActiveViewColorControls(state);
}

void MainWindow::syncActiveViewColorControls(const PlaneViewState& state)
{
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

std::array<int, 2> MainWindow::nativeOutputSize(
    const PlaneViewState& state) const
{
    if (!m_openMetadata || m_openMetadata->levels.empty()) {
        return {1, 1};
    }
    const auto target = state.visibleRegion.value_or(
        datasetSampleBounds(*m_openMetadata));
    return finestNativeOutputSize(
        *m_openMetadata, target, state.normal);
}

std::array<int, 2> MainWindow::sliceOutputSize(
    const PlaneViewState& state, bool forceRemote) const
{
    if (!forceRemote
        && !std::dynamic_pointer_cast<remote::RemoteDatasetSession>(m_dataset)) {
        return nativeOutputSize(state);
    }
    if (!m_openMetadata || m_openMetadata->levels.empty()) {
        return {1, 1};
    }
    const auto viewportPixels = viewportPixelSize(state);
    const auto target = state.visibleRegion.value_or(
        datasetSampleBounds(*m_openMetadata));
    std::array<int, 2> outputSize{};
    if (state.view->transformMode() == ImageView::TransformMode::FixedScale) {
        outputSize = finestNativeOutputSize(
            *m_openMetadata, target, state.normal);
    } else if (!state.visibleRegion.has_value()) {
        outputSize = nativeBoundedViewportOutputSize(
            *m_openMetadata, target, state.normal, viewportPixels);
    } else {
        // Rubber-band selections intentionally retain their exact physical
        // aspect. Their fractional edges need not have the rounded native-cell
        // aspect used to suppress Fit supersampling on a whole-domain view.
        outputSize = viewportBoundedOutputSize(
            *m_openMetadata, target, state.normal, viewportPixels);
    }
    return frameBudgetBoundedOutputSize(
        outputSize,
        m_dataset ? m_dataset->maximumResponseBytes() : std::nullopt);
}

std::array<int, 2> MainWindow::viewportPixelSize(
    const PlaneViewState& state) const
{
    const auto* viewport = state.view == nullptr ? nullptr : state.view->viewport();
    if (viewport == nullptr || viewport->width() < 1 || viewport->height() < 1) {
        return {1, 1};
    }
    const auto scale = state.view->devicePixelRatioF();
    return {
        std::clamp(static_cast<int>(std::lround(viewport->width() * scale)),
            1, maxSliceOutputDimension),
        std::clamp(static_cast<int>(std::lround(viewport->height() * scale)),
            1, maxSliceOutputDimension)};
}

QSize MainWindow::logicalImageSize(const PlaneViewState& state,
    const ScalarPlane& plane, const QImage& image) const
{
    if (!m_openMetadata || m_openMetadata->levels.empty()
        || displayIsSpherical()) {
        return image.size();
    }
    const auto native = finestNativeOutputSize(
        *m_openMetadata, plane.physicalRegion, state.normal);
    return {native[0], native[1]};
}

bool MainWindow::displayIsSpherical() const
{
    return m_dataset && isSpherical2D(m_dataset->metadata());
}

bool MainWindow::displayIsSphericalWarp() const
{
    return displayIsSpherical() && m_sphericalDisplay == SphericalDisplay::RZ;
}

void MainWindow::updateSphericalControls()
{
    const bool spherical = displayIsSpherical();
    if (m_sphericalMenu != nullptr) {
        m_sphericalMenu->setEnabled(spherical);
    }
    if (m_sphericalSupersampleMenu != nullptr) {
        // Supersampling only affects the R-Z warp.
        m_sphericalSupersampleMenu->setEnabled(
            spherical && m_sphericalDisplay == SphericalDisplay::RZ);
    }
}

std::array<QString, 2> MainWindow::sphericalAxisLabels(SphericalDisplay mode)
{
    const QString theta(QChar(0x03B8));
    switch (mode) {
    case SphericalDisplay::RTheta:
        return {QStringLiteral("r"), theta};
    case SphericalDisplay::ThetaR:
        return {theta, QStringLiteral("r")};
    case SphericalDisplay::RZ:
    default:
        return {QStringLiteral("R"), QStringLiteral("Z")};
    }
}

PlaneMapping MainWindow::planeMapping(const PlaneViewState& state) const
{
    PlaneMapping mapping;
    mapping.spherical = displayIsSpherical();
    mapping.mode = state.sphericalDisplay;
    mapping.logicalRegion = state.plane->physicalRegion;
    mapping.displayRegion = state.displayRegion;
    mapping.sceneWidth = std::max(1, state.view->image().width());
    mapping.sceneHeight = std::max(1, state.view->image().height());
    mapping.planeWidth = std::max(1, state.plane->width);
    mapping.planeHeight = std::max(1, state.plane->height);
    return mapping;
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

    const auto configureRemoteEndpoint = [this]() {
        // Prefill the loopback host and the last port used (this session, or
        // the one saved from a previous run against a still-running server).
        // The token is never persisted, so it is always entered fresh.
        QString current;
        if (m_remotePort != 0) {
            current = QStringLiteral("%1:%2")
                          .arg(QString::fromStdString(m_remoteHost))
                          .arg(m_remotePort);
        } else {
            const auto savedPort = makeSettings()
                    .value(QStringLiteral("remote/port"), 0U)
                    .toUInt();
            current = (savedPort > 0 && savedPort <= 65535)
                ? QStringLiteral("127.0.0.1:%1").arg(savedPort)
                : QStringLiteral("127.0.0.1:");
        }
        bool accepted = false;
        const auto text = QInputDialog::getText(this,
            tr("Connect to Remote Server"), tr("Host and port:"),
            QLineEdit::Normal, current, &accepted);
        if (!accepted) {
            return false;
        }
        const auto endpoint = parseRemoteEndpoint(text.toStdString());
        if (!endpoint) {
            QMessageBox::warning(this, tr("Invalid endpoint"),
                tr("Enter a numeric endpoint as IPv4:PORT or [IPv6]:PORT."));
            return false;
        }
        auto token = endpoint->token;
        if (token.empty()) {
            bool tokenAccepted = false;
            const auto tokenText = QInputDialog::getText(this,
                tr("Connect to Remote Server"),
                tr("Session token (printed by the server at startup):"),
                QLineEdit::Password, QString(), &tokenAccepted);
            if (!tokenAccepted) {
                return false;
            }
            token = tokenText.trimmed().toStdString();
        }
        if (token.empty()) {
            QMessageBox::warning(this, tr("Missing token"),
                tr("A session token is required to connect."));
            return false;
        }
        m_remoteHost = endpoint->host;
        m_remotePort = endpoint->port;
        m_remoteToken = std::move(token);
        saveRemoteSettings();
        statusBar()->showMessage(tr("Remote endpoint set to %1:%2")
                .arg(QString::fromStdString(m_remoteHost))
                .arg(m_remotePort));
        updateDiagnostics();
        return true;
    };
    auto* connectRemoteAction = new QAction(
        tr("&Connect to Remote Server..."), this);
    connect(connectRemoteAction, &QAction::triggered, this,
        [this, configureRemoteEndpoint] {
            if (configureRemoteEndpoint()) {
                verifyRemoteEndpoint(
                    m_remoteHost, m_remotePort, m_remoteToken);
            }
        });

    auto* openRemoteAction = new QAction(
        tr("Open Remote &Plotfile..."), this);
    connect(openRemoteAction, &QAction::triggered, this,
        [this, configureRemoteEndpoint] {
            if (m_remotePort == 0 && !configureRemoteEndpoint()) {
                return;
            }
            bool accepted = false;
            const auto path = QInputDialog::getText(this,
                tr("Open Remote Plotfile"),
                tr("Server-visible plotfile path:"), QLineEdit::Normal,
                QString(), &accepted);
            if (accepted && !path.trimmed().isEmpty()) {
                openRemoteDataset(m_remoteHost, m_remotePort,
                    path.toStdString(), m_remoteToken);
            }
        });

    auto* openRemoteSequenceAction = new QAction(
        tr("Open &Remote Plotfile Sequence..."), this);
    connect(openRemoteSequenceAction, &QAction::triggered, this,
        [this, configureRemoteEndpoint] {
            if (m_remotePort == 0 && !configureRemoteEndpoint()) {
                return;
            }
            bool accepted = false;
            const auto text = QInputDialog::getMultiLineText(this,
                tr("Open Remote Plotfile Sequence"),
                tr("Server-visible paths, one per line, in playback order:"),
                QString(), &accepted);
            if (!accepted) {
                return;
            }
            std::vector<std::string> paths;
            for (const auto& line : text.split(
                     QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                const auto path = line.trimmed();
                if (!path.isEmpty()) {
                    paths.push_back(path.toStdString());
                }
            }
            openRemoteSequence(
                m_remoteHost, m_remotePort, paths, m_remoteToken);
        });

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
    // Reverses the selected palette's color ramp (the "_r" variant, e.g.
    // plasma_r), applied on top of whichever builtin or file palette is active.
    m_reversePaletteAction = paletteMenu->addAction(tr("&Reverse Colormap"));
    m_reversePaletteAction->setCheckable(true);
    m_reversePaletteAction->setChecked(m_reversePalette);
    connect(m_reversePaletteAction, &QAction::toggled, this,
        [this](bool on) { setReversePalette(on); });
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
    fileMenu->addSeparator();
    fileMenu->addAction(connectRemoteAction);
    fileMenu->addAction(openRemoteAction);
    fileMenu->addAction(openRemoteSequenceAction);
    fileMenu->addSeparator();
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
            applyFixedScale(factor);
        });
        scaleMenu->addAction(action);
    }
    scaleMenu->addSeparator();
    scaleMenu->addAction(m_syncRubberBandZoomAction);

    // "2-D Spherical" groups the options specific to warped spherical
    // (r, theta) -> (R, Z) display; the whole submenu is enabled only while
    // such a dataset is shown (see showSlice). More options will be added here.
    m_sphericalMenu = new QMenu(tr("2-D Spherical"), this);
    m_sphericalMenu->setEnabled(false);

    // Display layout: the physical R-Z warp, or the logical r-theta / theta-r
    // (transposed) grid. Only R-Z uses the supersample control below.
    m_sphericalDisplayGroup = new QActionGroup(this);
    m_sphericalDisplayMenu = new QMenu(tr("&Display"), this);
    const QString theta(QChar(0x03B8));
    const std::array<SphericalDisplay, 3> displayModes{
        SphericalDisplay::RZ, SphericalDisplay::RTheta, SphericalDisplay::ThetaR};
    const std::array<QString, 3> displayLabels{
        tr("R-Z (physical)"), QStringLiteral("r-") + theta,
        theta + QStringLiteral("-r")};
    for (std::size_t index = 0; index < displayModes.size(); ++index) {
        const auto displayMode = displayModes[index];
        auto* action = new QAction(displayLabels[index], m_sphericalDisplayMenu);
        action->setCheckable(true);
        action->setActionGroup(m_sphericalDisplayGroup);
        action->setData(static_cast<int>(displayMode));
        action->setChecked(displayMode == m_sphericalDisplay);
        connect(action, &QAction::triggered, this, [this, displayMode] {
            if (displayMode == m_sphericalDisplay) {
                return;
            }
            m_sphericalDisplay = displayMode;
            saveSettings();
            updateSphericalControls();
            // Display-only change: re-render from the cached planes, no query.
            if (displayIsSpherical()) {
                scheduleSliceRequest(true);
            }
        });
        m_sphericalDisplayMenu->addAction(action);
    }
    m_sphericalMenu->addMenu(m_sphericalDisplayMenu);

    // Warp resolution: higher factors trace the curved cell boundaries more
    // smoothly at the cost of a larger warped raster.
    m_sphericalSupersampleGroup = new QActionGroup(this);
    m_sphericalSupersampleMenu = new QMenu(tr("&Supersampling"), this);
    constexpr std::array<int, 5> supersampleFactors{1, 2, 4, 8, 16};
    for (const auto factor : supersampleFactors) {
        auto* action = new QAction(
            tr("%1x").arg(factor), m_sphericalSupersampleMenu);
        action->setCheckable(true);
        action->setActionGroup(m_sphericalSupersampleGroup);
        action->setData(factor);
        action->setChecked(factor == m_sphericalSupersample);
        connect(action, &QAction::triggered, this, [this, factor] {
            if (factor == m_sphericalSupersample) {
                return;
            }
            m_sphericalSupersample = factor;
            saveSettings();
            // Display-only change: re-warp the cached planes, no new query.
            if (displayIsSpherical()) {
                scheduleSliceRequest(true);
            }
        });
        m_sphericalSupersampleMenu->addAction(action);
    }
    m_sphericalMenu->addMenu(m_sphericalSupersampleMenu);

    m_levelMenu = new QMenu(tr("&Level"), this);
    m_levelGroup = new QActionGroup(this);
    m_levelMenu->setEnabled(false);

    m_boxesAction = new QAction(tr("&Boxes"), this);
    m_boxesAction->setCheckable(true);
    m_boxesAction->setShortcuts(
        {QKeySequence(Qt::Key_B), QKeySequence(Qt::SHIFT | Qt::Key_B)});
    m_boxesAction->setEnabled(false);
    connect(m_boxesAction, &QAction::toggled, this, [this](bool visible) {
        if (visible) {
            for (auto* state : currentViews()) {
                state->gridBoxes.clear();
                // The displayed raster remains valid, but the next request
                // must fetch geometry even if the last one also had boxes.
                if (state->hasCachedRequest) {
                    state->cachedRequest.includeGridBoxes = false;
                }
            }
        }
        updateGridBoxes();
        if (visible && m_controlsReady) {
            scheduleSliceRequest(false);
        }
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

    m_particlesAction = new QAction(tr("&Particles..."), this);
    m_particlesAction->setEnabled(false);
    connect(m_particlesAction, &QAction::triggered,
        this, [this] { showParticlesDialog(); });

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
    viewMenu->addMenu(m_sphericalMenu);
    viewMenu->addSeparator();
    viewMenu->addAction(m_contoursAction);
    viewMenu->addAction(m_particlesAction);
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

    const auto builtinLabel = [](int index) {
        const auto raw =
            builtinPaletteName(builtinPalettes[static_cast<std::size_t>(index)]);
        auto label = QString::fromLatin1(
            raw.data(), static_cast<qsizetype>(raw.size()));
        if (!label.isEmpty()) {
            label[0] = label[0].toUpper();
        }
        return label;
    };
    // Reversal is a global modifier, so every palette name carries the "_r"
    // suffix (the plasma_r convention) while it is on -- including the closed
    // selector, which shows the active one.
    const QString suffix = m_reversePalette ? QStringLiteral("_r") : QString();
    for (int item = 0; item < m_paletteSelector->count(); ++item) {
        const auto entryData = m_paletteSelector->itemData(item);
        if (!entryData.isValid()) {
            continue;  // separator
        }
        const auto value = entryData.toInt();
        if (value >= 0) {
            m_paletteSelector->setItemText(item, builtinLabel(value) + suffix);
        } else if (value == -3) {
            // The toggle item shows a check mark while reversal is on.
            m_paletteSelector->setItemText(item,
                (m_reversePalette ? QStringLiteral("✓ ") : QString())
                    + tr("Reverse Colormap"));
        }
    }

    if (m_paletteFromFile) {
        const auto label =
            tr("Custom: %1").arg(QFileInfo(m_paletteFilePath).fileName()) + suffix;
        // Insert just after the builtins (and before the separator) so the
        // reverse toggle stays anchored at the bottom.
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
    m_basePalette = palette;
    m_paletteFromFile = !builtinIndex.has_value();
    if (builtinIndex.has_value()) {
        m_builtinIndex = *builtinIndex;
        m_paletteFilePath.clear();
    } else {
        m_paletteFilePath = filePath;
    }
    syncPaletteChecks();
    syncPaletteSelector();
    saveSettings();
    refreshPaletteDisplay();
}

void MainWindow::refreshPaletteDisplay()
{
    m_palette = m_reversePalette ? m_basePalette.reversed() : m_basePalette;
    m_colorBar->setPalette(&m_palette);
    scheduleSliceRequest();
    updateGridBoxes();
    updateOverlays();
    updateCrosshairs();
    m_isoWidget->update();
}

void MainWindow::setReversePalette(bool reversed)
{
    if (reversed == m_reversePalette) {
        return;
    }
    m_reversePalette = reversed;
    if (m_reversePaletteAction != nullptr) {
        const QSignalBlocker blocker(m_reversePaletteAction);
        m_reversePaletteAction->setChecked(reversed);
    }
    saveSettings();
    refreshPaletteDisplay();
    syncPaletteSelector();
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

bool MainWindow::animationDockVisibleForTest() const
{
    return m_animationDock != nullptr && m_animationDock->isVisible();
}

void MainWindow::setAnimationDockVisibleForTest(bool visible)
{
    if (m_animationDock != nullptr) {
        m_animationDock->setVisible(visible);
    }
}

QString MainWindow::viewPlaceholderForTest()
{
    QString shared;
    for (const auto* state : allViewStates()) {
        if (state->view->hasImage()) {
            return {};
        }
        const auto& text = state->view->placeholderText();
        if (shared.isEmpty()) {
            shared = text;
        } else if (shared != text) {
            return {};
        }
    }
    return shared;
}

bool MainWindow::activeViewRasterMatchesDisplayRangeForTest()
{
    if (m_activeView == nullptr) {
        return false;
    }
    const auto& state = *m_activeView;
    if (state.plane->width <= 0 || state.plane->height <= 0
        || !state.view->hasImage()) {
        return false;
    }
    const auto reference = renderScalarPlane(*state.plane, ScalarRenderSettings{
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

bool MainWindow::activeViewUsesViewportBoundedOutputForTest() const
{
    if (!std::dynamic_pointer_cast<remote::RemoteDatasetSession>(m_dataset)
        || m_activeView == nullptr || m_activeView->plane->width <= 0
        || m_activeView->plane->height <= 0) {
        return false;
    }
    const auto expected = sliceOutputSize(*m_activeView, true);
    return m_activeView->plane->width == expected[0]
        && m_activeView->plane->height == expected[1];
}

bool MainWindow::activeViewUsesNativeOutputForTest() const
{
    if (m_activeView == nullptr || m_activeView->plane->width <= 0
        || m_activeView->plane->height <= 0) {
        return false;
    }
    const auto expected = nativeOutputSize(*m_activeView);
    return m_activeView->plane->width == expected[0]
        && m_activeView->plane->height == expected[1];
}

bool MainWindow::allViewsUseViewportBoundedOutputForTest() const
{
    if (!std::dynamic_pointer_cast<remote::RemoteDatasetSession>(m_dataset)) {
        return false;
    }
    const std::array<const PlaneViewState*, 3> threeDimensional{
        &m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
    const auto check = [&](const PlaneViewState& state) {
        if (state.plane->width <= 1 || state.plane->height <= 1) {
            return false;
        }
        const auto expected = sliceOutputSize(state, true);
        return state.plane->width == expected[0]
            && state.plane->height == expected[1];
    };
    if (m_viewDimension == 2) {
        return check(m_view2d);
    }
    return m_viewDimension == 3
        && std::all_of(threeDimensional.begin(), threeDimensional.end(),
            [&](const auto* state) { return check(*state); });
}

int MainWindow::slicesInFlightForTest() const
{
    if (m_viewDimension == 2) {
        return m_view2d.pendingRequests;
    }
    const std::array<const PlaneViewState*, 3> threeDimensional{
        &m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
    int total = 0;
    for (const auto* state : threeDimensional) {
        total += state->pendingRequests;
    }
    return total;
}

bool MainWindow::allViewsFixedScaleRasterCoversViewportForTest() const
{
    const auto check = [](const PlaneViewState& state) {
        const auto* view = state.view;
        if (view == nullptr || !view->hasImage()
            || view->transformMode() != ImageView::TransformMode::FixedScale
            || view->viewport() == nullptr) {
            return false;
        }
        const auto raster = QRectF(view->mapFromScene(
            view->imageSceneRect()).boundingRect());
        const auto domain = QRectF(view->mapFromScene(
            view->sceneRect()).boundingRect());
        // Everything the viewport shows of the domain must be backed by the
        // raster; one pixel of slack absorbs the integer mapping round-off.
        const auto shown = QRectF(view->viewport()->rect())
            .intersected(domain).adjusted(1.0, 1.0, -1.0, -1.0);
        return shown.isEmpty() || raster.contains(shown);
    };
    if (m_viewDimension == 2) {
        return check(m_view2d);
    }
    const std::array<const PlaneViewState*, 3> threeDimensional{
        &m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
    return m_viewDimension == 3
        && std::all_of(threeDimensional.begin(), threeDimensional.end(),
            [&](const auto* state) { return check(*state); });
}

bool MainWindow::activeViewHasPhysicalAspectForTest(
    double expectedAspect) const
{
    if (!m_dataset || m_activeView == nullptr
        || m_activeView->plane->width <= 0
        || m_activeView->plane->height <= 0 || !(expectedAspect > 0.0)) {
        return false;
    }
    const auto actualAspect = static_cast<double>(m_activeView->plane->width)
        / m_activeView->plane->height;
    return std::abs(actualAspect - expectedAspect)
        <= 0.02 * expectedAspect;
}

bool MainWindow::fabStateClearedForTest() const
{
    return !m_fabMode && !m_multifabReturn && !m_fabSourceMetadata
        && m_fabSourcePath.empty() && m_fabDataRoot.empty()
        && m_fabSelectorDock->entries().empty()
        && !m_fabSelectorDock->isVisible()
        && !windowTitle().endsWith(QStringLiteral(" FAB"));
}

void MainWindow::setGridBoxesVisibleForTest(bool visible)
{
    m_boxesAction->setChecked(visible);
}

std::size_t MainWindow::activeViewGridBoxCountForTest() const
{
    return m_activeView == nullptr
        ? 0 : m_activeView->view->gridBoxCount();
}

void MainWindow::rubberBandZoomActiveViewForTest()
{
    if (m_activeView == nullptr || m_activeView->plane->width <= 0
        || m_activeView->plane->height <= 0) {
        return;
    }
    const auto width = static_cast<double>(m_activeView->plane->width);
    const auto height = static_cast<double>(m_activeView->plane->height);
    rubberBandZoom(*m_activeView,
        QRectF(0.25 * width, 0.25 * height, 0.5 * width, 0.5 * height));
}

void MainWindow::rubberBandZoomRectangularActiveViewForTest()
{
    if (m_activeView == nullptr || m_activeView->plane->width <= 0
        || m_activeView->plane->height <= 0) {
        return;
    }
    const auto width = static_cast<double>(m_activeView->plane->width);
    const auto height = static_cast<double>(m_activeView->plane->height);
    rubberBandZoom(*m_activeView,
        QRectF(0.275 * width, 0.35 * height, 0.45 * width, 0.2 * height));
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

void MainWindow::selectFixedScaleForTest(int factor)
{
    if (m_scaleGroup == nullptr) {
        return;
    }
    const auto label = tr("%1x").arg(factor);
    for (auto* action : m_scaleGroup->actions()) {
        auto text = action->text();
        text.remove(QLatin1Char('&'));
        if (text == label) {
            action->trigger();
            return;
        }
    }
}

bool MainWindow::fixedScaleStateMatchesForTest(int factor) const
{
    if (m_activeView == nullptr || m_scaleGroup == nullptr
        || m_scaleButton == nullptr) {
        return false;
    }
    const auto expected = tr("%1x").arg(factor);
    auto* checked = m_scaleGroup->checkedAction();
    auto checkedText = checked == nullptr ? QString() : checked->text();
    checkedText.remove(QLatin1Char('&'));
    const auto* view = m_activeView->view;
    return view->transformMode() == ImageView::TransformMode::FixedScale
        && view->fixedScaleFactor() == factor
        && std::fabs(view->transform().m11() - factor) <= 1.0e-12
        && m_scaleButton->text() == expected && checkedText == expected;
}

void MainWindow::wheelZoomAndPanActiveViewForTest()
{
    if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
        return;
    }
    m_activeView->view->zoomBy(1.5);
    m_activeView->view->panViewport(QPoint(11, -7));
}

void MainWindow::shiftDragActiveViewForTest(int dx, int dy)
{
    if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
        return;
    }
    auto* const viewport = m_activeView->view->viewport();
    if (viewport == nullptr) {
        return;
    }
    const QPoint start = viewport->rect().center();
    const QPoint finish = start + QPoint(dx, dy);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(start),
        viewport->mapToGlobal(start), Qt::LeftButton, Qt::LeftButton,
        Qt::ShiftModifier);
    QApplication::sendEvent(viewport, &press);
    QMouseEvent move(QEvent::MouseMove, QPointF(finish),
        viewport->mapToGlobal(finish), Qt::NoButton, Qt::LeftButton,
        Qt::ShiftModifier);
    QApplication::sendEvent(viewport, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(finish),
        viewport->mapToGlobal(finish), Qt::LeftButton, Qt::NoButton,
        Qt::ShiftModifier);
    QApplication::sendEvent(viewport, &release);
}

bool MainWindow::activeViewScrollBarsVisibleForTest() const
{
    if (m_activeView == nullptr) {
        return false;
    }
    // Scroll range, not widget visibility: under the as-needed policy a
    // non-empty range is what shows the bars, and the range updates
    // synchronously while the widgets only follow on the next layout pass.
    const auto* view = m_activeView->view;
    return view->horizontalScrollBar()->maximum()
            > view->horizontalScrollBar()->minimum()
        || view->verticalScrollBar()->maximum()
            > view->verticalScrollBar()->minimum();
}

QRectF MainWindow::activeViewVisibleDataWindowForTest() const
{
    if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
        return {};
    }
    const auto* view = m_activeView->view;
    const auto& plane = *m_activeView->plane;
    if (plane.width < 1 || plane.height < 1) {
        return {};
    }
    // Raster pixels, not scene units: a remote fixed scale hosts the fetched
    // raster on a whole-domain virtual canvas whose scene coordinates are
    // finest cells, so intersecting the viewport's scene rect with the pixel
    // rect directly would report a window in the wrong space (and offset by
    // the fetched window's position in the domain) whenever the canvas
    // scrolls.
    //
    // The second clamp is not redundant with the one inside visibleImageRect:
    // that one clamps to the pixmap, while the normalization below divides by
    // the plane's dimensions, and a supersampled spherical warp makes the
    // pixmap larger than the plane. Clamping to the plane rect keeps the
    // fractions in range there, exactly as this probe did before.
    const auto visible = view->visibleImageRect().intersected(
        QRectF(0.0, 0.0, plane.width, plane.height));
    const auto axes = displayAxes(m_activeView->normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto& region = plane.physicalRegion;
    const auto xExtent = region.upper[xAxis] - region.lower[xAxis];
    const auto yExtent = region.upper[yAxis] - region.lower[yAxis];
    const auto x0 = region.lower[xAxis]
        + visible.left() / plane.width * xExtent;
    const auto x1 = region.lower[xAxis]
        + visible.right() / plane.width * xExtent;
    const auto y0 = region.upper[yAxis]
        - visible.bottom() / plane.height * yExtent;
    const auto y1 = region.upper[yAxis]
        - visible.top() / plane.height * yExtent;
    return QRectF(QPointF(x0, y0), QPointF(x1, y1)).normalized();
}

void MainWindow::panActiveViewForTest(
    double sceneDeltaX, double sceneDeltaY)
{
    if (m_activeView == nullptr) {
        return;
    }
    const QPointF delta(sceneDeltaX, sceneDeltaY);
    // A real drag reports the mouse movement in viewport pixels alongside
    // the scene delta; the view-only pan path (virtual canvases included)
    // scrolls by exactly those pixels.
    const auto* view = m_activeView->view;
    const QPoint viewportDelta(
        static_cast<int>(std::round(sceneDeltaX * view->transform().m11())),
        static_cast<int>(std::round(sceneDeltaY * view->transform().m22())));
    beginPanDrag(*m_activeView);
    updatePanDrag(*m_activeView, delta, viewportDelta);
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
    // Half-a-scene-pixel slack absorbs fitInView rounding at the borders.
    return visible.adjusted(-0.5, -0.5, 0.5, 0.5).contains(
        view->imageSceneRect());
}

void MainWindow::viewFabForTest(std::size_t index)
{
    viewFab(index);
}

bool MainWindow::activeViewIsZoomedForTest() const
{
    return m_activeView != nullptr && m_activeView->visibleRegion.has_value();
}

void MainWindow::setSphericalSupersampleForTest(int factor)
{
    // Mirror the menu handler: record the factor and re-warp the cached planes.
    m_sphericalSupersample = factor;
    if (displayIsSpherical()) {
        scheduleSliceRequest(true);
    }
}

int MainWindow::activeViewImageWidthForTest() const
{
    if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
        return 0;
    }
    return m_activeView->view->image().width();
}

std::array<int, 2> MainWindow::activeViewImageSizeForTest() const
{
    if (m_activeView == nullptr || !m_activeView->view->hasImage()) {
        return {0, 0};
    }
    const auto image = m_activeView->view->image();
    return {image.width(), image.height()};
}

std::array<int, 2> MainWindow::activeViewViewportSizeForTest() const
{
    if (m_activeView == nullptr || m_activeView->view->viewport() == nullptr) {
        return {0, 0};
    }
    const auto* viewport = m_activeView->view->viewport();
    return {viewport->width(), viewport->height()};
}

QImage MainWindow::activeViewViewportImageForTest() const
{
    if (m_activeView == nullptr || m_activeView->view->viewport() == nullptr) {
        return {};
    }
    return m_activeView->view->viewport()->grab().toImage();
}

bool MainWindow::activeViewFitsWindowForTest() const
{
    return m_activeView != nullptr && m_activeView->view->hasImage()
        && m_activeView->view->isFitToWindow();
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
    const auto top = static_cast<double>(state.plane->height) - 1.0;
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
    const auto planeReady = state.plane->width > 1 && state.plane->height > 1;
    if (!planeReady || m_displayMode == DisplayMode::Raster) {
        state.view->setOverlaySegments(overlays);
        state.view->setOverlayPaths(paths);
        return;
    }

    if (m_displayMode == DisplayMode::VelocityVectors) {
        // Segment coordinates depend on the layout the arrival was generated
        // for (state, not the in-flight menu selection): R-Z glyphs carry
        // display physical (R, Z) endpoints already rotated into physical
        // directions (see generateSphericalRZVectorGlyphs); the logical
        // r-theta / theta-r layouts carry plane pixels mapped through the
        // plane mapping (identity or transposed); Cartesian keeps the plain
        // pixel-to-scene flip.
        overlays.reserve(state.vectorSegments.size());
        const auto vectorColor = overlayColor();
        const bool spherical = displayIsSpherical();
        const bool sphericalRZ = spherical
            && state.sphericalDisplay == SphericalDisplay::RZ;
        const auto mapping = planeMapping(state);
        for (const auto& segment : state.vectorSegments) {
            const auto line = sphericalRZ
                ? QLineF(mapping.sceneFromDisplay(segment.x0, segment.y0),
                    mapping.sceneFromDisplay(segment.x1, segment.y1))
                : spherical
                ? QLineF(mapping.sceneFromPlanePixel(segment.x0, segment.y0),
                    mapping.sceneFromPlanePixel(segment.x1, segment.y1))
                : planeSegmentToScene(state,
                    segment.x0, segment.y0, segment.x1, segment.y1);
            overlays.push_back({line, vectorColor, 1.0F});
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
        const bool spherical = displayIsSpherical();
        const auto mapping = planeMapping(state);
        const auto top = static_cast<double>(state.plane->height) - 1.0;
        // Cartesian: plane pixel maps 1:1 to the scene (only the vertical flip).
        // Spherical: re-project each (r, theta) plane pixel through the warp.
        const auto toScene = [&](const auto& point) -> QPointF {
            if (spherical) {
                return mapping.sceneFromPlanePixel(point[0], point[1]);
            }
            return QPointF(point[0], top - point[1]);
        };
        std::map<double, QPainterPath> pathsByValue;
        for (const auto& polyline : state.contourPolylines) {
            if (polyline.points.empty()) {
                continue;
            }
            auto& path = pathsByValue[polyline.value];
            path.moveTo(toScene(polyline.points.front()));
            for (std::size_t i = 1; i < polyline.points.size(); ++i) {
                path.lineTo(toScene(polyline.points[i]));
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

void MainWindow::showParticlesDialog()
{
    if (!m_dataset || m_dataset->particleSpecies().empty()) {
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Particles"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(
        tr("Select particle species to draw. Sampling hashes the persistent "
           "particle ID/CPU identity, so the same particles remain selected "
           "across plotfile frames."),
        &dialog));

    struct SpeciesControls {
        std::string name;
        QCheckBox* enabled = nullptr;
        QPushButton* colorButton = nullptr;
        QSpinBox* alpha = nullptr;
        QColor color;
    };
    std::vector<SpeciesControls> speciesControls;
    speciesControls.reserve(m_dataset->particleSpecies().size());
    auto* speciesGrid = new QGridLayout;
    speciesGrid->addWidget(new QLabel(tr("Show"), &dialog), 0, 0);
    speciesGrid->addWidget(new QLabel(tr("Species"), &dialog), 0, 1);
    speciesGrid->addWidget(new QLabel(tr("Color"), &dialog), 0, 2);
    speciesGrid->addWidget(new QLabel(tr("Alpha"), &dialog), 0, 3);
    for (std::size_t speciesIndex = 0;
         speciesIndex < m_dataset->particleSpecies().size(); ++speciesIndex) {
        const auto& species = m_dataset->particleSpecies()[speciesIndex];
        auto* check = new QCheckBox(&dialog);
        check->setChecked(!m_particleSelectionInitialized
            || std::find(m_selectedParticleSpecies.begin(),
                m_selectedParticleSpecies.end(), species.name)
                != m_selectedParticleSpecies.end());
        auto* name = new QLabel(
            tr("%1 (%2 particles)")
                .arg(QString::fromStdString(species.name))
                .arg(species.particleCount),
            &dialog);
        auto color = m_particleColors.contains(species.name)
            ? m_particleColors.at(species.name)
            : defaultParticleColor(speciesIndex);
        auto* colorButton = new QPushButton(&dialog);
        updateColorButton(*colorButton, color);
        auto* alpha = new QSpinBox(&dialog);
        alpha->setRange(0, 100);
        alpha->setSuffix(tr("%"));
        alpha->setValue(qRound(color.alphaF() * 100.0));
        const auto row = static_cast<int>(speciesIndex + 1);
        speciesGrid->addWidget(check, row, 0, Qt::AlignHCenter);
        speciesGrid->addWidget(name, row, 1);
        speciesGrid->addWidget(colorButton, row, 2);
        speciesGrid->addWidget(alpha, row, 3);
        speciesControls.push_back(
            {species.name, check, colorButton, alpha, color});
    }
    for (auto& controls : speciesControls) {
        auto* controlsPtr = &controls;
        connect(controls.colorButton, &QPushButton::clicked, &dialog,
            [&dialog, controlsPtr] {
                auto chosen = QColorDialog::getColor(controlsPtr->color, &dialog,
                    QObject::tr("Particle color"));
                if (!chosen.isValid()) {
                    return;
                }
                chosen.setAlpha(controlsPtr->color.alpha());
                controlsPtr->color = chosen;
                updateColorButton(
                    *controlsPtr->colorButton, controlsPtr->color);
            });
    }
    layout->addLayout(speciesGrid);

    auto* fractionRow = new QHBoxLayout;
    fractionRow->addWidget(new QLabel(tr("Visible subset:"), &dialog));
    auto* fraction = new QDoubleSpinBox(&dialog);
    fraction->setRange(0.01, 100.0);
    fraction->setDecimals(2);
    fraction->setSuffix(tr("%"));
    fraction->setValue(m_particleFraction * 100.0);
    fractionRow->addWidget(fraction);
    fractionRow->addStretch(1);
    layout->addLayout(fractionRow);

    auto* seedRow = new QHBoxLayout;
    seedRow->addWidget(new QLabel(tr("Sampling seed:"), &dialog));
    auto* seed = new QLineEdit(QString::number(m_particleSeed), &dialog);
    seed->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]{1,20}")), seed));
    seed->setToolTip(tr(
        "Change the seed to select a different stable particle subset."));
    seedRow->addWidget(seed);
    seedRow->addStretch(1);
    layout->addLayout(seedRow);

    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(new QLabel(tr("Point size:"), &dialog));
    auto* pointSize = new QSpinBox(&dialog);
    pointSize->setRange(1, 12);
    pointSize->setValue(m_particlePointSize);
    sizeRow->addWidget(pointSize);
    sizeRow->addStretch(1);
    layout->addLayout(sizeRow);

    if (m_dataset->metadata().dimension == 3) {
        layout->addWidget(new QLabel(
            tr("In 3-D, points are projected onto each orthogonal view."),
            &dialog));
    }
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, seed] {
        bool valid = false;
        static_cast<void>(seed->text().toULongLong(&valid));
        if (valid) {
            dialog.accept();
        } else {
            QMessageBox::warning(&dialog, QObject::tr("Invalid seed"),
                QObject::tr(
                    "The sampling seed must be an integer from 0 through "
                    "18446744073709551615."));
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    std::vector<std::string> selectedSpecies;
    for (auto& controls : speciesControls) {
        if (controls.enabled->isChecked()) {
            selectedSpecies.push_back(controls.name);
        }
        controls.color.setAlphaF(
            static_cast<float>(controls.alpha->value()) / 100.0F);
        m_particleColors[controls.name] = controls.color;
    }
    const auto seedValue = seed->text().toULongLong();
    applyParticleSelection(std::move(selectedSpecies),
        fraction->value() / 100.0, pointSize->value(), seedValue);
}

void MainWindow::configureParticleControls(bool preserveSelection)
{
    if (!m_dataset) {
        m_particlesAction->setEnabled(false);
        return;
    }
    const auto& species = m_dataset->particleSpecies();
    if (!preserveSelection) {
        m_selectedParticleSpecies.clear();
        m_particleColors.clear();
        m_particleSeed = 0;
        m_particleSelectionInitialized = false;
    }
    for (std::size_t speciesIndex = 0; speciesIndex < species.size();
         ++speciesIndex) {
        m_particleColors.try_emplace(
            species[speciesIndex].name, defaultParticleColor(speciesIndex));
    }
    m_particlesAction->setEnabled(
        !species.empty() && !m_particleLoading);
}

void MainWindow::applyParticleSelection(
    std::vector<std::string> species, double fraction, int pointSize,
    std::uint64_t seed)
{
    const bool sampleChanged = !m_particleSelectionInitialized
        || species != m_selectedParticleSpecies
        || fraction != m_particleFraction
        || seed != m_particleSeed;
    m_selectedParticleSpecies = std::move(species);
    m_particleFraction = fraction;
    m_particleSeed = seed;
    m_particlePointSize = pointSize;
    m_particleSelectionInitialized = true;
    if (!sampleChanged) {
        // Color, alpha, and point size only affect the installed point batches;
        // do not reread particle files when the sampled identities are unchanged.
        updateParticleOverlays();
        return;
    }
    m_sequenceController->invalidatePrefetch();
    // Mid-sequence-load, restart the frame so the new particle selection is
    // baked into the frame spec; otherwise reload the particle overlay alone.
    if (m_sequenceController->inFlight()
        && m_sequenceController->currentIndex() >= 0) {
        goToSequenceFrame(m_sequenceController->currentIndex(), true);
    } else {
        requestParticleReload();
    }
}

void MainWindow::setParticleSelectionForTest(
    std::vector<std::string> species, double fraction, std::uint64_t seed)
{
    applyParticleSelection(
        std::move(species), fraction, m_particlePointSize, seed);
}

std::uint64_t MainWindow::particleSeedForTest() const noexcept
{
    return m_particleSeed;
}

void MainWindow::setParticleColorForTest(
    const std::string& species, const QColor& color)
{
    m_particleColors[species] = color;
    updateParticleOverlays();
}

bool MainWindow::particleOverlaysUseColorForTest(const QColor& color)
{
    bool found = false;
    for (const auto* state : currentViews()) {
        for (const auto& overlayColor : state->view->pointOverlayColors()) {
            found = true;
            if (overlayColor != color) {
                return false;
            }
        }
    }
    return found;
}

std::size_t MainWindow::particleSampleCountForTest() const
{
    std::size_t count = 0;
    for (const auto& sample : m_particleSamples) {
        count += sample.points.size();
    }
    return count;
}

std::size_t MainWindow::particleOverlayCountForTest()
{
    std::size_t count = 0;
    for (const auto* state : currentViews()) {
        count += state->view->pointOverlayCount();
    }
    return count;
}

bool MainWindow::particleLoadingForTest() const noexcept
{
    return m_particleLoading;
}

bool MainWindow::particleLoadingUiActiveForTest() const
{
    return m_particleProgress->isVisible() && !m_particlesAction->isEnabled();
}

bool MainWindow::particleLoadingUiSettledForTest() const
{
    return !m_particleProgress->isVisible() && m_particlesAction->isEnabled();
}

void MainWindow::requestParticleReload()
{
    m_particleStopSource.request_stop();
    m_particleStopSource = StopSource{};
    const auto cancellation = m_particleStopSource.get_token();
    const auto dataset = m_dataset;
    const auto selectedSpecies = m_selectedParticleSpecies;
    const auto fraction = m_particleFraction;
    const auto seed = m_particleSeed;
    const auto generation = m_generation;
    const auto particleGeneration = ++m_particleGeneration;
    m_particleSamples.clear();
    updateParticleOverlays();
    if (!dataset || selectedSpecies.empty()) {
        m_particleLoading = false;
        m_particleProgress->setVisible(false);
        configureParticleControls(true);
        return;
    }

    m_particleLoading = true;
    m_particleProgress->setVisible(true);
    m_particlesAction->setEnabled(false);
    ++m_activeRequests;
    statusBar()->showMessage(tr("Loading particle sample..."));
    updateDiagnostics();
    auto* watcher = new QFutureWatcher<std::vector<ParticleSample>>(this);
    connect(watcher, &QFutureWatcher<std::vector<ParticleSample>>::finished,
        this, [this, watcher, generation, particleGeneration, cancellation] {
            --m_activeRequests;
            try {
                auto samples = watcher->result();
                if (generation == m_generation
                    && particleGeneration == m_particleGeneration) {
                    m_particleSamples = std::move(samples);
                    updateParticleOverlays();
                    std::uint64_t count = 0;
                    for (const auto& sample : m_particleSamples) {
                        count += sample.points.size();
                    }
                    statusBar()->showMessage(
                        tr("Showing %1 sampled particles").arg(count), 3000);
                } else {
                    ++m_staleResults;
                }
            } catch (const std::exception& error) {
                if (generation == m_generation
                    && particleGeneration == m_particleGeneration
                    && !cancellation.stop_requested()) {
                    reportBackgroundError(
                        tr("Particles were not loaded: %1")
                            .arg(exceptionMessage(error)));
                }
            }
            if (particleGeneration == m_particleGeneration) {
                m_particleLoading = false;
                m_particleProgress->setVisible(false);
                configureParticleControls(true);
            }
            updateDiagnostics();
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [dataset, selectedSpecies, fraction, seed, cancellation] {
            return loadParticleSamples(
                *dataset, selectedSpecies, fraction, seed, cancellation);
        }));
}

void MainWindow::updateParticleOverlay(PlaneViewState& state)
{
    std::vector<PointOverlay> overlays;
    if (!m_dataset || !state.view->hasImage()
        || state.plane->width <= 0 || state.plane->height <= 0
        // The warped R-Z view has no linear plane-pixel mapping for points.
        || displayIsSphericalWarp()) {
        state.view->setPointOverlays(overlays);
        return;
    }
    const bool spherical = displayIsSpherical();
    const auto mapping = planeMapping(state);
    const auto planeHeight = static_cast<double>(state.plane->height);
    overlays.reserve(m_particleSamples.size());
    for (const auto& sample : m_particleSamples) {
        PointOverlay overlay;
        const auto color = m_particleColors.find(sample.species.name);
        overlay.color = color != m_particleColors.end()
            ? color->second : QColor(Qt::white);
        overlay.size = static_cast<float>(m_particlePointSize);
        const auto projected = projectParticlePoints(
            sample.points, *state.plane,
            m_dataset->metadata().dimension, state.normal);
        overlay.points.reserve(projected.size());
        for (const auto& point : projected) {
            if (spherical) {
                // projectParticlePoints returns r-theta scene coords (y flipped
                // from height to 0). Recover the plane pixel and re-map through
                // the active layout (identity for r-theta, transposed otherwise).
                const auto scene = mapping.sceneFromPlanePixel(
                    point.x, planeHeight - point.y);
                overlay.points.emplace_back(scene.x(), scene.y());
            } else {
                overlay.points.emplace_back(point.x, point.y);
            }
        }
        overlays.push_back(std::move(overlay));
    }
    state.view->setPointOverlays(overlays);
}

void MainWindow::updateParticleOverlays()
{
    for (auto* state : currentViews()) {
        updateParticleOverlay(*state);
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
    add(tr("Arrow keys"),
        tr("Pan the focused panel (5% of the view per step)"));
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
    state.view->setVirtualCanvas(std::nullopt);
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
    const auto& plane = *state.plane;
    if (!m_dataset || plane.width <= 0 || plane.height <= 0) {
        return tr("no data");
    }
    const auto& metadata = m_dataset->metadata();
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto& region = plane.physicalRegion;

    // Map the probed pixmap pixel to a logical (x, y)/(r, theta) position and
    // the source plane offset holding its value, validity, and level. Cartesian
    // pixmap pixels index the plane directly; spherical pixmap pixels are in
    // warped (R, Z) space, so invert the warp first and re-derive the pixel.
    std::array<double, 3> position{0.0, 0.0, 0.0};
    std::size_t offset = 0;
    if (displayIsSpherical()) {
        const auto mapping = planeMapping(state);
        const auto logical = mapping.logicalFromScene(
            static_cast<double>(x) + 0.5, static_cast<double>(displayY) + 0.5);
        if (logical[0] < region.lower[xAxis] || logical[0] > region.upper[xAxis]
            || logical[1] < region.lower[yAxis]
            || logical[1] > region.upper[yAxis]) {
            return tr("no data");  // cursor is outside the annular sector
        }
        position[xAxis] = logical[0];
        position[yAxis] = logical[1];
        const auto spanX = region.upper[xAxis] - region.lower[xAxis];
        const auto spanY = region.upper[yAxis] - region.lower[yAxis];
        const int col = std::clamp(static_cast<int>(
            (logical[0] - region.lower[xAxis]) / spanX * plane.width),
            0, plane.width - 1);
        const int row = std::clamp(static_cast<int>(
            (logical[1] - region.lower[yAxis]) / spanY * plane.height),
            0, plane.height - 1);
        offset = static_cast<std::size_t>(col)
            + static_cast<std::size_t>(plane.width)
                * static_cast<std::size_t>(row);
    } else {
        const auto y = plane.height - 1 - displayY;
        offset = static_cast<std::size_t>(x)
            + static_cast<std::size_t>(plane.width)
                * static_cast<std::size_t>(y);
        position[xAxis] = region.lower[xAxis]
            + (static_cast<double>(x) + 0.5) / static_cast<double>(plane.width)
                * (region.upper[xAxis] - region.lower[xAxis]);
        position[yAxis] = region.lower[yAxis]
            + (static_cast<double>(y) + 0.5) / static_cast<double>(plane.height)
                * (region.upper[yAxis] - region.lower[yAxis]);
    }
    if (offset >= plane.values.size() || plane.valid[offset] == 0) {
        return tr("no data");
    }
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

    // Standalone FABs and MultiFabs have no AMR hierarchy, so their readout
    // omits the level.
    const auto levelText = metadata.hasPhysicalGeometry
        ? tr(" level=%1").arg(level)
        : QString();
    const auto valueText = formatNumber(
        static_cast<double>(plane.values[offset]), m_numberFormat);
    if (displayIsSpherical()) {
        // position[xAxis] is r, position[yAxis] is theta (from logicalFromScene).
        const QString theta(QChar(0x03B8));
        const auto rText = formatNumber(position[xAxis], m_numberFormat);
        const auto thetaText = formatNumber(position[yAxis], m_numberFormat);
        QString coords;
        // The state's mode, not the menu selection: the readout labels must
        // match the mapping that produced the coordinates above.
        switch (state.sphericalDisplay) {
        case SphericalDisplay::RZ: {
            // Physical (R, Z) plus the native spherical (r, theta).
            const auto display = sphericalToDisplay(
                position[xAxis], position[yAxis]);
            coords = QStringLiteral("R=%1 Z=%2 r=%3 %4=%5").arg(
                formatNumber(display[0], m_numberFormat),
                formatNumber(display[1], m_numberFormat), rText, theta, thetaText);
            break;
        }
        case SphericalDisplay::ThetaR:
            coords = QStringLiteral("%1=%2 r=%3").arg(theta, thetaText, rText);
            break;
        case SphericalDisplay::RTheta:
        default:
            coords = QStringLiteral("r=%1 %2=%3").arg(rText, theta, thetaText);
            break;
        }
        return tr("%1 value=%2%3 %4=(%5) %6").arg(coords, valueText, levelText,
            QString::fromLatin1(indexKind), join(cell), boxText);
    }
    return tr("%1=%2 %3=%4 value=%5%6 %7=(%8) %9")
        .arg(QString::fromLatin1(axisNames[xAxis]))
        .arg(formatNumber(position[xAxis], m_numberFormat))
        .arg(QString::fromLatin1(axisNames[yAxis]))
        .arg(formatNumber(position[yAxis], m_numberFormat))
        .arg(valueText)
        .arg(levelText)
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
    const auto& plane = *state.plane;
    if (!m_dataset || plane.width <= 0 || plane.height <= 0) {
        return;
    }
    if (displayIsSpherical()) {
        // The scene is warped (R, Z); re-slicing a logical (r, theta) subregion
        // from it is deferred. Zoom the view only, leaving the full-domain
        // warped raster in place.
        const QRectF bounds(0.0, 0.0,
            static_cast<double>(state.view->image().width()),
            static_cast<double>(state.view->image().height()));
        const auto selection = sceneRect.normalized().intersected(bounds);
        if (selection.width() < 1.0 || selection.height() < 1.0) {
            return;
        }
        state.view->zoomToRect(selection);
        if (m_scaleGroup != nullptr) {
            if (auto* checked = m_scaleGroup->checkedAction()) {
                checked->setChecked(false);
            }
        }
        if (m_scaleButton != nullptr) {
            m_scaleButton->setText(tr("Custom"));
        }
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
    const auto& plane = *state.plane;
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
    // Local slices use one output pixel per finest cell, so their edges land
    // on cell boundaries. Remote slices are viewport-resampled; retaining the
    // exact selection keeps an arbitrary rubber-band aspect ratio intact.
    if (!std::dynamic_pointer_cast<remote::RemoteDatasetSession>(m_dataset)) {
        const auto& metadata = m_dataset->metadata();
        const auto& finest = metadata.levels[static_cast<std::size_t>(
            std::max(0, metadata.finestLevel))];
        visible = snapToCellBoundaries(
            visible, datasetSampleBounds(metadata), finest.cellSize, axes);
    }
    state.visibleRegion = visible;
    // Rubber-band zoom leaves the virtual canvas: as with local data, the
    // selection is re-rendered as a standalone raster fitted to the pane,
    // with no domain-spanning scroll bars.
    state.view->setVirtualCanvas(std::nullopt);
    // Zoom to the requested region mapped back to scene pixels, so the view
    // transform matches the region the requested slice will actually cover.
    const QRectF requestedScene(
        QPointF((visible.lower[xAxis] - region.lower[xAxis]) / xExtent * width,
            (region.upper[yAxis] - visible.upper[yAxis]) / yExtent * height),
        QPointF((visible.upper[xAxis] - region.lower[xAxis]) / xExtent * width,
            (region.upper[yAxis] - visible.lower[yAxis]) / yExtent * height));
    state.view->zoomToRect(requestedScene.normalized());
    scheduleSliceRequest(state);
}

void MainWindow::beginPanDrag(PlaneViewState& state)
{
    setActiveView(state);
    m_panView = &state;
    m_panSceneDelta = QPointF();
    m_panLastScheduledDelta = QPointF();
    // A virtual canvas pans by scrolling (which fetches on its own); the
    // region-shifting refresh is for classic rasters of a zoomed subregion.
    m_panDataRefresh = state.visibleRegion.has_value()
        && !state.view->virtualCanvasActive();
    if (m_panDataRefresh) {
        m_panStartRegion = *state.visibleRegion;
        m_panPlaneWidth = state.plane->width;
        m_panPlaneHeight = state.plane->height;
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

std::array<double, 2> MainWindow::viewCenterInData(
    const PlaneViewState& state) const
{
    const auto& plane = *state.plane;
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto& region = plane.physicalRegion;
    if (plane.width <= 0 || plane.height <= 0 || state.view == nullptr
        || state.view->viewport() == nullptr) {
        return {0.5 * (region.lower[xAxis] + region.upper[xAxis]),
            0.5 * (region.lower[yAxis] + region.upper[yAxis])};
    }
    const auto scene = state.view->mapToScene(
        state.view->viewport()->rect().center());
    if (state.view->virtualCanvasActive() && m_dataset
        && !m_dataset->metadata().levels.empty()) {
        // Virtual canvas: scene units are finest cells over the whole domain,
        // counted from the domain's physical top-left.
        const auto& metadata = m_dataset->metadata();
        const auto domain = datasetSampleBounds(metadata);
        const auto& finest = metadata.levels[static_cast<std::size_t>(
            std::max(0, metadata.finestLevel))];
        const auto sceneRect = state.view->sceneRect();
        const auto cellX = std::clamp(scene.x(), 0.0, sceneRect.width());
        const auto cellY = std::clamp(scene.y(), 0.0, sceneRect.height());
        return {
            domain.lower[xAxis] + cellX * finest.cellSize[xAxis],
            domain.upper[yAxis] - cellY * finest.cellSize[yAxis]};
    }
    const auto sceneX = std::clamp(
        scene.x(), 0.0, static_cast<double>(plane.width));
    const auto sceneY = std::clamp(
        scene.y(), 0.0, static_cast<double>(plane.height));
    return {
        region.lower[xAxis] + sceneX / plane.width
            * (region.upper[xAxis] - region.lower[xAxis]),
        region.upper[yAxis] - sceneY / plane.height
            * (region.upper[yAxis] - region.lower[yAxis])};
}

bool MainWindow::remoteDemandCanvas(const PlaneViewState& state) const
{
    return m_dataset != nullptr
        && std::dynamic_pointer_cast<remote::RemoteDatasetSession>(m_dataset)
            != nullptr
        && !displayIsSpherical() && state.view != nullptr
        && state.view->virtualCanvasActive();
}

std::optional<ImageView::VirtualPlacement> MainWindow::virtualPlacementFor(
    const PlaneViewState& state, const RealBox& region) const
{
    if (!m_dataset || m_dataset->metadata().levels.empty()) {
        return std::nullopt;
    }
    const auto& metadata = m_dataset->metadata();
    const auto domain = datasetSampleBounds(metadata);
    const auto& finest = metadata.levels[static_cast<std::size_t>(
        std::max(0, metadata.finestLevel))];
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto dx = finest.cellSize[xAxis];
    const auto dy = finest.cellSize[yAxis];
    if (!(dx > 0.0) || !(dy > 0.0)) {
        return std::nullopt;
    }
    // Scene y grows downward: the region's upper physical y is its top row.
    const QRectF itemCells(
        (region.lower[xAxis] - domain.lower[xAxis]) / dx,
        (domain.upper[yAxis] - region.upper[yAxis]) / dy,
        (region.upper[xAxis] - region.lower[xAxis]) / dx,
        (region.upper[yAxis] - region.lower[yAxis]) / dy);
    const QSizeF domainCells(
        (domain.upper[xAxis] - domain.lower[xAxis]) / dx,
        (domain.upper[yAxis] - domain.lower[yAxis]) / dy);
    if (itemCells.isEmpty() || domainCells.isEmpty()) {
        return std::nullopt;
    }
    return ImageView::VirtualPlacement{itemCells, domainCells};
}

void MainWindow::centerViewOnData(
    PlaneViewState& state, const std::array<double, 2>& dataCenter)
{
    if (!m_dataset || m_dataset->metadata().levels.empty()
        || !state.view->virtualCanvasActive()) {
        return;
    }
    const auto& metadata = m_dataset->metadata();
    const auto domain = datasetSampleBounds(metadata);
    const auto& finest = metadata.levels[static_cast<std::size_t>(
        std::max(0, metadata.finestLevel))];
    const auto axes = displayAxes(state.normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    const auto dx = finest.cellSize[xAxis];
    const auto dy = finest.cellSize[yAxis];
    if (!(dx > 0.0) || !(dy > 0.0)) {
        return;
    }
    state.view->centerOn(
        (dataCenter[0] - domain.lower[xAxis]) / dx,
        (domain.upper[yAxis] - dataCenter[1]) / dy);
}

void MainWindow::applyFixedScale(int factor)
{
    const auto views = currentViews();
    std::vector<std::array<double, 2>> centers;
    centers.reserve(views.size());
    for (const auto* state : views) {
        centers.push_back(viewCenterInData(*state));
    }
    const bool demandDriven = std::dynamic_pointer_cast<
            remote::RemoteDatasetSession>(m_dataset) != nullptr
        && !displayIsSpherical();
    for (std::size_t index = 0; index < views.size(); ++index) {
        auto& state = *views[index];
        if (demandDriven) {
            // Host the raster on a whole-domain virtual canvas so the scroll
            // bars span the domain exactly as they do for a local fixed
            // scale; the canvas scroll state then decides which cells to
            // fetch (see updateRemoteFixedScaleDemand).
            state.view->setVirtualCanvas(virtualPlacementFor(
                state, state.plane->physicalRegion));
        } else {
            state.view->setVirtualCanvas(std::nullopt);
        }
        state.view->setFixedScale(factor);
        if (remoteDemandCanvas(state)) {
            centerViewOnData(state, centers[index]);
            updateRemoteFixedScaleDemand(state);
        }
    }
}

void MainWindow::updateRemoteFixedScaleDemand(PlaneViewState& state)
{
    if (!remoteDemandCanvas(state) || state.view->viewport() == nullptr) {
        return;
    }
    const auto& metadata = m_dataset->metadata();
    if (metadata.levels.empty()) {
        return;
    }
    const auto domain = datasetSampleBounds(metadata);
    const auto& finest = metadata.levels[static_cast<std::size_t>(
        std::max(0, metadata.finestLevel))];
    const auto axes = displayAxes(state.normal);
    // What the canvas currently shows, in finest cells; fetch that window.
    // The raster cannot change the scroll bars (the scene spans the domain
    // regardless of what is loaded), so this can never feed back on itself.
    const auto visible = state.view->mapToScene(
        state.view->viewport()->rect()).boundingRect();
    const auto sceneRect = state.view->sceneRect();
    if (visible.isEmpty() || sceneRect.isEmpty()) {
        return;
    }
    const std::array<double, 2> visibleLower{visible.left(), visible.top()};
    const std::array<double, 2> visibleSpan{visible.width(), visible.height()};
    const std::array<double, 2> domainSpanCells{
        sceneRect.width(), sceneRect.height()};
    auto target = domain;
    for (std::size_t entry = 0; entry < axes.size(); ++entry) {
        const auto axis = static_cast<std::size_t>(axes[entry]);
        const auto dx = finest.cellSize[axis];
        const auto domainCells = std::max(1.0,
            std::round(domainSpanCells[entry]));
        // A constant one-cell slack keeps the fetched size independent of
        // the scroll phase, so a pan replaces the raster with an equal-size
        // one and the display transform is provably unaffected.
        const auto cells = std::min(domainCells,
            std::ceil(std::max(1.0, visibleSpan[entry])) + 1.0);
        if (cells >= domainCells) {
            continue;
        }
        const auto first = std::clamp(std::floor(visibleLower[entry]),
            0.0, domainCells - cells);
        if (entry == 0) {
            target.lower[axis] = domain.lower[axis] + first * dx;
            target.upper[axis] = target.lower[axis] + cells * dx;
        } else {
            // Scene y counts cells down from the domain's physical top.
            target.upper[axis] = domain.upper[axis] - first * dx;
            target.lower[axis] = target.upper[axis] - cells * dx;
        }
    }
    if (target == domain) {
        state.visibleRegion.reset();
    } else {
        state.visibleRegion = target;
    }
    if (!state.hasCachedRequest
        || state.cachedRequest.visibleRegion != target
        || state.cachedRequest.outputSize != sliceOutputSize(state)) {
        scheduleSliceRequest(state, true);
    }
}

void MainWindow::applyPanStep(PlaneViewState& state, const QPointF& direction)
{
    if (!state.view->hasImage() || state.plane->width <= 0 || state.plane->height <= 0) {
        return;
    }
    setActiveView(state);
    const auto stepX = std::max(1.0, static_cast<double>(state.plane->width) * 0.05);
    const auto stepY = std::max(1.0, static_cast<double>(state.plane->height) * 0.05);
    const QPointF sceneDelta(direction.x() * stepX, direction.y() * stepY);

    if (state.visibleRegion.has_value() && m_dataset
        && !state.view->virtualCanvasActive()) {
        const auto region = shiftedPanRegion(state, *state.visibleRegion,
            state.plane->width, state.plane->height, sceneDelta);
        if (!region.has_value()) {
            return;
        }
        state.visibleRegion = *region;
        const bool remoteFixed = std::dynamic_pointer_cast<
            remote::RemoteDatasetSession>(m_dataset) != nullptr
            && state.view->transformMode()
                == ImageView::TransformMode::FixedScale;
        if (!remoteFixed) {
            state.view->fitToWindow();
            m_resetZoomAction->setChecked(true);
            if (m_scaleButton != nullptr) {
                m_scaleButton->setText(tr("Fit"));
            }
        }
        scheduleSliceRequest(state, false);
        return;
    }

    // shiftedPanRegion above and panViewport here share one convention: the
    // delta says how far the *content* moves. Handing both the same unnegated
    // sceneDelta is what keeps an arrow key moving the view rather than the
    // image once the pan falls through to the scroll bars. The axes convert
    // independently because a fixed scale over a raster whose aspect differs
    // from the logical size yields an anisotropic transform.
    const auto transform = state.view->transform();
    state.view->panViewport(QPoint(
        static_cast<int>(std::round(sceneDelta.x() * transform.m11())),
        static_cast<int>(std::round(sceneDelta.y() * transform.m22()))));
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
    const auto& plane = *state.plane;
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
    LineRequest request;
    if (displayIsSpherical()) {
        // Logical r-theta / theta-r layout: the click is in the possibly
        // transposed pixmap, so makeLineRequest's "r is horizontal" assumption
        // does not hold. Derive the varied axis and the fixed coordinate through
        // the mode-aware mapping instead. A horizontal drag varies whichever
        // logical axis is horizontal (r for r-theta, theta for theta-r).
        const auto mapping = planeMapping(state);
        const auto logical = mapping.logicalFromScene(
            static_cast<double>(imageX) + 0.5, static_cast<double>(imageY) + 0.5);
        // The state's mode, matching the mapping the click was interpreted
        // through (the raster on screen, not a pending menu selection).
        const int horizontalAxis =
            state.sphericalDisplay == SphericalDisplay::ThetaR ? 1 : 0;
        const int variedAxis = horizontal ? horizontalAxis : 1 - horizontalAxis;
        const int fixedAxis = 1 - variedAxis;
        request.dataset = dataset->id();
        request.field = FieldId{field};
        request.maximumLevel = maximumLevel;
        request.composition = composition;
        request.axis = variedAxis;
        request.fixedCoordinates[static_cast<std::size_t>(fixedAxis)] =
            logical[static_cast<std::size_t>(fixedAxis)];
        request.region = plane.physicalRegion;
    } else {
        request = makeLineRequest(plane.physicalRegion,
            plane.width, plane.height, imageX, imageY, horizontal,
            metadata.dimension, state.normal, slicePosition,
            dataset->id(), FieldId{field}, maximumLevel, composition);
    }
    const auto outputWidth = horizontal ? state.view->image().width()
                                        : state.view->image().height();
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
        [dataset, request, outputWidth, cancellation] {
            auto result = dataset->requestView(
                ViewDataRequest{LineViewRequest{request, outputWidth}},
                cancellation);
            return std::get<LineQueryResult>(std::move(result));
        }));
}

void MainWindow::sliceMoveRequested(PlaneViewState& state, int imageX, int imageY,
    Qt::MouseButton /*button*/)
{
    setActiveView(state);
    if (!m_dataset || m_dataset->metadata().dimension != 3
        || state.plane->width <= 0 || state.plane->height <= 0) {
        return;
    }
    // Move both in-plane axes so the three slices intersect at the clicked
    // point. A single right-click replaces the old middle=x / right=y split,
    // which was inaccessible on Mac (no middle button).
    const auto axes = displayAxes(state.normal);
    const auto& region = state.plane->physicalRegion;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto axis = axes[i];
        const auto fraction = (i == 0)
            ? (static_cast<double>(imageX) + 0.5)
                / static_cast<double>(state.plane->width)
            : (static_cast<double>(state.plane->height - 1 - imageY) + 0.5)
                / static_cast<double>(state.plane->height);
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
    if (displayIsSpherical()) {
        // Logical axes 0 and 1 are always r and theta, whichever screen layout
        // is active.
        curve.axisNames = {QStringLiteral("r"), QString(QChar(0x03B8)), QString()};
    }
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
    // Stop resubmit timers and request cancellation on every async task this
    // window owns; running tasks re-check their stop token and bail promptly,
    // so a task mid-read cannot leave the process lingering at quit. This does
    // NOT clear the shared global pool -- that would strand other windows'
    // queued work; the pool clear happens only on aboutToQuit (see
    // cancelInFlight).
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
    // Stop the timers that resubmit work and request stop on every async task
    // this window can launch. The slice/prefetch/line-query/initial-load tasks
    // run on QThreadPool::globalInstance() via QtConcurrent::run; that pool's
    // destructor calls waitForDone() with no timeout, and a task caught mid-read
    // holds the global AMReX I/O mutex and only re-checks its cancellation token
    // at the next chunk boundary (PlotfileBlockReader checks every 1 MiB / 4096
    // values). request_stop is the cooperative signal those tasks poll, so once
    // set a running task bails promptly and teardown unblocks -- which is what
    // keeps closing a window (or quitting) from looking like a hang.
    //
    // This must NOT clear() the global pool: it is shared by every MainWindow
    // (File -> Open New Window), and clear() discards *other* windows' queued-
    // but-unstarted runnables, whose QFutureWatchers then never fire, wedging
    // those windows on "Loading..." forever (see
    // window-close-clears-shared-thread-pool). Cancellation is per-task via the
    // stop tokens above; a queued task starts, observes its token, and exits
    // cheaply. clear() belongs only on the aboutToQuit path (every window is
    // going away), where it is wired in the constructor.
    if (m_sliceDebounce != nullptr) {
        m_sliceDebounce->stop();
    }
    if (m_playbackTimer != nullptr) {
        m_playbackTimer->stop();
    }
    m_initialStopSource.request_stop();
    m_metadataStopSource.request_stop();
    m_remoteVerifyStopSource.request_stop();
    ++m_remoteVerifyGeneration;
    m_sequenceController->cancelActiveWork();
    m_linePlotStopSource.request_stop();
    m_particleStopSource.request_stop();
    m_view2d.stopSource.request_stop();
    for (auto& state : m_planeViews) {
        state.stopSource.request_stop();
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
                m_basePalette = Palette::load(path.toStdString());
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
        m_basePalette = builtinPalette(
            builtinPalettes[static_cast<std::size_t>(m_builtinIndex)]);
        m_paletteFromFile = false;
        m_paletteFilePath.clear();
    }
    m_reversePalette = settings.value(QStringLiteral("palette/reversed"), false)
        .toBool();
    if (m_reversePaletteAction != nullptr) {
        const QSignalBlocker blocker(m_reversePaletteAction);
        m_reversePaletteAction->setChecked(m_reversePalette);
    }
    m_palette = m_reversePalette ? m_basePalette.reversed() : m_basePalette;
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
    if (m_sphericalSupersampleGroup != nullptr) {
        const auto stored = settings.value(
            QStringLiteral("spherical/supersample"), m_sphericalSupersample).toInt();
        // Accept only a factor the menu offers; otherwise keep the default.
        // setChecked emits toggled, not triggered, so the re-warp slot is not
        // fired here.
        for (auto* action : m_sphericalSupersampleGroup->actions()) {
            if (action->data().toInt() == stored) {
                m_sphericalSupersample = stored;
                action->setChecked(true);
                break;
            }
        }
    }
    if (m_sphericalDisplayGroup != nullptr) {
        const auto stored = settings.value(QStringLiteral("spherical/display"),
            static_cast<int>(m_sphericalDisplay)).toInt();
        for (auto* action : m_sphericalDisplayGroup->actions()) {
            if (action->data().toInt() == stored) {
                m_sphericalDisplay = static_cast<SphericalDisplay>(stored);
                action->setChecked(true);
                break;
            }
        }
    }
    applySpeed();

    const auto geometry = settings.value(QStringLiteral("geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
}

void MainWindow::saveRemoteSettings()
{
    // Only the port is persisted, purely to prefill the Connect dialog after a
    // client restart against a still-running server. The token is a secret and
    // is deliberately never written to disk; the host is always loopback via
    // the SSH tunnel.
    auto settings = makeSettings();
    settings.setValue(QStringLiteral("remote/port"),
        static_cast<uint>(m_remotePort));
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
    settings.setValue(QStringLiteral("palette/reversed"), m_reversePalette);
    settings.setValue(QStringLiteral("numberFormat"), m_numberFormat);
    settings.setValue(QStringLiteral("animation/speed"),
        m_animationPanel->speedValue());
    settings.setValue(QStringLiteral("zoom/syncRubberBand"),
        m_syncRubberBandZoomAction->isChecked());
    settings.setValue(QStringLiteral("spherical/supersample"),
        m_sphericalSupersample);
    settings.setValue(QStringLiteral("spherical/display"),
        static_cast<int>(m_sphericalDisplay));
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
    // Standalone FABs and MultiFabs carry neither a simulation time nor an
    // AMR hierarchy, so their titles show just the format name.
    if (m_fabMode) {
        setWindowTitle(tr("AMReXplorer — %1 — FAB").arg(name));
    } else if (!metadata.hasPhysicalGeometry) {
        setWindowTitle(tr("AMReXplorer — %1 — MultiFab").arg(name));
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
    // Directory pickers descend into a plotfile on double-click instead of
    // selecting it, so the choice easily lands on an inner directory
    // (Level_1, a particle species, ...). Such a selection resolves up to
    // the enclosing plotfile rather than failing on the subdirectory.
    auto path = std::filesystem::path(directory.toStdString());
    for (auto candidate = path; !candidate.empty();
        candidate = candidate.parent_path()) {
        if (isAmrexPlotfile(candidate)) {
            path = candidate;
            break;
        }
        if (candidate.parent_path() == candidate) {
            break;
        }
    }
    openDataset(path);
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

namespace {

// Scene-space annular-sector outline for a logical (r, theta) box: two
// straight radial edges (constant theta) and two subdivided arcs (constant r).
// Used for the spherical grid-box outlines and the picked-cell highlight.
QPainterPath sphericalSectorPath(const PlaneMapping& mapping,
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
FabSelectorBuild buildFabSelector(
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

} // namespace

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

    QString selectedFilter;
    auto filename = QFileDialog::getSaveFileName(
        this, tr("Export scalar image"), QString(),
        tr("PNG image (*.png);;FITS float64 image (*.fits *.fit)"),
        &selectedFilter);
    if (filename.isEmpty()) {
        return;
    }

    const bool hasFitsExtension = filename.endsWith(
            QStringLiteral(".fits"), Qt::CaseInsensitive)
        || filename.endsWith(QStringLiteral(".fit"), Qt::CaseInsensitive);
    const bool hasPngExtension = filename.endsWith(
        QStringLiteral(".png"), Qt::CaseInsensitive);
    // An explicitly typed recognized extension wins over the selected filter.
    const bool fits = hasFitsExtension
        || (!hasPngExtension
            && selectedFilter.contains(QStringLiteral("*.fits")));
    const QString extension = filename.endsWith(
            QStringLiteral(".fit"), Qt::CaseInsensitive)
        ? QStringLiteral(".fit")
        : fits ? QStringLiteral(".fits") : QStringLiteral(".png");

    // Normalize the chosen extension and get the base name used for the
    // per-panel suffixes of a 3-D export.
    QString base = filename;
    if (base.endsWith(QStringLiteral(".fits"), Qt::CaseInsensitive)) {
        base.chop(5);
    } else if (base.endsWith(QStringLiteral(".fit"), Qt::CaseInsensitive)
        || base.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
        base.chop(4);
    }
    filename = base + extension;

    if (fits) {
        const auto writePlane = [this](const QString& outPath,
                                    const ScalarPlane& plane) {
            try {
                writeFloat64Fits(
                    std::filesystem::path(outPath.toStdString()), plane);
                return true;
            } catch (const std::exception& error) {
                QMessageBox::critical(this, tr("Cannot export image"),
                    tr("Could not write %1.\n\n%2")
                        .arg(outPath, QString::fromUtf8(error.what())));
                return false;
            }
        };
        if (m_viewDimension == 3) {
            constexpr std::array<const char*, 3> suffixes{"_yz", "_xz", "_xy"};
            for (std::size_t normal = 0; normal < m_planeViews.size(); ++normal) {
                const auto& state = m_planeViews[normal];
                if (state.plane->width <= 0 || state.plane->height <= 0) {
                    continue;
                }
                const auto outPath = base
                    + QString::fromLatin1(suffixes[normal]) + extension;
                writePlane(outPath, *state.plane);
            }
        } else if (m_activeView != nullptr) {
            writePlane(filename, *m_activeView->plane);
        }
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
    if (!m_sequenceController->hasSequence()) {
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
    if (view == nullptr || !view->hasImage()
        || !m_sequenceController->hasSequence()) {
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
            m_sequenceController->frameCount(),
            m_sequenceController->currentIndex(), scale,
            std::move(suffixes), this)) {
        return;
    }

    // Freeze the action and stop playback while exporting.
    m_exportAnimationAction->setEnabled(false);
    setPlaybackMode(PlaybackMode::None);

    // forceRestart because the export drives itself off sequenceFrameDisplayed,
    // and an export started while frame 0 is already on screen would otherwise
    // be suppressed as a no-op and never receive the signal that advances it.
    goToSequenceFrame(0, true);
}

std::optional<DatasetRequest> MainWindow::buildDatasetRequest() const
{
    if (!m_dataset || m_activeView == nullptr
        || m_activeView->plane->width <= 0 || m_activeView->plane->height <= 0
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
    request.region = m_activeView->plane->physicalRegion;
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
    const auto& plane = *m_activeView->plane;
    if (plane.width <= 0 || plane.height <= 0) {
        return;
    }
    const auto axes = displayAxes(m_activeView->normal);
    const auto xAxis = static_cast<std::size_t>(axes[0]);
    const auto yAxis = static_cast<std::size_t>(axes[1]);
    if (displayIsSpherical()) {
        // xAxis is r, yAxis is theta.
        const double r0 = physicalCell.lower[xAxis];
        const double r1 = physicalCell.upper[xAxis];
        const double t0 = physicalCell.lower[yAxis];
        const double t1 = physicalCell.upper[yAxis];
        const auto mapping = planeMapping(*m_activeView);
        const bool valid = r1 > r0 && t1 > t0;
        // Branch on the view state's mode, matching the mapping (see
        // updateGridBoxes).
        if (m_activeView->sphericalDisplay == SphericalDisplay::RZ) {
            std::optional<QPainterPath> highlight;
            if (valid) {
                highlight = sphericalSectorPath(mapping, r0, r1, t0, t1);
            }
            m_activeView->view->setCellHighlightPath(highlight);
        } else {
            std::optional<QRectF> highlight;
            if (valid) {
                QRectF rect(mapping.sceneFromLogical(r0, t0),
                    mapping.sceneFromLogical(r1, t1));
                rect = rect.normalized();
                if (!rect.isEmpty()) {
                    highlight = rect;
                }
            }
            m_activeView->view->setCellHighlight(highlight);
        }
        return;
    }
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

void MainWindow::resetFabState()
{
    m_fabMode = false;
    m_multifabReturn.reset();
    m_fabSourceMetadata.reset();
    m_fabSourcePath.clear();
    m_fabDataRoot.clear();
    m_fabSelectorDock->setEntries({});
    m_fabSelectorDock->setBackAvailable(false);
    m_fabSelectorDock->setVisible(false);
}

void MainWindow::verifyRemoteEndpoint(
    std::string host, std::uint16_t port, std::string token)
{
    m_remoteVerifyStopSource.request_stop();
    m_remoteVerifyStopSource = StopSource{};
    const auto cancellation = m_remoteVerifyStopSource.get_token();
    const auto generation = ++m_remoteVerifyGeneration;
    statusBar()->showMessage(tr("Verifying connection to %1:%2...")
            .arg(QString::fromStdString(host))
            .arg(port));
    auto* watcher = new QFutureWatcher<RemoteVerifyOutcome>(this);
    connect(watcher, &QFutureWatcher<RemoteVerifyOutcome>::finished, this,
        [this, watcher, host, port, token, generation] {
            const auto outcome = watcher->result();
            watcher->deleteLater();
            if (m_closing || generation != m_remoteVerifyGeneration) {
                return;
            }
            // Skip a stale check whose endpoint the user has since changed.
            if (host != m_remoteHost || port != m_remotePort
                || token != m_remoteToken) {
                return;
            }
            if (outcome.ok) {
                statusBar()->showMessage(
                    tr("Connected to %1:%2 — session token accepted")
                        .arg(QString::fromStdString(host))
                        .arg(port));
            } else if (outcome.unauthorized) {
                statusBar()->showMessage(tr("Remote authentication failed"));
                QMessageBox::warning(this, tr("Remote authentication failed"),
                    tr("The server at %1:%2 rejected the session token.\n\n"
                       "Enter the token exactly as the server printed it after "
                       "\"TOKEN\" on its startup line. A new token is generated "
                       "each time the server starts.")
                        .arg(QString::fromStdString(host))
                        .arg(port));
            } else {
                statusBar()->showMessage(tr("Could not reach remote server"));
                reportBackgroundError(tr("Could not connect to %1:%2: %3")
                        .arg(QString::fromStdString(host))
                        .arg(port)
                        .arg(outcome.message));
            }
            updateDiagnostics();
        });
    watcher->setFuture(QtConcurrent::run(
        [host, port, token, cancellation]() -> RemoteVerifyOutcome {
            try {
                remote::Connection connection(host, port,
                    remote::ConnectionOptions{
                        .clientName = "AMReXplorer Qt",
                        .softwareVersion = AMREXPLORER_VERSION,
                        .sessionToken = token},
                    cancellation);
                connection.ping(cancellation);
                return {true, false, {}};
            } catch (const remote::RemoteError& error) {
                return {false,
                    error.code() == remote::ErrorCode::Unauthorized,
                    QString::fromUtf8(error.what())};
            } catch (const std::exception& error) {
                return {false, false, QString::fromUtf8(error.what())};
            }
        }));
}

void MainWindow::openRemoteDataset(std::string host, std::uint16_t port,
    std::string remotePath, std::string token)
{
    m_remoteHost = host;
    m_remotePort = port;
    m_remoteToken = token;
    const auto displayPath = std::filesystem::path(remotePath);
    openDatasetImpl(displayPath, false, std::nullopt, {}, false,
        std::nullopt,
        std::tuple{std::move(host), port, std::move(remotePath),
            std::move(token)});
}

void MainWindow::openDatasetImpl(const std::filesystem::path& path,
    bool metadataOnly,
    std::optional<PlotfileMetadataResult> preparedMetadata,
    std::filesystem::path dataRoot, bool preserveFabSelector,
    std::optional<FrameSliceSpec> initialSpec,
    std::optional<
        std::tuple<std::string, std::uint16_t, std::string, std::string>>
        remoteOpen)
{
    if (!preserveFabSelector) {
        resetFabState();
    }
    // Opening a single dataset ends any plotfile sequence and stops playback
    // of either animation mode.
    setPlaybackMode(PlaybackMode::None);
    closeSequence();
    resetRangeState();
    // Invalidate every in-flight per-view slice and reset the view states.
    for (auto* state : allViewStates()) {
        state->stopSource.request_stop();
        ++state->sliceGeneration;
        state->view->setPlaceholder(tr("Loading dataset..."));
        // Fresh empty snapshots — the pointers must stay non-null (see
        // PlaneViewState).
        state->plane = std::make_shared<const ScalarPlane>();
        state->contourPlane = std::make_shared<const ScalarPlane>();
        state->contourFinePlane = std::make_shared<const ScalarPlane>();
        state->contourFineFactor = 1;
        state->contourPolylines.clear();
        state->fieldName.clear();
        state->visibleRegion.reset();
        state->vectorSegments.clear();
        state->gridBoxes.clear();
        state->cachedRequest = {};
        state->hasCachedRequest = false;
        state->cachedMode = DisplayMode::Raster;
        state->cachedVectorUField = 0;
        state->cachedVectorVField = 0;
        state->cachedContourCount = 0;
    }
    m_initialStopSource.request_stop();
    m_linePlotStopSource.request_stop();
    m_particleStopSource.request_stop();
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
    setSlicePositionControlsVisible(false);
    m_animationPanel->setSweepVisible(false);
    m_levelMenu->setEnabled(false);
    m_contoursAction->setEnabled(false);
    m_particlesAction->setEnabled(false);
    m_datasetAction->setEnabled(false);
    m_exportAnimationAction->setEnabled(false);
    m_openMetadata.reset();
    m_fileVersion.clear();
    m_probeLines.clear();
    m_vectorUField = -1;
    m_vectorVField = -1;
    m_vectorWField = -1;
    m_particleSamples.clear();
    m_selectedParticleSpecies.clear();
    m_particleSelectionInitialized = false;
    m_particleLoading = false;
    m_particleProgress->setVisible(false);
    ++m_particleGeneration;
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

    auto* watcher = new QFutureWatcher<OpenedDataset>(this);
    connect(watcher, &QFutureWatcher<OpenedDataset>::finished, this,
        [this, watcher, generation, path, metadataOnly,
            dataRoot = std::move(dataRoot),
            initialSpec = std::move(initialSpec)]() mutable {
            --m_activeRequests;
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            try {
                auto result = watcher->result();
                if (generation == m_generation) {
                    showMetadata(result.metadata, path);
                    // The FAB selector contents were built off-thread with the
                    // metadata (when not preserving the existing selector);
                    // here we only apply them, no file I/O on the GUI thread.
                    if (result.fabSelector) {
                        auto& fab = *result.fabSelector;
                        if (fab.matched) {
                            m_fabMode = fab.fabMode;
                            if (fab.hasSourceMetadata) {
                                m_fabSourceMetadata = result.metadata;
                            } else {
                                m_fabSourceMetadata.reset();
                            }
                        }
                        if (fab.entries.empty()) {
                            m_fabSelectorDock->setVisible(false);
                        } else {
                            m_fabSourcePath = path;
                            m_fabDataRoot = fab.root;
                            m_fabSelectorDock->setEntries(std::move(fab.entries));
                            m_fabSelectorDock->setBackAvailable(false);
                            m_fabSelectorDock->setVisible(true);
                            m_fabSelectorDock->raise();
                            updateWindowTitle();
                        }
                    }
                    emit datasetOpenFinished(true);
                    if (!metadataOnly) {
                        auto root = std::move(dataRoot);
                        if (root.empty()) {
                            root = result.session
                                ? std::filesystem::path{"."}
                                : (std::filesystem::is_directory(path)
                                      ? path
                                      : path.parent_path());
                            if (root.empty()) {
                                root = ".";
                            }
                        }
                        requestInitialSlice(path, generation,
                            std::move(result.metadata), std::move(root),
                            std::move(initialSpec),
                            std::move(result.session));
                    }
                } else {
                    ++m_staleResults;
                }
            } catch (const std::exception& error) {
                if (generation == m_generation) {
                    if (remoteErrorCode(error)
                        == remote::ErrorCode::Unauthorized) {
                        QMessageBox::warning(this,
                            tr("Remote authentication failed"),
                            tr("The server at %1:%2 rejected the session "
                               "token.\n\nEnter the token exactly as the "
                               "server printed it after \"TOKEN\" on its "
                               "startup line, via File → Connect to "
                               "Remote Server… (or paste it as "
                               "HOST:PORT#TOKEN). A new token is generated "
                               "each time the server starts.")
                                .arg(QString::fromStdString(m_remoteHost))
                                .arg(m_remotePort));
                    } else {
                        reportBackgroundError(tr("Cannot open dataset: %1")
                                .arg(exceptionMessage(error)));
                    }
                    // The prior dataset was torn down before this attempt even
                    // began, so leaving "Loading dataset..." up would claim a
                    // load is still coming. Name what failed instead; the
                    // controls are already disabled from the teardown, so this
                    // is the whole of the settled failure state.
                    setAllViewPlaceholders(
                        tr("Could not open %1")
                            .arg(QString::fromStdString(path.string())));
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
            cancellation = metadataCancellation, preserveFabSelector,
            remoteOpen = std::move(remoteOpen)]() mutable {
        OpenedDataset opened;
        if (remoteOpen) {
            auto& [host, port, remotePath, token] = *remoteOpen;
            auto connection = std::make_shared<remote::Connection>(
                host, port, remote::ConnectionOptions{
                    .clientName = "AMReXplorer Qt",
                    .softwareVersion = AMREXPLORER_VERSION,
                    .sessionToken = token},
                cancellation);
            opened.session = remote::RemoteDatasetSession::open(
                std::move(connection), remotePath,
                initialCacheBudget(), cancellation);
            opened.metadata.metadata
                = std::make_shared<const DatasetMetadata>(
                    opened.session->metadata());
            opened.metadata.metrics
                = opened.session->metadataReadMetrics();
            opened.metadata.fileVersion
                = opened.session->fileVersion();
        } else {
            opened.metadata = preparedMetadata
                ? std::move(*preparedMetadata)
                : readDatasetMetadata(path, cancellation);
        }
        // Build the FAB selector entries here, off the GUI thread, so the
        // header scans / per-block preads it needs never freeze the event
        // loop. Skipped when the caller preserves the existing selector.
        if (!preserveFabSelector && !remoteOpen) {
            opened.fabSelector = buildFabSelector(opened.metadata, path);
        }
        return opened;
    }));
}

void MainWindow::requestInitialSlice(
    const std::filesystem::path& path, std::uint64_t generation,
    std::optional<PlotfileMetadataResult> preparedMetadata,
    std::filesystem::path dataRoot,
    std::optional<FrameSliceSpec> initialSpec,
    std::shared_ptr<DatasetSession> preparedSession)
{
    validateVectorMode();
    const auto& metadata = *m_openMetadata;
    m_viewDimension = metadata.dimension;
    // Make the correct page visible and synchronously activate its layout
    // before deriving remote viewport budgets. The 3-D grid is otherwise still
    // the hidden stacked page and reports construction-time placeholder sizes.
    m_stack->setCurrentIndex(m_viewDimension == 3 ? 1 : 0);
    if (auto* page = m_stack->currentWidget(); page != nullptr
        && page->layout() != nullptr) {
        page->layout()->activate();
    }
    const auto views = currentViews();
    // The XY view starts out as the active one in 3-D.
    setActiveView(m_viewDimension == 3
        ? m_planeViews[2] : m_view2d);
    // ...and takes keyboard focus, so the arrow-key pan works on a freshly
    // opened dataset rather than only after the view has been clicked. This is
    // the one place focus is taken: the other setActiveView callers run while
    // the user may be in a toolbar widget, and panning must not cost them their
    // place in it.
    if (m_activeView != nullptr) {
        m_activeView->view->setFocus(::Qt::OtherFocusReason);
    }
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
    m_particleStopSource.request_stop();
    m_initialStopSource = StopSource{};
    const auto cancellation = m_initialStopSource.get_token();
    // The initial open uses default slice state: field 0, finest available,
    // file range (falling back to Visible when metadata statistics are
    // unavailable), linear scale, whole domain, midpoint positions.
    FrameSliceSpec spec = initialSpec.value_or(FrameSliceSpec{});
    if (!initialSpec) {
        spec.palette = m_palette;
        spec.displayMode = m_displayMode;
        spec.includeGridBoxes = m_boxesAction->isChecked();
        spec.vectorUField =
            static_cast<std::uint32_t>(std::max(m_vectorUField, 0));
        spec.vectorVField =
            static_cast<std::uint32_t>(std::max(m_vectorVField, 0));
        spec.vectorWField =
            static_cast<std::uint32_t>(std::max(m_vectorWField, 0));
        spec.contourCount = m_contourCount;
        spec.sphericalSupersample = m_sphericalSupersample;
        spec.sphericalDisplay = m_sphericalDisplay;
    }
    const auto isRemote = std::dynamic_pointer_cast<
        remote::RemoteDatasetSession>(preparedSession) != nullptr;
    if (spec.outputSizes.size() != views.size()) {
        spec.outputSizes.clear();
        spec.outputSizes.reserve(views.size());
        for (const auto* state : views) {
            auto outputSize = sliceOutputSize(*state, isRemote);
            if (preparedSession) {
                outputSize = frameBudgetBoundedOutputSize(outputSize,
                    preparedSession->maximumResponseBytes());
            }
            spec.outputSizes.push_back(outputSize);
        }
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
            restoredSpec, isRemote] {
            --m_activeRequests;
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            try {
                auto result = watcher->result();
                if (generation == m_generation) {
                    m_dataset = result.dataset;
                    m_particleSamples = std::move(result.particles);
                    if (restoredSpec) {
                        m_selectedParticleSpecies
                            = restoredSpec->particleSpecies;
                        m_particleFraction = restoredSpec->particleFraction;
                        m_particleSeed = restoredSpec->particleSeed;
                        m_particleSelectionInitialized
                            = restoredSpec->particleSelectionInitialized;
                    }
                    configureParticleControls(restoredSpec.has_value());
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
                    if (isRemote) {
                        // A resize during the initial worker cannot submit a
                        // slice yet because m_dataset is not published. Once
                        // that result settles, coalesce each changed viewport
                        // into exactly one request using its newest size.
                        for (std::size_t index = 0; index < views.size(); ++index) {
                            if (result.displays[index].request.outputSize
                                != sliceOutputSize(*views[index])) {
                                scheduleSliceRequest(*views[index]);
                            }
                        }
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
                    // The metadata opened but its first slice did not, so the
                    // panels are still on the open's placeholder with nothing
                    // left in flight to replace it. The dataset name is known
                    // here, so say which one has no displayable slice.
                    setAllViewPlaceholders(
                        tr("Could not display %1")
                            .arg(QString::fromStdString(m_datasetPath.string())));
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
            dataRoot = std::move(dataRoot),
            preparedSession = std::move(preparedSession)]() mutable {
        if (preparedSession) {
            return executeSessionFrameLoad(
                std::move(preparedSession), spec, cancellation);
        }
        return executeFrameLoad(path, DatasetId{generation}, spec,
            initialCacheBudget(), cancellation,
            std::move(preparedMetadata), std::move(dataRoot));
    }));
}

void MainWindow::enableDatasetControls(const DatasetMetadata& metadata)
{
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

    enableDatasetControls(metadata);

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

void MainWindow::setSlicePositionControlsVisible(bool visible)
{
    m_slicePositionControls->setVisible(visible);
    if (m_positionSeparator != nullptr) {
        m_positionSeparator->setVisible(visible);
    }
}

void MainWindow::configureSlicePositionControls()
{
    if (!m_dataset) {
        setSlicePositionControlsVisible(false);
        return;
    }
    setSlicePositionControlsVisible(true);
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
    // The cached full-domain Visible range is now stale — and so is any
    // pending deferred store, whose union was computed from pre-move planes.
    m_displayCoordinator.invalidateRangeCache();
    m_pendingRangeStore.reset();
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
        m_sequenceController->invalidatePrefetch();
        // If a sequence frame is still loading, restart it so the in-flight
        // load is rebuilt from the new spec instead of finishing stale.
        if (m_sequenceController->inFlight()
            && m_sequenceController->currentIndex() >= 0) {
            goToSequenceFrame(m_sequenceController->currentIndex(), true);
            return;
        }
        m_pendingRasterDirty = m_pendingRasterDirty || rasterDirty;
        m_pendingAllViews = true;
        m_sliceDebounce->start();
    }
}

void MainWindow::scheduleSliceRequest(PlaneViewState& state, bool rasterDirty)
{
    if (m_controlsReady && m_dataset) {
        m_sequenceController->invalidatePrefetch();
        // If a sequence frame is still loading, restart it so the in-flight
        // load is rebuilt from the new spec instead of finishing stale.
        if (m_sequenceController->inFlight()
            && m_sequenceController->currentIndex() >= 0) {
            goToSequenceFrame(m_sequenceController->currentIndex(), true);
            return;
        }
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
    request.outputSize = sliceOutputSize(state);
    const auto level = m_levelSelector->currentData().toInt();
    const auto [composition, maximumLevel] = decodeLevelData(
        level, metadata.finestLevel);
    request.composition = composition;
    request.maximumLevel = maximumLevel;
    request.includeGridBoxes = m_boxesAction->isChecked();
    request.sphericalSupersample = m_sphericalSupersample;
    request.sphericalDisplay = m_sphericalDisplay;

    const auto requestedRangeMode = static_cast<RangeMode>(
        m_rangeMode->currentData().toInt());
    const auto rangeMode = effectiveRangeMode(dataset, request.field,
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
        && state.plane->width > 0
        && sameSliceSpec(state.cachedRequest, request)
        && state.cachedVectorVField == vectorVField
        && state.cachedVectorUField == vectorUField
        && displayMode == state.cachedMode
        && (!isContourMode(displayMode) || state.contourFinePlane->width > 0)
        && (displayMode != DisplayMode::VelocityVectors
            || (!state.vectorSegments.empty()
                && contourCount == state.cachedContourCount
                // Cached glyphs are layout-specific for a spherical dataset:
                // R-Z segments carry display (R, Z) coordinates while the
                // logical layouts carry plane pixels, so a display-mode switch
                // must regenerate them. (A supersample change is fine: R-Z
                // segments are resolution-independent physical coordinates.)
                && (!displayIsSpherical()
                    || (state.cachedRequest.sphericalDisplay
                            == request.sphericalDisplay)
                    || (state.cachedRequest.sphericalDisplay
                            != SphericalDisplay::RZ
                        && request.sphericalDisplay != SphericalDisplay::RZ))));

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
        // on a worker; no SliceQuery runs at all. The captures are shared_ptr
        // snapshots — refcount bumps, not the former ~110 MB plane deep copy
        // on the GUI thread. A newer arrival can safely replace the view's
        // pointers meanwhile; this worker keeps reading its own snapshots.
        // (refreshCachedSlice's by-value parameters still copy the planes,
        // but on the worker thread.)
        future = QtConcurrent::run([dataset, request,
            displayPlane = state.plane,
            contourPlane = state.contourPlane,
            contourFinePlane = state.contourFinePlane,
            contourFineFactor = state.contourFineFactor,
            vectors = state.vectorSegments,
            rangeMode, userRange, logarithmic, palette, displayMode,
            vectorUField, vectorVField, contourCount, rasterDirty,
            cancellation]() mutable {
            return refreshCachedSlice(dataset, request, *displayPlane,
                *contourPlane, *contourFinePlane,
                contourFineFactor, std::move(vectors), rangeMode, userRange,
                logarithmic, palette, displayMode, vectorUField, vectorVField,
                contourCount, rasterDirty, cancellation);
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
                    // Whether this slice covered the full domain must come from
                    // the request that produced it, not live view state:
                    // rubber-band zoom / pan / reset-zoom mutate
                    // state.visibleRegion without bumping sliceGeneration (they
                    // only schedule the debounced re-slice), so a slice in
                    // flight when one of those fires would be misclassified --
                    // caching a subregion range as the full-domain range on a
                    // reset, or dropping the full-domain range on a zoom
                    // (range-cache-staleness-races).
                    const bool isFullDomain = result.request.visibleRegion
                        == datasetSampleBounds(dataset->metadata());
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
                    // Cache the full-domain range. In 3-D the store defers to
                    // the (async) shared-range sync's completion so the union
                    // across all panels is captured; 2-D has no later sync
                    // and stores the panel's own range immediately.
                    if (isFullDomain && rangeMode == RangeMode::Visible
                        && state.plane->width > 0) {
                        if (m_viewDimension == 3) {
                            m_pendingRangeStore = rangeKey;
                        } else {
                            m_displayCoordinator.storeFullDomainRange(rangeKey,
                                {state.displayMinimum, state.displayMaximum});
                        }
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
        || state.plane->width <= 0 || state.plane->height <= 0) {
        state.view->setGridBoxes(overlays);
        return;
    }

    const auto& metadata = m_dataset->metadata();
    const auto& plane = *state.plane;
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
    const bool spherical = displayIsSpherical();
    const auto mapping = planeMapping(state);
    for (const auto& gridBox : state.gridBoxes) {
        const auto levelIndex = gridBox.level;
        if (levelIndex < firstLevel || levelIndex > lastLevel) {
            continue;
        }
        const auto& physicalBox = gridBox.physicalRegion;
        const auto xLower = physicalBox.lower[xAxis];
        const auto xUpper = physicalBox.upper[xAxis];
        const auto yLower = physicalBox.lower[yAxis];
        const auto yUpper = physicalBox.upper[yAxis];
        const auto color = levelIndex == firstLevel
            ? QColor(Qt::white)
            : QColor::fromRgb(static_cast<QRgb>(
                m_palette.levelColor(levelIndex, lastLevel)));
        if (spherical) {
            // xAxis is r, yAxis is theta. Branch on the state's mode (the
            // raster on screen), not m_sphericalDisplay (the menu
            // selection): between a mode change and the re-rendered
            // arrival the two disagree, and the overlay must match the
            // displayed raster.
            if (!(xUpper > xLower) || !(yUpper > yLower)) {
                continue;
            }
            if (state.sphericalDisplay == SphericalDisplay::RZ) {
                // Warped wedge: a curved annular sector.
                GridBoxOverlay overlay;
                overlay.color = color;
                overlay.path = sphericalSectorPath(
                    mapping, xLower, xUpper, yLower, yUpper);
                overlays.push_back(std::move(overlay));
            } else {
                // Logical r-theta / theta-r: an axis-aligned rectangle.
                QRectF rect(mapping.sceneFromLogical(xLower, yLower),
                    mapping.sceneFromLogical(xUpper, yUpper));
                rect = rect.normalized();
                if (!rect.isEmpty()) {
                    overlays.push_back({rect, color, QPainterPath{}});
                }
            }
            continue;
        }
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
            overlays.push_back({rectangle, color, QPainterPath{}});
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
        && state.plane->width > 0 && state.plane->height > 0) {
        const auto axes = displayAxes(state.normal);
        const auto xAxis = static_cast<std::size_t>(axes[0]);
        const auto yAxis = static_cast<std::size_t>(axes[1]);
        const auto& region = state.plane->physicalRegion;
        const auto width = static_cast<double>(state.plane->width);
        const auto height = static_cast<double>(state.plane->height);
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

    // Standalone FABs and MultiFabs carry neither a simulation time nor an
    // AMR hierarchy, so those rows (and the per-level listing below) would
    // show invented values; they are skipped for such data.
    const bool standalone = !metadata.hasPhysicalGeometry;
    addValue(tr("Dataset"), QString::fromStdString(path.string()));
    addValue(tr("Format"), QString::fromStdString(result.fileVersion));
    addValue(tr("Dimension"), QString::number(metadata.dimension));
    if (!standalone) {
        addValue(tr("Time"), QString::number(metadata.time, 'g', 17));
        addValue(tr("Finest level"), QString::number(metadata.finestLevel));
    }

    auto* fields = new QTreeWidgetItem(
        m_metadataTree, {tr("Fields"), QString::number(metadata.fields.size())});
    for (const auto& field : metadata.fields) {
        const char* centering = "cell";
        switch (field.centering) {
        case amrvis::Centering::Node: centering = "node"; break;
        case amrvis::Centering::FaceX: centering = "face-x"; break;
        case amrvis::Centering::FaceY: centering = "face-y"; break;
        case amrvis::Centering::FaceZ: centering = "face-z"; break;
        case amrvis::Centering::EdgeX: centering = "edge-x"; break;
        case amrvis::Centering::EdgeY: centering = "edge-y"; break;
        case amrvis::Centering::EdgeZ: centering = "edge-z"; break;
        case amrvis::Centering::Mixed: centering = "mixed"; break;
        case amrvis::Centering::Cell: break;
        }
        new QTreeWidgetItem(fields, {
            QString::fromStdString(field.name),
            QString::fromLatin1(centering)
        });
    }

    if (standalone) {
        const auto& level = metadata.levels.front();
        addValue(tr("Grids"), tr("%1 grid(s), %2").arg(level.boxes.size()).arg(
            QString::fromStdString(level.dataPath)));
    } else {
        auto* levels = new QTreeWidgetItem(m_metadataTree,
            {tr("Levels"), QString::number(metadata.levels.size())});
        for (const auto& level : metadata.levels) {
            new QTreeWidgetItem(levels, {
                tr("Level %1").arg(level.level),
                tr("%1 grid(s), %2").arg(level.boxes.size()).arg(
                    QString::fromStdString(level.dataPath))
            });
        }
    }
    m_metadataTree->expandAll();

    m_openMetadata = result.metadata;
    m_fileVersion = result.fileVersion;
    updateWindowTitle();

    m_lastFilesRead = result.metrics.filesRead;
    m_lastBytesRead = result.metrics.bytesRead;
    statusBar()->showMessage(standalone
        ? tr("Metadata loaded: %1 field(s), %2 grid(s)")
              .arg(metadata.fields.size())
              .arg(metadata.levels.front().boxes.size())
        : tr("Metadata loaded: %1 field(s), %2 level(s)")
              .arg(metadata.fields.size())
              .arg(metadata.levels.size()));
}

std::optional<QRectF> MainWindow::preservedDataWindow(
    const PlaneViewState& state, const ScalarPlane& incoming) const
{
    // Spherical scenes are warped (R, Z) while the plane geometry is logical
    // (r, theta); the linear viewport->physical->scene re-frame below does not
    // apply. Spherical never re-slices a subregion (zoom is view-only), so the
    // plain Preserve transform is already correct.
    if (displayIsSpherical()) {
        return std::nullopt;
    }
    const auto& cached = *state.plane;
    const auto axes = displayAxes(state.normal);
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

std::optional<QRectF> MainWindow::sphericalReframe(
    const PlaneViewState& state, const SliceDisplayResult& display) const
{
    // Only the R-Z warp has a resolution knob (supersampling); r-theta and
    // theta-r never resize in place, and a mode switch changes displayRegion
    // (so the plain refit below applies). Only once a raster is already on
    // screen (the first frame refits), and only when the user has actually
    // zoomed (a fit-to-window view should stay fit and keep auto-fitting).
    if (!displayIsSpherical()
        || display.sphericalDisplay != SphericalDisplay::RZ
        || state.sphericalDisplay != SphericalDisplay::RZ
        || !state.view->hasImage() || state.view->isFitToWindow()
        || !(display.displayRegion == state.displayRegion)) {
        return std::nullopt;
    }
    const auto oldSize = state.view->image().size();
    const QSize newSize(display.image.width, display.image.height);
    if (oldSize.width() <= 0 || oldSize.height() <= 0 || newSize == oldSize) {
        return std::nullopt;  // no resolution change (e.g. a field/range refresh)
    }
    // The physical bounds are unchanged, so the currently-visible physical
    // window occupies a scene rect scaled by the pixmap-resolution ratio.
    const auto visible = state.view->mapToScene(
        state.view->viewport()->rect()).boundingRect();
    const double sx = static_cast<double>(newSize.width())
        / static_cast<double>(oldSize.width());
    const double sy = static_cast<double>(newSize.height())
        / static_cast<double>(oldSize.height());
    return QRectF(visible.x() * sx, visible.y() * sy,
        visible.width() * sx, visible.height() * sy);
}

void MainWindow::showSlice(PlaneViewState& state, const SliceDisplayResult& display)
{
    if (!display.rasterUnchanged) {
        if (!display.image.valid()) {
            throw std::runtime_error("renderer produced an invalid image");
        }
        // A spherical supersample change keeps the same physical (R, Z) bounds
        // but resizes the warped pixmap. Keep what the user is looking at by
        // re-framing the visible window to the new resolution rather than
        // refitting to the whole sector (the GeometryAware size-change refit).
        if (const auto sphericalWindow = sphericalReframe(state, display)) {
            state.view->setImage(displayImageFor(display.image),
                ImageTransformPolicy::Preserve);
            state.view->zoomToRect(*sphericalWindow);
        } else {
            // Preserve/Refit/GeometryAware from the cached-vs-incoming request
            // pair; the rationale lives with the decision in the coordinator.
            const auto& metadata = m_dataset->metadata();
            const DisplayCoordinator::RasterGeometry incomingGeometry{
                metadata.physicalDomain, metadata.dimension,
                metadata.coordinateSystem, display.request.normalDirection,
                display.sphericalDisplay};
            const auto transformPolicy
                = DisplayCoordinator::rasterTransformPolicy(
                    state.rasterGeometry, incomingGeometry);
            // Preserve Custom mode by capturing the visible physical window
            // through the old plane and re-framing it through the new one.
            // This is required when pixel density changes (issue #45), and it
            // also protects same-density sequence replacements from scene or
            // scrollbar recentering while the pixmap item is replaced.
            std::optional<QRectF> dataWindowInNewScene;
            const auto axes = displayAxes(state.normal);
            const bool ownerChanged = state.hasCachedRequest
                && state.cachedRequest.dataset != display.request.dataset;
            const bool densityChanged = DisplayCoordinator::planeDensitiesDiffer(
                *state.plane, display.slice.plane, axes);
            if (transformPolicy == ImageTransformPolicy::Preserve
                && state.view->transformMode()
                    == ImageView::TransformMode::Custom
                && (ownerChanged || densityChanged)) {
                dataWindowInNewScene = preservedDataWindow(
                    state, display.slice.plane);
            }
            const auto image = displayImageFor(display.image);
            // A view on a virtual canvas keeps it: the raster lands at its
            // cell offset while the scroll position stays put.
            std::optional<ImageView::VirtualPlacement> placement;
            if (state.view->virtualCanvasActive() && !displayIsSpherical()) {
                placement = virtualPlacementFor(
                    state, display.slice.plane.physicalRegion);
            }
            state.view->setImage(image, transformPolicy,
                logicalImageSize(state, display.slice.plane, image),
                placement);
            if (dataWindowInNewScene) {
                state.view->zoomToRect(*dataWindowInNewScene);
            }
            state.rasterGeometry = incomingGeometry;
        }
    }
    // Fresh immutable snapshots: replace the pointers, never mutate the
    // pointees a cached-planes refresh worker may still be reading.
    state.plane = std::make_shared<const ScalarPlane>(display.slice.plane);
    // Spherical warps the raster into physical (R, Z); overlays and the probe
    // map through displayRegion, which for every other system is just the
    // plane's logical bounds (see PlaneMapping).
    state.coordinateSystem = display.coordinateSystem;
    state.sphericalDisplay = display.sphericalDisplay;
    state.displayRegion = display.displayRegion;
    state.contourPlane
        = std::make_shared<const ScalarPlane>(display.contourPlane);
    state.contourFinePlane
        = std::make_shared<const ScalarPlane>(display.contourFinePlane);
    state.contourFineFactor = display.contourFineFactor;
    state.contourPolylines = display.contourPolylines;
    const auto fieldName = QString::fromStdString(display.fieldName);
    state.fieldName = fieldName;
    state.displayMinimum = display.minimum;
    state.displayMaximum = display.maximum;
    state.displayLogarithmic = display.logarithmic;
    state.vectorSegments = display.vectors;
    if (display.slice.gridBoxesIncluded) {
        state.gridBoxes = display.slice.gridBoxes;
    }
    // Cache key for the re-render-from-cache path (see requestSlice).
    state.cachedRequest = display.request;
    state.hasCachedRequest = true;
    state.cachedMode = display.mode;
    state.cachedVectorUField = display.vectorUField;
    state.cachedVectorVField = display.vectorVField;
    state.cachedContourCount = display.contourCount;
    if (m_activeView == &state) {
        // Tracks the active view; if log was requested but fell back to linear,
        // the checkbox reflects that log did not apply.
        syncActiveViewColorControls(state);
    }
    // The straight-line profile tool works on the logical r-theta / theta-r
    // grid but not on the warped R-Z view.
    state.view->setLineToolEnabled(!displayIsSphericalWarp());
    // The 2-D Spherical menu (and, within it, Supersampling only in R-Z mode)
    // is available only for spherical datasets.
    updateSphericalControls();
    if (m_viewDimension == 2) {
        // The 2-D view carries no axis indicator normally; spherical labels its
        // horizontal/vertical axes per display mode (R-Z, r-theta, or theta-r).
        if (displayIsSpherical()) {
            const auto labels = sphericalAxisLabels(state.sphericalDisplay);
            state.view->setAxisIndicator(labels[0], labels[1]);
        } else {
            state.view->setAxisIndicator(QString(), QString());
        }
    }
    updateGridBoxes(state);
    updateOverlay(state);
    updateParticleOverlay(state);
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
    if (m_visibleSyncInFlight) {
        // Coalesce: rerun with fresh state once the in-flight worker lands
        // instead of stacking a worker per arrival.
        m_visibleSyncRerun = true;
        return;
    }

    // The coordinator resolves the shared range (the cached full-domain
    // range when current, so the color bar stays stable during zoom and pan;
    // else the union of the panels' finite extrema) and produces every
    // panel's raster and contours realigned to it. Only the cheap cached-
    // range lookup runs here — the coordinator stays confined to the GUI
    // thread; the heavy half (extrema scans, contour re-extraction, up to
    // three 16 Mpx renders, and the QImage flips) runs on a worker over the
    // panels' immutable plane snapshots.
    const FieldId currentField{m_fieldSelector->currentData().toUInt()};
    const auto rawLevel = m_levelSelector->currentData().toInt();
    const auto [composition, maximumLevel] = decodeLevelData(
        rawLevel, m_dataset->metadata().finestLevel);
    const auto cachedRange = m_displayCoordinator.cachedFullDomainRange(
        {m_dataset->id(), currentField, maximumLevel, composition});

    struct PanelSnapshot {
        std::shared_ptr<const ScalarPlane> plane;
        std::shared_ptr<const ScalarPlane> contourFinePlane;
        int contourFineFactor = 1;
        std::array<int, 2> outputSize{0, 0};
    };
    std::array<PlaneViewState*, 3> views{
        &m_planeViews[0], &m_planeViews[1], &m_planeViews[2]};
    std::array<PanelSnapshot, 3> snapshots;
    for (std::size_t index = 0; index < views.size(); ++index) {
        const auto* state = views[index];
        snapshots[index] = {state->plane, state->contourFinePlane,
            state->contourFineFactor, state->cachedRequest.outputSize};
    }

    struct SyncOutcome {
        std::optional<DisplayCoordinator::SharedRangeSync> sync;
        std::array<QImage, 3> images;   // display-ready (flipped) rasters
    };

    m_visibleSyncInFlight = true;
    // The sync participates in the interactive batch: the settled signal
    // (which the smoke tests use to read synchronized state) must not fire
    // until its results are applied.
    ++m_activeRequests;
    const auto generation = m_generation;
    auto* watcher = new QFutureWatcher<SyncOutcome>(this);
    connect(watcher, &QFutureWatcher<SyncOutcome>::finished, this,
        [this, watcher, generation, snapshots, views] {
            m_visibleSyncInFlight = false;
            --m_activeRequests;
            if (m_closing) {
                watcher->deleteLater();
                return;
            }
            auto outcome = watcher->result();
            const auto nowMode = static_cast<RangeMode>(
                m_rangeMode->currentData().toInt());
            const bool current = generation == m_generation
                && m_viewDimension == 3 && m_dataset
                && nowMode == RangeMode::Visible;
            if (current && outcome.sync) {
                const auto [globalMin, globalMax] = outcome.sync->range;
                bool activeApplied = false;
                for (std::size_t index = 0; index < views.size(); ++index) {
                    auto* state = views[index];
                    auto& update = outcome.sync->panels[index];
                    // Apply only while the snapshot this update was computed
                    // from is still the displayed plane (pointer identity);
                    // a superseded panel's own arrival re-syncs.
                    if (!update.applies
                        || state->plane != snapshots[index].plane) {
                        continue;
                    }
                    state->displayMinimum = globalMin;
                    state->displayMaximum = globalMax;
                    // One shared log flag across the panel set (see
                    // shared-log-range-render-throw-fails-load): keep every
                    // panel's stored flag, and thus the color bar below, in
                    // agreement with the raster the sync just rendered.
                    state->displayLogarithmic = outcome.sync->logarithmic;
                    if (update.contoursRecomputed) {
                        state->contourPolylines
                            = std::move(update.contourPolylines);
                    }
                    if (!outcome.images[index].isNull()) {
                        // Same plane, re-colored: a virtual canvas keeps its
                        // placement so the scroll position stays put.
                        std::optional<ImageView::VirtualPlacement> placement;
                        if (state->view->virtualCanvasActive()
                            && !displayIsSpherical()) {
                            placement = virtualPlacementFor(
                                *state, state->plane->physicalRegion);
                        }
                        state->view->setImage(outcome.images[index],
                            ImageTransformPolicy::GeometryAware,
                            logicalImageSize(*state, *state->plane,
                                outcome.images[index]),
                            placement);
                        // setImage clears the scene overlays; restore them.
                        updateGridBoxes(*state);
                        updateOverlay(*state);
                        updateParticleOverlay(*state);
                    }
                    activeApplied = activeApplied || state == m_activeView;
                }
                if (activeApplied && m_activeView->plane->width > 0) {
                    const auto fieldName = m_fieldSelector->currentText();
                    const auto label = m_activeView->displayLogarithmic
                        ? fieldName + tr(" (log)") : fieldName;
                    m_colorBar->setLogarithmic(
                        m_activeView->displayLogarithmic);
                    m_colorBar->setFieldRange(label, globalMin, globalMax);
                    const QSignalBlocker minBlocker(m_rangeMinimum);
                    const QSignalBlocker maxBlocker(m_rangeMaximum);
                    m_rangeMinimum->setValue(globalMin);
                    m_rangeMaximum->setValue(globalMax);
                }
                // The deferred full-domain range store (see the slice-arrival
                // completion): the union is only known here. When a rerun is
                // queued the union is about to be recomputed with newer
                // planes — leave the store for the rerun's completion.
                if (m_pendingRangeStore && !m_visibleSyncRerun) {
                    // Only store if the pending key still describes the current
                    // (dataset, field, level, composition): a full-domain
                    // arrival's key can outlive its own sync (e.g. its
                    // syncVisibleRanges early-returned because the mode was
                    // File), and this sync's union is for the *current* field,
                    // so storing it under the stale key would poison that
                    // field's cached range (range-cache-staleness-races).
                    const FieldId liveField{
                        m_fieldSelector->currentData().toUInt()};
                    const auto [liveComposition, liveMaximumLevel] =
                        decodeLevelData(m_levelSelector->currentData().toInt(),
                            m_dataset->metadata().finestLevel);
                    const DisplayCoordinator::RangeKey liveKey{
                        m_dataset->id(), liveField, liveMaximumLevel,
                        liveComposition};
                    if (*m_pendingRangeStore == liveKey) {
                        m_displayCoordinator.storeFullDomainRange(
                            *m_pendingRangeStore, outcome.sync->range);
                    }
                    m_pendingRangeStore.reset();
                }
            } else if (generation != m_generation) {
                m_pendingRangeStore.reset();
                m_visibleSyncRerun = false;
            }
            updateDiagnostics();
            watcher->deleteLater();
            if (m_visibleSyncRerun) {
                m_visibleSyncRerun = false;
                syncVisibleRanges();
            }
            if (m_activeRequests == 0) {
                emit interactiveSlicesSettled();
            }
        });
    watcher->setFuture(QtConcurrent::run([cachedRange, snapshots,
        logarithmic = m_logarithmic->isChecked(),
        contourMode = isContourMode(m_displayMode),
        contourCount = m_contourCount, palette = m_palette] {
        std::array<DisplayCoordinator::PanelSyncInput, 3> inputs;
        for (std::size_t index = 0; index < snapshots.size(); ++index) {
            const auto& snapshot = snapshots[index];
            inputs[index] = {snapshot.plane.get(),
                snapshot.contourFinePlane.get(), snapshot.contourFineFactor,
                snapshot.outputSize};
        }
        SyncOutcome outcome;
        outcome.sync = DisplayCoordinator::renderPanelsToSharedRange(
            cachedRange, inputs, logarithmic, contourMode, contourCount,
            palette);
        if (outcome.sync) {
            for (std::size_t index = 0; index < inputs.size(); ++index) {
                const auto& image = outcome.sync->panels[index].image;
                if (image.valid() && image.width > 0) {
                    outcome.images[index] = displayImageFor(image);
                }
            }
        }
        return outcome;
    }));
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
            tr("Select two or more plotfile directories, each containing a "
               "Header."));
        return;
    }

    prepareSequence(sorted.size());
    m_sequenceController->open(std::move(sorted));
}

void MainWindow::prepareSequence(std::size_t frameCount)
{
    // A sequence replaces any standalone FAB/MultiFab with plotfile frames.
    // Both local and remote entry points bypass openDatasetImpl, so establish
    // their complete state transition in one place after validation.
    setPlaybackMode(PlaybackMode::None);
    closeSequence();
    resetRangeState();
    resetFabState();
    m_particleStopSource.request_stop();
    m_particleSamples.clear();
    m_selectedParticleSpecies.clear();
    m_particleSelectionInitialized = false;
    m_particleLoading = false;
    m_particleProgress->setVisible(false);
    ++m_particleGeneration;
    m_remoteSequenceConnectionGeneration = 0;

    m_animationPanel->setSequenceFrameCount(static_cast<int>(frameCount));
    m_animationPanel->setSequenceVisible(true);
    updateAnimationDockVisibility();
    // Line plot curves are snapshots of the previous dataset; drop the window.
    auto* linePlotWindow = m_linePlotWindow;
    m_linePlotWindow = nullptr;
    if (linePlotWindow != nullptr) {
        linePlotWindow->close();
    }
}

void MainWindow::openRemoteSequence(std::string host, std::uint16_t port,
    const std::vector<std::string>& remotePaths, std::string token)
{
    if (remotePaths.size() < 2
        || std::any_of(remotePaths.begin(), remotePaths.end(),
            [](const auto& path) { return path.empty(); })) {
        emit sequenceFrameFailed();
        QMessageBox::warning(this, tr("Cannot open remote sequence"),
            tr("Enter two or more plotfile paths as they appear on %1:%2. "
               "Remote frames are named by their path on the server, not "
               "chosen from a local file dialog.")
                .arg(QString::fromStdString(host))
                .arg(port));
        return;
    }

    m_remoteHost = host;
    m_remotePort = port;
    m_remoteToken = token;
    prepareSequence(remotePaths.size());
    m_remoteSequence = true;

    std::vector<std::filesystem::path> frames;
    frames.reserve(remotePaths.size());
    for (const auto& path : remotePaths) {
        frames.emplace_back(path);
    }
    struct SharedRemoteConnection {
        std::mutex mutex;
        std::shared_ptr<remote::Connection> connection;
        std::uint64_t generation = 0;
    };
    auto shared = std::make_shared<SharedRemoteConnection>();
    auto loader = [shared, host = std::move(host), port,
                      token = std::move(token)](
                      const std::filesystem::path& path, DatasetId,
                      const FrameSliceSpec& spec, StopToken cancellation) {
        std::shared_ptr<remote::Connection> connection;
        std::uint64_t connectionGeneration = 0;
        {
            std::scoped_lock lock(shared->mutex);
            connection = shared->connection;
            connectionGeneration = shared->generation;
        }
        if (!connection || !connection->connected()) {
            // Connect outside the shared-state mutex. Foreground loads and
            // prefetches may overlap, and neither should inherit the other's
            // network wait or lose its own cancellation deadline.
            auto candidate = std::make_shared<remote::Connection>(host, port,
                remote::ConnectionOptions{
                    .clientName = "AMReXplorer Qt sequence",
                    .softwareVersion = AMREXPLORER_VERSION,
                    .sessionToken = token},
                cancellation);
            {
                std::scoped_lock lock(shared->mutex);
                if (!shared->connection || !shared->connection->connected()) {
                    shared->connection = std::move(candidate);
                    ++shared->generation;
                }
                connection = shared->connection;
                connectionGeneration = shared->generation;
            }
            // If another load won the connection race, release this redundant
            // connection only after dropping the shared-state mutex.
            candidate.reset();
        }
        auto session = remote::RemoteDatasetSession::open(
            std::move(connection), path.string(), initialCacheBudget(),
            cancellation);
        auto result = executeSessionFrameLoad(
            std::move(session), spec, cancellation);
        result.connectionGeneration = connectionGeneration;
        return result;
    };
    m_sequenceController->open(std::move(frames), std::move(loader));
}

void MainWindow::closeSequence()
{
    if (m_playbackMode == PlaybackMode::Sequence) {
        setPlaybackMode(PlaybackMode::None);
    }
    m_sequenceController->close();
    m_remoteSequence = false;
    m_animationPanel->setSequenceVisible(false);
    updateAnimationDockVisibility();
}

void MainWindow::updateAnimationDockVisibility()
{
    // The Animation panel hosts the 3-D slice-sweep controls and the
    // plotfile-sequence controls. Keep it visible only when one of those
    // applies; otherwise it is dead space.
    const auto sequenceActive = m_sequenceController->hasSequence();
    const auto threeD = m_dataset != nullptr
        && m_dataset->metadata().dimension == 3;
    const auto applies = sequenceActive || threeD;
    // Only act on a transition. This runs again for every sequence frame, by
    // way of configureSequenceControls, and forcing the dock visible there
    // reopened it after the user hid it mid-playback. Whether the panel applies
    // at all is ours to decide; whether it is shown while it applies is theirs.
    if (applies == m_animationDockApplies) {
        return;
    }
    m_animationDockApplies = applies;
    m_animationDock->setVisible(applies);
}

void MainWindow::stepSequence(int direction)
{
    m_sequenceController->step(direction);
}

void MainWindow::goToSequenceFrame(int index, bool forceRestart)
{
    m_sequenceController->goToFrame(index, forceRestart);
}

void MainWindow::displayFrameResult(InitialSliceResult& result,
    bool defaultPositions)
{
    if (result.connectionGeneration != 0
        && result.connectionGeneration
            != m_remoteSequenceConnectionGeneration) {
        // DatasetId is allocated by the server and can restart at one after a
        // reconnect. Drop every dataset-scoped display range before publishing
        // a frame from a new connection generation so an old ID cannot alias.
        m_displayCoordinator.invalidateRangeCache();
        m_pendingRangeStore.reset();
        m_remoteSequenceConnectionGeneration = result.connectionGeneration;
    }
    m_dataset = result.dataset;
    m_particleSamples = std::move(result.particles);
    configureParticleControls(true);
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

    enableDatasetControls(metadata);
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
    m_pendingRangeStore.reset();
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
    const auto fileAvailable = m_dataset->rangeAvailable(
        RangeRequest{field, maximumLevel, composition, RangeScope::File});
    const auto levelAvailable = m_dataset->rangeAvailable(
        RangeRequest{field, maximumLevel, composition, RangeScope::Level});

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
    spec.sphericalSupersample = m_sphericalSupersample;
    spec.sphericalDisplay = m_sphericalDisplay;
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
    spec.particleSelectionInitialized = m_particleSelectionInitialized;
    if (m_particleSelectionInitialized) {
        spec.particleSpecies = m_selectedParticleSpecies;
    }
    spec.particleFraction = m_particleFraction;
    spec.particleSeed = m_particleSeed;
    spec.includeGridBoxes = m_boxesAction->isChecked();
    const auto views = currentViews();
    spec.visibleRegions.reserve(views.size());
    if (m_remoteSequence) {
        spec.outputSizesAreViewportBounds = true;
        spec.outputSizes.reserve(views.size());
    }
    for (const auto* state : views) {
        spec.visibleRegions.push_back(state->visibleRegion);
        if (m_remoteSequence) {
            spec.outputSizes.push_back(viewportPixelSize(*state));
        }
    }
    return spec;
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
    if (m_sequenceController->frameCount() < 2) {
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
        if (m_sequenceController->frameCount() < 2) {
            setPlaybackMode(PlaybackMode::None);
            return;
        }
        // Skip the tick while the previous frame is still loading.
        if (m_sequenceController->inFlight()) {
            return;
        }
        m_sequenceController->step(1);
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
            .arg(m_sequenceController->lastFrameSwitchMs());
    if (m_remotePort != 0) {
        text += tr("\nremote endpoint: %1:%2")
                    .arg(QString::fromStdString(m_remoteHost))
                    .arg(m_remotePort);
        if (auto remoteSession = std::dynamic_pointer_cast<
                remote::RemoteDatasetSession>(m_dataset)) {
            text += tr("\nremote status: %1\nremote path: %2")
                        .arg(remoteSession->connection()->connected()
                                ? tr("connected") : tr("disconnected"),
                            QString::fromStdString(
                                remoteSession->remotePath()));
        } else {
            text += tr("\nremote status: configured");
        }
    }
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
