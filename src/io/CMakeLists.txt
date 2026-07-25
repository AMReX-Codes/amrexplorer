add_library(amrexplorer_io
    plotfile/FabCatalog.cpp
    plotfile/PlotfileBlockReader.cpp
    plotfile/PlotfileDataset.cpp
    plotfile/PlotfileMetadataReader.cpp
    plotfile/StandaloneMetadataReader.cpp
)
add_library(amrexplorer::io ALIAS amrexplorer_io)

target_include_directories(amrexplorer_io
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(amrexplorer_io
    PUBLIC amrexplorer::cache amrexplorer::core
    PRIVATE amrexplorer::warnings
)
target_compile_features(amrexplorer_io PUBLIC cxx_std_20)
