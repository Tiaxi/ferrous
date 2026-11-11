# SPDX-License-Identifier: GPL-3.0-or-later
# Fails unless the SHARDS shards of the Qt test BINARY, each run with
# FERROUS_TEST_SHARD=<shard>/<SHARDS>, together run every test that QTest's
# -datatags lists for the binary (a function without data, or a row of one),
# each in exactly one shard. A shard lists its tests with -datatags.
foreach(variable IN ITEMS BINARY SHARDS)
    if(NOT ${variable})
        message(FATAL_ERROR "Set ${variable}.")
    endif()
endforeach()

function(ferrous_shard_tests out shard)
    if(shard)
        set(environment "FERROUS_TEST_SHARD=${shard}")
    else()
        set(environment "--unset=FERROUS_TEST_SHARD")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${environment} "${BINARY}" -datatags
        OUTPUT_VARIABLE listing
        ERROR_VARIABLE errors
        RESULT_VARIABLE result
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${BINARY} -datatags failed (shard '${shard}'): ${errors}")
    endif()
    # One list element a line; a data tag may have characters that CMake's
    # lists take apart.
    string(REPLACE ";" "<semicolon>" listing "${listing}")
    string(REPLACE "[" "<open>" listing "${listing}")
    string(REPLACE "]" "<close>" listing "${listing}")
    string(REPLACE "\n" ";" tests "${listing}")
    list(REMOVE_ITEM tests "")
    if(NOT tests)
        message(FATAL_ERROR "${BINARY} listed no tests (shard '${shard}')")
    endif()
    set(${out} "${tests}" PARENT_SCOPE)
endfunction()

ferrous_shard_tests(expected "")
set(sharded "")
foreach(shard RANGE 1 ${SHARDS})
    ferrous_shard_tests(tests "${shard}/${SHARDS}")
    list(APPEND sharded ${tests})
endforeach()

set(unique ${sharded})
list(REMOVE_DUPLICATES unique)
list(LENGTH sharded sharded_count)
list(LENGTH unique unique_count)
if(NOT sharded_count EQUAL unique_count)
    set(repeated ${sharded})
    foreach(test IN LISTS unique)
        list(FIND repeated "${test}" index)
        list(REMOVE_AT repeated ${index})
    endforeach()
    list(JOIN repeated ", " repeated)
    message(FATAL_ERROR "More than one shard runs: ${repeated}")
endif()
list(SORT expected)
list(SORT unique)
if(NOT unique STREQUAL expected)
    set(missing ${expected})
    list(REMOVE_ITEM missing ${unique})
    set(extra ${unique})
    list(REMOVE_ITEM extra ${expected})
    list(JOIN missing ", " missing)
    list(JOIN extra ", " extra)
    message(FATAL_ERROR "No shard runs: ${missing}. Not tests of the binary: ${extra}")
endif()
list(LENGTH expected count)
message(STATUS "The ${SHARDS} shards run each of the ${count} tests once")
