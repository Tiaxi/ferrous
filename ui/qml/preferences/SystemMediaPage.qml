import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import "../components" as Components

ScrollView {
    id: root

    required property var uiBridge
    required property var palette

    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    ColumnLayout {
        width: root.availableWidth
        spacing: 0

        Components.SurfaceCard {
            Layout.fillWidth: true
            color: root.palette.uiSurfaceColor
            borderColor: root.palette.uiBorderColor
            implicitHeight: contentColumn.implicitHeight + 36

            ColumnLayout {
                id: contentColumn
                anchors.fill: parent
                anchors.margins: 18
                spacing: 14

                Label {
                    Layout.fillWidth: true
                    text: "System Media"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                CheckBox {
                    text: "Enable KDE media controls and media buttons"
                    focusPolicy: Qt.NoFocus
                    checked: root.uiBridge.systemMediaControlsEnabled
                    onToggled: root.uiBridge.setSystemMediaControlsEnabled(checked)
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Kirigami.Theme.disabledTextColor
                    text: "When enabled, Ferrous appears in Plasma's media controls and responds to Play/Pause, Previous, Next, and Stop media buttons. Keyboard volume buttons always control system volume, not Ferrous volume."
                }
            }
        }
    }
}
