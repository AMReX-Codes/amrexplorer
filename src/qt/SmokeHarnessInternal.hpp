#pragma once

// Shared by the smoke-harness theme files (SmokeHarness*.cpp): each theme
// claims its own --*-smoke-test / --*-repro options through one of these
// dispatchers, and SmokeHarness.cpp's dispatch() tries them in turn. Every
// option is claimed by exactly one theme, so the order is only a reading
// order. Compiled only with AMREXPLORER_BUILD_TESTS, like SmokeHarness.hpp.

#include "SmokeHarness.hpp"

#include "MainWindow.hpp"

#include <QString>

#include <amrexplorer/remote/Connection.hpp>
#include <amrexplorer/remote/Server.hpp>

#include <memory>

namespace amrvis::qt::smoke {

// The smoke tests speak to an in-process loopback server; the handshake
// against it takes milliseconds, so it runs right here on the GUI thread.
// Shared, so the client name and the options cannot drift between themes.
inline void attachSmokeServer(amrvis::qt::MainWindow& window,
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

// SmokeHarnessRemote.cpp
Outcome dispatchRemote(Context& context);
// SmokeHarnessLifecycle.cpp
Outcome dispatchLifecycle(Context& context);
// SmokeHarnessRange.cpp
Outcome dispatchRange(Context& context);
// SmokeHarnessZoom.cpp
Outcome dispatchZoom(Context& context);
// SmokeHarnessFab.cpp
Outcome dispatchFab(Context& context);
// SmokeHarnessSequence.cpp
Outcome dispatchSequence(Context& context);
// SmokeHarnessVolume.cpp
Outcome dispatchVolume(Context& context);

} // namespace amrvis::qt::smoke
