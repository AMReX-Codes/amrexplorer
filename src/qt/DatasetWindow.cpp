#include "DatasetWindow.hpp"
#include "NumberFormat.hpp"

#include <amrexplorer/io/PlotfileBlockReader.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <QCloseEvent>
#include <QColor>
#include <QException>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <cstdint>
#include <exception>
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
                // One message for a real failure, then the window closes; a
                // cancelled read (close/refresh) stays silent.
                if (!m_stopSource.stop_requested()) {
                    QMessageBox::critical(this,
                        tr("Cannot load dataset values"),
                        exceptionMessage(error));
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
        auto* table = new QTableWidget(extract.ny, extract.nx, page);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        QStringList columnLabels;
        for (int column = 0; column < extract.nx; ++column) {
            columnLabels << QString::number(extract.lower[0] + column);
        }
        table->setHorizontalHeaderLabels(columnLabels);
        QStringList rowLabels;
        for (int row = 0; row < extract.ny; ++row) {
            // Row 0 shows the highest j, matching the image and the legacy
            // dataset window.
            rowLabels << QString::number(extract.upper[1] - row);
        }
        table->setVerticalHeaderLabels(rowLabels);
        for (int row = 0; row < extract.ny; ++row) {
            const auto valueRow = static_cast<std::size_t>(
                static_cast<std::int64_t>(extract.upper[1] - row)
                - extract.lower[1]);
            for (int column = 0; column < extract.nx; ++column) {
                const auto offset = static_cast<std::size_t>(column)
                    + static_cast<std::size_t>(extract.nx) * valueRow;
                auto* item = new QTableWidgetItem;
                if (extract.covered[offset] != 0) {
                    item->setText(formatNumber(
                        static_cast<double>(extract.values[offset]),
                        m_numberFormat));
                    item->setTextAlignment(
                        Qt::AlignRight | Qt::AlignVCenter);
                } else {
                    item->setBackground(QColor(Qt::darkGray));
                }
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                table->setItem(row, column, item);
            }
        }
        table->resizeColumnsToContents();
        auto* pageLayout = new QVBoxLayout(page);
        pageLayout->addWidget(info);
        pageLayout->addWidget(table, 1);
        auto label = tr("Level %1").arg(levelData.level);
        if (extract.truncatedX || extract.truncatedY) {
            label += tr(" (truncated)");
        }
        m_tabs->addTab(page, label);
        connect(table, &QTableWidget::cellClicked, this,
            [this, entry](int row, int column) {
                cellClicked(entry, row, column);
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
