// The remote browser against a real loopback Server: listing, entering a
// directory, going up, an unreadable path, and the two selection modes. The
// dialog never runs its own event loop here; the test pumps events itself.

#include "RemoteFileDialog.hpp"

#include <amrexplorer/remote/Connection.hpp>
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

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
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
        : m_server(server)
        , m_thread([this] {
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

bool waitFor(QApplication& application, const std::function<bool()>& condition)
{
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

QTreeWidgetItem* findEntry(QTreeWidget& entries, const QString& name)
{
    for (int index = 0; index < entries.topLevelItemCount(); ++index) {
        auto* item = entries.topLevelItem(index);
        if (item->text(0) == name) {
            return item;
        }
    }
    return nullptr;
}

QTreeWidgetItem* waitForEntry(
    QApplication& application, QTreeWidget& entries, const QString& name)
{
    QTreeWidgetItem* found = nullptr;
    static_cast<void>(waitFor(application, [&] {
        found = findEntry(entries, name);
        return found != nullptr;
    }));
    return found;
}

QPushButton* buttonNamed(QDialog& dialog, const QString& text)
{
    const auto buttons = dialog.findChildren<QPushButton*>();
    const auto found = std::find_if(buttons.begin(), buttons.end(),
        [&](QPushButton* button) { return button->text() == text; });
    return found == buttons.end() ? nullptr : *found;
}

bool samePath(const std::string& lhs, const std::filesystem::path& rhs)
{
    return std::filesystem::path(lhs).lexically_normal()
        == rhs.lexically_normal();
}

void makeFakePlotfile(const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory / "Level_0");
    std::ofstream(directory / "Header") << "fake plotfile header\n";
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    using amrvis::qt::RemoteFileDialog;
    using Mode = RemoteFileDialog::SelectionMode;

    // A scratch tree: two plotfiles and a subdirectory holding a third.
    QTemporaryDir scratch;
    require(scratch.isValid(), "could not create a scratch directory");
    const auto root = std::filesystem::path(scratch.path().toStdString());
    makeFakePlotfile(root / "plt00000");
    makeFakePlotfile(root / "plt00001");
    makeFakePlotfile(root / "sub" / "plt00002");

    amrvis::remote::Server server;
    RunningServer running(server);
    auto connection = std::make_shared<amrvis::remote::Connection>(
        "127.0.0.1", server.port(),
        amrvis::remote::ConnectionOptions{
            .clientName = "remote file dialog test",
            .sessionToken = server.token()});

    // Single mode: listing, selection, entering a directory, up, bad path.
    {
        RemoteFileDialog dialog(connection,
            QString::fromStdString(root.string()), Mode::SinglePlotfile);
        auto* entries = dialog.findChild<QTreeWidget*>();
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        auto* status = dialog.findChild<QLabel*>();
        auto* pathEdit = dialog.findChild<QLineEdit*>();
        auto* upButton = buttonNamed(dialog, QStringLiteral("Up"));
        auto* goButton = buttonNamed(dialog, QStringLiteral("Go"));
        require(entries != nullptr && buttons != nullptr && status != nullptr
                && pathEdit != nullptr && upButton != nullptr
                && goButton != nullptr,
            "browser did not create its widgets");
        require(entries->selectionMode() == QAbstractItemView::SingleSelection,
            "single browser allows more than one selection");
        auto* open = buttons->button(QDialogButtonBox::Open);
        require(open != nullptr && !open->isEnabled(),
            "Open was enabled before anything was selected");

        auto* first = waitForEntry(application, *entries, "plt00000");
        require(first != nullptr, "browser did not list the plotfile");
        require(first->text(1) == QStringLiteral("AMReX plotfile"),
            "plotfile was not typed as one");
        auto* sub = findEntry(*entries, "sub");
        require(sub != nullptr && sub->text(1) == QStringLiteral("Directory"),
            "plain directory was not listed as one");
        require(!(sub->flags() & Qt::ItemIsSelectable),
            "a plain directory was selectable");
        require(samePath(dialog.currentDirectory().toStdString(), root)
                && samePath(pathEdit->text().toStdString(), root),
            "browser did not report the listed directory");
        require(upButton->isEnabled(), "Up was disabled below the root");

        first->setSelected(true);
        application.processEvents();
        require(open->isEnabled(), "Open stayed disabled with a selection");
        auto selected = dialog.selectedPaths();
        require(selected.size() == 1
                && samePath(selected.front(), root / "plt00000"),
            "browser did not return the selected plotfile");

        // Enter the subdirectory the way a double-click does.
        emit entries->itemActivated(sub, 0);
        require(waitForEntry(application, *entries, "plt00002") != nullptr,
            "browser did not enter the subdirectory");
        require(samePath(dialog.currentDirectory().toStdString(), root / "sub"),
            "current directory did not follow the navigation");
        require(!open->isEnabled(), "Open survived a directory change");
        require(dialog.selectedPaths().empty(),
            "a selection survived a directory change");

        upButton->click();
        require(waitForEntry(application, *entries, "plt00001") != nullptr
                && samePath(dialog.currentDirectory().toStdString(), root),
            "Up did not return to the parent directory");

        // A path that is not a directory: error shown, listing kept.
        pathEdit->setText(
            QString::fromStdString((root / "plt00000" / "Header").string()));
        goButton->click();
        require(waitFor(application,
                    [&] {
                        return status->text().contains(
                            QStringLiteral("Could not list"));
                    }),
            "unreadable path did not report an error");
        require(samePath(dialog.currentDirectory().toStdString(), root)
                && findEntry(*entries, "plt00001") != nullptr,
            "a failed listing replaced the previous one");
        require(pathEdit->isEnabled() && goButton->isEnabled(),
            "controls stayed disabled after a failed listing");
    }

    // Sequence mode: several plotfiles, returned in name order regardless of
    // the order they were selected in.
    {
        RemoteFileDialog dialog(connection,
            QString::fromStdString(root.string()), Mode::PlotfileSequence);
        auto* entries = dialog.findChild<QTreeWidget*>();
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        require(entries != nullptr && buttons != nullptr,
            "sequence browser did not create its widgets");
        require(
            entries->selectionMode() == QAbstractItemView::ExtendedSelection,
            "sequence browser does not allow multiple selection");
        auto* later = waitForEntry(application, *entries, "plt00001");
        auto* earlier = findEntry(*entries, "plt00000");
        require(later != nullptr && earlier != nullptr,
            "sequence browser did not list the plotfiles");
        later->setSelected(true);
        earlier->setSelected(true);
        application.processEvents();
        const auto selected = dialog.selectedPaths();
        require(selected.size() == 2
                && samePath(selected.front(), root / "plt00000")
                && samePath(selected.back(), root / "plt00001"),
            "sequence browser did not return the plotfiles in name order");
        require(buttons->button(QDialogButtonBox::Open)->isEnabled(),
            "Open stayed disabled with plotfiles selected");
    }

    connection->close();
    return 0;
}
