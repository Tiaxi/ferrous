# SPDX-License-Identifier: GPL-3.0-or-later
# Checks that ui/cmake/FerrousCargoVersion.cmake, which configuring the UI
# uses, and scripts/cargo-package-version.sh read the same [package] version
# from fixture Cargo.toml files in WORK_DIR, and fail where there is none.
#
# Run with -DBASH=<path> -DSCRIPT=<cargo-package-version.sh> -DWORK_DIR=<dir>.
# With -DCARGO_TOML=<path>, it only prints that file's version, as configuring
# would read it.

if(DEFINED CARGO_TOML)
    include("${CMAKE_CURRENT_LIST_DIR}/../cmake/FerrousCargoVersion.cmake")
    ferrous_cargo_package_version("${CARGO_TOML}" version)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${version}")
    return()
endif()

if(NOT BASH OR NOT SCRIPT OR NOT WORK_DIR)
    message(FATAL_ERROR "Pass -DBASH=<path>, -DSCRIPT=<path> and -DWORK_DIR=<path>.")
endif()

set(failures "")

# fixture(<name> <expected version, or FAIL> <Cargo.toml content...>)
function(fixture name expected)
    string(CONCAT content ${ARGN})
    set(path "${WORK_DIR}/${name}/Cargo.toml")
    file(WRITE "${path}" "${content}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DCARGO_TOML=${path} -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        RESULT_VARIABLE cmake_result
        OUTPUT_VARIABLE cmake_version
        ERROR_VARIABLE cmake_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND "${BASH}" "${SCRIPT}" "${path}"
        RESULT_VARIABLE shell_result
        OUTPUT_VARIABLE shell_version
        ERROR_VARIABLE shell_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    foreach(reader IN ITEMS cmake shell)
        if(expected STREQUAL "FAIL")
            if(${reader}_result EQUAL 0)
                list(APPEND failures "${name}: ${reader} read '${${reader}_version}' but should fail")
            endif()
        elseif(NOT ${reader}_result EQUAL 0)
            list(APPEND failures "${name}: ${reader} failed: ${${reader}_error}")
        elseif(NOT ${reader}_version STREQUAL expected)
            list(APPEND failures "${name}: ${reader} read '${${reader}_version}', not '${expected}'")
        endif()
    endforeach()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")

fixture(double_quotes 1.2.3 "[package]\nname = \"ferrous\"\nversion = \"1.2.3\"\n")
fixture(single_quotes 1.2.3 "[package]\nname = 'ferrous'\nversion = '1.2.3'\n")
fixture(comment_with_quoted_version 0.2.2
    "[package]\nversion = '0.2.2' # next \"0.3.0\"\n")
fixture(double_quoted_comment 0.2.2 "[package]\nversion = \"0.2.2\" # was \"0.2.1\"\n")
fixture(spaced_header 1.2.3 "[ package ]\nversion = \"1.2.3\"\n")
fixture(tabbed_header_with_comment 1.2.3 "[\tpackage\t] # the app\nversion = \"1.2.3\"\n")
fixture(quoted_header 1.2.3 "[ \"package\" ]\nversion = \"1.2.3\"\n")
fixture(indented_and_tabbed_key 1.2.3 "[package]\n  version\t=\t\"1.2.3\"\n")
fixture(after_dependency_tables 1.2.3
    "[dependencies.extra]\nversion = \"9.9.9\"\n\n[workspace.package]\nversion = \"8.8.8\"\n\n"
    "[package]\nname = \"ferrous\"\nkeywords = [\"music\", \"player\"]\nversion = \"1.2.3\"\n\n"
    "[dependencies.other]\nversion = \"7.7.7\"\n")
fixture(crlf 1.2.3 "[dependencies.extra]\r\nversion = \"9.9.9\"\r\n[package]\r\nversion = \"1.2.3\"\r\n")
fixture(only_metadata_table FAIL "[package]\nname = \"x\"\n\n[package.metadata.x]\nversion = \"7\"\n")
fixture(array_table FAIL "[[package]]\nversion = \"1.2.3\"\n")
fixture(inherited_from_workspace FAIL "[package]\nversion.workspace = true\n")
fixture(unquoted FAIL "[package]\nversion = 1\n")
fixture(no_package FAIL "[dependencies.extra]\nversion = \"9.9.9\"\n")

if(failures)
    list(JOIN failures "\n" message)
    message(FATAL_ERROR "${message}")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
