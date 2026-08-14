#include "RemoteFileDialog.hpp"

#include <amrexplorer/remote/Connection.hpp>

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <utility>

namespace amrvis::qt {
namespace {

constexpr int pathRole = Qt::UserRole;
constexpr int plotfileRole = Qt::UserRole + 1;

} // namespace

RemoteFileDialog::RemoteFileDialog(std::string host, std::uint16_t port,
                                   std::string token, QString initialPath,
                                   SelectionMode mode, QWidget* parent)
    : QDialog(parent), m_host(std::move(host)), m_port(port),
      m_token(std::move(token)), m_mode(mode)
{
    setWindowTitle(mode == SelectionMode::SinglePlotfile
                       ? tr("Open Remote Plotfile")
                       : tr("Open Remote Plotfile Sequence"));
    resize(720, 480);

    auto* layout = new QVBoxLayout(this);
    auto* pathLayout = new QHBoxLayout;
    m_upButton = new QPushButton(tr("Up"), this);
    m_upButton->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText(tr("Server-visible directory path"));
    m_goButton = new QPushButton(tr("Go"), this);
    pathLayout->addWidget(m_upButton);
    pathLayout->addWidget(m_pathEdit, 1);
    pathLayout->addWidget(m_goButton);
    layout->addLayout(pathLayout);

    if (mode == SelectionMode::PlotfileSequence) {
        m_manualPaths = new QPlainTextEdit(this);
        m_manualPaths->setPlaceholderText(
            tr("Server-visible plotfile paths, one per line, in playback order"));
        m_manualPaths->setVisible(false);
        layout->addWidget(m_manualPaths, 1);
    }

    m_entries = new QTreeWidget(this);
    m_entries->setColumnCount(2);
    m_entries->setHeaderLabels({tr("Name"), tr("Type")});
    m_entries->header()->setStretchLastSection(false);
    m_entries->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_entries->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_entries->setRootIsDecorated(false);
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

    m_watcher = new QFutureWatcher<BrowseResult>(this);
    connect(m_watcher, &QFutureWatcher<BrowseResult>::finished, this,
            [this] { finishLoad(); });
    connect(m_upButton, &QPushButton::clicked, this,
            [this] { loadDirectory(m_parentDirectory); });
    connect(m_goButton, &QPushButton::clicked, this, [this] {
        if (!m_manualEntry) {
            loadDirectory(m_pathEdit->text());
        }
    });
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this] {
        if (!m_manualEntry) {
            loadDirectory(m_pathEdit->text());
        }
    });
    connect(m_pathEdit, &QLineEdit::textChanged, this, [this] { updateOpenButton(); });
    if (m_manualPaths != nullptr) {
        connect(m_manualPaths, &QPlainTextEdit::textChanged, this, [this] { updateOpenButton(); });
    }
    connect(m_entries, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) { activateItem(item); });
    connect(m_entries, &QTreeWidget::itemSelectionChanged, this, [this] {
        recordSelectionOrder();
        updateOpenButton();
    });
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    loadDirectory(initialPath.trimmed());
}

RemoteFileDialog::~RemoteFileDialog()
{
    m_browseStop.request_stop();
}

std::vector<std::string> RemoteFileDialog::selectedPaths() const
{
    if (m_manualEntry) {
        if (m_mode == SelectionMode::SinglePlotfile) {
            const auto text = m_pathEdit->text().trimmed();
            if (text.isEmpty()) {
                return {};
            }
            return {text.toStdString()};
        }
        std::vector<std::string> paths;
        for (const auto& line : m_manualPaths->toPlainText().split('\n', Qt::SkipEmptyParts)) {
            const auto trimmed = line.trimmed();
            if (!trimmed.isEmpty()) {
                paths.push_back(trimmed.toStdString());
            }
        }
        return paths;
    }
    std::vector<std::string> paths;
    for (const auto* item : m_selectionOrder) {
        if (item->data(0, plotfileRole).toBool()) {
            paths.push_back(item->data(0, pathRole).toString().toStdString());
        }
    }
    return paths;
}

QString RemoteFileDialog::currentDirectory() const
{
    if (m_manualEntry) {
        const auto paths = selectedPaths();
        if (!paths.empty()) {
            return QString::fromStdString(
                std::filesystem::path(paths.front()).parent_path().string());
        }
    }
    return m_currentDirectory;
}

void RemoteFileDialog::loadDirectory(const QString& path)
{
    if (m_manualEntry || m_watcher->isRunning()) {
        return;
    }
    m_browseStop = StopSource{};
    const auto cancellation = m_browseStop.get_token();
    const auto host = m_host;
    const auto port = m_port;
    const auto token = m_token;
    const auto existingConnection = m_connection;
    const auto requestedPath = path.toStdString();
    m_selectionOrder.clear();
    m_entries->clear();
    m_status->setText(tr("Loading remote directory..."));
    m_pathEdit->setEnabled(false);
    m_upButton->setEnabled(false);
    m_goButton->setEnabled(false);
    m_buttons->button(QDialogButtonBox::Open)->setEnabled(false);
    m_watcher->setFuture(QtConcurrent::run(
        [host, port, token, existingConnection, requestedPath, cancellation] {
            BrowseResult result;
            try {
                result.connection = existingConnection;
                if (!result.connection) {
                    result.connection =
                        std::make_shared<amrvis::remote::Connection>(
                            host, port,
                            amrvis::remote::ConnectionOptions{
                                .clientName = "AMReXplorer remote browser",
                                .sessionToken = token},
                            cancellation);
                }
                result.browsingSupported =
                    result.connection->serverInfo().selectedMinorVersion >= 1;
                if (result.browsingSupported) {
                    result.listing = result.connection->listDirectory(requestedPath, cancellation);
                }
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
            }
            return result;
        }));
}

void RemoteFileDialog::finishLoad()
{
    const auto result = m_watcher->result();
    m_connection = result.connection;
    // A transport-level failure (idle timeout, tunnel teardown, server exit)
    // leaves the cached connection permanently unusable; drop it so the next
    // browse reconnects instead of failing for the dialog's whole lifetime.
    if (!m_connection || !m_connection->connected()) {
        m_connection.reset();
    }
    m_pathEdit->setEnabled(true);
    m_goButton->setEnabled(true);
    if (!result.error.isEmpty()) {
        m_status->setText(
            tr("Could not browse the remote directory: %1").arg(result.error));
        m_upButton->setEnabled(!m_parentDirectory.isEmpty() &&
                               m_parentDirectory != m_currentDirectory);
        return;
    }
    if (!result.browsingSupported) {
        enterManualMode();
        return;
    }

    m_currentDirectory = QString::fromStdString(result.listing.path);
    m_parentDirectory = QString::fromStdString(result.listing.parentPath);
    m_pathEdit->setText(m_currentDirectory);
    m_upButton->setEnabled(!m_parentDirectory.isEmpty() &&
                           m_parentDirectory != m_currentDirectory);
    for (const auto& entry : result.listing.entries) {
        auto* item = new QTreeWidgetItem(m_entries);
        item->setText(0, QString::fromStdString(entry.name));
        item->setText(1, entry.isPlotfile ? tr("AMReX plotfile")
                                          : tr("Directory"));
        item->setData(0, pathRole, QString::fromStdString(entry.path));
        item->setData(0, plotfileRole, entry.isPlotfile);
        item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
    }
    m_status->setText(
        m_mode == SelectionMode::SinglePlotfile
            ? tr("Select a plotfile directory, or double-click it to open.")
            : tr("Select one or more plotfile directories in playback order."));
    updateOpenButton();
}

void RemoteFileDialog::enterManualMode()
{
    m_manualEntry = true;
    m_entries->setVisible(false);
    m_upButton->setVisible(false);
    m_goButton->setVisible(false);
    m_pathEdit->setPlaceholderText(tr("Server-visible plotfile path"));
    if (m_manualPaths != nullptr) {
        m_pathEdit->setVisible(false);
        m_manualPaths->setVisible(true);
        m_status->setText(tr("This server does not support filesystem "
                             "browsing. Enter the server-visible plotfile "
                             "paths, one per line, in playback order:"));
    } else {
        m_status->setText(tr("This server does not support filesystem "
                             "browsing. Enter the server-visible plotfile "
                             "path:"));
    }
    updateOpenButton();
}

void RemoteFileDialog::updateOpenButton()
{
    const auto paths = selectedPaths();
    m_buttons->button(QDialogButtonBox::Open)
        ->setEnabled(m_mode == SelectionMode::SinglePlotfile ? paths.size() == 1
                                                             : !paths.empty());
}

void RemoteFileDialog::recordSelectionOrder()
{
    // Keep still-selected items in their recorded order, then append newly
    // selected ones in display order, so the sequence playback order matches
    // the order in which the user selected the plotfiles.
    std::vector<QTreeWidgetItem*> updated;
    updated.reserve(m_selectionOrder.size());
    for (auto* item : m_selectionOrder) {
        if (item->isSelected()) {
            updated.push_back(item);
        }
    }
    for (int index = 0; index < m_entries->topLevelItemCount(); ++index) {
        auto* item = m_entries->topLevelItem(index);
        if (item->isSelected()
            && std::find(updated.begin(), updated.end(), item) == updated.end()) {
            updated.push_back(item);
        }
    }
    m_selectionOrder = std::move(updated);
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
        } else {
            updateOpenButton();
        }
        return;
    }
    loadDirectory(item->data(0, pathRole).toString());
}

} // namespace amrvis::qt
