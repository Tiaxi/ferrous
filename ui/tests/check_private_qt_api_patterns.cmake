# SPDX-License-Identifier: GPL-3.0-or-later
# Fails unless QtPrivateApiReferences.cmake finds the private-API references
# in `objdump -p` output, names their libraries, and tells whether the output
# refers to Qt at all.
include("${CMAKE_CURRENT_LIST_DIR}/QtPrivateApiReferences.cmake")

set(header "Version References:\n")
set(glibc "  required from libc.so.6:\n    0x09691a75 0x00 02 GLIBC_2.2.5\n")
set(qml_private "  required from libQt6Qml.so.6:\n    0x096ec9f9 0x00 38 Qt_6.11_PRIVATE_API\n    0x00058a26 0x00 12 Qt_6\n")
set(quick_private "  required from libQt6Quick.so.6:\n    0x00058a26 0x00 07 Qt_6\n    0x096ec9f9 0x00 09 Qt_6.6_PRIVATE_API\n")
set(core_public "  required from libQt6Core.so.6:\n    0x00058a26 0x00 03 Qt_6\n    0x00058a00 0x00 04 Qt_6.9\n")
# A version reference without a trailing newline.
set(last_line "  required from libQt6Qml.so.6:\n    0x096ec9f9 0x00 38 Qt_6.11_PRIVATE_API")
# Private API of a library other than Qt is not this check's concern.
set(other_private "  required from libKF6Foo.so.6:\n    0x0000aaaa 0x00 05 KF6_PRIVATE_API\n")

# Each case: its name, whether it refers to Qt, the expected references
# ("-" for none, entries separated by "|"), and its objdump output.
set(case_names public private two_private last no_qt other)
set(public_expect TRUE -)
set(public_dump "${header}${glibc}${core_public}")
set(private_expect TRUE "libQt6Qml.so.6 (Qt_6.11_PRIVATE_API)")
set(private_dump "${header}${core_public}${qml_private}${glibc}")
set(two_private_expect TRUE "libQt6Qml.so.6 (Qt_6.11_PRIVATE_API)|libQt6Quick.so.6 (Qt_6.6_PRIVATE_API)")
set(two_private_dump "${header}${qml_private}${quick_private}")
set(last_expect TRUE "libQt6Qml.so.6 (Qt_6.11_PRIVATE_API)")
set(last_dump "${header}${last_line}")
set(no_qt_expect FALSE -)
set(no_qt_dump "${header}${glibc}")
set(other_expect TRUE -)
set(other_dump "${header}${core_public}${other_private}")

foreach(name IN LISTS case_names)
    list(GET ${name}_expect 0 expected_qt)
    list(GET ${name}_expect 1 expected)
    if(expected STREQUAL "-")
        set(expected "")
    endif()
    string(REPLACE "|" ";" expected "${expected}")
    ferrous_qt_private_api_references(references has_qt ${name}_dump)
    if(NOT "${references}" STREQUAL "${expected}")
        message(FATAL_ERROR "${name}: found \"${references}\", expected \"${expected}\"")
    endif()
    if(NOT has_qt STREQUAL expected_qt)
        message(FATAL_ERROR "${name}: refers to Qt ${has_qt}, expected ${expected_qt}")
    endif()
endforeach()
