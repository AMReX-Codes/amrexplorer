# Runs a Qt smoke test under a comma-decimal locale.
#
# The application pins LC_NUMERIC to "C" after constructing QApplication,
# because Qt's setlocale(LC_ALL, "") otherwise hands the C locale to the
# environment and the plotfile reader's strtod stops at the '.' in "0.5" --
# under which no plotfile opens at all. This exercises that pin end to end:
# without it the slice smoke test fails to open its fixture.
#
# Expected -D arguments: MATERIALIZER, AMREXPLORER_QT, SOURCE, WORK.
#
# Skips -- visibly, via ctest's SKIP_RETURN_CODE, never silently -- when no
# comma locale can be obtained. A silent pass is what let this defect hide:
# low-priority-robustness item 5 recorded "no German locale installed" and
# stopped there, when localedef needs no root.
foreach(argument MATERIALIZER AMREXPLORER_QT SOURCE WORK)
    if(NOT DEFINED ${argument})
        message(FATAL_ERROR "qt_locale_smoke_driver.cmake requires -D${argument}=...")
    endif()
endforeach()

# An installed comma locale, if there is one.
set(commaLocale "")
execute_process(COMMAND locale -a
    OUTPUT_VARIABLE installedLocales
    ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
foreach(candidate de_DE.UTF-8 de_DE.utf8 en_DK.UTF-8 en_DK.utf8 fr_FR.UTF-8 fr_FR.utf8)
    if(installedLocales MATCHES "(^|\n)${candidate}(\n|$)")
        set(commaLocale "${candidate}")
        break()
    endif()
endforeach()

# Otherwise build one. --no-archive plus LOCPATH needs no root.
set(localeRoot "${WORK}/locales")
if(commaLocale STREQUAL "")
    file(MAKE_DIRECTORY "${localeRoot}")
    execute_process(
        COMMAND localedef -i de_DE -f UTF-8 --no-archive "${localeRoot}/de_DE.UTF-8"
        RESULT_VARIABLE localedefResult ERROR_QUIET OUTPUT_QUIET)
    if(localedefResult STREQUAL "0")
        set(commaLocale "de_DE.UTF-8")
        set(ENV{LOCPATH} "${localeRoot}")
    endif()
endif()

if(commaLocale STREQUAL "")
    # The registration sets SKIP_REGULAR_EXPRESSION on this marker, so ctest
    # reports "Skipped" rather than a green pass. A cmake -P script cannot
    # choose its own exit code, which rules out SKIP_RETURN_CODE here.
    message(STATUS
        "AMREXPLORER_LOCALE_SKIP: no comma-decimal locale installed and "
        "localedef could not build one; the LC_NUMERIC pin was NOT exercised")
    return()
endif()

message(STATUS "exercising the LC_NUMERIC pin under ${commaLocale}")

set(ENV{QT_QPA_PLATFORM} offscreen)
file(REMOVE_RECURSE "${WORK}/config")
set(ENV{XDG_CONFIG_HOME} "${WORK}/config")

execute_process(COMMAND "${MATERIALIZER}" "${SOURCE}" "${WORK}/plt"
    RESULT_VARIABLE materializeResult
    OUTPUT_VARIABLE materializeOutput ERROR_VARIABLE materializeError)
if(NOT materializeResult STREQUAL "0")
    message(FATAL_ERROR
        "fixture materialization failed: ${materializeOutput}${materializeError}")
endif()

# LC_ALL so the whole environment, not just LC_NUMERIC, is the comma locale --
# which is what a user in that locale actually has.
set(ENV{LC_ALL} "${commaLocale}")
execute_process(COMMAND "${AMREXPLORER_QT}" --slice-smoke-test "${WORK}/plt"
    RESULT_VARIABLE smokeResult
    OUTPUT_VARIABLE smokeOutput ERROR_VARIABLE smokeError)
if(NOT smokeResult STREQUAL "0")
    message(FATAL_ERROR
        "the slice smoke test failed under ${commaLocale} (${smokeResult}): "
        "the LC_NUMERIC pin is not holding\n${smokeOutput}${smokeError}")
endif()
