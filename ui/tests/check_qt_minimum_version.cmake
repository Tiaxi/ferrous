# SPDX-License-Identifier: GPL-3.0-or-later
# Checks that the app's and the perf probe's CMake projects under ROOT_DIR
# ask for the Qt that the build needs, QT_MINIMUM (QTP0004 is new in Qt 6.8),
# so that an older Qt fails at configure time, and that the install guide
# names the same minimum. A fixture that asks for Qt 6.6 must fail the same
# check.

if(NOT QT_MINIMUM OR NOT ROOT_DIR)
    message(FATAL_ERROR "Pass -DQT_MINIMUM=<version> and -DROOT_DIR=<repository root>.")
endif()
set(SOURCE_FILES "${ROOT_DIR}/ui/CMakeLists.txt" "${ROOT_DIR}/scripts/perf/ui-probe/CMakeLists.txt")
set(INSTALL_DOC "${ROOT_DIR}/docs/INSTALL.md")
if(QT_MINIMUM VERSION_LESS 6.8)
    message(FATAL_ERROR "The declared Qt minimum ${QT_MINIMUM} is older than 6.8, which QTP0004 needs.")
endif()

# Sets `out` to the problems in `content`: each find_package(Qt6) and
# qt_standard_project_setup() must ask for QT_MINIMUM, literally or through
# FERROUS_QT_MINIMUM_VERSION.
function(qt_minimum_problems content name out)
    set(problems "")
    string(REGEX MATCHALL "find_package\\([ \t\r\n]*Qt6[^)]*\\)" calls "${content}")
    string(REGEX MATCHALL "qt_standard_project_setup\\([^)]*\\)" setups "${content}")
    if(NOT calls)
        list(APPEND problems "${name}: no find_package(Qt6)")
    endif()
    foreach(call IN LISTS calls)
        if(NOT call MATCHES "^find_package\\([ \t\r\n]*Qt6[ \t\r\n]+([^ \t\r\n)]+)")
            list(APPEND problems "${name}: ${call} names no version")
            continue()
        endif()
        set(version "${CMAKE_MATCH_1}")
        if(NOT version STREQUAL "\${FERROUS_QT_MINIMUM_VERSION}" AND NOT version STREQUAL QT_MINIMUM)
            list(APPEND problems "${name}: ${call} asks for ${version}, not ${QT_MINIMUM}")
        endif()
    endforeach()
    foreach(setup IN LISTS setups)
        if(NOT setup MATCHES "REQUIRES[ \t\r\n]+([^ \t\r\n)]+)")
            list(APPEND problems "${name}: ${setup} names no minimum")
            continue()
        endif()
        set(version "${CMAKE_MATCH_1}")
        if(NOT version STREQUAL "\${FERROUS_QT_MINIMUM_VERSION}" AND NOT version STREQUAL QT_MINIMUM)
            list(APPEND problems "${name}: ${setup} requires ${version}, not ${QT_MINIMUM}")
        endif()
    endforeach()
    if(content MATCHES "set\\(FERROUS_QT_MINIMUM_VERSION[ \t\r\n]+([^ \t\r\n)]+)")
        if(NOT CMAKE_MATCH_1 STREQUAL QT_MINIMUM)
            list(APPEND problems "${name}: FERROUS_QT_MINIMUM_VERSION is ${CMAKE_MATCH_1}, not ${QT_MINIMUM}")
        endif()
    endif()
    set(${out} "${problems}" PARENT_SCOPE)
endfunction()

# The check itself catches the old declaration.
qt_minimum_problems(
    "find_package(Qt6 6.6 REQUIRED COMPONENTS Core)\nqt_standard_project_setup(REQUIRES 6.6)\n"
    "fixture" fixture_problems)
list(LENGTH fixture_problems fixture_count)
if(NOT fixture_count EQUAL 2)
    message(FATAL_ERROR "A fixture asking for Qt 6.6 should have 2 problems, has: ${fixture_problems}")
endif()

set(all_problems "")
foreach(file IN LISTS SOURCE_FILES)
    file(READ "${file}" content)
    qt_minimum_problems("${content}" "${file}" problems)
    list(APPEND all_problems ${problems})
endforeach()

file(READ "${INSTALL_DOC}" doc)
string(REPLACE "." "\\." minimum_pattern "${QT_MINIMUM}")
if(NOT doc MATCHES "Qt ${minimum_pattern} or newer development libraries")
    list(APPEND all_problems "${INSTALL_DOC}: does not ask for Qt ${QT_MINIMUM} or newer")
endif()
if(NOT doc MATCHES "CMake refuses a Qt older than ${minimum_pattern}\\.")
    list(APPEND all_problems "${INSTALL_DOC}: does not say that CMake refuses a Qt older than ${QT_MINIMUM}")
endif()

if(all_problems)
    list(JOIN all_problems "\n" text)
    message(FATAL_ERROR "${text}")
endif()
message(STATUS "Every Qt lookup asks for Qt ${QT_MINIMUM}.")
