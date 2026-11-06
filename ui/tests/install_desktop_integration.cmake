# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installs the desktop_integration component (the desktop file, the AppStream
# metadata and the app icon's sizes) of BUILD_DIR into PREFIX, which it first
# empties, for tests/tst_app_icon.cpp.
foreach(_variable IN ITEMS BUILD_DIR PREFIX)
    if(NOT DEFINED ${_variable})
        message(FATAL_ERROR "${_variable} is not set")
    endif()
endforeach()
file(REMOVE_RECURSE "${PREFIX}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=DESTDIR
        "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${PREFIX}" --component desktop_integration
    RESULT_VARIABLE _result
)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Installing the desktop_integration component failed: ${_result}")
endif()
