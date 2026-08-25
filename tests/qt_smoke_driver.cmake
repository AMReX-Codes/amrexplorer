# Drives one Qt smoke test end to end: materializes FAB payloads into a fresh
# copy of a metadata-only fixture (tests/fixture_materializer), then runs the
# Qt smoke binary on it offscreen. Expected -D arguments:
#   MATERIALIZER  path to the fixture_materializer executable
#   AMREXPLORER_QT     path to the amrexplorer_qt executable
#   SOURCE        fixture source directory (e.g. tests/data/plotfile_2d)
#   WORK          directory the materialized copies are written into
#   MODE          slice | sequence | sequence-after-fab |
#                 derived-field-reload-race |
#                 remote-sequence-after-fab | missing-range |
#                 non-finite | raw-fab |
#                 multifab-fab | quit | quit-on-failure | window-close-pool |
#                 close-window |
#                 export-quit |
#                 contour-sync | raster-zoom | rubber-zoom-sync |
#                 particle-visible-range | particle-dialog |
#                 particle-settings-reset |
#                 rubber-zoom-local | rubber-overzoom | pan-zoom |
#                 range-cache | fab-zoom | cache-budget |
#                 fixed-scale-1 | fixed-scale-4 | sequence-transform-preserve |
#                 remote-fixed-scale | remote-fixed-scale-flicker |
#                 local-remote-fixed-scale-window |
#                 sequence-density-preserve |
#                 sequence-equal-size-transform-preserve |
#                 sequence-geometry-refit | sequence-noop | sequence-failure |
#                 remote-canvas-wheel | remote-cell-aspect | volume |
#                 derived-field | derived-field-sequence |
#                 derived-field-frames | derived-field-playback |
#                 scale-state | effective-scale |
#                 arrow-key-routing | animation-dock-role | open-failure |
#                 idle-ui-state | sequence-scale-report |
#                 spherical-scale-report |
#                 fixed-scale-centre | fab-overlap-failure |
#                 fab-direct-open-failure
foreach(argument MATERIALIZER AMREXPLORER_QT SOURCE WORK MODE)
    if(NOT DEFINED ${argument})
        message(FATAL_ERROR "qt_smoke_driver.cmake requires -D${argument}=...")
    endif()
endforeach()

set(ENV{QT_QPA_PLATFORM} offscreen)

# Isolate QSettings per run: a fresh, empty config directory makes every smoke
# test start from defaults, so persisted UI state (spherical display mode and
# supersample factor, palette, log scale, ...) never leaks between runs or from
# the developer's own config and skews an assertion.
file(REMOVE_RECURSE "${WORK}/config")
set(ENV{XDG_CONFIG_HOME} "${WORK}/config")

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
elseif(MODE STREQUAL "volume")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --volume-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "derived-field")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --derived-field-smoke-test "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --field-range-memory-smoke-test
        "${WORK}/plt")
elseif(MODE STREQUAL "derived-field-reload-race")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --derived-field-reload-race-smoke-test
        "${WORK}/plt")
elseif(MODE STREQUAL "derived-field-frames")
    # Two frames that do not list the same fields: the second drops the one a
    # definition reads, so that definition is left out of it and every id after
    # it moves. The one shape in which a carried field *index* means a
    # different field from one frame to the next.
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5"
        --drop-field density)
    run_or_die("${AMREXPLORER_QT}" --derived-field-frames-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "derived-field-playback")
    # A plane sweep never reopens the dataset, and a window counts as unable to
    # take derived fields for the whole of an open: two ways a committed
    # definition can fail to reach the session on screen.
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --derived-field-playback-smoke-test
        "${WORK}/plt")
elseif(MODE STREQUAL "derived-field-sequence")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
    run_or_die("${AMREXPLORER_QT}" --derived-field-sequence-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "sequence")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
    run_or_die("${AMREXPLORER_QT}" --sequence-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "sequence-noop")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
    run_or_die("${AMREXPLORER_QT}" --sequence-noop-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "sequence-failure")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
    # Drop the second frame's payloads only: its metadata still reads, so the
    # sequence opens and frame 0 displays, and the failure lands on the frame
    # step that playback makes.
    file(GLOB brokenPayloads "${WORK}/plt00010/Level_*/*_D_*")
    foreach(payload ${brokenPayloads})
        file(REMOVE "${payload}")
    endforeach()
    run_or_die("${AMREXPLORER_QT}" --sequence-failure-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "sequence-after-fab")
    # Open a raw FAB first, then a plotfile sequence: the sequence must clear
    # the stale FAB view state (dock, "— FAB" title).
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
    run_or_die("${AMREXPLORER_QT}" --sequence-after-fab-smoke-test
        "${WORK}/plt/Level_0/Cell_D_00000" "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "remote-sequence-after-fab")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
    run_or_die("${AMREXPLORER_QT}" --remote-sequence-after-fab-smoke-test
        "${WORK}/plt/Level_0/Cell_D_00000" "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "sequence-spec-change")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
    run_or_die("${AMREXPLORER_QT}" --sequence-spec-change-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "fixed-scale-1" OR MODE STREQUAL "fixed-scale-4")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    if(MODE STREQUAL "fixed-scale-1")
        set(factor 1)
    else()
        set(factor 4)
    endif()
    run_or_die("${AMREXPLORER_QT}" --fixed-scale-arrival-smoke-test
        "${WORK}/plt" "${factor}")
elseif(MODE STREQUAL "remote-fixed-scale")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --remote-fixed-scale-smoke-test
        "${WORK}/plt")
elseif(MODE STREQUAL "remote-fixed-scale-flicker")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --remote-fixed-scale-flicker-smoke-test
        "${WORK}/plt")
elseif(MODE STREQUAL "fixed-scale-centre")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --fixed-scale-centre-smoke-test
        "${WORK}/plt")
elseif(MODE STREQUAL "effective-scale")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --effective-scale-smoke-test
        "${WORK}/plt" 32)
elseif(MODE STREQUAL "spherical-scale-report")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --spherical-scale-report-smoke-test
        "${WORK}/plt")
elseif(MODE STREQUAL "idle-ui-state")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --idle-ui-state-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "open-failure")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --open-failure-smoke-test
        "${WORK}/no_such_plotfile" "${WORK}/plt")
elseif(MODE STREQUAL "sequence-scale-report")
    if(NOT DEFINED SECOND_SOURCE)
        message(FATAL_ERROR
            "sequence-scale-report requires -DSECOND_SOURCE=...")
    endif()
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${MATERIALIZER}" "${SECOND_SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SECOND_SOURCE}" "${WORK}/plt00010" "2.5")
    run_or_die("${AMREXPLORER_QT}" --sequence-scale-report-smoke-test
        "${WORK}/plt" "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "animation-dock-role")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
    run_or_die("${AMREXPLORER_QT}" --animation-dock-role-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010" "${WORK}/no_such_plotfile")
elseif(MODE STREQUAL "arrow-key-routing")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --arrow-key-routing-smoke-test
        "${WORK}/plt")
elseif(MODE STREQUAL "scale-state")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
    run_or_die("${AMREXPLORER_QT}" --scale-state-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "remote-cell-aspect")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --remote-cell-aspect-smoke-test
        "${WORK}/plt")
elseif(MODE STREQUAL "remote-canvas-wheel")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --remote-canvas-wheel-smoke-test
        "${WORK}/plt")
elseif(MODE STREQUAL "local-remote-fixed-scale-window")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}"
        --local-remote-fixed-scale-window-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "sequence-transform-preserve")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
    run_or_die("${AMREXPLORER_QT}" --sequence-transform-preserve-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "sequence-density-preserve")
    if(NOT DEFINED SECOND_SOURCE)
        message(FATAL_ERROR
            "sequence-density-preserve requires -DSECOND_SOURCE=...")
    endif()
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SECOND_SOURCE}" "${WORK}/plt00010")
    run_or_die("${AMREXPLORER_QT}" --sequence-density-preserve-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "sequence-equal-size-transform-preserve")
    if(NOT DEFINED SECOND_SOURCE)
        message(FATAL_ERROR
            "sequence-equal-size-transform-preserve requires -DSECOND_SOURCE=...")
    endif()
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SECOND_SOURCE}" "${WORK}/plt00010")
    run_or_die("${AMREXPLORER_QT}"
        --sequence-equal-size-transform-preserve-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "sequence-geometry-refit")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5"
        --domain-upper-x "2.0")
    run_or_die("${AMREXPLORER_QT}" --sequence-geometry-refit-smoke-test
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
elseif(MODE STREQUAL "visible-sync-staleness")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --visible-sync-staleness-smoke-test
        "${WORK}/plt")
elseif(MODE STREQUAL "particle-visible-range")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --particle-visible-range-smoke-test
        "${WORK}/plt")
elseif(MODE STREQUAL "particle-dialog")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
    run_or_die("${AMREXPLORER_QT}" --particle-dialog-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "particle-settings-reset")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "2.5")
    run_or_die("${AMREXPLORER_QT}" --particle-settings-reset-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "raster-zoom")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --raster-zoom-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "spherical-supersample")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --spherical-supersample-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "rubber-zoom-sync")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --rubber-zoom-sync-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "rubber-zoom-local")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --rubber-zoom-local-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "rubber-overzoom")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --rubber-overzoom-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "pan-zoom")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --pan-zoom-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "range-cache")
    # Two frames of the same fixture; frame 1's field is scaled 10x so its
    # range differs, making a stale range-cache reuse observable.
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00000")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt00010" "--scale" "10")
    run_or_die("${AMREXPLORER_QT}" --range-cache-smoke-test
        "${WORK}/plt00000" "${WORK}/plt00010")
elseif(MODE STREQUAL "raw-fab")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --raw-fab-smoke-test
        "${WORK}/plt/Level_0/Cell_D_00000")
elseif(MODE STREQUAL "multifab-fab")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --multifab-fab-smoke-test
        "${WORK}/plt/Level_0/Cell")
elseif(MODE STREQUAL "fab-overlap-failure")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --fab-overlap-failure-smoke-test
        "${WORK}/plt/Level_0/Cell_D_00000")
elseif(MODE STREQUAL "fab-direct-open-failure")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --fab-direct-open-failure-smoke-test
        "${WORK}/plt/Level_0/Cell_D_00000")
elseif(MODE STREQUAL "fab-zoom")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --fab-zoom-smoke-test
        "${WORK}/plt/Level_0/Cell")
elseif(MODE STREQUAL "cache-budget")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --cache-budget-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "quit")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --quit-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "window-close-pool")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --window-close-pool-smoke-test "${WORK}/plt")
elseif(MODE STREQUAL "close-window")
    run_or_die("${MATERIALIZER}" "${SOURCE}" "${WORK}/plt")
    run_or_die("${AMREXPLORER_QT}" --close-window-smoke-test "${WORK}/plt")
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
