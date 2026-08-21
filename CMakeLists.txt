cmake_minimum_required(VERSION 3.25)

project(
    amrexplorer
    VERSION 0.3.0
    DESCRIPTION "Demand-driven AMR visualization"
    LANGUAGES CXX
)

option(AMREXPLORER_ENABLE_QT "Build the Qt 6 desktop application" ON)
option(AMREXPLORER_ENABLE_VTK "Build the optional VTK volume module" OFF)
option(AMREXPLORER_ENABLE_REMOTE
    "Build production remote client/server support" ${AMREXPLORER_ENABLE_QT})
option(AMREXPLORER_BUILD_TESTS "Build unit and integration tests" ON)
option(AMREXPLORER_ENABLE_SERVER_TEST_HOOKS
    "Enable internal remote-server synchronization hooks for tests" OFF)
option(AMREXPLORER_BUILD_MACOS_APP_BUNDLE
    "Build the Qt application as a macOS .app bundle" ${APPLE})
option(AMREXPLORER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(AMREXPLORER_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(AMREXPLORER_ENABLE_TSAN "Enable ThreadSanitizer (mutually exclusive with AMREXPLORER_ENABLE_SANITIZERS)" OFF)
option(AMREXPLORER_LIBFUZZER
    "Build the fuzz harnesses as coverage-guided libFuzzer drivers (Clang)" OFF)
option(AMREXPLORER_FORCE_FETCH_FLATBUFFERS
    "Skip installed FlatBuffers discovery and use the pinned fallback" OFF)
# Off under the sanitizers, where it is both pointless and a hazard: every
# sanitizer runtime pulls in libgcc_s.so.1, so -static-libgcc buys nothing, and
# libubsan pulls libstdc++.so.6 too -- the process would then carry a statically
# linked copy inside the executable alongside that one. The portability this
# option exists for only concerns release and remote builds.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND NOT AMREXPLORER_ENABLE_SANITIZERS
        AND NOT AMREXPLORER_ENABLE_TSAN)
    set(amrexplorer_static_cxx_default ON)
else()
    set(amrexplorer_static_cxx_default OFF)
endif()
option(AMREXPLORER_SERVER_STATIC_CXX_RUNTIME
    "Link amrexplorer-server against a static libstdc++/libgcc"
    ${amrexplorer_static_cxx_default})
unset(amrexplorer_static_cxx_default)

option(AMREXPLORER_USER_INSTALL_PREFIX
    "Default the install prefix to the platform's per-user location" OFF)

# CMake would default the install prefix to /usr/local, which essentially
# nobody who builds this can write to: the users are HPC users without root and
# desktop users who would otherwise reach for sudo. Default to the per-user
# location each platform actually uses instead.
#
# Opt-in, and set by the presets rather than unconditionally, because changing
# a cached prefix is not free for packagers: DESTDIR is *prepended* to the
# prefix rather than replacing it, so a DESTDIR-only staging install with this
# forced on would produce stage/home/<builder>/.local/bin/... and bake the
# builder's home directory into the package. Packagers invoke cmake directly
# rather than through a preset, so they keep CMake's own default and DESTDIR
# behaves as documented.
# DEFINED is not enough: HOME set but empty would yield a prefix of "/.local".
if(AMREXPLORER_USER_INSTALL_PREFIX
        AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT
        AND NOT WIN32 AND NOT "$ENV{HOME}" STREQUAL "")
    # Gated on the GUI actually being built, not just on the bundle option,
    # which defaults to ${APPLE} independently of it. A server-only build on a
    # Mac (`cmake --preset remote`) otherwise took the bundle branch and
    # installed ~/Applications/bin/amrexplorer-server with no .app anywhere.
    if(APPLE AND AMREXPLORER_ENABLE_QT AND AMREXPLORER_BUILD_MACOS_APP_BUNDLE)
        # The GUI installs as a .app bundle at the prefix root (BUNDLE
        # DESTINATION .), so here the prefix *is* the applications folder.
        # amrexplorer-server lands in a bin/ beside it, which is unusual for
        # macOS but harmless -- the server is a headless HPC tool and those
        # machines are not Macs.
        set(amrexplorer_default_prefix "$ENV{HOME}/Applications")
    else()
        # ~/.local is the XDG user prefix, and the choice is load-bearing
        # rather than cosmetic: ~/.local/share is the *default* $XDG_DATA_HOME,
        # which is where the desktop entry and icons below have to land to be
        # found at all. Installed under $HOME directly they are inert -- nothing
        # searches $HOME/share. ~/.local/bin is also on PATH by default on most
        # distributions.
        #
        # A user who has moved XDG_DATA_HOME elsewhere gets the installed entry
        # in a directory their desktop does not search. The application's own
        # ensureDesktopEntry (src/qt/main.cpp) writes through
        # QStandardPaths::GenericDataLocation and so honours the variable, which
        # means the menu entry still appears -- the two disagree only about
        # which copy is the live one, and the runtime one wins.
        set(amrexplorer_default_prefix "$ENV{HOME}/.local")
    endif()
    set(CMAKE_INSTALL_PREFIX "${amrexplorer_default_prefix}" CACHE PATH
        "Install path prefix, prepended onto install directories" FORCE)
    unset(amrexplorer_default_prefix)
endif()

if(AMREXPLORER_ENABLE_SERVER_TEST_HOOKS
    AND NOT AMREXPLORER_BUILD_TESTS)
    message(FATAL_ERROR
        "AMREXPLORER_ENABLE_SERVER_TEST_HOOKS requires AMREXPLORER_BUILD_TESTS=ON")
endif()

if(AMREXPLORER_ENABLE_QT AND NOT AMREXPLORER_ENABLE_REMOTE)
    message(FATAL_ERROR
        "The Qt application requires AMREXPLORER_ENABLE_REMOTE=ON. "
        "Use the headless preset for a build without remote support.")
endif()

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
    # The remedy matters more than the diagnosis here. This fails most often on
    # HPC login nodes, where the OS is several years old and /usr/bin/c++ is
    # too, but a perfectly good compiler is one `module load` away -- so say so
    # rather than leaving the reader to conclude the machine is unsupported.
    message(FATAL_ERROR
        "AMReXplorer requires a compiler and standard library with C++20 support "
        "(including <compare> and defaulted comparisons). "
        "The configured compiler is ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION} at ${CMAKE_CXX_COMPILER}.\n"
        "\n"
        "On an HPC system this usually means CMake picked up the system "
        "compiler in /usr/bin rather than a module-provided one. Load a newer "
        "toolchain and re-run the configure you just ran, adding --fresh and "
        "the compiler -- keeping your own preset, since the one below is only "
        "an example:\n"
        "    module load gcc\n"
        "    cmake --preset remote --fresh -DCMAKE_CXX_COMPILER=g++\n"
        "On Cray systems (Perlmutter, Frontier) the compiler wrapper is CC:\n"
        "    cmake --preset remote --fresh -DCMAKE_CXX_COMPILER=CC\n"
        "--fresh matters: this configure has already written a cache naming the "
        "old compiler, and CMake normally clears such a cache and retries "
        "within the same run -- but the error above aborts before it can, so "
        "without --fresh the next attempt fails identically and only a third "
        "would succeed.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(amrexplorer_warnings)

if(AMREXPLORER_ENABLE_SANITIZERS AND AMREXPLORER_ENABLE_TSAN)
    message(FATAL_ERROR
        "AMREXPLORER_ENABLE_SANITIZERS and AMREXPLORER_ENABLE_TSAN are mutually exclusive")
endif()

if(AMREXPLORER_ENABLE_SANITIZERS)
    include(amrexplorer_sanitizers)
    amrexplorer_enable_sanitizers()
endif()

if(AMREXPLORER_ENABLE_TSAN)
    include(amrexplorer_sanitizers)
    amrexplorer_enable_tsan()
endif()

if(AMREXPLORER_LIBFUZZER)
    include(amrexplorer_sanitizers)
    amrexplorer_enable_libfuzzer_instrumentation()
endif()

add_subdirectory(src/core)
add_subdirectory(src/cache)
add_subdirectory(src/io)
add_subdirectory(src/query)
add_subdirectory(src/render3d)
add_subdirectory(src/data)
if(AMREXPLORER_ENABLE_REMOTE)
    if(NOT AMREXPLORER_FORCE_FETCH_FLATBUFFERS)
        find_package(Flatbuffers CONFIG QUIET)
    endif()
    if(Flatbuffers_FOUND)
        if(NOT TARGET flatbuffers::flatbuffers
            OR NOT TARGET flatbuffers::flatc)
            message(FATAL_ERROR
                "The installed FlatBuffers package must provide both "
                "flatbuffers::flatbuffers and flatbuffers::flatc")
        endif()
        set(AMREXPLORER_FLATBUFFERS_LIBRARY_TARGET
            flatbuffers::flatbuffers)
        set(AMREXPLORER_FLATC_TARGET flatbuffers::flatc)
        message(STATUS "Using installed FlatBuffers package")
    else()
        include(FetchContent)
        set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(FLATBUFFERS_BUILD_GRPCTEST OFF CACHE BOOL "" FORCE)
        set(FLATBUFFERS_BUILD_SHAREDLIB OFF CACHE BOOL "" FORCE)
        set(FLATBUFFERS_BUILD_STATICLIB OFF CACHE BOOL "" FORCE)
        set(FLATBUFFERS_BUILD_FLATC ON CACHE BOOL "" FORCE)
        set(FLATBUFFERS_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(flatbuffers
            GIT_REPOSITORY https://github.com/google/flatbuffers.git
            GIT_TAG v25.12.19
            GIT_SHALLOW TRUE
            SYSTEM
        )
        FetchContent_MakeAvailable(flatbuffers)
        # GCC reports a bogus -Wstringop-overflow inside the std::vector insert
        # that flatbuffers::AddFlatBuffer performs; the destination is a vector
        # GCC cannot see the capacity of after inlining. Upstream knows about
        # it (google/flatbuffers#7366) but only demotes it from an error under
        # FLATBUFFERS_STRICT_MODE, so it still shows up in our build on the GCC
        # versions that emit it. Nothing we compile is at fault, so silence it
        # on the dependency rather than let it bury our own warnings.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            foreach(_fb_target flatbuffers flatc)
                if(TARGET ${_fb_target})
                    target_compile_options(${_fb_target}
                        PRIVATE -Wno-stringop-overflow)
                endif()
            endforeach()
        endif()
        set(AMREXPLORER_FLATBUFFERS_LIBRARY_TARGET
            FlatBuffers::FlatBuffers)
        set(AMREXPLORER_FLATC_TARGET flatc)
        message(STATUS "Using pinned FetchContent FlatBuffers fallback")
    endif()
    add_subdirectory(src/remote)
    add_subdirectory(tools/amrexplorer_server)
    add_subdirectory(tools/render_equivalence)
endif()
add_subdirectory(src/render2d)
add_subdirectory(src/pipeline)

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
