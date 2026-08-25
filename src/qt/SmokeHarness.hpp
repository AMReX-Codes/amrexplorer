#pragma once

// The offscreen smoke-test harnesses behind amrexplorer's --*-smoke-test and
// --*-repro options. Compiled only with AMREXPLORER_BUILD_TESTS (they drive
// MainWindow through its ForTest accessors); main() hands them the window
// before running the event loop and they arm the connections and timers one
// scenario needs. See SmokeHarness.cpp.

#include <QApplication>

#include <memory>
#include <optional>
#include <thread>

namespace amrvis::remote {
class Server;
}

namespace amrvis::qt {
class MainWindow;
}

namespace amrvis::qt::smoke {

// What the harness branches share with main(): the window and application
// they arm against, the arguments, and the loopback server some scenarios
// start (main() stops and joins it after exec()).
struct Context {
    QApplication& application;
    MainWindow& window;
    int argc;
    char** argv;
    std::shared_ptr<remote::Server> server;
    std::optional<std::thread> serverThread;
};

struct Outcome {
    // argv[1] named a smoke option.
    bool handled = false;
    // Set when the scenario ends the process before the event loop (a usage
    // error, an up-front failure); main() returns it.
    std::optional<int> exitCode;
};

// Points QSettings at a store of this run's own, before anything reads one.
// A scenario persists settings deliberately -- the derived-field ones apply a
// definition in one run and expect it back in the next -- so each test needs
// its own store, which the driver arranges through XDG_CONFIG_HOME. QSettings
// honours that on Unix only: on Windows it is registry-backed, where every
// test of a run shares one key and they overwrite each other's state. Pinning
// the format and the path makes the driver's isolation hold everywhere. A
// no-op unless argv names a smoke scenario.
void isolateSettings(int argc, char** argv);

// Arms the scenario argv names, exactly as the inline branches in main() did:
// connections and single-shot timers on the window and the application.
// {false, nullopt} for a non-smoke option.
Outcome dispatch(Context& context);

// After exec(): stops and joins the loopback server, if one was started.
void shutdown(Context& context);

} // namespace amrvis::qt::smoke
