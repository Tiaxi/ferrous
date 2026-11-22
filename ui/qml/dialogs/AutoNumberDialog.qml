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
    // Breeze draws a check box's 16 px box in the middle of its 20 px
    // indicator, which leaves room for the focus frame: the check boxes,
    // box and text, move left by that room, so their boxes start where the
    // fields' frames do (they were 2 px in). A style whose box
    // fills its indicator needs none.
    readonly property real checkBoxInset: Math.max(0, (writeTotalsCheck.indicator.width - 16) / 2)

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
    // they are in; the lead and the scope above it wrap.
    contentWidth: autoNumberForm.implicitWidth
    defaultWidth: Math.max(root.minimumWidth, root._neededWidth)
    // The keyboard in the first field, its number selected, so that typing
    // replaces it at once.
    initialFocusItem: startingTrackField
    onAboutToShow: startingTrackField.selectAll()

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

    // The lead, semibold as every dialog's first sentence is, and under it
    // which rows it numbers, secondary, as a confirmation's question and
    // what it acts on (the whole of it, secondary, opened the window on
    // its quietest text). They start where the form does, and are no
    // wider, so the dialog has one left edge however wide the user makes it.
    ColumnLayout {
        Layout.fillWidth: true
        Layout.maximumWidth: autoNumberForm.implicitWidth
        Layout.alignment: Qt.AlignHCenter
        spacing: 2

        Label {
            objectName: "autoNumberLead"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.weight: Font.DemiBold
            color: root.uiPalette.text
            textFormat: Text.PlainText
            text: I18n.i18nc("@info", "Number tracks in the table’s order.")
        }

        Label {
            objectName: "autoNumberDescription"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: root.uiPalette.textSecondary
            textFormat: Text.PlainText
            text: I18n.i18nc("@info the rows that Auto Number numbers",
                "Applies to the selected rows, or all rows if none are selected.")
        }
    }

    // One form in reading order, in three groups a FormGroupGap apart, the
    // form's own row spacing within them: the tracks' start and totals;
    // the discs' option, and the start and the reset at each disc that it
    // enables (the reset was in the last group, apart from the
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
            // Enter numbers, as Enter in Add Library Folder's field adds.
            onAccepted: root.apply()
        }
        Label {
            objectName: "autoNumberStartingTrackError"
            // It wraps under its field: its whole sentence, which the form
            // counts even while it is hidden, would widen the fields'
            // column past the window at large fonts.
            Layout.maximumWidth: root.errorWidth
            visible: !root.startingTrackValid
            wrapMode: Text.WordWrap
            color: root.uiPalette.negative
            textFormat: Text.PlainText
            text: I18n.i18nc("@info", "Enter a starting track from 1 to %1.", root.maximumNumber)
        }
        // With the tracks (under the disc's start, it read as a
        // disc option): it writes each track's total, and the discs' too
        // when it writes disc numbers.
        CheckBox {
            id: writeTotalsCheck
            leftPadding: -root.checkBoxInset
            objectName: "autoNumberWriteTotalsCheck"
            checked: false
            text: I18n.i18nc("@option:check", "Write totals")
        }

        Components.FormGroupGap {
            uiPalette: root.uiPalette
        }

        CheckBox {
            id: writeDiscCheck
            leftPadding: -root.checkBoxInset
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
            // Enter numbers, as Enter in Add Library Folder's field adds.
            onAccepted: root.apply()
        }
        Label {
            objectName: "autoNumberStartingDiscError"
            Layout.maximumWidth: root.errorWidth
            visible: !root.startingDiscValid
            wrapMode: Text.WordWrap
            color: root.uiPalette.negative
            textFormat: Text.PlainText
            text: I18n.i18nc("@info", "Enter a starting disc from 1 to %1.", root.maximumNumber)
        }
        CheckBox {
            id: resetOnDiscCheck
            leftPadding: -root.checkBoxInset
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
            leftPadding: -root.checkBoxInset
            objectName: "autoNumberResetOnFolderCheck"
            checked: false
            text: I18n.i18nc("@option:check", "Reset track numbers at each new folder or section")
        }
    }
}
