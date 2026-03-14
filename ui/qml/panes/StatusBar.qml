import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami

ToolBar {
    id: root

    required property var uiPalette
    required property var sections
    required property var channelStatusIconSource
    required property var mixColor
    required property bool themeIsDark

    implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
    leftPadding: 14
    rightPadding: 10
    topPadding: 2
    bottomPadding: 2

    contentItem: RowLayout {
        spacing: 6

        Repeater {
            model: root.sections

            delegate: RowLayout {
                required property int index
                required property var modelData

                spacing: 6
                Layout.fillWidth: !!modelData.stretch

                Label {
                    visible: index > 0
                    text: "|"
                    color: root.uiPalette.uiMutedTextColor
                }

                RowLayout {
                    readonly property string channelIconPath: root.channelStatusIconSource(
                        modelData.iconKey || "")
                    spacing: channelIconPath.length > 0 ? 4 : 0
                    Layout.fillWidth: !!modelData.stretch

                    Item {
                        id: channelIconItem
                        visible: parent.channelIconPath.length > 0
                        Layout.preferredWidth: visible ? 22 : 0
                        Layout.preferredHeight: 20
                        property url iconSource: parent.channelIconPath.length > 0
                            ? parent.channelIconPath
                            : ""

                        Image {
                            anchors.fill: parent
                            source: channelIconItem.iconSource
                            asynchronous: false
                            fillMode: Image.PreserveAspectFit
                            smooth: false
                            sourceSize.width: 44
                            sourceSize.height: 40
                        }
                    }

                    Label {
                        Layout.fillWidth: !!modelData.stretch
                        text: modelData.text || ""
                        elide: Text.ElideRight
                        color: modelData.kind === "error"
                            ? (modelData.emphasis
                                ? root.mixColor(
                                    Kirigami.Theme.negativeTextColor,
                                    root.uiPalette.uiTextColor,
                                    root.themeIsDark ? 0.18 : 0.08)
                                : Kirigami.Theme.negativeTextColor)
                            : (modelData.emphasis
                                ? Kirigami.Theme.highlightColor
                                : root.uiPalette.uiTextColor)
                        font.weight: modelData.emphasis ? Font.DemiBold : Font.Normal
                    }
                }
            }
        }
    }
}
