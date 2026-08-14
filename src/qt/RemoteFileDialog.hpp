#pragma once

#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/remote/Protocol.hpp>

#include <QDialog>
#include <QFutureWatcher>

#include <cstdint>
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

class RemoteFileDialog final : public QDialog {
  public:
    enum class SelectionMode { SinglePlotfile, PlotfileSequence };

    RemoteFileDialog(std::string host, std::uint16_t port, std::string token,
                     QString initialPath, SelectionMode mode,
                     QWidget* parent = nullptr);
    ~RemoteFileDialog() override;

    [[nodiscard]] std::vector<std::string> selectedPaths() const;
    [[nodiscard]] QString currentDirectory() const;

  private:
    struct BrowseResult {
        amrvis::remote::RemoteDirectoryListing listing;
        std::shared_ptr<amrvis::remote::Connection> connection;
        QString error;
    };

    void loadDirectory(const QString& path);
    void finishLoad();
    void updateOpenButton();
    void activateItem(QTreeWidgetItem* item);

    std::string m_host;
    std::uint16_t m_port = 0;
    std::string m_token;
    std::shared_ptr<amrvis::remote::Connection> m_connection;
    SelectionMode m_mode = SelectionMode::SinglePlotfile;
    QString m_currentDirectory;
    QString m_parentDirectory;
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
