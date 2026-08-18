#include "SmokeHarnessInternal.hpp"

#include <amrexplorer/remote/Server.hpp>

#include <initializer_list>
#include <optional>

// The offscreen smoke-test harnesses, moved out of main() as they were and
// then split by theme (SmokeHarness<Theme>.cpp): each branch arms connections
// and timers on the window and the application for one --*-smoke-test /
// --*-repro option, and main() then runs exec(). They drive the window
// through its ForTest accessors, so they compile only where those do
// (AMREXPLORER_BUILD_TESTS), and the release binary carries neither.

namespace amrvis::qt::smoke {

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
             &dispatchVolume
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
