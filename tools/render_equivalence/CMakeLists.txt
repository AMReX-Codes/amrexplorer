add_executable(amrexplorer_render_equivalence main.cpp)
set_target_properties(amrexplorer_render_equivalence PROPERTIES
    OUTPUT_NAME amrexplorer-render-equivalence)
target_link_libraries(amrexplorer_render_equivalence
    PRIVATE
        amrexplorer::pipeline
        amrexplorer::remote
        amrexplorer::render2d
        amrexplorer::warnings
        Threads::Threads
)
target_compile_features(amrexplorer_render_equivalence PRIVATE cxx_std_20)

# Not installed by default: this is a local/remote render comparison tool for
# development, documented nowhere a user would look, and it was landing in
# everyone's bin/ beside the two executables they asked for. Still installable
# deliberately, for comparison work on a machine without a build tree:
#     cmake --install build --component tools
install(TARGETS amrexplorer_render_equivalence
    RUNTIME DESTINATION bin
    COMPONENT tools
    EXCLUDE_FROM_ALL)
