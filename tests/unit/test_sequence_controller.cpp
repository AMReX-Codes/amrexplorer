#include "SequenceController.hpp"

#include <QCoreApplication>
#include <QTimer>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    bool receivedExpectedMessage = false;
    amrvis::qt::SequenceController controller(
        amrvis::qt::SequenceController::Hooks{
            [] { return amrvis::FrameSliceSpec{}; },
            [](amrvis::InitialSliceResult&, bool) {},
            [] { return false; },
        });
    QObject::connect(&controller,
        &amrvis::qt::SequenceController::frameLoadFailed,
        &application, [&application, &receivedExpectedMessage](
                          const QString& message) {
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
    const auto result = application.exec();
    if (result != 0 || !receivedExpectedMessage) {
        std::cerr << "FAILED: worker exception detail was not preserved\n";
        return 1;
    }
    return 0;
}
