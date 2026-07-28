#include "DatasetWindow.hpp"
#include "NumberFormat.hpp"

#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <QAbstractTableModel>
#include <QCloseEvent>
#include <QColor>
#include <QException>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QModelIndex>
#include <QPushButton>
#include <QTabWidget>
#include <QTableView>
#include <QVariant>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace amrvis::qt {
namespace {

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
    LevelTableModel(const DatasetLevelExtract& extract, QString format,
        QObject* parent)
        : QAbstractTableModel(parent)
        , m_extract(extract)
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
        case Qt::BackgroundRole:
            // Cells no grid covers at this level are shaded, as before.
            return covered ? QVariant() : QVariant(QColor(Qt::darkGray));
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
        auto extract = extractDatasetLevel(*request.dataset, request.field,
            level, request.region, request.normalAxis, request.slicePosition,
            datasetExtractMaxExtent, cancellation);
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
                m_levels = watcher->result();
                populateTabs();
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
        auto* model = new LevelTableModel(extract, m_numberFormat, table);
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
        connect(table, &QTableView::clicked, this,
            [this, entry](const QModelIndex& index) {
                cellClicked(entry, index.row(), index.column());
            });
    }
}

void DatasetWindow::cellClicked(std::size_t levelEntry, int row, int column)
{
    if (levelEntry >= m_levels.size() || !m_request.dataset) {
        return;
    }
    const auto& levelData = m_levels[levelEntry];
    const auto& extract = levelData.extract;
    if (column < 0 || column >= extract.nx || row < 0 || row >= extract.ny) {
        return;
    }
    const auto j = extract.upper[1] - row;
    const auto offset = static_cast<std::size_t>(column)
        + static_cast<std::size_t>(extract.nx) * static_cast<std::size_t>(
            static_cast<std::int64_t>(j) - extract.lower[1]);
    if (extract.covered[offset] == 0) {
        return;
    }

    const auto& metadata = m_request.dataset->metadata();
    const auto& level = metadata.levels[static_cast<std::size_t>(levelData.level)];
    const auto axes = dataset_extract_detail::inPlaneAxes(
        metadata.dimension, m_request.normalAxis);
    // The sample's physical bin at this level's resolution. On nodal axes
    // this is centered on the node rather than shifted to the next cell.
    const std::array<int, 2> sample{extract.lower[0] + column, j};
    auto pointBox = level.domain;
    for (std::size_t entry = 0; entry < 2; ++entry) {
        const auto axis = static_cast<std::size_t>(axes[entry]);
        pointBox.lower[axis] = sample[entry];
        pointBox.upper[axis] = sample[entry];
    }
    if (metadata.dimension == 3) {
        const auto normal = static_cast<std::size_t>(m_request.normalAxis);
        pointBox.lower[normal] = extract.sliceIndex;
        pointBox.upper[normal] = extract.sliceIndex;
    }
    emit cellActivated(sampleBounds(level, pointBox, metadata.dimension));
}

} // namespace amrvis::qt
