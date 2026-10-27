// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
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

                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 2
                    text: I18n.i18nc("@title", "Display")
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Kirigami.Theme.disabledTextColor
                    text: I18n.i18nc("@label", "Shared viewer presentation options for album art and spectrogram.")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        text: I18n.i18nc("@label", "Viewer Fullscreen")
                        Layout.preferredWidth: root.uiPalette.rowHeight * 5
                    }

                    ComboBox {
                        Layout.preferredWidth: root.uiPalette.rowHeight * 9
                        model: [
                            I18n.i18nc("@item:inlistbox viewer fullscreen", "Within app window"),
                            I18n.i18nc("@item:inlistbox viewer fullscreen", "Whole screen")
                        ]
                        currentIndex: Math.max(0, Math.min(1, root.uiBridge.viewerFullscreenMode))
                        onActivated: root.uiBridge.setViewerFullscreenMode(currentIndex)
                    }

                    Item { Layout.fillWidth: true }
                }

                CheckBox {
                    objectName: "showLevelMeterCheckBox"
                    text: I18n.i18nc("@option:check", "Show level meter below the main visualization")
                    checked: root.uiBridge.showLevelMeter
                    onToggled: root.uiBridge.setShowLevelMeter(checked)
                }

                CheckBox {
                    objectName: "preventDisplaySleepInFullscreenCheckBox"
                    text: I18n.i18nc("@option:check", "Prevent display sleep and dimming while playing in fullscreen viewer")
                    checked: root.uiBridge.preventDisplaySleepInFullscreen
                    onToggled: root.uiBridge.setPreventDisplaySleepInFullscreen(checked)
                }
            }
        }
    }
}
