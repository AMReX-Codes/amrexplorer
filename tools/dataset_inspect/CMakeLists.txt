add_executable(dataset_inspect main.cpp)
target_link_libraries(dataset_inspect PRIVATE amrexplorer::io amrexplorer::warnings)
target_compile_features(dataset_inspect PRIVATE cxx_std_20)

