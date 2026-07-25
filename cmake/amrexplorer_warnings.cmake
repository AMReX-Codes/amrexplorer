add_library(amrexplorer_warnings INTERFACE)
add_library(amrexplorer::warnings ALIAS amrexplorer_warnings)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(amrexplorer_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
    )
    if(AMREXPLORER_WARNINGS_AS_ERRORS)
        target_compile_options(amrexplorer_warnings INTERFACE -Werror)
    endif()
elseif(MSVC)
    target_compile_options(amrexplorer_warnings INTERFACE /W4)
    if(AMREXPLORER_WARNINGS_AS_ERRORS)
        target_compile_options(amrexplorer_warnings INTERFACE /WX)
    endif()
endif()
