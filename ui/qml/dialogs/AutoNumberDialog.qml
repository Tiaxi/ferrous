import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Dialog {
    id: root

    required property var tagEditorApi
    required property var uiPalette
    required property var windowRoot

    modal: true
    x: Math.round((root.windowRoot.width - width) / 2)
    y: Math.round((root.windowRoot.height - height) / 2)
    width: 420
    title: "Auto Number"
    standardButtons: Dialog.NoButton

    background: Rectangle {
        radius: 16
        color: root.uiPalette.uiSurfaceRaisedColor
        border.color: root.uiPalette.uiBorderColor
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: root.uiPalette.uiMutedTextColor
            text: "Number checked rows, or all rows if none are checked, using the current table order."
        }

        GridLayout {
            columns: 2
            columnSpacing: 10
            rowSpacing: 8
            Layout.fillWidth: true

            Label { text: "Starting Track" }
            TextField {
                id: startingTrackField
                Layout.fillWidth: true
                text: "1"
                inputMethodHints: Qt.ImhDigitsOnly
            }

            Label { text: "Starting Disc" }
            TextField {
                id: startingDiscField
                Layout.fillWidth: true
                text: "1"
                enabled: writeDiscCheck.checked
                inputMethodHints: Qt.ImhDigitsOnly
            }
        }

        CheckBox {
            id: writeDiscCheck
            checked: false
            text: "Write disc numbers"
        }
        CheckBox {
            id: writeTotalsCheck
            checked: false
            text: "Write totals"
        }
        CheckBox {
            id: resetOnFolderCheck
            checked: false
            text: "Reset track numbers on folder or section change"
        }
        CheckBox {
            id: resetOnDiscCheck
            checked: false
            enabled: writeDiscCheck.checked
            text: "Reset track numbers when disc changes in current values"
        }

        RowLayout {
            Layout.fillWidth: true

            Item { Layout.fillWidth: true }

            Button {
                text: "Cancel"
                onClicked: root.close()
            }
            Button {
                text: "Apply"
                onClicked: {
                    root.tagEditorApi.autoNumber(
                        Number(startingTrackField.text || "1"),
                        Number(startingDiscField.text || "1"),
                        writeDiscCheck.checked,
                        writeTotalsCheck.checked,
                        resetOnFolderCheck.checked,
                        resetOnDiscCheck.checked)
                    root.close()
                }
            }
        }
    }
}
