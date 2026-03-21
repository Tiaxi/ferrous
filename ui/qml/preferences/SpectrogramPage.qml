import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
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

                Label {
                    Layout.fillWidth: true
                    text: "Spectrogram"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Kirigami.Theme.disabledTextColor
                    text: "Spectrogram-specific rendering and analysis options."
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        text: "View"
                        Layout.preferredWidth: 120
                    }

                    ComboBox {
                        Layout.preferredWidth: 220
                        model: ["Downmix", "Per-channel"]
                        currentIndex: Math.max(0, Math.min(1, root.uiBridge.spectrogramViewMode))
                        onActivated: root.uiBridge.setSpectrogramViewMode(currentIndex)
                    }

                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        text: "Display"
                        Layout.preferredWidth: 120
                    }

                    ComboBox {
                        Layout.preferredWidth: 220
                        model: ["Rolling", "Centered"]
                        currentIndex: Math.max(0, Math.min(1, root.uiBridge.spectrogramDisplayMode))
                        onActivated: root.uiBridge.setSpectrogramDisplayMode(currentIndex)
                    }

                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        text: "FFT Window"
                        Layout.preferredWidth: 120
                    }

                    ComboBox {
                        Layout.preferredWidth: 220
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
                        text: "dB Range"
                        Layout.preferredWidth: 120
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
                        Layout.preferredWidth: 32
                        horizontalAlignment: Text.AlignRight
                    }
                }

                CheckBox {
                    text: "Log Scale Spectrogram"
                    focusPolicy: Qt.NoFocus
                    checked: root.uiBridge.logScale
                    onToggled: root.uiBridge.setLogScale(checked)
                }

                CheckBox {
                    text: "Show Spectrogram FPS Overlay"
                    focusPolicy: Qt.NoFocus
                    checked: root.uiBridge.showFps
                    onToggled: root.uiBridge.setShowFps(checked)
                }
            }
        }
    }
}
