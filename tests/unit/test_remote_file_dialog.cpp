#include "RemoteFileDialog.hpp"

#include "../../src/remote/Codec.hpp"

#include <amrexplorer/remote/Frame.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <QAbstractItemView>
#include <QApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeWidget>

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
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

bool waitFor(QApplication& application, const std::function<bool()>& condition) {
    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < 5000) {
        application.processEvents();
        if (condition()) {
            return true;
        }
        QThread::msleep(10);
    }
    application.processEvents();
    return condition();
}

QPushButton* goButton(QDialog& dialog)
{
    const auto buttons = dialog.findChildren<QPushButton*>();
    const auto found = std::find_if(buttons.begin(), buttons.end(),
        [](QPushButton* button) { return button->text() == QStringLiteral("Go"); });
    return found == buttons.end() ? nullptr : *found;
}

// Minimal remote peer: completes a handshake at `minorVersion` and answers one
// ListDirectoryRequest with a fixed listing before dropping the connection.
void serveHandshake(amrvis::remote::Socket& peer, std::uint16_t minorVersion)
{
    using namespace amrvis::remote;
    const auto frame = readFrame(peer, defaultMaximumFrameBytes);
    require(frame.has_value(), "stub peer closed before the handshake");
    const auto request = codec::decode(*frame);
    require(codec::inspect(*request).payload == PayloadKind::HelloRequest,
        "stub peer did not receive a hello request");
    HelloResponseData hello;
    hello.serverName = "stub peer";
    hello.softwareVersion = "test";
    hello.selectedMinorVersion = minorVersion;
    hello.maximumFrameBytes = defaultMaximumFrameBytes;
    hello.maximumDatasets = 8;
    hello.maximumOutstandingRequests = 8;
    hello.workerCount = 1;
    writeFrame(peer,
        codec::encode(request->request_id, codec::toWire(hello), minorVersion),
        defaultMaximumFrameBytes);
}

void serveDirectoryListing(amrvis::remote::Socket& peer)
{
    using namespace amrvis::remote;
    const auto frame = readFrame(peer, defaultMaximumFrameBytes);
    require(frame.has_value(), "stub peer closed before the list request");
    const auto request = codec::decode(*frame);
    const auto info = codec::inspect(*request);
    require(info.payload == PayloadKind::ListDirectoryRequest,
        "stub peer did not receive a directory-listing request");
    RemoteDirectoryListing listing;
    listing.path = "/";
    listing.parentPath = "/";
    listing.entries.push_back({"plt00000", "/plt00000", true});
    writeFrame(peer,
        codec::encode(info.requestId, codec::toWire(listing)),
        defaultMaximumFrameBytes);
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

    // A protocol 1.0 server cannot browse; the dialog must fall back to
    // accepting a typed plotfile path.
    auto legacyListener = amrvis::remote::listenOnLoopback(0);
    std::thread legacyPeer([&] {
        auto peer = amrvis::remote::acceptConnection(legacyListener.socket);
        serveHandshake(peer, 0);
        // Hold the socket open until the dialog tears its connection down.
        while (amrvis::remote::readFrame(peer,
                   amrvis::remote::defaultMaximumFrameBytes)
                   .has_value()) {
        }
    });
    {
        amrvis::qt::RemoteFileDialog legacyDialog(
            "127.0.0.1", legacyListener.port, server.token(),
            QStringLiteral("/"),
            amrvis::qt::RemoteFileDialog::SelectionMode::SinglePlotfile);
        auto* legacyStatus = legacyDialog.findChild<QLabel*>();
        require(legacyStatus != nullptr,
            "legacy browser did not create a status label");
        require(waitFor(application, [&] {
            return legacyStatus->text().contains(
                QStringLiteral("does not support filesystem browsing"));
        }), "legacy browser did not explain that the server cannot browse");
        auto* legacyPathEdit = legacyDialog.findChild<QLineEdit*>();
        require(legacyPathEdit != nullptr,
            "legacy browser lost its path entry");
        legacyPathEdit->setText(QStringLiteral("/scratch/run/plt00010"));
        application.processEvents();
        const auto legacyPaths = legacyDialog.selectedPaths();
        require(legacyPaths.size() == 1
                && legacyPaths.front() == "/scratch/run/plt00010",
            "legacy browser did not return the typed plotfile path");
        auto* legacyButtons = legacyDialog.findChild<QDialogButtonBox*>();
        require(legacyButtons != nullptr
                && legacyButtons->button(QDialogButtonBox::Open)->isEnabled(),
            "legacy browser did not allow opening the typed plotfile path");
    }
    legacyPeer.join();

    // Sequence playback order must follow selection order, not display order.
    QTemporaryDir orderScratch;
    require(orderScratch.isValid(), "could not create a scratch directory");
    const auto orderRoot = std::filesystem::path(orderScratch.path().toStdString());
    for (const char* name : {"plt00000", "plt00001"}) {
        const auto directory = orderRoot / name;
        std::filesystem::create_directories(directory / "Level_0");
        std::ofstream(directory / "Header") << "fake plotfile header\n";
    }
    amrvis::qt::RemoteFileDialog orderDialog(
        "127.0.0.1", server.port(), server.token(),
        QString::fromStdString(orderRoot.string()),
        amrvis::qt::RemoteFileDialog::SelectionMode::PlotfileSequence);
    auto* orderEntries = orderDialog.findChild<QTreeWidget*>();
    require(orderEntries != nullptr, "order browser did not create its file view");
    auto* laterItem = waitForPlotfile(application, *orderEntries,
        QStringLiteral("plt00001"));
    auto* earlierItem = waitForPlotfile(application, *orderEntries,
        QStringLiteral("plt00000"));
    require(laterItem != nullptr && earlierItem != nullptr,
        "order browser did not display the scratch plotfiles");
    laterItem->setSelected(true);
    earlierItem->setSelected(true);
    application.processEvents();
    const auto orderPaths = orderDialog.selectedPaths();
    require(orderPaths.size() == 2
            && orderPaths.front() == (orderRoot / "plt00001").string()
            && orderPaths.back() == (orderRoot / "plt00000").string(),
        "remote browser did not preserve the selection order");

    // A cached connection that died must be dropped so a later Go reconnects.
    auto dropListener = amrvis::remote::listenOnLoopback(0);
    std::atomic<int> dropConnections{0};
    std::thread dropPeer([&] {
        for (int session = 0; session < 2; ++session) {
            auto peer =
                amrvis::remote::acceptConnection(dropListener.socket);
            dropConnections.fetch_add(1);
            serveHandshake(peer, 1);
            serveDirectoryListing(peer);
        }
    });
    amrvis::qt::RemoteFileDialog dropDialog(
        "127.0.0.1", dropListener.port, server.token(), QStringLiteral("/"),
        amrvis::qt::RemoteFileDialog::SelectionMode::SinglePlotfile);
    auto* dropEntries = dropDialog.findChild<QTreeWidget*>();
    require(dropEntries != nullptr, "drop browser did not create its file view");
    require(waitForPlotfile(application, *dropEntries,
                  QStringLiteral("plt00000"))
                != nullptr,
        "drop browser did not display the first stub listing");
    auto* dropGo = goButton(dropDialog);
    require(dropGo != nullptr, "drop browser did not create a Go button");
    require(waitFor(application, [&] {
        if (dropConnections.load() >= 2) {
            return true;
        }
        if (dropGo->isEnabled()) {
            dropGo->click();
        }
        return false;
    }), "a dead cached connection was reused instead of dropped");
    require(waitForPlotfile(application, *dropEntries,
                  QStringLiteral("plt00000"))
                != nullptr,
        "drop browser did not recover after the connection died");
    dropPeer.join();
}
