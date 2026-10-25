// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
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
    property bool closeAfterSave: false
    // Ferrous is waiting to quit until the edits here are saved or dropped.
    property bool quitPending: false
    // The user chose to quit without waiting for the running save or rename,
    // which the exit leaves running.
    property bool quitWithoutOperation: false
    // How long quitting waits for a save or rename before it offers to quit
    // without it, as quitting does for the settings and queue.
    readonly property int quitWaitBoundMs: 5000
    readonly property int tableRowHeight: 24
    readonly property string keepText: "<keep>"
    readonly property color uiSurfaceColor: root.uiPalette.uiSurfaceColor
    readonly property color uiSurfaceRaisedColor: root.uiPalette.uiPaneColor
    readonly property color uiHeaderColor: root.uiPalette.uiHeaderColor
    readonly property color uiColumnsColor: root.uiPalette.uiColumnsColor
    readonly property color uiBorderColor: root.uiPalette.uiBorderColor
    readonly property color uiMutedTextColor: root.uiPalette.uiMutedTextColor
    readonly property bool statusHasFailure: root.tagEditorApi.statusIsError === true
    readonly property var editorFields: [
        { key: "title", label: "Title", allowTitleCase: true, allowCapitalize: true },
        { key: "artist", label: "Artist", allowTitleCase: true, allowCapitalize: true },
        { key: "album", label: "Album", allowTitleCase: true, allowCapitalize: true },
        { key: "genre", label: "Genre", allowTitleCase: false, allowCapitalize: true },
        { key: "year", label: "Year", allowTitleCase: false, allowCapitalize: false },
        { key: "albumArtist", label: "Album Artist", allowTitleCase: true, allowCapitalize: true },
        { key: "trackNo", label: "Track", allowTitleCase: false, allowCapitalize: false },
        { key: "discNo", label: "Disc", allowTitleCase: false, allowCapitalize: false },
        { key: "totalTracks", label: "Tracks", allowTitleCase: false, allowCapitalize: false },
        { key: "totalDiscs", label: "Discs", allowTitleCase: false, allowCapitalize: false },
        { key: "comment", label: "Comment", allowTitleCase: false, allowCapitalize: false }
    ]
    readonly property var tableColumns: [
        { key: "fileName", label: "File", width: 168 },
        { key: "title", label: "Title", width: 210 },
        { key: "artist", label: "Artist", width: 136 },
        { key: "album", label: "Album", width: 182 },
        { key: "trackNo", label: "#", width: 42 },
        { key: "genre", label: "Genre", width: 96 },
        { key: "year", label: "Year", width: 52 },
        { key: "discNo", label: "Disc", width: 46 },
        { key: "formatKind", label: "Fmt", width: 52 },
        { key: "directory", label: "Folder", width: 156 },
        { key: "status", label: "Status", width: 120 }
    ]
    readonly property int tableContentWidth: root.tableColumns.reduce(
        function(sum, column) { return sum + column.width + 6 }, 20)

    modal: true
    x: Math.round((root.windowRoot.width - width) / 2)
    y: Math.round((root.windowRoot.height - height) / 2)
    width: Math.min(root.windowRoot.width - 28, 1440)
    height: Math.min(root.windowRoot.height - 24, 820)
    title: "Edit Tags"
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
        root.operationText = "Saving tags..."
        root.closeAfterSave = closeAfterSuccess
        if (!root.tagEditorApi.save()) {
            root.operationInFlight = false
            root.operationText = ""
            root.closeAfterSave = false
            return false
        }
        return true
    }

    function triggerRename() {
        if (root.tagEditorApi.loading || root.tagEditorApi.saving || root.selectedCount === 0) return
        root.commitPendingEditorEdits()
        root.operationInFlight = true
        root.operationText = "Renaming files..."
        if (!root.tagEditorApi.renameSelectedFiles()) {
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
            root.quitPending = true
            // A repeated quit request neither stacks dialogs nor postpones
            // the offer to quit anyway.
            if (!tagEditorQuitWaitDialog.visible) {
                tagEditorQuitWaitDialog.offerChoice = false
                tagEditorQuitWaitDialog.open()
                tagEditorQuitWaitTimeout.restart()
            }
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
            return row.errorText.length > 0 ? row.errorText : (row.dirty ? "Modified" : "")
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
            tagEditorStatusFlash.visible = true
        }
    }

    Timer {
        id: tagEditorStatusFlash
        interval: 2200
        repeat: false
        property bool visible: false
        onTriggered: visible = false
    }

    Dialogs.AutoNumberDialog {
        id: autoNumberDialog
        tagEditorApi: root.tagEditorApi
        uiPalette: root.uiPalette
        windowRoot: root.windowRoot
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

    background: Rectangle {
        radius: 6
        color: root.uiSurfaceRaisedColor
        border.color: root.uiBorderColor
    }

    header: Frame {
        padding: 8
        background: Rectangle {
            radius: 6
            color: root.uiHeaderColor
            border.color: root.uiBorderColor
        }

        contentItem: RowLayout {
            spacing: 8

            Label {
                text: "Unified Tag Editor"
                font.pixelSize: 17
                font.weight: Font.DemiBold
            }

            Rectangle {
                visible: root.operationInFlight || root.tagEditorApi.loading || root.tagEditorApi.saving
                    || tagEditorStatusFlash.visible || root.tagEditorApi.statusText.length > 0
                radius: 4
                color: root.operationInFlight || root.tagEditorApi.loading || root.tagEditorApi.saving
                    ? Qt.rgba(0.12, 0.44, 0.72, 0.16)
                    : (root.statusHasFailure
                        ? Qt.rgba(0.78, 0.20, 0.20, 0.14)
                        : Qt.rgba(0.20, 0.55, 0.24, 0.14))
                border.color: root.operationInFlight || root.tagEditorApi.loading || root.tagEditorApi.saving
                    ? Qt.rgba(0.12, 0.44, 0.72, 0.42)
                    : (root.statusHasFailure
                        ? Qt.rgba(0.78, 0.20, 0.20, 0.42)
                        : Qt.rgba(0.20, 0.55, 0.24, 0.42))
                Layout.fillWidth: true
                implicitHeight: 28

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 8

                    BusyIndicator {
                        running: root.operationInFlight || root.tagEditorApi.loading || root.tagEditorApi.saving
                        visible: running
                        implicitWidth: 16
                        implicitHeight: 16
                    }

                    Label {
                        text: root.operationInFlight || root.tagEditorApi.loading || root.tagEditorApi.saving
                            ? (root.tagEditorApi.loading ? "Loading tags..." : root.operationText)
                            : root.tagEditorApi.statusText
                        color: root.statusHasFailure
                            ? Kirigami.Theme.negativeTextColor
                            : Kirigami.Theme.textColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Button {
                        visible: !root.operationInFlight
                            && !root.tagEditorApi.loading
                            && !root.tagEditorApi.saving
                            && root.tagEditorApi.statusDetails.length > 0
                        text: "Details"
                        padding: 3
                        implicitHeight: 22
                        onClicked: tagEditorStatusDetailsDialog.open()
                    }
                }
            }

            Item {
                visible: !root.operationInFlight
                    && !root.tagEditorApi.loading
                    && !root.tagEditorApi.saving
                    && !tagEditorStatusFlash.visible
                    && !root.tagEditorApi.statusText.length
                Layout.fillWidth: true
            }

            Label {
                text: root.selectedCount > 0
                    ? root.selectedCount + " selected"
                    : root.totalRows + " loaded"
                color: root.tagEditorApi.dirty ? Kirigami.Theme.negativeTextColor : root.uiMutedTextColor
                font.weight: Font.DemiBold
            }

            Button {
                objectName: "tagEditorCloseButton"
                text: "\u00d7"
                enabled: !root.tagEditorApi.saving
                padding: 2
                implicitWidth: 26
                implicitHeight: 24
                onClicked: root.requestClose()
            }
        }
    }

    footer: Frame {
        padding: 8
        background: Rectangle {
            radius: 6
            color: root.uiHeaderColor
            border.color: root.uiBorderColor
        }

        contentItem: RowLayout {
            spacing: 8

            Button {
                text: "Clear Selection"
                enabled: root.selectedCount > 0
                onClicked: root.clearSelection()
            }

            Item { Layout.fillWidth: true }

            Button {
                objectName: "tagEditorReloadButton"
                text: "Reload"
                enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving
                onClicked: root.requestReload()
            }
            Button {
                objectName: "tagEditorAutoNumberButton"
                text: "Auto Number"
                enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving
                onClicked: autoNumberDialog.open()
            }
            Button {
                text: "Rename Files"
                enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving && root.selectedCount > 0
                onClicked: root.triggerRename()
            }
            Button {
                objectName: "tagEditorCancelButton"
                text: "Cancel"
                enabled: !root.tagEditorApi.saving
                onClicked: root.requestClose()
            }
            Button {
                objectName: "tagEditorSaveButton"
                text: root.tagEditorApi.saving ? "Saving..." : "Save"
                enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving && root.tagEditorApi.dirty
                highlighted: true
                onClicked: root.triggerSave(false)
            }
        }
    }

    contentItem: RowLayout {
        spacing: 6

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
                && !tagEditorQuitWaitDialog.visible
            onActivated: root.requestClose()
        }

        Frame {
            id: tagEditorEditorPane
            Layout.preferredWidth: 332
            Layout.minimumWidth: 320
            Layout.fillHeight: true
            padding: 3
            background: Rectangle {
                radius: 4
                color: root.uiSurfaceColor
                border.color: root.uiBorderColor
            }

            contentItem: ScrollView {
                clip: true
                padding: 0
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                Column {
                    width: tagEditorEditorPane.availableWidth
                    spacing: 4

                    Repeater {
                        model: root.editorFields
                        delegate: Column {
                            required property var modelData
                            width: tagEditorEditorPane.availableWidth
                            spacing: 1

                            Row {
                                width: parent.width
                                spacing: 2

                                Label {
                                    text: modelData.label
                                    font.pixelSize: 11
                                    color: root.uiMutedTextColor
                                    width: parent.width - (caseButton.visible ? caseButton.width + parent.spacing : 0)
                                    verticalAlignment: Text.AlignVCenter
                                }

                                Button {
                                    id: caseButton
                                    visible: modelData.allowTitleCase || modelData.allowCapitalize
                                    text: "Aa"
                                    padding: 1
                                    implicitWidth: 22
                                    implicitHeight: 18
                                    onClicked: {
                                        const popupPosition = mapToItem(
                                            root.contentItem,
                                            0,
                                            height + 2)
                                        casePopup.x = popupPosition.x
                                        casePopup.y = popupPosition.y
                                        casePopup.open()
                                    }
                                }
                            }

                            TextField {
                                id: fieldEditor
                                objectName: "tagEditorField_" + modelData.key
                                width: parent.width
                                selectByMouse: true
                                persistentSelection: true
                                property bool touched: false
                                property var editRowsSnapshot: []

                                function captureEditRowsSnapshot() {
                                    editRowsSnapshot = root.tagEditorApi.selectedRows()
                                }

                                function syncFromController() {
                                    extraValuesNote.text = root.tagEditorApi.extraValuesNote(modelData.key)
                                    const value = root.tagEditorApi.bulkValue(modelData.key)
                                    const mixed = value === root.keepText
                                    if (activeFocus && touched) {
                                        return
                                    }
                                    text = mixed ? "" : value
                                    placeholderText = mixed ? root.keepText : ""
                                    editRowsSnapshot = []
                                    touched = false
                                }

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
                                        modelData.key,
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
                            }

                            Label {
                                id: extraValuesNote
                                objectName: "tagEditorExtraValuesNote_" + modelData.key
                                width: parent.width
                                visible: text.length > 0
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Kirigami.Theme.neutralTextColor
                            }

                            Popup {
                                id: casePopup
                                parent: root.contentItem
                                width: 128
                                padding: 6
                                z: 1000
                                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                                background: Rectangle {
                                    radius: 4
                                    color: root.uiSurfaceRaisedColor
                                    border.color: root.uiBorderColor
                                }

                                contentItem: ColumnLayout {
                                    spacing: 4

                                    Button {
                                        visible: modelData.allowTitleCase
                                        text: "Title Case"
                                        Layout.fillWidth: true
                                        onClicked: {
                                            root.tagEditorApi.applyEnglishTitleCase(modelData.key)
                                            fieldEditor.syncFromController()
                                            casePopup.close()
                                        }
                                    }

                                    Button {
                                        visible: modelData.allowCapitalize
                                        text: "Capital Case"
                                        Layout.fillWidth: true
                                        onClicked: {
                                            if (modelData.key === "genre") {
                                                root.tagEditorApi.applyGenreCapitalize()
                                            } else {
                                                root.tagEditorApi.applyFinnishCapitalize(modelData.key)
                                            }
                                            fieldEditor.syncFromController()
                                            casePopup.close()
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Frame {
            Layout.fillWidth: true
            Layout.fillHeight: true
            padding: 0
            background: Rectangle {
                radius: 4
                color: root.uiSurfaceColor
                border.color: root.uiBorderColor
            }

            // Only the visible rows have items, so thousands of rows open at
            // once. Selection is a role, so each row reads it once.
            contentItem: ListView {
                id: tagEditorTable
                objectName: "tagEditorTable"
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
                ScrollBar.vertical: ScrollBar {}
                ScrollBar.horizontal: ScrollBar {}
                Accessible.role: Accessible.Table
                Accessible.name: "Files"

                Keys.onPressed: function(event) { root.handleTableKey(event) }

                header: Rectangle {
                    z: 2
                    width: root.tableContentWidth
                    height: 26
                    color: root.uiColumnsColor

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        spacing: 6

                        Repeater {
                            model: root.tableColumns
                            delegate: Label {
                                required property var modelData
                                width: modelData.width
                                height: parent.height
                                text: modelData.label
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                verticalAlignment: Text.AlignVCenter
                                horizontalAlignment: Text.AlignLeft
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                delegate: Rectangle {
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
                    color: tableRow.selected
                        ? Qt.rgba(0.12, 0.44, 0.72, 0.14)
                        : (tableRow.index % 2 === 0
                            ? Qt.rgba(1, 1, 1, 0.02)
                            : "transparent")
                    border.color: tableRow.errorText.length > 0
                        ? Kirigami.Theme.negativeTextColor
                        : (tableRow.current
                            ? Kirigami.Theme.focusColor
                            : (tableRow.selected
                                ? Qt.rgba(0.12, 0.44, 0.72, 0.34)
                                : root.uiBorderColor))
                    border.width: tableRow.errorText.length > 0 || tableRow.selected || tableRow.current ? 1 : 0
                    Accessible.role: Accessible.Row
                    Accessible.name: tableRow.fileName
                    Accessible.selected: tableRow.selected

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        onClicked: function(mouse) {
                            root.handleRowClick(tableRow.index, mouse.modifiers)
                        }
                    }

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        spacing: 6

                        Repeater {
                            model: root.tableColumns
                            delegate: Label {
                                required property var modelData
                                width: modelData.width
                                height: parent.height
                                text: root.cellText(modelData.key, tableRow)
                                color: modelData.key === "status" && tableRow.errorText.length > 0
                                    ? Kirigami.Theme.negativeTextColor
                                    : Kirigami.Theme.textColor
                                verticalAlignment: Text.AlignVCenter
                                horizontalAlignment: Text.AlignLeft
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }
        }
    }
}
