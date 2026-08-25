# The fuzz harnesses build two ways. Default (AMREXPLORER_LIBFUZZER OFF): a
# deterministic, bounded ctest that runs fixed seeds + a fixed-PRNG stream, so
# CI and the sanitizers preset exercise the untrusted-input parsers on every
# run. ON (Clang only): each harness is compiled as a coverage-guided libFuzzer
# driver (LLVMFuzzerTestOneInput, its own main from the fuzzer runtime) to run
# by hand -- pair it with AMREXPLORER_ENABLE_SANITIZERS=ON, or the fuzzer
# only sees hard crashes; the coverage instrumentation itself is project-wide (see
# amrexplorer_enable_libfuzzer_instrumentation), so the fuzzer sees the
# parsers' own branches. Only the driver main is linked per target: it is
# what would conflict with the compiler check and every other executable.
function(amrexplorer_add_fuzz_target target)
    if(AMREXPLORER_LIBFUZZER)
        target_compile_definitions(${target} PRIVATE AMREXPLORER_LIBFUZZER)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer)
    else()
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES TIMEOUT 120)
    endif()
endfunction()

add_executable(test_expression unit/test_expression.cpp)
target_link_libraries(
    test_expression PRIVATE amrexplorer::expression amrexplorer::warnings
    Threads::Threads
)
add_test(NAME expression COMMAND test_expression)

add_executable(test_derived_field unit/test_derived_field.cpp)
target_link_libraries(
    test_derived_field PRIVATE amrexplorer::core amrexplorer::warnings)
add_test(NAME derived_field COMMAND test_derived_field)

add_executable(test_core_metadata unit/test_core_metadata.cpp)
target_link_libraries(test_core_metadata PRIVATE amrexplorer::core amrexplorer::warnings)
add_test(NAME core_metadata COMMAND test_core_metadata)

add_executable(test_ortho_projection unit/test_ortho_projection.cpp)
target_link_libraries(test_ortho_projection PRIVATE amrexplorer::core amrexplorer::warnings)
add_test(NAME ortho_projection COMMAND test_ortho_projection)

add_executable(test_volume_types unit/test_volume_types.cpp)
target_link_libraries(test_volume_types PRIVATE amrexplorer::core amrexplorer::warnings)
add_test(NAME volume_types COMMAND test_volume_types)

add_executable(test_volume_raycaster unit/test_volume_raycaster.cpp)
target_link_libraries(test_volume_raycaster PRIVATE amrexplorer::render3d amrexplorer::warnings Threads::Threads)
add_test(NAME volume_raycaster COMMAND test_volume_raycaster)

add_executable(test_geometry unit/test_geometry.cpp)
target_link_libraries(test_geometry PRIVATE amrexplorer::core amrexplorer::warnings)
add_test(NAME geometry COMMAND test_geometry)

add_executable(test_version unit/test_version.cpp)
target_link_libraries(test_version PRIVATE amrexplorer::version amrexplorer::warnings)
add_test(NAME version COMMAND test_version)

# The generator behind --version, against the two source trees no build here
# can arrange: one with no git history, as a release tarball is built from, and
# one whose tag name carries characters a C++ literal and a CMake list mangle.
find_package(Git QUIET)
add_test(
    NAME git_version_generator
    COMMAND "${CMAKE_COMMAND}"
        "-DGENERATOR=${PROJECT_SOURCE_DIR}/cmake/GitVersion.cmake"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/git_version_generator"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DCXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}"
        "-DCXX_COMPILER_FRONTEND_VARIANT=${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/git_version_driver.cmake"
)
set_tests_properties(git_version_generator PROPERTIES TIMEOUT 60)

# What the two binaries must print. Where this build reports a git description,
# it has to appear -- that end of the plumbing (generator to header to binary)
# is otherwise asserted nowhere. A tarball has none to carry, so the
# requirement follows the build rather than the presence of git.
if(AMREXPLORER_REPORTS_GIT_DESCRIBE)
    set(amrexplorer_version_tail " [(]")
else()
    set(amrexplorer_version_tail "")
endif()

add_executable(test_view_data unit/test_view_data.cpp)
target_link_libraries(test_view_data PRIVATE amrexplorer::data amrexplorer::warnings)
add_test(NAME view_data COMMAND test_view_data)

add_executable(test_session_validation unit/test_session_validation.cpp)
target_link_libraries(
    test_session_validation PRIVATE amrexplorer::data amrexplorer::warnings
)
add_test(NAME session_validation COMMAND test_session_validation)

add_executable(test_stop_token unit/test_stop_token.cpp)
target_compile_definitions(
    test_stop_token PRIVATE AMREXPLORER_TEST_FORCE_FALLBACK_STOP_TOKEN
)
target_link_libraries(
    test_stop_token PRIVATE amrexplorer::core amrexplorer::warnings
    Threads::Threads
)
add_test(NAME stop_token_fallback COMMAND test_stop_token)

add_executable(test_byte_lru_cache unit/test_byte_lru_cache.cpp)
target_link_libraries(
    test_byte_lru_cache
    PRIVATE amrexplorer::cache amrexplorer::warnings Threads::Threads
)
add_test(NAME byte_lru_cache COMMAND test_byte_lru_cache)

add_executable(test_cache_config unit/test_cache_config.cpp)
target_include_directories(test_cache_config PRIVATE "${CMAKE_SOURCE_DIR}/src/qt")
target_link_libraries(test_cache_config PRIVATE amrexplorer::warnings)
add_test(NAME cache_config COMMAND test_cache_config)
set_tests_properties(cache_config PROPERTIES
    ENVIRONMENT "AMREXPLORER_CACHE_SIZE_MB=768"
)

add_executable(test_scalar_renderer unit/test_scalar_renderer.cpp)
target_link_libraries(test_scalar_renderer PRIVATE amrexplorer::render2d amrexplorer::warnings)
add_test(NAME scalar_renderer COMMAND test_scalar_renderer)

add_executable(test_palette unit/test_palette.cpp)
target_link_libraries(test_palette PRIVATE amrexplorer::render2d amrexplorer::warnings)
add_test(
    NAME palette
    COMMAND test_palette "${CMAKE_CURRENT_SOURCE_DIR}/../palettes/rainbow.pal"
)

add_executable(test_contours unit/test_contours.cpp)
target_link_libraries(test_contours PRIVATE amrexplorer::render2d amrexplorer::warnings)
add_test(NAME contours COMMAND test_contours)

add_executable(test_spherical_coords unit/test_spherical_coords.cpp)
target_link_libraries(test_spherical_coords
    PRIVATE amrexplorer::core amrexplorer::render2d amrexplorer::warnings)
add_test(NAME spherical_coords COMMAND test_spherical_coords)

add_executable(test_vector_glyphs unit/test_vector_glyphs.cpp)
target_link_libraries(test_vector_glyphs PRIVATE amrexplorer::render2d amrexplorer::warnings)
add_test(NAME vector_glyphs COMMAND test_vector_glyphs)

add_executable(test_image_buffer unit/test_image_buffer.cpp)
target_link_libraries(test_image_buffer PRIVATE amrexplorer::render2d amrexplorer::warnings)
add_test(NAME image_buffer COMMAND test_image_buffer)

add_executable(test_display_coordinator unit/test_display_coordinator.cpp)
target_link_libraries(
    test_display_coordinator
    PRIVATE amrexplorer::pipeline amrexplorer::warnings
)
add_test(NAME display_coordinator COMMAND test_display_coordinator)

add_executable(test_particle_projection unit/test_particle_projection.cpp)
target_link_libraries(
    test_particle_projection
    PRIVATE amrexplorer::pipeline amrexplorer::warnings
)
add_test(NAME particle_projection COMMAND test_particle_projection)

add_executable(test_slice_range_resolver unit/test_slice_range_resolver.cpp)
target_link_libraries(
    test_slice_range_resolver
    PRIVATE amrexplorer::pipeline amrexplorer::warnings
)
add_test(NAME slice_range_resolver COMMAND test_slice_range_resolver)

add_executable(test_slice_pipeline unit/test_slice_pipeline.cpp)
target_link_libraries(
    test_slice_pipeline
    PRIVATE amrexplorer::pipeline amrexplorer::warnings
)
add_test(NAME slice_pipeline COMMAND test_slice_pipeline)

if(TARGET amrexplorer_remote)
    add_executable(test_remote_frame unit/test_remote_frame.cpp)
    target_link_libraries(
        test_remote_frame
        PRIVATE amrexplorer::remote amrexplorer::warnings Threads::Threads
    )
    add_test(NAME remote_frame COMMAND test_remote_frame)
    # Timing-sensitive: it measures how long a write survives a slow reader, so
    # it must not be scheduled against other tests competing for the same cores.
    # It normally runs in a few seconds; the timeout is there so that a reader
    # which hangs fails in two minutes rather than sitting on ctest's 1500 s
    # default, now that the write budgets it configures are deliberately
    # generous ceilings.
    set_tests_properties(remote_frame PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 120)

    add_executable(test_remote_codec unit/test_remote_codec.cpp)
    target_include_directories(test_remote_codec PRIVATE
        "${PROJECT_SOURCE_DIR}/src/remote"
        "${PROJECT_BINARY_DIR}/src/remote/generated")
    target_link_libraries(test_remote_codec
        PRIVATE
            amrexplorer::remote
            ${AMREXPLORER_FLATBUFFERS_LIBRARY_TARGET}
            amrexplorer::warnings
    )
    add_test(NAME remote_codec COMMAND test_remote_codec)

    # Fuzz/property harness for the remote wire decoder (malicious-peer trust
    # boundary). Deterministic + bounded so it runs as a normal ctest; run under
    # the qt-sanitizers preset for UB/OOB -- the plain sanitizers preset builds
    # with remote OFF, so this target does not exist there. See the file header
    # for libFuzzer use.
    add_executable(fuzz_wire_codec fuzz/fuzz_wire_codec.cpp)
    target_include_directories(fuzz_wire_codec PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/fuzz"
        "${PROJECT_SOURCE_DIR}/src/remote"
        "${PROJECT_BINARY_DIR}/src/remote/generated")
    target_link_libraries(fuzz_wire_codec
        PRIVATE
            amrexplorer::remote
            ${AMREXPLORER_FLATBUFFERS_LIBRARY_TARGET}
            amrexplorer::warnings
    )
    amrexplorer_add_fuzz_target(fuzz_wire_codec)

    add_executable(test_remote_server
        integration/test_remote_server.cpp)
    target_include_directories(test_remote_server PRIVATE
        "${PROJECT_BINARY_DIR}/src/remote/generated")
    target_link_libraries(test_remote_server
        PRIVATE
            amrexplorer::remote
            ${AMREXPLORER_FLATBUFFERS_LIBRARY_TARGET}
            amrexplorer::warnings
            Threads::Threads
    )
    if(AMREXPLORER_ENABLE_SERVER_TEST_HOOKS)
        target_compile_definitions(test_remote_server
            PRIVATE AMREXPLORER_SERVER_TEST_HOOKS)
    endif()
    set(remote_server_fixture_dir
        "${CMAKE_CURRENT_BINARY_DIR}/fixtures_remote_server")
    add_test(NAME remote_server_fixture
        COMMAND "$<TARGET_FILE:fixture_materializer>"
            "${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "${remote_server_fixture_dir}")
    set_tests_properties(remote_server_fixture PROPERTIES
        FIXTURES_SETUP remote_server_materialized)
    set(remote_server_private_fixture_dir
        "${CMAKE_CURRENT_BINARY_DIR}/fixtures_remote_server_private")
    add_test(NAME remote_server_private_fixture
        COMMAND "$<TARGET_FILE:fixture_materializer>"
            "${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "${remote_server_private_fixture_dir}")
    set_tests_properties(remote_server_private_fixture PROPERTIES
        FIXTURES_SETUP remote_server_private_materialized)
    set(remote_server_fixture_3d_dir
        "${CMAKE_CURRENT_BINARY_DIR}/fixtures_remote_server_3d")
    add_test(NAME remote_server_fixture_3d
        COMMAND "$<TARGET_FILE:fixture_materializer>"
            "${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "${remote_server_fixture_3d_dir}")
    set_tests_properties(remote_server_fixture_3d PROPERTIES
        FIXTURES_SETUP remote_server_3d_materialized)
    set(remote_server_fixture_spherical_dir
        "${CMAKE_CURRENT_BINARY_DIR}/fixtures_remote_server_spherical")
    add_test(NAME remote_server_fixture_spherical
        COMMAND "$<TARGET_FILE:fixture_materializer>"
            "${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d_spherical"
            "${remote_server_fixture_spherical_dir}")
    set_tests_properties(remote_server_fixture_spherical PROPERTIES
        FIXTURES_SETUP remote_server_spherical_materialized)
    add_test(NAME remote_server
        COMMAND test_remote_server "${remote_server_private_fixture_dir}")
    # The trickle case now paces the write through the test hook instead of
    # provoking one with a 50 MiB response, so this costs a couple of seconds
    # rather than the ten to twelve it used to under the sanitizer presets.
    # RUN_SERIAL stays: the case still asserts against a stall interval, and
    # keeping it off shared cores is cheap for a test this short.
    set_tests_properties(remote_server PROPERTIES
        FIXTURES_REQUIRED remote_server_private_materialized
        RUN_SERIAL TRUE
        TIMEOUT 90)

    add_executable(test_remote_connection
        integration/test_remote_connection.cpp)
    target_include_directories(test_remote_connection PRIVATE
        "${PROJECT_SOURCE_DIR}/src/remote"
        "${PROJECT_BINARY_DIR}/src/remote/generated")
    target_link_libraries(test_remote_connection
        PRIVATE
            amrexplorer::remote
            ${AMREXPLORER_FLATBUFFERS_LIBRARY_TARGET}
            amrexplorer::warnings
            Threads::Threads
    )
    if(AMREXPLORER_ENABLE_SERVER_TEST_HOOKS)
        target_compile_definitions(test_remote_connection
            PRIVATE AMREXPLORER_SERVER_TEST_HOOKS)
    endif()
    add_test(NAME remote_connection
        COMMAND test_remote_connection "${remote_server_fixture_dir}")
    set_tests_properties(remote_connection PROPERTIES
        FIXTURES_REQUIRED remote_server_materialized
        TIMEOUT 30)

    add_executable(test_remote_session
        integration/test_remote_session.cpp)
    target_link_libraries(test_remote_session
        PRIVATE
            amrexplorer::pipeline
            amrexplorer::remote
            amrexplorer::warnings
            Threads::Threads
    )
    add_test(NAME remote_session
        COMMAND test_remote_session "${remote_server_fixture_dir}")
    set_tests_properties(remote_session PROPERTIES
        FIXTURES_REQUIRED remote_server_materialized
        TIMEOUT 30)

    add_executable(test_remote_volume
        integration/test_remote_volume.cpp)
    target_include_directories(test_remote_volume PRIVATE
        "${PROJECT_BINARY_DIR}/src/remote/generated")
    target_link_libraries(test_remote_volume
        PRIVATE
            amrexplorer::remote
            ${AMREXPLORER_FLATBUFFERS_LIBRARY_TARGET}
            amrexplorer::warnings
            Threads::Threads
    )
    add_test(NAME remote_volume
        COMMAND test_remote_volume "${remote_server_fixture_3d_dir}")
    set_tests_properties(remote_volume PROPERTIES
        FIXTURES_REQUIRED remote_server_3d_materialized
        TIMEOUT 60)

    if(NOT WIN32)
        add_executable(test_remote_stdio
            integration/test_remote_stdio.cpp)
        target_link_libraries(test_remote_stdio
            PRIVATE
                amrexplorer::remote
                amrexplorer::warnings
                Threads::Threads
        )
        add_test(NAME remote_stdio
            COMMAND test_remote_stdio "${remote_server_fixture_dir}"
                "$<TARGET_FILE:amrexplorer_server>")
        set_tests_properties(remote_stdio PROPERTIES
            FIXTURES_REQUIRED remote_server_materialized
            TIMEOUT 60)
    endif()

    # FAIL_REGULAR_EXPRESSION as well as PASS: an unrecognized option prints
    # the same usage text (to stderr, after "server error") and a pass
    # expression makes ctest ignore the exit code, so without this an option
    # the server does not know would look like one it does.
    add_test(NAME remote_server_help
        COMMAND "$<TARGET_FILE:amrexplorer_server>" --help)
    set_tests_properties(remote_server_help PROPERTIES
        PASS_REGULAR_EXPRESSION "usage: amrexplorer-server"
        FAIL_REGULAR_EXPRESSION "server error"
        TIMEOUT 30)
    add_test(NAME remote_server_help_short
        COMMAND "$<TARGET_FILE:amrexplorer_server>" -h)
    set_tests_properties(remote_server_help_short PROPERTIES
        PASS_REGULAR_EXPRESSION "usage: amrexplorer-server"
        FAIL_REGULAR_EXPRESSION "server error"
        TIMEOUT 30)
    # The version CMake read out of Version.hpp and the string the binary
    # composed from the same line, checked against each other. No ^ anchor: the
    # expression is matched against everything the run wrote, so a loader or
    # sanitizer line ahead of the output would otherwise fail it. FAIL as well
    # as PASS, because a pass expression makes ctest ignore the exit code.
    foreach(flag --version -v)
        string(REGEX REPLACE "^-+" "" name "${flag}")
        add_test(NAME remote_server_version_${name}
            COMMAND "$<TARGET_FILE:amrexplorer_server>" "${flag}")
        set_tests_properties(remote_server_version_${name} PROPERTIES
            PASS_REGULAR_EXPRESSION
                "amrexplorer-server ${AMREXPLORER_VERSION_PATTERN}${amrexplorer_version_tail}"
            FAIL_REGULAR_EXPRESSION "server error"
            TIMEOUT 30)
    endforeach()
    add_test(NAME remote_server_rejects_zero_datasets
        COMMAND "${CMAKE_COMMAND}"
            "-DAMREXPLORER_SERVER=$<TARGET_FILE:amrexplorer_server>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/server_cli_failure_driver.cmake")
endif()

add_executable(test_vismf_index unit/test_vismf_index.cpp)
target_link_libraries(test_vismf_index PRIVATE amrexplorer::io amrexplorer::warnings)
add_test(NAME vismf_index COMMAND test_vismf_index)

add_executable(test_plotfile_header unit/test_plotfile_header.cpp)
target_link_libraries(test_plotfile_header PRIVATE amrexplorer::io amrexplorer::warnings)
add_test(NAME plotfile_header COMMAND test_plotfile_header)

# Replaces global operator new to measure what a crafted Header reserves, so it
# must stay its own executable: the counting would otherwise apply to every
# other case sharing the binary.
#
# It runs under the sanitizers like everything else. Replacing operator new was
# briefly thought to take the binary out from under ASan; it does not. ASan
# interposes malloc/free at the libc level, so an operator new built on malloc
# still allocates from ASan's allocator with its redzones intact -- verified by
# compiling this shape under -fsanitize=address and watching it report a
# heap-buffer-overflow. Only new/delete-mismatch detection is lost, and this
# file pairs them by construction.
add_executable(test_header_allocation_bounds unit/test_header_allocation_bounds.cpp)
target_link_libraries(test_header_allocation_bounds
    PRIVATE amrexplorer::data amrexplorer::io amrexplorer::warnings)
add_test(NAME header_allocation_bounds COMMAND test_header_allocation_bounds)

add_executable(test_particle_header unit/test_particle_header.cpp)
target_link_libraries(test_particle_header PRIVATE amrexplorer::io amrexplorer::warnings)
add_test(NAME particle_header COMMAND test_particle_header)

add_executable(test_fits_writer unit/test_fits_writer.cpp)
target_link_libraries(test_fits_writer PRIVATE amrexplorer::io amrexplorer::warnings)
add_test(NAME fits_writer COMMAND test_fits_writer)

add_executable(test_block_lookup unit/test_block_lookup.cpp)
target_link_libraries(test_block_lookup PRIVATE amrexplorer::query amrexplorer::warnings)
add_test(NAME block_lookup COMMAND test_block_lookup)

add_executable(test_plotfile_metadata integration/test_plotfile_metadata.cpp)
target_link_libraries(test_plotfile_metadata PRIVATE amrexplorer::io amrexplorer::warnings)
add_test(
    NAME plotfile_metadata_only
    COMMAND test_plotfile_metadata
        "${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_nonuniform"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
)

add_executable(test_selective_block_read integration/test_selective_block_read.cpp)
target_link_libraries(test_selective_block_read PRIVATE amrexplorer::io amrexplorer::warnings)
add_test(NAME selective_block_read COMMAND test_selective_block_read)

add_executable(test_particle_reader integration/test_particle_reader.cpp)
target_link_libraries(
    test_particle_reader
    PRIVATE amrexplorer::pipeline amrexplorer::warnings
)
add_test(
    NAME particle_reader
    COMMAND test_particle_reader "${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
)

add_executable(test_dataset_extract integration/test_dataset_extract.cpp)
target_link_libraries(
    test_dataset_extract PRIVATE amrexplorer::data amrexplorer::warnings
)
add_test(NAME dataset_extract COMMAND test_dataset_extract)

add_executable(test_dataset_concurrency integration/test_dataset_concurrency.cpp)
target_link_libraries(
    test_dataset_concurrency PRIVATE amrexplorer::io amrexplorer::warnings
    Threads::Threads
)
add_test(NAME dataset_concurrency COMMAND test_dataset_concurrency)
set_tests_properties(dataset_concurrency PROPERTIES TIMEOUT 60)

add_executable(test_fab_catalog integration/test_fab_catalog.cpp)
target_link_libraries(test_fab_catalog PRIVATE amrexplorer::query amrexplorer::warnings)
add_test(NAME fab_catalog COMMAND test_fab_catalog)

add_executable(
    test_single_precision_plotfile
    integration/test_single_precision_plotfile.cpp
)
target_link_libraries(
    test_single_precision_plotfile
    PRIVATE amrexplorer::query amrexplorer::warnings
)
add_test(NAME single_precision_plotfile COMMAND test_single_precision_plotfile)

add_executable(test_display_transitions integration/test_display_transitions.cpp)
target_link_libraries(
    test_display_transitions
    PRIVATE amrexplorer::pipeline amrexplorer::warnings
)
add_test(NAME display_transitions COMMAND test_display_transitions)

add_executable(test_derived_field_query integration/test_derived_field_query.cpp)
target_link_libraries(
    test_derived_field_query PRIVATE amrexplorer::data amrexplorer::query
    amrexplorer::warnings)
add_test(NAME derived_field_query COMMAND test_derived_field_query)

add_executable(test_slice_query integration/test_slice_query.cpp)
target_link_libraries(test_slice_query PRIVATE amrexplorer::query amrexplorer::warnings)
add_test(NAME slice_query COMMAND test_slice_query)

add_executable(test_volume_query integration/test_volume_query.cpp)
target_link_libraries(test_volume_query PRIVATE amrexplorer::query amrexplorer::warnings)
add_test(NAME volume_query COMMAND test_volume_query)

add_executable(test_volume_pipeline integration/test_volume_pipeline.cpp)
target_link_libraries(test_volume_pipeline PRIVATE amrexplorer::pipeline amrexplorer::warnings)
add_test(NAME volume_pipeline COMMAND test_volume_pipeline)

add_executable(test_spherical_display integration/test_spherical_display.cpp)
target_link_libraries(
    test_spherical_display
    PRIVATE
        amrexplorer::pipeline
        amrexplorer::render2d
        amrexplorer::warnings
        Threads::Threads
)
add_test(NAME spherical_display COMMAND test_spherical_display)

add_executable(test_line_query integration/test_line_query.cpp)
target_link_libraries(
    test_line_query
    PRIVATE
        amrexplorer::query
        amrexplorer::warnings
        Threads::Threads
)
add_test(
    NAME line_query
    COMMAND test_line_query
        "${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
)

# Writes FAB payloads into copies of the metadata-only fixtures; Qt-free so
# the headless build can compile it too.
add_executable(fixture_materializer fixture_materializer/main.cpp)
target_link_libraries(fixture_materializer PRIVATE amrexplorer::warnings)

# Smoke tests for the standalone tools (tools/), previously built but never run
# by ctest. dataset_inspect reads metadata only, so it runs on the metadata-only
# fixture directly; the output regex pins the run to a real parse, not just a
# clean exit. query_benchmark and line_repro read FAB payloads, so each
# materializes a fixture copy first (see tools_smoke_driver.cmake).
add_test(NAME tool_dataset_inspect
    COMMAND dataset_inspect "${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d")
set_tests_properties(tool_dataset_inspect PROPERTIES
    PASS_REGULAR_EXPRESSION "dimension: 2")
add_test(NAME tool_dataset_inspect_json
    COMMAND dataset_inspect --json "${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d")
set_tests_properties(tool_dataset_inspect_json PROPERTIES
    PASS_REGULAR_EXPRESSION "\"dimension\": 2")

add_test(NAME tool_query_benchmark
    COMMAND "${CMAKE_COMMAND}"
        "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
        "-DTOOL=$<TARGET_FILE:query_benchmark>"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_tool_query_benchmark"
        -DMODE=query-benchmark
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tools_smoke_driver.cmake")
set_tests_properties(tool_query_benchmark PROPERTIES TIMEOUT 30)

add_test(NAME tool_line_repro
    COMMAND "${CMAKE_COMMAND}"
        "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
        "-DTOOL=$<TARGET_FILE:line_repro>"
        "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_tool_line_repro"
        -DMODE=line-repro
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tools_smoke_driver.cmake")
set_tests_properties(tool_line_repro PROPERTIES TIMEOUT 60)

if(TARGET amrexplorer_qt)
    # As above for the client -- and run with a platform plugin that cannot
    # load, because --version is answered before QApplication exists and has to
    # keep working on a machine with no display. Were it ever moved after,
    # Qt would abort here instead of printing.
    foreach(flag --version -v)
        string(REGEX REPLACE "^-+" "" name "${flag}")
        add_test(NAME qt_version_${name}
            COMMAND "${CMAKE_COMMAND}" -E env
                QT_QPA_PLATFORM=amrexplorer-no-plugin
                "$<TARGET_FILE:amrexplorer_qt>" "${flag}")
        set_tests_properties(qt_version_${name} PROPERTIES
            PASS_REGULAR_EXPRESSION
                "amrexplorer ${AMREXPLORER_VERSION_PATTERN}${amrexplorer_version_tail}"
            FAIL_REGULAR_EXPRESSION "platform plugin"
            TIMEOUT 30)
    endforeach()

    add_executable(test_sequence_controller
        unit/test_sequence_controller.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/SequenceController.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/SequenceController.hpp"
    )
    set_target_properties(test_sequence_controller PROPERTIES AUTOMOC ON)
    target_include_directories(test_sequence_controller
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_sequence_controller
        PRIVATE
            amrexplorer::pipeline
            amrexplorer::warnings
            Qt6::Concurrent
            Qt6::Core
    )
    add_test(NAME sequence_controller COMMAND test_sequence_controller)

    add_executable(test_derived_field_controller
        unit/test_derived_field_controller.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/DerivedFieldController.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/DerivedFieldController.hpp"
        "${PROJECT_SOURCE_DIR}/src/qt/DerivedFieldStore.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/DerivedFieldStore.hpp"
        "${PROJECT_SOURCE_DIR}/src/qt/ExpressionEditorDialog.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/ExpressionEditorDialog.hpp"
    )
    set_target_properties(test_derived_field_controller
        PROPERTIES AUTOMOC ON)
    target_include_directories(test_derived_field_controller
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_derived_field_controller
        PRIVATE
            amrexplorer::core
            amrexplorer::warnings
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    # Builds the real editor widgets, so it needs a QPA platform.
    add_test(NAME derived_field_controller
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_derived_field_controller>")

    add_executable(test_palette_controller
        unit/test_palette_controller.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/PaletteController.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/PaletteController.hpp"
    )
    set_target_properties(test_palette_controller PROPERTIES AUTOMOC ON)
    target_include_directories(test_palette_controller
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_palette_controller
        PRIVATE
            amrexplorer::render2d
            amrexplorer::warnings
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    # The controller builds real widgets (menu, selector), so it needs a QPA
    # platform; offscreen keeps it headless like the Qt smoke tests.
    add_test(NAME palette_controller
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_palette_controller>")

    add_executable(test_range_controller
        unit/test_range_controller.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/RangeController.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/RangeController.hpp"
        "${PROJECT_SOURCE_DIR}/src/qt/ScientificDoubleSpinBox.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/ScientificDoubleSpinBox.hpp"
        "${PROJECT_SOURCE_DIR}/src/qt/NumberFormat.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/NumberFormat.hpp"
    )
    set_target_properties(test_range_controller PROPERTIES AUTOMOC ON)
    target_include_directories(test_range_controller
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_range_controller
        PRIVATE
            amrexplorer::pipeline
            amrexplorer::warnings
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    # The controller builds real widgets into a toolbar; offscreen keeps it
    # headless.
    add_test(NAME range_controller
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_range_controller>")
    set_tests_properties(range_controller PROPERTIES TIMEOUT 60)

    add_executable(test_particle_controller
        unit/test_particle_controller.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/ParticleController.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/ParticleController.hpp"
    )
    set_target_properties(test_particle_controller PROPERTIES AUTOMOC ON)
    target_include_directories(test_particle_controller
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_particle_controller
        PRIVATE
            amrexplorer::pipeline
            amrexplorer::warnings
            Qt6::Concurrent
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    # The controller builds real widgets (the dialog, the action, the progress
    # bar), so it needs a QPA platform; offscreen keeps it headless.
    add_test(NAME particle_controller
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_particle_controller>")
    set_tests_properties(particle_controller PROPERTIES TIMEOUT 60)

    # The export rules on their own: a header of free functions, so this needs
    # no window and no amrexplorer library -- just Qt and a synthetic widget.
    add_executable(test_widget_image_export unit/test_widget_image_export.cpp)
    # AUTOMOC as its siblings have it: the test's widgets omit Q_OBJECT today,
    # so it links without -- and the first tr() or slot added to them would
    # fail on missing vtable symbols with nothing to point at the cause.
    set_target_properties(test_widget_image_export PROPERTIES AUTOMOC ON)
    target_include_directories(test_widget_image_export
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_widget_image_export
        PRIVATE
            amrexplorer::warnings
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    add_test(NAME widget_image_export
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_widget_image_export>")
    set_tests_properties(widget_image_export PROPERTIES TIMEOUT 60)

    add_executable(test_volume_controller
        unit/test_volume_controller.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/VolumeController.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/VolumeController.hpp"
        "${PROJECT_SOURCE_DIR}/src/qt/VolumeWindow.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/VolumeWindow.hpp"
        "${PROJECT_SOURCE_DIR}/src/qt/IsoWidget.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/OpacityCurveWidget.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/IsoWidget.hpp"
    )
    set_target_properties(test_volume_controller PROPERTIES AUTOMOC ON)
    target_include_directories(test_volume_controller
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_volume_controller
        PRIVATE
            amrexplorer::pipeline
            amrexplorer::render3d
            amrexplorer::warnings
            Qt6::Concurrent
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    add_test(NAME volume_controller
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_volume_controller>")
    set_tests_properties(volume_controller PROPERTIES TIMEOUT 60)
    # The same tests again on a scaled display. QT_SCALE_FACTOR makes the
    # offscreen platform report a device pixel ratio of 2 while leaving logical
    # sizes alone, which is the only way here to tell a request sized in device
    # pixels from one sized in logical pixels -- the volume render was sized in
    # logical ones, so every frame on such a display arrived soft.
    add_test(NAME volume_controller_hidpi
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            QT_SCALE_FACTOR=2
            "$<TARGET_FILE:test_volume_controller>")
    set_tests_properties(volume_controller_hidpi PROPERTIES TIMEOUT 60)

    add_executable(test_diagnostics_model
        unit/test_diagnostics_model.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/DiagnosticsModel.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/DiagnosticsModel.hpp"
    )
    set_target_properties(test_diagnostics_model PROPERTIES AUTOMOC ON)
    target_include_directories(test_diagnostics_model
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_diagnostics_model
        PRIVATE
            amrexplorer::cache
            amrexplorer::warnings
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    add_test(NAME diagnostics_model
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_diagnostics_model>")

    add_executable(test_fab_navigator
        unit/test_fab_navigator.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/FabNavigator.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/FabNavigator.hpp"
        "${PROJECT_SOURCE_DIR}/src/qt/FabSelectorDock.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/FabSelectorDock.hpp"
    )
    set_target_properties(test_fab_navigator PROPERTIES AUTOMOC ON)
    target_include_directories(test_fab_navigator
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_fab_navigator
        PRIVATE
            amrexplorer::io
            amrexplorer::pipeline
            amrexplorer::warnings
            Qt6::Concurrent
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )
    add_test(NAME fab_navigator
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_fab_navigator>")
    set_tests_properties(fab_navigator PROPERTIES TIMEOUT 60)

    add_executable(test_remote_endpoint unit/test_remote_endpoint.cpp)
    target_include_directories(test_remote_endpoint
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_remote_endpoint
        PRIVATE amrexplorer::remote amrexplorer::warnings)
    add_test(NAME remote_endpoint COMMAND test_remote_endpoint)

    add_executable(test_ssh_connect_arguments
        unit/test_ssh_connect_arguments.cpp)
    target_include_directories(test_ssh_connect_arguments
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_ssh_connect_arguments
        PRIVATE amrexplorer::warnings)
    add_test(NAME ssh_connect_arguments COMMAND test_ssh_connect_arguments)

    # The Open Remote dialog as a widget: prefill, the executable following
    # the destination, the path fields, Browse... versus Open.
    add_executable(test_remote_open_dialog
        unit/test_remote_open_dialog.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/RemoteOpenDialog.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/RemoteOpenDialog.hpp")
    target_include_directories(test_remote_open_dialog
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_remote_open_dialog
        PRIVATE amrexplorer::warnings Qt6::Widgets)
    add_test(NAME remote_open_dialog
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_remote_open_dialog>")
    set_tests_properties(remote_open_dialog PROPERTIES TIMEOUT 60)

    # RemoteSessionController: install() over a loopback Server, start() over
    # the stand-in ssh script running the real server, per-destination
    # settings, diagnostics lines. Never opens a modal dialog.
    add_executable(test_remote_session_controller
        unit/test_remote_session_controller.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/RemoteSessionController.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/RemoteSessionController.hpp"
        "${PROJECT_SOURCE_DIR}/src/qt/RemoteOpenDialog.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/RemoteOpenDialog.hpp"
        "${PROJECT_SOURCE_DIR}/src/qt/RemoteFileDialog.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/RemoteFileDialog.hpp"
        "${PROJECT_SOURCE_DIR}/src/qt/SshRemoteSession.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/SshRemoteSession.hpp")
    set_target_properties(test_remote_session_controller
        PROPERTIES AUTOMOC ON)
    target_include_directories(test_remote_session_controller
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_remote_session_controller
        PRIVATE amrexplorer::remote amrexplorer::warnings
            Qt6::Widgets Qt6::Concurrent)
    add_test(NAME remote_session_controller
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_remote_session_controller>"
            "$<TARGET_FILE:amrexplorer_server>"
            "${CMAKE_CURRENT_SOURCE_DIR}/fake_ssh.sh")
    set_tests_properties(remote_session_controller PROPERTIES TIMEOUT 120)

    # The remote browser against a loopback Server: listing, navigation, an
    # unreadable path, and both selection modes. Offscreen; no display needed.
    add_executable(test_remote_file_dialog
        unit/test_remote_file_dialog.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/RemoteFileDialog.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/RemoteFileDialog.hpp")
    target_include_directories(test_remote_file_dialog
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_remote_file_dialog
        PRIVATE amrexplorer::remote amrexplorer::warnings
            Qt6::Widgets Qt6::Concurrent)
    add_test(NAME remote_file_dialog
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_remote_file_dialog>")
    set_tests_properties(remote_file_dialog PROPERTIES TIMEOUT 60)

    # The argument builders and the ready-line parser, then -- where the
    # transport exists -- the whole client flow against the real server binary
    # run by a stand-in ssh script: socket pair, preamble, handshake, ready
    # callback, stop() ending the server on end of stream.
    add_executable(test_ssh_remote_session
        unit/test_ssh_remote_session.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/SshRemoteSession.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/SshRemoteSession.hpp")
    target_include_directories(test_ssh_remote_session
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_ssh_remote_session
        PRIVATE amrexplorer::remote amrexplorer::warnings
            Qt6::Core Qt6::Concurrent)
    add_test(NAME ssh_remote_session
        COMMAND test_ssh_remote_session
            "${remote_server_fixture_dir}"
            "$<TARGET_FILE:amrexplorer_server>"
            "${CMAKE_CURRENT_SOURCE_DIR}/fake_ssh.sh")
    set_tests_properties(ssh_remote_session PROPERTIES
        FIXTURES_REQUIRED remote_server_materialized
        TIMEOUT 60)

    # --help exits before QApplication is constructed, so it needs no display.
    add_test(NAME qt_help
        COMMAND "$<TARGET_FILE:amrexplorer_qt>" --help)
    set_tests_properties(qt_help PROPERTIES
        PASS_REGULAR_EXPRESSION "usage: amrexplorer"
        FAIL_REGULAR_EXPRESSION "unrecognized option"
        TIMEOUT 10)
    add_test(NAME qt_help_short
        COMMAND "$<TARGET_FILE:amrexplorer_qt>" -h)
    set_tests_properties(qt_help_short PROPERTIES
        PASS_REGULAR_EXPRESSION "usage: amrexplorer"
        FAIL_REGULAR_EXPRESSION "unrecognized option"
        TIMEOUT 10)

    add_test(
        NAME qt_remote_slice_smoke
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:amrexplorer_qt>"
            --remote-slice-smoke-test "${remote_server_fixture_dir}"
    )
    set_tests_properties(qt_remote_slice_smoke PROPERTIES
        FIXTURES_REQUIRED remote_server_materialized
        TIMEOUT 30)

    add_test(
        NAME qt_remote_fixed_scale_smoke
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:amrexplorer_qt>"
            --remote-fixed-scale-smoke-test "${remote_server_fixture_dir}"
    )
    set_tests_properties(qt_remote_fixed_scale_smoke PROPERTIES
        FIXTURES_REQUIRED remote_server_materialized
        TIMEOUT 30)

    # The wide fixture has enough finest cells (8192 across) that a 32x
    # raster can never cover the viewport width, the regime where the
    # demand/scrollbar feedback loop lived; the shared 8x8 fixture is
    # domain-clamped at every scale and cannot reach it.
    add_test(
        NAME qt_remote_fixed_scale_flicker_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d_wide"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_remote_fixed_scale_flicker"
            -DMODE=remote-fixed-scale-flicker
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_remote_fixed_scale_flicker_smoke PROPERTIES
        TIMEOUT 60)

    # Regression for virtual-canvas-survives-wheel-zoom: a wheel notch over a
    # remote fixed scale switches the transform mode to Custom while leaving the
    # whole-domain canvas installed, so every scene-to-physical reader has to
    # cope with that pairing. The view must stay where the wheel put it rather
    # than jumping to whatever cell-space coordinates read as raster pixels.
    add_test(
        NAME qt_remote_canvas_wheel_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d_wide"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_remote_canvas_wheel"
            -DMODE=remote-canvas-wheel
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_remote_canvas_wheel_smoke PROPERTIES TIMEOUT 30)

    # Regression for fixed-scale-switch-lands-off-center-remotely: selecting a
    # fixed scale keeps looking at the same place. Local does that implicitly
    # through the view's transformation anchor while remote re-centres on the
    # centre viewCenterInData reports, so the two agree only if that centre is
    # the true one. It was a fraction of a raster pixel off, which is thirteen
    # finest cells on this domain.
    add_test(
        NAME qt_remote_fixed_scale_centre_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d_wide"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_fixed_scale_centre"
            -DMODE=fixed-scale-centre
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_remote_fixed_scale_centre_smoke
        PROPERTIES TIMEOUT 30)

    # Local and remote must agree on what a scrolled fixed scale shows.
    # Scrolling is what separates the two coordinate spaces: local scales a
    # whole-domain raster while remote scales a whole-domain virtual canvas of
    # finest cells with the fetched raster placed inside it. The assertion
    # covers both the reported physical window and the painted viewport pixels.
    # The refined fixture is the fixture for this: its 16 finest cells overflow
    # the test window at 32x, and unlike plotfile_2d_wide its whole domain
    # still fits in one native raster, so a fixed scale means the same number
    # of view pixels per finest cell on both sides (see
    # agent-notes/issues/fixed-scale-clamped-native-raster.md).
    add_test(
        NAME qt_local_remote_fixed_scale_window_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_refined_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_local_remote_fixed_scale_window"
            -DMODE=local-remote-fixed-scale-window
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_local_remote_fixed_scale_window_smoke PROPERTIES
        TIMEOUT 60)

    add_test(
        NAME qt_remote_fixed_scale_wide_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d_wide"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_remote_fixed_scale_wide"
            -DMODE=remote-fixed-scale
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_remote_fixed_scale_wide_smoke PROPERTIES
        TIMEOUT 30)

    # Regression for remote-fit-anisotropic-cells: with cells that are not
    # square, the remote Fit raster and a wheel-zoomed Custom raster must be
    # sized to the region's aspect in finest cells (the display's unit), not
    # its physical aspect, which squeezed them to one or two pixels wide.
    add_test(
        NAME qt_remote_cell_aspect_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d_tall"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_remote_cell_aspect"
            -DMODE=remote-cell-aspect
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_remote_cell_aspect_smoke PROPERTIES TIMEOUT 30)

    add_test(
        NAME qt_remote_rubber_aspect_smoke
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:amrexplorer_qt>"
            --remote-rubber-aspect-smoke-test
            "${remote_server_fixture_dir}"
    )
    set_tests_properties(qt_remote_rubber_aspect_smoke PROPERTIES
        FIXTURES_REQUIRED remote_server_materialized
        TIMEOUT 30)

    add_test(
        NAME qt_remote_3d_initial_geometry_smoke
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:amrexplorer_qt>"
            --remote-initial-geometry-smoke-test
            "${remote_server_fixture_3d_dir}"
    )
    set_tests_properties(qt_remote_3d_initial_geometry_smoke PROPERTIES
        FIXTURES_REQUIRED remote_server_3d_materialized
        TIMEOUT 30)

    # The same volume window over the in-process server (protocol 1.2): the
    # frame is rendered on the server and displayed here.
    add_test(
        NAME qt_remote_volume_smoke
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:amrexplorer_qt>"
            --remote-volume-smoke-test
            "${remote_server_fixture_3d_dir}"
    )
    set_tests_properties(qt_remote_volume_smoke PROPERTIES
        FIXTURES_REQUIRED remote_server_3d_materialized
        TIMEOUT 60)

    add_test(
        NAME qt_remote_spherical_initial_geometry_smoke
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:amrexplorer_qt>"
            --remote-initial-geometry-smoke-test
            "${remote_server_fixture_spherical_dir}"
    )
    set_tests_properties(qt_remote_spherical_initial_geometry_smoke PROPERTIES
        FIXTURES_REQUIRED remote_server_spherical_materialized
        TIMEOUT 30)

    add_test(
        NAME qt_remote_grid_boxes_smoke
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:amrexplorer_qt>"
            --remote-grid-boxes-smoke-test
            "${remote_server_fixture_dir}"
    )
    set_tests_properties(qt_remote_grid_boxes_smoke PROPERTIES
        FIXTURES_REQUIRED remote_server_materialized
        TIMEOUT 30)

    add_test(
        NAME qt_remote_sequence_smoke
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:amrexplorer_qt>"
            --remote-sequence-smoke-test "${remote_server_fixture_dir}"
    )
    set_tests_properties(qt_remote_sequence_smoke PROPERTIES
        FIXTURES_REQUIRED remote_server_materialized
        TIMEOUT 30)

    add_executable(test_user_guide_dialog
        unit/test_user_guide_dialog.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/UserGuideDialog.cpp"
        "${PROJECT_SOURCE_DIR}/resources/user-guide.qrc"
    )
    set_target_properties(test_user_guide_dialog PROPERTIES AUTORCC ON)
    target_include_directories(test_user_guide_dialog
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt"
    )
    target_link_libraries(test_user_guide_dialog
        PRIVATE amrexplorer::warnings Qt6::Widgets
    )
    add_test(
        NAME user_guide_dialog
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_user_guide_dialog>"
    )

    add_executable(test_plane_mapping unit/test_plane_mapping.cpp)
    target_include_directories(test_plane_mapping
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt"
    )
    target_link_libraries(test_plane_mapping
        PRIVATE amrexplorer::core amrexplorer::warnings Qt6::Core
    )
    add_test(NAME plane_mapping COMMAND test_plane_mapping)

    add_executable(test_scientific_double_spin_box
        unit/test_scientific_double_spin_box.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/NumberFormat.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/ScientificDoubleSpinBox.cpp"
    )
    target_include_directories(test_scientific_double_spin_box
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt"
    )
    target_link_libraries(test_scientific_double_spin_box
        PRIVATE amrexplorer::warnings Qt6::Widgets
    )
    add_test(
        NAME scientific_double_spin_box
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_scientific_double_spin_box>"
    )

    # NumberFormat needs only QString (QtCore), no widgets or QApplication.
    add_executable(test_number_format
        unit/test_number_format.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/NumberFormat.cpp"
    )
    target_include_directories(test_number_format
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt"
    )
    target_link_libraries(test_number_format
        PRIVATE amrexplorer::warnings Qt6::Core
    )
    add_test(NAME number_format COMMAND test_number_format)

    add_executable(test_line_plot_hover
        unit/test_line_plot_hover.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/LinePlotWindow.cpp"
        "${PROJECT_SOURCE_DIR}/src/qt/NumberFormat.cpp"
    )
    set_target_properties(test_line_plot_hover PROPERTIES AUTOMOC ON)
    target_include_directories(test_line_plot_hover
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt"
    )
    target_link_libraries(test_line_plot_hover
        PRIVATE amrexplorer::core amrexplorer::warnings Qt6::Test Qt6::Widgets
    )
    add_test(
        NAME line_plot_hover
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_line_plot_hover>"
    )

    add_executable(test_image_view_pan
        unit/test_image_view_pan.cpp
        "${PROJECT_SOURCE_DIR}/src/qt/ImageView.cpp"
    )
    set_target_properties(test_image_view_pan PROPERTIES AUTOMOC ON)
    target_include_directories(test_image_view_pan
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt"
    )
    target_link_libraries(test_image_view_pan
        PRIVATE amrexplorer::core amrexplorer::warnings Qt6::Widgets
    )
    add_test(
        NAME image_view_pan
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_image_view_pan>"
    )

    add_test(
        NAME qt_metadata_smoke
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:amrexplorer_qt>" --smoke-test
            "${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
    )
    set_tests_properties(qt_metadata_smoke PROPERTIES TIMEOUT 10)
    add_test(
        NAME qt_metadata_smoke_3d
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:amrexplorer_qt>" --smoke-test
            "${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
    )
    set_tests_properties(qt_metadata_smoke_3d PROPERTIES TIMEOUT 10)
    # A failed open destroys the previous dataset before it knows whether the
    # replacement is openable, so it has to leave a placeholder that says so --
    # and the window must still open a good dataset afterwards. The recovery
    # leg renders, which is the only way the placeholder is proven cleared, so
    # it needs a materialized fixture with payloads rather than the source tree.
    add_test(
        NAME qt_open_failure_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_open_failure"
            -DMODE=open-failure
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_open_failure_smoke PROPERTIES TIMEOUT 30)
    # The slice and sequence smoke paths need FAB payloads, so each test
    # materializes a fixture copy first (see qt_smoke_driver.cmake).
    add_test(
        NAME qt_slice_smoke_2d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_2d"
            -DMODE=slice
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_slice_smoke_2d PROPERTIES TIMEOUT 30)
    # Regression test for raster-colorbar-mismatch-on-2d-visible-zoom: after a
    # full-domain Visible slice caches the range, zooming a 2-D view must
    # re-render the raster against that reused range so it matches the color bar.
    add_test(
        NAME qt_raster_zoom_smoke_2d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_raster_zoom_2d"
            -DMODE=raster-zoom
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_raster_zoom_smoke_2d PROPERTIES TIMEOUT 30)
    # Regression test for the 2-D spherical supersample control: zooming a
    # spherical view then changing the warp factor must resize the warped raster
    # while preserving the zoomed framing (not refit to the whole sector).
    add_test(
        NAME qt_spherical_supersample_smoke_2d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d_spherical"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_spherical_supersample_2d"
            -DMODE=spherical-supersample
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_spherical_supersample_smoke_2d PROPERTIES TIMEOUT 30)
    # Regression test for sequence-frame-range-cache-goes-stale: across sequence
    # frames the full-domain range cache must invalidate (its key includes the
    # dataset), so a zoomed Visible color bar tracks the current frame instead
    # of reusing an earlier frame's range. Frame 1's field is scaled 10x.
    add_test(
        NAME qt_range_cache_smoke_2d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_range_cache_2d"
            -DMODE=range-cache
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_range_cache_smoke_2d PROPERTIES TIMEOUT 30)
    add_test(
        NAME qt_slice_smoke_3d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_3d"
            -DMODE=slice
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_slice_smoke_3d PROPERTIES TIMEOUT 30)
    # The Volume Rendering window over the 3-D fixture, locally: opened as the
    # View menu action opens it, its first frame must have lit pixels (the
    # ray caster ran end to end through the session, pipeline and window).
    add_test(
        NAME qt_volume_smoke_3d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_volume_3d"
            -DMODE=volume
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_volume_smoke_3d PROPERTIES TIMEOUT 60)

    # Derived fields through the GUI: the Variable menu's Expression Editor
    # refuses a definition the dataset cannot satisfy and reports it in place,
    # then an accepted one reaches the field selector and the Variable menu and
    # renders when selected. The 2-D fixture has the two fields the expression
    # multiplies.
    add_test(
        NAME qt_derived_field_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_derived_field"
            -DMODE=derived-field
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_derived_field_smoke PROPERTIES TIMEOUT 60)

    # An interaction that lands while a reload is still replacing the session.
    # The reload skips its own display for that view and the interaction's
    # raster came from the session being replaced, so without the re-slice the
    # reload owes it, the window keeps the outgoing session's pixels under the
    # new catalog.
    add_test(
        NAME qt_derived_field_reload_race_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_derived_reload_race"
            -DMODE=derived-field-reload-race
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(
        qt_derived_field_reload_race_smoke PROPERTIES TIMEOUT 60)

    # The two ways a committed definition can miss the session on screen: an
    # Apply during a plane sweep, which never reopens the dataset of its own
    # accord, and a list changed while this window is opening one, when it
    # counts as unable to take derived fields and is told nothing. The 3-D
    # fixture is what a plane sweep needs.
    add_test(
        NAME qt_derived_field_playback_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_derived_field_playback"
            -DMODE=derived-field-playback
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_derived_field_playback_smoke PROPERTIES TIMEOUT 60)

    # Derived fields over the protocol (1.4): the editor is available over a
    # remote dataset, Apply reopens it on its own connection with the
    # definitions, the computed field arrives as the tail of the catalog and
    # renders, one the plotfile cannot satisfy is greyed with the server's own
    # reason, and the reload settles instead of looping. The server runs
    # in-process, so the "remote" path is the materialized fixture.
    add_test(
        NAME qt_remote_derived_field_smoke
        COMMAND "$<TARGET_FILE:amrexplorer_qt>"
            --remote-derived-field-smoke-test
            "${CMAKE_CURRENT_BINARY_DIR}/fixtures_remote_server"
    )
    set_tests_properties(qt_remote_derived_field_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        FIXTURES_REQUIRED remote_server_materialized
        TIMEOUT 90)

    # Applying a definition while a plotfile sequence is open: the frame on
    # screen reloads with it and the sequence is still navigable afterwards,
    # which is what separates the reload from an ordinary dataset open.
    add_test(
        NAME qt_derived_field_sequence_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_derived_field_sequence"
            -DMODE=derived-field-sequence
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_derived_field_sequence_smoke PROPERTIES TIMEOUT 60)

    # The same, over frames that list different fields: the definition is left
    # out of the frame that cannot satisfy it, which moves every id after it.
    add_test(
        NAME qt_derived_field_frames_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_derived_field_frames"
            -DMODE=derived-field-frames
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_derived_field_frames_smoke PROPERTIES TIMEOUT 60)
    add_test(
        NAME qt_particle_visible_range_smoke_3d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_particle_visible_3d"
            -DMODE=particle-visible-range
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(
        qt_particle_visible_range_smoke_3d PROPERTIES TIMEOUT 30)
    # The particles dialog is modeless: it must not block the main window, one
    # instance is reused, and Apply draws without closing it.
    add_test(
        NAME qt_particle_dialog_smoke_3d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_particle_dialog_3d"
            -DMODE=particle-dialog
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_particle_dialog_smoke_3d PROPERTIES TIMEOUT 30)
    # A plain open and a sequence open are both dataset changes, so both reset
    # every particle setting rather than carrying the previous dataset's
    # species, subset, seed, point size, and colors into the new one.
    add_test(
        NAME qt_particle_settings_reset_smoke_3d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_particle_settings_reset_3d"
            -DMODE=particle-settings-reset
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(
        qt_particle_settings_reset_smoke_3d PROPERTIES TIMEOUT 30)
    # A rubber-band selection in one 3-D panel synchronizes the selected
    # normalized subregion to all three panels by default.
    add_test(
        NAME qt_rubber_zoom_sync_smoke_3d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_rubber_zoom_sync_3d"
            -DMODE=rubber-zoom-sync
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_rubber_zoom_sync_smoke_3d PROPERTIES TIMEOUT 30)
    # Disabling the option preserves the former panel-local rubber-band
    # behavior; wheel zoom is independently panel-local in ImageView.
    add_test(
        NAME qt_rubber_zoom_local_smoke_3d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_rubber_zoom_local_3d"
            -DMODE=rubber-zoom-local
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_rubber_zoom_local_smoke_3d PROPERTIES TIMEOUT 30)
    # Regression test for issue #45: rubber-band zoom on a dataset whose
    # full-domain raster is capped at maxSliceOutputDimension must keep the
    # arrived (finer-density) crop fully framed instead of over-zooming. The
    # wide fixture is 8192x8 cells, so its full raster is capped to 4096 px.
    add_test(
        NAME qt_rubber_overzoom_smoke_2d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d_wide"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_rubber_overzoom"
            -DMODE=rubber-overzoom
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_rubber_overzoom_smoke_2d PROPERTIES TIMEOUT 30)
    # Shift+dragging a rubber-band-cropped panel must preserve its transform,
    # both for a valid data-region pan and for a no-op at the domain edge.
    add_test(
        NAME qt_pan_zoom_preservation_smoke_3d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_pan_zoom_3d"
            -DMODE=pan-zoom
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_pan_zoom_preservation_smoke_3d PROPERTIES TIMEOUT 30)
    # Regression test for contours-stale-after-visible-range-sync: switching a
    # 3-D dataset to contours in Visible+log mode must re-derive every panel's
    # contour levels from the shared range, not each panel's local range.
    add_test(
        NAME qt_contour_sync_smoke_3d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_contour_sync_3d"
            -DMODE=contour-sync
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_contour_sync_smoke_3d PROPERTIES TIMEOUT 30)
    add_test(
        NAME qt_visible_sync_staleness_smoke_3d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_visible_sync_staleness_3d"
            -DMODE=visible-sync-staleness
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(
        qt_visible_sync_staleness_smoke_3d PROPERTIES TIMEOUT 30)
    add_test(
        NAME qt_quit_smoke_3d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_quit_3d"
            -DMODE=quit
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_quit_smoke_3d PROPERTIES TIMEOUT 30)
    add_test(
        NAME qt_window_close_pool_smoke_3d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_window_close_pool_3d"
            -DMODE=window-close-pool
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_window_close_pool_smoke_3d PROPERTIES TIMEOUT 30)
    add_test(
        NAME qt_close_window_smoke_3d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_close_window_3d"
            -DMODE=close-window
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_close_window_smoke_3d PROPERTIES TIMEOUT 30)
    add_test(
        NAME qt_quit_on_failure_smoke_3d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_quit_on_failure_3d"
            -DMODE=quit-on-failure
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_quit_on_failure_smoke_3d PROPERTIES TIMEOUT 30)
    if(NOT WIN32)
        # Uses a POSIX shell stand-in for ffmpeg (see the driver's export-quit
        # MODE); skipped on Windows.
        add_test(
            NAME qt_export_quit_smoke_3d
            COMMAND "${CMAKE_COMMAND}"
                "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
                "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
                "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
                "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_export_quit_3d"
                -DMODE=export-quit
                -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
        )
        set_tests_properties(qt_export_quit_smoke_3d PROPERTIES TIMEOUT 30)
    endif()
    add_test(
        NAME qt_multifab_missing_range
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_missing_range"
            -DMODE=missing-range
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_multifab_missing_range PROPERTIES TIMEOUT 30)
    add_test(
        NAME qt_multifab_non_finite
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_non_finite"
            -DMODE=non-finite
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_multifab_non_finite PROPERTIES TIMEOUT 30)
    add_test(
        NAME qt_raw_fab_selector
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_raw_fab"
            -DMODE=raw-fab
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_raw_fab_selector PROPERTIES TIMEOUT 30)
    add_test(
        NAME qt_multifab_fab_mode
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_multifab_fab"
            -DMODE=multifab-fab
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_multifab_fab_mode PROPERTIES TIMEOUT 30)
    # Two regressions for the standalone-FAB selector rollback. Both gate the
    # shared pool to one occupied thread so the overlap is structural rather
    # than timed: the reads cannot start until the test releases them.
    add_test(
        NAME qt_fab_overlap_failure
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_fab_overlap_failure"
            -DMODE=fab-overlap-failure
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_fab_overlap_failure PROPERTIES TIMEOUT 60)
    # The LC_NUMERIC pin, end to end: under a comma-decimal locale the reader's
    # strtod stops at the '.' in "0.5" and nothing opens, so an ordinary slice
    # smoke test is the whole regression. Skips visibly (never silently) when no
    # comma locale is installed and localedef cannot build one.
    add_test(
        NAME qt_comma_locale_open
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_comma_locale"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_locale_smoke_driver.cmake"
    )
    set_tests_properties(qt_comma_locale_open PROPERTIES
        SKIP_REGULAR_EXPRESSION "AMREXPLORER_LOCALE_SKIP"
        TIMEOUT 120)
    add_test(
        NAME qt_fab_direct_open_failure
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_fab_direct_open"
            -DMODE=fab-direct-open-failure
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_fab_direct_open_failure PROPERTIES TIMEOUT 60)
    # Regression test for fab-round-trip-loses-visible-region: zooming a MultiFab
    # slice, drilling into a FAB, and going back must leave the restored view
    # still zoomed (visibleRegion written back into the view state).
    add_test(
        NAME qt_fab_zoom_smoke_2d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_fab_zoom"
            -DMODE=fab-zoom
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_fab_zoom_smoke_2d PROPERTIES TIMEOUT 30)
    # Regression test for cache-budget-exceeded-hard-fails-after-load: a
    # post-load slice that overflows the cache budget must degrade to a lower
    # composite level (like the initial load) rather than hard-fail.
    add_test(
        NAME qt_cache_budget_smoke_2d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_cache_budget"
            -DMODE=cache-budget
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_cache_budget_smoke_2d PROPERTIES TIMEOUT 30)
    add_test(
        NAME qt_sequence_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_sequence"
            -DMODE=sequence
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_sequence_smoke PROPERTIES TIMEOUT 30)
    # Regression for sequence-ui-annoyances: an idle frame request must not
    # reload the frame already on screen, and a frame refresh must not reopen
    # the Animation dock the user hid.
    add_test(
        NAME qt_sequence_noop_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_sequence_noop"
            -DMODE=sequence-noop
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_sequence_noop_smoke PROPERTIES TIMEOUT 30)
    # Regression for low-priority-ui-polish item 3: the toolbar Scale button and
    # View > Scale are one state, and it resets when a dataset opens fitted.
    add_test(
        NAME qt_scale_state_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_scale_state"
            -DMODE=scale-state
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_scale_state_smoke PROPERTIES TIMEOUT 30)
    # A fixed scale survives into a sequence, but what it comes to depends on
    # the frame's domain: 4x picked on a narrow plotfile really applies 2x on a
    # sequence 8192 finest cells across, and the button must be restated.
    add_test(
        NAME qt_sequence_scale_report_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DSECOND_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d_wide"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_sequence_scale_report"
            -DMODE=sequence-scale-report
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_sequence_scale_report_smoke PROPERTIES TIMEOUT 30)
    # A spherical view never reports a reduced scale: its raster is warped, so
    # one pixel does not stand for a fixed number of cells. The fixture is
    # 8192 finest cells across, so a Cartesian view of it would report one --
    # which is what makes this distinguish the exclusion from a domain that
    # simply does not clamp.
    add_test(
        NAME qt_spherical_scale_report_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d_wide_spherical"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_spherical_scale_report"
            -DMODE=spherical-scale-report
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_spherical_scale_report_smoke PROPERTIES TIMEOUT 30)
    # Controls reachable before any dataset is: an Animation panel opened from
    # the View menu with nothing loaded must not survive the next open holding
    # no controls, and Reset Zoom with no views must still report Fit.
    add_test(
        NAME qt_idle_ui_state_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_idle_ui_state"
            -DMODE=idle-ui-state
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_idle_ui_state_smoke PROPERTIES TIMEOUT 30)
    # Hiding the Animation panel while it holds the 3-D sweep controls must not
    # keep it hidden when a sequence arrives and it holds the transport.
    add_test(
        NAME qt_animation_dock_role_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_3d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_animation_dock_role"
            -DMODE=animation-dock-role
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_animation_dock_role_smoke PROPERTIES TIMEOUT 30)
    # The arrow keys must pan the focused view and must not be taken from a
    # toolbar widget, which is what the window-context QShortcuts used to do.
    # The ImageView unit test cannot see this: it delivers events to the view
    # itself, bypassing the focus routing that is the whole point.
    add_test(
        NAME qt_arrow_key_routing_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_arrow_key_routing"
            -DMODE=arrow-key-routing
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_arrow_key_routing_smoke PROPERTIES TIMEOUT 30)
    # Regression for fixed-scale-clamped-native-raster: plotfile_2d_wide is
    # 8192 finest cells across, twice the largest whole-domain raster, so a
    # local 32x really applies 16x. The UI must say so, and the number it says
    # must be the one the view is using.
    add_test(
        NAME qt_effective_scale_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d_wide"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_effective_scale"
            -DMODE=effective-scale
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_effective_scale_smoke PROPERTIES TIMEOUT 30)
    # ...and playback must stop on a frame it cannot read, rather than wrapping
    # back onto it every cycle.
    add_test(
        NAME qt_sequence_failure_stops_playback_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_sequence_failure"
            -DMODE=sequence-failure
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_sequence_failure_stops_playback_smoke
        PROPERTIES TIMEOUT 30)
    # Regression for open-sequence-stale-fab-state: opening a plotfile sequence
    # after a raw FAB must clear the FAB view state (dock hidden, no "— FAB"
    # title), since openSequence does not go through openDatasetImpl.
    add_test(
        NAME qt_sequence_after_fab_smoke_2d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_sequence_after_fab"
            -DMODE=sequence-after-fab
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_sequence_after_fab_smoke_2d PROPERTIES TIMEOUT 30)
    add_test(
        NAME qt_remote_sequence_after_fab_smoke_2d
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_remote_sequence_after_fab"
            -DMODE=remote-sequence-after-fab
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_remote_sequence_after_fab_smoke_2d PROPERTIES
        TIMEOUT 30)
    # A normal slice-affecting control change while a foreground frame is
    # loading invalidates that frame specification. The replacement load must
    # complete instead of leaving sequence playback latched in-flight.
    add_test(
        NAME qt_sequence_spec_change_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_sequence_spec_change"
            -DMODE=sequence-spec-change
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(qt_sequence_spec_change_smoke PROPERTIES TIMEOUT 30)
    foreach(factor IN ITEMS 1 4)
        add_test(
            NAME qt_fixed_scale_${factor}x_arrival_smoke
            COMMAND "${CMAKE_COMMAND}"
                "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
                "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
                "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
                "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_fixed_scale_${factor}x"
                "-DMODE=fixed-scale-${factor}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
        )
        set_tests_properties(
            qt_fixed_scale_${factor}x_arrival_smoke PROPERTIES TIMEOUT 30)
    endforeach()
    add_test(
        NAME qt_sequence_transform_preserve_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_sequence_transform_preserve"
            -DMODE=sequence-transform-preserve
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(
        qt_sequence_transform_preserve_smoke PROPERTIES TIMEOUT 30)
    # A compatible physical crop rendered at a different raster resolution in
    # the next sequence frame preserves its visible data window.
    add_test(
        NAME qt_sequence_density_preserve_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DSECOND_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_nonuniform"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_sequence_density_preserve"
            -DMODE=sequence-density-preserve
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(
        qt_sequence_density_preserve_smoke PROPERTIES TIMEOUT 30)
    # Regression test for the sequence timing edge: if frame stepping cancels
    # a rubber-band crop before its raster lands, the next frame's cropped
    # raster can have the same pixel dimensions as the cached full-domain
    # raster. The incompatible full-domain transform must still be discarded.
    add_test(
        NAME qt_sequence_equal_size_transform_preserve_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DSECOND_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_refined_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_sequence_equal_size_transform_preserve"
            -DMODE=sequence-equal-size-transform-preserve
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(
        qt_sequence_equal_size_transform_preserve_smoke PROPERTIES TIMEOUT 30)
    add_test(
        NAME qt_sequence_geometry_refit_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMATERIALIZER=$<TARGET_FILE:fixture_materializer>"
            "-DAMREXPLORER_QT=$<TARGET_FILE:amrexplorer_qt>"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/data/plotfile_2d"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fixtures_sequence_geometry_refit"
            -DMODE=sequence-geometry-refit
            -P "${CMAKE_CURRENT_SOURCE_DIR}/qt_smoke_driver.cmake"
    )
    set_tests_properties(
        qt_sequence_geometry_refit_smoke PROPERTIES TIMEOUT 30)
endif()

# Slice-path benchmark. Registered as a functional guard (correctness + a
# generous timeout), not a wall-clock threshold -- perf gates flake in CI. Run
# it by hand for numbers; scale the workload via its arguments.
add_executable(bench_slice_query benchmark/bench_slice_query.cpp)
target_link_libraries(
    bench_slice_query PRIVATE amrexplorer::query amrexplorer::warnings)
add_test(NAME slice_query_benchmark COMMAND bench_slice_query)
set_tests_properties(slice_query_benchmark PROPERTIES TIMEOUT 120)
# A second run at a non-1:1 outputDim: at outputDim == domain every bilinear
# weight is exactly 0, so only this one actually exercises linear blending (and
# the outputDim-derived expectation) in CI. No RUN_SERIAL: this asserts nothing
# about wall clock and ctest hides its timings on success.
add_test(NAME slice_query_benchmark_resampled
    COMMAND bench_slice_query 16 8 200 1)
set_tests_properties(slice_query_benchmark_resampled PROPERTIES TIMEOUT 120)
# Two clusters of blocks far apart (the ordinary AMR shape): exercises the
# block index's sparse-tile path and the layout-aware coverage checks.
# The trailing 1 requires the linear pass to have validated interpolated
# values, so a change to these arguments cannot make the run vacuous.
add_test(NAME slice_query_benchmark_clustered
    COMMAND bench_slice_query 8 8 200 1 24 1)
set_tests_properties(slice_query_benchmark_clustered PROPERTIES TIMEOUT 120)

# Volume ray-caster benchmark: the same shape as bench_slice_query (a
# functional guard with a generous timeout, timings printed for reading by
# hand), over the per-frame cast the interactive volume view repeats.
add_executable(bench_volume_render benchmark/bench_volume_render.cpp)
target_link_libraries(
    bench_volume_render PRIVATE amrexplorer::render3d amrexplorer::warnings)
add_test(NAME volume_render_benchmark COMMAND bench_volume_render)
set_tests_properties(volume_render_benchmark PROPERTIES TIMEOUT 120)

# Fuzz/property harness for the crafted-plotfile header parsers. No remote
# dependency, so it builds and runs (including under the sanitizers preset)
# regardless of remote support. Deterministic + bounded; see the file header
# for libFuzzer use.
add_executable(fuzz_header_parsing fuzz/fuzz_header_parsing.cpp)
target_include_directories(fuzz_header_parsing PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/fuzz")
# amrexplorer::io, matching the sibling tests of amrvis::detail: the helpers
# are header-only today, but the io target is their home.
target_link_libraries(
    fuzz_header_parsing PRIVATE amrexplorer::io amrexplorer::warnings)
amrexplorer_add_fuzz_target(fuzz_header_parsing)

# Fuzz/property harness for the expression parser. Expression text is typed by
# hand but also imported from files, so it gets the same treatment as the other
# untrusted-input parsers. Deterministic + bounded; see the file header for
# libFuzzer use.
add_executable(fuzz_expression fuzz/fuzz_expression.cpp)
target_include_directories(fuzz_expression PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/fuzz")
target_link_libraries(
    fuzz_expression PRIVATE amrexplorer::expression amrexplorer::warnings)
amrexplorer_add_fuzz_target(fuzz_expression)
