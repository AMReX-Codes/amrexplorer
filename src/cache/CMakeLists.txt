add_library(amrexplorer_cache CacheMetrics.cpp)
add_library(amrexplorer::cache ALIAS amrexplorer_cache)

target_include_directories(amrexplorer_cache
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(amrexplorer_cache PUBLIC amrexplorer::core PRIVATE amrexplorer::warnings)
target_compile_features(amrexplorer_cache PUBLIC cxx_std_20)
