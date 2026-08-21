add_library(amrexplorer_render3d
    VolumeRaycaster.cpp
)
add_library(amrexplorer::render3d ALIAS amrexplorer_render3d)

target_include_directories(amrexplorer_render3d
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(amrexplorer_render3d
    PUBLIC amrexplorer::core
    PRIVATE Threads::Threads amrexplorer::warnings
)
target_compile_features(amrexplorer_render3d PUBLIC cxx_std_20)
