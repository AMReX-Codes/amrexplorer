if(NOT DEFINED AMREXPLORER_SERVER)
    message(FATAL_ERROR "AMREXPLORER_SERVER was not provided")
endif()

# Each row is: the flag, its bad value, and the diagnostic the server owes the
# operator for it. Table-driven because every size flag carries its own
# hand-written range check, and a check that quietly stops rejecting is
# indistinguishable from one that never ran.
set(cases
    "--max-datasets|0|--max-datasets must be greater than zero"
    "--write-stall-timeout-seconds|0|--write-stall-timeout-seconds must be greater than zero"
    "--max-volume-voxels|0|--max-volume-voxels is outside its allowed range"
    "--max-volume-voxels|999999999999|--max-volume-voxels is outside its allowed range"
    "--volume-cache-mib|0|--volume-cache-mib is outside its allowed range"
    "--volume-cache-mib|65537|--volume-cache-mib is outside its allowed range"
)

foreach(case IN LISTS cases)
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 flag)
    list(GET parts 1 value)
    list(GET parts 2 expected)

    execute_process(
        COMMAND "${AMREXPLORER_SERVER}" "${flag}" "${value}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )

    if(result EQUAL 0)
        message(FATAL_ERROR
            "amrexplorer-server accepted the invalid ${flag} ${value} option")
    endif()

    set(combined_output "${output}${error}")
    if(NOT combined_output MATCHES "${expected}")
        message(FATAL_ERROR
            "amrexplorer-server did not print the expected diagnostic for "
            "${flag} ${value}:\n${combined_output}")
    endif()
endforeach()

# Not a range check: two flags that cannot both be given.
execute_process(
    COMMAND "${AMREXPLORER_SERVER}" --stdio --port 1
    RESULT_VARIABLE stdio_port_result
    OUTPUT_VARIABLE stdio_port_output
    ERROR_VARIABLE stdio_port_error
)

if(stdio_port_result EQUAL 0)
    message(FATAL_ERROR
        "amrexplorer-server accepted --stdio together with --port")
endif()

set(stdio_port_combined_output "${stdio_port_output}${stdio_port_error}")
if(NOT stdio_port_combined_output MATCHES
        "--stdio and --port are mutually exclusive")
    message(FATAL_ERROR
        "amrexplorer-server did not print the expected --stdio/--port "
        "diagnostic:\n${stdio_port_combined_output}")
endif()
