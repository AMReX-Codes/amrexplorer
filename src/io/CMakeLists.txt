add_library(amrexplorer_io
    AmrDataset.cpp
    DatasetOpen.cpp
    FitsWriter.cpp
    openpmd/OpenPmd.cpp
    plotfile/FabCatalog.cpp
    plotfile/PlotfileBlockReader.cpp
    plotfile/PlotfileDataset.cpp
    plotfile/PlotfileMetadataReader.cpp
    plotfile/PlotfileSource.cpp
    plotfile/ParticleReader.cpp
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

if(AMREXPLORER_ENABLE_OPENPMD)
    target_sources(amrexplorer_io PRIVATE openpmd/OpenPmdSource.cpp)
    target_link_libraries(amrexplorer_io PRIVATE openPMD::openPMD)
    target_compile_definitions(amrexplorer_io PRIVATE AMREXPLORER_HAS_OPENPMD=1)
endif()
