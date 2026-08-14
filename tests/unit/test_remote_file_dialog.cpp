#include "RemoteFileDialog.hpp"

#include <amrexplorer/remote/Server.hpp>

#include <QAbstractItemView>
#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTreeWidget>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

class RunningServer {
  public:
    explicit RunningServer(amrvis::remote::Server& server)
        : m_server(server), m_thread([this] {
              try {
                  m_server.run();
              } catch (...) {
                  m_failure = std::current_exception();
              }
          })
    {
    }

    ~RunningServer()
    {
        m_server.requestStop();
        m_thread.join();
        if (m_failure) {
            std::terminate();
        }
    }

  private:
    amrvis::remote::Server& m_server;
    std::thread m_thread;
    std::exception_ptr m_failure;
};

QTreeWidgetItem* waitForPlotfile(QApplication& application,
                                 QTreeWidget& entries, const QString& name)
{
    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < 5000) {
        application.processEvents();
        for (int index = 0; index < entries.topLevelItemCount(); ++index) {
            auto* item = entries.topLevelItem(index);
            if (item->text(0) == name &&
                item->text(1) == QStringLiteral("AMReX plotfile")) {
                return item;
            }
        }
        QThread::msleep(10);
    }
    return nullptr;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "usage: test_remote_file_dialog MATERIALIZED_PLOTFILE\n";
        return 2;
    }
    QApplication application(argc, argv);
    amrvis::remote::Server server;
    RunningServer running(server);
    const auto plotfile = std::filesystem::absolute(argv[1]);

    amrvis::qt::RemoteFileDialog dialog(
        "127.0.0.1", server.port(), server.token(),
        QString::fromStdString(plotfile.parent_path().string()),
        amrvis::qt::RemoteFileDialog::SelectionMode::SinglePlotfile);
    auto* entries = dialog.findChild<QTreeWidget*>();
    require(entries != nullptr, "remote browser did not create its file view");
    auto* plotfileItem =
        waitForPlotfile(application, *entries,
                        QString::fromStdString(plotfile.filename().string()));
    require(plotfileItem != nullptr,
            "remote browser did not display the remote plotfile");
    plotfileItem->setSelected(true);
    application.processEvents();
    const auto selected = dialog.selectedPaths();
    require(selected.size() == 1 && selected.front() == plotfile.string(),
            "remote browser did not return the selected plotfile path");
    require(dialog.currentDirectory() ==
                QString::fromStdString(plotfile.parent_path().string()),
            "remote browser did not retain the displayed server directory");

    amrvis::qt::RemoteFileDialog sequenceDialog(
        "127.0.0.1", server.port(), server.token(),
        QString::fromStdString(plotfile.parent_path().string()),
        amrvis::qt::RemoteFileDialog::SelectionMode::PlotfileSequence);
    auto* sequenceEntries = sequenceDialog.findChild<QTreeWidget*>();
    require(sequenceEntries != nullptr &&
                sequenceEntries->selectionMode() ==
                    QAbstractItemView::ExtendedSelection,
            "remote sequence browser did not allow multiple plotfiles");
    auto* sequenceItem =
        waitForPlotfile(application, *sequenceEntries,
                        QString::fromStdString(plotfile.filename().string()));
    require(sequenceItem != nullptr,
            "remote sequence browser did not display the remote plotfile");
    sequenceItem->setSelected(true);
    application.processEvents();
    require(sequenceDialog.selectedPaths() == selected,
            "remote sequence browser did not return its selected plotfiles");
}
