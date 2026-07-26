# Smoke-drives a standalone tool that reads FAB payloads: materializes FAB data
# into a fresh copy of a metadata-only fixture (tests/fixture_materializer),
# then runs the tool on it. The tool CLIs are fixed, so their arguments live
# here (mirroring qt_smoke_driver.cmake). Expected -D arguments:
#   MATERIALIZER  path to the fixture_materializer executable
#   TOOL          path to the tool executable
#   SOURCE        fixture source directory (e.g. tests/data/plotfile_2d)
#   WORK          directory the materialized copy is written into
#   MODE          query-benchmark | line-repro
foreach(argument MATERIALIZER TOOL SOURCE WORK MODE)
    if(NOT DEFINED ${argument})
        message(FATAL_ERROR "tools_smoke_driver.cmake requires -D${argument}=...")
    endif()
endforeach()

macro(run_or_die)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE stepResult
        OUTPUT_VARIABLE stepOutput
        ERROR_VARIABLE stepError)
    if(NOT stepResult STREQUAL "0")
        message(FATAL_ERROR
            "step failed (${stepResult}): ${ARGN}\n${stepOutput}${stepError}")
    endif()
endmacro()

run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")

if(MODE STREQUAL "query-benchmark")
    # Level 0, grid 0, field "density" (present in the 2-D fixture).
    run_or_die("${TOOL}" "${WORK}/plt" 0 0 density)
elseif(MODE STREQUAL "line-repro")
    # The line query completes near-instantly on this fixture, so a 30s watchdog
    # only trips on a real hang, not a slow machine (line_repro exits 3 on hang).
    run_or_die("${TOOL}" "${WORK}/plt" 30)
else()
    message(FATAL_ERROR "tools_smoke_driver.cmake: unknown MODE '${MODE}'")
endif()
