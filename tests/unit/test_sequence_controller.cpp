#include "SequenceController.hpp"

#include <QCoreApplication>
#include <QTimer>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

amrvis::qt::SequenceController::Hooks quietHooks()
{
    return amrvis::qt::SequenceController::Hooks{
        [] { return amrvis::FrameSliceSpec{}; },
        [](amrvis::InitialSliceResult&, bool) {},
        [] { return false; },
    };
}

// A frame load runs on the pool, so the reason it failed reaches the host only
// if the worker's exception message survives the watcher.
void workerExceptionDetailReachesTheHost(QCoreApplication& application)
{
    bool receivedExpectedMessage = false;
    amrvis::qt::SequenceController controller(quietHooks());
    QObject::connect(&controller,
        &amrvis::qt::SequenceController::frameLoadFailed, &application,
        [&application, &receivedExpectedMessage](const QString& message) {
            receivedExpectedMessage
                = message == QStringLiteral("specific frame-load failure");
            application.quit();
        });
    QTimer::singleShot(5000, &application, [&application] {
        std::cerr << "FAILED: timed out waiting for frame failure\n";
        application.exit(1);
    });

    controller.open(
        std::vector<std::filesystem::path>{"frame-0", "frame-1"},
        [](const std::filesystem::path&, amrvis::DatasetId,
            const amrvis::FrameSliceSpec&, amrvis::StopToken)
            -> amrvis::InitialSliceResult {
            throw std::runtime_error("specific frame-load failure");
        });
    require(application.exec() == 0 && receivedExpectedMessage,
        "worker exception detail was not preserved");
}

// Asking for the frame already on screen must not restart the switch.
// goToFrame used to suppress a duplicate index only while a load was in
// flight, so an idle press-and-release of the frame slider re-ran the whole
// thing: cancel the in-flight work, close the Dataset and Line Plot windows,
// then re-open and re-render the frame already displayed -- over the network,
// for a remote sequence.
//
// frameSwitchStarted is the property itself rather than a proxy for it: it is
// emitted synchronously, exactly when a switch proceeds. Counting it settles
// the question without waiting to see whether a redundant load *arrives*,
// which is all an end-to-end test can observe and is why the smoke test
// carries a timing margin.
void idleRequestForTheDisplayedFrameStartsNoSwitch(
    QCoreApplication& application)
{
    amrvis::qt::SequenceController controller(quietHooks());

    int switchesStarted = 0;
    QObject::connect(&controller,
        &amrvis::qt::SequenceController::frameSwitchStarted, &application,
        [&switchesStarted](int) { ++switchesStarted; });
    QObject::connect(&controller,
        &amrvis::qt::SequenceController::frameLoadFailed, &application,
        [&application](const QString&) {
            std::cerr << "FAILED: the opening frame did not load\n";
            application.exit(1);
        });
    QObject::connect(&controller,
        &amrvis::qt::SequenceController::frameDisplayed, &application,
        [&application, &controller, &switchesStarted](int index) {
            require(index == 0, "the sequence must open on frame 0");
            // Queued, so the controller is observed idle after finishLoad
            // returns rather than part-way through its own emission.
            QTimer::singleShot(0, &application,
                [&application, &controller, &switchesStarted] {
                    const auto before = switchesStarted;
                    controller.goToFrame(0);
                    require(switchesStarted == before,
                        "a request for the frame already on screen must not "
                        "start a switch");
                    controller.goToFrame(0, true);
                    require(switchesStarted == before + 1,
                        "forceRestart must reload even the displayed frame");
                    controller.cancelActiveWork();
                    application.quit();
                });
        });
    QTimer::singleShot(5000, &application, [&application] {
        std::cerr << "FAILED: timed out waiting for the opening frame\n";
        application.exit(1);
    });

    controller.open(
        std::vector<std::filesystem::path>{"frame-0", "frame-1"},
        [](const std::filesystem::path&, amrvis::DatasetId,
            const amrvis::FrameSliceSpec&, amrvis::StopToken) {
            return amrvis::InitialSliceResult{};
        });
    require(application.exec() == 0, "the redundant-request case did not run");
    // The opening load is a switch: m_index starts at -1, so suppressing
    // "the index is already current" must not swallow it.
    require(switchesStarted >= 1, "opening a sequence must start a switch");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    workerExceptionDetailReachesTheHost(application);
    idleRequestForTheDisplayedFrameStartsNoSwitch(application);
    return 0;
}
