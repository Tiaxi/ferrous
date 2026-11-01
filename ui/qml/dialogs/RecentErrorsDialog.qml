// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/ErrorUtils.js" as ErrorUtils
import "../logic/KeyUtils.js" as KeyUtils

// The errors the app keeps, newest first. Opening it counts as seeing them.
Dialog {
    id: root

    required property var errorController
    required property var uiPalette
    required property var windowRoot
    required property int popupTransitionMs

    readonly property int errorCount: root.errorController.count
    readonly property real hostWidth: (parent && parent.width > 0) ? parent.width : root.windowRoot.width
    readonly property real hostHeight: (parent && parent.height > 0) ? parent.height : root.windowRoot.height

    parent: Overlay.overlay
    modal: true
    focus: true
    // Above the viewer popups.
    z: 100
    width: Math.min(Kirigami.Units.gridUnit * 38, root.hostWidth - Kirigami.Units.gridUnit * 2)
    height: Math.min(root.implicitHeight, Kirigami.Units.gridUnit * 32,
        root.hostHeight - Kirigami.Units.gridUnit * 2)
    x: Math.round((root.hostWidth - width) / 2)
    y: Math.round((root.hostHeight - height) / 2)
    title: I18n.i18nc("@title:window", "Recent Errors")
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape
    enter: Components.PopupTransition { duration: root.popupTransitionMs }
    exit: Components.PopupTransition { duration: root.popupTransitionMs }

    onAboutToShow: root.errorController.dismissLatest()
    // Over a viewer the keyboard would otherwise stay with the viewer.
    onOpened: historyView.forceActiveFocus(Qt.PopupFocusReason)
    // Errors that arrived while it was open have been seen too.
    onClosed: root.errorController.dismissLatest()
    onErrorCountChanged: {
        if (root.errorCount === 0) {
            root.close()
        }
    }

    // Keyboard focus moves on to the next row: the focused one is going.
    function removeEntry(index, hadFocus) {
        root.errorController.remove(index)
        if (hadFocus && root.errorCount > 0) {
            historyList.itemAt(Math.min(index, root.errorCount - 1)).focusRemoveButton()
        }
    }

    background: Rectangle {
        radius: 6
        color: root.uiPalette.uiPaneColor
        border.color: root.uiPalette.uiBorderColor
    }

    contentItem: ColumnLayout {
        spacing: 12

        // Space presses the focused button rather than playing or pausing:
        // the main window's shortcuts stay active under a fullscreen viewer.
        Keys.onShortcutOverride: function(event) {
            event.accepted = KeyUtils.isPlainSpace(event)
        }

        Components.KeyScrollView {
            id: historyView
            objectName: "errorHistoryView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            // Room for the focus frame around the rows.
            padding: 2
            Accessible.role: Accessible.List
            Accessible.name: I18n.i18nc("@label", "Recent errors")
            Accessible.description: I18n.i18nc("@info", "Scroll with the arrow and Page keys")
            Accessible.focusable: true

            Column {
                id: historyColumn
                width: historyView.availableWidth
                spacing: 2

                // The history is short: every row exists, so Tab reaches them all.
                Repeater {
                    id: historyList
                    objectName: "errorHistoryList"
                    // Builds rows only while the dialog is shown.
                    model: root.visible ? root.errorController.entries : null

                    delegate: Rectangle {
                        id: historyRow
                        required property int index
                        required property int category
                        required property int severity
                        required property string subject
                        required property string message
                        required property int count
                        required property double lastSeenMs
                        readonly property string heading: ErrorUtils.categoryLabel(historyRow.category)
                            + ErrorUtils.countSuffix(historyRow.count)

                        function focusRemoveButton() {
                            removeButton.forceActiveFocus(Qt.TabFocusReason)
                        }

                        width: historyColumn.width
                        height: historyRowLayout.implicitHeight + 8
                        color: historyRow.index % 2 === 0
                            ? "transparent"
                            : root.uiPalette.uiSurfaceAltColor
                        Accessible.role: Accessible.ListItem
                        Accessible.name: I18n.i18nc("@info error heading, time, message", "%1, %2: %3",
                            historyRow.heading, ErrorUtils.timeText(historyRow.lastSeenMs), historyRow.message)
                        Accessible.description: historyRow.subject

                        RowLayout {
                            id: historyRowLayout
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 4
                            anchors.rightMargin: 4
                            spacing: 8

                            Kirigami.Icon {
                                Layout.alignment: Qt.AlignTop
                                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                                source: historyRow.severity === ErrorUtils.SeverityWarning
                                    ? "dialog-warning"
                                    : "dialog-error"
                                Accessible.ignored: true
                            }

                            Label {
                                Layout.alignment: Qt.AlignTop
                                text: ErrorUtils.timeText(historyRow.lastSeenMs)
                                color: root.uiPalette.uiMutedTextColor
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0

                                Label {
                                    Layout.fillWidth: true
                                    text: historyRow.heading
                                    textFormat: Text.PlainText
                                    font.weight: Font.DemiBold
                                    color: root.uiPalette.uiTextColor
                                }
                                Label {
                                    objectName: "errorHistoryMessage"
                                    Layout.fillWidth: true
                                    text: historyRow.message
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    color: root.uiPalette.uiTextColor
                                }
                                Label {
                                    objectName: "errorHistorySubject"
                                    Layout.fillWidth: true
                                    visible: historyRow.subject.length > 0
                                    text: historyRow.subject
                                    textFormat: Text.PlainText
                                    elide: Text.ElideMiddle
                                    color: root.uiPalette.uiMutedTextColor
                                }
                            }

                            ToolButton {
                                id: removeButton
                                objectName: "errorHistoryRemoveButton"
                                Layout.alignment: Qt.AlignTop
                                icon.name: "edit-delete-remove"
                                text: I18n.i18nc("@action:button", "Remove")
                                display: AbstractButton.IconOnly
                                focusPolicy: Qt.StrongFocus
                                Accessible.name: I18n.i18nc("@action:button", "Remove this error")
                                ToolTip.visible: hovered
                                ToolTip.text: I18n.i18nc("@info:tooltip", "Remove")
                                onActiveFocusChanged: {
                                    if (activeFocus) {
                                        historyView.reveal(historyRow.y, historyRow.y + historyRow.height)
                                    }
                                }
                                onClicked: root.removeEntry(historyRow.index, removeButton.activeFocus)
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                objectName: "errorHistoryClearButton"
                text: I18n.i18nc("@action:button", "Clear All")
                onClicked: root.errorController.clear()
            }

            Item { Layout.fillWidth: true }

            Button {
                objectName: "errorHistoryCloseButton"
                text: I18n.i18nc("@action:button", "Close")
                onClicked: root.close()
            }
        }
    }
}
