// The version text policy: the commit is named whenever the build had one to
// name, release or not, and nothing is named when there was none. The
// composition is a pure function so both cases are testable without a git
// tree, which is exactly the one (release tarball, unpacked copy, no git
// installed) hardest to reproduce in a test.

#include <amrexplorer/core/Version.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using amrvis::versionText;

    require(versionText("0.4.0-dev", "v0.3.0-42-g0fabe23")
            == "0.4.0-dev (v0.3.0-42-g0fabe23)",
        "a development version did not name its commit");
    require(versionText("0.4.0-dev", "v0.3.0-42-g0fabe23-dirty")
            == "0.4.0-dev (v0.3.0-42-g0fabe23-dirty)",
        "a dirty tree was not reported as such");
    // A shallow clone has no tags, so describe --always answers a bare hash.
    require(versionText("0.4.0-dev", "0fabe23") == "0.4.0-dev (0fabe23)",
        "a bare commit hash was not carried through");

    // No git history: a release tarball, an unpacked copy, or a machine with
    // no git. The version stands alone -- with no empty parentheses.
    require(versionText("0.4.0-dev", "") == "0.4.0-dev",
        "a build with no git information did not fall back to the version");

    // A release version says it too. On the tag itself that repeats the
    // version, which is the boring case; the ones that matter are a release
    // build made off the tag, or from a modified tree, where the difference is
    // the whole point.
    require(versionText("0.4.0", "v0.4.0") == "0.4.0 (v0.4.0)",
        "a release version dropped its description");
    require(versionText("0.4.0", "v0.4.0-3-g0fabe23-dirty")
            == "0.4.0 (v0.4.0-3-g0fabe23-dirty)",
        "a release build off the tag hid what it was built from");
    require(versionText("0.4.0", "") == "0.4.0",
        "a release version with no git information was not itself");

    // This build's own string starts with this build's own version.
    const std::string own = versionText();
    require(own.rfind(amrvis::kVersion, 0) == 0,
        "versionText() did not start with kVersion");

    return 0;
}
