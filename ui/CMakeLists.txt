cmake_minimum_required(VERSION 3.24)
project(FerrousUi LANGUAGES CXX)

include(CTest)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(FERROUS_ENABLE_PROFILE_LOGS "Compile UI instrumentation and profiling log output into binaries" OFF)
option(FERROUS_WARNINGS_AS_ERRORS "Fail UI builds on compiler warnings" ON)
set(FERROUS_CARGO_ARGS "" CACHE STRING "Extra arguments for the Cargo build of the Rust library, such as --frozen")

find_package(Qt6 6.6 REQUIRED COMPONENTS Core DBus Gui Qml Quick QuickControls2 Network Widgets Concurrent)
find_package(KF6Kirigami 6.0 REQUIRED)
find_package(PkgConfig REQUIRED)
# The system libraries that the Rust static library needs, which a static
# archive cannot bring along; dbus-1 is the Secret Service keyring's.
pkg_check_modules(FERROUS_GSTREAMER REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-audio-1.0
    gstreamer-pbutils-1.0
    gio-2.0
    dbus-1
)

qt_standard_project_setup(REQUIRES 6.6)
qt_policy(SET QTP0004 NEW)

function(ferrous_enable_strict_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
        if(FERROUS_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
        if(FERROUS_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE -Werror)
        endif()
    endif()
endfunction()

set(FERROUS_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Cargo from $CARGO, or else from the path, as in a shell; with rustup, its
# proxy then picks the toolchain at build time and honours RUSTUP_TOOLCHAIN.
# Cargo's own install directory is only a fallback for an environment without
# it on the path.
if(DEFINED ENV{CARGO} AND NOT FERROUS_CARGO)
    set(FERROUS_CARGO "$ENV{CARGO}" CACHE FILEPATH "Cargo executable for the Rust library")
endif()
set(_ferrous_cargo_fallback_dirs "$ENV{HOME}/.cargo/bin")
if(DEFINED ENV{CARGO_HOME})
    list(PREPEND _ferrous_cargo_fallback_dirs "$ENV{CARGO_HOME}/bin")
endif()
find_program(FERROUS_CARGO cargo
    PATHS ${_ferrous_cargo_fallback_dirs}
    DOC "Cargo executable for the Rust library"
    REQUIRED
)
message(STATUS "Cargo: ${FERROUS_CARGO}")

# Each build directory has its own Cargo target directory, so a build with
# other features, such as profiling, never replaces the archive this one links,
# and CARGO_TARGET_DIR in the environment cannot move it away from here.
set(FERROUS_CARGO_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo"
    CACHE PATH "Cargo target directory for the Rust library")
set(FERROUS_RUST_STATIC_LIB "${FERROUS_CARGO_TARGET_DIR}/release/libferrous.a")

# Read app version from Cargo.toml's [package] table (single source of truth),
# and configure again when Cargo.toml changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FERROUS_ROOT_DIR}/Cargo.toml")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FerrousCargoVersion.cmake")
ferrous_cargo_package_version("${FERROUS_ROOT_DIR}/Cargo.toml" FERROUS_APP_VERSION)
message(STATUS "Ferrous version: ${FERROUS_APP_VERSION}")
set(FERROUS_RUST_FEATURES "gst")
if(FERROUS_ENABLE_PROFILE_LOGS)
    set(FERROUS_RUST_FEATURES "gst,profiling-logs")
endif()
separate_arguments(_ferrous_cargo_extra_args UNIX_COMMAND "${FERROUS_CARGO_ARGS}")

add_custom_target(ferrous_rust_bridge ALL
    COMMAND "${FERROUS_CARGO}" build --release --locked --lib
        --manifest-path "${FERROUS_ROOT_DIR}/Cargo.toml"
        --target-dir "${FERROUS_CARGO_TARGET_DIR}"
        --no-default-features --features "${FERROUS_RUST_FEATURES}"
        ${_ferrous_cargo_extra_args}
    WORKING_DIRECTORY "${FERROUS_ROOT_DIR}"
    BYPRODUCTS "${FERROUS_RUST_STATIC_LIB}"
    COMMENT "Building Rust static library for in-process bridge"
    VERBATIM
)

qt_add_executable(ferrous
    src/AppInstanceController.cpp
    src/main.cpp
    src/CoverImageProvider.cpp
    src/DiagnosticsLog.cpp
    src/DisplaySleepInhibitor.cpp
    src/BinaryBridgeCodec.cpp
    src/BridgeClient.cpp
    src/GlobalSearchResultsModel.cpp
    src/LibraryTreeModel.cpp
    src/MprisController.cpp
    src/CoverRevisionLinks.cpp
    src/PlaybackClock.cpp
    src/TagEditorController.cpp
    src/TagEditorTableModel.cpp
    src/SpectrogramItem.cpp
    src/WindowActivation.cpp
    src/WaveformItem.cpp
    src/WaveformSceneGraph.cpp
    src/WaveformSceneGraph.h
    src/WaveformEditorItem.cpp
    src/LevelMeterItem.cpp
    src/QmlTypes.cpp
)

target_include_directories(ferrous PRIVATE src)
target_compile_definitions(ferrous PRIVATE FERROUS_APP_VERSION="${FERROUS_APP_VERSION}")
add_dependencies(ferrous ferrous_rust_bridge)
ferrous_enable_strict_warnings(ferrous)

qt_add_resources(ferrous ferrous_app_resources
    PREFIX "/icons"
    FILES
        assets/ferrous.svg
)

# The QML module is a library of its own, which the app and the test that
# loads the module as the app does both link. They link the whole archive:
# nothing refers to its compiled QML or its type registration, which the
# linker would otherwise leave out, so QML would compile at run time.
qt_add_library(ferrous_ui_qml STATIC)
qt_add_qml_module(ferrous_ui_qml
    URI FerrousUi
    VERSION 1.0
    NO_PLUGIN
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/FerrousUi"
    QML_FILES
        qml/Main.qml
        qml/components/ActionShortcuts.qml
        qml/components/AlbumArtTile.qml
        qml/components/QueueUndoNotice.qml
        qml/components/ColumnHeaderRow.qml
        qml/components/ErrorBanner.qml
        qml/components/SearchResultIcon.qml
        qml/components/FullscreenControlsAutoHide.qml
        qml/components/MetadataMarqueeRow.qml
        qml/components/PopupTransition.qml
        qml/components/SurfaceCard.qml
        qml/components/TrackMetadataCard.qml
        qml/components/UiPalette.qml
        qml/components/ViewerCloseButton.qml
        qml/components/VisualizationModeSwitch.qml
        qml/controllers/ErrorController.qml
        qml/controllers/GlobalSearchController.qml
        qml/controllers/LibraryController.qml
        qml/controllers/PlaybackController.qml
        qml/controllers/QueueController.qml
        qml/controllers/ViewerController.qml
        qml/controllers/WindowLayoutController.qml
        qml/dialogs/AboutDialog.qml
        qml/dialogs/ConfirmDialog.qml
        qml/dialogs/DiagnosticsDialog.qml
        qml/dialogs/GlobalSearchDialog.qml
        qml/dialogs/ItunesArtworkDialog.qml
        qml/dialogs/LibraryRootNameDialog.qml
        qml/dialogs/PreferencesDialog.qml
        qml/dialogs/QuitSaveFailedDialog.qml
        qml/dialogs/RenamePreviewDialog.qml
        qml/dialogs/AutoNumberDialog.qml
        qml/dialogs/TagEditorCloseConfirmDialog.qml
        qml/dialogs/TagEditorDialog.qml
        qml/dialogs/TagEditorQuitWaitDialog.qml
        qml/dialogs/TagEditorStatusDetailsDialog.qml
        qml/panes/LibraryPane.qml
        qml/panes/QueuePane.qml
        qml/panes/SidebarPane.qml
        qml/panes/SpectrogramPane.qml
        qml/panes/StatusBar.qml
        qml/panes/TransportBar.qml
        qml/preferences/DisplayPage.qml
        qml/preferences/LastFmPage.qml
        qml/preferences/LibraryPage.qml
        qml/preferences/SpectrogramPage.qml
        qml/preferences/SystemMediaPage.qml
        qml/viewers/AlbumArtSurface.qml
        qml/viewers/AlbumArtViewerShell.qml
        qml/viewers/SpectrogramSurface.qml
        qml/viewers/SpectrogramViewerShell.qml
        qml/viewers/WaveformSurface.qml
    RESOURCES
        qml/assets/channel-mono.svg
        qml/assets/channel-stereo.svg
        qml/assets/channel-4_0.svg
        qml/assets/channel-5_0.svg
        qml/assets/channel-5_1.svg
        qml/assets/channel-7_1.svg
        qml/logic/ChannelLabels.js
        qml/logic/ColorUtils.js
        qml/logic/ErrorUtils.js
        qml/logic/FormatUtils.js
        qml/logic/KeyUtils.js
        qml/logic/PathUtils.js
        qml/logic/WheelUtils.js
)

ferrous_enable_strict_warnings(ferrous_ui_qml)

set_target_properties(ferrous PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
target_link_libraries(ferrous PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,ferrous_ui_qml>"
    "${FERROUS_RUST_STATIC_LIB}"
    PkgConfig::FERROUS_GSTREAMER
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Concurrent
    Qt6::Network
    Qt6::Widgets
    KF6::Kirigami
    dl
    m
    pthread
)

if(FERROUS_ENABLE_PROFILE_LOGS)
    target_sources(ferrous PRIVATE src/SpectrogramSeekTrace.cpp)
    target_compile_definitions(ferrous PRIVATE FERROUS_ENABLE_PROFILE_LOGS=1)
endif()

install(TARGETS ferrous RUNTIME DESTINATION bin)
install(FILES ferrous.desktop DESTINATION share/applications)
install(FILES assets/ferrous.svg DESTINATION share/icons/hicolor/scalable/apps RENAME ferrous.svg)
install(FILES io.github.tiaxi.ferrous.metainfo.xml DESTINATION share/metainfo)

if(BUILD_TESTING)
    find_package(Qt6 6.6 REQUIRED COMPONENTS Test)

    # Run without a platform chosen, as a developer may run it, a test binary
    # draws offscreen instead of opening windows; tests/TestPlatform.h. With
    # -platform it uses the platform chosen, and the check skips.
    function(ferrous_add_default_platform_test target test_function)
        set(no_platform "QT_QPA_PLATFORM=unset:;DISPLAY=unset:;WAYLAND_DISPLAY=unset:")
        add_test(NAME ${target}_default_platform COMMAND ${target} ${test_function})
        set_tests_properties(${target}_default_platform PROPERTIES
            ENVIRONMENT_MODIFICATION "${no_platform}"
            FAIL_REGULAR_EXPRESSION "SKIP"
            TIMEOUT 60
        )
        add_test(NAME ${target}_chosen_platform
            COMMAND ${target} ${test_function} -platform minimal
        )
        set_tests_properties(${target}_chosen_platform PROPERTIES
            ENVIRONMENT_MODIFICATION "${no_platform}"
            PASS_REGULAR_EXPRESSION "SKIP   : [A-Za-z]+::${test_function}\\(\\)"
            TIMEOUT 60
        )
    endfunction()

    add_test(NAME ferrous_metainfo_release_version
        COMMAND "${CMAKE_COMMAND}"
            -DMETAINFO_FILE=${CMAKE_CURRENT_SOURCE_DIR}/io.github.tiaxi.ferrous.metainfo.xml
            -DEXPECTED_VERSION=${FERROUS_APP_VERSION}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_metainfo_release.cmake"
    )
    # The release workflow runs this check on the pushed tag.
    find_program(FERROUS_BASH bash)
    if(FERROUS_BASH)
        add_test(NAME ferrous_release_version_check
            COMMAND "${CMAKE_COMMAND}"
                -DBASH=${FERROUS_BASH}
                -DCHECK_SCRIPT=${FERROUS_ROOT_DIR}/scripts/check-release-version.sh
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/release-version-check
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_release_version.cmake"
        )
        add_test(NAME ferrous_release_version_consistent
            COMMAND "${FERROUS_BASH}" "${FERROUS_ROOT_DIR}/scripts/check-release-version.sh"
                "v${FERROUS_APP_VERSION}"
        )
        # Configuring and the scripts read the same [package] version.
        add_test(NAME ferrous_cargo_package_version
            COMMAND "${CMAKE_COMMAND}"
                -DBASH=${FERROUS_BASH}
                -DSCRIPT=${FERROUS_ROOT_DIR}/scripts/cargo-package-version.sh
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cargo-package-version
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cargo_package_version.cmake"
        )
        # The other repository scripts, run on scratch copies and stand-in
        # commands.
        foreach(_script_test IN ITEMS bump-version install-rpm run-ui-cleanup)
            add_test(NAME ferrous_script_${_script_test}
                COMMAND "${FERROUS_BASH}" "${FERROUS_ROOT_DIR}/scripts/tests/${_script_test}-test.sh"
                    "${CMAKE_CURRENT_BINARY_DIR}/script-tests/${_script_test}"
            )
        endforeach()
    endif()
    find_program(FERROUS_APPSTREAMCLI appstreamcli)
    if(FERROUS_APPSTREAMCLI)
        add_test(NAME ferrous_metainfo_validate
            COMMAND "${FERROUS_APPSTREAMCLI}" validate --no-net
                "${CMAKE_CURRENT_SOURCE_DIR}/io.github.tiaxi.ferrous.metainfo.xml"
        )
    endif()
    find_program(FERROUS_DESKTOP_FILE_VALIDATE desktop-file-validate)
    if(FERROUS_DESKTOP_FILE_VALIDATE)
        add_test(NAME ferrous_desktop_file_validate
            COMMAND "${FERROUS_DESKTOP_FILE_VALIDATE}" "${CMAKE_CURRENT_SOURCE_DIR}/ferrous.desktop"
        )
    endif()
    foreach(metadata_test IN ITEMS
            ferrous_metainfo_release_version
            ferrous_release_version_check
            ferrous_release_version_consistent
            ferrous_cargo_package_version
            ferrous_script_bump-version
            ferrous_script_install-rpm
            ferrous_script_run-ui-cleanup
            ferrous_metainfo_validate
            ferrous_desktop_file_validate)
        if(TEST ${metadata_test})
            set_tests_properties(${metadata_test} PROPERTIES TIMEOUT 60)
        endif()
    endforeach()

    qt_add_executable(ferrous_playback_clock_tests
        tests/tst_playback_clock.cpp
        src/PlaybackClock.cpp
    )
    target_include_directories(ferrous_playback_clock_tests PRIVATE src)
    target_link_libraries(ferrous_playback_clock_tests PRIVATE Qt6::Core Qt6::Test)
    ferrous_enable_strict_warnings(ferrous_playback_clock_tests)
    add_test(NAME ferrous_playback_clock_tests COMMAND ferrous_playback_clock_tests)
    set_tests_properties(ferrous_playback_clock_tests PROPERTIES TIMEOUT 60)

    qt_add_executable(ferrous_level_meter_tests
        tests/tst_level_meter.cpp
        src/LevelMeterItem.cpp
        src/PlaybackClock.cpp
    )
    target_include_directories(ferrous_level_meter_tests PRIVATE src)
    set_target_properties(ferrous_level_meter_tests PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
    add_dependencies(ferrous_level_meter_tests ferrous_rust_bridge)
    target_link_libraries(ferrous_level_meter_tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Concurrent Qt6::Test
        "${FERROUS_RUST_STATIC_LIB}" PkgConfig::FERROUS_GSTREAMER dl m pthread
    )
    ferrous_enable_strict_warnings(ferrous_level_meter_tests)
    add_test(NAME ferrous_level_meter_tests COMMAND ferrous_level_meter_tests)
    set_tests_properties(ferrous_level_meter_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        TIMEOUT 90
    )
    ferrous_add_default_platform_test(ferrous_level_meter_tests drawsOffscreenUnlessAPlatformIsChosen)

    qt_add_executable(ferrous_qml_smoke_tests
        tests/tst_qml_smoke.cpp
        src/QmlTypes.cpp
        src/BinaryBridgeCodec.cpp
        src/BridgeClient.cpp
        src/CoverImageProvider.cpp
        src/DiagnosticsLog.cpp
        src/DisplaySleepInhibitor.cpp
        src/GlobalSearchResultsModel.cpp
        src/LibraryTreeModel.cpp
        src/MprisController.cpp
        src/CoverRevisionLinks.cpp
        src/PlaybackClock.cpp
        src/TagEditorController.cpp
        src/TagEditorTableModel.cpp
        src/SpectrogramItem.cpp
        src/WindowActivation.cpp
        src/WaveformItem.cpp
        src/WaveformSceneGraph.cpp
        src/WaveformSceneGraph.h
        src/WaveformEditorItem.cpp
        src/LevelMeterItem.cpp
    )
    target_include_directories(ferrous_qml_smoke_tests PRIVATE src)
    add_dependencies(ferrous_qml_smoke_tests ferrous_rust_bridge)
    target_compile_definitions(ferrous_qml_smoke_tests PRIVATE
        FERROUS_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(ferrous_qml_smoke_tests PRIVATE
        Qt6::Core
        Qt6::DBus
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Concurrent
        Qt6::Network
        Qt6::Widgets
        Qt6::Test
        KF6::Kirigami
        "${FERROUS_RUST_STATIC_LIB}"
        PkgConfig::FERROUS_GSTREAMER
        dl
        m
        pthread
    )
    ferrous_enable_strict_warnings(ferrous_qml_smoke_tests)
    if(FERROUS_ENABLE_PROFILE_LOGS)
        target_sources(ferrous_qml_smoke_tests PRIVATE src/SpectrogramSeekTrace.cpp)
        target_compile_definitions(ferrous_qml_smoke_tests PRIVATE FERROUS_ENABLE_PROFILE_LOGS=1)
    endif()
    set_target_properties(ferrous_qml_smoke_tests PROPERTIES
        QT_QML_MODULE_NO_IMPORT_SCAN TRUE
    )
    # The smoke tests run by area, each area in a process of its own, so a
    # crash or hang in one area costs only that area's results, and quickly.
    # The areas are the test classes of tst_qml_smoke.cpp. Each timeout is
    # many times the area's run time in CI, for slower machines.
    set(FERROUS_QML_SMOKE_AREAS
        spectrogram 180
        waveform 120
        library_queue 120
        transport_input 90
        window 120
        preferences 60
        errors 120
        tag_editor 180
    )
    set(ferrous_smoke_area_names "")
    list(LENGTH FERROUS_QML_SMOKE_AREAS ferrous_smoke_area_values)
    math(EXPR ferrous_smoke_area_last "${ferrous_smoke_area_values} - 1")
    foreach(index RANGE 0 ${ferrous_smoke_area_last} 2)
        math(EXPR timeout_index "${index} + 1")
        list(GET FERROUS_QML_SMOKE_AREAS ${index} area)
        list(GET FERROUS_QML_SMOKE_AREAS ${timeout_index} timeout)
        list(APPEND ferrous_smoke_area_names ${area})
        add_test(NAME ferrous_qml_smoke_${area} COMMAND ferrous_qml_smoke_tests -area ${area})
        set_tests_properties(ferrous_qml_smoke_${area} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            TIMEOUT ${timeout}
        )
    endforeach()
    # Fails when the binary has an area that is not registered here.
    add_test(NAME ferrous_qml_smoke_areas
        COMMAND ferrous_qml_smoke_tests -check-areas ${ferrous_smoke_area_names}
    )
    set_tests_properties(ferrous_qml_smoke_areas PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60
    )
    ferrous_add_default_platform_test(ferrous_qml_smoke_tests drawsOffscreenUnlessAPlatformIsChosen)

    qt_add_executable(ferrous_accessibility_tests
        tests/tst_accessibility.cpp
        src/BinaryBridgeCodec.cpp
        src/BridgeClient.cpp
        src/CoverImageProvider.cpp
        src/DiagnosticsLog.cpp
        src/DisplaySleepInhibitor.cpp
        src/GlobalSearchResultsModel.cpp
        src/LibraryTreeModel.cpp
        src/MprisController.cpp
        src/CoverRevisionLinks.cpp
        src/PlaybackClock.cpp
        src/TagEditorController.cpp
        src/TagEditorTableModel.cpp
        src/SpectrogramItem.cpp
        src/WindowActivation.cpp
        src/WaveformItem.cpp
        src/WaveformSceneGraph.cpp
        src/WaveformSceneGraph.h
        src/WaveformEditorItem.cpp
        src/LevelMeterItem.cpp
    )
    target_include_directories(ferrous_accessibility_tests PRIVATE src)
    add_dependencies(ferrous_accessibility_tests ferrous_rust_bridge)
    target_compile_definitions(ferrous_accessibility_tests PRIVATE
        FERROUS_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(ferrous_accessibility_tests PRIVATE
        Qt6::Core
        Qt6::DBus
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Concurrent
        Qt6::Network
        Qt6::Widgets
        Qt6::Test
        KF6::Kirigami
        "${FERROUS_RUST_STATIC_LIB}"
        PkgConfig::FERROUS_GSTREAMER
        dl
        m
        pthread
    )
    ferrous_enable_strict_warnings(ferrous_accessibility_tests)
    if(FERROUS_ENABLE_PROFILE_LOGS)
        target_sources(ferrous_accessibility_tests PRIVATE src/SpectrogramSeekTrace.cpp)
        target_compile_definitions(ferrous_accessibility_tests PRIVATE
            FERROUS_ENABLE_PROFILE_LOGS=1
        )
    endif()
    set_target_properties(ferrous_accessibility_tests PROPERTIES
        QT_QML_MODULE_NO_IMPORT_SCAN TRUE
    )
    add_test(NAME ferrous_accessibility_tests COMMAND ferrous_accessibility_tests)
    set_tests_properties(ferrous_accessibility_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 180
    )
    ferrous_add_default_platform_test(ferrous_accessibility_tests drawsOffscreenUnlessAPlatformIsChosen)

    qt_add_executable(ferrous_app_instance_tests
        tests/tst_app_instance_controller.cpp
        src/AppInstanceController.cpp
        src/WindowActivation.cpp
    )
    target_include_directories(ferrous_app_instance_tests PRIVATE src)
    target_link_libraries(ferrous_app_instance_tests PRIVATE
        Qt6::Core
        Qt6::DBus
        Qt6::Gui
        Qt6::Test
    )
    ferrous_enable_strict_warnings(ferrous_app_instance_tests)
    add_test(NAME ferrous_app_instance_tests COMMAND ferrous_app_instance_tests)
    set_tests_properties(ferrous_app_instance_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60
    )
    ferrous_add_default_platform_test(ferrous_app_instance_tests drawsOffscreenUnlessAPlatformIsChosen)

    qt_add_executable(ferrous_display_sleep_inhibitor_tests
        tests/tst_display_sleep_inhibitor.cpp
        src/DisplaySleepInhibitor.cpp
    )
    target_include_directories(ferrous_display_sleep_inhibitor_tests PRIVATE src)
    target_link_libraries(ferrous_display_sleep_inhibitor_tests PRIVATE
        Qt6::Core
        Qt6::DBus
        Qt6::Test
    )
    ferrous_enable_strict_warnings(ferrous_display_sleep_inhibitor_tests)
    add_test(
        NAME ferrous_display_sleep_inhibitor_tests
        COMMAND ferrous_display_sleep_inhibitor_tests
    )
    set_tests_properties(ferrous_display_sleep_inhibitor_tests PROPERTIES TIMEOUT 60)

    qt_add_executable(ferrous_bridge_client_tests
        tests/tst_bridge_client.cpp
        src/BinaryBridgeCodec.cpp
        src/BridgeClient.cpp
        src/CoverImageProvider.cpp
        src/DiagnosticsLog.cpp
        src/GlobalSearchResultsModel.cpp
        src/LibraryTreeModel.cpp
        src/MprisController.cpp
        src/CoverRevisionLinks.cpp
        src/PlaybackClock.cpp
        src/TagEditorController.cpp
        src/TagEditorTableModel.cpp
        src/SpectrogramItem.cpp
        src/WindowActivation.cpp
        src/WaveformItem.cpp
        src/WaveformSceneGraph.cpp
        src/WaveformSceneGraph.h
        src/WaveformEditorItem.cpp
    )
    target_include_directories(ferrous_bridge_client_tests PRIVATE src)
    add_dependencies(ferrous_bridge_client_tests ferrous_rust_bridge)
    target_link_libraries(ferrous_bridge_client_tests PRIVATE
        Qt6::Core
        Qt6::DBus
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Concurrent
        Qt6::Network
        Qt6::Widgets
        Qt6::Test
        KF6::Kirigami
        "${FERROUS_RUST_STATIC_LIB}"
        PkgConfig::FERROUS_GSTREAMER
        dl
        m
        pthread
    )
    ferrous_enable_strict_warnings(ferrous_bridge_client_tests)
    if(FERROUS_ENABLE_PROFILE_LOGS)
        target_sources(ferrous_bridge_client_tests PRIVATE src/SpectrogramSeekTrace.cpp)
        target_compile_definitions(ferrous_bridge_client_tests PRIVATE
            FERROUS_ENABLE_PROFILE_LOGS=1
        )
    endif()
    set_target_properties(ferrous_bridge_client_tests PROPERTIES
        QT_QML_MODULE_NO_IMPORT_SCAN TRUE
    )
    add_test(NAME ferrous_bridge_client_tests COMMAND ferrous_bridge_client_tests)
    # Tests that start the real bridge must not depend on an audio device:
    # a machine without one, such as a CI runner, reports it as an error.
    set_tests_properties(ferrous_bridge_client_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;FERROUS_GST_OUTPUT_SINK=fakesink"
        TIMEOUT 180
    )
    ferrous_add_default_platform_test(ferrous_bridge_client_tests drawsOffscreenUnlessAPlatformIsChosen)

    # The QML module as the app loads it; tests/tst_qml_module.cpp.
    qt_add_executable(ferrous_qml_module_tests
        tests/tst_qml_module.cpp
        src/BinaryBridgeCodec.cpp
        src/CoverRevisionLinks.cpp
        src/LevelMeterItem.cpp
        src/LibraryTreeModel.cpp
        src/PlaybackClock.cpp
        src/QmlTypes.cpp
        src/SpectrogramItem.cpp
        src/WaveformEditorItem.cpp
        src/WaveformItem.cpp
        src/WaveformSceneGraph.cpp
        src/WaveformSceneGraph.h
    )
    target_include_directories(ferrous_qml_module_tests PRIVATE src)
    add_dependencies(ferrous_qml_module_tests ferrous_rust_bridge)
    target_compile_definitions(ferrous_qml_module_tests PRIVATE
        FERROUS_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(ferrous_qml_module_tests PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,ferrous_ui_qml>"
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Concurrent
        Qt6::Widgets
        Qt6::Test
        KF6::Kirigami
        "${FERROUS_RUST_STATIC_LIB}"
        PkgConfig::FERROUS_GSTREAMER
        dl
        m
        pthread
    )
    ferrous_enable_strict_warnings(ferrous_qml_module_tests)
    if(FERROUS_ENABLE_PROFILE_LOGS)
        target_sources(ferrous_qml_module_tests PRIVATE src/SpectrogramSeekTrace.cpp)
        target_compile_definitions(ferrous_qml_module_tests PRIVATE FERROUS_ENABLE_PROFILE_LOGS=1)
    endif()
    set_target_properties(ferrous_qml_module_tests PROPERTIES
        QT_QML_MODULE_NO_IMPORT_SCAN TRUE
    )
    add_test(NAME ferrous_qml_module_tests COMMAND ferrous_qml_module_tests)
    set_tests_properties(ferrous_qml_module_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 120
    )
    ferrous_add_default_platform_test(ferrous_qml_module_tests drawsOffscreenUnlessAPlatformIsChosen)

    # The app and the module test have the module's compiled QML.
    if(CMAKE_NM)
        foreach(qml_binary IN ITEMS ferrous ferrous_qml_module_tests)
            add_test(NAME ${qml_binary}_qml_cache_linked
                COMMAND "${CMAKE_COMMAND}"
                    -DNM=${CMAKE_NM}
                    -DBINARY=$<TARGET_FILE:${qml_binary}>
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_qml_cache_linked.cmake"
            )
            set_tests_properties(${qml_binary}_qml_cache_linked PROPERTIES TIMEOUT 60)
        endforeach()
    endif()
endif()
