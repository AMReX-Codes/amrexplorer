#pragma once

#include <string>
#include <string_view>

namespace amrvis {

// The version this build is, and the only place it is written down: "x.y.z" on
// a tagged release, "x.y.z-dev" while x.y.z is what the work is heading for.
// CMake reads the x.y.z out of the line below for project(VERSION), so keep it
// a single literal in this form.
inline constexpr const char* kVersion = "0.4.0";

// What --version prints after the program name: the version, and the commit it
// came from in parentheses whenever the build had one to name. gitDescribe is
// `git describe --tags --dirty --always` as of the build, and is empty for a
// source tree with no git history to read -- a release tarball, an unpacked
// copy, a machine without git -- in which case the version stands alone. A
// release build says it too: on a clean tag that repeats the version, and off
// one it is the difference that matters.
[[nodiscard]] std::string versionText(
    std::string_view version, std::string_view gitDescribe);

// The same for this build: kVersion, plus whatever git information the build
// captured.
[[nodiscard]] std::string versionText();

} // namespace amrvis
