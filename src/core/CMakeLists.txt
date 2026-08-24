add_library(amrexplorer_core
    CoordinateSystem.cpp
    DerivedField.cpp
    Metadata.cpp
    OrthoProjection.cpp
    Request.cpp
    Statistics.cpp
    Volume.cpp
)
add_library(amrexplorer::core ALIAS amrexplorer_core)

target_include_directories(amrexplorer_core
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# expression is PUBLIC: DerivedField.hpp exposes CompiledExpression.
target_link_libraries(amrexplorer_core
    PUBLIC amrexplorer::expression
    PRIVATE amrexplorer::warnings)
target_compile_features(amrexplorer_core PUBLIC cxx_std_20)

# Version.cpp is deliberately not part of amrexplorer_core. It is the one file
# that sees the generated git description, and that description changes every
# time the working tree goes clean or dirty and on every commit -- so in core it
# re-archived the library and relinked every binary that links it, ~70 of them,
# for a string two of them print. On its own it costs one compile and the few
# links that actually need it.
add_library(amrexplorer_version Version.cpp)
add_library(amrexplorer::version ALIAS amrexplorer_version)
target_include_directories(amrexplorer_version
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    # The generated header is this target's business alone.
    PRIVATE "${AMREXPLORER_GENERATED_INCLUDE_DIR}")
add_dependencies(amrexplorer_version amrexplorer_git_describe)
target_link_libraries(amrexplorer_version PRIVATE amrexplorer::warnings)
target_compile_features(amrexplorer_version PUBLIC cxx_std_20)
# A tag name may be Unicode. MSVC reads a BOM-less file in the active code page
# and warns (C4819) on a character it cannot represent, which /WX makes fatal;
# /utf-8 settles both the source and execution charset. Every other compiler
# here is UTF-8 already.
target_compile_options(amrexplorer_version
    PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
