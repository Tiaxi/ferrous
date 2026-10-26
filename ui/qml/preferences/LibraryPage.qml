// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import "../components" as Components
import "../dialogs" as Dialogs

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

    // Removing a root takes its tracks out of the library, so it asks first.
    function requestRemoveRoot(path, displayName, trackCount) {
        removeRootConfirmDialog.rootPath = path
        removeRootConfirmDialog.message = displayName === path
            ? "Remove the library folder \u201c" + path + "\u201d?"
            : "Remove the library folder \u201c" + displayName + "\u201d (" + path + ")?"
        const tracks = trackCount === 1 ? "1 track" : trackCount + " tracks"
        removeRootConfirmDialog.details = "This removes its " + tracks + " from the library. "
            + "The files themselves stay on disk, and adding the folder again brings them back."
        removeRootConfirmDialog.open()
    }

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

                CheckBox {
                    objectName: "libraryWatchCheckBox"
                    text: "Watch library folders for changes"
                    focusPolicy: Qt.NoFocus
                    checked: root.uiBridge.libraryWatchEnabled
                    onToggled: root.uiBridge.setLibraryWatchEnabled(checked)
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Kirigami.Theme.disabledTextColor
                    text: "When enabled, files added, changed, or removed in the library folders appear in the library without a rescan, and the folders are rescanned at startup. Folders that cannot be watched, such as network shares, are rescanned periodically instead and are marked below."
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
                    Layout.preferredHeight: Math.min(260, rootList.contentHeight + 16)
                    color: root.uiPalette.uiSurfaceAltColor
                    border.color: root.uiPalette.uiBorderColor
                    radius: 8
                    visible: root.uiBridge.libraryRootEntries.length > 0

                    ListView {
                        id: rootList
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
                            readonly property string unwatchedReason: rootEntry.unwatchedReason || ""
                            readonly property string scanProblem: rootEntry.scanProblem || ""
                            readonly property int trackCount: rootEntry.trackCount || 0

                            width: ListView.view.width
                            height: Math.max(52, rootRow.implicitHeight + 12)
                            radius: 6
                            color: root.uiPalette.uiPaneColor
                            border.color: Qt.rgba(0, 0, 0, 0.06)

                            RowLayout {
                                id: rootRow
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

                                    Label {
                                        objectName: "libraryRootUnwatchedLabel"
                                        Layout.fillWidth: true
                                        visible: unwatchedReason.length > 0
                                        text: "Not watched for changes: " + unwatchedReason
                                        wrapMode: Text.Wrap
                                        color: Kirigami.Theme.neutralTextColor
                                        font.pixelSize: Math.max(11, root.windowRoot.font.pixelSize - 1)
                                    }

                                    // Stays until a rescan of the whole folder finds nothing wrong.
                                    Label {
                                        objectName: "libraryRootScanProblemLabel"
                                        Layout.fillWidth: true
                                        visible: scanProblem.length > 0
                                        text: "Last scan: " + scanProblem
                                        wrapMode: Text.Wrap
                                        color: Kirigami.Theme.negativeTextColor
                                        font.pixelSize: Math.max(11, root.windowRoot.font.pixelSize - 1)
                                        Accessible.role: Accessible.AlertMessage
                                        Accessible.name: "Problem with library folder "
                                            + rootDisplayName + ": " + scanProblem
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
                                    objectName: "libraryRootRemoveButton"
                                    text: "Remove..."
                                    onClicked: root.requestRemoveRoot(rootPath, rootDisplayName, trackCount)
                                }
                            }
                        }
                    }
                }
            }
        }

        Dialogs.ConfirmDialog {
            id: removeRootConfirmDialog
            objectName: "removeLibraryRootConfirmDialog"
            property string rootPath: ""
            uiPalette: root.uiPalette
            windowRoot: root.windowRoot
            title: "Remove Library Folder"
            confirmText: "Remove Folder"
            onConfirmed: root.uiBridge.removeLibraryRoot(rootPath)
        }
    }
}
