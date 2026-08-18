add_library(amrexplorer_core
    CoordinateSystem.cpp
    Metadata.cpp
    OrthoProjection.cpp
    Request.cpp
    Statistics.cpp
    Volume.cpp
)
add_library(amrexplorer::core ALIAS amrexplorer_core)

target_include_directories(amrexplorer_core
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(amrexplorer_core PRIVATE amrexplorer::warnings)
target_compile_features(amrexplorer_core PUBLIC cxx_std_20)
