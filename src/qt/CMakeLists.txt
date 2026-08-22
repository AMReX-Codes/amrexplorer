add_executable(amrexplorer_qt
    AnimationExporter.cpp
    AnimationExporter.hpp
    AnimationPanel.cpp
    AnimationPanel.hpp
    AppSettings.hpp
    CacheConfig.hpp
    ColorBarWidget.cpp
    ColorBarWidget.hpp
    CurrentRowBulletDelegate.hpp
    DatasetExtract.hpp
    DatasetWindow.cpp
    DatasetWindow.hpp
    DiagnosticsModel.cpp
    DiagnosticsModel.hpp
    FabNavigator.cpp
    FabNavigator.hpp
    FabSelectorDock.cpp
    FabSelectorDock.hpp
    ImageView.cpp
    ImageView.hpp
    IsoWidget.cpp
    IsoWidget.hpp
    LinePlotRequest.hpp
    LinePlotWindow.cpp
    LinePlotWindow.hpp
    MainWindow.cpp
    MainWindow.hpp
    MainWindowDataset.cpp
    MainWindowInteraction.cpp
    MainWindowInternal.hpp
    MainWindowSlice.cpp
    NumberFormat.cpp
    NumberFormat.hpp
    PaletteController.cpp
    PaletteController.hpp
    ParticleController.cpp
    ParticleController.hpp
    QtErrorText.hpp
    RangeController.cpp
    RangeController.hpp
    RemoteEndpoint.hpp
    RemoteFileDialog.cpp
    RemoteFileDialog.hpp
    RemoteOpenDialog.cpp
    RemoteOpenDialog.hpp
    RemoteSessionController.cpp
    RemoteSessionController.hpp
    ScientificDoubleSpinBox.cpp
    ScientificDoubleSpinBox.hpp
    SshConnectArguments.hpp
    SshRemoteSession.cpp
    SshRemoteSession.hpp
    SequenceController.cpp
    SequenceController.hpp
    SetContoursDialog.cpp
    SetContoursDialog.hpp
    Theme.hpp
    UserGuideDialog.cpp
    UserGuideDialog.hpp
    VolumeController.cpp
    VolumeController.hpp
    VolumeWindow.cpp
    VolumeWindow.hpp
    WidgetImageExport.hpp
    main.cpp
    ${CMAKE_SOURCE_DIR}/resources/amrexplorer.qrc
    ${CMAKE_SOURCE_DIR}/resources/user-guide.qrc
)
set_target_properties(amrexplorer_qt PROPERTIES OUTPUT_NAME amrexplorer AUTOMOC ON AUTORCC ON)

# MainWindowTestAccess.cpp defines the ~90 ForTest accessors the offscreen
# smoke tests reach through, and the SmokeHarness*.cpp "--...-smoke-test"
# branches (which main.cpp dispatches into) are their only callers. All are
# gated on the same definition, so a release build carries none of them --
# the same shape AMREXPLORER_ENABLE_SERVER_TEST_HOOKS uses for the remote
# library. The smoke tests run against this executable, so they need
# AMREXPLORER_BUILD_TESTS=ON, which is the default and what CI configures.
if(AMREXPLORER_BUILD_TESTS)
    target_sources(amrexplorer_qt PRIVATE
        MainWindowTestAccess.cpp
        SmokeHarness.cpp
        SmokeHarness.hpp
        SmokeHarnessFab.cpp
        SmokeHarnessInternal.hpp
        SmokeHarnessLifecycle.cpp
        SmokeHarnessRange.cpp
        SmokeHarnessRemote.cpp
        SmokeHarnessSequence.cpp
        SmokeHarnessVolume.cpp
        SmokeHarnessZoom.cpp)
    target_compile_definitions(amrexplorer_qt PRIVATE AMREXPLORER_QT_TEST_ACCESS)
endif()

if(APPLE AND AMREXPLORER_BUILD_MACOS_APP_BUNDLE)
    set(amrexplorer_macos_icon "${CMAKE_SOURCE_DIR}/resources/amrexplorer.icns")
    set_source_files_properties("${amrexplorer_macos_icon}" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
    )
    target_sources(amrexplorer_qt PRIVATE "${amrexplorer_macos_icon}")
    set_target_properties(amrexplorer_qt PROPERTIES
        OUTPUT_NAME amrexplorer
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME AMReXplorer
        MACOSX_BUNDLE_GUI_IDENTIFIER org.amrex-codes.amrexplorer
        MACOSX_BUNDLE_ICON_FILE amrexplorer.icns
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
endif()

target_link_libraries(amrexplorer_qt
    PRIVATE
        amrexplorer::core
        amrexplorer::data
        amrexplorer::io
        amrexplorer::query
        amrexplorer::render2d
        amrexplorer::render3d
        amrexplorer::pipeline
        amrexplorer::warnings
        Qt6::Concurrent
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)
target_link_libraries(amrexplorer_qt PRIVATE amrexplorer::remote)
target_compile_features(amrexplorer_qt PRIVATE cxx_std_20)
target_compile_definitions(amrexplorer_qt PRIVATE AMREXPLORER_VERSION="${PROJECT_VERSION}")

install(TARGETS amrexplorer_qt
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

if(UNIX AND NOT APPLE)
    install(FILES "${CMAKE_SOURCE_DIR}/resources/amrexplorer.desktop"
        DESTINATION share/applications
    )
    foreach(icon_size 16 32 64 128 256)
        install(FILES
            "${CMAKE_SOURCE_DIR}/resources/amrexplorer-${icon_size}.png"
            DESTINATION "share/icons/hicolor/${icon_size}x${icon_size}/apps"
            RENAME amrexplorer.png
        )
    endforeach()
endif()
