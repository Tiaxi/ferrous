# SPDX-License-Identifier: GPL-3.0-or-later
# The symbols that show a binary has the QML module's compiled QML and its type
# registration; check_qml_cache_linked.cmake. They are ones whose address the
# module hands to Qt, so they keep their names with link-time optimization too:
# - Main.qml's compiled unit, and the qmlcache loader's entry for it, which the
#   loader stores in its lookup table. The entry is defined only in the loader.
# - The type registration function, which is passed to Qt at startup.
# The loader's qInitResources_qmlcache_ferrous_ui_qml is not checked: only its
# static initializer calls it, and link-time optimization inlines it there.

# Sets OUT_VAR to the first of the symbols missing from the demangled nm output
# in the variable named SYMBOLS_VAR, or to an empty string if none is missing.
function(ferrous_qml_cache_missing_symbol out_var symbols_var)
    set(main_qml "QmlCacheGeneratedCode::_qt_qml_FerrousUi_qml_Main_qml")
    foreach(symbol IN ITEMS "${main_qml}::qmlData" "${main_qml}::unit" "qml_register_types_FerrousUi()")
        string(REGEX REPLACE "[][()*+.?^$|\\]" "\\\\\\0" pattern "${symbol}")
        # Link-time optimization may add a suffix, which GNU nm prints as
        # "[clone .lto_priv.0]" and LLVM's nm as "(.lto_priv.0)".
        if(NOT "${${symbols_var}}" MATCHES " ${pattern}( \\[clone [^\n]*| \\(\\.[^\n]*)?\n")
            set(${out_var} "${symbol}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()
