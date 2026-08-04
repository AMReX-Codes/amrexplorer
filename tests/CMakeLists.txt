add_executable(test_core_metadata unit/test_core_metadata.cpp)
target_link_libraries(test_core_metadata PRIVATE amrexplorer::core amrexplorer::warnings)
add_test(NAME core_metadata COMMAND test_core_metadata)

add_executable(test_geometry unit/test_geometry.cpp)
target_link_libraries(test_geometry PRIVATE amrexplorer::core amrexplorer::warnings)
add_test(NAME geometry COMMAND test_geometry)

add_executable(test_view_data unit/test_view_data.cpp)
target_link_libraries(test_view_data PRIVATE amrexplorer::data amrexplorer::warnings)
add_test(NAME view_data COMMAND test_view_data)

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

    add_executable(test_remote_codec unit/test_remote_codec.cpp)
    target_include_directories(test_remote_codec PRIVATE
        "${PROJECT_SOURCE_DIR}/src/remote"
        "${PROJECT_BINARY_DIR}/src/remote/generated")
    target_link_libraries(test_remote_codec
        PRIVATE
            amrexplorer::remote
            FlatBuffers::FlatBuffers
            amrexplorer::warnings
    )
    add_test(NAME remote_codec COMMAND test_remote_codec)

    add_executable(test_remote_server
        integration/test_remote_server.cpp)
    target_include_directories(test_remote_server PRIVATE
        "${PROJECT_BINARY_DIR}/src/remote/generated")
    target_link_libraries(test_remote_server
        PRIVATE
            amrexplorer::remote
            FlatBuffers::FlatBuffers
            amrexplorer::warnings
            Threads::Threads
    )
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
    set_tests_properties(remote_server PROPERTIES
        FIXTURES_REQUIRED remote_server_private_materialized
        TIMEOUT 30)

    add_executable(test_remote_connection
        integration/test_remote_connection.cpp)
    target_include_directories(test_remote_connection PRIVATE
        "${PROJECT_SOURCE_DIR}/src/remote"
        "${PROJECT_BINARY_DIR}/src/remote/generated")
    target_link_libraries(test_remote_connection
        PRIVATE
            amrexplorer::remote
            FlatBuffers::FlatBuffers
            amrexplorer::warnings
            Threads::Threads
    )
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
endif()

add_executable(test_vismf_index unit/test_vismf_index.cpp)
target_link_libraries(test_vismf_index PRIVATE amrexplorer::io amrexplorer::warnings)
add_test(NAME vismf_index COMMAND test_vismf_index)

add_executable(test_plotfile_header unit/test_plotfile_header.cpp)
target_link_libraries(test_plotfile_header PRIVATE amrexplorer::io amrexplorer::warnings)
add_test(NAME plotfile_header COMMAND test_plotfile_header)

add_executable(test_fits_writer unit/test_fits_writer.cpp)
target_link_libraries(test_fits_writer PRIVATE amrexplorer::io amrexplorer::warnings)
add_test(NAME fits_writer COMMAND test_fits_writer)

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

add_executable(test_slice_query integration/test_slice_query.cpp)
target_link_libraries(test_slice_query PRIVATE amrexplorer::query amrexplorer::warnings)
add_test(NAME slice_query COMMAND test_slice_query)

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
    add_executable(test_remote_endpoint unit/test_remote_endpoint.cpp)
    target_include_directories(test_remote_endpoint
        PRIVATE "${PROJECT_SOURCE_DIR}/src/qt")
    target_link_libraries(test_remote_endpoint
        PRIVATE amrexplorer::warnings)
    add_test(NAME remote_endpoint COMMAND test_remote_endpoint)

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
        NAME qt_remote_silent_hello_close_smoke
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:amrexplorer_qt>"
            --remote-silent-hello-close-smoke-test
    )
    set_tests_properties(qt_remote_silent_hello_close_smoke PROPERTIES
        TIMEOUT 10)

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
        PRIVATE amrexplorer::core amrexplorer::warnings Qt6::Widgets
    )
    add_test(
        NAME line_plot_hover
        COMMAND "${CMAKE_COMMAND}" -E env QT_QPA_PLATFORM=offscreen
            "$<TARGET_FILE:test_line_plot_hover>"
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
