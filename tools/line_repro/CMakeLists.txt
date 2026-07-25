add_executable(line_repro main.cpp)
target_link_libraries(
    line_repro
    PRIVATE
        amrexplorer::query
        amrexplorer::warnings
        Threads::Threads
)
target_compile_features(line_repro PRIVATE cxx_std_20)
