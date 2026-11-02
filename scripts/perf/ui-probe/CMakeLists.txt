# SPDX-License-Identifier: GPL-3.0-or-later
# Measurement-only LD_PRELOAD probe for the Ferrous desktop app; see README.md.
cmake_minimum_required(VERSION 3.24)
project(FerrousUiProbe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC OFF)
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui GuiPrivate Qml Quick QuickPrivate)
add_library(ferrous_ui_probe SHARED probe.cpp)
target_link_libraries(ferrous_ui_probe PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Quick Qt6::QuickPrivate)
# The interposed QRhi symbols carry Qt's private-API version, as the
# references to them in libQt6Quick do.
set(QT_VERSION_MAJOR ${Qt6_VERSION_MAJOR})
set(QT_VERSION_MINOR ${Qt6_VERSION_MINOR})
configure_file(probe.map.in ${CMAKE_CURRENT_BINARY_DIR}/probe.map @ONLY)
target_link_options(ferrous_ui_probe PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/probe.map")
