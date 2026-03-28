// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import "../components" as Components

Dialog {
    id: root

    required property int popupTransitionMs

    modal: true
    title: "About Ferrous"
    standardButtons: Dialog.Ok
    width: 420
    enter: Components.PopupTransition { duration: root.popupTransitionMs }
    exit: Components.PopupTransition { duration: root.popupTransitionMs }

    contentItem: Label {
        width: parent.width
        wrapMode: Text.Wrap
        text: "Ferrous is a fast, Linux-native desktop music player with a Qt/QML UI and Rust backend."
        color: Kirigami.Theme.textColor
    }
}
