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
    readonly property int renameCount: root.countRows(function(row) { return root.isRenamed(row) })
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

    // The summary's sentences: how many files are renamed, then how many
    // can't be; or why no new name could be worked out.
    function summarySentences() {
        if (root.errorText.length > 0) {
            return [I18n.i18nc("@info", "The new names couldn’t be worked out: %1", root.errorText)]
        }
        const sentences = [I18n.i18ncp("@info", "%1 file will be renamed.", "%1 files will be renamed.",
            root.renameCount)]
        if (root.refusedCount > 0) {
            sentences.push(I18n.i18ncp("@info", "%1 file can’t be renamed and keeps its name.",
                "%1 files can’t be renamed and keep their names.", root.refusedCount))
        }
        return sentences
    }

    // Each sentence on a line of its own, as wide as the window opens for
    // it, so none wraps a word onto a line alone.
    function summaryText() {
        return root.summarySentences().join("\n")
    }

    // Whether a file gets a new name.
    function isRenamed(row) {
        return row.ok && !row.unchanged
    }

    // Whether a file's line has an arrow and a new name: it gets one, or
    // can't have the one its tags give.
    function showsNewName(row) {
        return !row.unchanged && (row.newName || "").length > 0 && row.newName !== row.oldName
    }

    // The new names' column in a file's line: that name, or that the file
    // keeps its own.
    function newColumnText(row) {
        return root.showsNewName(row) ? row.newName
            : I18n.i18nc("@item in the column of files' new names", "keeps its name")
    }

    // A file's line as screen readers read it: the names and the arrow
    // show in columns.
    function rowText(row) {
        return root.showsNewName(row)
            ? I18n.i18nc("@item old file name, new file name", "%1 → %2", row.oldName, row.newName)
            : I18n.i18nc("@item file name", "%1 (keeps its name)", row.oldName)
    }

    objectName: "renamePreviewDialog"
    title: I18n.i18nc("@title:window", "Rename Files")
    // The content, which has the keyboard as it opens.
    readonly property Item body: renamePreviewContent
    minimumWidth: Kirigami.Units.gridUnit * 24
    minimumHeight: Kirigami.Units.gridUnit * 9
    // As wide as its widest line, each time it opens: a file's old name,
    // arrow and new name in their columns, a sentence of the summary, the
    // message that nothing is renamed, or the buttons; no narrower than its
    // minimum, and no wider than 42 grid units, which grow with a larger
    // font, nor than Edit Tags under it less an inset on either side,
    // within the screen, where a longer name wraps in its column (a fixed
    // 756 px held 390 px of lines, and 1814 px at 24 pt; long new names
    // opened it at 1691 px over a 1600 px Edit Tags). Only the first rows
    // are measured: more scroll, and a list that long opens at its limit
    // anyway.
    readonly property real parentInset: root.uiPalette.scaledSize(Kirigami.Units.largeSpacing * 3)
    readonly property real maximumDefaultWidth: Math.min(root.uiPalette.scaledSize(Kirigami.Units.gridUnit * 42),
        root.windowRoot && root.windowRoot.width > 0
            ? root.windowRoot.width - 2 * root.parentInset : Number.POSITIVE_INFINITY)
    readonly property int measuredRowLimit: 400
    // Between a renamed file's old name and its new one, with room on
    // either side: 8 px with the default font, more with a larger one.
    readonly property string arrowText: I18n.i18nc("@item:intext between a file's old name and its new name", "→")
    readonly property real arrowClearance: root.uiPalette.scaledSize(Kirigami.Units.largeSpacing)
    readonly property real arrowColumnWidth: {
        rowMetrics.font
        return Math.ceil(rowMetrics.advanceWidth(root.arrowText)) + 2 * root.arrowClearance
    }
    // The widest name in each column, as their labels draw them, the widest
    // old name of the files that get a new one, and the old names' column
    // as the window opens: no wider than the names need that fit in it
    // beside the new ones at the window's widest. A longer name wraps
    // there, and is given the room for about two lines, or a quarter of the
    // list, where it is the widest (at 24 pt a kept name opened the window
    // at its 1814 px, with 180 px empty before the other rows' arrows).
    readonly property var measuredRows: {
        // The binding follows the font: advanceWidth() is a function, which
        // it cannot follow.
        rowMetrics.font
        // A label lays its text out up to a pixel or so wider than its
        // advance, and would wrap a word of it.
        const widthOf = function(text) {
            return Math.ceil(rowMetrics.advanceWidth(text)) + 2
        }
        let renamedName = 0
        let oldName = 0
        let newName = 0
        const oldWidths = []
        const measured = Math.min(root.rows.length, root.measuredRowLimit)
        for (let i = 0; i < measured; ++i) {
            const row = root.rows[i]
            const width = widthOf(row.oldName)
            oldWidths.push(width)
            oldName = Math.max(oldName, width)
            if (root.showsNewName(row)) {
                renamedName = Math.max(renamedName, width)
            }
            newName = Math.max(newName, widthOf(root.newColumnText(row)))
        }
        const widest = root.maximumDefaultWidth - 2 * root.contentPadding - root.listGutter
        const room = widest - root.arrowColumnWidth - newName
        const wrapped = Math.ceil(Math.max(0.25 * widest, 0.5 * oldName))
        const openingOldName = oldWidths.reduce(function(column, width) {
            return width <= room ? Math.max(column, width) : column
        }, Math.max(renamedName, Math.min(oldName, room, wrapped)))
        return {
            renamedName: renamedName,
            oldName: oldName,
            newName: newName,
            openingOldName: openingOldName
        }
    }
    readonly property real widestContentLine: {
        if (root.rows.length === 0 && root.errorText.length === 0) {
            return Math.max(emptyTitleMetrics.advanceWidth(emptyTitle.text),
                emptyTextMetrics.advanceWidth(emptyText.text))
        }
        summaryMetrics.font
        let widest = 0
        const sentences = root.summarySentences()
        for (let i = 0; i < sentences.length; ++i) {
            widest = Math.max(widest, summaryMetrics.advanceWidth(sentences[i]))
        }
        const measured = root.measuredRows
        if (root.rows.length > 0) {
            widest = Math.max(widest,
                measured.openingOldName + root.arrowColumnWidth + measured.newName + root.listGutter)
        }
        return widest
    }
    defaultWidth: Math.min(root.maximumDefaultWidth,
        Math.max(Math.ceil(2 * root.contentPadding + root.widestContentLine), dialogButtons.implicitWidth))
    // The old names' column, every row's: as wide as the widest, where the
    // window holds it; where it does not, it gives way to the new names,
    // which are what is checked before renaming. The renamed files' old
    // names give way down to 40 % of the list, and then the new names wrap
    // too (the arrows stood at seven places, and the new names had no
    // column). A name that stays gives way further, to the renamed files'
    // column or a quarter of the list, and wraps in the column, so a long
    // one is whole and keeps "keeps its name" under the others (elided, it
    // was cut; whole on one line, it pushed that out of the column).
    function oldNameColumnIn(line) {
        const measured = root.measuredRows
        const least = Math.max(Math.floor(0.25 * line), Math.min(measured.renamedName, Math.floor(0.4 * line)))
        return Math.max(0, Math.min(measured.oldName,
            Math.max(least, line - root.arrowColumnWidth - measured.newName)))
    }
    readonly property real oldNameColumnWidth: root.oldNameColumnIn(previewList.width - root.listGutter)
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
    // shows (its height, read as the window opened, was still
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
            // As many rows as the tallest window shows as they wrap, each
            // one past them a line.
            height += root.contentSpacing + (root.rows.length - 1) * root.listSpacing
                + (root.rows.length - rowSizers.count) * Math.ceil(nameLineSizer.implicitHeight)
            for (let i = 0; i < rowSizers.count; ++i) {
                const sizer = rowSizers.itemAt(i)
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

    // The content's lines, in their fonts, for the width it opens at.
    FontMetrics {
        id: summaryMetrics
        font: summary.font
    }

    FontMetrics {
        id: rowMetrics
        font: nameLineSizer.font
    }

    FontMetrics {
        id: emptyTitleMetrics
        font: emptyTitle.font
    }

    FontMetrics {
        id: emptyTextMetrics
        font: emptyText.font
    }

    // The content's text at the width the window opens at, in the content's
    // fonts and wrapping: they set its default height.
    Item {
        id: openingSizers
        width: root.openingWidth - 2 * root.contentPadding
        visible: false
        readonly property real line: openingSizers.width - root.listGutter
        readonly property real oldNameColumn: root.oldNameColumnIn(openingSizers.line)

        Label {
            id: summarySizer
            width: parent.width
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: root.summaryText()
            font: summary.font
        }

        // A file's line, unwrapped.
        Label {
            id: nameLineSizer
            textFormat: Text.PlainText
            text: " "
        }

        // The first rows, their names wrapped in their columns and a
        // refused file's error under them, as many as the tallest window
        // shows.
        Repeater {
            id: rowSizers
            model: root.rows.slice(0, Math.ceil(root.maximumHeight / Math.max(1, nameLineSizer.implicitHeight)))
            delegate: Item {
                id: rowSizer
                required property var modelData
                implicitHeight: Math.max(oldNameSizer.implicitHeight, newNameSizer.implicitHeight)
                    + (rowSizer.modelData.ok ? 0 : Math.ceil(errorSizer.implicitHeight))

                Label {
                    id: oldNameSizer
                    width: openingSizers.oldNameColumn
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    text: rowSizer.modelData.oldName
                }

                Label {
                    id: newNameSizer
                    width: Math.max(0, openingSizers.line - openingSizers.oldNameColumn - root.arrowColumnWidth)
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    text: root.newColumnText(rowSizer.modelData)
                    font.pointSize: root.showsNewName(rowSizer.modelData)
                        ? root.uiPalette.basePointSize : root.uiPalette.smallPointSize
                }

                Components.MessageLabel {
                    id: errorSizer
                    width: openingSizers.line - root.errorIndent
                    message: rowSizer.modelData.error || ""
                    font.pointSize: root.uiPalette.smallPointSize
                }
            }
        }

        Label {
            id: emptyTitleSizer
            width: parent.width
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: emptyTitle.text
            font: emptyTitle.font
        }

        Label {
            id: emptyTextSizer
            width: parent.width
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
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

        // The lead, semibold, as every dialog's first sentence is.
        Label {
            id: summary
            objectName: "renamePreviewSummary"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.weight: Font.DemiBold
            textFormat: Text.PlainText
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
                textFormat: Text.PlainText
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
                textFormat: Text.PlainText
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
                id: previewRow
                required property var modelData
                readonly property bool renamed: root.isRenamed(modelData)
                readonly property bool showsNewName: root.showsNewName(modelData)
                // What recedes in the row: the old name and the arrow, and
                // further that a file keeps its name, which is no name. All
                // of one that can't be renamed is in the negative colour.
                readonly property color quietColor: previewRow.modelData.ok
                    ? root.uiPalette.textSecondary : root.uiPalette.negative
                width: ListView.view.width - root.listGutter
                spacing: 0

                // A file, in the three columns: its old name, the arrow where
                // it has a new one, and that name, or that it keeps its own,
                // which every row starts at one x. A name too long for its
                // column wraps in it, so none is lost, and the arrow and
                // the new name are on its first line.
                Item {
                    objectName: "renamePreviewFileRow"
                    Layout.fillWidth: true
                    implicitHeight: Math.max(oldNameLabel.implicitHeight, newNameLabel.implicitHeight)
                    Accessible.role: Accessible.StaticText
                    Accessible.name: root.rowText(previewRow.modelData)

                    Label {
                        id: oldNameLabel
                        objectName: "renamePreviewOldName"
                        width: root.oldNameColumnWidth
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        text: previewRow.modelData.oldName
                        color: previewRow.quietColor
                        Accessible.ignored: true
                    }

                    Label {
                        objectName: "renamePreviewArrow"
                        x: root.oldNameColumnWidth
                        width: root.arrowColumnWidth
                        visible: previewRow.showsNewName
                        horizontalAlignment: Text.AlignHCenter
                        textFormat: Text.PlainText
                        text: root.arrowText
                        color: previewRow.quietColor
                        Accessible.ignored: true
                    }

                    Label {
                        id: newNameLabel
                        objectName: "renamePreviewNewName"
                        anchors.baseline: oldNameLabel.baseline
                        x: root.oldNameColumnWidth + root.arrowColumnWidth
                        width: Math.max(0, parent.width - x)
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        text: root.newColumnText(previewRow.modelData)
                        // "keeps its name" is quieter than the names, in
                        // the small font and the tertiary colour (in theirs,
                        // it read as a file's).
                        font.pointSize: previewRow.showsNewName
                            ? root.uiPalette.basePointSize : root.uiPalette.smallPointSize
                        color: previewRow.renamed ? root.uiPalette.text
                            : previewRow.showsNewName || !previewRow.modelData.ok ? previewRow.quietColor
                            : root.uiPalette.textTertiary
                        Accessible.ignored: true
                    }
                }

                // A path or a system error in it wraps as a message's does.
                Components.MessageLabel {
                    Layout.fillWidth: true
                    Layout.leftMargin: root.errorIndent
                    visible: !previewRow.modelData.ok
                    message: previewRow.modelData.error || ""
                    color: root.uiPalette.negative
                    font.pointSize: root.uiPalette.smallPointSize
                }
            }
        }
    }
}
