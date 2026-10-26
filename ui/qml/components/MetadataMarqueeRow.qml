// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami

RowLayout {
    id: root

    required property string labelText
    required property string valueText
    property string resetKey: ""
    property color labelColor: Kirigami.Theme.disabledTextColor
    property color textColor: Kirigami.Theme.textColor
    property real labelWidth: implicitLabelWidth
    readonly property real implicitLabelWidth: labelItem.implicitWidth
    property int pauseDuration: 1400
    property bool emphasized: false

    Layout.fillWidth: true
    spacing: 8

    Label {
        id: labelItem
        text: root.labelText
        Layout.preferredWidth: root.labelWidth
        horizontalAlignment: Text.AlignRight
        color: root.labelColor
    }

    Item {
        id: marqueeHost
        Layout.fillWidth: true
        Layout.preferredHeight: valueLabel.implicitHeight
        clip: true
        property real overflowPx: Math.max(0, valueLabel.implicitWidth - width)
        property real offsetPx: 0

        onOverflowPxChanged: {
            if (overflowPx <= 1) {
                offsetPx = 0
            } else if (offsetPx > overflowPx) {
                offsetPx = overflowPx
            }
        }

        onVisibleChanged: {
            if (!visible) {
                offsetPx = 0
            }
        }

        Connections {
            target: root
            function onResetKeyChanged() {
                marqueeHost.offsetPx = 0
                if (marqueeAnimation.running) {
                    marqueeAnimation.restart()
                }
            }
        }

        Label {
            id: valueLabel
            anchors.verticalCenter: parent.verticalCenter
            x: -marqueeHost.offsetPx
            text: root.valueText
            color: root.textColor
            textFormat: Text.PlainText
            font.weight: root.emphasized ? Font.DemiBold : Font.Normal
        }

        SequentialAnimation {
            id: marqueeAnimation
            running: marqueeHost.visible && marqueeHost.overflowPx > 1
            loops: Animation.Infinite

            PauseAnimation { duration: root.pauseDuration }
            NumberAnimation {
                target: marqueeHost
                property: "offsetPx"
                to: marqueeHost.overflowPx
                duration: Math.max(900, marqueeHost.overflowPx * 24)
                easing.type: Easing.Linear
            }
            ScriptAction { script: marqueeHost.offsetPx = marqueeHost.overflowPx }
            PauseAnimation { duration: root.pauseDuration }
            NumberAnimation {
                target: marqueeHost
                property: "offsetPx"
                to: 0
                duration: Math.max(900, marqueeHost.overflowPx * 24)
                easing.type: Easing.Linear
            }
            ScriptAction { script: marqueeHost.offsetPx = 0 }
        }
    }
}
