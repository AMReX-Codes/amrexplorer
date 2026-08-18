add_library(amrexplorer_data
    DatasetPage.cpp
    LocalDatasetSession.cpp
    SessionValidation.cpp
    ViewData.cpp
)
add_library(amrexplorer::data ALIAS amrexplorer_data)

target_include_directories(amrexplorer_data
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(amrexplorer_data
    PUBLIC
        amrexplorer::cache
        amrexplorer::core
        amrexplorer::io
        amrexplorer::query
        amrexplorer::render3d
    PRIVATE amrexplorer::warnings
)
target_compile_features(amrexplorer_data PUBLIC cxx_std_20)
