add_executable(amrexplorer_qt
    AnimationPanel.cpp
    AnimationPanel.hpp
    CacheConfig.hpp
    ColorBarWidget.cpp
    ColorBarWidget.hpp
    DatasetExtract.hpp
    DatasetWindow.cpp
    DatasetWindow.hpp
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
    NumberFormat.cpp
    NumberFormat.hpp
    ScientificDoubleSpinBox.cpp
    ScientificDoubleSpinBox.hpp
    SetContoursDialog.cpp
    SetContoursDialog.hpp
    Theme.hpp
    UserGuideDialog.cpp
    UserGuideDialog.hpp
    main.cpp
    ${CMAKE_SOURCE_DIR}/resources/amrexplorer.qrc
    ${CMAKE_SOURCE_DIR}/resources/user-guide.qrc
)
set_target_properties(amrexplorer_qt PROPERTIES OUTPUT_NAME amrexplorer AUTOMOC ON AUTORCC ON)

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
        amrexplorer::io
        amrexplorer::query
        amrexplorer::render2d
        amrexplorer::warnings
        Qt6::Concurrent
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)
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
