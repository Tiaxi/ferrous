// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
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
            ? I18n.i18nc("@info", "Remove the library folder \u201c%1\u201d?", path)
            : I18n.i18nc("@info folder name, path", "Remove the library folder \u201c%1\u201d (%2)?", displayName, path)
        removeRootConfirmDialog.details = I18n.i18ncp("@info",
            "This removes its %1 track from the library. "
                + "The files themselves stay on disk, and adding the folder again brings them back.",
            "This removes its %1 tracks from the library. "
                + "The files themselves stay on disk, and adding the folder again brings them back.",
            trackCount)
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

                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 2
                    text: I18n.i18nc("@title", "Library")
                    font.weight: Font.DemiBold
                }

                RowLayout {
                    Layout.fillWidth: true

                    Button {
                        text: I18n.i18nc("@action:button", "Add Root...")
                        onClicked: root.promptAddLibraryRoot("preferences")
                    }

                    Button {
                        text: I18n.i18nc("@action:button", "Rescan All")
                        onClicked: root.uiBridge.rescanAllLibraryRoots()
                    }

                    Item { Layout.fillWidth: true }
                }

                CheckBox {
                    objectName: "libraryWatchCheckBox"
                    text: I18n.i18nc("@option:check", "Watch library folders for changes")
                    checked: root.uiBridge.libraryWatchEnabled
                    onToggled: root.uiBridge.setLibraryWatchEnabled(checked)
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Kirigami.Theme.disabledTextColor
                    text: I18n.i18nc("@label", "When enabled, files added, changed, or removed in the library folders appear in the library without a rescan, and the folders are rescanned at startup. Folders that cannot be watched, such as network shares, are rescanned periodically instead and are marked below.")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        text: I18n.i18nc("@label", "Album Sort")
                        Layout.preferredWidth: root.uiPalette.rowHeight * 5
                    }

                    ComboBox {
                        Layout.preferredWidth: root.uiPalette.rowHeight * 7.5
                        model: [
                            I18n.i18nc("@item:inlistbox album sort", "Year"),
                            I18n.i18nc("@item:inlistbox album sort", "Title")
                        ]
                        currentIndex: Math.max(0, Math.min(1, root.uiBridge.librarySortMode))
                        onActivated: root.uiBridge.setLibrarySortMode(currentIndex)
                    }

                    Item { Layout.fillWidth: true }
                }

                Label {
                    Layout.fillWidth: true
                    text: root.uiBridge.libraryRootEntries.length === 0
                        ? I18n.i18nc("@info", "No library roots configured.")
                        : I18n.i18nc("@label", "Configured roots")
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
                            border.color: root.uiPalette.uiBorderColor

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
                                        font: Kirigami.Theme.smallFont
                                    }

                                    Label {
                                        objectName: "libraryRootUnwatchedLabel"
                                        Layout.fillWidth: true
                                        visible: unwatchedReason.length > 0
                                        text: I18n.i18nc("@info:status", "Not watched for changes: %1", unwatchedReason)
                                        wrapMode: Text.Wrap
                                        color: Kirigami.Theme.neutralTextColor
                                        font: Kirigami.Theme.smallFont
                                    }

                                    // Stays until a rescan of the whole folder finds nothing wrong.
                                    Label {
                                        objectName: "libraryRootScanProblemLabel"
                                        Layout.fillWidth: true
                                        visible: scanProblem.length > 0
                                        text: I18n.i18nc("@info:status problem the last library scan found", "Last scan: %1", scanProblem)
                                        wrapMode: Text.Wrap
                                        color: Kirigami.Theme.negativeTextColor
                                        font: Kirigami.Theme.smallFont
                                        Accessible.role: Accessible.AlertMessage
                                        Accessible.name: I18n.i18nc("@info folder name, problem",
                                            "Problem with library folder %1: %2", rootDisplayName, scanProblem)
                                    }
                                }

                                ToolButton {
                                    text: I18n.i18nc("@action:button", "Open")
                                    onClicked: root.uiBridge.openInFileBrowser(rootPath)
                                }

                                ToolButton {
                                    text: I18n.i18nc("@action:button", "Rename")
                                    onClicked: root.openLibraryRootNameDialog("rename", rootPath, rootName)
                                }

                                ToolButton {
                                    text: I18n.i18nc("@action:button", "Rescan")
                                    onClicked: root.uiBridge.rescanLibraryRoot(rootPath)
                                }

                                ToolButton {
                                    objectName: "libraryRootRemoveButton"
                                    text: I18n.i18nc("@action:button", "Remove...")
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
            title: I18n.i18nc("@title:window", "Remove Library Folder")
            confirmText: I18n.i18nc("@action:button", "Remove Folder")
            onConfirmed: root.uiBridge.removeLibraryRoot(rootPath)
        }
    }
}
