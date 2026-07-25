#include "FabSelectorDock.hpp"

#include <QAbstractTableModel>
#include <QEvent>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QModelIndex>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace amrvis::qt {
namespace {

QString formatBox(const IntBox& box, int dimension)
{
    QStringList lower;
    QStringList upper;
    QStringList centering;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto index = static_cast<std::size_t>(axis);
        lower.push_back(QString::number(box.lower[index]));
        upper.push_back(QString::number(box.upper[index]));
        centering.push_back(QString::number(box.centering[index]));
    }
    return QStringLiteral("((%1) (%2) (%3))")
        .arg(lower.join(QLatin1Char(',')),
            upper.join(QLatin1Char(',')),
            centering.join(QLatin1Char(',')));
}

std::optional<Int3> parsePoint(const QString& text, int dimension)
{
    const auto trimmed = text.trimmed();
    if (dimension < 1 || dimension > 3
        || !trimmed.startsWith(QLatin1Char('('))
        || !trimmed.endsWith(QLatin1Char(')'))) {
        return std::nullopt;
    }
    const auto components = trimmed.sliced(1, trimmed.size() - 2)
        .split(QLatin1Char(','), Qt::KeepEmptyParts);
    if (components.size() != dimension) {
        return std::nullopt;
    }
    Int3 point;
    for (int axis = 0; axis < dimension; ++axis) {
        bool valid = false;
        const auto value = components[axis].trimmed().toInt(&valid);
        if (!valid) {
            return std::nullopt;
        }
        point[static_cast<std::size_t>(axis)] = value;
    }
    return point;
}

QString formatPoint(const Int3& point, int dimension)
{
    QStringList components;
    for (int axis = 0; axis < dimension; ++axis) {
        components.push_back(
            QString::number(point[static_cast<std::size_t>(axis)]));
    }
    return QStringLiteral("(%1)").arg(components.join(QLatin1Char(',')));
}

QString pointExample(int dimension)
{
    if (dimension == 1) {
        return QStringLiteral("(34)");
    }
    if (dimension == 2) {
        return QStringLiteral("(34,24)");
    }
    return QStringLiteral("(34,24,0)");
}

} // namespace

class FabSelectorDock::Model final : public QAbstractTableModel {
public:
    explicit Model(QObject* parent = nullptr)
        : QAbstractTableModel(parent)
    {}

    void setEntries(std::vector<FabSelectorEntry> entries)
    {
        beginResetModel();
        m_entries = std::move(entries);
        endResetModel();
    }

    [[nodiscard]] const std::vector<FabSelectorEntry>& entries() const noexcept
    {
        return m_entries;
    }

    int rowCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_entries.size());
    }

    int columnCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : 7;
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() < 0
            || static_cast<std::size_t>(index.row()) >= m_entries.size()) {
            return {};
        }
        const auto& entry = m_entries[static_cast<std::size_t>(index.row())];
        if (role == Qt::UserRole) {
            return static_cast<qulonglong>(entry.ordinal);
        }
        if (role != Qt::DisplayRole) {
            return {};
        }
        switch (index.column()) {
        case 0: return static_cast<qulonglong>(entry.blockIndex);
        case 1: return formatBox(entry.validBox, entry.dimension);
        case 2: return formatBox(entry.storedBox, entry.dimension);
        case 3: return entry.components;
        case 4: return QString::fromStdString(entry.filePath.filename().string());
        case 5: return static_cast<qulonglong>(entry.fileOffset);
        case 6: return entry.precision;
        default: return {};
        }
    }

    QVariant headerData(
        int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        constexpr std::array<const char*, 7> headers{
            "Grid", "Valid box", "FAB Box", "Components",
            "File", "Offset", "Precision"};
        if (section < 0 || static_cast<std::size_t>(section) >= headers.size()) {
            return {};
        }
        return tr(headers[static_cast<std::size_t>(section)]);
    }

private:
    std::vector<FabSelectorEntry> m_entries;
};

class FabSelectorDock::ProxyModel final : public QSortFilterProxyModel {
public:
    explicit ProxyModel(QObject* parent = nullptr)
        : QSortFilterProxyModel(parent)
    {}

    void setDimension(int dimension)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
        m_dimension = dimension;
        endFilterChange(Direction::Rows);
#else
        m_dimension = dimension;
        invalidateRowsFilter();
#endif
    }

    void setPointText(QString text)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
        m_pointText = std::move(text);
        endFilterChange(Direction::Rows);
#else
        m_pointText = std::move(text);
        invalidateRowsFilter();
#endif
    }

protected:
    bool filterAcceptsRow(
        int sourceRow, const QModelIndex& sourceParent) const override
    {
        if (m_pointText.trimmed().isEmpty()) {
            return true;
        }
        const auto point = parsePoint(m_pointText, m_dimension);
        if (!point) {
            return true;
        }
        const auto* model = dynamic_cast<const Model*>(sourceModel());
        if (model == nullptr || sourceParent.isValid() || sourceRow < 0
            || static_cast<std::size_t>(sourceRow) >= model->entries().size()) {
            return false;
        }
        const auto& entry = model->entries()[static_cast<std::size_t>(sourceRow)];
        const auto& box = entry.storedBox;
        for (int axis = 0; axis < entry.dimension; ++axis) {
            const auto index = static_cast<std::size_t>(axis);
            if ((*point)[index] < box.lower[index]
                || (*point)[index] > box.upper[index]) {
                return false;
            }
        }
        return true;
    }

private:
    int m_dimension = 0;
    QString m_pointText;
};

FabSelectorDock::FabSelectorDock(QWidget* parent)
    : QDockWidget(tr("FAB Selector"), parent)
    , m_model(new Model(this))
    , m_proxy(new ProxyModel(this))
{
    setObjectName(QStringLiteral("fabSelectorDock"));
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(4, 4, 4, 4);

    auto* filterLayout = new QHBoxLayout;
    m_filter = new QLineEdit(content);
    m_filter->setObjectName(QStringLiteral("fabSelectorFilter"));
    m_filter->setReadOnly(true);
    m_filter->setPlaceholderText(
        tr("Filter int tuple (e.g., (34,24,0))"));
    m_filter->setToolTip(
        tr("Click to find FABs containing an integer point"));
    m_filter->installEventFilter(this);
    m_clearFilter = new QPushButton(tr("Clear"), content);
    m_clearFilter->setObjectName(QStringLiteral("fabSelectorClearFilter"));
    m_clearFilter->setVisible(false);
    filterLayout->addWidget(m_filter);
    filterLayout->addWidget(m_clearFilter);
    layout->addLayout(filterLayout);

    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(Qt::DisplayRole);

    m_table = new QTableView(content);
    m_table->setObjectName(QStringLiteral("fabSelectorTable"));
    m_table->setModel(m_proxy);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(0, Qt::AscendingOrder);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table);

    auto* buttons = new QHBoxLayout;
    m_view = new QPushButton(tr("View FAB"), content);
    m_back = new QPushButton(tr("Back to MultiFab"), content);
    m_view->setObjectName(QStringLiteral("fabViewButton"));
    m_back->setObjectName(QStringLiteral("fabBackButton"));
    buttons->addWidget(m_view);
    buttons->addWidget(m_back);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    setWidget(content);

    connect(m_filter, &QLineEdit::textChanged,
        this, [this](const QString& text) {
            m_proxy->setPointText(text);
            m_clearFilter->setVisible(!text.trimmed().isEmpty());
        });
    connect(m_clearFilter, &QPushButton::clicked,
        m_filter, &QLineEdit::clear);
    connect(m_view, &QPushButton::clicked, this,
        [this] { activateCurrent(); });
    connect(m_table, &QTableView::doubleClicked, this,
        [this](const QModelIndex&) { activateCurrent(); });
    connect(m_back, &QPushButton::clicked, this,
        &FabSelectorDock::backRequested);
    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged,
        this, [this] {
            m_view->setEnabled(m_table->currentIndex().isValid());
        });
    m_view->setEnabled(false);
    m_back->setVisible(false);
}

void FabSelectorDock::setEntries(std::vector<FabSelectorEntry> entries)
{
    const auto showValidBox = std::any_of(
        entries.begin(), entries.end(),
        [](const FabSelectorEntry& entry) { return !entry.rawRecord; });
    m_dimension = entries.empty() ? 0 : entries.front().dimension;
    m_model->setEntries(std::move(entries));
    m_proxy->setDimension(m_dimension);
    m_table->setColumnHidden(1, !showValidBox);
    m_filter->clear();
    m_filter->setPlaceholderText(
        tr("Filter int tuple (e.g., %1)").arg(pointExample(m_dimension)));
    m_view->setEnabled(false);
    if (m_proxy->rowCount() > 0) {
        m_table->selectRow(0);
    }
}

bool FabSelectorDock::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_filter
        && event->type() == QEvent::MouseButtonRelease) {
        promptForPoint();
        return true;
    }
    if (watched == m_filter && event->type() == QEvent::KeyPress) {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Return
            || keyEvent->key() == Qt::Key_Enter
            || keyEvent->key() == Qt::Key_Space) {
            promptForPoint();
            return true;
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

void FabSelectorDock::promptForPoint()
{
    if (m_dimension < 1 || m_dimension > 3) {
        return;
    }
    bool accepted = false;
    const auto example = pointExample(m_dimension);
    const auto text = QInputDialog::getText(
        this, tr("Find FABs containing a point"),
        tr("Enter an integer tuple such as %1:").arg(example),
        QLineEdit::Normal, m_filter->text(), &accepted);
    if (!accepted) {
        return;
    }
    if (text.trimmed().isEmpty()) {
        m_filter->clear();
        return;
    }
    const auto point = parsePoint(text, m_dimension);
    if (!point) {
        QMessageBox::warning(
            this, tr("Invalid point"),
            tr("Enter exactly %1 comma-separated integer(s), for example %2.")
                .arg(m_dimension)
                .arg(example));
        return;
    }
    m_filter->setText(formatPoint(*point, m_dimension));
    if (m_proxy->rowCount() == 1) {
        m_table->selectRow(0);
        activateCurrent();
    }
}

const std::vector<FabSelectorEntry>& FabSelectorDock::entries() const noexcept
{
    return m_model->entries();
}

void FabSelectorDock::setBackAvailable(bool available)
{
    m_back->setVisible(available);
    m_back->setEnabled(available);
}

void FabSelectorDock::selectEntry(std::size_t ordinal)
{
    for (int row = 0; row < m_proxy->rowCount(); ++row) {
        const auto index = m_proxy->index(row, 0);
        if (index.data(Qt::UserRole).toULongLong() == ordinal) {
            m_table->selectRow(row);
            m_table->scrollTo(index);
            return;
        }
    }
}

void FabSelectorDock::activateCurrent()
{
    const auto proxyIndex = m_table->currentIndex();
    if (!proxyIndex.isValid()) {
        return;
    }
    const auto sourceIndex = m_proxy->mapToSource(proxyIndex);
    if (sourceIndex.row() < 0) {
        return;
    }
    emit viewRequested(static_cast<std::size_t>(sourceIndex.row()));
}

} // namespace amrvis::qt
