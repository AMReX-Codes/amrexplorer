# Drives one Qt smoke test end to end: materializes FAB payloads into a fresh
# copy of a metadata-only fixture (tests/fixture_materializer), then runs the
# Qt smoke binary on it offscreen. Expected -D arguments:
#   MATERIALIZER  path to the fixture_materializer executable
#   AMREXPLORER_QT     path to the amrexplorer_qt executable
#   SOURCE        fixture source directory (e.g. tests/data/plotfile_2d)
#   WORK          directory the materialized copies are written into
#   MODE          slice | expression-editor | expression-range | sequence |
#                 missing-range | non-finite | raw-fab | multifab-fab
foreach(argument MATERIALIZER AMREXPLORER_QT SOURCE WORK MODE)
    if(NOT DEFINED ${argument})
        message(FATAL_ERROR "qt_smoke_driver.cmake requires -D${argument}=...")
    endif()
endforeach()

set(ENV{QT_QPA_PLATFORM} offscreen)

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

if(MODE STREQUAL "slice")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --slice-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "expression-editor")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --expression-editor-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "expression-range")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --expression-range-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "sequence")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
    file(READ "${WORK}/plt00010/Header" secondHeader)
    string(REPLACE "\ndensity\n" "\nrho\n" secondHeader "${secondHeader}")
    file(WRITE "${WORK}/plt00010/Header" "${secondHeader}")
    run_or_die("${AMREXPLORER_QT}" --sequence-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "missing-range")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt" "--no-statistics")
    run_or_die("${AMREXPLORER_QT}" --missing-range-smoke-test
        "${WORK}/plt/Level_0/Cell")
elseif(MODE STREQUAL "non-finite")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt"
        "--no-statistics" "--non-finite")
    run_or_die("${AMREXPLORER_QT}" --missing-range-smoke-test
        "${WORK}/plt/Level_0/Cell")
elseif(MODE STREQUAL "raw-fab")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --raw-fab-smoke-test
        "${WORK}/plt/Level_0/Cell_D_00000")
elseif(MODE STREQUAL "multifab-fab")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --multifab-fab-smoke-test
        "${WORK}/plt/Level_0/Cell")
else()
    message(FATAL_ERROR "qt_smoke_driver.cmake: unknown MODE '${MODE}'")
endif()
