add_library(amrexplorer_viewer
    ViewerSession.cpp
)
add_library(amrexplorer::viewer ALIAS amrexplorer_viewer)

target_include_directories(amrexplorer_viewer
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(amrexplorer_viewer
    PUBLIC amrexplorer::io
    PRIVATE amrexplorer::warnings
)
target_compile_features(amrexplorer_viewer PUBLIC cxx_std_20)
