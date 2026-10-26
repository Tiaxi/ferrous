// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import "../logic/ErrorUtils.js" as ErrorUtils

// The newest error in full, until the user dismisses it, and the history
// of recent errors below it when the user expands it.
Item {
    id: root

    required property var errorController
    required property var uiPalette
    // Viewers show the newest error only.
    property bool historyAvailable: true
    // Whether the banner may show at all, such as while its viewer is open.
    property bool active: true
    // The message showing the newest error, while one is shown.
    readonly property alias message: bannerLoader.item
    // The palette's colors; viewers made without one show no history.
    readonly property color paletteTextColor: root.uiPalette ? root.uiPalette.uiTextColor : "black"
    readonly property color paletteMutedTextColor: root.uiPalette
        ? root.uiPalette.uiMutedTextColor
        : "gray"
    readonly property color paletteSurfaceColor: root.uiPalette
        ? root.uiPalette.uiSurfaceColor
        : "transparent"
    readonly property color paletteSurfaceAltColor: root.uiPalette
        ? root.uiPalette.uiSurfaceAltColor
        : "transparent"
    readonly property color paletteBorderColor: root.uiPalette
        ? root.uiPalette.uiBorderColor
        : "transparent"
    readonly property int errorCount: root.errorController ? root.errorController.count : 0
    readonly property var entry: root.errorController ? root.errorController.latest : null
    readonly property bool bannerShown: root.active
        && root.errorController !== null
        && root.errorController.latestShown
        && root.entry !== null
    readonly property bool historyShown: root.active
        && root.historyAvailable
        && root.errorController !== null
        && root.errorController.historyExpanded
        && root.errorController.count > 0

    implicitWidth: content.implicitWidth
    implicitHeight: root.visible ? content.implicitHeight : 0
    visible: root.bannerShown || root.historyShown

    // Clicks on the errors stay here; viewers close on a click.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: function(wheel) { wheel.accepted = true }
    }

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 0

        // Built only while an error is shown: the message's actions build
        // their overflow menu in the background.
        Loader {
            id: bannerLoader
            Layout.fillWidth: true
            active: root.bannerShown
            visible: active
            sourceComponent: Kirigami.InlineMessage {
                objectName: "errorBanner"
                visible: true
                type: root.entry && root.entry.severity === ErrorUtils.SeverityWarning
                    ? Kirigami.MessageType.Warning
                    : Kirigami.MessageType.Error
                // Wraps; the whole message is shown however long it is.
                text: ErrorUtils.describe(root.entry)
                // The close button would hide the message for good, so
                // dismissing is an action that only hides the newest error.
                showCloseButton: false
                actions: [
                    Kirigami.Action {
                        text: root.historyShown
                            ? "Hide Recent Errors"
                            : "Recent Errors (" + root.errorCount + ")"
                        icon.name: "view-list-details"
                        visible: root.historyAvailable
                        onTriggered: root.errorController.toggleHistory()
                    },
                    Kirigami.Action {
                        text: "Dismiss"
                        icon.name: "dialog-close"
                        onTriggered: root.errorController.dismissLatest()
                    }
                ]
            }
        }

        Rectangle {
            id: historyPanel
            objectName: "errorHistory"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(
                historyHeader.implicitHeight + historyList.contentHeight + 16,
                Kirigami.Units.gridUnit * 14)
            visible: root.historyShown
            color: root.paletteSurfaceColor
            border.color: root.paletteBorderColor

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 4

                RowLayout {
                    id: historyHeader
                    Layout.fillWidth: true
                    spacing: 6

                    Kirigami.Heading {
                        Layout.fillWidth: true
                        level: 4
                        text: "Recent Errors"
                        color: root.paletteTextColor
                    }
                    Button {
                        objectName: "errorHistoryClearButton"
                        text: "Clear All"
                        onClicked: root.errorController.clear()
                    }
                    ToolButton {
                        icon.name: "window-close"
                        text: "Hide Recent Errors"
                        display: AbstractButton.IconOnly
                        Accessible.name: text
                        ToolTip.visible: hovered
                        ToolTip.text: text
                        onClicked: root.errorController.toggleHistory()
                    }
                }

                ListView {
                    id: historyList
                    objectName: "errorHistoryList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 2
                    // Builds rows only while the history is shown.
                model: root.historyShown ? root.errorController.entries : null
                    boundsBehavior: Flickable.StopAtBounds
                    Accessible.role: Accessible.List
                    Accessible.name: "Recent errors"
                    ScrollBar.vertical: ScrollBar {}

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

                        width: ListView.view.width
                        height: historyRowLayout.implicitHeight + 8
                        color: historyRow.index % 2 === 0
                            ? "transparent"
                            : root.paletteSurfaceAltColor
                        Accessible.role: Accessible.ListItem
                        Accessible.name: historyRow.heading + ", "
                            + ErrorUtils.timeText(historyRow.lastSeenMs) + ": " + historyRow.message

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
                                color: root.paletteMutedTextColor
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0

                                Label {
                                    Layout.fillWidth: true
                                    text: historyRow.heading
                                    font.weight: Font.DemiBold
                                    color: root.paletteTextColor
                                }
                                Label {
                                    objectName: "errorHistoryMessage"
                                    Layout.fillWidth: true
                                    text: historyRow.message
                                    wrapMode: Text.Wrap
                                    color: root.paletteTextColor
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: historyRow.subject.length > 0
                                    text: historyRow.subject
                                    elide: Text.ElideMiddle
                                    color: root.paletteMutedTextColor
                                }
                            }

                            ToolButton {
                                Layout.alignment: Qt.AlignTop
                                icon.name: "edit-delete-remove"
                                text: "Remove"
                                display: AbstractButton.IconOnly
                                Accessible.name: "Remove this error"
                                ToolTip.visible: hovered
                                ToolTip.text: "Remove"
                                onClicked: root.errorController.remove(historyRow.index)
                            }
                        }
                    }
                }
            }
        }
    }
}
