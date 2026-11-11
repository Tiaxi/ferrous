// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

// Numbers the tracks, in a window of its own over the tag editor.
Components.CompactDialogWindow {
    id: root

    required property var tagEditorApi

    readonly property int maximumNumber: 9999
    readonly property bool startingTrackValid: startingTrackField.acceptableInput
    readonly property bool startingDiscValid: !writeDiscCheck.checked || startingDiscField.acceptableInput
    readonly property bool inputValid: root.startingTrackValid && root.startingDiscValid

    function startingNumber(field) {
        return Number.parseInt(field.text, 10)
    }

    // Applies only valid start numbers, so mistyped text never numbers from 0.
    function apply() {
        if (!root.inputValid) {
            return false
        }
        root.tagEditorApi.autoNumber(
            root.startingNumber(startingTrackField),
            writeDiscCheck.checked ? root.startingNumber(startingDiscField) : 1,
            writeDiscCheck.checked,
            writeTotalsCheck.checked,
            resetOnFolderCheck.checked,
            resetOnDiscCheck.checked)
        root.accept()
        return true
    }

    objectName: "autoNumberDialog"
    title: I18n.i18nc("@title:window", "Auto Number")
    defaultWidth: Math.max(Kirigami.Units.gridUnit * 26, root.minimumWidth, root._neededWidth)
    // The form and the check boxes whole, in the font they are in; the
    // description above them wraps.
    contentWidth: Math.max(autoNumberForm.implicitWidth, writeDiscCheck.implicitWidth,
        writeTotalsCheck.implicitWidth, resetOnFolderCheck.implicitWidth, resetOnDiscCheck.implicitWidth)

    footer: Components.DialogFooter {

        id: dialogButtons
        primary: autoNumberApplyButton
        Button {
            id: autoNumberApplyButton
            objectName: "autoNumberApplyButton"
            text: I18n.i18nc("@action:button", "Number Tracks")
            icon.name: "format-list-ordered"
            enabled: root.inputValid
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: root.apply()
        }
        Button {
            objectName: "autoNumberCancelButton"
            text: I18n.i18nc("@action:button", "Cancel")
            icon.name: "dialog-cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
    }

    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: root.uiPalette.textSecondary
        text: I18n.i18nc("@label", "Number the selected rows, or all rows if none are selected, using the current table order.")
    }

    Kirigami.FormLayout {
        id: autoNumberForm
        objectName: "autoNumberForm"
        Layout.fillWidth: true

        TextField {
            id: startingTrackField
            Kirigami.FormData.label: I18n.i18nc("@label:textbox", "First track:")
            objectName: "autoNumberStartingTrackField"
            Layout.fillWidth: true
            text: "1"
            inputMethodHints: Qt.ImhDigitsOnly
            validator: IntValidator { bottom: 1; top: root.maximumNumber }
        }
        Label {
            objectName: "autoNumberStartingTrackError"
            Layout.fillWidth: true
            // It wraps under its field, as wide: its whole sentence, which
            // the form counts even while it is hidden, would widen the
            // fields' column past the window at large fonts (#373 H10).
            Layout.maximumWidth: startingTrackField.implicitWidth
            visible: !root.startingTrackValid
            wrapMode: Text.WordWrap
            color: root.uiPalette.negative
            text: I18n.i18nc("@info", "Enter a starting track from 1 to %1.", root.maximumNumber)
        }

        TextField {
            id: startingDiscField
            Kirigami.FormData.label: I18n.i18nc("@label:textbox", "First disc:")
            objectName: "autoNumberStartingDiscField"
            Layout.fillWidth: true
            text: "1"
            enabled: writeDiscCheck.checked
            inputMethodHints: Qt.ImhDigitsOnly
            validator: IntValidator { bottom: 1; top: root.maximumNumber }
        }
        Label {
            objectName: "autoNumberStartingDiscError"
            Layout.fillWidth: true
            // It wraps under its field, as wide: its whole sentence, which
            // the form counts even while it is hidden, would widen the
            // fields' column past the window at large fonts (#373 H10).
            Layout.maximumWidth: startingDiscField.implicitWidth
            visible: !root.startingDiscValid
            wrapMode: Text.WordWrap
            color: root.uiPalette.negative
            text: I18n.i18nc("@info", "Enter a starting disc from 1 to %1.", root.maximumNumber)
        }
    }

    CheckBox {
        id: writeDiscCheck
        objectName: "autoNumberWriteDiscCheck"
        checked: false
        text: I18n.i18nc("@option:check", "Write disc numbers")
    }
    CheckBox {
        id: writeTotalsCheck
        objectName: "autoNumberWriteTotalsCheck"
        checked: false
        text: I18n.i18nc("@option:check", "Write totals")
    }
    CheckBox {
        id: resetOnFolderCheck
        objectName: "autoNumberResetOnFolderCheck"
        checked: false
        text: I18n.i18nc("@option:check", "Reset track numbers on folder or section change")
    }
    CheckBox {
        id: resetOnDiscCheck
        objectName: "autoNumberResetOnDiscCheck"
        checked: false
        enabled: writeDiscCheck.checked
        text: I18n.i18nc("@option:check", "Reset track numbers when disc changes in current values")
    }
}
