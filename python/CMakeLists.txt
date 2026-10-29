# The amrexplorer Python package: the nanobind extension built here, next to
# the pure-Python modules copied from python/amrexplorer, so that
# ${PROJECT_BINARY_DIR}/python is an importable tree (PYTHONPATH) without an
# install step. pip installs through scikit-build-core (pyproject.toml), which
# sets SKBUILD and takes the install() rules below instead.
find_package(Python 3.9 REQUIRED
    COMPONENTS Interpreter Development.Module
    OPTIONAL_COMPONENTS Development.SABIModule)

if(NOT nanobind_DIR AND NOT nanobind_ROOT)
    execute_process(
        COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        OUTPUT_VARIABLE amrexplorer_nanobind_dir
        RESULT_VARIABLE amrexplorer_nanobind_status)
    if(amrexplorer_nanobind_status EQUAL 0)
        set(nanobind_ROOT "${amrexplorer_nanobind_dir}")
    endif()
endif()
find_package(nanobind CONFIG)
if(NOT nanobind_FOUND)
    message(FATAL_ERROR
        "AMREXPLORER_ENABLE_PYTHON needs nanobind for ${Python_EXECUTABLE}. "
        "Install it there (`${Python_EXECUTABLE} -m pip install nanobind`) or "
        "point -DPython_EXECUTABLE at an interpreter that has it.")
endif()

set(AMREXPLORER_PYTHON_PACKAGE_DIR "${PROJECT_BINARY_DIR}/python/amrexplorer")

nanobind_add_module(_amrexplorer NB_STATIC src/module.cpp)
target_link_libraries(_amrexplorer PRIVATE
    amrexplorer::analysis amrexplorer::pipeline amrexplorer::render2d
    amrexplorer::render3d amrexplorer::query amrexplorer::io amrexplorer::core)
set_target_properties(_amrexplorer PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${AMREXPLORER_PYTHON_PACKAGE_DIR}")

file(GLOB amrexplorer_python_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/amrexplorer/*.py")
set(amrexplorer_python_copies)
foreach(source IN LISTS amrexplorer_python_sources)
    get_filename_component(name "${source}" NAME)
    set(copy "${AMREXPLORER_PYTHON_PACKAGE_DIR}/${name}")
    add_custom_command(OUTPUT "${copy}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${source}" "${copy}"
        DEPENDS "${source}"
        VERBATIM)
    list(APPEND amrexplorer_python_copies "${copy}")
endforeach()
add_custom_target(amrexplorer_python_package ALL
    DEPENDS ${amrexplorer_python_copies})
add_dependencies(_amrexplorer amrexplorer_python_package)

if(SKBUILD)
    install(TARGETS _amrexplorer LIBRARY DESTINATION amrexplorer)
    install(FILES ${amrexplorer_python_sources} DESTINATION amrexplorer)
endif()
