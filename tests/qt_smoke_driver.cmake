# Drives one Qt smoke test end to end: materializes FAB payloads into a fresh
# copy of a metadata-only fixture (tests/fixture_materializer), then runs the
# Qt smoke binary on it offscreen. Expected -D arguments:
#   MATERIALIZER  path to the fixture_materializer executable
#   AMREXPLORER_QT     path to the amrexplorer_qt executable
#   SOURCE        fixture source directory (e.g. tests/data/plotfile_2d)
#   WORK          directory the materialized copies are written into
#   MODE          slice | sequence | missing-range | non-finite | raw-fab |
#                 multifab-fab | quit | quit-on-failure | export-quit |
#                 contour-sync
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
elseif(MODE STREQUAL "sequence")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
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
elseif(MODE STREQUAL "contour-sync")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --contour-sync-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "raw-fab")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --raw-fab-smoke-test
        "${WORK}/plt/Level_0/Cell_D_00000")
elseif(MODE STREQUAL "multifab-fab")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --multifab-fab-smoke-test
        "${WORK}/plt/Level_0/Cell")
elseif(MODE STREQUAL "quit")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --quit-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "quit-on-failure")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    # Drop the FAB payloads so the slice read fails while metadata stays valid
    # (a non-modal background failure), then quit.
    file(GLOB fabPayloads "${WORK}/plt/Level_*/*_D_*")
    foreach(payload ${fabPayloads})
        file(REMOVE "${payload}")
    endforeach()
    run_or_die("${AMREXPLORER_QT}" --quit-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "export-quit")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
    # Stand-in ffmpeg: answers -version so the app enables encoding, then blocks
    # on the encode call. A quit while it is "encoding" must cancel and
    # terminate it; if the encoder is not bounded the process never exits and
    # the ctest TIMEOUT fails the test.
    set(fakeBin "${WORK}/bin")
    file(MAKE_DIRECTORY "${fakeBin}")
    # exec so the tracked child IS the sleep, not a /bin/sh (dash) wrapper that
    # dies on SIGTERM and orphans its sleep child for the full hour. This makes
    # the stand-in a single process that terminate() actually kills.
    file(WRITE "${fakeBin}/ffmpeg"
"#!/bin/sh
if [ \"$1\" = \"-version\" ]; then
  echo 'ffmpeg version 0.0-fake'
  exit 0
fi
exec sleep 3600
")
    execute_process(COMMAND chmod 755 "${fakeBin}/ffmpeg")
    set(ENV{PATH} "${fakeBin}:$ENV{PATH}")
    run_or_die("${AMREXPLORER_QT}" --export-quit-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
else()
    message(FATAL_ERROR "qt_smoke_driver.cmake: unknown MODE '${MODE}'")
endif()
