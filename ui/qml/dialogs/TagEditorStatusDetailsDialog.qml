// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/BreakableText.js" as BreakableText

// The whole of the tag editor's last status, such as each file a save
// could not write, in a window of its own over the editor: the editor's
// summary, semibold, the lead, then each section of the details, its
// heading over its files, each file 16 px in, in the text's colour, the
// lines it wraps to under its own start (it was the plain text,
// every line at the heading's x past the first). The heading of what
// failed is semibold, in the negative colour; that of what succeeded is
// regular, secondary, so the summary leads (all three were
// semibold). It opens as tall as they are, up to a limit.
Components.DialogWindow {
    id: root

    required property var tagEditorApi

    readonly property real contentPadding: Kirigami.Units.gridUnit
    readonly property real contentSpacing: Kirigami.Units.largeSpacing
    readonly property real itemIndent: root.uiPalette.scaledSize(16)
    // The details' scroll bar shows only while they are taller than the
    // window, and their lines leave it room whether it shows or not: lines
    // that wrapped anew, narrower beside it, could need it then, and it
    // stayed (at 24 pt Details opened with the bar, its last line
    // out of view, where the window had been laid out hidden and small).
    readonly property real scrollBarRoom: detailsView.ScrollBar.vertical.width
    readonly property real lineWidth: Math.max(0, detailsView.width - root.scrollBarRoom)
    // The details as shown.
    readonly property string detailsHtml: root.detailsHtmlAt(root.lineWidth)

    // Set as they change, such as where a narrower window breaks a path
    // anew. A new text drops the selection, which goes first: the style
    // warns of a selection the text no longer has.
    onDetailsHtmlChanged: {
        detailsText.deselect()
        detailsText.text = root.detailsHtml
    }

    // TagEditorController.statusDetailSections as rich text: each heading
    // a paragraph, each item one of its own, indented, with 12 px before
    // each section after the first (not a grid unit, 18 px). A path
    // in an item breaks between its folders, those that fit on a line
    // `width` wide whole, and an error's code stays whole (a line broke
    // in "ferrous-tagsave").
    function detailsHtmlAt(width) {
        const sections = root.tagEditorApi.statusDetailSections || []
        const indent = root.itemIndent
        const itemWidth = width - indent
        const fits = function(name) {
            return detailsMetrics.advanceWidth(name) <= itemWidth
        }
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
                    + textColor + "\">"
                    + root.escaped(BreakableText.breakableMessage(String(items[j] || "").trim(), fits)) + "</p>"
            }
        }
        return html
    }

    // Copies the selected details as they read, without the characters
    // that only shape their lines.
    function copySelectedDetails() {
        if (detailsText.selectionStart === detailsText.selectionEnd) {
            return
        }
        detailsClipboardText.text = BreakableText.plainText(
            detailsText.getText(detailsText.selectionStart, detailsText.selectionEnd))
        detailsClipboardText.selectAll()
        detailsClipboardText.copy()
        detailsClipboardText.text = ""
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
    // Its width and its limit below with the default font grow with a
    // larger one, within the screen.
    defaultWidth: root.uiPalette.scaledSize(Kirigami.Units.gridUnit * 36)
    // As tall as the summary and the details at the width it opens at,
    // each time it opens, up to a limit (it opened 522 px tall
    // for six lines).
    defaultHeight: Math.min(root.uiPalette.scaledSize(Kirigami.Units.gridUnit * 29),
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
            width: Math.max(0, parent.width - root.scrollBarRoom)
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.Wrap
            text: root.detailsHtmlAt(detailsSizer.width)
            font: detailsText.font
        }

        FontMetrics {
            id: detailsMetrics
            font: detailsText.font
        }

        // Holds the text Copy puts on the clipboard.
        TextEdit {
            id: detailsClipboardText
            textFormat: TextEdit.PlainText
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
            id: detailsView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                id: detailsText
                objectName: "tagEditorStatusDetailsText"
                readOnly: true
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.Wrap
                Component.onCompleted: detailsText.text = root.detailsHtml
                selectByMouse: true
                // On the summary's x, as plain text on the window; on the
                // right, the scroll bar's room, where the view does not
                // give it to the bar.
                leftPadding: 0
                rightPadding: Math.max(0, root.scrollBarRoom - detailsView.rightPadding)
                topPadding: 0
                bottomPadding: 0
                background: null
                Accessible.name: I18n.i18nc("@label", "Details")

                // Copy, from the keyboard or the context menu, copies the
                // details as they read (see copySelectedDetails).
                function copy() {
                    root.copySelectedDetails()
                }

                Keys.onPressed: event => {
                    if (event.matches(StandardKey.Copy)) {
                        root.copySelectedDetails()
                        event.accepted = true
                    }
                }
            }
        }
    }
}
