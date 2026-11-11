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
    minimumWidth: Math.ceil(Math.max(Kirigami.Units.gridUnit * 30, diagnosticsToolbar.implicitWidth
        + 2 * Kirigami.Units.gridUnit))
    minimumHeight: Kirigami.Units.gridUnit * 18
    defaultWidth: Kirigami.Units.gridUnit * 54
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
        spacing: Kirigami.Units.largeSpacing

        RowLayout {
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

            Button {
                icon.name: "document-open-folder"
                text: I18n.i18nc("@action:button", "Open Folder")
                enabled: (root.uiBridge.diagnosticsLogPath || "").length > 0
                onClicked: root.uiBridge.openContainingFolder(root.uiBridge.diagnosticsLogPath || "")
            }
        }

        RowLayout {
            id: diagnosticsToolbar
            Layout.fillWidth: true

            Button {
                icon.name: "view-refresh"
                text: I18n.i18nc("@action:button", "Reload")
                onClicked: {
                    root.uiBridge.reloadDiagnosticsFromDisk()
                    root.syncDiagnosticsText()
                }
            }

            Button {
                objectName: "diagnosticsClearButton"
                icon.name: "edit-clear-history"
                text: I18n.i18nc("@action:button", "Clear\u2026")
                enabled: (root.diagnosticsText || "").length > 0
                onClicked: clearDiagnosticsConfirmDialog.open()
            }

            Item { Layout.fillWidth: true }

            Button {
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

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                id: diagnosticsTextArea
                objectName: "diagnosticsTextArea"
                text: root.diagnosticsText
                // The bridge reads the log off the GUI thread.
                placeholderText: (root.uiBridge.diagnosticsLoading || false)
                    ? I18n.i18nc("@info:placeholder", "Loading the log\u2026")
                    : ""
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.NoWrap
                font: Kirigami.Theme.fixedWidthFont
                persistentSelection: true

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

    // Clearing deletes the log file, so it asks first.
    Dialogs.ConfirmDialog {
        id: clearDiagnosticsConfirmDialog
        objectName: "clearDiagnosticsConfirmDialog"
        uiPalette: root.uiPalette
        windowRoot: root
        title: I18n.i18nc("@title:window", "Clear Diagnostics")
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
