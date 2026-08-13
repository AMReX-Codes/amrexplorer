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
if(AMREXPLORER_SERVER_STATIC_CXX_RUNTIME
        AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckCXXSourceCompiles)
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
endif()

install(TARGETS amrexplorer_server RUNTIME DESTINATION bin)
