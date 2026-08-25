#include "SmokeHarnessInternal.hpp"

#include "MainWindow.hpp"

#include <QSettings>
#include <QString>

#include <cstdlib>
#include <string_view>

#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <initializer_list>
#include <memory>
#include <optional>

// The offscreen smoke-test harnesses, moved out of main() as they were and
// then split by theme (SmokeHarness<Theme>.cpp): each branch arms connections
// and timers on the window and the application for one --*-smoke-test /
// --*-repro option, and main() then runs exec(). They drive the window
// through its ForTest accessors, so they compile only where those do
// (AMREXPLORER_BUILD_TESTS), and the release binary carries neither.

namespace amrvis::qt::smoke {

void attachSmokeServer(amrvis::qt::MainWindow& window,
    const std::shared_ptr<amrvis::remote::Server>& server)
{
    window.useRemoteConnection(
        std::make_shared<amrvis::remote::Connection>("127.0.0.1",
            server->port(),
            amrvis::remote::ConnectionOptions{
                .clientName = "AMReXplorer Qt smoke",
                .sessionToken = server->token()}),
        QStringLiteral("127.0.0.1:%1").arg(server->port()));
}

void isolateSettings(int argc, char** argv)
{
    if (argc < 2) {
        return;
    }
    const std::string_view option(argv[1]);
    if (option.find("-smoke-test") == std::string_view::npos
        && option.find("-repro") == std::string_view::npos) {
        return;
    }
#if defined(_MSC_VER)
    char* configured = nullptr;
    std::size_t configuredSize = 0;
    if (::_dupenv_s(&configured, &configuredSize, "XDG_CONFIG_HOME") != 0) {
        return;
    }
    const auto directory = configured == nullptr
        ? QString() : QString::fromLocal8Bit(configured);
    std::free(configured);
#else
    const auto* configured = std::getenv("XDG_CONFIG_HOME");
    const auto directory = configured == nullptr
        ? QString() : QString::fromLocal8Bit(configured);
#endif
    if (directory.isEmpty()) {
        return;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(
        QSettings::IniFormat, QSettings::UserScope, directory);
}

Outcome dispatch(Context& context)
{
    // One theme file per family of scenarios (see SmokeHarnessInternal.hpp);
    // the first that recognises argv[1] arms it. None recognises a
    // production option, so an unrecognised one falls through to main().
    for (auto* const themed : {
             &dispatchRemote,
             &dispatchLifecycle,
             &dispatchRange,
             &dispatchZoom,
             &dispatchFab,
             &dispatchSequence,
             &dispatchVolume,
             &dispatchDerived
         }) {
        if (auto outcome = themed(context); outcome.handled) {
            return outcome;
        }
    }
    return {false, std::nullopt};
}

void shutdown(Context& context)
{
    if (context.server) {
        context.server->requestStop();
    }
    if (context.serverThread) {
        context.serverThread->join();
    }
}

} // namespace amrvis::qt::smoke
