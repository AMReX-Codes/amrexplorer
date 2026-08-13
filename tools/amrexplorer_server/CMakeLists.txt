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
if(AMREXPLORER_SERVER_STATIC_CXX_RUNTIME
        AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(amrexplorer_server
        PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS amrexplorer_server RUNTIME DESTINATION bin)
