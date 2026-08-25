#pragma once

// Shared by the smoke-harness theme files (SmokeHarness*.cpp): each theme
// claims its own --*-smoke-test / --*-repro options through one of these
// dispatchers, and SmokeHarness.cpp's dispatch() tries them in turn. Every
// option is claimed by exactly one theme, so the order is only a reading
// order. Compiled only with AMREXPLORER_BUILD_TESTS, like SmokeHarness.hpp.

#include "SmokeHarness.hpp"

#include <memory>

namespace amrvis::qt {
class MainWindow;
}

namespace amrvis::remote {
class Server;
}

namespace amrvis::qt::smoke {

// The smoke tests speak to an in-process loopback server; the handshake
// against it takes milliseconds, so it runs right here on the GUI thread.
// Shared, so the client name and the options cannot drift between themes --
// declared here and defined in SmokeHarness.cpp, so the two themes that need
// a server do not put MainWindow and the transport headers in front of the
// six that do not.
void attachSmokeServer(amrvis::qt::MainWindow& window,
    const std::shared_ptr<amrvis::remote::Server>& server);

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
// SmokeHarnessDerived.cpp
Outcome dispatchDerived(Context& context);

} // namespace amrvis::qt::smoke
