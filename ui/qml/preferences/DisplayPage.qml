import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import "../components" as Components

ScrollView {
    id: root

    required property var uiBridge
    required property var uiPalette

    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    ColumnLayout {
        width: root.availableWidth
        spacing: 0

        Components.SurfaceCard {
            Layout.fillWidth: true
            color: root.uiPalette.uiSurfaceColor
            borderColor: root.uiPalette.uiBorderColor
            implicitHeight: contentColumn.implicitHeight + 36

            ColumnLayout {
                id: contentColumn
                anchors.fill: parent
                anchors.margins: 18
                spacing: 14

                Label {
                    Layout.fillWidth: true
                    text: "Display"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Kirigami.Theme.disabledTextColor
                    text: "Shared viewer presentation options for album art and spectrogram."
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        text: "Viewer Fullscreen"
                        Layout.preferredWidth: 120
                    }

                    ComboBox {
                        Layout.preferredWidth: 220
                        model: ["Within app window", "Whole screen"]
                        currentIndex: Math.max(0, Math.min(1, root.uiBridge.viewerFullscreenMode))
                        onActivated: root.uiBridge.setViewerFullscreenMode(currentIndex)
                    }

                    Item { Layout.fillWidth: true }
                }
            }
        }
    }
}
