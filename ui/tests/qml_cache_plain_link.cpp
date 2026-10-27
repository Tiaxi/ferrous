// SPDX-License-Identifier: GPL-3.0-or-later

// Links the ferrous_ui_qml archive as an ordinary static library, without
// WHOLE_ARCHIVE. Like the app, it refers to nothing in the archive, so the
// linker leaves out the compiled QML, and check_qml_cache_linked.cmake must
// reject this binary.
int main() {
    return 0;
}
