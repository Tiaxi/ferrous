cmake_minimum_required(VERSION 3.24)
project(FerrousNativeUi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network)
find_package(KF6Kirigami 6.0 REQUIRED)

qt_standard_project_setup(REQUIRES 6.6)

qt_add_executable(ferrous_kirigami_shell
    src/main.cpp
    src/BridgeClient.cpp
    src/SpectrogramItem.cpp
    src/WaveformItem.cpp
)

target_include_directories(ferrous_kirigami_shell PRIVATE src)

qt_add_qml_module(ferrous_kirigami_shell
    URI FerrousNative
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

target_link_libraries(ferrous_kirigami_shell PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    KF6::Kirigami
)
