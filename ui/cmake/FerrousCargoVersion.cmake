# SPDX-License-Identifier: GPL-3.0-or-later
# ferrous_cargo_package_version(<Cargo.toml> <out-var>)
#
# Sets <out-var> to the version in the [package] table of <Cargo.toml>, the
# same value that scripts/cargo-package-version.sh prints. Other tables, such
# as a dependency's, have versions of their own. The value is the string in
# single or double quotes right after `=`, so a comment after it cannot supply
# one. Fails configuration when there is none.
function(ferrous_cargo_package_version cargo_toml out_var)
    file(STRINGS "${cargo_toml}" lines)
    set(table "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "\r$" "" line "${line}")
        if(line MATCHES "^[ \t]*\\[\\[([^]]*)\\]\\]")
            string(STRIP "${CMAKE_MATCH_1}" table)
            set(table "[[${table}]]")
        elseif(line MATCHES "^[ \t]*\\[([^]]*)\\]")
            string(STRIP "${CMAKE_MATCH_1}" table)
            # Separate tests: a failed MATCHES clears CMAKE_MATCH_1.
            if(table MATCHES "^\"([^\"]*)\"$")
                set(table "${CMAKE_MATCH_1}")
            elseif(table MATCHES "^'([^']*)'$")
                set(table "${CMAKE_MATCH_1}")
            endif()
        elseif(table STREQUAL "package" AND line MATCHES "^[ \t]*version[ \t]*=")
            if(line MATCHES "^[ \t]*version[ \t]*=[ \t]*\"([^\"\\\\]*)\"")
                set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
                return()
            elseif(line MATCHES "^[ \t]*version[ \t]*=[ \t]*'([^']*)'")
                set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
                return()
            endif()
            message(FATAL_ERROR "The [package] version in ${cargo_toml} is not a plain quoted string")
        endif()
    endforeach()
    message(FATAL_ERROR "No version in the [package] table of ${cargo_toml}")
endfunction()
