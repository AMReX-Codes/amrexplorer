add_library(amrexplorer_query LineQuery.cpp SliceQuery.cpp)
add_library(amrexplorer::query ALIAS amrexplorer_query)

target_include_directories(amrexplorer_query
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(amrexplorer_query
    PUBLIC amrexplorer::core amrexplorer::io
    PRIVATE amrexplorer::warnings
)
target_compile_features(amrexplorer_query PUBLIC cxx_std_20)

