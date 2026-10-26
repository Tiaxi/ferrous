// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami

// Offers to undo the latest change that removed tracks from the queue:
// Clear, removing rows, or replacing the queue. It shows while the backend
// offers the undo, which any later queue change withdraws, until the user
// dismisses it; a new change shows it again.
Rectangle {
    id: root

    required property var uiBridge
    // The offer the user dismissed.
    property double dismissedId: 0
    readonly property double offerId: root.uiBridge.queueUndoId || 0
    readonly property bool shown: root.offerId > 0 && root.offerId !== root.dismissedId
    readonly property string text: root.describe(
        root.uiBridge.queueUndoKind || 0,
        root.uiBridge.queueUndoTrackCount || 0)

    // BinaryBridgeCodec::QueueUndoKind.
    function describe(kind, count) {
        const tracks = count === 1 ? "1 track" : count + " tracks"
        switch (kind) {
        case 1:
            return "Queue cleared (" + tracks + ")."
        case 2:
            return "Removed " + tracks + " from the queue."
        case 3:
            return "Queue replaced; the previous queue had " + tracks + "."
        default:
            return ""
        }
    }

    function undo() {
        if (!root.shown) {
            return
        }
        root.dismissedId = root.offerId
        root.uiBridge.undoQueueChange()
    }

    function dismiss() {
        root.dismissedId = root.offerId
    }

    visible: root.shown
    implicitHeight: noticeRow.implicitHeight + 12
    color: Kirigami.Theme.backgroundColor
    border.color: Kirigami.Theme.highlightColor
    radius: 4

    Accessible.role: Accessible.AlertMessage
    Accessible.name: root.text

    RowLayout {
        id: noticeRow
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 6
        anchors.topMargin: 6
        anchors.bottomMargin: 6
        spacing: 8

        Kirigami.Icon {
            source: "dialog-information"
            implicitWidth: 16
            implicitHeight: 16
        }

        Label {
            objectName: "queueUndoNoticeText"
            Layout.fillWidth: true
            text: root.text
            elide: Text.ElideRight
        }

        Button {
            objectName: "queueUndoButton"
            text: "Undo"
            icon.name: "edit-undo"
            onClicked: root.undo()
        }

        ToolButton {
            objectName: "queueUndoDismissButton"
            icon.name: "dialog-close"
            text: "Dismiss"
            display: AbstractButton.IconOnly
            ToolTip.visible: hovered
            ToolTip.text: text
            onClicked: root.dismiss()
        }
    }
}
