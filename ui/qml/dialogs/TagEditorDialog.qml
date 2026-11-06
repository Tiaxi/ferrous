// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/FormatUtils.js" as FormatUtils
import "." as Dialogs

Dialog {
    id: root

    required property var tagEditorApi
    required property var uiPalette
    required property var windowRoot

    property int selectionAnchor: -1
    readonly property int totalRows: tagEditorTable.count
    readonly property int selectedCount: root.tagEditorApi.selectedCount || 0
    property bool operationInFlight: false
    property string operationText: ""
    // A rename preview runs; a load started meanwhile drops its answer.
    property bool renamePreviewPending: false
    property bool closeAfterSave: false
    // Ferrous is waiting to quit until the edits here are saved or dropped.
    property bool quitPending: false
    // The user chose to quit without waiting for the running save or rename,
    // which the exit leaves running.
    property bool quitWithoutOperation: false
    // How long quitting waits for a save or rename before it offers to quit
    // without it, as quitting does for the settings and queue.
    readonly property int quitWaitBoundMs: 5000
    readonly property real tableRowHeight: root.uiPalette.rowHeight
    // Shown in a field whose rows hold different values, which saving keeps.
    readonly property string keepText: I18n.i18nc("@info:placeholder the selected files keep their different values", "Multiple values")
    readonly property bool statusHasFailure: root.tagEditorApi.statusIsError === true
    readonly property bool busy: root.operationInFlight || root.tagEditorApi.loading || root.tagEditorApi.saving
    // Bumped whenever the selection or the edits change, for the bindings
    // that ask the editor which fields changed.
    property int summaryRevision: 0
    readonly property var editorFields: [
        { key: "title", label: I18n.i18nc("@label tag field", "Title"), formLabel: I18n.i18nc("@label:textbox tag field", "Title:"), allowTitleCase: true, allowCapitalize: true },
        { key: "artist", label: I18n.i18nc("@label tag field", "Artist"), formLabel: I18n.i18nc("@label:textbox tag field", "Artist:"), allowTitleCase: true, allowCapitalize: true },
        { key: "album", label: I18n.i18nc("@label tag field", "Album"), formLabel: I18n.i18nc("@label:textbox tag field", "Album:"), allowTitleCase: true, allowCapitalize: true },
        { key: "albumArtist", label: I18n.i18nc("@label tag field", "Album Artist"), formLabel: I18n.i18nc("@label:textbox tag field", "Album Artist:"), allowTitleCase: true, allowCapitalize: true },
        { key: "genre", label: I18n.i18nc("@label tag field", "Genre"), formLabel: I18n.i18nc("@label:textbox tag field", "Genre:"), allowTitleCase: false, allowCapitalize: true },
        { key: "year", label: I18n.i18nc("@label tag field", "Year"), formLabel: I18n.i18nc("@label:textbox tag field", "Year:"), allowTitleCase: false, allowCapitalize: false },
        { key: "trackNo", label: I18n.i18nc("@label tag field", "Track"), formLabel: I18n.i18nc("@label:textbox tag field", "Track:"), allowTitleCase: false, allowCapitalize: false },
        { key: "totalTracks", label: I18n.i18nc("@label tag field", "Tracks"), formLabel: "", allowTitleCase: false, allowCapitalize: false },
        { key: "discNo", label: I18n.i18nc("@label tag field", "Disc"), formLabel: I18n.i18nc("@label:textbox tag field", "Disc:"), allowTitleCase: false, allowCapitalize: false },
        { key: "totalDiscs", label: I18n.i18nc("@label tag field", "Discs"), formLabel: "", allowTitleCase: false, allowCapitalize: false },
        { key: "comment", label: I18n.i18nc("@label tag field", "Comment"), formLabel: I18n.i18nc("@label:textbox tag field", "Comment:"), allowTitleCase: false, allowCapitalize: false }
    ]
    // The table's columns; the last one takes the room that is left.
    readonly property var tableColumns: [
        { key: "trackNo", label: I18n.i18nc("@title:column track number", "#"), width: 40, number: true },
        { key: "title", label: I18n.i18nc("@title:column", "Title"), width: 200 },
        { key: "artist", label: I18n.i18nc("@title:column", "Artist"), width: 160 },
        { key: "album", label: I18n.i18nc("@title:column", "Album"), width: 140 },
        { key: "genre", label: I18n.i18nc("@title:column", "Genre"), width: 110 },
        { key: "year", label: I18n.i18nc("@title:column", "Year"), width: 56 },
        { key: "discNo", label: I18n.i18nc("@title:column", "Disc"), width: 48 },
        { key: "fileName", label: I18n.i18nc("@title:column", "File Name"), width: 220 },
        { key: "formatKind", label: I18n.i18nc("@title:column file format", "Format"), width: 64 },
        { key: "directory", label: I18n.i18nc("@title:column", "Folder"), width: 156 },
        { key: "status", label: I18n.i18nc("@title:column", "Status"), width: 120 }
    ]
    readonly property real tableInset: Kirigami.Units.largeSpacing
    readonly property real fixedColumnsWidth: root.tableColumns.reduce(
        function(sum, column, index) {
            return index === root.tableColumns.length - 1 ? sum : sum + column.width
        }, 2 * root.tableInset)
    readonly property real tableContentWidth: Math.max(
        root.fixedColumnsWidth + root.tableColumns[root.tableColumns.length - 1].width,
        tagEditorTable.width)

    function columnWidth(index) {
        return index === root.tableColumns.length - 1
            ? root.tableContentWidth - root.fixedColumnsWidth
            : root.tableColumns[index].width
    }

    // The fields the edits changed, by their labels.
    function changedFieldLabels() {
        root.summaryRevision
        const labels = []
        for (let i = 0; i < root.editorFields.length; ++i) {
            if (root.tagEditorApi.bulkFieldChanged(root.editorFields[i].key)) {
                labels.push(root.editorFields[i])
            }
        }
        return labels
    }

    // "8 files selected. Genre changed from Classical."
    function summaryText() {
        const count = root.selectedCount > 0 ? root.selectedCount : root.totalRows
        let text = root.selectedCount > 0
            ? I18n.i18ncp("@info:status", "%1 file selected.", "%1 files selected.", count)
            : I18n.i18ncp("@info:status", "%1 file loaded.", "%1 files loaded.", count)
        const changed = root.changedFieldLabels()
        if (changed.length === 1) {
            const key = changed[0].key
            const original = root.tagEditorApi.bulkOriginalValueMixed(key)
                ? "" : root.tagEditorApi.bulkOriginalValue(key)
            text += " " + (original.length > 0
                ? I18n.i18nc("@info:status tag field, value it had", "%1 changed from %2.", changed[0].label, original)
                : I18n.i18nc("@info:status tag field", "%1 changed.", changed[0].label))
        } else if (changed.length > 1) {
            text += " " + I18n.i18ncp("@info:status", "%1 field changed.", "%1 fields changed.", changed.length)
        }
        return text
    }

    function statusLine() {
        if (root.tagEditorApi.loading) {
            return I18n.i18nc("@info:status", "Loading tags…")
        }
        if (root.operationInFlight || root.tagEditorApi.saving) {
            return root.operationText
        }
        if (root.tagEditorApi.statusText.length > 0 && (root.statusHasFailure || tagEditorStatusFlash.visible)) {
            return root.tagEditorApi.statusText
        }
        return root.summaryText()
    }

    // The tooltip of a field's revert button: "Revert to Classical".
    function revertToolTip(key) {
        root.summaryRevision
        const original = root.tagEditorApi.bulkOriginalValueMixed(key) ? "" : root.tagEditorApi.bulkOriginalValue(key)
        return original.length > 0
            ? I18n.i18nc("@info:tooltip value the field had", "Revert to %1", original)
            : I18n.i18nc("@info:tooltip", "Revert")
    }

    modal: true
    x: Math.round((root.windowRoot.width - width) / 2)
    y: Math.round((root.windowRoot.height - height) / 2)
    width: Math.min(root.windowRoot.width - 2 * Kirigami.Units.gridUnit, Kirigami.Units.gridUnit * 80)
    height: Math.min(root.windowRoot.height - 2 * Kirigami.Units.gridUnit, Kirigami.Units.gridUnit * 46)
    title: I18n.i18nc("@title:window", "Edit Tags")
    standardButtons: Dialog.NoButton
    closePolicy: Popup.NoAutoClose

    // Sent when the edits that held up quitting are saved or dropped.
    signal quitMayProceed()

    objectName: "tagEditorDialog"

    function closeEditor() {
        // The results and errors of a save show here, so the editor stays
        // open until the save has finished.
        if (root.tagEditorApi.saving) {
            return
        }
        root.tagEditorApi.close()
        root.close()
    }

    // A field commits its typed text when it loses focus.
    function commitPendingEditorEdits() {
        if (root.visible) {
            tagEditorTable.forceActiveFocus()
        }
    }

    function triggerSave(closeAfterSuccess) {
        if (root.tagEditorApi.loading || root.tagEditorApi.saving || !root.tagEditorApi.dirty) {
            return false
        }
        root.commitPendingEditorEdits()
        root.operationInFlight = true
        root.operationText = I18n.i18nc("@info:status", "Saving tags…")
        root.closeAfterSave = closeAfterSuccess
        if (!root.tagEditorApi.save()) {
            root.operationInFlight = false
            root.operationText = ""
            root.closeAfterSave = false
            return false
        }
        return true
    }

    // Rename Files shows the new names first; renameNow renames once the
    // user agrees.
    function triggerRename() {
        if (root.tagEditorApi.loading || root.tagEditorApi.saving || root.selectedCount === 0) return
        root.commitPendingEditorEdits()
        root.operationInFlight = true
        root.operationText = I18n.i18nc("@info:status", "Checking new file names…")
        root.renamePreviewPending = root.tagEditorApi.previewRenameSelectedFiles()
        if (!root.renamePreviewPending) {
            root.operationInFlight = false
            root.operationText = ""
        }
    }

    // Renames what the preview showed, even if the selection changed since.
    function renameNow() {
        if (root.tagEditorApi.loading || root.tagEditorApi.saving) return
        root.operationInFlight = true
        root.operationText = I18n.i18nc("@info:status", "Renaming files…")
        if (!root.tagEditorApi.renamePreviewedFiles()) {
            root.operationInFlight = false
            root.operationText = ""
        }
    }

    // The close button, Cancel, and Escape all close through here.
    function requestClose() {
        if (root.tagEditorApi.saving) {
            return
        }
        root.commitPendingEditorEdits()
        if (root.tagEditorApi.dirty) {
            tagEditorCloseConfirmDialog.purpose = "close"
            tagEditorCloseConfirmDialog.open()
            return
        }
        root.closeEditor()
    }

    // Reloading drops unsaved edits, so it asks first, as closing does.
    function requestReload() {
        if (root.tagEditorApi.loading || root.tagEditorApi.saving) {
            return
        }
        root.commitPendingEditorEdits()
        if (root.tagEditorApi.dirty) {
            tagEditorCloseConfirmDialog.purpose = "reload"
            tagEditorCloseConfirmDialog.open()
            return
        }
        root.reloadNow()
    }

    function reloadNow() {
        root.clearSelection()
        root.tagEditorApi.reload()
        root.initializeSelection()
    }

    function saveAndClose() {
        if (!root.triggerSave(true)) {
            tagEditorCloseConfirmDialog.close()
            root.continueQuit(!root.tagEditorApi.dirty)
            return
        }
        if (root.quitPending) {
            // Quitting waits for this save within the same bound as for any
            // other, so the confirmation gives way to the wait.
            tagEditorCloseConfirmDialog.close()
            root.waitForOperationToQuit()
        }
    }

    // Whether Ferrous may quit now. When edits are unsaved, this asks whether
    // to save them, and when a save is running, it waits for it; either way
    // it sends quitMayProceed once quitting may go on.
    function prepareToQuit() {
        if (root.tagEditorApi.saving) {
            if (root.quitWithoutOperation) {
                return true
            }
            root.waitForOperationToQuit()
            return false
        }
        root.commitPendingEditorEdits()
        if (root.tagEditorApi.dirty) {
            root.quitPending = true
            if (!root.visible) {
                root.open()
            }
            tagEditorCloseConfirmDialog.purpose = "quit"
            tagEditorCloseConfirmDialog.open()
            return false
        }
        root.quitPending = false
        return true
    }

    // The one way quitting waits for a running save or rename: it shows the
    // files being written, and after the bound offers to quit without them.
    function waitForOperationToQuit() {
        root.quitPending = true
        // A repeated quit request neither stacks dialogs nor postpones the
        // offer to quit anyway.
        if (!tagEditorQuitWaitDialog.visible) {
            tagEditorQuitWaitDialog.offerChoice = false
            tagEditorQuitWaitDialog.open()
            tagEditorQuitWaitTimeout.restart()
        }
    }

    // The save or rename that quitting waited for has finished.
    function stopWaitingForOperation() {
        tagEditorQuitWaitTimeout.stop()
        tagEditorQuitWaitDialog.close()
        root.quitWithoutOperation = false
    }

    function continueQuit(succeeded) {
        if (!root.quitPending) {
            return
        }
        root.quitPending = false
        // A failure stays on screen, and the user decides again.
        if (succeeded && root.prepareToQuit()) {
            root.quitMayProceed()
        }
    }

    function selectOnlyRow(row) {
        root.tagEditorApi.setSelectedRows([row])
        root.selectionAnchor = row
        tagEditorTable.currentIndex = row
    }

    function handleRowClick(row, modifiers) {
        tagEditorTable.forceActiveFocus()
        const ctrl = (modifiers & Qt.ControlModifier) !== 0
        const shift = (modifiers & Qt.ShiftModifier) !== 0
        if (shift && root.selectionAnchor >= 0) {
            root.tagEditorApi.selectRange(root.selectionAnchor, row)
            tagEditorTable.currentIndex = row
            return
        }
        if (ctrl) {
            root.tagEditorApi.toggleRowSelected(row)
            root.selectionAnchor = row
            tagEditorTable.currentIndex = row
            return
        }
        root.selectOnlyRow(row)
    }

    // Moves the current row by keyboard; with Shift, the selection extends
    // from the anchor to it.
    function moveCurrentRow(row, extend) {
        if (root.totalRows === 0) {
            return
        }
        const target = Math.max(0, Math.min(root.totalRows - 1, row))
        if (extend && root.selectionAnchor >= 0) {
            root.tagEditorApi.selectRange(root.selectionAnchor, target)
            tagEditorTable.currentIndex = target
        } else {
            root.selectOnlyRow(target)
        }
        tagEditorTable.positionViewAtIndex(target, ListView.Contain)
    }

    function handleTableKey(event) {
        const current = Math.max(0, tagEditorTable.currentIndex)
        const page = Math.max(1, Math.floor(tagEditorTable.height / root.tableRowHeight) - 2)
        const extend = (event.modifiers & Qt.ShiftModifier) !== 0
        switch (event.key) {
        case Qt.Key_Up: root.moveCurrentRow(tagEditorTable.currentIndex < 0 ? 0 : current - 1, extend); break
        case Qt.Key_Down: root.moveCurrentRow(tagEditorTable.currentIndex < 0 ? 0 : current + 1, extend); break
        case Qt.Key_PageUp: root.moveCurrentRow(current - page, extend); break
        case Qt.Key_PageDown: root.moveCurrentRow(current + page, extend); break
        case Qt.Key_Home: root.moveCurrentRow(0, extend); break
        case Qt.Key_End: root.moveCurrentRow(root.totalRows - 1, extend); break
        case Qt.Key_Space:
            if (tagEditorTable.currentIndex < 0) {
                return
            }
            root.tagEditorApi.toggleRowSelected(tagEditorTable.currentIndex)
            root.selectionAnchor = tagEditorTable.currentIndex
            break
        default:
            return
        }
        event.accepted = true
    }

    function initializeSelection() {
        if (root.totalRows > 0 && root.selectedCount === 0) {
            root.selectOnlyRow(0)
        }
    }

    function clearSelection() {
        root.tagEditorApi.setSelectedRows([])
        root.selectionAnchor = -1
    }

    function selectAllRows() {
        root.tagEditorApi.selectAllRows()
        root.selectionAnchor = root.totalRows > 0 ? 0 : -1
    }

    function cellText(columnKey, row) {
        switch (columnKey) {
        case "directory": return FormatUtils.basenameFromPath(row.directory || "")
        case "title":
        case "artist":
        case "album":
        case "genre": {
            // Show every value of a field that holds several.
            const extra = row.extraValues ? row.extraValues[columnKey] : undefined
            const value = row[columnKey] || ""
            return extra && extra.length > 0 ? [value].concat(extra).join("; ") : value
        }
        case "status":
            return row.errorText.length > 0 ? row.errorText : (row.dirty ? I18n.i18nc("@item tag editor row status", "Modified") : "")
        default:
            return row[columnKey] || ""
        }
    }

    onOpened: {
        root.initializeSelection()
        tagEditorTable.forceActiveFocus()
    }
    onClosed: {
        root.clearSelection()
        root.renamePreviewPending = false
        root.operationInFlight = false
        root.operationText = ""
        tagEditorStatusFlash.stop()
        tagEditorStatusFlash.visible = false
    }

    Connections {
        target: root.tagEditorApi
        ignoreUnknownSignals: true

        function onOpenChanged() {
            if (root.tagEditorApi.open && !root.visible) {
                root.open()
            } else if (!root.tagEditorApi.open && root.visible) {
                root.close()
            }
        }

        function onLoadingChanged() {
            if (root.tagEditorApi.loading && root.renamePreviewPending) {
                root.renamePreviewPending = false
                root.operationInFlight = false
                root.operationText = ""
            }
            if (!root.tagEditorApi.loading) root.initializeSelection()
        }
        function onSaveFinished(success) {
            root.stopWaitingForOperation()
            root.operationInFlight = false
            root.operationText = ""
            // A failure shows in the editor, under the confirmation.
            tagEditorCloseConfirmDialog.close()
            if (success && root.closeAfterSave) {
                root.closeEditor()
            }
            root.closeAfterSave = false
            tagEditorStatusFlash.restart()
            root.continueQuit(success)
        }
        function onRenamePreviewReady(rows, error) {
            // A closed editor has nothing to rename.
            if (!root.renamePreviewPending || !root.visible) {
                return
            }
            root.renamePreviewPending = false
            root.operationInFlight = false
            root.operationText = ""
            renamePreviewDialog.rows = rows
            renamePreviewDialog.errorText = error
            renamePreviewDialog.open()
        }
        function onRenameFinished() {
            root.stopWaitingForOperation()
            root.operationInFlight = false
            root.operationText = ""
            tagEditorStatusFlash.restart()
            root.continueQuit(!root.tagEditorApi.statusIsError)
        }
        function onStatusChanged() {
            if (!root.tagEditorApi.statusText.length) {
                tagEditorStatusFlash.stop()
                tagEditorStatusFlash.visible = false
                return
            }
            tagEditorStatusFlash.restart()
        }
    }

    // A result shows in the status line for a moment, then the selection's
    // summary again; a failure stays.
    Timer {
        id: tagEditorStatusFlash
        interval: 2200
        repeat: false
        property bool visible: false
        onRunningChanged: {
            if (running) {
                visible = true
            }
        }
        onTriggered: visible = false
    }

    Connections {
        target: root.tagEditorApi
        ignoreUnknownSignals: true
        function onBulkSummaryChanged() { root.summaryRevision++ }
        function onSelectionChanged() { root.summaryRevision++ }
    }

    Dialogs.AutoNumberDialog {
        id: autoNumberDialog
        tagEditorApi: root.tagEditorApi
        uiPalette: root.uiPalette
        windowRoot: root.windowRoot
    }

    Dialogs.RenamePreviewDialog {
        id: renamePreviewDialog
        uiPalette: root.uiPalette
        windowRoot: root.windowRoot
        onRenameRequested: root.renameNow()
        onClosed: rows = []
    }

    Dialogs.TagEditorCloseConfirmDialog {
        id: tagEditorCloseConfirmDialog
        tagEditorApi: root.tagEditorApi
        uiPalette: root.uiPalette
        windowRoot: root.windowRoot
        onSaveRequested: root.saveAndClose()
        onDiscardRequested: {
            if (tagEditorCloseConfirmDialog.forReload) {
                root.reloadNow()
                return
            }
            root.closeEditor()
            root.continueQuit(true)
        }
        onKeepEditingRequested: root.quitPending = false
    }

    Dialogs.TagEditorQuitWaitDialog {
        id: tagEditorQuitWaitDialog
        tagEditorApi: root.tagEditorApi
        uiPalette: root.uiPalette
        windowRoot: root.windowRoot
        onKeepWaitingRequested: {
            tagEditorQuitWaitDialog.offerChoice = false
            tagEditorQuitWaitTimeout.restart()
        }
        onQuitAnywayRequested: {
            tagEditorQuitWaitTimeout.stop()
            tagEditorQuitWaitDialog.close()
            root.quitPending = false
            root.quitWithoutOperation = true
            root.quitMayProceed()
        }
        onStopQuittingRequested: {
            tagEditorQuitWaitTimeout.stop()
            root.quitPending = false
        }
    }

    Timer {
        id: tagEditorQuitWaitTimeout
        objectName: "tagEditorQuitWaitTimeout"
        interval: root.quitWaitBoundMs
        onTriggered: tagEditorQuitWaitDialog.offerChoice = true
    }

    Dialogs.TagEditorStatusDetailsDialog {
        id: tagEditorStatusDetailsDialog
        tagEditorApi: root.tagEditorApi
        uiPalette: root.uiPalette
        windowRoot: root.windowRoot
    }

    background: Components.DialogSurface {
        uiPalette: root.uiPalette
    }

    // The style's header closes at once; this one asks about unsaved edits.
    header: Components.DialogHeader {
        title: root.title
        closeButtonName: "tagEditorCloseButton"
        closeEnabled: !root.tagEditorApi.saving
        onCloseRequested: root.requestClose()
    }

    // A text field of a tag: its label turns semibold and a revert button
    // shows while the edits change it.
    component TagField: TextField {
        id: fieldEditor

        required property var field
        property bool touched: false
        property var editRowsSnapshot: []
        readonly property bool changed: root.summaryRevision >= 0
            && root.tagEditorApi.bulkFieldChanged(fieldEditor.field.key)

        function captureEditRowsSnapshot() {
            editRowsSnapshot = root.tagEditorApi.selectedRows()
        }

        function syncFromController() {
            const value = root.tagEditorApi.bulkValue(fieldEditor.field.key)
            const mixed = root.tagEditorApi.bulkValueMixed(fieldEditor.field.key)
            root.setExtraValuesNote(fieldEditor.field.key,
                root.tagEditorApi.extraValuesNote(fieldEditor.field.key))
            if (activeFocus && touched) {
                return
            }
            text = mixed ? "" : value
            placeholderText = mixed ? root.keepText : ""
            editRowsSnapshot = []
            touched = false
        }

        objectName: "tagEditorField_" + fieldEditor.field.key
        Layout.fillWidth: true
        selectByMouse: true
        persistentSelection: true
        rightPadding: fieldEditor.changed
            ? revertButton.width + Kirigami.Units.smallSpacing
            : leftPadding
        Accessible.name: fieldEditor.field.label
        Accessible.description: fieldEditor.changed ? I18n.i18nc("@info", "Changed") : ""

        onActiveFocusChanged: {
            if (activeFocus && !touched) {
                captureEditRowsSnapshot()
            }
        }
        onTextEdited: {
            if (!touched) {
                captureEditRowsSnapshot()
            }
            touched = true
        }
        onEditingFinished: {
            if (!touched) {
                return
            }
            root.tagEditorApi.applyBulkFieldToRows(
                editRowsSnapshot,
                fieldEditor.field.key,
                text)
            editRowsSnapshot = []
            touched = false
            syncFromController()
        }

        Component.onCompleted: syncFromController()

        Connections {
            target: root.tagEditorApi
            ignoreUnknownSignals: true
            function onSelectionChanged() { fieldEditor.syncFromController() }
            function onBulkSummaryChanged() { fieldEditor.syncFromController() }
        }

        ToolButton {
            id: revertButton
            objectName: "tagEditorRevert_" + fieldEditor.field.key
            anchors.right: parent.right
            anchors.rightMargin: 2
            anchors.verticalCenter: parent.verticalCenter
            width: Kirigami.Units.iconSizes.small + 2 * Kirigami.Units.smallSpacing
            height: width
            visible: fieldEditor.changed
            enabled: !root.busy
            icon.name: "edit-undo"
            display: AbstractButton.IconOnly
            text: I18n.i18nc("@action:button %1 is a tag field", "Revert %1", fieldEditor.field.label)
            ToolTip.visible: hovered || visualFocus
            ToolTip.delay: Kirigami.Units.toolTipDelay
            ToolTip.text: root.revertToolTip(fieldEditor.field.key)
            onClicked: {
                fieldEditor.touched = false
                root.tagEditorApi.revertBulkField(fieldEditor.field.key)
                fieldEditor.syncFromController()
            }
        }
    }

    // A case change of one field, in the Tools menu.
    component CaseItem: MenuItem {
        required property string fieldKey
        required property bool titleCase
        objectName: (titleCase ? "tagEditorTitleCase_" : "tagEditorCapitalCase_") + fieldKey
        text: root.fieldAt(fieldKey).label
        onClicked: root.changeCase(fieldKey, titleCase)
    }

    // The label of a field: semibold while the edits change it.
    component FieldLabel: Label {
        required property var field
        readonly property bool changed: root.summaryRevision >= 0
            && root.tagEditorApi.bulkFieldChanged(field.key)
        objectName: "tagEditorLabel_" + field.key
        Layout.preferredWidth: root.fieldLabelWidth
        horizontalAlignment: Text.AlignRight
        text: field.formLabel
        font.weight: changed ? Font.DemiBold : Font.Normal
        color: root.uiPalette.text
        Accessible.ignored: true
    }

    // The notes under the fields of rows with several values, by field.
    property var extraValuesNotes: ({})

    function setExtraValuesNote(key, note) {
        if ((root.extraValuesNotes[key] || "") === note) {
            return
        }
        const notes = Object.assign({}, root.extraValuesNotes)
        notes[key] = note
        root.extraValuesNotes = notes
    }

    function fieldAt(key) {
        for (let i = 0; i < root.editorFields.length; ++i) {
            if (root.editorFields[i].key === key) {
                return root.editorFields[i]
            }
        }
        return null
    }

    // A case change of `key` in the edited rows.
    function changeCase(key, titleCase) {
        root.commitPendingEditorEdits()
        if (titleCase) {
            root.tagEditorApi.applyEnglishTitleCase(key)
        } else if (key === "genre") {
            root.tagEditorApi.applyGenreCapitalize()
        } else {
            root.tagEditorApi.applyFinnishCapitalize(key)
        }
    }

    footer: RowLayout {
        spacing: Kirigami.Units.largeSpacing

        Button {
            id: toolsButton
            objectName: "tagEditorToolsButton"
            Layout.leftMargin: Kirigami.Units.largeSpacing * 2
            text: I18n.i18nc("@action:button", "Tools")
            enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving
            // A menu button, which Breeze draws with its arrow.
            Accessible.role: Accessible.ButtonMenu
            onClicked: toolsMenu.open()

            Menu {
                id: toolsMenu
                objectName: "tagEditorToolsMenu"
                y: toolsButton.height

                MenuItem {
                    objectName: "tagEditorAutoNumberButton"
                    text: I18n.i18nc("@action:inmenu", "Auto Number…")
                    icon.name: "format-list-ordered"
                    enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving
                    onClicked: {
                        root.commitPendingEditorEdits()
                        autoNumberDialog.open()
                    }
                }
                MenuItem {
                    objectName: "tagEditorRenameButton"
                    text: I18n.i18nc("@action:inmenu", "Rename Files…")
                    icon.name: "edit-rename"
                    enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving && root.selectedCount > 0
                    onClicked: root.triggerRename()
                }
                MenuSeparator {}
                Menu {
                    title: I18n.i18nc("@title:menu", "Title Case")

                    CaseItem { fieldKey: "title"; titleCase: true }
                    CaseItem { fieldKey: "artist"; titleCase: true }
                    CaseItem { fieldKey: "album"; titleCase: true }
                    CaseItem { fieldKey: "albumArtist"; titleCase: true }
                }
                Menu {
                    title: I18n.i18nc("@title:menu", "Capital Case")

                    CaseItem { fieldKey: "title"; titleCase: false }
                    CaseItem { fieldKey: "artist"; titleCase: false }
                    CaseItem { fieldKey: "album"; titleCase: false }
                    CaseItem { fieldKey: "albumArtist"; titleCase: false }
                    CaseItem { fieldKey: "genre"; titleCase: false }
                }
                MenuSeparator {}
                MenuItem {
                    text: I18n.i18nc("@action:inmenu", "Select All")
                    icon.name: "edit-select-all"
                    enabled: root.totalRows > 0
                    onClicked: root.selectAllRows()
                }
                MenuItem {
                    objectName: "tagEditorClearSelectionItem"
                    text: I18n.i18nc("@action:inmenu", "Clear Selection")
                    icon.name: "edit-select-none"
                    enabled: root.selectedCount > 0
                    onClicked: root.clearSelection()
                }
            }
        }

        Button {
            objectName: "tagEditorReloadButton"
            text: I18n.i18nc("@action:button", "Reload")
            icon.name: "view-refresh"
            enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving
            onClicked: root.requestReload()
        }

        BusyIndicator {
            running: root.busy
            visible: running
            Layout.preferredWidth: Kirigami.Units.iconSizes.small
            Layout.preferredHeight: Kirigami.Units.iconSizes.small
        }

        Label {
            objectName: "tagEditorStatusLabel"
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.smallSpacing
            text: root.statusLine()
            font.pointSize: root.uiPalette.smallPointSize
            color: root.statusHasFailure && !root.busy ? root.uiPalette.negative : root.uiPalette.textSecondary
            elide: Text.ElideRight
            Accessible.role: Accessible.StatusBar
        }

        Button {
            objectName: "tagEditorDetailsButton"
            visible: !root.busy && root.tagEditorApi.statusDetails.length > 0
            text: I18n.i18nc("@action:button", "Details…")
            onClicked: tagEditorStatusDetailsDialog.open()
        }

        Components.DialogButtons {

            id: dialogButtons
            primary: tagEditorSaveButton
            Button {
                id: tagEditorSaveButton
                objectName: "tagEditorSaveButton"
                text: root.tagEditorApi.saving
                    ? I18n.i18nc("@action:button", "Saving…")
                    : I18n.i18nc("@action:button", "Save")
                icon.name: "document-save"
                enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving && root.tagEditorApi.dirty
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                onClicked: root.triggerSave(false)
            }
            Button {
                objectName: "tagEditorCancelButton"
                text: I18n.i18nc("@action:button", "Cancel")
                icon.name: "dialog-cancel"
                enabled: !root.tagEditorApi.saving
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                onClicked: root.requestClose()
            }
        }
    }

    contentItem: RowLayout {
        // Enter presses the default button, as in KDE's dialogs.
        Keys.onPressed: function(event) { dialogButtons.handleEnter(event, Window.activeFocusItem) }

        spacing: Kirigami.Units.gridUnit - 2

        Shortcut {
            sequences: ["Ctrl+S"]
            context: Qt.WindowShortcut
            enabled: root.visible && !root.tagEditorApi.loading && !root.tagEditorApi.saving && root.tagEditorApi.dirty
            onActivated: root.triggerSave(false)
        }

        Shortcut {
            sequences: ["Ctrl+A"]
            context: Qt.WindowShortcut
            enabled: root.visible && root.totalRows > 0
            onActivated: root.selectAllRows()
        }

        Shortcut {
            sequences: ["Esc"]
            context: Qt.WindowShortcut
            enabled: root.visible && !tagEditorCloseConfirmDialog.visible
                && !autoNumberDialog.visible && !tagEditorStatusDetailsDialog.visible
                && !tagEditorQuitWaitDialog.visible && !renamePreviewDialog.visible
                && !toolsMenu.visible
            onActivated: root.requestClose()
        }

        ScrollView {
            id: tagEditorFieldsScroll
            objectName: "tagEditorFields"
            Layout.preferredWidth: Kirigami.Units.gridUnit * 19
            Layout.minimumWidth: Kirigami.Units.gridUnit * 16
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: fieldsColumn
                width: tagEditorFieldsScroll.availableWidth
                spacing: Kirigami.Units.largeSpacing + 2

                FieldRow { field: root.fieldAt("title") }
                FieldRow { field: root.fieldAt("artist") }
                FieldRow { field: root.fieldAt("album") }
                FieldRow { field: root.fieldAt("albumArtist") }
                FieldRow { field: root.fieldAt("genre") }
                FieldRow { field: root.fieldAt("year") }
                NumberPair { first: root.fieldAt("trackNo"); second: root.fieldAt("totalTracks") }
                NumberPair { first: root.fieldAt("discNo"); second: root.fieldAt("totalDiscs") }
                FieldRow { field: root.fieldAt("comment") }
            }
        }

        // The files, on the view colour, with the queue's header.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.uiPalette.view
            radius: 3
            border.width: 1
            border.color: Kirigami.ColorUtils.linearInterpolation(
                root.uiPalette.view, root.uiPalette.text, Kirigami.Theme.frameContrast)

            // Only the visible rows have items, so thousands of rows open at
            // once. Selection is a role, so each row reads it once.
            ListView {
                id: tagEditorTable
                objectName: "tagEditorTable"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                focus: true
                activeFocusOnTab: true
                model: root.tagEditorApi.tableModel
                currentIndex: -1
                keyNavigationEnabled: false
                highlightFollowsCurrentItem: false
                contentWidth: root.tableContentWidth
                flickableDirection: Flickable.AutoFlickIfNeeded
                boundsBehavior: Flickable.StopAtBounds
                headerPositioning: ListView.OverlayHeader
                pixelAligned: true
                ScrollBar.vertical: ScrollBar {}
                ScrollBar.horizontal: ScrollBar {}
                Accessible.role: Accessible.Table
                Accessible.name: I18n.i18nc("@label", "Files")

                Keys.onPressed: function(event) { root.handleTableKey(event) }

                header: Rectangle {
                    objectName: "tagEditorTableHeader"
                    z: 2
                    width: root.tableContentWidth
                    height: root.tableRowHeight
                    color: root.uiPalette.view

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: root.tableInset
                        anchors.rightMargin: root.tableInset
                        anchors.bottomMargin: 1

                        Repeater {
                            model: root.tableColumns
                            delegate: Item {
                                id: headerCell
                                required property var modelData
                                required property int index
                                readonly property bool editable: root.fieldAt(headerCell.modelData.key) !== null
                                readonly property bool changed: headerCell.editable && root.summaryRevision >= 0
                                    && root.tagEditorApi.fieldChanged(headerCell.modelData.key)
                                objectName: "tagEditorColumn_" + headerCell.modelData.key
                                width: root.columnWidth(headerCell.index)
                                height: parent.height

                                Row {
                                    anchors.fill: parent
                                    anchors.leftMargin: Kirigami.Units.largeSpacing
                                    anchors.rightMargin: Kirigami.Units.largeSpacing
                                    spacing: Kirigami.Units.smallSpacing
                                    layoutDirection: headerCell.modelData.number ? Qt.RightToLeft : Qt.LeftToRight

                                    Label {
                                        objectName: "tagEditorColumnLabel"
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: Math.min(implicitWidth, parent.width
                                            - (columnRevert.visible ? columnRevert.width + parent.spacing : 0))
                                        text: headerCell.modelData.label
                                        font.pointSize: root.uiPalette.smallPointSize
                                        font.weight: headerCell.changed ? Font.DemiBold : Font.Normal
                                        color: headerCell.changed ? root.uiPalette.text : root.uiPalette.textSecondary
                                        elide: Text.ElideRight
                                    }

                                    ToolButton {
                                        id: columnRevert
                                        objectName: "tagEditorColumnRevert_" + headerCell.modelData.key
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: headerCell.changed
                                        enabled: !root.busy
                                        width: Kirigami.Units.iconSizes.small + Kirigami.Units.smallSpacing
                                        height: width
                                        padding: 0
                                        icon.name: "edit-undo"
                                        icon.width: Kirigami.Units.iconSizes.small - 2
                                        icon.height: Kirigami.Units.iconSizes.small - 2
                                        display: AbstractButton.IconOnly
                                        text: I18n.i18nc("@action:button %1 is a tag field", "Revert %1 in Every File",
                                            headerCell.modelData.label)
                                        ToolTip.visible: hovered || visualFocus
                                        ToolTip.delay: Kirigami.Units.toolTipDelay
                                        ToolTip.text: text
                                        onClicked: root.tagEditorApi.revertField(headerCell.modelData.key)
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: root.uiPalette.hairline
                    }
                }

                delegate: Item {
                    id: tableRow
                    objectName: "tagEditorRow"
                    required property int index
                    required property string fileName
                    required property string directory
                    required property string formatKind
                    required property string title
                    required property string artist
                    required property string album
                    required property string genre
                    required property string year
                    required property string trackNo
                    required property string discNo
                    required property bool dirty
                    required property bool selected
                    required property string errorText
                    required property var extraValues
                    readonly property bool current: ListView.isCurrentItem && tagEditorTable.activeFocus

                    width: root.tableContentWidth
                    height: root.tableRowHeight
                    Accessible.role: Accessible.Row
                    Accessible.name: tableRow.fileName
                    Accessible.selected: tableRow.selected

                    Components.RowBackground {
                        objectName: "tagEditorRowBackground"
                        uiPalette: root.uiPalette
                        scrollbarLane: 0
                        hovered: rowMouse.containsMouse
                        selected: tableRow.selected
                        active: tagEditorTable.activeFocus
                        focused: tableRow.current
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        hoverEnabled: true
                        onClicked: function(mouse) {
                            root.handleRowClick(tableRow.index, mouse.modifiers)
                        }
                    }

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: root.tableInset
                        anchors.rightMargin: root.tableInset

                        Repeater {
                            model: root.tableColumns
                            delegate: Label {
                                required property var modelData
                                required property int index
                                width: root.columnWidth(index)
                                height: parent.height
                                leftPadding: Kirigami.Units.largeSpacing
                                rightPadding: Kirigami.Units.largeSpacing
                                text: root.cellText(modelData.key, tableRow)
                                font.features: modelData.number ? root.uiPalette.tabularFigures : ({})
                                color: modelData.key === "status" && tableRow.errorText.length > 0
                                    ? root.uiPalette.negative
                                    : (modelData.number
                                        ? root.uiPalette.textTertiaryOn(tableRow.selected)
                                        : root.uiPalette.text)
                                verticalAlignment: Text.AlignVCenter
                                horizontalAlignment: modelData.number ? Text.AlignRight : Text.AlignLeft
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }
        }
    }

    // The labels' column: as wide as the longest label, semibold.
    readonly property real fieldLabelWidth: {
        let widest = 0
        for (let i = 0; i < root.editorFields.length; ++i) {
            widest = Math.max(widest, boldLabelMetrics.advanceWidth(root.editorFields[i].formLabel))
        }
        return Math.ceil(widest)
    }

    FontMetrics {
        id: boldLabelMetrics
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.basePointSize
        font.weight: Font.DemiBold
    }

    // A field with its label on the left and, under it, the note on rows
    // that hold several values.
    component FieldRow: ColumnLayout {
        id: fieldRow
        required property var field
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing

            FieldLabel { field: fieldRow.field }
            TagField { field: fieldRow.field }
        }

        Label {
            objectName: "tagEditorExtraValuesNote_" + fieldRow.field.key
            Layout.fillWidth: true
            Layout.leftMargin: root.fieldLabelWidth + Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
            visible: text.length > 0
            text: root.extraValuesNotes[fieldRow.field.key] || ""
            wrapMode: Text.WordWrap
            font.pointSize: root.uiPalette.smallPointSize
            color: root.uiPalette.textSecondary
        }
    }

    // Track and Disc: the number, "of", and the total.
    component NumberPair: RowLayout {
        id: pair
        required property var first
        required property var second
        Layout.fillWidth: true
        spacing: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing

        FieldLabel { field: pair.first }

        TagField {
            field: pair.first
            Layout.fillWidth: false
            Layout.preferredWidth: Kirigami.Units.gridUnit * 7
        }

        Label {
            text: I18n.i18nc("@label track or disc number of the total", "of")
            color: root.uiPalette.textSecondary
            Accessible.ignored: true
        }

        TagField {
            field: pair.second
            Layout.fillWidth: false
            Layout.preferredWidth: Kirigami.Units.gridUnit * 3 + Kirigami.Units.largeSpacing
        }

        Item { Layout.fillWidth: true }
    }
}
