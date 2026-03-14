import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "../components" as Components
import "../preferences" as Preferences

Dialog {
    id: root

    required property var uiBridge
    required property var palette
    required property var windowRoot
    required property int popupTransitionMs
    required property var spectrogramFftChoices
    required property var promptAddLibraryRoot
    required property var openLibraryRootNameDialog
    required property var stepScrollView
    required property real snappyScrollFlickDeceleration
    required property real snappyScrollMaxFlickVelocity

    property int pageIndex: 0
    readonly property var pageTitles: [
        "Library",
        "Spectrogram",
        "Display",
        "Last.fm",
        "System Media"
    ]

    modal: true
    title: "Preferences"
    standardButtons: Dialog.Close
    width: Math.min(760, root.windowRoot.width - 80)
    height: Math.min(620, root.windowRoot.height - 80)
    enter: Components.PopupTransition { duration: root.popupTransitionMs }
    exit: Components.PopupTransition { duration: root.popupTransitionMs }

    contentItem: ColumnLayout {
        spacing: 14

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: preferencesTabsRow.implicitHeight
            color: root.palette.uiSurfaceAltColor
            radius: 8
            border.color: root.palette.uiBorderColor
            clip: true

            RowLayout {
                id: preferencesTabsRow
                anchors.fill: parent
                spacing: 0

                Repeater {
                    model: root.pageTitles

                    delegate: Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        color: root.pageIndex === index
                            ? root.palette.uiSelectionColor
                            : "transparent"

                        Label {
                            anchors.centerIn: parent
                            text: modelData
                            color: root.palette.uiTextColor
                            font.weight: root.pageIndex === index ? Font.DemiBold : Font.Normal
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.pageIndex = index
                        }
                    }
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.pageIndex

            Preferences.LibraryPage {
                Layout.fillWidth: true
                Layout.fillHeight: true
                uiBridge: root.uiBridge
                palette: root.palette
                windowRoot: root.windowRoot
                promptAddLibraryRoot: root.promptAddLibraryRoot
                openLibraryRootNameDialog: root.openLibraryRootNameDialog
                stepScrollView: root.stepScrollView
                snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
                snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
            }

            Preferences.SpectrogramPage {
                Layout.fillWidth: true
                Layout.fillHeight: true
                uiBridge: root.uiBridge
                palette: root.palette
                spectrogramFftChoices: root.spectrogramFftChoices
            }

            Preferences.DisplayPage {
                Layout.fillWidth: true
                Layout.fillHeight: true
                uiBridge: root.uiBridge
                palette: root.palette
            }

            Preferences.LastFmPage {
                Layout.fillWidth: true
                Layout.fillHeight: true
                uiBridge: root.uiBridge
                palette: root.palette
            }

            Preferences.SystemMediaPage {
                Layout.fillWidth: true
                Layout.fillHeight: true
                uiBridge: root.uiBridge
                palette: root.palette
            }
        }
    }
}
