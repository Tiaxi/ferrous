# SPDX-License-Identifier: GPL-3.0-or-later
# Runs scripts/extract-messages.sh into WORK_DIR and fails unless it writes a
# template with known strings from the C++, the QML, and its JavaScript, and
# without warnings from xgettext.

if(NOT BASH OR NOT EXTRACT_SCRIPT OR NOT WORK_DIR)
    message(FATAL_ERROR "Pass -DBASH=<path> -DEXTRACT_SCRIPT=<path> -DWORK_DIR=<dir>.")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
execute_process(
    COMMAND "${BASH}" "${EXTRACT_SCRIPT}" "${WORK_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "extract-messages.sh failed (${result}):\n${output}${errors}")
endif()
if(NOT errors STREQUAL "")
    message(FATAL_ERROR "extract-messages.sh warned:\n${errors}")
endif()

set(template "${WORK_DIR}/ferrous.pot")
if(NOT EXISTS "${template}")
    message(FATAL_ERROR "extract-messages.sh wrote no ${template}.")
endif()
file(READ "${template}" contents)
foreach(expected
        # QML, with a context.
        "msgctxt \"@title:window\"\nmsgid \"Edit Tags\""
        # A QML plural.
        "msgid \"%1 track (%2)\"\nmsgid_plural \"%1 tracks (%2)\""
        # JavaScript.
        "msgid \"Repeat one\""
        # C++, and a C++ plural.
        "msgid \"Searching iTunes...\""
        "msgid \"Saved %1 file.\"\nmsgid_plural \"Saved %1 files.\""
        # KI18n's placeholders.
        "#, kde-format")
    string(FIND "${contents}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${template} lacks:\n${expected}")
    endif()
endforeach()
