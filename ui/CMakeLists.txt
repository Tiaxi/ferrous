cmake_minimum_required(VERSION 3.24)
project(FerrousUi LANGUAGES CXX)

include(CTest)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(FERROUS_ENABLE_PROFILE_LOGS "Compile profiling log output into binaries" OFF)
option(FERROUS_WARNINGS_AS_ERRORS "Fail UI builds on compiler warnings" ON)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core DBus Gui Qml Quick QuickControls2 Network Widgets Concurrent)
find_package(KF6Kirigami 6.0 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FERROUS_GSTREAMER REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-audio-1.0
    gstreamer-pbutils-1.0
    gio-2.0
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
set(FERROUS_RUST_STATIC_LIB "${FERROUS_ROOT_DIR}/target/release/libferrous.a")
set(FERROUS_RUST_FEATURES "gst")
if(FERROUS_ENABLE_PROFILE_LOGS)
    set(FERROUS_RUST_FEATURES "gst,profiling-logs")
endif()

add_custom_target(ferrous_rust_bridge ALL
    COMMAND /bin/zsh -lc "if [ -f \"$HOME/.cargo/env\" ]; then source \"$HOME/.cargo/env\"; fi; cargo build --release --features ${FERROUS_RUST_FEATURES} --lib"
    WORKING_DIRECTORY "${FERROUS_ROOT_DIR}"
    BYPRODUCTS "${FERROUS_RUST_STATIC_LIB}"
    COMMENT "Building Rust static library for in-process bridge"
    VERBATIM
)

qt_add_executable(ferrous
    src/AppInstanceController.cpp
    src/main.cpp
    src/DiagnosticsLog.cpp
    src/BinaryBridgeCodec.cpp
    src/BridgeClient.cpp
    src/GlobalSearchResultsModel.cpp
    src/LibraryTreeModel.cpp
    src/MprisController.cpp
    src/TagEditorController.cpp
    src/TagEditorTableModel.cpp
    src/SpectrogramItem.cpp
    src/WindowActivation.cpp
    src/WaveformItem.cpp
)

target_include_directories(ferrous PRIVATE src)
add_dependencies(ferrous ferrous_rust_bridge)
ferrous_enable_strict_warnings(ferrous)

qt_add_resources(ferrous ferrous_app_resources
    PREFIX "/icons"
    FILES
        assets/ferrous.svg
)

qt_add_qml_module(ferrous
    URI FerrousUi
    VERSION 1.0
    NO_IMPORT_SCAN
    QML_FILES
        qml/Main.qml
    RESOURCES
        qml/assets/channel-mono.svg
        qml/assets/channel-stereo.svg
        qml/assets/channel-4_0.svg
        qml/assets/channel-5_0.svg
        qml/assets/channel-5_1.svg
        qml/assets/channel-7_1.svg
)

if(NOT MSVC)
    set_source_files_properties(
        "${CMAKE_CURRENT_BINARY_DIR}/.rcc/qmlcache/ferrous_qml/Main_qml.cpp"
        PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter"
    )
endif()

target_link_libraries(ferrous PRIVATE
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

if(UNIX AND NOT APPLE AND NOT MSVC)
    # Keep symbols from static archives (notably vendored libdbus from Rust deps)
    # out of the executable's dynamic symbol table so Qt binds to the shared
    # system libraries it loaded, not our private static copies.
    target_link_options(ferrous PRIVATE "-Wl,--exclude-libs,ALL")
endif()

if(FERROUS_ENABLE_PROFILE_LOGS)
    target_compile_definitions(ferrous PRIVATE FERROUS_ENABLE_PROFILE_LOGS=1)
endif()

install(TARGETS ferrous RUNTIME DESTINATION bin)
install(FILES ferrous.desktop DESTINATION share/applications)
install(FILES assets/ferrous.svg DESTINATION share/icons/hicolor/scalable/apps RENAME ferrous.svg)

if(BUILD_TESTING)
    find_package(Qt6 6.6 REQUIRED COMPONENTS Test)

    qt_add_executable(ferrous_qml_smoke_tests
        tests/tst_qml_smoke.cpp
        src/BinaryBridgeCodec.cpp
        src/BridgeClient.cpp
        src/DiagnosticsLog.cpp
        src/GlobalSearchResultsModel.cpp
        src/LibraryTreeModel.cpp
        src/TagEditorController.cpp
        src/TagEditorTableModel.cpp
        src/SpectrogramItem.cpp
        src/WaveformItem.cpp
    )
    target_include_directories(ferrous_qml_smoke_tests PRIVATE src)
    add_dependencies(ferrous_qml_smoke_tests ferrous_rust_bridge)
    target_compile_definitions(ferrous_qml_smoke_tests PRIVATE
        FERROUS_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(ferrous_qml_smoke_tests PRIVATE
        Qt6::Core
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
        target_compile_definitions(ferrous_qml_smoke_tests PRIVATE FERROUS_ENABLE_PROFILE_LOGS=1)
    endif()
    set_target_properties(ferrous_qml_smoke_tests PROPERTIES
        QT_QML_MODULE_NO_IMPORT_SCAN TRUE
    )
    add_test(NAME ferrous_qml_smoke_tests COMMAND ferrous_qml_smoke_tests)
    set_tests_properties(ferrous_qml_smoke_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

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
    )

    qt_add_executable(ferrous_bridge_client_tests
        tests/tst_bridge_client.cpp
        src/BinaryBridgeCodec.cpp
        src/BridgeClient.cpp
        src/DiagnosticsLog.cpp
        src/GlobalSearchResultsModel.cpp
        src/LibraryTreeModel.cpp
        src/MprisController.cpp
        src/TagEditorController.cpp
        src/TagEditorTableModel.cpp
        src/SpectrogramItem.cpp
        src/WindowActivation.cpp
        src/WaveformItem.cpp
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
        target_compile_definitions(ferrous_bridge_client_tests PRIVATE
            FERROUS_ENABLE_PROFILE_LOGS=1
        )
    endif()
    set_target_properties(ferrous_bridge_client_tests PROPERTIES
        QT_QML_MODULE_NO_IMPORT_SCAN TRUE
    )
    add_test(NAME ferrous_bridge_client_tests COMMAND ferrous_bridge_client_tests)
    set_tests_properties(ferrous_bridge_client_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()
