set(amrexplorer_wire_schema
    "${PROJECT_SOURCE_DIR}/schemas/amrexplorer_wire.fbs")
set(amrexplorer_wire_generated_dir
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(amrexplorer_wire_generated
    "${amrexplorer_wire_generated_dir}/amrexplorer_wire_generated.h")

add_custom_command(
    OUTPUT "${amrexplorer_wire_generated}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${amrexplorer_wire_generated_dir}"
    COMMAND "$<TARGET_FILE:flatc>"
        --cpp
        --gen-object-api
        --scoped-enums
        --warnings-as-errors
        -o "${amrexplorer_wire_generated_dir}"
        "${amrexplorer_wire_schema}"
    DEPENDS flatc "${amrexplorer_wire_schema}"
    COMMENT "Generating production AMReXplorer wire bindings"
    VERBATIM
)

add_library(amrexplorer_remote
    Codec.cpp
    Codec.hpp
    Connection.cpp
    Frame.cpp
    Server.cpp
    "${amrexplorer_wire_generated}"
)
add_library(amrexplorer::remote ALIAS amrexplorer_remote)

target_include_directories(amrexplorer_remote
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE "${amrexplorer_wire_generated_dir}"
)
target_link_libraries(amrexplorer_remote
    PUBLIC amrexplorer::data
    PRIVATE
        FlatBuffers::FlatBuffers
        Threads::Threads
        amrexplorer::warnings
)
if(WIN32)
    target_link_libraries(amrexplorer_remote PRIVATE ws2_32)
endif()
target_compile_features(amrexplorer_remote PUBLIC cxx_std_20)
