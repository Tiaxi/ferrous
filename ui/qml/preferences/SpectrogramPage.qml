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
    required property var spectrogramFftChoices

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
                    text: I18n.i18nc("@title", "Spectrogram")
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Kirigami.Theme.disabledTextColor
                    text: I18n.i18nc("@label", "Spectrogram-specific rendering and analysis options.")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        text: I18n.i18nc("@label", "View")
                        Layout.preferredWidth: root.uiPalette.rowHeight * 5
                    }

                    ComboBox {
                        Layout.preferredWidth: root.uiPalette.rowHeight * 9
                        model: [
                            I18n.i18nc("@item:inlistbox spectrogram view", "Downmix"),
                            I18n.i18nc("@item:inlistbox spectrogram view", "Per-channel")
                        ]
                        currentIndex: Math.max(0, Math.min(1, root.uiBridge.spectrogramViewMode))
                        onActivated: root.uiBridge.setSpectrogramViewMode(currentIndex)
                    }

                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        text: I18n.i18nc("@label", "Display")
                        Layout.preferredWidth: root.uiPalette.rowHeight * 5
                    }

                    ComboBox {
                        Layout.preferredWidth: root.uiPalette.rowHeight * 9
                        model: [
                            I18n.i18nc("@item:inlistbox spectrogram display", "Rolling"),
                            I18n.i18nc("@item:inlistbox spectrogram display", "Centered")
                        ]
                        currentIndex: Math.max(0, Math.min(1, root.uiBridge.spectrogramDisplayMode))
                        onActivated: root.uiBridge.setSpectrogramDisplayMode(currentIndex)
                    }

                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        text: I18n.i18nc("@label", "FFT Window")
                        Layout.preferredWidth: root.uiPalette.rowHeight * 5
                    }

                    ComboBox {
                        Layout.preferredWidth: root.uiPalette.rowHeight * 9
                        model: root.spectrogramFftChoices
                        currentIndex: {
                            const index = root.spectrogramFftChoices.indexOf(root.uiBridge.fftSize)
                            return index >= 0 ? index : 0
                        }
                        onActivated: root.uiBridge.setFftSize(
                            root.spectrogramFftChoices[Math.max(0, currentIndex)])
                    }

                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        text: I18n.i18nc("@label", "dB Range")
                        Layout.preferredWidth: root.uiPalette.rowHeight * 5
                    }

                    Slider {
                        id: dbRangeSlider
                        Layout.fillWidth: true
                        from: 50
                        to: 150
                        stepSize: 1
                        value: root.uiBridge.dbRange
                        onMoved: root.uiBridge.setDbRange(value)
                        onPressedChanged: {
                            if (!pressed) {
                                root.uiBridge.setDbRange(value)
                            }
                        }
                    }

                    Label {
                        text: Math.round(dbRangeSlider.value).toString()
                        Layout.preferredWidth: root.uiPalette.rowHeight * 1.5
                        horizontalAlignment: Text.AlignRight
                    }
                }

                CheckBox {
                    text: I18n.i18nc("@option:check", "Log Scale Spectrogram")
                    checked: root.uiBridge.logScale
                    onToggled: root.uiBridge.setLogScale(checked)
                }

                CheckBox {
                    text: I18n.i18nc("@option:check", "Show Visualization FPS Overlay")
                    checked: root.uiBridge.showFps
                    onToggled: root.uiBridge.setShowFps(checked)
                }

                CheckBox {
                    text: I18n.i18nc("@option:check", "Show Crosshair on Hover")
                    checked: root.uiBridge.showSpectrogramCrosshair
                    onToggled: root.uiBridge.setShowSpectrogramCrosshair(checked)
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 32
                    wrapMode: Text.Wrap
                    color: Kirigami.Theme.disabledTextColor
                    font: Kirigami.Theme.smallFont
                    text: I18n.i18nc("@label", "When enabled, right-clicking the spectrogram in centered mode seeks to that position in the track.")
                }

                CheckBox {
                    text: I18n.i18nc("@option:check", "Show Frequency/Time Scale")
                    checked: root.uiBridge.showSpectrogramScale
                    onToggled: root.uiBridge.setShowSpectrogramScale(checked)
                }

                CheckBox {
                    text: I18n.i18nc("@option:check", "Allow Spectrogram Zoom")
                    checked: root.uiBridge.spectrogramZoomEnabled
                    onToggled: root.uiBridge.setSpectrogramZoomEnabled(checked)
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 32
                    wrapMode: Text.Wrap
                    color: Kirigami.Theme.disabledTextColor
                    font: Kirigami.Theme.smallFont
                    text: I18n.i18nc("@label", "Scroll to zoom in/out. Middle-click to reset.")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        text: I18n.i18nc("@label", "Channel Buttons")
                        Layout.preferredWidth: root.uiPalette.rowHeight * 5
                    }

                    ComboBox {
                        Layout.preferredWidth: root.uiPalette.rowHeight * 9
                        model: [
                            I18n.i18nc("@item:inlistbox channel buttons", "Disabled"),
                            I18n.i18nc("@item:inlistbox channel buttons", "On Hover"),
                            I18n.i18nc("@item:inlistbox channel buttons", "Always")
                        ]
                        currentIndex: Math.max(0, Math.min(2, root.uiBridge.channelButtonsVisibility))
                        onActivated: root.uiBridge.setChannelButtonsVisibility(currentIndex)
                    }

                    Item { Layout.fillWidth: true }
                }
            }
        }
    }
}
