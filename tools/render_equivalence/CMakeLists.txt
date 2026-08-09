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

install(TARGETS amrexplorer_render_equivalence RUNTIME DESTINATION bin)
