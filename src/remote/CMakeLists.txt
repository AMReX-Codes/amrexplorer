add_library(amrexplorer_remote
    Frame.cpp
)
add_library(amrexplorer::remote ALIAS amrexplorer_remote)

target_include_directories(amrexplorer_remote
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(amrexplorer_remote PRIVATE amrexplorer::warnings)
if(WIN32)
    target_link_libraries(amrexplorer_remote PRIVATE ws2_32)
endif()
target_compile_features(amrexplorer_remote PUBLIC cxx_std_20)
