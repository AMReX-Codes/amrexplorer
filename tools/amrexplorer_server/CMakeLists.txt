add_executable(amrexplorer_server main.cpp)
set_target_properties(
    amrexplorer_server PROPERTIES OUTPUT_NAME amrexplorer-server)
target_link_libraries(amrexplorer_server
    PRIVATE amrexplorer::remote amrexplorer::warnings)
target_compile_features(amrexplorer_server PRIVATE cxx_std_20)
target_compile_definitions(amrexplorer_server
    PRIVATE AMREXPLORER_VERSION="${PROJECT_VERSION}")

# The server's whole portability story is that it needs nothing but the C++
# runtime -- four shared libraries against the GUI's forty-three. Linking that
# runtime statically removes the most fragile of them: HPC users build under a
# `module load gcc`, and the resulting binary then fails for anyone who has not
# loaded that same module, because the module's libstdc++.so.6 is not on the
# default search path. Static here leaves only glibc, so "build once on the
# login node, copy it where you like" actually holds.
#
# Off outside Linux, and overridable: a distro packager wants the system
# runtime, and should configure with -DAMREXPLORER_SERVER_STATIC_CXX_RUNTIME=OFF.
# Probed rather than assumed: the static libstdc++ is a separate package on
# several distributions (Fedora's libstdc++-static, for one) and is not pulled
# in by an otherwise complete C++ toolchain. Enabling this blind would turn a
# working desktop build into a link failure, since the server is built by the
# default preset too. Fall back to the shared runtime with a warning instead.
# The sanitizer conditions are repeated here rather than left to the option's
# default, because a default only applies to a build tree's first configure: a
# directory configured before that default changed keeps ON in its cache and
# would still get the flags. Guarding where they are applied makes the
# behaviour right in every tree, including hand-configured ones that no preset
# covers.
#
# Restricted to the GNU and Clang families, whose handling of these flags is
# known, and *not* silently. Both halves matter, and each was learned the hard
# way. A probe alone is not enough: a driver that accepts an unknown switch and
# ignores it makes the probe succeed while still linking libstdc++ dynamically,
# so the build would claim a copyable binary and not have one. An allowlist
# alone is not enough either: when this was a bare condition the flags simply
# vanished on other toolchains while the cache still read ON and INSTALL.md
# still promised the static link. So: restrict, and say so when the restriction
# bites. GNU is what we recommend and test; Clang is included because the clang
# preset and the CI job build with it.
if(AMREXPLORER_SERVER_STATIC_CXX_RUNTIME
        AND NOT AMREXPLORER_ENABLE_SANITIZERS
        AND NOT AMREXPLORER_ENABLE_TSAN
        AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckCXXSourceCompiles)
    # Re-probed on every configure. check_cxx_source_compiles caches its result,
    # so a tree first configured on a machine without libstdc++.a would keep the
    # warning and the dynamic fallback for ever -- installing the package and
    # reconfiguring would change nothing, and the only way out would be to
    # delete the build directory. A warning that names a package to install has
    # to be actionable without that.
    unset(AMREXPLORER_STATIC_CXX_RUNTIME_LINKS CACHE)
    set(CMAKE_REQUIRED_LINK_OPTIONS -static-libstdc++ -static-libgcc)
    check_cxx_source_compiles([=[
        #include <stdexcept>
        #include <string>

        int main()
        {
            try {
                throw std::runtime_error(std::string("probe"));
            } catch (const std::exception& error) {
                return error.what() == nullptr ? 1 : 0;
            }
        }
    ]=] AMREXPLORER_STATIC_CXX_RUNTIME_LINKS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)

    if(AMREXPLORER_STATIC_CXX_RUNTIME_LINKS)
        target_link_options(amrexplorer_server
            PRIVATE -static-libstdc++ -static-libgcc)
    else()
        message(WARNING
            "A static libstdc++/libgcc is not available with this toolchain, so "
            "amrexplorer-server will link the shared C++ runtime. The binary "
            "then requires a matching libstdc++.so.6 wherever it runs, which "
            "matters on HPC systems where it was built under a compiler module. "
            "Install the static runtime (Fedora and RHEL: libstdc++-static) to "
            "get a freely copyable server, or set "
            "-DAMREXPLORER_SERVER_STATIC_CXX_RUNTIME=OFF to silence this.")
    endif()
elseif(AMREXPLORER_SERVER_STATIC_CXX_RUNTIME
        AND NOT AMREXPLORER_ENABLE_SANITIZERS
        AND NOT AMREXPLORER_ENABLE_TSAN)
    message(STATUS
        "amrexplorer-server: linking the shared C++ runtime, because this "
        "build uses ${CMAKE_CXX_COMPILER_ID} and only the GNU and Clang "
        "families are known to honour -static-libstdc++ rather than accept and "
        "ignore it. The server will need a matching libstdc++.so.6 wherever it "
        "runs, which on an HPC system means the compiler module it was built "
        "under. Build with GNU (module load gcc, or PrgEnv-gnu) for a freely "
        "copyable server.")
elseif(AMREXPLORER_SERVER_STATIC_CXX_RUNTIME)
    # Asked for, and vetoed above. Say so: the veto leaves the cache reading ON
    # and AMREXPLORER_STATIC_CXX_RUNTIME_LINKS possibly reading 1 from an earlier
    # configure, both describing a build that ignores them, and silence there is
    # how the stale-cache defect went unnoticed in the first place.
    message(STATUS
        "amrexplorer-server: linking the shared C++ runtime despite "
        "AMREXPLORER_SERVER_STATIC_CXX_RUNTIME=ON, because a sanitizer is "
        "enabled. Their runtimes load libstdc++.so.6 and libgcc_s.so.1 anyway, "
        "so a static copy inside the executable would only add a second one.")
endif()

install(TARGETS amrexplorer_server RUNTIME DESTINATION bin)
