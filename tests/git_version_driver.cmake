# Checks the one case cmake/GitVersion.cmake exists to survive: a source tree
# with no git history, which is how a release tarball or an unpacked copy is
# built. The generator must still write its header, and write an empty
# description into it, so --version falls back to the plain version rather than
# failing the build or reporting some other repository's commits. Expected -D
# arguments:
#   GENERATOR  path to cmake/GitVersion.cmake
#   WORK       directory to write into
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
