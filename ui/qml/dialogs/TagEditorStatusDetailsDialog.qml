// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

// The whole of the tag editor's last status, such as each file a save
// could not write, in a window of its own over the editor: the editor's
// summary, semibold, the lead, then each section of the details, its
// heading over its files, each file 16 px in, in the text's colour, the
// lines it wraps to under its own start (#373 H40: it was the plain text,
// every line at the heading's x past the first). The heading of what
// failed is semibold, in the negative colour; that of what succeeded is
// regular, secondary, so the summary leads (#373 H40: all three were
// semibold). It opens as tall as they are, up to a limit.
Components.DialogWindow {
    id: root

    required property var tagEditorApi

    readonly property real contentPadding: Kirigami.Units.gridUnit
    readonly property real contentSpacing: Kirigami.Units.largeSpacing
    // TagEditorController.statusDetailSections as rich text: each heading
    // a paragraph, each item one of its own, indented, with 12 px before
    // each section after the first (#373 H40: a grid unit, 18 px).
    readonly property string detailsHtml: {
        const sections = root.tagEditorApi.statusDetailSections || []
        const indent = root.uiPalette.scaledSize(16)
        const sectionGap = root.uiPalette.scaledSize(12)
        const textColor = root.cssColor(root.uiPalette.text)
        let html = ""
        for (let i = 0; i < sections.length; ++i) {
            const section = sections[i] || {}
            const headingStyle = section.failed === true
                ? "font-weight:600; color:" + root.cssColor(root.uiPalette.negative)
                : "font-weight:400; color:" + root.cssColor(root.uiPalette.textSecondary)
            html += "<p style=\"margin-top:" + (i > 0 ? sectionGap : 0) + "px; margin-bottom:0px; "
                + headingStyle + "\">" + root.escaped(section.heading) + "</p>"
            const items = section.items || []
            for (let j = 0; j < items.length; ++j) {
                html += "<p style=\"margin-top:0px; margin-bottom:0px; margin-left:" + indent + "px; color:"
                    + textColor + "\">" + root.escaped(String(items[j] || "").trim()) + "</p>"
            }
        }
        return html
    }

    // A colour as rich text's CSS takes it, opaque.
    function cssColor(color) {
        return Qt.rgba(color.r, color.g, color.b, 1).toString()
    }

    function escaped(value) {
        return String(value || "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }

    objectName: "tagEditorStatusDetailsDialog"
    title: I18n.i18nc("@title:window", "Tag Editor Details")
    minimumWidth: Kirigami.Units.gridUnit * 20
    minimumHeight: Kirigami.Units.gridUnit * 9
    defaultWidth: Kirigami.Units.gridUnit * 36
    // As tall as the summary and the details at the width it opens at,
    // each time it opens, up to a limit (#373 H40: it opened 522 px tall
    // for six lines).
    defaultHeight: Math.min(Kirigami.Units.gridUnit * 29,
        root.contentPadding
        + (summary.text.length > 0 ? Math.ceil(summarySizer.implicitHeight) + root.contentSpacing : 0)
        + Math.ceil(detailsSizer.implicitHeight) + detailsText.topPadding + detailsText.bottomPadding
        + root.contentPadding + statusDetailsButtons.implicitHeight)
    sizesOnEachOpen: true

    onAboutToShow: statusDetailsCloseButton.forceActiveFocus(Qt.PopupFocusReason)

    footer: Components.DialogButtons {
        id: statusDetailsButtons
        primary: statusDetailsCloseButton
        Button {
            id: statusDetailsCloseButton
            objectName: "tagEditorStatusDetailsCloseButton"
            text: I18n.i18nc("@action:button", "Close")
            icon.name: "window-close"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
    }

    // The summary and the details at the width the window opens at, which
    // its content has only once it shows: they set its default height.
    Item {
        width: root.openingWidth - 2 * root.contentPadding
        visible: false

        Label {
            id: summarySizer
            width: parent.width
            wrapMode: Text.Wrap
            text: summary.text
            font: summary.font
        }

        TextEdit {
            id: detailsSizer
            width: parent.width - detailsText.leftPadding - detailsText.rightPadding
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.Wrap
            text: root.detailsHtml
            font: detailsText.font
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: root.contentPadding
        anchors.rightMargin: root.contentPadding
        anchors.topMargin: root.contentPadding
        anchors.bottomMargin: root.contentPadding
        spacing: root.contentSpacing

        // The editor's own status, such as "Saved 1 file; 1 failed.".
        Label {
            id: summary
            objectName: "tagEditorStatusDetailsSummary"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.tagEditorApi.statusText
            visible: text.length > 0
            font.weight: Font.DemiBold
            color: root.uiPalette.text
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                id: detailsText
                objectName: "tagEditorStatusDetailsText"
                readOnly: true
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.Wrap
                text: root.detailsHtml
                selectByMouse: true
                // On the summary's x, as plain text on the window.
                leftPadding: 0
                rightPadding: 0
                topPadding: 0
                bottomPadding: 0
                background: null
                Accessible.name: I18n.i18nc("@label", "Details")
            }
        }
    }
}
