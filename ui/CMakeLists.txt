cmake_minimum_required(VERSION 3.24)
project(FerrousNativeUi LANGUAGES CXX)

include(CTest)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network Widgets Concurrent)
find_package(KF6Kirigami 6.0 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FERROUS_GSTREAMER REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-audio-1.0
    gio-2.0
)

qt_standard_project_setup(REQUIRES 6.6)
qt_policy(SET QTP0004 NEW)

set(FERROUS_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(FERROUS_RUST_STATIC_LIB "${FERROUS_ROOT_DIR}/target/release/libferrous.a")

add_custom_target(ferrous_rust_bridge ALL
    COMMAND /bin/zsh -lc "if [ -f \"$HOME/.cargo/env\" ]; then source \"$HOME/.cargo/env\"; fi; cargo build --release --features gst --lib"
    WORKING_DIRECTORY "${FERROUS_ROOT_DIR}"
    BYPRODUCTS "${FERROUS_RUST_STATIC_LIB}"
    COMMENT "Building Rust static library for in-process bridge"
    VERBATIM
)

qt_add_executable(ferrous
    src/main.cpp
    src/BinaryBridgeCodec.cpp
    src/BridgeClient.cpp
    src/LibraryTreeModel.cpp
    src/SpectrogramItem.cpp
    src/WaveformItem.cpp
)

target_include_directories(ferrous PRIVATE src)
add_dependencies(ferrous ferrous_rust_bridge)

qt_add_qml_module(ferrous
    URI FerrousNative
    VERSION 1.0
    NO_IMPORT_SCAN
    QML_FILES
        qml/Main.qml
)

target_link_libraries(ferrous PRIVATE
    "${FERROUS_RUST_STATIC_LIB}"
    PkgConfig::FERROUS_GSTREAMER
    Qt6::Core
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

if(BUILD_TESTING)
    find_package(Qt6 6.6 REQUIRED COMPONENTS Test)

    qt_add_executable(ferrous_qml_smoke_tests
        tests/tst_qml_smoke.cpp
        src/LibraryTreeModel.cpp
        src/SpectrogramItem.cpp
        src/WaveformItem.cpp
    )
    target_include_directories(ferrous_qml_smoke_tests PRIVATE src)
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
        Qt6::Test
        KF6::Kirigami
    )
    set_target_properties(ferrous_qml_smoke_tests PROPERTIES
        QT_QML_MODULE_NO_IMPORT_SCAN TRUE
    )
    add_test(NAME ferrous_qml_smoke_tests COMMAND ferrous_qml_smoke_tests)
    set_tests_properties(ferrous_qml_smoke_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()
