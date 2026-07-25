add_library(amrexplorer_render2d
    BuiltinPalettes.cpp
    Contours.cpp
    ImageBuffer.cpp
    Palette.cpp
    ScalarRenderer.cpp
    VectorGlyphs.cpp
)
add_library(amrexplorer::render2d ALIAS amrexplorer_render2d)

target_include_directories(amrexplorer_render2d
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(amrexplorer_render2d PUBLIC amrexplorer::core PRIVATE amrexplorer::warnings)
target_compile_features(amrexplorer_render2d PUBLIC cxx_std_20)
