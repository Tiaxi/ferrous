// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import "../components" as Components

ScrollView {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var windowRoot
    required property var promptAddLibraryRoot
    required property var openLibraryRootNameDialog
    required property var stepScrollView
    required property real snappyScrollFlickDeceleration
    required property real snappyScrollMaxFlickVelocity

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
            implicitHeight: libraryPrefsColumn.implicitHeight + 36

            ColumnLayout {
                id: libraryPrefsColumn
                anchors.fill: parent
                anchors.margins: 18
                spacing: 14

                Label {
                    Layout.fillWidth: true
                    text: "Library"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                RowLayout {
                    Layout.fillWidth: true

                    Button {
                        text: "Add Root..."
                        onClicked: root.promptAddLibraryRoot("preferences")
                    }

                    Button {
                        text: "Rescan All"
                        onClicked: root.uiBridge.rescanAllLibraryRoots()
                    }

                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        text: "Album Sort"
                        Layout.preferredWidth: 120
                    }

                    ComboBox {
                        Layout.preferredWidth: 180
                        model: ["Year", "Title"]
                        currentIndex: Math.max(0, Math.min(1, root.uiBridge.librarySortMode))
                        onActivated: root.uiBridge.setLibrarySortMode(currentIndex)
                    }

                    Item { Layout.fillWidth: true }
                }

                Label {
                    Layout.fillWidth: true
                    text: root.uiBridge.libraryRootEntries.length === 0
                        ? "No library roots configured."
                        : "Configured roots"
                    color: Kirigami.Theme.disabledTextColor
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(
                        260,
                        (60 * Math.max(1, root.uiBridge.libraryRootEntries.length)) + 12)
                    color: root.uiPalette.uiSurfaceAltColor
                    border.color: root.uiPalette.uiBorderColor
                    radius: 8
                    visible: root.uiBridge.libraryRootEntries.length > 0

                    ListView {
                        anchors.fill: parent
                        anchors.margins: 8
                        clip: true
                        model: root.uiBridge.libraryRootEntries
                        boundsBehavior: Flickable.StopAtBounds
                        boundsMovement: Flickable.StopAtBounds
                        flickDeceleration: root.snappyScrollFlickDeceleration
                        maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                        pixelAligned: true
                        spacing: 4

                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.NoButton
                            preventStealing: true
                            onWheel: function(wheel) {
                                root.stepScrollView(parent, wheel, 30, 3)
                            }
                        }

                        delegate: Rectangle {
                            readonly property var rootEntry: modelData || ({})
                            readonly property string rootPath: rootEntry.path || ""
                            readonly property string rootName: rootEntry.name || ""
                            readonly property string rootDisplayName: rootEntry.displayName || rootPath

                            width: ListView.view.width
                            height: 52
                            radius: 6
                            color: root.uiPalette.uiPaneColor
                            border.color: Qt.rgba(0, 0, 0, 0.06)

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 8

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2

                                    Label {
                                        Layout.fillWidth: true
                                        text: rootDisplayName
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: rootName.length > 0
                                        text: rootPath
                                        elide: Text.ElideMiddle
                                        color: root.uiPalette.uiMutedTextColor
                                        font.pixelSize: Math.max(11, root.windowRoot.font.pixelSize - 1)
                                    }
                                }

                                ToolButton {
                                    text: "Open"
                                    onClicked: root.uiBridge.openInFileBrowser(rootPath)
                                }

                                ToolButton {
                                    text: "Rename"
                                    onClicked: root.openLibraryRootNameDialog("rename", rootPath, rootName)
                                }

                                ToolButton {
                                    text: "Rescan"
                                    onClicked: root.uiBridge.rescanLibraryRoot(rootPath)
                                }

                                ToolButton {
                                    text: "Remove"
                                    onClicked: root.uiBridge.removeLibraryRoot(rootPath)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
