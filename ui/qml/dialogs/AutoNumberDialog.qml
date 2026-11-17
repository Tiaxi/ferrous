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
    // A start number's field fits four digits; its error wraps under it.
    readonly property real numberFieldWidth: root.uiPalette.gridUnit * 5
    readonly property real errorWidth: root.uiPalette.gridUnit * 14

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
    // As wide as its form, whose check boxes do not wrap, in the font
    // they are in; the description above it wraps (#373 H10, H38).
    contentWidth: autoNumberForm.implicitWidth
    defaultWidth: Math.max(root.minimumWidth, root._neededWidth)

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

    // The description starts where the form does, and is no wider, so the
    // dialog has one left edge however wide the user makes it.
    Label {
        objectName: "autoNumberDescription"
        Layout.fillWidth: true
        Layout.maximumWidth: autoNumberForm.implicitWidth
        Layout.alignment: Qt.AlignHCenter
        wrapMode: Text.WordWrap
        color: root.uiPalette.textSecondary
        text: I18n.i18nc("@label", "Number the selected rows, or all rows if none are selected, using the current table order.")
    }

    // One form in reading order, in three groups a FormGroupGap apart, the
    // form's own row spacing within them: the tracks' start and totals;
    // the discs' option, and the start and the reset at each disc that it
    // enables (#373 H41: the reset was in the last group, apart from the
    // option that enables it); then the reset at each folder. The check
    // boxes have no labels, so they start where the fields do.
    Kirigami.FormLayout {
        id: autoNumberForm
        objectName: "autoNumberForm"
        Layout.fillWidth: true

        TextField {
            id: startingTrackField
            Kirigami.FormData.label: I18n.i18nc("@label:textbox", "First track:")
            objectName: "autoNumberStartingTrackField"
            // Kirigami's form stretches a text field unless it has a
            // maximum.
            Layout.preferredWidth: root.numberFieldWidth
            Layout.maximumWidth: root.numberFieldWidth
            text: "1"
            inputMethodHints: Qt.ImhDigitsOnly
            validator: IntValidator { bottom: 1; top: root.maximumNumber }
        }
        Label {
            objectName: "autoNumberStartingTrackError"
            // It wraps under its field: its whole sentence, which the form
            // counts even while it is hidden, would widen the fields'
            // column past the window at large fonts (#373 H10).
            Layout.maximumWidth: root.errorWidth
            visible: !root.startingTrackValid
            wrapMode: Text.WordWrap
            color: root.uiPalette.negative
            text: I18n.i18nc("@info", "Enter a starting track from 1 to %1.", root.maximumNumber)
        }
        // With the tracks (#373 H38: under the disc's start, it read as a
        // disc option): it writes each track's total, and the discs' too
        // when it writes disc numbers.
        CheckBox {
            id: writeTotalsCheck
            objectName: "autoNumberWriteTotalsCheck"
            checked: false
            text: I18n.i18nc("@option:check", "Write totals")
        }

        Components.FormGroupGap {
            uiPalette: root.uiPalette
        }

        CheckBox {
            id: writeDiscCheck
            objectName: "autoNumberWriteDiscCheck"
            checked: false
            text: I18n.i18nc("@option:check", "Write disc numbers")
        }
        TextField {
            id: startingDiscField
            Kirigami.FormData.label: I18n.i18nc("@label:textbox", "First disc:")
            objectName: "autoNumberStartingDiscField"
            // Kirigami's form stretches a text field unless it has a
            // maximum.
            Layout.preferredWidth: root.numberFieldWidth
            Layout.maximumWidth: root.numberFieldWidth
            text: "1"
            enabled: writeDiscCheck.checked
            inputMethodHints: Qt.ImhDigitsOnly
            validator: IntValidator { bottom: 1; top: root.maximumNumber }
        }
        Label {
            objectName: "autoNumberStartingDiscError"
            Layout.maximumWidth: root.errorWidth
            visible: !root.startingDiscValid
            wrapMode: Text.WordWrap
            color: root.uiPalette.negative
            text: I18n.i18nc("@info", "Enter a starting disc from 1 to %1.", root.maximumNumber)
        }
        CheckBox {
            id: resetOnDiscCheck
            objectName: "autoNumberResetOnDiscCheck"
            checked: false
            enabled: writeDiscCheck.checked
            text: I18n.i18nc("@option:check", "Reset track numbers at each new disc")
        }

        Components.FormGroupGap {
            uiPalette: root.uiPalette
        }

        CheckBox {
            id: resetOnFolderCheck
            objectName: "autoNumberResetOnFolderCheck"
            checked: false
            text: I18n.i18nc("@option:check", "Reset track numbers at each new folder or section")
        }
    }
}
