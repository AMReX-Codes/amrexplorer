add_library(amrexplorer_pipeline
    DisplayCoordinator.cpp
    ParticleProjection.cpp
    SlicePipeline.cpp
    SliceRangeResolver.cpp
)
add_library(amrexplorer::pipeline ALIAS amrexplorer_pipeline)

target_include_directories(amrexplorer_pipeline
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(amrexplorer_pipeline
    PUBLIC
        amrexplorer::core
        amrexplorer::data
        amrexplorer::io
        amrexplorer::query
        amrexplorer::render2d
    PRIVATE amrexplorer::warnings
)
target_compile_features(amrexplorer_pipeline PUBLIC cxx_std_20)
