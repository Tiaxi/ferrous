# SPDX-License-Identifier: GPL-3.0-or-later
# Fails if BINARY refers to a symbol of Qt's private API. Such a symbol ties
# the binary, and through RPM's automatic dependencies the package, to the
# exact Qt minor version it was built with. QtPrivateApiReferences.cmake
# finds the references.
include("${CMAKE_CURRENT_LIST_DIR}/QtPrivateApiReferences.cmake")
foreach(variable IN ITEMS OBJDUMP BINARY)
    if(NOT ${variable})
        message(FATAL_ERROR "Set ${variable}.")
    endif()
endforeach()
execute_process(
    COMMAND "${OBJDUMP}" -p "${BINARY}"
    OUTPUT_VARIABLE dump
    ERROR_VARIABLE errors
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${OBJDUMP} could not read ${BINARY}: ${errors}")
endif()
ferrous_qt_private_api_references(references has_qt dump)
if(NOT has_qt)
    message(FATAL_ERROR "${OBJDUMP} found no version references to Qt in ${BINARY}")
endif()
if(references)
    list(JOIN references ", " references)
    message(FATAL_ERROR "${BINARY} refers to Qt's private API: ${references}. "
        "Use public API instead, so that a Qt update needs no rebuild.")
endif()
