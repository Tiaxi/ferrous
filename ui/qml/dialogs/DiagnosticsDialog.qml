// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "." as Dialogs

Dialog {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var windowRoot
    required property int popupTransitionMs

    property string diagnosticsText: ""

    modal: true
    title: I18n.i18nc("@title:window", "Diagnostics")
    standardButtons: Dialog.Close
    width: Math.min(980, root.windowRoot.width - 80)
    height: Math.min(680, root.windowRoot.height - 80)
    enter: Components.PopupTransition { duration: root.popupTransitionMs }
    exit: Components.PopupTransition { duration: root.popupTransitionMs }

    function syncDiagnosticsText() {
        root.diagnosticsText = root.uiBridge.diagnosticsText || ""
    }

    onOpened: {
        root.uiBridge.reloadDiagnosticsFromDisk()
        syncDiagnosticsText()
    }

    onClosed: diagnosticsText = ""

    Connections {
        target: root.uiBridge

        function onDiagnosticsChanged() {
            root.syncDiagnosticsText()
        }
    }

    contentItem: ColumnLayout {
        spacing: 8

        RowLayout {
            Layout.fillWidth: true

            Label {
                text: I18n.i18nc("@label", "Log file:")
                color: Kirigami.Theme.disabledTextColor
            }

            TextField {
                Layout.fillWidth: true
                readOnly: true
                text: root.uiBridge.diagnosticsLogPath || ""
                selectByMouse: true
            }

            Button {
                text: I18n.i18nc("@action:button", "Open Folder")
                enabled: (root.uiBridge.diagnosticsLogPath || "").length > 0
                onClicked: root.uiBridge.openContainingFolder(root.uiBridge.diagnosticsLogPath || "")
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Button {
                text: I18n.i18nc("@action:button", "Reload")
                onClicked: {
                    root.uiBridge.reloadDiagnosticsFromDisk()
                    root.syncDiagnosticsText()
                }
            }

            Button {
                objectName: "diagnosticsClearButton"
                text: I18n.i18nc("@action:button", "Clear...")
                enabled: (root.diagnosticsText || "").length > 0
                onClicked: clearDiagnosticsConfirmDialog.open()
            }

            Item { Layout.fillWidth: true }

            Button {
                text: I18n.i18nc("@action:button", "Copy All")
                onClicked: {
                    if ((diagnosticsTextArea.text || "").length > 0) {
                        diagnosticsTextArea.selectAll()
                        diagnosticsTextArea.copy()
                    }
                }
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                id: diagnosticsTextArea
                text: root.diagnosticsText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.NoWrap
                font.family: "Monospace"
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
                    enter: Components.PopupTransition { duration: root.popupTransitionMs }
                    exit: Components.PopupTransition { duration: root.popupTransitionMs }

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
                }
            }
        }
    }

    // Clearing deletes the log file, so it asks first.
    Dialogs.ConfirmDialog {
        id: clearDiagnosticsConfirmDialog
        objectName: "clearDiagnosticsConfirmDialog"
        uiPalette: root.uiPalette
        windowRoot: root.windowRoot
        title: I18n.i18nc("@title:window", "Clear Diagnostics")
        message: I18n.i18nc("@info", "Delete the diagnostics log?")
        details: I18n.i18nc("@info", "Ferrous deletes the log file and starts a new one. Use Copy All first to keep "
            + "what it holds, for example for a bug report.")
        confirmText: I18n.i18nc("@action:button", "Delete Log")
        onConfirmed: {
            root.uiBridge.clearDiagnostics()
            root.syncDiagnosticsText()
        }
    }
}
