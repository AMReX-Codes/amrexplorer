# PROTOTYPE: remove or absorb once the FlatBuffers wire design is validated.

set(amrvis_wire_schema "${CMAKE_CURRENT_SOURCE_DIR}/amrvis_wire.fbs")
set(amrvis_wire_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(amrvis_wire_generated
    "${amrvis_wire_generated_dir}/amrvis_wire_generated.h")

add_custom_command(
    OUTPUT "${amrvis_wire_generated}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${amrvis_wire_generated_dir}"
    COMMAND "$<TARGET_FILE:flatc>"
        --cpp
        --warnings-as-errors
        -o "${amrvis_wire_generated_dir}"
        "${amrvis_wire_schema}"
    DEPENDS flatc "${amrvis_wire_schema}"
    COMMENT "Generating the AMReXplorer FlatBuffers wire prototype"
    VERBATIM
)

add_library(amrvis_wire_prototype
    Frame.cpp
    Frame.hpp
    "${amrvis_wire_generated}"
)
target_include_directories(amrvis_wire_prototype
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${amrvis_wire_generated_dir}"
)
target_link_libraries(amrvis_wire_prototype
    PUBLIC FlatBuffers::FlatBuffers
    PRIVATE amrexplorer::warnings
)
target_compile_features(amrvis_wire_prototype PUBLIC cxx_std_20)

add_executable(amrvis_wire_server server.cpp)
target_link_libraries(amrvis_wire_server
    PRIVATE
        amrvis_wire_prototype
        amrexplorer::query
        amrexplorer::warnings
)
target_compile_features(amrvis_wire_server PRIVATE cxx_std_20)

add_executable(amrvis_wire_client client.cpp)
target_link_libraries(amrvis_wire_client
    PRIVATE amrvis_wire_prototype amrexplorer::warnings
)
target_compile_features(amrvis_wire_client PRIVATE cxx_std_20)
