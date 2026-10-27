# SPDX-License-Identifier: GPL-3.0-or-later
# Fails unless QmlCacheSymbols.cmake finds the symbols in nm output as GNU and
# LLVM nm print it, with and without link-time optimization's suffixes, and
# reports the first one missing.
include("${CMAKE_CURRENT_LIST_DIR}/QmlCacheSymbols.cmake")

set(main_qml "QmlCacheGeneratedCode::_qt_qml_FerrousUi_qml_Main_qml")
set(data "0000000000001000 R ${main_qml}::qmlData\n")
set(unit "0000000000002000 r ${main_qml}::unit\n")
set(registration "0000000000003000 T qml_register_types_FerrousUi()\n")

# Each case: its name, the expected missing symbol ("-" for none), and the
# variable with its nm output.
set(plain "${data}${unit}${registration}")
set(gnu_clones "0000000000001000 R ${main_qml}::qmlData [clone .lto_priv.0]\n"
    "0000000000002000 d ${main_qml}::unit [clone .lto_priv.1]\n"
    "0000000000003000 t qml_register_types_FerrousUi() [clone .lto_priv.0] [clone .cold]\n")
string(CONCAT gnu_clones ${gnu_clones})
set(llvm_suffixes "0000000000001000 r ${main_qml}::qmlData (.lto_priv.0)\n"
    "0000000000002000 d ${main_qml}::unit (.lto_priv.1)\n"
    "0000000000003000 t qml_register_types_FerrousUi() (.lto_priv.0)\n")
string(CONCAT llvm_suffixes ${llvm_suffixes})
set(no_data "${unit}${registration}")
set(only_longer_names "0000000000001000 R ${main_qml}::qmlDataEnd\n"
    "0000000000002000 b ${main_qml}::unitRegistry\n${registration}")
string(CONCAT only_longer_names ${only_longer_names})
set(no_unit "${data}${registration}")
set(other_registration "${data}${unit}0000000000003000 T qml_register_types_FerrousUiExtra()\n")
set(cases
    plain - gnu_clones - llvm_suffixes -
    no_data "${main_qml}::qmlData"
    only_longer_names "${main_qml}::qmlData"
    no_unit "${main_qml}::unit"
    other_registration "qml_register_types_FerrousUi()"
)
while(cases)
    list(POP_FRONT cases name expected)
    if(expected STREQUAL "-")
        set(expected "")
    endif()
    ferrous_qml_cache_missing_symbol(missing ${name})
    if(NOT missing STREQUAL expected)
        message(FATAL_ERROR "${name}: missing \"${missing}\", expected \"${expected}\"")
    endif()
endwhile()
