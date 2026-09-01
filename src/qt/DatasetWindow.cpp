#include "DatasetWindow.hpp"

#include <amrexplorer/pipeline/SlicePipeline.hpp>
#include "CloseWindowAction.hpp"
#include "NumberFormat.hpp"
#include "QtErrorText.hpp"
#include "Theme.hpp"

#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <QAbstractTableModel>
#include <QCloseEvent>
#include <QColor>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QLabel>
#include <QModelIndex>
#include <QPalette>
#include <QPushButton>
#include <QTabWidget>
#include <QTableView>
#include <QVariant>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace amrvis::qt {
namespace {

int datasetPageMaximumExtent(const DatasetSession& dataset)
{
    const auto maximumResponseBytes = dataset.maximumResponseBytes();
    if (!maximumResponseBytes) {
        return datasetPageMaxExtent;
    }
    // Dataset pages carry the same per-cell float value and byte coverage
    // vectors as slice responses. Reserve the shared envelope allowance, then
    // choose the largest square page that fits the negotiated frame.
    const auto frameBytes
        = static_cast<std::uint64_t>(*maximumResponseBytes);
    const auto maximumCells = frameBytes > sliceResponseOverheadBytes
        ? (frameBytes - sliceResponseOverheadBytes)
            / sliceResponseBytesPerCell
        : 0;
    const auto extent = maximumCells == 0
        ? 1
        : static_cast<int>(std::floor(
            std::sqrt(static_cast<double>(maximumCells))));
    return std::clamp(extent, 1, datasetPageMaxExtent);
}

// Presents one level's already-dense DatasetLevelExtract to a QTableView.
// Cells are produced lazily per visible index instead of materializing a
// QTableWidgetItem (plus QString) for every one of up to 512x512 samples and
// running resizeColumnsToContents over all of them, which froze the GUI for
// seconds and allocated hundreds of MB on a full-domain multi-level dataset.
// The model holds the extract by reference; it lives no longer than the tab
// page it is parented to, and the owning DatasetWindow rebuilds all tabs
// (destroying every model) before or while the backing m_levels changes.
class LevelTableModel final : public QAbstractTableModel {
public:
    LevelTableModel(const DatasetLevelExtract& extract,
        const DatasetColoring& coloring, QString format, QObject* parent)
        : QAbstractTableModel(parent)
        , m_extract(extract)
        , m_coloring(coloring)
        , m_format(std::move(format))
    {
    }

    int rowCount(const QModelIndex& parent) const override
    {
        return parent.isValid() ? 0 : m_extract.ny;
    }

    int columnCount(const QModelIndex& parent) const override
    {
        return parent.isValid() ? 0 : m_extract.nx;
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_extract.ny
            || index.column() < 0 || index.column() >= m_extract.nx) {
            return {};
        }
        const auto offset = cellOffset(index.row(), index.column());
        const bool covered = m_extract.covered[offset] != 0;
        switch (role) {
        case Qt::DisplayRole:
            return covered
                ? formatNumber(
                      static_cast<double>(m_extract.values[offset]), m_format)
                : QString();
        case Qt::TextAlignmentRole:
            return covered
                ? QVariant(static_cast<int>(Qt::AlignRight | Qt::AlignVCenter))
                : QVariant();
        case Qt::ForegroundRole:
            // The color the color bar gives this value, which is the color the
            // image draws the sample in.
            return covered
                ? QVariant(datasetValueColor(m_coloring,
                      static_cast<double>(m_extract.values[offset])))
                : QVariant();
        case Qt::BackgroundRole:
            // Cells no grid covers at this level are shaded, as before -- in
            // the renderer's own no-data color now that the covered cells sit
            // on the viewport background, which the old dark gray matched
            // almost exactly.
            return covered
                ? QVariant() : QVariant(datasetUncoveredBackground());
        default:
            return {};
        }
    }

    QVariant headerData(
        int section, Qt::Orientation orientation, int role) const override
    {
        if (role != Qt::DisplayRole || section < 0) {
            return {};
        }
        if (orientation == Qt::Horizontal) {
            return QString::number(m_extract.lower[0] + section);
        }
        // Row 0 shows the highest j, matching the image and the legacy window.
        return QString::number(m_extract.upper[1] - section);
    }

private:
    // Row 0 is the highest j; map (row, column) back to the value array, which
    // runs the first in-plane axis fastest with j ascending.
    [[nodiscard]] std::size_t cellOffset(int row, int column) const noexcept
    {
        const auto valueRow = static_cast<std::size_t>(m_extract.ny - 1 - row);
        return static_cast<std::size_t>(column)
            + static_cast<std::size_t>(m_extract.nx) * valueRow;
    }

    const DatasetLevelExtract& m_extract;
    // Held by reference like the extract above, and on the same terms: it is a
    // member of the owning DatasetWindow, which outlives every model it builds.
    const DatasetColoring& m_coloring;
    QString m_format;
};

} // namespace

DatasetWindow::DatasetWindow(DatasetRequest request, QWidget* parent)
    : QWidget(parent)
    , m_request(std::move(request))
    , m_numberFormat(defaultNumberFormat())
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Dataset — %1").arg(m_request.fieldName));
    resize(640, 480);

    m_status = new QLabel(this);
    m_tabs = new QTabWidget(this);
    auto* refreshButton = new QPushButton(tr("Refresh"), this);
    auto* closeButton = new QPushButton(tr("Close"), this);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(refreshButton);
    buttons->addWidget(closeButton);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_status);
    layout->addWidget(m_tabs, 1);
    layout->addLayout(buttons);

    connect(refreshButton, &QPushButton::clicked, this,
        [this] { emit refreshRequested(); });
    connect(closeButton, &QPushButton::clicked, this, &QWidget::close);
    // The same key that closes the other windows. There is no menu bar here to
    // show it in, so the action lives on the window itself, next to the button.
    addCloseWindowAction(*this, tr("&Close"))
        ->setObjectName(QStringLiteral("datasetCloseAction"));

    startLoad();
}

DatasetWindow::~DatasetWindow()
{
    m_stopSource.request_stop();
}

void DatasetWindow::closeEvent(QCloseEvent* event)
{
    m_stopSource.request_stop();
    QWidget::closeEvent(event);
}

void DatasetWindow::reload(DatasetRequest request)
{
    m_request = std::move(request);
    setWindowTitle(tr("Dataset — %1").arg(m_request.fieldName));
    startLoad();
}

void DatasetWindow::setNumberFormat(QString format)
{
    m_numberFormat = std::move(format);
    // The loaded values are still on hand; re-rendering the tabs is cheap
    // compared to re-reading the dataset.
    if (!m_levels.empty()) {
        populateTabs();
    }
}

void DatasetWindow::setColoring(DatasetColoring coloring)
{
    m_coloring = std::move(coloring);
    // Not populateTabs: the models read the coloring by reference and it has
    // just moved under them, so all that is owed is a repaint. Rebuilding the
    // tabs would throw away the current tab and every scroll position, and the
    // palette can change under an open window at any time.
    //
    // A repaint is enough because the views hold nothing: QTableView asks the
    // model for each visible cell as it paints it.
    for (int page = 0; page < m_tabs->count(); ++page) {
        if (auto* table = m_tabs->widget(page)->findChild<QTableView*>()) {
            table->viewport()->update();
        }
    }
}

std::vector<DatasetWindow::LevelData> DatasetWindow::extractLevels(
    const DatasetRequest& request, StopToken cancellation)
{
    if (!request.dataset) {
        throw std::runtime_error("dataset window opened without a dataset");
    }
    const auto& metadata = request.dataset->metadata();
    std::vector<LevelData> levels;
    for (int level = 0; level <= metadata.finestLevel; ++level) {
        if (cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        DatasetPageRequest pageRequest;
        pageRequest.dataset = request.dataset->id();
        pageRequest.field = request.field;
        pageRequest.level = level;
        pageRequest.region = request.region;
        pageRequest.normalAxis = request.normalAxis;
        pageRequest.slicePosition = request.slicePosition;
        pageRequest.maximumExtent
            = datasetPageMaximumExtent(*request.dataset);
        auto extract = request.dataset->requestDatasetPage(
            pageRequest, cancellation);
        // Levels the region misses geometrically get no tab.
        if (extract.nx > 0 && extract.ny > 0) {
            levels.push_back(LevelData{level, std::move(extract)});
        }
    }
    return levels;
}

void DatasetWindow::startLoad()
{
    m_stopSource.request_stop();
    m_stopSource = StopSource{};
    const auto cancellation = m_stopSource.get_token();
    const auto generation = ++m_generation;
    m_status->setText(tr("Loading %1...").arg(m_request.fieldName));

    const auto request = m_request;
    auto* watcher = new QFutureWatcher<std::vector<LevelData>>(this);
    connect(watcher, &QFutureWatcher<std::vector<LevelData>>::finished, this,
        [this, watcher, generation] {
            if (generation != m_generation) {
                watcher->deleteLater();
                return;
            }
            try {
                // Retrieve first, and only then take the old extracts out of
                // the way, destroying them once the models have been rebuilt
                // off the new ones. Assigning straight into m_levels destroys
                // the old extracts while the old LevelTableModels still hold
                // `const DatasetLevelExtract&` into them; nothing dispatches in
                // between today, so nothing observes it, but the window is one
                // queued call away from doing so.
                //
                // The retrieval has to come first because it throws: a worker
                // failure with the old extracts already moved into a local here
                // would destroy them during unwinding, before the handler below
                // runs and without populateTabs having rebuilt anything, so
                // every existing model would be left dangling until the
                // window's deferred delete. Leaving m_levels alone until the
                // result is in hand keeps the failure path exactly as it was.
                auto next = watcher->future().takeResult();
                auto previous = std::exchange(m_levels, std::move(next));
                populateTabs();
                previous.clear();
                m_status->setText(tr("Field: %1").arg(m_request.fieldName));
            } catch (const std::exception& error) {
                // Report a real failure non-modally through the owner, then
                // close; a cancelled read (close/refresh) stays silent. A modal
                // dialog here would disable the whole app and block quitting.
                if (!m_stopSource.stop_requested()) {
                    emit extractionFailed(tr("Cannot load dataset values: %1")
                        .arg(exceptionMessage(error)));
                    close();
                }
            }
            watcher->deleteLater();
        });
    watcher->setFuture(QtConcurrent::run(
        [request, cancellation] { return extractLevels(request, cancellation); }));
}

void DatasetWindow::populateTabs()
{
    while (m_tabs->count() > 0) {
        auto* page = m_tabs->widget(0);
        m_tabs->removeTab(0);
        delete page;
    }
    for (std::size_t entry = 0; entry < m_levels.size(); ++entry) {
        const auto& levelData = m_levels[entry];
        const auto& extract = levelData.extract;

        auto* page = new QWidget(m_tabs);
        auto* info = new QLabel(page);
        if (extract.hasFiniteValues) {
            info->setText(tr("min=%1 max=%2  (%3 x %4 samples)")
                .arg(formatNumber(extract.minimum, m_numberFormat))
                .arg(formatNumber(extract.maximum, m_numberFormat))
                .arg(extract.nx)
                .arg(extract.ny));
        } else {
            info->setText(tr("no finite values  (%1 x %2 samples)")
                .arg(extract.nx)
                .arg(extract.ny));
        }
        // A model/view over the dense extract: the view realizes only the
        // visible cells, so a full-domain table no longer freezes the GUI.
        auto* table = new QTableView(page);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        // The values are drawn in their color-bar colors, so the cells they sit
        // on are the viewport's background rather than the theme's -- which is
        // white under a light theme, where the bright end of viridis, plasma or
        // blackbody would be all but invisible. Through the view's palette so
        // the empty area past the last row and column matches the cells.
        auto viewPalette = table->palette();
        viewPalette.setColor(QPalette::Base, viewportBackground());
        table->setPalette(viewPalette);
        auto* model
            = new LevelTableModel(extract, m_coloring, m_numberFormat, table);
        table->setModel(model);
        auto* pageLayout = new QVBoxLayout(page);
        pageLayout->addWidget(info);
        pageLayout->addWidget(table, 1);
        // Standalone MultiFabs and FABs have no AMR hierarchy, so a
        // "Level 0" tab would suggest a concept those formats lack; their
        // single tab is named after the format instead.
        const auto& metadata = m_request.dataset->metadata();
        auto label = metadata.hasPhysicalGeometry
            ? tr("Level %1").arg(levelData.level)
            : metadata.isFab ? tr("FAB") : tr("MultiFab");
        if (extract.truncatedX || extract.truncatedY) {
            label += tr(" (truncated)");
        }
        m_tabs->addTab(page, label);
        // Driven off the selection rather than QTableView::clicked, which
        // fires only when the press and the release land on the same cell and
        // so says nothing about a drag. One handler then covers the click, the
        // drag and the keyboard (shift-arrow) alike, because each of them is
        // just a selection.
        connect(table->selectionModel(),
            &QItemSelectionModel::selectionChanged, this,
            [this, entry, table] { selectionChanged(entry, *table); });
    }
}

void DatasetWindow::selectionChanged(
    std::size_t levelEntry, const QTableView& table)
{
    const auto* selection = table.selectionModel();
    if (selection == nullptr || !selection->hasSelection()) {
        // Nothing selected here, so there is nothing to mark -- and this is
        // also what the tables cleared below report, which is what keeps
        // clearOtherSelections from coming back round through their handlers.
        // A cleared selection leaves the last highlight up, as a click on an
        // uncovered cell always has.
        return;
    }
    clearOtherSelections(table);
    const auto selected = selection->selection();
    std::vector<CellRange> ranges;
    ranges.reserve(static_cast<std::size_t>(selected.size()));
    for (const auto& range : selected) {
        ranges.push_back(CellRange{range.top(), range.left(), range.bottom(),
            range.right()});
    }
    cellsSelected(levelEntry, ranges);
}

void DatasetWindow::clearOtherSelections(const QTableView& keep)
{
    for (int page = 0; page < m_tabs->count(); ++page) {
        auto* table = m_tabs->widget(page)->findChild<QTableView*>();
        if (table == nullptr || table == &keep
            || table->selectionModel() == nullptr) {
            continue;
        }
        table->clearSelection();
    }
}

void DatasetWindow::cellsSelected(
    std::size_t levelEntry, std::span<const CellRange> ranges)
{
    if (levelEntry >= m_levels.size() || !m_request.dataset) {
        return;
    }
    const auto& levelData = m_levels[levelEntry];
    const auto bounds = coveredSelectionBounds(levelData.extract, ranges);
    if (!bounds) {
        return;
    }

    const auto& metadata = m_request.dataset->metadata();
    const auto& level = metadata.levels[static_cast<std::size_t>(levelData.level)];
    const auto axes = slicePlaneAxes(
        metadata.dimension, m_request.normalAxis);
    // The samples' physical bins at this level's resolution. On nodal axes
    // these are centered on the nodes rather than shifted to the next cell.
    const std::array<int, 2> low{bounds->iLow, bounds->jLow};
    const std::array<int, 2> high{bounds->iHigh, bounds->jHigh};
    auto sampleBox = level.domain;
    for (std::size_t entry = 0; entry < 2; ++entry) {
        const auto axis = static_cast<std::size_t>(axes[entry]);
        sampleBox.lower[axis] = low[entry];
        sampleBox.upper[axis] = high[entry];
    }
    if (metadata.dimension == 3) {
        const auto normal = static_cast<std::size_t>(m_request.normalAxis);
        sampleBox.lower[normal] = levelData.extract.sliceIndex;
        sampleBox.upper[normal] = levelData.extract.sliceIndex;
    }
    emit cellActivated(sampleBounds(level, sampleBox, metadata.dimension));
}

} // namespace amrvis::qt
