add_library(amrexplorer_core
    CoordinateSystem.cpp
    Metadata.cpp
    OrthoProjection.cpp
    Request.cpp
    Statistics.cpp
    Version.cpp
    Volume.cpp
)
add_library(amrexplorer::core ALIAS amrexplorer_core)

target_include_directories(amrexplorer_core
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# Version.cpp includes the generated GitDescribe.hpp; the directory is PRIVATE
# because no consumer of this library needs it, and the dependency keeps the
# header written before this target compiles.
target_include_directories(amrexplorer_core
    PRIVATE "${AMREXPLORER_GENERATED_INCLUDE_DIR}")
add_dependencies(amrexplorer_core amrexplorer_git_describe)
# Version.cpp carries whatever git printed, and a tag name may be Unicode.
# MSVC reads a BOM-less file in the active code page and warns (C4819) on a
# character it cannot represent, which /WX makes fatal; /utf-8 settles both the
# source and execution charset. Every other compiler here is UTF-8 already.
target_compile_options(amrexplorer_core
    PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(amrexplorer_core PRIVATE amrexplorer::warnings)
target_compile_features(amrexplorer_core PUBLIC cxx_std_20)
