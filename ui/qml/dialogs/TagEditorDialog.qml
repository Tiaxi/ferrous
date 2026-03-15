import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import "." as Dialogs

Dialog {
    id: root

    required property var tagEditorApi
    required property var uiPalette
    required property var windowRoot
    required property var basenameFromPath

    property var selectedRows: []
    property int selectionAnchor: -1
    property int totalRows: 0
    property bool operationInFlight: false
    property string operationText: ""
    readonly property string keepText: "<keep>"
    readonly property color uiSurfaceColor: root.uiPalette.uiSurfaceColor
    readonly property color uiSurfaceRaisedColor: root.uiPalette.uiSurfaceRaisedColor
    readonly property color uiHeaderColor: root.uiPalette.uiHeaderColor
    readonly property color uiColumnsColor: root.uiPalette.uiColumnsColor
    readonly property color uiBorderColor: root.uiPalette.uiBorderColor
    readonly property color uiMutedTextColor: root.uiPalette.uiMutedTextColor
    readonly property bool statusHasFailure: root.tagEditorApi.statusText.toLowerCase().indexOf("failed") >= 0
        || root.tagEditorApi.statusText.toLowerCase().indexOf("error") >= 0
        || root.tagEditorApi.statusDetails.indexOf("Failed:") >= 0
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

    function closeEditor() {
        root.tagEditorApi.close()
        root.close()
    }

    function commitPendingEditorEdits() {
        tagEditorListFocusSink.forceActiveFocus()
    }

    function triggerSave(closeAfterSuccess) {
        if (root.tagEditorApi.loading || root.tagEditorApi.saving || !root.tagEditorApi.dirty) {
            return
        }
        root.commitPendingEditorEdits()
        root.operationInFlight = true
        root.operationText = "Saving tags..."
        Qt.callLater(function() {
            const ok = root.tagEditorApi.save()
            root.operationInFlight = false
            root.operationText = ""
            if (ok && closeAfterSuccess) {
                tagEditorCloseConfirmDialog.close()
                root.closeEditor()
            }
            tagEditorStatusFlash.restart()
        })
    }

    function triggerRename() {
        if (root.tagEditorApi.loading || root.tagEditorApi.saving || root.selectedRows.length === 0) {
            return
        }
        root.commitPendingEditorEdits()
        root.operationInFlight = true
        root.operationText = "Renaming files..."
        Qt.callLater(function() {
            root.tagEditorApi.renameSelectedFiles()
            root.operationInFlight = false
            root.operationText = ""
            tagEditorStatusFlash.restart()
        })
    }

    function requestClose() {
        if (root.tagEditorApi.loading || root.tagEditorApi.saving) {
            return
        }
        if (root.tagEditorApi.dirty) {
            tagEditorCloseConfirmDialog.open()
            return
        }
        root.closeEditor()
    }

    function saveAndClose() {
        if (root.tagEditorApi.loading || root.tagEditorApi.saving) {
            return
        }
        root.triggerSave(true)
    }

    function normalizedSelection(rows) {
        const next = []
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            if (row < 0 || row >= root.totalRows || next.indexOf(row) >= 0) {
                continue
            }
            next.push(row)
        }
        next.sort(function(a, b) { return a - b })
        return next
    }

    function isSelected(row) {
        return root.selectedRows.indexOf(row) >= 0
    }

    function updateSelection(rows, anchor) {
        root.selectedRows = root.normalizedSelection(rows)
        root.selectionAnchor = anchor
        root.tagEditorApi.setSelectedRows(root.selectedRows)
    }

    function selectOnlyRow(row) {
        root.updateSelection([row], row)
    }

    function toggleRow(row) {
        let next = root.selectedRows.slice()
        const existing = next.indexOf(row)
        if (existing >= 0) {
            next.splice(existing, 1)
        } else {
            next.push(row)
        }
        root.updateSelection(next, row)
    }

    function selectRange(row) {
        if (root.selectionAnchor < 0) {
            root.selectOnlyRow(row)
            return
        }
        const start = Math.min(root.selectionAnchor, row)
        const end = Math.max(root.selectionAnchor, row)
        const next = []
        for (let current = start; current <= end; ++current) {
            next.push(current)
        }
        root.updateSelection(next, root.selectionAnchor)
    }

    function handleRowClick(row, modifiers) {
        tagEditorListFocusSink.forceActiveFocus()
        const ctrl = (modifiers & Qt.ControlModifier) !== 0
        const shift = (modifiers & Qt.ShiftModifier) !== 0
        if (shift) {
            root.selectRange(row)
            return
        }
        if (ctrl) {
            root.toggleRow(row)
            return
        }
        root.selectOnlyRow(row)
    }

    function initializeSelection() {
        root.totalRows = root.tagEditorApi.loadedPaths().length
        if (root.totalRows > 0 && root.selectedRows.length === 0) {
            root.selectOnlyRow(0)
        }
    }

    function clearSelection() {
        root.selectedRows = []
        root.selectionAnchor = -1
        root.tagEditorApi.setSelectedRows([])
    }

    function selectAllRows() {
        const next = []
        for (let row = 0; row < root.totalRows; ++row) {
            next.push(row)
        }
        root.updateSelection(next, root.totalRows > 0 ? 0 : -1)
    }

    function rowText(columnKey, rowData) {
        switch (columnKey) {
        case "fileName": return rowData.fileName || ""
        case "directory": return root.basenameFromPath(rowData.directory || "")
        case "formatKind": return rowData.formatKind || ""
        case "title": return rowData.title || ""
        case "artist": return rowData.artist || ""
        case "album": return rowData.album || ""
        case "albumArtist": return rowData.albumArtist || ""
        case "genre": return rowData.genre || ""
        case "year": return rowData.year || ""
        case "trackNo": return rowData.trackNo || ""
        case "discNo": return rowData.discNo || ""
        case "status":
            return rowData.errorText && rowData.errorText.length > 0
                ? rowData.errorText
                : (rowData.dirty ? "Modified" : "")
        default:
            return ""
        }
    }

    onOpened: root.initializeSelection()
    onClosed: {
        root.clearSelection()
        root.totalRows = 0
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
        closeEditor: root.closeEditor
        saveAndClose: root.saveAndClose
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
                            ? root.operationText
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
                text: root.selectedRows.length > 0
                    ? root.selectedRows.length + " selected"
                    : root.totalRows + " loaded"
                color: root.tagEditorApi.dirty ? Kirigami.Theme.negativeTextColor : root.uiMutedTextColor
                font.weight: Font.DemiBold
            }

            Button {
                text: "\u00d7"
                enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving
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
                enabled: root.selectedRows.length > 0
                onClicked: root.clearSelection()
            }

            Item { Layout.fillWidth: true }

            Button {
                text: "Reload"
                enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving
                onClicked: {
                    root.clearSelection()
                    root.tagEditorApi.reload()
                    root.initializeSelection()
                }
            }
            Button {
                text: "Auto Number"
                enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving
                onClicked: autoNumberDialog.open()
            }
            Button {
                text: "Rename Files"
                enabled: !root.tagEditorApi.loading && !root.tagEditorApi.saving && root.selectedRows.length > 0
                onClicked: root.triggerRename()
            }
            Button {
                text: "Cancel"
                onClicked: root.closeEditor()
            }
            Button {
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
            onActivated: root.requestClose()
        }

        FocusScope {
            id: tagEditorListFocusSink
            visible: false
            width: 0
            height: 0
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
                                width: parent.width
                                selectByMouse: true
                                persistentSelection: true
                                property bool touched: false
                                property var editRowsSnapshot: []

                                function captureEditRowsSnapshot() {
                                    editRowsSnapshot = root.selectedRows.slice()
                                }

                                function syncFromController() {
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

            contentItem: ScrollView {
                clip: true

                Flickable {
                    id: tagEditorTableFlick
                    contentWidth: root.tableContentWidth
                    contentHeight: tagEditorTableColumn.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds

                    Column {
                        id: tagEditorTableColumn
                        width: tagEditorTableFlick.contentWidth
                        spacing: 0

                        Rectangle {
                            width: parent.width
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

                        Repeater {
                            model: root.tagEditorApi.tableModel
                            delegate: Rectangle {
                                required property int index
                                required property string path
                                required property string fileName
                                required property string directory
                                required property string formatKind
                                required property string title
                                required property string artist
                                required property string album
                                required property string albumArtist
                                required property string genre
                                required property string year
                                required property string trackNo
                                required property string discNo
                                required property string totalTracks
                                required property string totalDiscs
                                required property string comment
                                required property bool dirty
                                required property string errorText

                                width: tagEditorTableColumn.width
                                height: 24
                                color: root.isSelected(index)
                                    ? Qt.rgba(0.12, 0.44, 0.72, 0.14)
                                    : (index % 2 === 0
                                        ? Qt.rgba(1, 1, 1, 0.02)
                                        : "transparent")
                                border.color: errorText.length > 0
                                    ? Kirigami.Theme.negativeTextColor
                                    : (root.isSelected(index)
                                        ? Qt.rgba(0.12, 0.44, 0.72, 0.34)
                                        : root.uiBorderColor)
                                border.width: errorText.length > 0 || root.isSelected(index) ? 1 : 0

                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton
                                    onClicked: function(mouse) {
                                        root.handleRowClick(index, mouse.modifiers)
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
                                            text: root.rowText(modelData.key, {
                                                fileName: fileName,
                                                directory: directory,
                                                formatKind: formatKind,
                                                title: title,
                                                artist: artist,
                                                album: album,
                                                albumArtist: albumArtist,
                                                genre: genre,
                                                year: year,
                                                trackNo: trackNo,
                                                discNo: discNo,
                                                dirty: dirty,
                                                errorText: errorText
                                            })
                                            color: modelData.key === "status" && errorText.length > 0
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
        }
    }
}
