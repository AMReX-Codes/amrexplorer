// The version text policy: a development version names the commit it came
// from, a release version does not, and a build with no git history to read
// names nothing. The composition is a pure function so all four combinations
// are testable without a git tree, which is exactly the case (release tarball,
// unpacked copy, no git installed) hardest to reproduce in a test.

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

    // A release version is the tag, so it never repeats it back, whether or
    // not the tree it was built from had git information to offer.
    require(versionText("0.4.0", "v0.4.0") == "0.4.0",
        "a release version repeated its own tag");
    require(versionText("0.4.0", "v0.4.0-3-g0fabe23-dirty") == "0.4.0",
        "a release version carried git information");
    require(versionText("0.4.0", "") == "0.4.0",
        "a release version with no git information was not itself");

    // "-dev" is what marks a development version, not a substring of it.
    require(versionText("0.4.0-devel", "0fabe23") == "0.4.0-devel",
        "a version merely starting with -dev was treated as a development one");

    // This build's own string starts with this build's own version.
    const std::string own = versionText();
    require(own.rfind(amrvis::kVersion, 0) == 0,
        "versionText() did not start with kVersion");

    return 0;
}
