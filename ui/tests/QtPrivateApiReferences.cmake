# SPDX-License-Identifier: GPL-3.0-or-later
# Finds the references to Qt's private API in a binary's ELF version
# references, as `objdump -p` prints them; check_no_private_qt_api.cmake.
# Symbols of Qt's private API carry a version named for the exact Qt release
# (Qt_6.11_PRIVATE_API), and RPM turns each such reference into a dependency
# on that Qt minor version.

# Sets OUT_VAR to the private-API references in the objdump output in the
# variable named DUMP_VAR, as "<library> (<version>)" entries, and QT_VAR to
# true if the output has a version reference to any Qt library.
function(ferrous_qt_private_api_references out_var qt_var dump_var)
    set(references "")
    set(has_qt FALSE)
    set(library "")
    string(REPLACE ";" "\;" dump "${${dump_var}}")
    string(REPLACE "\n" ";" lines "${dump}")
    foreach(line IN LISTS lines)
        if(line MATCHES "^[ \t]*required from ([^ \t]+):[ \t]*$")
            set(library "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^[ \t]*0x[0-9a-fA-F]+[ \t]+0x[0-9a-fA-F]+[ \t]+[0-9]+[ \t]+([^ \t]+)[ \t]*$")
            set(version "${CMAKE_MATCH_1}")
            if(version MATCHES "^Qt_")
                set(has_qt TRUE)
            endif()
            if(version MATCHES "^Qt_.*_PRIVATE_API$")
                list(APPEND references "${library} (${version})")
            endif()
        endif()
    endforeach()
    set(${out_var} "${references}" PARENT_SCOPE)
    set(${qt_var} ${has_qt} PARENT_SCOPE)
endfunction()
