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
