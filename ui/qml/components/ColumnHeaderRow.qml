import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

RowLayout {
    id: root

    required property var columns
    property color textColor: "white"
    property int leftMargin: 0
    property int rightMargin: 0
    property int spacingValue: 6

    anchors.leftMargin: root.leftMargin
    anchors.rightMargin: root.rightMargin
    spacing: root.spacingValue

    Repeater {
        model: root.columns

        delegate: Label {
            required property var modelData

            text: modelData.text || ""
            Layout.fillWidth: !!modelData.fill
            Layout.preferredWidth: modelData.width !== undefined ? modelData.width : -1
            horizontalAlignment: modelData.alignment !== undefined
                ? modelData.alignment
                : Text.AlignLeft
            font.weight: Font.DemiBold
            color: root.textColor
            elide: Text.ElideRight
        }
    }
}
