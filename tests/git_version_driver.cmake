# Checks cmake/GitVersion.cmake against the two source trees it has to survive
# and the build cannot arrange for itself: one with no git history, which is how
# a release tarball or an unpacked copy is built, and one whose tag name carries
# characters that a C++ string literal and a CMake list each take badly.
# Expected -D arguments:
#   GENERATOR         path to cmake/GitVersion.cmake
#   WORK              directory to write into
#   GIT_EXECUTABLE    git, for the tag case; that case is skipped without it
#   CXX_COMPILER      compiler, to syntax-check the generated header
#   CXX_COMPILER_ID   its CMake id, since only some are driven the same way
foreach(argument GENERATOR WORK)
    if(NOT DEFINED ${argument})
        message(FATAL_ERROR "git_version_driver.cmake requires -D${argument}=...")
    endif()
endforeach()

# A directory that is not a git checkout, and is not inside this one either --
# under WORK, which is in the build tree.
set(source "${WORK}/no-git-source")
set(output "${WORK}/GitDescribe.hpp")
file(MAKE_DIRECTORY "${source}")
file(REMOVE "${output}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${source}" "-DOUTPUT=${output}"
        -P "${GENERATOR}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(NOT status STREQUAL "0")
    message(FATAL_ERROR
        "the generator failed on a tree with no git history (${status}):\n"
        "${out}${err}")
endif()
if(NOT EXISTS "${output}")
    message(FATAL_ERROR "the generator wrote no header")
endif()

file(READ "${output}" header)
if(NOT header MATCHES "#define AMREXPLORER_GIT_DESCRIBE \"\"")
    message(FATAL_ERROR
        "a tree with no git history produced a description:\n${header}")
endif()

# Run again: with nothing changed the header must be left untouched, not merely
# rewritten identically. It is regenerated on every build, so a write each time
# would give it a new timestamp and recompile the file that includes it forever.
# The second of sleep is what makes an unwanted write visible in the timestamp.
file(TIMESTAMP "${output}" before "%Y%m%d%H%M%S" UTC)
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${source}" "-DOUTPUT=${output}"
        -P "${GENERATOR}"
    RESULT_VARIABLE status)
if(NOT status STREQUAL "0")
    message(FATAL_ERROR "the generator failed on its second run (${status})")
endif()
file(TIMESTAMP "${output}" after "%Y%m%d%H%M%S" UTC)
if(NOT before STREQUAL after)
    message(FATAL_ERROR
        "the generator rewrote an unchanged header, ${before} then ${after}")
endif()

# --- a tag name carrying characters the generator has to survive -------------
# git rejects spaces, control characters and a few glob characters in a ref, but
# permits `"` and `;`. The first would end the C++ string literal the
# description lands in; the second is a list separator to CMake, and vanishes
# from a list that is joined again. Both have to reach the header intact, and
# the header still has to compile.
if(NOT GIT_EXECUTABLE)
    message(STATUS "no git executable given: skipping the tag-escaping case")
    return()
endif()

set(tagged "${WORK}/tagged-source")
set(taggedOutput "${WORK}/GitDescribeTagged.hpp")
file(REMOVE_RECURSE "${tagged}")
file(MAKE_DIRECTORY "${tagged}")

macro(run_git)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -c user.email=test@example.invalid
            -c user.name=test -c commit.gpgsign=false ${ARGN}
        WORKING_DIRECTORY "${tagged}"
        RESULT_VARIABLE gitStatus
        OUTPUT_QUIET
        ERROR_VARIABLE gitError)
    if(NOT gitStatus STREQUAL "0")
        message(FATAL_ERROR "git ${ARGN} failed (${gitStatus}): ${gitError}")
    endif()
endmacro()

run_git(init -q .)
run_git(commit -q --allow-empty -m "tag holder")
# Not through run_git: its ${ARGN} is a list, and the `;` in the tag would split
# there into two arguments -- the very mangling this case exists to catch, one
# layer up. Quoted argument, passed straight to git.
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -c user.email=test@example.invalid
        -c user.name=test tag "v9.9.9-test\"quote;semi"
    WORKING_DIRECTORY "${tagged}"
    RESULT_VARIABLE gitStatus
    OUTPUT_QUIET
    ERROR_VARIABLE gitError)
if(NOT gitStatus STREQUAL "0")
    message(FATAL_ERROR "git tag failed (${gitStatus}): ${gitError}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${tagged}"
        "-DOUTPUT=${taggedOutput}" -P "${GENERATOR}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(NOT status STREQUAL "0")
    message(FATAL_ERROR
        "the generator failed on an awkward tag (${status}):\n${out}${err}")
endif()

# Spelled out rather than derived from the generator's own escaping, which is
# the thing under test: the quote arrives backslash-escaped, the semicolon
# arrives as itself.
file(READ "${taggedOutput}" taggedHeader)
if(NOT taggedHeader MATCHES
        "AMREXPLORER_GIT_DESCRIBE \"v9[.]9[.]9-test[\\]\"quote;semi\"")
    message(FATAL_ERROR
        "the tag did not reach the header intact:\n${taggedHeader}")
endif()

# And the escaping is only right if what it produces is a header a compiler
# accepts -- a stray quote closes the literal early and nothing else notices.
if(CXX_COMPILER AND CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    set(probe "${WORK}/tagged-probe.cpp")
    file(WRITE "${probe}"
        "#include \"GitDescribeTagged.hpp\"\n"
        "const char* describe = AMREXPLORER_GIT_DESCRIBE;\n")
    execute_process(
        COMMAND "${CXX_COMPILER}" -std=c++20 -fsyntax-only "-I${WORK}" "${probe}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    if(NOT status STREQUAL "0")
        message(FATAL_ERROR
            "the generated header does not compile (${status}):\n${out}${err}")
    endif()
else()
    message(STATUS "no usable compiler given: skipping the header compile")
endif()
