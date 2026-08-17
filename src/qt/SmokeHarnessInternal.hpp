#pragma once

// Shared by the smoke-harness theme files (SmokeHarness*.cpp): each theme
// claims its own --*-smoke-test / --*-repro options through one of these
// dispatchers, and SmokeHarness.cpp's dispatch() tries them in turn. Every
// option is claimed by exactly one theme, so the order is only a reading
// order. Compiled only with AMREXPLORER_BUILD_TESTS, like SmokeHarness.hpp.

#include "SmokeHarness.hpp"

namespace amrvis::qt::smoke {

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

} // namespace amrvis::qt::smoke
