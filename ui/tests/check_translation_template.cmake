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
        "msgid \"%1 new issue\"\nmsgid_plural \"%1 new issues\""
        # JavaScript.
        "msgid \"Repeat one\""
        # C++, and a C++ plural.
        "msgid \"Searching iTunes…\""
        "msgid \"Saved %1 file.\"\nmsgid_plural \"Saved %1 files.\""
        # KI18n's placeholders.
        "#, kde-format")
    string(FIND "${contents}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${template} lacks:\n${expected}")
    endif()
endforeach()

# Failure texts the UI owns say "couldn't", as their titles and leads do:
# "could not" beside "Couldn't" read as two voices. A context may still say
# it; texts built in the Rust backend are not in the template.
# Semicolons and brackets would split or join CMake's list items.
string(REPLACE ";" "<semicolon>" lines "${contents}")
string(REPLACE "[" "<open>" lines "${lines}")
string(REPLACE "]" "<close>" lines "${lines}")
string(REPLACE "\n" ";" lines "${lines}")
set(field "")
foreach(line IN LISTS lines)
    if(line MATCHES "^(msgctxt|msgid_plural|msgid|msgstr)")
        set(field "${CMAKE_MATCH_1}")
    elseif(NOT line MATCHES "^\"")
        set(field "")
    endif()
    if(field MATCHES "^msgid" AND line MATCHES "[Cc]ould not")
        message(FATAL_ERROR "${template} has a message that says \"could not\":\n${line}")
    endif()
endforeach()
