add_library(amrexplorer_analysis
    Blocks.cpp
    Deposition.cpp
    LeafChunks.cpp
    Projection.cpp
    Surfaces.cpp
    Tiles.cpp
)
add_library(amrexplorer::analysis ALIAS amrexplorer_analysis)

target_include_directories(amrexplorer_analysis
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(amrexplorer_analysis
    PUBLIC amrexplorer::core amrexplorer::io
    PRIVATE amrexplorer::warnings
)
target_compile_features(amrexplorer_analysis PUBLIC cxx_std_20)
