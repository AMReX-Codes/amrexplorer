cmake_minimum_required(VERSION 3.25)

project(
    amrexplorer
    VERSION 0.1.0
    DESCRIPTION "Demand-driven AMR visualization"
    LANGUAGES CXX
)

option(AMREXPLORER_ENABLE_QT "Build the Qt 6 desktop application" ON)
option(AMREXPLORER_ENABLE_VTK "Build the optional VTK volume module" OFF)
option(AMREXPLORER_ENABLE_MPI "Enable MPI-aware backends" OFF)
option(AMREXPLORER_BUILD_TESTS "Build unit and integration tests" ON)
option(AMREXPLORER_BUILD_MACOS_APP_BUNDLE
    "Build the Qt application as a macOS .app bundle" ${APPLE})
option(AMREXPLORER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(AMREXPLORER_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CheckCXXSourceCompiles)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
check_cxx_source_compiles([=[
    #include <compare>

    struct Value {
        int value;

        constexpr auto operator<=>(const Value&) const = default;
    };

    int main()
    {
        static_assert(Value{1} < Value{2});
        return 0;
    }
]=] AMREXPLORER_HAS_REQUIRED_CXX20_SUPPORT)

if(NOT AMREXPLORER_HAS_REQUIRED_CXX20_SUPPORT)
    message(FATAL_ERROR
        "AMReXplorer requires a compiler and standard library with C++20 support "
        "(including <compare> and defaulted comparisons). "
        "The configured compiler is ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION} at ${CMAKE_CXX_COMPILER}.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(amrexplorer_warnings)

if(AMREXPLORER_ENABLE_SANITIZERS)
    include(amrexplorer_sanitizers)
    amrexplorer_enable_sanitizers()
endif()

if(AMREXPLORER_ENABLE_MPI)
    find_package(MPI REQUIRED COMPONENTS CXX)
endif()

add_subdirectory(src/core)
add_subdirectory(src/cache)
add_subdirectory(src/io)
add_subdirectory(src/query)
add_subdirectory(src/render2d)

if(AMREXPLORER_ENABLE_QT)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Concurrent Core Gui Widgets)
    add_subdirectory(src/qt)
endif()

if(AMREXPLORER_ENABLE_VTK)
    message(FATAL_ERROR
        "AMREXPLORER_ENABLE_VTK is not yet supported. The system VTK package "
        "is Qt 5-based and must not be linked into this Qt 6 application.")
endif()

add_subdirectory(tools/dataset_inspect)
add_subdirectory(tools/query_benchmark)
add_subdirectory(tools/line_repro)

if(AMREXPLORER_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
