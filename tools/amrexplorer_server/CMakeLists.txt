add_executable(amrexplorer_server main.cpp)
set_target_properties(
    amrexplorer_server PROPERTIES OUTPUT_NAME amrexplorer-server)
target_link_libraries(amrexplorer_server
    PRIVATE amrexplorer::remote amrexplorer::warnings)
target_compile_features(amrexplorer_server PRIVATE cxx_std_20)

install(TARGETS amrexplorer_server RUNTIME DESTINATION bin)
