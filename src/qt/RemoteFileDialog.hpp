#pragma once

#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/remote/Protocol.hpp>

#include <QDialog>
#include <QFutureWatcher>
#include <QString>

#include <memory>
#include <string>
#include <vector>

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace amrvis::remote {
class Connection;
}

namespace amrvis::qt {

// Browses the directories the remote server can see and picks plotfiles
// there, over the window's live connection: each listing is one
// Connection::listDirectory call on a worker thread, applied on the GUI
// thread when it lands. Only directories are shown; the ones the server
// recognizes as plotfiles are marked and are the only selectable entries.
// The path box accepts anything a typed dataset path accepts ("~/run",
// "run", "/scratch/run"); the server resolves it. Selection order is not
// tracked: a sequence plays in the listing's name order, which is what
// plotfile numbering means.
class RemoteFileDialog final : public QDialog {
public:
    enum class SelectionMode { SinglePlotfile, PlotfileSequence };

    // `initialPath` empty means the server's home.
    RemoteFileDialog(std::shared_ptr<remote::Connection> connection,
        QString initialPath, SelectionMode mode, QWidget* parent = nullptr);
    ~RemoteFileDialog() override;

    // Server-side paths of the selected plotfiles, in listing order.
    [[nodiscard]] std::vector<std::string> selectedPaths() const;
    // The directory shown when the dialog closed; empty before the first
    // listing lands.
    [[nodiscard]] QString currentDirectory() const;

private:
    struct BrowseResult {
        QString requestedPath;
        remote::RemoteDirectoryListing listing;
        QString error;
    };

    void loadDirectory(const QString& path);
    void finishLoad();
    void updateOpenButton();
    void activateItem(QTreeWidgetItem* item);

    std::shared_ptr<remote::Connection> m_connection;
    SelectionMode m_mode = SelectionMode::SinglePlotfile;
    QString m_currentDirectory;
    QString m_parentDirectory;
    // Why the first listing fell back to the home directory, prepended to
    // the status once that listing lands.
    QString m_fallbackNotice;
    QLineEdit* m_pathEdit = nullptr;
    QPushButton* m_upButton = nullptr;
    QPushButton* m_goButton = nullptr;
    QTreeWidget* m_entries = nullptr;
    QLabel* m_status = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    QFutureWatcher<BrowseResult>* m_watcher = nullptr;
    StopSource m_browseStop;
};

} // namespace amrvis::qt
