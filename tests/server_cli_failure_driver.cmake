if(NOT DEFINED AMREXPLORER_SERVER)
    message(FATAL_ERROR "AMREXPLORER_SERVER was not provided")
endif()

execute_process(
    COMMAND "${AMREXPLORER_SERVER}" --max-datasets 0
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

if(result EQUAL 0)
    message(FATAL_ERROR
        "amrexplorer-server accepted the invalid --max-datasets 0 option")
endif()

set(combined_output "${output}${error}")
if(NOT combined_output MATCHES "--max-datasets must be greater than zero")
    message(FATAL_ERROR
        "amrexplorer-server did not print the expected diagnostic:\n"
        "${combined_output}")
endif()

execute_process(
    COMMAND "${AMREXPLORER_SERVER}" --write-stall-timeout-seconds 0
    RESULT_VARIABLE stall_timeout_result
    OUTPUT_VARIABLE stall_timeout_output
    ERROR_VARIABLE stall_timeout_error
)

if(stall_timeout_result EQUAL 0)
    message(FATAL_ERROR
        "amrexplorer-server accepted an invalid zero write stall timeout")
endif()

set(stall_timeout_combined_output
    "${stall_timeout_output}${stall_timeout_error}")
if(NOT stall_timeout_combined_output MATCHES
        "--write-stall-timeout-seconds must be greater than zero")
    message(FATAL_ERROR
        "amrexplorer-server did not print the expected stall-timeout "
        "diagnostic:\n${stall_timeout_combined_output}")
endif()
