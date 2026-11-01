// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../logic/ErrorUtils.js" as ErrorUtils
import "." as Components

// The newest error in full, until the user dismisses it or sees it in the
// recent errors. For the viewers, which cover the status bar.
FocusScope {
    id: root

    required property var errorController
    // Whether the banner may show at all, such as while its viewer is open.
    property bool active: true
    // The message showing the newest error.
    readonly property alias message: banner
    // How many lines of the newest error show before its text scrolls.
    property int maximumLines: 4
    readonly property int errorCount: root.errorController ? root.errorController.count : 0
    readonly property var entry: root.errorController ? root.errorController.latest : null
    readonly property bool bannerShown: root.active
        && root.errorController !== null
        && root.errorController.latestUnseen
        && root.entry !== null

    implicitWidth: banner.implicitWidth
    implicitHeight: root.visible ? banner.height : 0
    visible: root.bannerShown

    // Keyboard focus goes back to the viewer rather than stay on a hidden button.
    onBannerShownChanged: {
        if (!root.bannerShown && root.activeFocus) {
            root.focus = false
        }
    }

    // Clicks on the error stay here; viewers close on a click.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: function(wheel) { wheel.accepted = true }
    }

    // Styled as Kirigami's inline messages. Its text scrolls past a few
    // lines, so a long error cannot push the window's controls, or its own
    // buttons, out of reach.
    Rectangle {
        id: banner
        objectName: "errorBanner"
        readonly property bool warning: root.entry !== null
            && root.entry.severity === ErrorUtils.SeverityWarning
        readonly property string text: ErrorUtils.describe(root.entry)
        readonly property color accentColor: banner.warning
            ? Kirigami.Theme.neutralTextColor
            : Kirigami.Theme.negativeTextColor

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        implicitWidth: bannerRow.implicitWidth + 2 * bannerRow.anchors.margins
        height: bannerRow.implicitHeight + 2 * bannerRow.anchors.margins
        radius: Kirigami.Units.cornerRadius
        color: banner.warning
            ? Kirigami.Theme.neutralBackgroundColor
            : Kirigami.Theme.negativeBackgroundColor
        border.color: banner.accentColor
        Accessible.role: Accessible.AlertMessage
        Accessible.name: banner.warning
            ? I18n.i18nc("@info accessible name of a warning banner", "Warning: %1", banner.text)
            : I18n.i18nc("@info accessible name of an error banner", "Error: %1", banner.text)

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

            Components.KeyScrollView {
                id: bannerBody
                objectName: "errorBannerBody"
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: Math.min(
                    bannerText.implicitHeight, bannerMetrics.lineSpacing * root.maximumLines)
                lineStep: bannerMetrics.lineSpacing
                Accessible.role: Accessible.StaticText
                Accessible.name: banner.text
                Accessible.description: I18n.i18nc("@info", "Scroll with the arrow and Page keys")
                Accessible.focusable: true

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
                    icon.name: "view-list-details"
                    text: I18n.i18nc("@action:button", "Recent Errors (%1)", root.errorCount)
                    onClicked: {
                        // The dialog hides the banner, and hands the keyboard
                        // back to whatever had it before it opened.
                        root.focus = false
                        root.errorController.historyRequested()
                    }
                }
                Button {
                    objectName: "errorBannerDismissButton"
                    icon.name: "dialog-close"
                    text: I18n.i18nc("@action:button", "Dismiss")
                    Accessible.name: I18n.i18nc("@action:button", "Dismiss this error")
                    onClicked: root.errorController.dismissLatest()
                }
            }
        }

        FontMetrics {
            id: bannerMetrics
            font: bannerText.font
        }
    }
}
