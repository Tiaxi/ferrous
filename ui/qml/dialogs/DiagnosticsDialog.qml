// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import "../components" as Components

Dialog {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var windowRoot
    required property int popupTransitionMs

    property string diagnosticsText: ""

    modal: true
    title: "Diagnostics"
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
                text: "Log file:"
                color: Kirigami.Theme.disabledTextColor
            }

            TextField {
                Layout.fillWidth: true
                readOnly: true
                text: root.uiBridge.diagnosticsLogPath || ""
                selectByMouse: true
            }

            Button {
                text: "Open Folder"
                enabled: (root.uiBridge.diagnosticsLogPath || "").length > 0
                onClicked: root.uiBridge.openContainingFolder(root.uiBridge.diagnosticsLogPath || "")
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Button {
                text: "Reload"
                onClicked: {
                    root.uiBridge.reloadDiagnosticsFromDisk()
                    root.syncDiagnosticsText()
                }
            }

            Button {
                text: "Clear"
                onClicked: {
                    root.uiBridge.clearDiagnostics()
                    root.syncDiagnosticsText()
                }
            }

            Item { Layout.fillWidth: true }

            Button {
                text: "Copy All"
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
                        text: "Copy"
                        enabled: (diagnosticsTextArea.selectedText || "").length > 0
                        onTriggered: diagnosticsTextArea.copy()
                    }

                    MenuItem {
                        text: "Select All"
                        enabled: (diagnosticsTextArea.text || "").length > 0
                        onTriggered: diagnosticsTextArea.selectAll()
                    }

                    MenuItem {
                        text: "Copy All"
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
}
