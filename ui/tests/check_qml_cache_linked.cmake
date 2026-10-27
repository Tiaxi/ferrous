# Fails unless BINARY has the QML module's compiled QML and its type
# registration. The linker leaves both out of a binary that links the
# ferrous_ui_qml archive without taking it whole, and the QML then compiles
# from source at run time.
execute_process(
    COMMAND "${NM}" -C "${BINARY}"
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE errors
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${NM} could not read ${BINARY}: ${errors}")
endif()
foreach(symbol IN ITEMS qInitResources_qmlcache_ferrous_ui_qml qml_register_types_FerrousUi)
    string(FIND "${symbols}" "${symbol}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "${BINARY} lacks ${symbol}: its QML would compile at run time")
    endif()
endforeach()
