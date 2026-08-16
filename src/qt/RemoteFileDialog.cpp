#include "RemoteFileDialog.hpp"

#include <amrexplorer/remote/Connection.hpp>

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <exception>
#include <utility>

namespace amrvis::qt {
namespace {

constexpr int pathRole = Qt::UserRole;
constexpr int plotfileRole = Qt::UserRole + 1;

} // namespace

RemoteFileDialog::RemoteFileDialog(
    std::shared_ptr<remote::Connection> connection, QString initialPath,
    SelectionMode mode, QWidget* parent)
    : QDialog(parent)
    , m_connection(std::move(connection))
    , m_mode(mode)
{
    setWindowTitle(mode == SelectionMode::SinglePlotfile
            ? tr("Open Remote Plotfile")
            : tr("Open Remote Plotfile Sequence"));
    resize(720, 480);

    auto* layout = new QVBoxLayout(this);
    auto* pathLayout = new QHBoxLayout;
    m_upButton = new QPushButton(tr("Up"), this);
    m_upButton->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    m_upButton->setToolTip(tr("Go to the parent directory"));
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText(
        tr("Directory on the remote machine, e.g. ~/run or /scratch/run"));
    m_goButton = new QPushButton(tr("Go"), this);
    pathLayout->addWidget(m_upButton);
    pathLayout->addWidget(m_pathEdit, 1);
    pathLayout->addWidget(m_goButton);
    layout->addLayout(pathLayout);

    m_entries = new QTreeWidget(this);
    m_entries->setColumnCount(2);
    m_entries->setHeaderLabels({tr("Name"), tr("Type")});
    m_entries->header()->setStretchLastSection(false);
    m_entries->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_entries->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_entries->setRootIsDecorated(false);
    m_entries->setUniformRowHeights(true);
    m_entries->setSelectionMode(mode == SelectionMode::SinglePlotfile
            ? QAbstractItemView::SingleSelection
            : QAbstractItemView::ExtendedSelection);
    layout->addWidget(m_entries, 1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_buttons = new QDialogButtonBox(
        QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Open)->setEnabled(false);
    layout->addWidget(m_buttons);
    // No default button: Enter in the path box navigates, Enter on an entry
    // activates it, and neither may also fire Open.
    for (auto* button : m_buttons->buttons()) {
        if (auto* pushButton = qobject_cast<QPushButton*>(button)) {
            pushButton->setAutoDefault(false);
        }
    }
    m_upButton->setAutoDefault(false);
    m_goButton->setAutoDefault(false);

    m_watcher = new QFutureWatcher<BrowseResult>(this);
    connect(m_watcher, &QFutureWatcher<BrowseResult>::finished, this,
        [this] { finishLoad(); });
    connect(m_upButton, &QPushButton::clicked, this,
        [this] { loadDirectory(m_parentDirectory); });
    connect(m_goButton, &QPushButton::clicked, this,
        [this] { loadDirectory(m_pathEdit->text()); });
    connect(m_pathEdit, &QLineEdit::returnPressed, this,
        [this] { loadDirectory(m_pathEdit->text()); });
    connect(m_entries, &QTreeWidget::itemActivated, this,
        [this](QTreeWidgetItem* item, int) { activateItem(item); });
    connect(m_entries, &QTreeWidget::itemSelectionChanged, this,
        [this] { updateOpenButton(); });
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    loadDirectory(initialPath.trimmed());
}

RemoteFileDialog::~RemoteFileDialog()
{
    // A listing still in flight holds its own copy of the connection and
    // finishes on the worker; this only stops it waiting for the server.
    m_browseStop.request_stop();
}

std::vector<std::string> RemoteFileDialog::selectedPaths() const
{
    std::vector<std::string> paths;
    for (int index = 0; index < m_entries->topLevelItemCount(); ++index) {
        const auto* item = m_entries->topLevelItem(index);
        if (item->isSelected() && item->data(0, plotfileRole).toBool()) {
            paths.push_back(item->data(0, pathRole).toString().toStdString());
        }
    }
    return paths;
}

QString RemoteFileDialog::currentDirectory() const
{
    return m_currentDirectory;
}

void RemoteFileDialog::loadDirectory(const QString& path)
{
    if (m_watcher->isRunning()) {
        return;
    }
    m_browseStop = StopSource{};
    m_status->setText(tr("Listing %1...")
            .arg(path.isEmpty() ? tr("the home directory") : path));
    m_pathEdit->setEnabled(false);
    m_upButton->setEnabled(false);
    m_goButton->setEnabled(false);
    // Greyed out until the new listing lands: on a slow link the old one
    // stays visible for a moment and must not read as the current one, and
    // Open must not act on its selection either.
    m_entries->setEnabled(false);
    m_buttons->button(QDialogButtonBox::Open)->setEnabled(false);
    m_watcher->setFuture(QtConcurrent::run(
        [connection = m_connection, requestedPath = path.toStdString(),
            cancellation = m_browseStop.get_token()] {
            BrowseResult result;
            try {
                result.listing
                    = connection->listDirectory(requestedPath, cancellation);
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
            }
            return result;
        }));
}

void RemoteFileDialog::finishLoad()
{
    const auto result = m_watcher->result();
    m_pathEdit->setEnabled(true);
    m_goButton->setEnabled(true);
    m_entries->setEnabled(true);
    m_upButton->setEnabled(!m_parentDirectory.isEmpty()
        && m_parentDirectory != m_currentDirectory);
    if (!result.error.isEmpty()) {
        // The previous listing stays up, selection included; only the
        // message changes.
        m_status->setText(
            tr("Could not list the remote directory: %1").arg(result.error));
        updateOpenButton();
        m_pathEdit->setFocus();
        m_pathEdit->selectAll();
        return;
    }
    m_currentDirectory = QString::fromStdString(result.listing.path);
    m_parentDirectory = QString::fromStdString(result.listing.parentPath);
    m_pathEdit->setText(m_currentDirectory);
    m_upButton->setEnabled(m_parentDirectory != m_currentDirectory);
    // Repopulate with updates off, then lay out and repaint the whole view in
    // one go. Left to the view's own delayed relayout, the first row (made
    // current by setFocus below) can repaint over the stale layout while the
    // rest of the old listing lingers until the next repaint trigger.
    m_entries->setUpdatesEnabled(false);
    m_entries->clear();
    const auto directoryIcon = style()->standardIcon(QStyle::SP_DirIcon);
    const auto plotfileIcon = style()->standardIcon(QStyle::SP_FileIcon);
    for (const auto& entry : result.listing.entries) {
        auto* item = new QTreeWidgetItem(m_entries);
        item->setText(0, QString::fromStdString(entry.name));
        item->setText(1, entry.isPlotfile ? tr("AMReX plotfile")
                                          : tr("Directory"));
        item->setIcon(0, entry.isPlotfile ? plotfileIcon : directoryIcon);
        item->setData(0, pathRole, QString::fromStdString(entry.path));
        item->setData(0, plotfileRole, entry.isPlotfile);
        // Only plotfiles are selectable; other directories are entered.
        item->setFlags(entry.isPlotfile
                ? Qt::ItemIsEnabled | Qt::ItemIsSelectable
                : Qt::ItemIsEnabled);
    }
    m_entries->doItemsLayout();
    m_entries->scrollToTop();
    m_entries->setUpdatesEnabled(true);
    auto status = m_mode == SelectionMode::SinglePlotfile
        ? tr("Select a plotfile, or double-click a directory to enter it.")
        : tr("Select one or more plotfiles; they play in name order.");
    if (result.listing.entries.empty()) {
        status = tr("No subdirectories here.");
    }
    if (result.listing.truncated) {
        status += tr(" Only the first %1 entries are shown; enter a "
                     "subdirectory path to narrow the listing.")
                      .arg(result.listing.entries.size());
    }
    m_status->setText(status);
    m_entries->setFocus();
    updateOpenButton();
}

void RemoteFileDialog::updateOpenButton()
{
    const auto count = selectedPaths().size();
    m_buttons->button(QDialogButtonBox::Open)
        ->setEnabled(m_mode == SelectionMode::SinglePlotfile ? count == 1
                                                             : count >= 1);
}

void RemoteFileDialog::activateItem(QTreeWidgetItem* item)
{
    if (item == nullptr) {
        return;
    }
    if (item->data(0, plotfileRole).toBool()) {
        item->setSelected(true);
        if (m_mode == SelectionMode::SinglePlotfile) {
            accept();
        }
        return;
    }
    loadDirectory(item->data(0, pathRole).toString());
}

} // namespace amrvis::qt
