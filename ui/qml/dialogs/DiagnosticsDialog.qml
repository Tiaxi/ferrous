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
    // Wide enough for the actions in one row.
    defaultWidth: Math.max(Kirigami.Units.gridUnit * 54,
        Math.ceil(diagnosticsToolbar.implicitWidth + 2 * Kirigami.Units.gridUnit))
    defaultHeight: Kirigami.Units.gridUnit * 38

    function syncDiagnosticsText() {
        root.diagnosticsText = root.uiBridge.diagnosticsText || ""
    }

    onAboutToShow: {
        root.uiBridge.reloadDiagnosticsFromDisk()
        syncDiagnosticsText()
        diagnosticsTextArea.forceActiveFocus(Qt.PopupFocusReason)
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

        function onDiagnosticsChanged() {
            root.syncDiagnosticsText()
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
        // one row (#373 H38: they were over two rows, with two ragged right
        // edges): the log's own on the left, reading it out on the right.
        RowLayout {
            objectName: "diagnosticsLogPathRow"
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Label {
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
            // footer's, which their captions' font calls for (#373 H43).
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
                // and its confirming button say (#373 H38: it was Clear…).
                Button {
                    objectName: "diagnosticsClearButton"
                    icon.width: dialogButtons.glyphSize
                    icon.height: dialogButtons.glyphSize
                    icon.name: "edit-delete"
                    text: I18n.i18nc("@action:button", "Delete Log\u2026")
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
                    text: I18n.i18nc("@action:button", "Copy All")
                    onClicked: {
                        if ((diagnosticsTextArea.text || "").length > 0) {
                            diagnosticsTextArea.selectAll()
                            diagnosticsTextArea.copy()
                        }
                    }
                }

                Button {
                    objectName: "diagnosticsCopyWithoutPathsButton"
                    icon.width: dialogButtons.glyphSize
                    icon.height: dialogButtons.glyphSize
                    icon.name: "edit-copy"
                    text: I18n.i18nc("@action:button", "Copy Log Without Paths")
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

        // The log on the view colour in a frame, its scroll bars inside the
        // frame, as the tag editor's table (#373 H38: the horizontal one ran
        // under the frame, outside it). The frame takes the focus colour
        // while the log has the focus, as a text area's.
        Rectangle {
            objectName: "diagnosticsLogFrame"
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.uiPalette.view
            radius: 3
            border.width: 1
            border.color: diagnosticsTextArea.activeFocus
                ? Kirigami.Theme.focusColor
                : Kirigami.ColorUtils.linearInterpolation(
                    root.uiPalette.view, root.uiPalette.text, Kirigami.Theme.frameContrast)

            ScrollView {
                id: diagnosticsScrollView
                objectName: "diagnosticsScrollView"
                anchors.fill: parent
                anchors.margins: 1
                clip: true

                TextArea {
                    id: diagnosticsTextArea
                    objectName: "diagnosticsTextArea"
                    text: root.diagnosticsText
                    // The bridge reads the log off the GUI thread.
                    placeholderText: (root.uiBridge.diagnosticsLoading || false)
                        ? I18n.i18nc("@info:placeholder", "Loading the log\u2026")
                        : ""
                    placeholderTextColor: root.uiPalette.placeholderText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.NoWrap
                    font: Kirigami.Theme.fixedWidthFont
                    persistentSelection: true
                    // The frame round the view is the log's; the text
                    // stands 8 px clear of its sides and 6 px of its top
                    // and bottom (#373 H42: 5 px, nearly on the frame).
                    background: null
                    leftPadding: Kirigami.Units.largeSpacing
                    rightPadding: Kirigami.Units.largeSpacing
                    topPadding: Kirigami.Units.smallSpacing + 2
                    bottomPadding: Kirigami.Units.smallSpacing + 2

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.RightButton
                        propagateComposedEvents: true
                        cursorShape: Qt.IBeamCursor

                        onPressed: function(mouse) {
                            if (mouse.button !== Qt.RightButton) {
                                mouse.accepted = false
                            }
                        }

                        onClicked: function(mouse) {
                            if (mouse.button === Qt.RightButton) {
                                diagnosticsTextArea.forceActiveFocus()
                                diagnosticsContextMenu.popup()
                            } else {
                                mouse.accepted = false
                            }
                        }
                    }

                    Menu {
                        id: diagnosticsContextMenu

                        MenuItem {
                            text: I18n.i18nc("@action:inmenu", "Copy")
                            enabled: (diagnosticsTextArea.selectedText || "").length > 0
                            onTriggered: diagnosticsTextArea.copy()
                        }

                        MenuItem {
                            text: I18n.i18nc("@action:inmenu", "Select All")
                            enabled: (diagnosticsTextArea.text || "").length > 0
                            onTriggered: diagnosticsTextArea.selectAll()
                        }

                        MenuItem {
                            text: I18n.i18nc("@action:inmenu", "Copy All")
                            enabled: (diagnosticsTextArea.text || "").length > 0
                            onTriggered: {
                                diagnosticsTextArea.selectAll()
                                diagnosticsTextArea.copy()
                            }
                        }

                        MenuItem {
                            text: I18n.i18nc("@action:inmenu", "Copy Log Without Paths")
                            enabled: (diagnosticsTextArea.text || "").length > 0
                            onTriggered: root.uiBridge.copyDiagnosticsWithoutPaths()
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
        details: I18n.i18nc("@info", "Ferrous deletes the log file and starts a new one. Use Copy All first to keep "
            + "what it holds, for example for a bug report.")
        confirmText: I18n.i18nc("@action:button", "Delete Log")
        confirmIcon: "edit-delete"
        destructive: true
        onConfirmed: {
            root.uiBridge.clearDiagnostics()
            root.syncDiagnosticsText()
        }
    }
}
