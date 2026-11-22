// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "." as Dialogs

// Help › Diagnostics: the log of what went wrong, in a window of its own.
Components.DialogWindow {
    id: root

    required property var uiBridge

    property string diagnosticsText: ""

    objectName: "diagnosticsDialog"
    title: I18n.i18nc("@title:window", "Diagnostics")
    // As wide as the wider group of actions needs: a button is a little
    // wider while it is enabled, with its mnemonic, so the whole row would
    // move the minimum as the log fills or empties.
    minimumWidth: Math.ceil(Math.max(Kirigami.Units.gridUnit * 30, Math.max(diagnosticsLogActions.implicitWidth,
        diagnosticsReadActions.implicitWidth) + 2 * Kirigami.Units.gridUnit))
    minimumHeight: Kirigami.Units.gridUnit * 18
    // Wide enough for the actions in one row. Its size with the default
    // font grows with a larger one, within the screen, so the log shows as
    // many of its lines, as long.
    defaultWidth: Math.max(root.uiPalette.scaledSize(Kirigami.Units.gridUnit * 54),
        Math.ceil(diagnosticsToolbar.implicitWidth + 2 * Kirigami.Units.gridUnit))
    defaultHeight: root.uiPalette.scaledSize(Kirigami.Units.gridUnit * 38)

    function syncDiagnosticsText() {
        root.diagnosticsText = root.uiBridge.diagnosticsText || ""
    }

    onAboutToShow: {
        root.uiBridge.reloadDiagnosticsFromDisk()
        syncDiagnosticsText()
        diagnosticsLogView.forceActiveFocus(Qt.PopupFocusReason)
    }

    onClosed: diagnosticsText = ""

    footer: Components.DialogButtons {
        id: dialogButtons
        primary: diagnosticsCloseButton
        Button {
            id: diagnosticsCloseButton
            objectName: "diagnosticsCloseButton"
            text: I18n.i18nc("@action:button", "Close")
            icon.name: "window-close"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
    }

    Connections {
        target: root.uiBridge

        // Closed, it keeps no copy of the log: it takes it as it opens.
        function onDiagnosticsChanged() {
            if (root.visible) {
                root.syncDiagnosticsText()
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: Kirigami.Units.gridUnit
        anchors.rightMargin: Kirigami.Units.gridUnit
        anchors.topMargin: Kirigami.Units.gridUnit
        // The buttons have no room of their own above them.
        anchors.bottomMargin: Kirigami.Units.gridUnit
        spacing: Kirigami.Units.largeSpacing

        // The log's path on a line of its own, and under it every action in
        // one row (they were over two rows, with two ragged right
        // edges): the log's own on the left, reading it out on the right.
        RowLayout {
            objectName: "diagnosticsLogPathRow"
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Label {
                textFormat: Text.PlainText
                text: I18n.i18nc("@label", "Log file:")
                color: root.uiPalette.text
            }

            Components.PathLabel {
                objectName: "diagnosticsLogPath"
                Layout.fillWidth: true
                path: root.uiBridge.diagnosticsLogPath || ""
                color: root.uiPalette.textSecondary
            }
        }

        // In a window made too narrow for the row, the actions on the right
        // go under the others, at the same right edge. Bindings place them,
        // at once.
        Item {
            id: diagnosticsToolbar
            objectName: "diagnosticsToolbar"
            readonly property real groupGap: Kirigami.Units.gridUnit
            readonly property bool oneRow: diagnosticsLogActions.implicitWidth + groupGap
                + diagnosticsReadActions.implicitWidth <= width
            Layout.fillWidth: true
            implicitWidth: diagnosticsLogActions.implicitWidth + groupGap + diagnosticsReadActions.implicitWidth
            implicitHeight: oneRow
                ? Math.max(diagnosticsLogActions.implicitHeight, diagnosticsReadActions.implicitHeight)
                : diagnosticsLogActions.implicitHeight + Kirigami.Units.smallSpacing
                    + diagnosticsReadActions.implicitHeight

            // The log's own actions. The actions' glyphs are as large as the
            // footer's, which their captions' font calls for.
            RowLayout {
                id: diagnosticsLogActions
                width: implicitWidth
                height: implicitHeight

                Button {
                    objectName: "diagnosticsReloadButton"
                    icon.width: dialogButtons.glyphSize
                    icon.height: dialogButtons.glyphSize
                    icon.name: "view-refresh"
                    text: I18n.i18nc("@action:button", "Reload")
                    onClicked: {
                        root.uiBridge.reloadDiagnosticsFromDisk()
                        root.syncDiagnosticsText()
                    }
                }

                // One verb for one act: it deletes the log, as its question
                // and its confirming button say (it was Clear…).
                // No ellipsis: its confirmation asks for nothing more, as
                // KDE's guidelines have it.
                Button {
                    objectName: "diagnosticsClearButton"
                    icon.width: dialogButtons.glyphSize
                    icon.height: dialogButtons.glyphSize
                    icon.name: "edit-delete"
                    text: I18n.i18nc("@action:button", "Delete Log")
                    enabled: (root.diagnosticsText || "").length > 0
                    onClicked: clearDiagnosticsConfirmDialog.open()
                }
            }

            // Reading it out.
            RowLayout {
                id: diagnosticsReadActions
                x: diagnosticsToolbar.width - width
                y: diagnosticsToolbar.oneRow ? 0 : diagnosticsLogActions.height + Kirigami.Units.smallSpacing
                width: implicitWidth
                height: implicitHeight

                Button {
                    objectName: "diagnosticsOpenFolderButton"
                    icon.width: dialogButtons.glyphSize
                    icon.height: dialogButtons.glyphSize
                    icon.name: "document-open-folder"
                    text: I18n.i18nc("@action:button", "Open Folder")
                    enabled: (root.uiBridge.diagnosticsLogPath || "").length > 0
                    onClicked: root.uiBridge.openContainingFolder(root.uiBridge.diagnosticsLogPath || "")
                }

                Button {
                    objectName: "diagnosticsCopyAllButton"
                    icon.width: dialogButtons.glyphSize
                    icon.height: dialogButtons.glyphSize
                    icon.name: "edit-copy"
                    text: I18n.i18nc("@action:button", "Copy With Paths")
                    onClicked: {
                        if (diagnosticsLogView.length > 0) {
                            diagnosticsLogView.selectAll()
                            diagnosticsLogView.copy()
                        }
                    }
                }

                Button {
                    objectName: "diagnosticsCopyWithoutPathsButton"
                    icon.width: dialogButtons.glyphSize
                    icon.height: dialogButtons.glyphSize
                    icon.name: "edit-copy"
                    text: I18n.i18nc("@action:button", "Copy Without Paths")
                    enabled: (root.diagnosticsText || "").length > 0
                    ToolTip.visible: hovered
                    ToolTip.delay: Kirigami.Units.toolTipDelay
                    ToolTip.text: I18n.i18nc("@info:tooltip",
                        "Copy the log with folders and file names replaced, for a bug report. Artist and track "
                        + "names in the Last.fm lines are kept.")
                    onClicked: root.uiBridge.copyDiagnosticsWithoutPaths()
                }
            }
        }

        // The log on the view colour in a frame, its scroll bar inside the
        // frame, as the tag editor's table. Its records wrap (#735), so it
        // scrolls one way. The frame takes the focus colour
        // while the log has the focus, as a text area's.
        Rectangle {
            objectName: "diagnosticsLogFrame"
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.uiPalette.view
            radius: 3
            border.width: 1
            border.color: diagnosticsLogView.activeFocus
                ? Kirigami.Theme.focusColor
                : Kirigami.ColorUtils.linearInterpolation(
                    root.uiPalette.view, root.uiPalette.text, Kirigami.Theme.frameContrast)

            ScrollView {
                id: diagnosticsScrollView
                objectName: "diagnosticsScrollView"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                // The scroll bar's lane is kept while there is none, so the
                // records don't wrap again as it comes and goes.
                leftPadding: mirrored ? ScrollBar.vertical.width : 0
                rightPadding: mirrored ? 0 : ScrollBar.vertical.width
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                // Holds no text: it is as large as the log, and the view
                // over it draws the part that shows (#724).
                Flickable {
                    id: diagnosticsFlickable
                    contentWidth: width
                    contentHeight: diagnosticsLogView.contentHeight
                    boundsBehavior: Flickable.StopAtBounds
                    flickableDirection: Flickable.VerticalFlick

                    DiagnosticsLogView {
                        id: diagnosticsLogView
                        objectName: "diagnosticsLogView"
                        parent: diagnosticsFlickable
                        width: diagnosticsFlickable.width
                        height: diagnosticsFlickable.height
                        contentY: diagnosticsFlickable.contentY
                        focus: true
                        Kirigami.Theme.colorSet: Kirigami.Theme.View
                        Kirigami.Theme.inherit: false
                        text: root.diagnosticsText
                        // As a control has it: what the monospaced font
                        // leaves open, such as its size, is the window's.
                        font: diagnosticsLogPlaceholder.font
                        color: Kirigami.Theme.textColor
                        selectionColor: Kirigami.Theme.highlightColor
                        selectedTextColor: Kirigami.Theme.highlightedTextColor
                        // The frame round the view is the log's; the text
                        // stands 8 px clear of its sides and 6 px of its top
                        // and bottom (at 5 px it was nearly on the frame).
                        leftPadding: Kirigami.Units.largeSpacing
                        rightPadding: Kirigami.Units.largeSpacing
                        topPadding: Kirigami.Units.smallSpacing + 2
                        bottomPadding: Kirigami.Units.smallSpacing + 2
                        Accessible.role: Accessible.EditableText
                        // Qt Quick reads it out from text and cursorPosition.
                        Accessible.name: I18n.i18nc("@label accessible name of the diagnostics log's text", "Log")
                        Accessible.multiLine: true
                        Accessible.focusable: true

                        onScrollRequested: function(y) {
                            diagnosticsFlickable.contentY = y
                        }
                        Keys.onMenuPressed: diagnosticsContextMenu.popupAt(
                            diagnosticsContextArea, diagnosticsLogView.leftPadding, diagnosticsLogView.topPadding)

                        Label {
                            id: diagnosticsLogPlaceholder
                            objectName: "diagnosticsLogPlaceholder"
                            x: diagnosticsLogView.leftPadding
                            y: diagnosticsLogView.topPadding
                            width: diagnosticsLogView.width - x - diagnosticsLogView.rightPadding
                            visible: diagnosticsLogView.length === 0
                            textFormat: Text.PlainText
                            // The bridge reads the log off the GUI thread.
                            text: (root.uiBridge.diagnosticsLoading || false)
                                ? I18n.i18nc("@info:placeholder", "Loading the log\u2026")
                                : ""
                            font: Kirigami.Theme.fixedWidthFont
                            color: root.uiPalette.placeholderText
                            wrapMode: Text.Wrap
                        }

                        MouseArea {
                            id: diagnosticsContextArea
                            anchors.fill: parent
                            acceptedButtons: Qt.RightButton
                            cursorShape: Qt.IBeamCursor

                            onPressed: diagnosticsLogView.forceActiveFocus(Qt.MouseFocusReason)
                            onClicked: function(mouse) {
                                diagnosticsContextMenu.popupAt(diagnosticsContextArea, mouse.x, mouse.y)
                            }
                        }

                        Components.AppMenu {
                            id: diagnosticsContextMenu

                            MenuItem {
                                text: I18n.i18nc("@action:inmenu", "Copy")
                                enabled: diagnosticsLogView.hasSelection
                                onTriggered: diagnosticsLogView.copy()
                            }

                            MenuItem {
                                text: I18n.i18nc("@action:inmenu", "Select All")
                                enabled: diagnosticsLogView.length > 0
                                onTriggered: diagnosticsLogView.selectAll()
                            }

                            MenuItem {
                                objectName: "diagnosticsCopyWithPathsMenuItem"
                                text: I18n.i18nc("@action:inmenu", "Copy With Paths")
                                enabled: diagnosticsLogView.length > 0
                                onTriggered: {
                                    diagnosticsLogView.selectAll()
                                    diagnosticsLogView.copy()
                                }
                            }

                            MenuItem {
                                objectName: "diagnosticsCopyWithoutPathsMenuItem"
                                text: I18n.i18nc("@action:inmenu", "Copy Without Paths")
                                enabled: diagnosticsLogView.length > 0
                                onTriggered: root.uiBridge.copyDiagnosticsWithoutPaths()
                            }
                        }
                    }
                }
            }
        }
    }

    // Deleting the log asks first.
    Dialogs.ConfirmDialog {
        id: clearDiagnosticsConfirmDialog
        objectName: "clearDiagnosticsConfirmDialog"
        uiPalette: root.uiPalette
        windowRoot: root
        title: I18n.i18nc("@title:window", "Delete Diagnostics Log")
        message: I18n.i18nc("@info", "Delete the diagnostics log?")
        // For a bug report, the copy made for sharing: Copy With Paths,
        // which it named, copies every path (#547).
        details: I18n.i18nc("@info", "Ferrous deletes the log file and starts a new one. To keep what it holds "
            + "for a bug report, use Copy Without Paths first.")
        confirmText: I18n.i18nc("@action:button", "Delete Log")
        confirmIcon: "edit-delete"
        destructive: true
        onConfirmed: {
            root.uiBridge.clearDiagnostics()
            root.syncDiagnosticsText()
        }
    }
}
