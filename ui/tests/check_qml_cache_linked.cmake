# Fails unless BINARY has the QML module's compiled QML and its type
# registration. The linker leaves both out of a binary that links the
# ferrous_ui_qml archive without taking it whole, and the QML then compiles
# from source at run time.
#
# The symbols checked for are ones whose address the module hands to Qt, so
# they keep their names with link-time optimization too:
# - Main.qml's compiled unit, and the qmlcache loader's entry for it, which the
#   loader stores in its lookup table. The entry is defined only in the loader.
# - The type registration function, which is passed to Qt at startup.
# The loader's qInitResources_qmlcache_ferrous_ui_qml is not checked: only its
# static initializer calls it, and link-time optimization inlines it there.
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
set(main_qml "QmlCacheGeneratedCode::_qt_qml_FerrousUi_qml_Main_qml")
foreach(symbol IN ITEMS "${main_qml}::qmlData" "${main_qml}::unit" "qml_register_types_FerrousUi()")
    string(REGEX REPLACE "[][()*+.?^$|\\]" "\\\\\\0" pattern "${symbol}")
    # Link-time optimization may add a clone suffix, such as
    # "[clone .lto_priv.0]".
    if(NOT symbols MATCHES " ${pattern}( \\[clone [^\n]*)?\n")
        message(FATAL_ERROR "${BINARY} lacks ${symbol}, so its QML would compile "
            "at run time; link the ferrous_ui_qml archive whole")
    endif()
endforeach()
