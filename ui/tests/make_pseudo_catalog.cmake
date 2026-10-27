# SPDX-License-Identifier: GPL-3.0-or-later
# Makes the x-test pseudo-translation that the i18n tests run with: every
# message of the template, wrapped in ⟦ and ⟧, compiled into
# OUT_DIR/x-test/LC_MESSAGES/ferrous.mo. Nothing of it is installed.

foreach(variable BASH EXTRACT_SCRIPT WORK_DIR OUT_DIR MSGEN MSGFILTER MSGFMT SED)
    if(NOT ${variable})
        message(FATAL_ERROR "Pass -D${variable}=<value>.")
    endif()
endforeach()

function(run_checked)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${ARGN} failed (${result}):\n${errors}")
    endif()
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
run_checked("${BASH}" "${EXTRACT_SCRIPT}" "${WORK_DIR}")

# The template leaves the charset and plural rule to each translation.
file(READ "${WORK_DIR}/ferrous.pot" template)
string(REPLACE "charset=CHARSET" "charset=UTF-8" template "${template}")
string(REPLACE "nplurals=INTEGER; plural=EXPRESSION;" "nplurals=2; plural=(n != 1);"
    template "${template}")
string(REPLACE "\"Language: \\n\"" "\"Language: x-test\\n\"" template "${template}")
file(WRITE "${WORK_DIR}/x-test.pot" "${template}")

run_checked("${MSGEN}" --output-file=${WORK_DIR}/english.po "${WORK_DIR}/x-test.pot")
run_checked("${MSGFILTER}" --keep-header --input=${WORK_DIR}/english.po
    --output-file=${WORK_DIR}/x-test.po "${SED}" -e "s/^/⟦/" -e "s/$/⟧/")
file(MAKE_DIRECTORY "${OUT_DIR}/x-test/LC_MESSAGES")
run_checked("${MSGFMT}" --check-format --output-file=${OUT_DIR}/x-test/LC_MESSAGES/ferrous.mo
    "${WORK_DIR}/x-test.po")
