# Fails unless BINARY has the QML module's compiled QML and its type
# registration. The linker leaves both out of a binary that links the
# ferrous_ui_qml archive without taking it whole, and the QML then compiles
# from source at run time. QmlCacheSymbols.cmake lists the symbols checked for.
include("${CMAKE_CURRENT_LIST_DIR}/QmlCacheSymbols.cmake")
foreach(variable IN ITEMS NM BINARY)
    if(NOT ${variable})
        message(FATAL_ERROR "Set ${variable}.")
    endif()
endforeach()
execute_process(
    COMMAND "${NM}" -C --defined-only "${BINARY}"
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE errors
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${NM} could not read ${BINARY}: ${errors}")
endif()
if(symbols STREQUAL "")
    message(FATAL_ERROR "${NM} found no symbols in ${BINARY}; the check needs an unstripped binary: ${errors}")
endif()
ferrous_qml_cache_missing_symbol(missing symbols)
if(missing)
    message(FATAL_ERROR "${BINARY} lacks ${missing}, so its QML would compile "
        "at run time; link the ferrous_ui_qml archive whole")
endif()
