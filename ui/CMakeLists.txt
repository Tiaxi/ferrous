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
# KLocalizedQmlContext is new in 6.8.
find_package(KF6I18n 6.8 REQUIRED)
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
# The gettext domain of every i18n() call; see src/Localization.h.
add_compile_definitions(TRANSLATION_DOMAIN="ferrous")
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
    src/AboutComponents.cpp
    src/AppInstanceController.cpp
    src/ConsoleTee.cpp
    src/main.cpp
    src/CoverImageProvider.cpp
    src/DiagnosticsLog.cpp
    src/Localization.cpp
    src/DisplaySleepInhibitor.cpp
    src/HeapAllocator.cpp
    src/HeapTrimmer.cpp
    src/BinaryBridgeCodec.cpp
    src/BridgeClient.cpp
    src/QueueRows.cpp
    src/GlobalSearchResultsModel.cpp
    src/LibraryTreeModel.cpp
    src/MprisController.cpp
    src/CoverRevisionLinks.cpp
    src/PlaybackClock.cpp
    src/PlaybackFrameScheduler.cpp
    src/WindowPresence.cpp
    src/InputModality.cpp
    src/WaveformReaderLease.cpp
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

# The app icon: a drawing of its own for each size (src/AppIcon.h), installed
# in the hicolor icon theme and built in, for the window icon and the queue's
# first-run image. The QML that shows it is loaded from the source directory
# by the tests, which link this library too.
set(FERROUS_APP_ICON_DIRS 16x16 22x22 32x32 48x48 64x64 128x128 256x256 scalable)
set(FERROUS_APP_ICON_FILES "")
foreach(_icon_dir IN LISTS FERROUS_APP_ICON_DIRS)
    list(APPEND FERROUS_APP_ICON_FILES assets/icons/hicolor/${_icon_dir}/apps/ferrous.svg)
endforeach()
qt_add_library(ferrous_app_icon STATIC src/AppIcon.cpp src/AppIcon.h)
target_include_directories(ferrous_app_icon PUBLIC src)
target_link_libraries(ferrous_app_icon PUBLIC Qt6::Core Qt6::Gui)
ferrous_enable_strict_warnings(ferrous_app_icon)
qt_add_resources(ferrous_app_icon ferrous_app_icon_resources
    PREFIX "/"
    BASE assets
    FILES ${FERROUS_APP_ICON_FILES}
)
target_link_libraries(ferrous PRIVATE ferrous_app_icon)

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
        qml/components/AppMenu.qml
        qml/components/AppMenuBarItem.qml
        qml/components/CompactDialogWindow.qml
        qml/components/DialogButtons.qml
        qml/components/DialogComboBox.qml
        qml/components/DialogFooter.qml
        qml/components/DialogWindow.qml
        qml/components/FormGroupGap.qml
        qml/components/FormHelpText.qml
        qml/components/FullscreenControlsAutoHide.qml
        qml/components/KeyScrollView.qml
        qml/components/KeyHintMenuItem.qml
        qml/components/LaneScrollBar.qml
        qml/components/MenuHeading.qml
        qml/components/NowFadeOut.qml
        qml/components/NowPlayingPanel.qml
        qml/components/PathLabel.qml
        qml/components/PlotLaneHead.qml
        qml/components/PlotModeSwitch.qml
        qml/components/PlotPlate.qml
        qml/components/PlotToolButton.qml
        qml/components/PlotZoomPlate.qml
        qml/components/PopupClosing.qml
        qml/components/RegionHeader.qml
        qml/components/RowBackground.qml
        qml/components/ScrobbleIndicator.qml
        qml/components/UiPalette.qml
        qml/components/ViewerControls.qml
        qml/components/ViewerCornerControls.qml
        qml/components/ViewerStrip.qml
        qml/components/ViewerToolButton.qml
        qml/components/ZoomReadout.qml
        qml/controllers/ErrorController.qml
        qml/controllers/GlobalSearchController.qml
        qml/controllers/LibraryController.qml
        qml/controllers/PlaybackController.qml
        qml/controllers/QueueController.qml
        qml/controllers/ViewerController.qml
        qml/controllers/WindowLayoutController.qml
        qml/dialogs/AboutDialog.qml
        qml/dialogs/AboutKdeDialog.qml
        qml/dialogs/ConfirmDialog.qml
        qml/dialogs/DiagnosticsDialog.qml
        qml/dialogs/GlobalSearchDialog.qml
        qml/dialogs/ItunesArtworkDialog.qml
        qml/dialogs/LibraryRootNameDialog.qml
        qml/dialogs/PreferencesDialog.qml
        qml/dialogs/QuitSaveFailedDialog.qml
        qml/dialogs/RecentErrorsDialog.qml
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
        qml/preferences/GeneralPage.qml
        qml/preferences/LastFmPage.qml
        qml/preferences/LibraryPage.qml
        qml/preferences/PreferencesPage.qml
        qml/preferences/VisualizationPage.qml
        qml/viewers/AlbumArtSurface.qml
        qml/viewers/AlbumArtViewerShell.qml
        qml/viewers/PlotLayer.qml
        qml/viewers/SpectrogramSurface.qml
        qml/viewers/SpectrogramViewerShell.qml
        qml/viewers/WaveformSurface.qml
    RESOURCES
        qml/assets/lastfm.svg
        qml/assets/lastfm-paused.svg
        qml/assets/status-offline.svg
        qml/assets/status-warning.svg
        qml/assets/transport-next.svg
        qml/assets/transport-pause.svg
        qml/assets/transport-play.svg
        qml/assets/transport-previous.svg
        qml/assets/transport-stop.svg
        qml/assets/volume.svg
        qml/assets/volume-muted.svg
        qml/logic/ChannelLabels.js
        qml/logic/ColorUtils.js
        qml/logic/ErrorUtils.js
        qml/logic/FormatUtils.js
        qml/logic/KeyUtils.js
        qml/logic/ListRows.js
        qml/logic/PathUtils.js
        qml/logic/PlotLanes.js
        qml/logic/WheelUtils.js
)

ferrous_enable_strict_warnings(ferrous_ui_qml)
# Compile the QML to bytecode only. Ahead-of-time compiled bindings are C++
# that calls Qt's private API, whose symbols carry the exact Qt minor version
# (Qt_6.11_PRIVATE_API), so the package would need a rebuild for every Qt
# minor update. Bytecode needs no such symbols, and when a newer Qt no longer
# reads it, the engine compiles the QML from the source kept beside it.
# ferrous_no_private_qt_api checks the binaries.
set_target_properties(ferrous_ui_qml PROPERTIES QT_QMLCACHEGEN_ARGUMENTS --only-bytecode)

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
    KF6::I18n
    KF6::I18nQml
    dl
    m
    pthread
)

if(FERROUS_ENABLE_PROFILE_LOGS)
    target_sources(ferrous PRIVATE src/SpectrogramSeekTrace.cpp)
    target_compile_definitions(ferrous PRIVATE FERROUS_ENABLE_PROFILE_LOGS=1)
endif()

# Writes the translation template to <build>/po/ferrous.pot; see
# docs/DEVELOPMENT.md. scripts/extract-messages.sh writes po/ferrous.pot.
add_custom_target(extract-messages
    COMMAND "${FERROUS_ROOT_DIR}/scripts/extract-messages.sh" "${CMAKE_CURRENT_BINARY_DIR}/po"
    WORKING_DIRECTORY "${FERROUS_ROOT_DIR}"
    COMMENT "Extracting translatable strings into po/ferrous.pot"
    VERBATIM
)

install(TARGETS ferrous RUNTIME DESTINATION bin)
install(FILES ferrous.desktop DESTINATION share/applications COMPONENT desktop_integration)
foreach(_icon_dir IN LISTS FERROUS_APP_ICON_DIRS)
    install(FILES assets/icons/hicolor/${_icon_dir}/apps/ferrous.svg
        DESTINATION share/icons/hicolor/${_icon_dir}/apps
        COMPONENT desktop_integration
    )
endforeach()
install(FILES io.github.tiaxi.ferrous.metainfo.xml DESTINATION share/metainfo COMPONENT desktop_integration)

if(BUILD_TESTING)
    find_package(Qt6 6.6 REQUIRED COMPONENTS Test QuickTest)
    find_program(FERROUS_BASH bash REQUIRED)

    # Each test program runs with a temporary directory and XDG config, data
    # and cache directories of its own, so that tests can run in parallel
    # (ctest -j); tests/own_directories.sh. CMake puts the launcher before the
    # command of every test that runs a program built here; older CMake
    # ignores CMAKE_TEST_LAUNCHER, and the tests would share directories.
    if(CMAKE_VERSION VERSION_LESS 3.29)
        message(FATAL_ERROR "The tests need CMake 3.29 or newer; configure with -DBUILD_TESTING=OFF to build without them.")
    endif()
    set(CMAKE_TEST_LAUNCHER "${FERROUS_BASH};${CMAKE_CURRENT_SOURCE_DIR}/tests/own_directories.sh")

    # The line a Qt test prints for a skipped test function, for any test
    # class name.
    function(ferrous_skip_line_regex out_var test_function)
        set(${out_var} "SKIP   : [A-Za-z0-9_]+::${test_function}\\(\\)" PARENT_SCOPE)
    endfunction()

    # Run without a platform chosen, as a developer may run it, a test binary
    # draws offscreen instead of opening windows; tests/TestPlatform.h. With
    # -platform it uses the platform chosen, and the check skips.
    function(ferrous_add_default_platform_test target test_function)
        ferrous_skip_line_regex(skip_line ${test_function})
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
            PASS_REGULAR_EXPRESSION "${skip_line}"
            TIMEOUT 60
        )
    endfunction()

    # Test class names may have digits and underscores.
    ferrous_skip_line_regex(skip_line drawsOffscreenUnlessAPlatformIsChosen)
    add_test(NAME ferrous_skip_line_regex_class_names
        COMMAND "${CMAKE_COMMAND}" -E echo
            "SKIP   : Area2_Test::drawsOffscreenUnlessAPlatformIsChosen()"
    )
    set_tests_properties(ferrous_skip_line_regex_class_names PROPERTIES
        PASS_REGULAR_EXPRESSION "${skip_line}"
        TIMEOUT 60
    )

    add_test(NAME ferrous_metainfo_release_version
        COMMAND "${CMAKE_COMMAND}"
            -DMETAINFO_FILE=${CMAKE_CURRENT_SOURCE_DIR}/io.github.tiaxi.ferrous.metainfo.xml
            -DEXPECTED_VERSION=${FERROUS_APP_VERSION}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_metainfo_release.cmake"
    )
    # The release workflow runs this check on the pushed tag.
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
    foreach(_script_test IN ITEMS
            bump-version collect-release-assets extract-messages install-rpm
            run-ui-cleanup stress-tests)
        add_test(NAME ferrous_script_${_script_test}
            COMMAND "${FERROUS_BASH}" "${FERROUS_ROOT_DIR}/scripts/tests/${_script_test}-test.sh"
                "${CMAKE_CURRENT_BINARY_DIR}/script-tests/${_script_test}"
        )
    endforeach()
    add_test(NAME ferrous_test_launcher_own_directories
        COMMAND "${FERROUS_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/own_directories-test.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/own_directories.sh"
            "${CMAKE_CURRENT_BINARY_DIR}/script-tests/own-directories"
            "${CMAKE_CURRENT_BINARY_DIR}"
            "${CMAKE_CTEST_COMMAND}"
    )
    set_tests_properties(ferrous_test_launcher_own_directories PROPERTIES TIMEOUT 60)
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

    qt_add_executable(ferrous_console_tee_tests
        tests/tst_console_tee.cpp
        src/ConsoleTee.cpp
        src/DiagnosticsLog.cpp
    )
    target_include_directories(ferrous_console_tee_tests PRIVATE src)
    target_link_libraries(ferrous_console_tee_tests PRIVATE Qt6::Core Qt6::Test)
    ferrous_enable_strict_warnings(ferrous_console_tee_tests)
    add_test(NAME ferrous_console_tee_tests COMMAND ferrous_console_tee_tests)
    set_tests_properties(ferrous_console_tee_tests PROPERTIES TIMEOUT 60)

    qt_add_executable(ferrous_diagnostics_log_tests
        tests/tst_diagnostics_log.cpp
        src/DiagnosticsLog.cpp
    )
    target_include_directories(ferrous_diagnostics_log_tests PRIVATE src)
    target_link_libraries(ferrous_diagnostics_log_tests PRIVATE Qt6::Core Qt6::Test)
    ferrous_enable_strict_warnings(ferrous_diagnostics_log_tests)
    add_test(NAME ferrous_diagnostics_log_tests COMMAND ferrous_diagnostics_log_tests)
    set_tests_properties(ferrous_diagnostics_log_tests PROPERTIES TIMEOUT 60)

    qt_add_executable(ferrous_heap_allocator_tests
        tests/tst_heap_allocator.cpp
        src/HeapAllocator.cpp
        src/HeapTrimmer.cpp
    )
    target_include_directories(ferrous_heap_allocator_tests PRIVATE src)
    target_link_libraries(ferrous_heap_allocator_tests PRIVATE Qt6::Core Qt6::Test)
    ferrous_enable_strict_warnings(ferrous_heap_allocator_tests)
    add_test(NAME ferrous_heap_allocator_tests COMMAND ferrous_heap_allocator_tests)
    set_tests_properties(ferrous_heap_allocator_tests PROPERTIES TIMEOUT 60)

    qt_add_executable(ferrous_level_meter_tests
        tests/tst_level_meter.cpp
        src/LevelMeterItem.cpp
        src/PlaybackClock.cpp
        src/WindowPresence.cpp
        src/WaveformReaderLease.cpp
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

    # The app icon: each size built in, and installed with the desktop file
    # into a prefix of the test's own.
    qt_add_executable(ferrous_app_icon_tests tests/tst_app_icon.cpp)
    target_link_libraries(ferrous_app_icon_tests PRIVATE ferrous_app_icon Qt6::Core Qt6::Gui Qt6::Test)
    ferrous_enable_strict_warnings(ferrous_app_icon_tests)
    set(FERROUS_APP_ICON_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/app-icon-install")
    add_test(NAME ferrous_app_icon_install
        COMMAND "${CMAKE_COMMAND}"
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DPREFIX=${FERROUS_APP_ICON_INSTALL_PREFIX}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_desktop_integration.cmake"
    )
    set_tests_properties(ferrous_app_icon_install PROPERTIES
        FIXTURES_SETUP ferrous_app_icon_installed
        TIMEOUT 60
    )
    add_test(NAME ferrous_app_icon_tests COMMAND ferrous_app_icon_tests)
    set_tests_properties(ferrous_app_icon_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;FERROUS_APP_ICON_INSTALL_PREFIX=${FERROUS_APP_ICON_INSTALL_PREFIX}"
        FIXTURES_REQUIRED ferrous_app_icon_installed
        FAIL_REGULAR_EXPRESSION "SKIP   :"
        TIMEOUT 60
    )

    # UiPalette's tokens and the shared components of #373.
    qt_add_executable(ferrous_ui_foundations_tests
        tests/tst_ui_foundations.cpp
    )
    set_target_properties(ferrous_ui_foundations_tests PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
    target_compile_definitions(ferrous_ui_foundations_tests PRIVATE
        FERROUS_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(ferrous_ui_foundations_tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Widgets Qt6::Test
    )
    ferrous_enable_strict_warnings(ferrous_ui_foundations_tests)
    add_test(NAME ferrous_ui_foundations_tests COMMAND ferrous_ui_foundations_tests)
    set_tests_properties(ferrous_ui_foundations_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 90
    )
    ferrous_add_default_platform_test(ferrous_ui_foundations_tests drawsOffscreenUnlessAPlatformIsChosen)

    # The transport and the status bar of #373, from the QML sources.
    qt_add_executable(ferrous_transport_status_tests
        tests/tst_transport_status.cpp
        src/Localization.cpp
        src/WaveformItem.cpp
    )
    set_target_properties(ferrous_transport_status_tests PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
    target_include_directories(ferrous_transport_status_tests PRIVATE src)
    target_compile_definitions(ferrous_transport_status_tests PRIVATE
        FERROUS_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(ferrous_transport_status_tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Widgets Qt6::Test
        KF6::KirigamiPlatform KF6::I18n KF6::I18nQml
    )
    ferrous_enable_strict_warnings(ferrous_transport_status_tests)
    add_test(NAME ferrous_transport_status_tests COMMAND ferrous_transport_status_tests)
    set_tests_properties(ferrous_transport_status_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 120
    )
    ferrous_add_default_platform_test(ferrous_transport_status_tests drawsOffscreenUnlessAPlatformIsChosen)

    # The search palette of #373, from the QML sources and the results model.
    qt_add_executable(ferrous_search_palette_tests
        tests/tst_search_palette.cpp
        src/GlobalSearchResultsModel.cpp
        src/Localization.cpp
    )
    set_target_properties(ferrous_search_palette_tests PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
    target_include_directories(ferrous_search_palette_tests PRIVATE src)
    target_compile_definitions(ferrous_search_palette_tests PRIVATE
        FERROUS_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(ferrous_search_palette_tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Widgets Qt6::Test
        KF6::I18n KF6::I18nQml
    )
    ferrous_enable_strict_warnings(ferrous_search_palette_tests)
    add_test(NAME ferrous_search_palette_tests COMMAND ferrous_search_palette_tests)
    set_tests_properties(ferrous_search_palette_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 120
    )
    ferrous_add_default_platform_test(ferrous_search_palette_tests drawsOffscreenUnlessAPlatformIsChosen)

    # One program, its test classes in files of their own, so that they
    # compile in parallel and a change to one recompiles only its file.
    qt_add_executable(ferrous_qml_smoke_tests
        tests/tst_qml_smoke.cpp
        tests/QmlSmokeTest.h
        tests/QmlSmokeTest.cpp
        tests/tst_qml_smoke_spectrogram.cpp
        tests/tst_qml_smoke_spectrogram_playback.cpp
        tests/tst_qml_smoke_spectrogram_canvas.cpp
        tests/tst_qml_smoke_waveform.cpp
        tests/tst_qml_smoke_waveform_scene_graph.cpp
        tests/tst_qml_smoke_waveform_coverage.cpp
        tests/tst_qml_smoke_library_queue.cpp
        tests/tst_qml_smoke_transport_input.cpp
        tests/tst_qml_smoke_album_art.cpp
        tests/tst_qml_smoke_window.cpp
        tests/tst_qml_smoke_preferences.cpp
        tests/tst_qml_smoke_errors.cpp
        tests/tst_qml_smoke_tag_editor.cpp
        tests/tst_qml_smoke_scrobble.cpp
        src/QmlTypes.cpp
        src/BinaryBridgeCodec.cpp
        src/BridgeClient.cpp
        src/QueueRows.cpp
        src/CoverImageProvider.cpp
        src/DiagnosticsLog.cpp
        src/Localization.cpp
        src/DisplaySleepInhibitor.cpp
        src/GlobalSearchResultsModel.cpp
        src/LibraryTreeModel.cpp
        src/MprisController.cpp
        src/CoverRevisionLinks.cpp
        src/PlaybackClock.cpp
        src/PlaybackFrameScheduler.cpp
        src/WindowPresence.cpp
        src/InputModality.cpp
        src/WaveformReaderLease.cpp
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
        ferrous_app_icon
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
        KF6::I18n
        KF6::I18nQml
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
    # An area has one or more test classes; see kSmokeTestAreas in
    # tst_qml_smoke.cpp. Each timeout is many times the area's run time in
    # CI, for slower machines.
    set(FERROUS_QML_SMOKE_AREAS
        spectrogram 180
        waveform 120
        waveform_coverage 120
        library_queue 120
        transport_input 180
        window 120
        preferences 60
        errors 120
        tag_editor 180
        scrobble 60
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
    # The Last.fm slot's ring moves by device pixels: its area again at 2x.
    add_test(NAME ferrous_qml_smoke_scrobble_2x COMMAND ferrous_qml_smoke_tests -area scrobble)
    set_tests_properties(ferrous_qml_smoke_scrobble_2x PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2"
        TIMEOUT 60
    )
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
        src/QueueRows.cpp
        src/CoverImageProvider.cpp
        src/DiagnosticsLog.cpp
        src/Localization.cpp
        src/DisplaySleepInhibitor.cpp
        src/GlobalSearchResultsModel.cpp
        src/LibraryTreeModel.cpp
        src/MprisController.cpp
        src/CoverRevisionLinks.cpp
        src/PlaybackClock.cpp
        src/PlaybackFrameScheduler.cpp
        src/WindowPresence.cpp
        src/InputModality.cpp
        src/WaveformReaderLease.cpp
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
        ferrous_app_icon
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
        KF6::I18n
        KF6::I18nQml
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

    # The main window as one sheet (#373), in a session of its own: no
    # desktop's platform theme, colours or fonts.
    qt_add_executable(ferrous_ui_main_window_tests
        tests/tst_ui_main_window.cpp
        src/BinaryBridgeCodec.cpp
        src/BridgeClient.cpp
        src/QueueRows.cpp
        src/CoverImageProvider.cpp
        src/DiagnosticsLog.cpp
        src/Localization.cpp
        src/DisplaySleepInhibitor.cpp
        src/GlobalSearchResultsModel.cpp
        src/LibraryTreeModel.cpp
        src/MprisController.cpp
        src/CoverRevisionLinks.cpp
        src/PlaybackClock.cpp
        src/PlaybackFrameScheduler.cpp
        src/WindowPresence.cpp
        src/InputModality.cpp
        src/WaveformReaderLease.cpp
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
    target_include_directories(ferrous_ui_main_window_tests PRIVATE src)
    add_dependencies(ferrous_ui_main_window_tests ferrous_rust_bridge)
    target_compile_definitions(ferrous_ui_main_window_tests PRIVATE
        FERROUS_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(ferrous_ui_main_window_tests PRIVATE
        ferrous_app_icon
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
        KF6::I18n
        KF6::I18nQml
        "${FERROUS_RUST_STATIC_LIB}"
        PkgConfig::FERROUS_GSTREAMER
        dl
        m
        pthread
    )
    ferrous_enable_strict_warnings(ferrous_ui_main_window_tests)
    if(FERROUS_ENABLE_PROFILE_LOGS)
        target_sources(ferrous_ui_main_window_tests PRIVATE src/SpectrogramSeekTrace.cpp)
        target_compile_definitions(ferrous_ui_main_window_tests PRIVATE
            FERROUS_ENABLE_PROFILE_LOGS=1
        )
    endif()
    set_target_properties(ferrous_ui_main_window_tests PROPERTIES
        QT_QML_MODULE_NO_IMPORT_SCAN TRUE
    )
    add_test(NAME ferrous_ui_main_window_tests COMMAND ferrous_ui_main_window_tests)
    set_tests_properties(ferrous_ui_main_window_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 180
    )
    ferrous_add_default_platform_test(ferrous_ui_main_window_tests drawsOffscreenUnlessAPlatformIsChosen)

    # The dialogs of #373, from the QML sources.
    qt_add_executable(ferrous_ui_dialogs_tests
        tests/tst_ui_dialogs.cpp
        src/AboutComponents.cpp
        src/BinaryBridgeCodec.cpp
        src/BridgeClient.cpp
        src/QueueRows.cpp
        src/CoverImageProvider.cpp
        src/DiagnosticsLog.cpp
        src/Localization.cpp
        src/DisplaySleepInhibitor.cpp
        src/GlobalSearchResultsModel.cpp
        src/LibraryTreeModel.cpp
        src/MprisController.cpp
        src/CoverRevisionLinks.cpp
        src/PlaybackClock.cpp
        src/PlaybackFrameScheduler.cpp
        src/WindowPresence.cpp
        src/InputModality.cpp
        src/WaveformReaderLease.cpp
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
    target_include_directories(ferrous_ui_dialogs_tests PRIVATE src)
    add_dependencies(ferrous_ui_dialogs_tests ferrous_rust_bridge)
    target_compile_definitions(ferrous_ui_dialogs_tests PRIVATE
        FERROUS_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(ferrous_ui_dialogs_tests PRIVATE
        ferrous_app_icon
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
        Qt6::QuickTest
        KF6::Kirigami
        KF6::I18n
        KF6::I18nQml
        "${FERROUS_RUST_STATIC_LIB}"
        PkgConfig::FERROUS_GSTREAMER
        dl
        m
        pthread
    )
    ferrous_enable_strict_warnings(ferrous_ui_dialogs_tests)
    if(FERROUS_ENABLE_PROFILE_LOGS)
        target_sources(ferrous_ui_dialogs_tests PRIVATE src/SpectrogramSeekTrace.cpp)
        target_compile_definitions(ferrous_ui_dialogs_tests PRIVATE
            FERROUS_ENABLE_PROFILE_LOGS=1
        )
    endif()
    set_target_properties(ferrous_ui_dialogs_tests PROPERTIES
        QT_QML_MODULE_NO_IMPORT_SCAN TRUE
    )
    add_test(NAME ferrous_ui_dialogs_tests COMMAND ferrous_ui_dialogs_tests)
    set_tests_properties(ferrous_ui_dialogs_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 180
    )
    ferrous_add_default_platform_test(ferrous_ui_dialogs_tests drawsOffscreenUnlessAPlatformIsChosen)

    # The motion of the main window (#373), in a session of its own: no
    # desktop's platform theme, so Kirigami's own durations.
    qt_add_executable(ferrous_ui_motion_tests
        tests/tst_ui_motion.cpp
        src/BinaryBridgeCodec.cpp
        src/BridgeClient.cpp
        src/QueueRows.cpp
        src/CoverImageProvider.cpp
        src/DiagnosticsLog.cpp
        src/Localization.cpp
        src/DisplaySleepInhibitor.cpp
        src/GlobalSearchResultsModel.cpp
        src/LibraryTreeModel.cpp
        src/MprisController.cpp
        src/CoverRevisionLinks.cpp
        src/PlaybackClock.cpp
        src/PlaybackFrameScheduler.cpp
        src/WindowPresence.cpp
        src/InputModality.cpp
        src/WaveformReaderLease.cpp
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
    target_include_directories(ferrous_ui_motion_tests PRIVATE src)
    add_dependencies(ferrous_ui_motion_tests ferrous_rust_bridge)
    target_compile_definitions(ferrous_ui_motion_tests PRIVATE
        FERROUS_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(ferrous_ui_motion_tests PRIVATE
        ferrous_app_icon
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
        KF6::KirigamiPlatform
        KF6::I18n
        KF6::I18nQml
        "${FERROUS_RUST_STATIC_LIB}"
        PkgConfig::FERROUS_GSTREAMER
        dl
        m
        pthread
    )
    ferrous_enable_strict_warnings(ferrous_ui_motion_tests)
    if(FERROUS_ENABLE_PROFILE_LOGS)
        target_sources(ferrous_ui_motion_tests PRIVATE src/SpectrogramSeekTrace.cpp)
        target_compile_definitions(ferrous_ui_motion_tests PRIVATE
            FERROUS_ENABLE_PROFILE_LOGS=1
        )
    endif()
    set_target_properties(ferrous_ui_motion_tests PROPERTIES
        QT_QML_MODULE_NO_IMPORT_SCAN TRUE
    )
    # Its tests run in shards, each in a process of its own and in parallel
    # with the others: run alone, the binary takes minutes. FERROUS_TEST_SHARD
    # picks a shard's tests; see testShardFromEnvironment().
    set(ferrous_ui_motion_shards 4)
    foreach(shard RANGE 1 ${ferrous_ui_motion_shards})
        set(shard_test ferrous_ui_motion_tests_shard_${shard}_of_${ferrous_ui_motion_shards})
        add_test(NAME ${shard_test} COMMAND ferrous_ui_motion_tests)
        set_tests_properties(${shard_test} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;FERROUS_TEST_SHARD=${shard}/${ferrous_ui_motion_shards}"
            TIMEOUT 180
            # For ctest -j, as the costs at the end of this file: about each
            # shard's seconds when run alone.
            COST 42
        )
    endforeach()
    add_test(NAME ferrous_ui_motion_tests_shards_run_every_test
        COMMAND "${CMAKE_COMMAND}"
            -DBINARY=$<TARGET_FILE:ferrous_ui_motion_tests>
            -DSHARDS=${ferrous_ui_motion_shards}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_test_shards.cmake"
    )
    set_tests_properties(ferrous_ui_motion_tests_shards_run_every_test PROPERTIES TIMEOUT 60)
    ferrous_add_default_platform_test(ferrous_ui_motion_tests drawsOffscreenUnlessAPlatformIsChosen)

    # The menus of #373, in a session of its own: no desktop's platform
    # theme, colours or fonts.
    qt_add_executable(ferrous_ui_menus_tests
        tests/tst_ui_menus.cpp
        src/BinaryBridgeCodec.cpp
        src/BridgeClient.cpp
        src/QueueRows.cpp
        src/CoverImageProvider.cpp
        src/DiagnosticsLog.cpp
        src/Localization.cpp
        src/DisplaySleepInhibitor.cpp
        src/GlobalSearchResultsModel.cpp
        src/LibraryTreeModel.cpp
        src/MprisController.cpp
        src/CoverRevisionLinks.cpp
        src/PlaybackClock.cpp
        src/PlaybackFrameScheduler.cpp
        src/WindowPresence.cpp
        src/InputModality.cpp
        src/WaveformReaderLease.cpp
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
    target_include_directories(ferrous_ui_menus_tests PRIVATE src)
    add_dependencies(ferrous_ui_menus_tests ferrous_rust_bridge)
    target_compile_definitions(ferrous_ui_menus_tests PRIVATE
        FERROUS_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(ferrous_ui_menus_tests PRIVATE
        ferrous_app_icon
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
        KF6::I18n
        KF6::I18nQml
        "${FERROUS_RUST_STATIC_LIB}"
        PkgConfig::FERROUS_GSTREAMER
        dl
        m
        pthread
    )
    ferrous_enable_strict_warnings(ferrous_ui_menus_tests)
    if(FERROUS_ENABLE_PROFILE_LOGS)
        target_sources(ferrous_ui_menus_tests PRIVATE src/SpectrogramSeekTrace.cpp)
        target_compile_definitions(ferrous_ui_menus_tests PRIVATE
            FERROUS_ENABLE_PROFILE_LOGS=1
        )
    endif()
    set_target_properties(ferrous_ui_menus_tests PROPERTIES
        QT_QML_MODULE_NO_IMPORT_SCAN TRUE
    )
    add_test(NAME ferrous_ui_menus_tests COMMAND ferrous_ui_menus_tests)
    set_tests_properties(ferrous_ui_menus_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 180
    )
    ferrous_add_default_platform_test(ferrous_ui_menus_tests drawsOffscreenUnlessAPlatformIsChosen)

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
        KF6::I18n
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
        src/QueueRows.cpp
        src/CoverImageProvider.cpp
        src/DiagnosticsLog.cpp
        src/Localization.cpp
        src/GlobalSearchResultsModel.cpp
        src/LibraryTreeModel.cpp
        src/MprisController.cpp
        src/CoverRevisionLinks.cpp
        src/PlaybackClock.cpp
        src/PlaybackFrameScheduler.cpp
        src/WindowPresence.cpp
        src/WaveformReaderLease.cpp
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
        KF6::I18n
        KF6::I18nQml
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
        src/Localization.cpp
        src/PlaybackClock.cpp
        src/PlaybackFrameScheduler.cpp
        src/WindowPresence.cpp
        src/InputModality.cpp
        src/WaveformReaderLease.cpp
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
        ferrous_app_icon
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
        KF6::I18n
        KF6::I18nQml
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
    # As in a KDE Plasma session, which CI has not; tests/TestSessionIsolation.h.
    add_test(NAME ferrous_qml_module_kde_session
        COMMAND ferrous_qml_module_tests looksTheSameWhateverTheDesktopSession
    )
    set_tests_properties(ferrous_qml_module_kde_session PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CURRENT_DESKTOP=KDE;QT_QPA_PLATFORMTHEME=kde"
        TIMEOUT 60
    )
    ferrous_add_default_platform_test(ferrous_qml_module_tests drawsOffscreenUnlessAPlatformIsChosen)

    # The translation template, and the x-test pseudo-translation made from it,
    # which wraps every message in ⟦ and ⟧ for the i18n tests.
    find_program(FERROUS_I18N_BASH bash REQUIRED)
    find_program(FERROUS_XGETTEXT xgettext REQUIRED)
    find_program(FERROUS_MSGEN msgen REQUIRED)
    find_program(FERROUS_MSGFILTER msgfilter REQUIRED)
    find_program(FERROUS_MSGFMT msgfmt REQUIRED)
    find_program(FERROUS_SED sed REQUIRED)
    add_test(NAME ferrous_translation_template
        COMMAND "${CMAKE_COMMAND}"
            -DBASH=${FERROUS_I18N_BASH}
            -DEXTRACT_SCRIPT=${FERROUS_ROOT_DIR}/scripts/extract-messages.sh
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/translation-template-check
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_translation_template.cmake"
    )
    set_tests_properties(ferrous_translation_template PROPERTIES TIMEOUT 60)
    file(GLOB_RECURSE FERROUS_TRANSLATED_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/*.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/*.js"
    )
    set(FERROUS_PSEUDO_LOCALE_DIR "${CMAKE_CURRENT_BINARY_DIR}/pseudo-locale")
    set(FERROUS_PSEUDO_CATALOG "${FERROUS_PSEUDO_LOCALE_DIR}/x-test/LC_MESSAGES/ferrous.mo")
    add_custom_command(
        OUTPUT "${FERROUS_PSEUDO_CATALOG}"
        COMMAND "${CMAKE_COMMAND}"
            -DBASH=${FERROUS_I18N_BASH}
            -DEXTRACT_SCRIPT=${FERROUS_ROOT_DIR}/scripts/extract-messages.sh
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/pseudo-locale-work
            -DOUT_DIR=${FERROUS_PSEUDO_LOCALE_DIR}
            -DMSGEN=${FERROUS_MSGEN}
            -DMSGFILTER=${FERROUS_MSGFILTER}
            -DMSGFMT=${FERROUS_MSGFMT}
            -DSED=${FERROUS_SED}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_pseudo_catalog.cmake"
        DEPENDS
            ${FERROUS_TRANSLATED_SOURCES}
            "${CMAKE_CURRENT_SOURCE_DIR}/Messages.sh"
            "${FERROUS_ROOT_DIR}/scripts/extract-messages.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_pseudo_catalog.cmake"
        COMMENT "Making the x-test pseudo-translation"
        VERBATIM
    )
    add_custom_target(ferrous_pseudo_catalog DEPENDS "${FERROUS_PSEUDO_CATALOG}")

    qt_add_executable(ferrous_i18n_tests
        tests/tst_i18n.cpp
        src/QmlTypes.cpp
        src/BinaryBridgeCodec.cpp
        src/BridgeClient.cpp
        src/QueueRows.cpp
        src/CoverImageProvider.cpp
        src/DiagnosticsLog.cpp
        src/Localization.cpp
        src/DisplaySleepInhibitor.cpp
        src/GlobalSearchResultsModel.cpp
        src/LibraryTreeModel.cpp
        src/MprisController.cpp
        src/CoverRevisionLinks.cpp
        src/PlaybackClock.cpp
        src/PlaybackFrameScheduler.cpp
        src/WindowPresence.cpp
        src/InputModality.cpp
        src/WaveformReaderLease.cpp
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
    target_include_directories(ferrous_i18n_tests PRIVATE src)
    add_dependencies(ferrous_i18n_tests ferrous_rust_bridge ferrous_pseudo_catalog)
    target_compile_definitions(ferrous_i18n_tests PRIVATE
        FERROUS_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        FERROUS_PSEUDO_LOCALE_DIR="${FERROUS_PSEUDO_LOCALE_DIR}"
    )
    target_link_libraries(ferrous_i18n_tests PRIVATE
        ferrous_app_icon
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
        KF6::I18n
        KF6::I18nQml
        "${FERROUS_RUST_STATIC_LIB}"
        PkgConfig::FERROUS_GSTREAMER
        dl
        m
        pthread
    )
    ferrous_enable_strict_warnings(ferrous_i18n_tests)
    if(FERROUS_ENABLE_PROFILE_LOGS)
        target_sources(ferrous_i18n_tests PRIVATE src/SpectrogramSeekTrace.cpp)
        target_compile_definitions(ferrous_i18n_tests PRIVATE FERROUS_ENABLE_PROFILE_LOGS=1)
    endif()
    set_target_properties(ferrous_i18n_tests PROPERTIES
        QT_QML_MODULE_NO_IMPORT_SCAN TRUE
    )
    add_test(NAME ferrous_i18n_tests COMMAND ferrous_i18n_tests)
    set_tests_properties(ferrous_i18n_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 120
    )
    ferrous_add_default_platform_test(ferrous_i18n_tests drawsOffscreenUnlessAPlatformIsChosen)

    # The app and the module test have the module's compiled QML.
    add_test(NAME ferrous_qml_cache_symbol_patterns
        COMMAND "${CMAKE_COMMAND}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_qml_cache_symbol_patterns.cmake"
    )
    set_tests_properties(ferrous_qml_cache_symbol_patterns PROPERTIES TIMEOUT 60)
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

        # The check rejects a binary that links the archive without
        # WHOLE_ARCHIVE, and one that nm cannot read.
        add_executable(ferrous_qml_cache_plain_link tests/qml_cache_plain_link.cpp)
        target_link_libraries(ferrous_qml_cache_plain_link PRIVATE ferrous_ui_qml)
        ferrous_enable_strict_warnings(ferrous_qml_cache_plain_link)
        add_test(NAME ferrous_qml_cache_check_rejects_plain_link
            COMMAND "${CMAKE_COMMAND}"
                -DNM=${CMAKE_NM}
                -DBINARY=$<TARGET_FILE:ferrous_qml_cache_plain_link>
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_qml_cache_linked.cmake"
        )
        add_test(NAME ferrous_qml_cache_check_rejects_unreadable_binary
            COMMAND "${CMAKE_COMMAND}"
                -DNM=${CMAKE_NM}
                -DBINARY=${CMAKE_CURRENT_SOURCE_DIR}/tests/qml_cache_plain_link.cpp
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_qml_cache_linked.cmake"
        )
        # CMake wraps the error message at spaces, so a line break and
        # indentation may separate the words.
        set_tests_properties(ferrous_qml_cache_check_rejects_plain_link PROPERTIES
            PASS_REGULAR_EXPRESSION "lacks[ \t\r\n]+QmlCacheGeneratedCode::_qt_qml_FerrousUi_qml_Main_qml::"
            TIMEOUT 60
        )
        set_tests_properties(ferrous_qml_cache_check_rejects_unreadable_binary PROPERTIES
            PASS_REGULAR_EXPRESSION "could[ \t\r\n]+not[ \t\r\n]+read"
            TIMEOUT 60
        )
    endif()

    # No binary refers to Qt's private API, whose symbols would tie it, and
    # the package through RPM's automatic dependencies, to the exact Qt minor
    # version it was built with. This covers every executable defined above.
    add_test(NAME ferrous_private_qt_api_patterns
        COMMAND "${CMAKE_COMMAND}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_private_qt_api_patterns.cmake"
    )
    set_tests_properties(ferrous_private_qt_api_patterns PROPERTIES TIMEOUT 60)
    # Without objdump no binary would be checked, so the tests require it.
    if(CMAKE_OBJDUMP)
        set(ferrous_objdump "${CMAKE_OBJDUMP}")
    else()
        find_program(FERROUS_OBJDUMP objdump REQUIRED)
        set(ferrous_objdump "${FERROUS_OBJDUMP}")
    endif()
    get_property(ferrous_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS ferrous_targets)
        get_target_property(target_type ${target} TYPE)
        if(NOT target_type STREQUAL "EXECUTABLE")
            continue()
        endif()
        add_test(NAME ${target}_no_private_qt_api
            COMMAND "${CMAKE_COMMAND}"
                -DOBJDUMP=${ferrous_objdump}
                -DBINARY=$<TARGET_FILE:${target}>
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_no_private_qt_api.cmake"
        )
        set_tests_properties(${target}_no_private_qt_api PROPERTIES TIMEOUT 60)
    endforeach()
    add_test(NAME ferrous_private_qt_api_check_rejects_unreadable_binary
        COMMAND "${CMAKE_COMMAND}"
            -DOBJDUMP=${ferrous_objdump}
            -DBINARY=${CMAKE_CURRENT_SOURCE_DIR}/tests/QtPrivateApiReferences.cmake
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_no_private_qt_api.cmake"
    )
    set_tests_properties(ferrous_private_qt_api_check_rejects_unreadable_binary PROPERTIES
        PASS_REGULAR_EXPRESSION "could[ \t\r\n]+not[ \t\r\n]+read"
        TIMEOUT 60
    )

    # ctest -j starts the tests of most COST first, so that a run does not
    # end waiting on a long test that started last. CTest takes the times of
    # the build directory's last run instead where it has them; these, about
    # each test's seconds when run alone, are for a first run, as in CI.
    # The shards of ferrous_ui_motion_tests have theirs where they are added.
    foreach(test_and_cost IN ITEMS
            ferrous_qml_smoke_transport_input:80
            ferrous_qml_smoke_tag_editor:50
            ferrous_ui_dialogs_tests:40
            ferrous_qml_smoke_waveform:32
            ferrous_qml_smoke_errors:32
            ferrous_qml_smoke_spectrogram:32
            ferrous_accessibility_tests:20
            ferrous_bridge_client_tests:14
            ferrous_ui_menus_tests:13
            ferrous_qml_smoke_library_queue:13
            ferrous_qml_smoke_window:11)
        string(REPLACE ":" ";" test_and_cost "${test_and_cost}")
        list(GET test_and_cost 0 test)
        list(GET test_and_cost 1 cost)
        set_tests_properties(${test} PROPERTIES COST ${cost})
    endforeach()
endif()
