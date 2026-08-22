#include "SmokeHarnessInternal.hpp"

#include "MainWindow.hpp"
#include "VolumeWindow.hpp"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QAbstractButton>
#include <QSet>
#include <QString>
#include <QTimer>

#include <amrexplorer/remote/Server.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

// Volume: the Volume Rendering window over a 3-D plotfile, opened locally
// and over an in-process server. Each waits for the initial slices, opens
// the window through the same path as the View menu action, and requires
// the first frame the controller displays to have lit some pixels -- the
// ray caster ran end to end, locally or on the server, and the frame came
// back through the session, the pipeline and the window. Coverage, not
// pixels: the exact picture depends on the viewport the platform gives an
// offscreen window.

namespace amrvis::qt::smoke {

namespace {

// Arms the volume checks on `window`: open the window once the initial
// slices are in, then exit 0 on the first displayed frame with coverage,
// 1 on one without, 2 on a failed load, 3 if no frame arrives in time.
void armVolumeChecks(amrvis::qt::MainWindow& window, QApplication& application)
{
    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [&window, &application](bool success) {
            if (!success) {
                application.exit(2);
                return;
            }
            window.showVolumeWindowForTest();
            if (!window.volumeWindowOpenForTest()) {
                qCritical("the volume window did not open");
                application.exit(1);
                return;
            }
        });
    QObject::connect(&window, &amrvis::qt::MainWindow::volumeFrameDisplayed,
        &application, [&window, &application] {
            const auto coverage = window.volumeFrameAlphaCoverageForTest();
            if (!(coverage > 0.0)) {
                qCritical("the volume frame lit no pixels");
                application.exit(1);
                return;
            }
            application.exit(0);
        });
    QTimer::singleShot(30000, &application, [&application] { application.exit(3); });
}

// The one Volume window: a QObject child of the main window, which parents it
// for placement.
amrvis::qt::VolumeWindow* volumeWindow(amrvis::qt::MainWindow& window)
{
    return window.findChild<amrvis::qt::VolumeWindow*>();
}

// Answers the modals an export raises, so the action can be triggered for real
// instead of the harness re-doing what the slot does. A poll rather than a
// single shot: one export can raise two in a row (the save dialog, then the
// changed-name question), each in its own nested event loop.
//
// The dialog is driven, not bypassed, because the bugs this covers were in the
// *use* of the name rule -- a harness that calls pngExportPath itself and
// asserts its own output proves nothing about the slot.
class ModalDriver {
public:
    explicit ModalDriver(QString typed)
        : m_typed(std::move(typed))
    {
        m_timer.setInterval(5);
        QObject::connect(&m_timer, &QTimer::timeout, [this] { pump(); });
        m_timer.start();
    }

    void expect(const QString& typed) { m_typed = typed; }
    [[nodiscard]] int fileDialogs() const noexcept { return m_fileDialogs; }
    [[nodiscard]] int messageBoxes() const noexcept { return m_messageBoxes; }
    // Something the driver could not drive; the run must fail rather than
    // report on counters taken in that state.
    [[nodiscard]] bool stuck() const noexcept { return m_stuck; }
    void resetCounts()
    {
        m_fileDialogs = 0;
        m_messageBoxes = 0;
    }

private:
    void pump()
    {
        auto* modal = QApplication::activeModalWidget();
        if (modal == nullptr) {
            return;
        }
        if (auto* save = qobject_cast<QFileDialog*>(modal)) {
            ++m_fileDialogs;
            save->selectFile(m_typed);
            // Through the meta-object: QFileDialog redeclares accept() as
            // protected, and this runs the dialog's real accept path (the one
            // a user's click runs) rather than done() closing it behind that
            // path's back.
            if (!QMetaObject::invokeMethod(save, "accept")) {
                // Otherwise pump() re-selects every 5 ms until the watchdog
                // fires, with nothing in the log to say why.
                qCritical("could not accept the save dialog");
                m_stuck = true;
                save->reject();
            }
            return;
        }
        if (auto* box = qobject_cast<QMessageBox*>(modal)) {
            // Only the window's own prompts are counted, by the name it sets
            // on them. QFileDialog raises its own "already exists / replace?"
            // warning, which this answers the same way -- counting both would
            // let Qt's box stand in for a prompt the slot stopped making. Not
            // by window title: macOS ignores it, so that counted nothing there.
            if (box->objectName() == QStringLiteral("volumeExportPrompt")) {
                ++m_messageBoxes;
            }
            // A specific button, not accept(): accept() leaves clickedButton()
            // null, which QMessageBox::question reads as the negative answer.
            if (auto* yes = box->button(QMessageBox::Yes)) {
                yes->click();
            } else if (auto* save = box->button(QMessageBox::Save)) {
                save->click();
            } else if (auto* ok = box->button(QMessageBox::Ok)) {
                ok->click();
            } else {
                box->accept();
            }
            return;
        }
        // A wrong state, not a nuisance: dismissing it quietly would let the
        // run finish green on a dialog nobody expected.
        qCritical("an unexpected modal was open during the export");
        m_stuck = true;
        modal->close();
    }

    QTimer m_timer;
    QString m_typed;
    int m_fileDialogs = 0;
    int m_messageBoxes = 0;
    bool m_stuck = false;
};

// Triggers File > Export Image... by name and returns once the whole export
// has run: getSaveFileName exec()s inside trigger(), so the driver's timers
// fire in that nested loop and trigger() returns after it closes.
bool triggerExport(amrvis::qt::VolumeWindow& volume)
{
    auto* action = volume.findChild<QAction*>(
        QStringLiteral("volumeExportImageAction"));
    if (action == nullptr) {
        qCritical("the export action is not reachable by name");
        return false;
    }
    action->trigger();
    return true;
}

// A rendered volume spans many pixel values; a bare background, or a
// background plus a white wireframe, spans a handful. Cheap proof that the
// frame reached the file without re-deriving what the frame should look like.
// The scan stops well above the threshold so the two are independent -- a cap
// equal to the threshold would only ever assert "the scan reached its cap".
constexpr int volumeColourThreshold = 9;
constexpr int colourScanCap = 64;

int distinctColours(const QImage& image, int cap)
{
    QSet<QRgb> seen;
    for (int y = 0; y < image.height() && seen.size() < cap; ++y) {
        for (int x = 0; x < image.width() && seen.size() < cap; ++x) {
            seen.insert(image.pixel(x, y));
        }
    }
    return static_cast<int>(seen.size());
}

// Drives File > Export Image... through the real action, three times: before
// any frame (the refusal), with a name that already says png (one dialog, no
// question), and with a bare name (the rule appends .png, which the slot must
// notice and report). What the unit test cannot cover is that the slot wires
// those rules together at all; what this must not do is re-derive the rules.
void armExportChecks(amrvis::qt::MainWindow& window, QApplication& application,
    const QString& stem)
{
    const auto driver = std::make_shared<ModalDriver>(
        stem + QStringLiteral(".png"));
    // Each export spins a nested event loop, which can deliver the next
    // render's frameDisplayed and re-enter these handlers -- clobbering the
    // counters the outer run is about to read. One pass each, as
    // SmokeHarnessSequence guards its own re-slices.
    const auto opened = std::make_shared<bool>(false);
    const auto exported = std::make_shared<bool>(false);

    QObject::connect(&window, &amrvis::qt::MainWindow::initialSliceFinished,
        &application, [&window, &application, driver, stem, opened](
                          bool success) {
            if (*opened) {
                return;
            }
            *opened = true;
            if (!success) {
                application.exit(2);
                return;
            }
            window.showVolumeWindowForTest();
            if (!window.volumeWindowOpenForTest()) {
                qCritical("the volume window did not open");
                application.exit(1);
                return;
            }
            auto* volume = volumeWindow(window);
            if (volume == nullptr) {
                qCritical("no volume window on screen");
                application.exit(1);
                return;
            }
            // No frame yet, so the export must refuse before reaching a
            // dialog. This is the only coverage the guard can get.
            driver->resetCounts();
            if (!triggerExport(*volume)) {
                application.exit(1);
                return;
            }
            if (driver->messageBoxes() != 1 || driver->fileDialogs() != 0) {
                qCritical("the export before the first frame did not refuse");
                application.exit(1);
                return;
            }
            if (QFileInfo::exists(stem + QStringLiteral(".png"))) {
                qCritical("the refused export wrote a file anyway");
                application.exit(1);
                return;
            }
            if (driver->stuck()) {
                application.exit(1);
            }
        });

    QObject::connect(&window, &amrvis::qt::MainWindow::volumeFrameDisplayed,
        &application, [&window, &application, driver, stem, exported] {
            if (*exported) {
                return;
            }
            *exported = true;
            auto* volume = volumeWindow(window);
            if (volume == nullptr) {
                qCritical("no volume window on screen");
                application.exit(1);
                return;
            }
            // A name that already says png: one dialog, and no question,
            // because the rule leaves such a name alone.
            driver->resetCounts();
            driver->expect(stem + QStringLiteral(".png"));
            if (!triggerExport(*volume)) {
                application.exit(1);
                return;
            }
            if (driver->fileDialogs() != 1 || driver->messageBoxes() != 0) {
                qCritical("exporting an unchanged name did not take one dialog");
                application.exit(1);
                return;
            }
            // Read after the export, not before it: showRendering makes the
            // "Rendering..." label visible in the same dock as the view, so
            // the view's size can change inside the dialog's nested loop and a
            // size sampled earlier would fail a correct export.
            const auto expected
                = volume->viewSize() * volume->devicePixelRatioF();
            QImage written;
            if (!written.load(stem + QStringLiteral(".png"))
                || written.size() != expected) {
                qCritical("the export wrote no file, or not at the view's size");
                application.exit(1);
                return;
            }
            if (distinctColours(written, colourScanCap)
                < volumeColourThreshold) {
                qCritical("the exported picture holds too few colours to be a "
                          "rendered volume");
                application.exit(1);
                return;
            }

            // A bare name: the rule appends .png, which the slot must notice
            // and report -- one dialog and one question.
            driver->resetCounts();
            driver->expect(stem + QStringLiteral("-renamed"));
            if (!triggerExport(*volume)) {
                application.exit(1);
                return;
            }
            if (driver->fileDialogs() != 1 || driver->messageBoxes() != 1) {
                qCritical("exporting a bare name did not report the new name");
                application.exit(1);
                return;
            }
            if (!QFileInfo::exists(stem + QStringLiteral("-renamed.png"))
                || QFileInfo::exists(stem + QStringLiteral("-renamed"))) {
                qCritical("the appended suffix did not reach the written file");
                application.exit(1);
                return;
            }
            if (QApplication::activeModalWidget() != nullptr) {
                qCritical("a modal was left open by the export");
                application.exit(1);
                return;
            }
            if (driver->stuck()) {
                application.exit(1);
                return;
            }
            application.exit(0);
        });
}

} // namespace

Outcome dispatchVolume(Context& context)
{
    auto& application = context.application;
    auto& window = context.window;
    const int argc = context.argc;
    char** argv = context.argv;
    auto& smokeServer = context.server;
    auto& smokeServerThread = context.serverThread;

    if (argc == 3 && std::string_view(argv[1]) == "--volume-smoke-test") {
        const std::filesystem::path path(argv[2]);
        armVolumeChecks(window, application);
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 4
        && std::string_view(argv[1]) == "--volume-export-smoke-test") {
        const std::filesystem::path path(argv[2]);
        // Not armVolumeChecks: its coverage check exits 0 on the first frame,
        // which would end the run before an export happened. armExportChecks
        // opens the window itself, so there is one handler for that here.
        armExportChecks(window, application, QString::fromUtf8(argv[3]));
        QTimer::singleShot(30000, &application,
            [&application] { application.exit(3); });
        QTimer::singleShot(0, &window, [&window, path] { window.openDataset(path); });
    } else if (argc == 3
        && std::string_view(argv[1]) == "--remote-volume-smoke-test") {
        smokeServer = std::make_shared<amrvis::remote::Server>();
        smokeServerThread.emplace(
            [server = smokeServer] { server->run(); });
        armVolumeChecks(window, application);
        QTimer::singleShot(0, &window,
            [&window, path = std::string(argv[2]), server = smokeServer] {
                attachSmokeServer(window, server);
                window.openRemoteDataset(path);
            });
    } else {
        return {false, std::nullopt};
    }
    return {true, std::nullopt};
}

} // namespace amrvis::qt::smoke
