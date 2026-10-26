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
    // The message showing the newest error.
    readonly property alias message: banner
    // How many lines of the newest error show before its text scrolls.
    property int maximumLines: 4
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

        // The newest error, styled as Kirigami's inline messages. Its text
        // scrolls past a few lines, so a long error cannot push the window's
        // controls, or its own buttons, out of reach.
        Rectangle {
            id: banner
            objectName: "errorBanner"
            readonly property bool warning: root.entry !== null
                && root.entry.severity === ErrorUtils.SeverityWarning
            readonly property string text: ErrorUtils.describe(root.entry)
            readonly property color accentColor: banner.warning
                ? Kirigami.Theme.neutralTextColor
                : Kirigami.Theme.negativeTextColor

            Layout.fillWidth: true
            Layout.preferredHeight: bannerRow.implicitHeight + 2 * bannerRow.anchors.margins
            visible: root.bannerShown
            radius: Kirigami.Units.cornerRadius
            color: banner.warning
                ? Kirigami.Theme.neutralBackgroundColor
                : Kirigami.Theme.negativeBackgroundColor
            border.color: banner.accentColor
            Accessible.role: Accessible.AlertMessage
            Accessible.name: (banner.warning ? "Warning: " : "Error: ") + banner.text

            RowLayout {
                id: bannerRow
                anchors.fill: parent
                anchors.margins: Kirigami.Units.smallSpacing * 2
                spacing: Kirigami.Units.largeSpacing

                Kirigami.Icon {
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                    source: banner.warning ? "dialog-warning" : "dialog-error"
                    color: banner.accentColor
                    Accessible.ignored: true
                }

                ScrollView {
                    id: bannerBody
                    objectName: "errorBannerBody"
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredHeight: Math.min(
                        bannerText.implicitHeight, bannerMetrics.lineSpacing * root.maximumLines)
                    contentWidth: availableWidth
                    clip: true
                    // Tab reaches the text, and the arrow and page keys scroll
                    // it. Other keys, such as a viewer's Escape, go on.
                    activeFocusOnTab: true
                    focusPolicy: Qt.StrongFocus
                    Accessible.role: Accessible.StaticText
                    Accessible.name: banner.text
                    Accessible.description: "Scroll with the arrow and Page keys"
                    Accessible.focusable: true

                    background: Rectangle {
                        color: "transparent"
                        radius: Kirigami.Units.cornerRadius
                        border.width: 2
                        border.color: bannerBody.activeFocus
                            ? Kirigami.Theme.focusColor
                            : "transparent"
                    }

                    function scrollBy(pixels) {
                        const flickable = bannerBody.contentItem
                        const bottom = Math.max(0, flickable.contentHeight - flickable.height)
                        flickable.contentY = Math.max(0, Math.min(bottom, flickable.contentY + pixels))
                    }

                    Keys.onPressed: function(event) {
                        const line = bannerMetrics.lineSpacing
                        const page = Math.max(line, bannerBody.height - line)
                        const all = bannerBody.contentItem.contentHeight
                        switch (event.key) {
                        case Qt.Key_Up: bannerBody.scrollBy(-line); break
                        case Qt.Key_Down: bannerBody.scrollBy(line); break
                        case Qt.Key_PageUp: bannerBody.scrollBy(-page); break
                        case Qt.Key_PageDown: bannerBody.scrollBy(page); break
                        case Qt.Key_Home: bannerBody.scrollBy(-all); break
                        case Qt.Key_End: bannerBody.scrollBy(all); break
                        default:
                            event.accepted = false
                            return
                        }
                        event.accepted = true
                    }

                    Label {
                        id: bannerText
                        objectName: "errorBannerText"
                        width: bannerBody.availableWidth
                        text: banner.text
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: Kirigami.Theme.textColor
                        Accessible.ignored: true
                    }
                }

                RowLayout {
                    Layout.alignment: Qt.AlignTop
                    spacing: Kirigami.Units.smallSpacing

                    Button {
                        objectName: "errorBannerHistoryButton"
                        visible: root.historyAvailable
                        icon.name: "view-list-details"
                        text: root.historyShown
                            ? "Hide Recent Errors"
                            : "Recent Errors (" + root.errorCount + ")"
                        onClicked: root.errorController.toggleHistory()
                    }
                    Button {
                        objectName: "errorBannerDismissButton"
                        icon.name: "dialog-close"
                        text: "Dismiss"
                        Accessible.name: "Dismiss this error"
                        onClicked: root.errorController.dismissLatest()
                    }
                }
            }

            FontMetrics {
                id: bannerMetrics
                font: bannerText.font
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
