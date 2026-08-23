#include <amrexplorer/core/Version.hpp>

// Regenerated on every build by cmake/GitVersion.cmake, and rewritten only when
// what it says changes -- so a new commit recompiles this file and nothing else.
// It defines AMREXPLORER_GIT_DESCRIBE, empty where there is no git to ask.
#include <amrexplorer/GitDescribe.hpp>

#include <string>
#include <string_view>

namespace amrvis {

std::string versionText(std::string_view version, std::string_view gitDescribe)
{
    std::string text(version);
    // Only a development version names its commit. A release version is the
    // tag, so appending the tag back to it would say the same thing twice.
    const bool development = text.ends_with("-dev");
    if (development && !gitDescribe.empty()) {
        text += " (";
        text += gitDescribe;
        text += ')';
    }
    return text;
}

std::string versionText()
{
    return versionText(kVersion, AMREXPLORER_GIT_DESCRIBE);
}

} // namespace amrvis
