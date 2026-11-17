// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

// The new name of each selected file, and why any cannot be renamed, before
// anything is renamed. Cancel and Escape rename nothing.
Components.DialogWindow {
    id: root

    // TagEditorController::renamePreviewReady's rows and error.
    property var rows: []
    property string errorText: ""
    readonly property int renameCount: root.countRows(function(row) {
        return row.ok && !row.unchanged
    })
    readonly property int refusedCount: root.countRows(function(row) { return !row.ok })

    signal renameRequested()

    function countRows(predicate) {
        let count = 0
        for (let i = 0; i < root.rows.length; ++i) {
            if (predicate(root.rows[i])) {
                ++count
            }
        }
        return count
    }

    function summaryText() {
        if (root.errorText.length > 0) {
            return I18n.i18nc("@info", "The new names could not be worked out: %1", root.errorText)
        }
        let text = I18n.i18ncp("@info", "%1 file will be renamed.", "%1 files will be renamed.", root.renameCount)
        if (root.refusedCount > 0) {
            text += " " + I18n.i18ncp("@info", "%1 file can't be renamed and keeps its name.",
                "%1 files can't be renamed and keep their names.", root.refusedCount)
        }
        return text
    }

    objectName: "renamePreviewDialog"
    title: I18n.i18nc("@title:window", "Rename Files")
    readonly property real contentPadding: Kirigami.Units.gridUnit
    // The content, which has the keyboard as it opens.
    readonly property Item body: renamePreviewContent
    minimumWidth: Kirigami.Units.gridUnit * 24
    minimumHeight: Kirigami.Units.gridUnit * 9
    // Its width and its limit below with the default font grow with a
    // larger one, within the screen.
    defaultWidth: root.uiPalette.scaledSize(Kirigami.Units.gridUnit * 42)
    readonly property real contentSpacing: 10
    // The room right of the rows for the list's scroll bar, and a refused
    // file's error's indent under its name.
    readonly property real listGutter: 12
    readonly property real errorIndent: 16
    readonly property real listSpacing: 4
    readonly property real maximumHeight: root.uiPalette.scaledSize(Kirigami.Units.gridUnit * 29)
    // As tall as the summary and the list, or the message that nothing is
    // renamed, up to a limit, each time it opens. They are measured at the
    // width the window opens at, by the sizers below: the content's layout
    // gets that width, and works out its own height, only once the window
    // shows (#373 H40: its height, read as the window opened, was still
    // that of what it last showed, which left 60 px of nothing under four
    // files, and cut the list short at 24 pt).
    defaultHeight: Math.min(root.maximumHeight,
        2 * root.contentPadding + root.measuredContentHeight + dialogButtons.implicitHeight)
    sizesOnEachOpen: true
    readonly property real measuredContentHeight: {
        if (root.rows.length === 0 && root.errorText.length === 0) {
            return 2 * Kirigami.Units.gridUnit + Kirigami.Units.iconSizes.huge
                + 2 * Kirigami.Units.largeSpacing + emptyTitleSizer.implicitHeight + emptyTextSizer.implicitHeight
        }
        let height = Math.ceil(summarySizer.implicitHeight)
        if (root.rows.length > 0) {
            // Each row a line, and its error under it where it has one.
            height += root.contentSpacing + root.rows.length * Math.ceil(nameLineSizer.implicitHeight)
                + (root.rows.length - 1) * root.listSpacing
            for (let i = 0; i < errorSizers.count; ++i) {
                const sizer = errorSizers.itemAt(i)
                height += sizer ? Math.ceil(sizer.implicitHeight) : 0
            }
        }
        return height
    }

    // The keyboard in the content, whose Enter presses the default button.
    onAboutToShow: renamePreviewContent.forceActiveFocus(Qt.PopupFocusReason)

    footer: Components.DialogFooter {

        id: dialogButtons
        primary: renamePreviewRenameButton
        Button {
            id: renamePreviewRenameButton
            objectName: "renamePreviewRenameButton"
            text: I18n.i18ncp("@action:button", "Rename %1 File", "Rename %1 Files", root.renameCount)
            icon.name: "edit-rename"
            enabled: root.renameCount > 0 && root.errorText.length === 0
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: {
                // Hidden at once: a second click finds no button.
                root.dismiss()
                root.renameRequested()
            }
        }
        Button {
            objectName: "renamePreviewCancelButton"
            text: I18n.i18nc("@action:button", "Cancel")
            icon.name: "dialog-cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
    }

    // The content's text at the width the window opens at, in the content's
    // fonts and wrapping: they set its default height.
    Item {
        width: root.openingWidth - 2 * root.contentPadding
        visible: false

        Label {
            id: summarySizer
            width: parent.width
            wrapMode: Text.Wrap
            text: root.summaryText()
            font: summary.font
        }

        // A file's line, elided to one.
        Label {
            id: nameLineSizer
            text: " "
        }

        // The refused files' errors, as many as the tallest window shows.
        Repeater {
            id: errorSizers
            model: {
                const errors = []
                const room = Math.ceil(root.maximumHeight / Math.max(1, nameLineSizer.implicitHeight))
                for (let i = 0; i < root.rows.length && errors.length < room; ++i) {
                    if (!root.rows[i].ok) {
                        errors.push(root.rows[i].error || "")
                    }
                }
                return errors
            }
            delegate: Label {
                required property string modelData
                width: parent.width - root.listGutter - root.errorIndent
                wrapMode: Text.Wrap
                text: modelData
                font.pointSize: root.uiPalette.smallPointSize
            }
        }

        Label {
            id: emptyTitleSizer
            width: parent.width
            wrapMode: Text.Wrap
            text: emptyTitle.text
            font: emptyTitle.font
        }

        Label {
            id: emptyTextSizer
            width: parent.width
            wrapMode: Text.Wrap
            text: emptyText.text
        }
    }

    ColumnLayout {
        id: renamePreviewContent
        objectName: "renamePreviewContent"
        anchors.fill: parent
        anchors.leftMargin: root.contentPadding
        anchors.rightMargin: root.contentPadding
        anchors.topMargin: root.contentPadding
        focus: true
        // Enter presses the default button, as in KDE's dialogs.
        Keys.onPressed: function(event) { dialogButtons.handleEnter(event, Window.activeFocusItem, root.visible) }

        spacing: root.contentSpacing

        // The lead, semibold, as every dialog's first sentence is (#373 H40).
        Label {
            id: summary
            objectName: "renamePreviewSummary"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.weight: Font.DemiBold
            text: root.summaryText()
            visible: root.rows.length > 0 || root.errorText.length > 0
            color: root.errorText.length > 0 ? root.uiPalette.negative : root.uiPalette.text
        }

        // Nothing to list: a message alone, in the middle. Plain labels: a
        // placeholder message's selectable text brings a Copy action whose
        // Ctrl+C the window's own Copy has.
        ColumnLayout {
            objectName: "renamePreviewEmpty"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: Kirigami.Units.gridUnit
            Layout.bottomMargin: Kirigami.Units.gridUnit
            visible: root.rows.length === 0 && root.errorText.length === 0
            spacing: Kirigami.Units.largeSpacing

            Item { Layout.fillHeight: true }

            Kirigami.Icon {
                Layout.alignment: Qt.AlignHCenter
                implicitWidth: Kirigami.Units.iconSizes.huge
                implicitHeight: Kirigami.Units.iconSizes.huge
                source: "edit-rename"
                opacity: 0.5
                Accessible.ignored: true
            }

            Label {
                id: emptyTitle
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                // A heading for screen readers, as Kirigami's placeholder
                // message gives its text.
                Accessible.role: Accessible.Heading
                text: I18n.i18nc("@info", "No file is renamed")
                font.pointSize: root.uiPalette.basePointSize * 1.125
                font.weight: Font.DemiBold
                color: root.uiPalette.textSecondary
            }

            Label {
                id: emptyText
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: I18n.i18nc("@info", "The selected files already have the names their tags give.")
                color: root.uiPalette.textSecondary
            }

            Item { Layout.fillHeight: true }
        }

        ListView {
            id: previewList
            objectName: "renamePreviewList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: contentHeight
            visible: root.rows.length > 0
            clip: true
            model: root.rows
            spacing: root.listSpacing
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: ColumnLayout {
                required property var modelData
                width: ListView.view.width - root.listGutter
                spacing: 0

                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    text: modelData.unchanged
                        ? I18n.i18nc("@item file name", "%1 (keeps its name)", modelData.oldName)
                        : I18n.i18nc("@item old file name, new file name", "%1 → %2",
                            modelData.oldName, modelData.newName || "")
                    color: modelData.ok
                        ? (modelData.unchanged ? root.uiPalette.textSecondary : root.uiPalette.text)
                        : root.uiPalette.negative
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: root.errorIndent
                    visible: !modelData.ok
                    wrapMode: Text.Wrap
                    text: modelData.error || ""
                    color: root.uiPalette.negative
                    font.pointSize: root.uiPalette.smallPointSize
                }
            }
        }
    }
}
