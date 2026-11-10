// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "." as Preferences
import "../dialogs" as Dialogs

// The library folders, which change at once, and the library's settings,
// which change on OK or Apply.
Preferences.PreferencesPage {
    id: root

    required property var uiBridge
    required property var windowRoot
    required property var promptAddLibraryRoot
    required property var openLibraryRootNameDialog

    readonly property var rootEntries: root.uiBridge.libraryRootEntries || []
    // The folder the buttons act on.
    property int currentRootIndex: 0
    readonly property var currentRoot: root.currentRootIndex >= 0
        && root.currentRootIndex < root.rootEntries.length
        ? root.rootEntries[root.currentRootIndex]
        : null

    function rootDisplayName(entry) {
        return entry ? (entry.displayName || entry.path || "") : ""
    }

    // Removing a folder takes its tracks out of the library, so it asks
    // first.
    function requestRemoveRoot(path, displayName, trackCount) {
        removeRootConfirmDialog.rootPath = path
        removeRootConfirmDialog.message = displayName === path
            ? I18n.i18nc("@info", "Remove the library folder “%1”?", path)
            : I18n.i18nc("@info folder name, path", "Remove the library folder “%1” (%2)?", displayName, path)
        removeRootConfirmDialog.details = I18n.i18ncp("@info",
            "This removes its %1 track from the library. "
                + "The files themselves stay on disk, and adding the folder again brings them back.",
            "This removes its %1 tracks from the library. "
                + "The files themselves stay on disk, and adding the folder again brings them back.",
            trackCount)
        removeRootConfirmDialog.open()
    }

    function removeCurrentRoot() {
        const entry = root.currentRoot
        if (entry) {
            root.requestRemoveRoot(entry.path || "", root.rootDisplayName(entry), entry.trackCount || 0)
        }
    }

    function renameCurrentRoot() {
        const entry = root.currentRoot
        if (entry) {
            root.openLibraryRootNameDialog("rename", entry.path || "", entry.name || "")
        }
    }

    function rescanCurrentRoot() {
        const entry = root.currentRoot
        if (entry) {
            root.uiBridge.rescanLibraryRoot(entry.path || "")
        }
    }

    function openCurrentRoot() {
        const entry = root.currentRoot
        if (entry) {
            root.uiBridge.openInFileBrowser(entry.path || "")
        }
    }

    onRootEntriesChanged: {
        if (root.currentRootIndex >= root.rootEntries.length) {
            root.currentRootIndex = Math.max(0, root.rootEntries.length - 1)
        }
    }

    title: I18n.i18nc("@title preferences page", "Library")

    // The folders take the column's width up to 29 grid units, and no less
    // than their buttons need: a long path elides.
    ColumnLayout {
        Kirigami.FormData.label: I18n.i18nc("@label", "Library folders:")
        // Next to the label's first line.
        Kirigami.FormData.labelAlignment: Qt.AlignTop
        Layout.fillWidth: true
        Layout.minimumWidth: rootButtons.implicitWidth
        Layout.preferredWidth: Math.max(Kirigami.Units.gridUnit * 16, rootButtons.implicitWidth)
        Layout.maximumWidth: Math.max(Kirigami.Units.gridUnit * 29, rootButtons.implicitWidth)
        spacing: Kirigami.Units.largeSpacing

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: rootList.visible
                ? Math.min(rootList.contentHeight, root.uiPalette.rowHeight * 8) + 6
                : emptyRootsLabel.implicitHeight + 2 * Kirigami.Units.largeSpacing
            color: root.uiPalette.view
            radius: 3
            border.width: 1
            border.color: Kirigami.ColorUtils.linearInterpolation(
                root.uiPalette.view, root.uiPalette.text, Kirigami.Theme.frameContrast)

            Label {
                id: emptyRootsLabel
                objectName: "libraryRootsEmptyLabel"
                anchors.centerIn: parent
                width: parent.width - 2 * Kirigami.Units.largeSpacing
                visible: root.rootEntries.length === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: root.uiPalette.textSecondary
                text: I18n.i18nc("@info", "No library folders yet.")
            }

            ListView {
                id: rootList
                objectName: "libraryRootList"
                anchors.fill: parent
                anchors.margins: 3
                visible: root.rootEntries.length > 0
                clip: true
                model: root.rootEntries
                currentIndex: root.currentRootIndex
                activeFocusOnTab: true
                keyNavigationEnabled: true
                boundsBehavior: Flickable.StopAtBounds
                pixelAligned: true
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                Accessible.role: Accessible.List
                Accessible.name: I18n.i18nc("@label", "Library folders")
                onCurrentIndexChanged: root.currentRootIndex = currentIndex

                Keys.onReturnPressed: root.openCurrentRoot()
                Keys.onEnterPressed: root.openCurrentRoot()
                Keys.onDeletePressed: root.removeCurrentRoot()
                Keys.onMenuPressed: rootContextMenu.popup(rootList.currentItem)

                delegate: ItemDelegate {
                    id: rootRow
                    objectName: "libraryRootRow"
                    required property int index
                    required property var modelData
                    readonly property var rootEntry: modelData || ({})
                    readonly property string rootPath: rootEntry.path || ""
                    readonly property string rootName: rootEntry.name || ""
                    readonly property string unwatchedReason: rootEntry.unwatchedReason || ""
                    readonly property string scanProblem: rootEntry.scanProblem || ""
                    readonly property int trackCount: rootEntry.trackCount || 0

                    width: ListView.view.width
                    implicitHeight: Math.max(root.uiPalette.rowHeight + Kirigami.Units.smallSpacing,
                        rowColumn.implicitHeight + 2 * Kirigami.Units.smallSpacing)
                    padding: 0
                    highlighted: ListView.isCurrentItem
                    focusPolicy: Qt.NoFocus
                    Accessible.role: Accessible.ListItem
                    Accessible.name: rootRow.rootName.length > 0
                        ? I18n.i18nc("@info folder name, path, track count", "%1, %2, %3",
                            rootRow.rootName, rootRow.rootPath,
                            I18n.i18ncp("@info", "%1 track", "%1 tracks", rootRow.trackCount))
                        : I18n.i18nc("@info path, track count", "%1, %2", rootRow.rootPath,
                            I18n.i18ncp("@info", "%1 track", "%1 tracks", rootRow.trackCount))

                    onClicked: {
                        rootList.currentIndex = rootRow.index
                        rootList.forceActiveFocus(Qt.MouseFocusReason)
                    }
                    onDoubleClicked: {
                        rootList.currentIndex = rootRow.index
                        root.openCurrentRoot()
                    }
                    onPressAndHold: rootContextMenu.popup()

                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: {
                            rootList.currentIndex = rootRow.index
                            rootContextMenu.popup()
                        }
                    }

                    background: Components.RowBackground {
                        uiPalette: root.uiPalette
                        scrollbarLane: 0
                        hovered: rootRow.hovered
                        selected: rootRow.highlighted
                        active: rootList.activeFocus
                        focused: rootRow.highlighted && rootList.activeFocus
                    }

                    contentItem: ColumnLayout {
                        id: rowColumn
                        spacing: 0

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: Kirigami.Units.largeSpacing + 2
                            Layout.rightMargin: Kirigami.Units.largeSpacing + 2
                            spacing: Kirigami.Units.largeSpacing + 2

                            Kirigami.Icon {
                                implicitWidth: Kirigami.Units.iconSizes.small
                                implicitHeight: Kirigami.Units.iconSizes.small
                                source: "folder"
                                color: root.uiPalette.textSecondary
                                isMask: true
                                Accessible.ignored: true
                            }

                            Label {
                                objectName: "libraryRootName"
                                visible: rootRow.rootName.length > 0
                                Layout.preferredWidth: Math.min(implicitWidth, Kirigami.Units.gridUnit * 8)
                                text: rootRow.rootName
                                color: root.uiPalette.text
                                elide: Text.ElideRight
                                Accessible.ignored: true
                            }

                            Label {
                                objectName: "libraryRootPath"
                                Layout.fillWidth: true
                                text: rootRow.rootPath
                                color: rootRow.rootName.length > 0 ? root.uiPalette.textSecondary : root.uiPalette.text
                                elide: Text.ElideMiddle
                                Accessible.ignored: true
                            }

                            Label {
                                objectName: "libraryRootTrackCount"
                                text: I18n.i18ncp("@info", "%1 track", "%1 tracks", rootRow.trackCount)
                                font.pointSize: root.uiPalette.smallPointSize
                                font.features: root.uiPalette.tabularFigures
                                color: root.uiPalette.textTertiaryOn(rootRow.highlighted)
                                Accessible.ignored: true
                            }
                        }

                        Label {
                            objectName: "libraryRootUnwatchedLabel"
                            Layout.fillWidth: true
                            Layout.leftMargin: 2 * Kirigami.Units.largeSpacing + 4 + Kirigami.Units.iconSizes.small
                            Layout.rightMargin: Kirigami.Units.largeSpacing + 2
                            visible: rootRow.unwatchedReason.length > 0
                            text: I18n.i18nc("@info:status", "Not watched for changes: %1", rootRow.unwatchedReason)
                            wrapMode: Text.Wrap
                            font.pointSize: root.uiPalette.smallPointSize
                            color: root.uiPalette.textSecondary
                        }

                        // Stays until a rescan of the whole folder finds nothing wrong.
                        Label {
                            objectName: "libraryRootScanProblemLabel"
                            Layout.fillWidth: true
                            Layout.leftMargin: 2 * Kirigami.Units.largeSpacing + 4 + Kirigami.Units.iconSizes.small
                            Layout.rightMargin: Kirigami.Units.largeSpacing + 2
                            visible: rootRow.scanProblem.length > 0
                            text: I18n.i18nc("@info:status problem the last library scan found", "Last scan: %1", rootRow.scanProblem)
                            wrapMode: Text.Wrap
                            font.pointSize: root.uiPalette.smallPointSize
                            color: root.uiPalette.negative
                            Accessible.role: Accessible.AlertMessage
                            Accessible.name: I18n.i18nc("@info folder name, problem",
                                "Problem with library folder %1: %2",
                                root.rootDisplayName(rootRow.rootEntry), rootRow.scanProblem)
                        }
                    }
                }
            }
        }

        RowLayout {
            id: rootButtons
            spacing: Kirigami.Units.largeSpacing

            Button {
                objectName: "libraryRootAddButton"
                icon.name: "folder-add"
                text: I18n.i18nc("@action:button", "Add…")
                Accessible.name: I18n.i18nc("@action:button", "Add Library Folder")
                onClicked: root.promptAddLibraryRoot("preferences")
            }

            Button {
                objectName: "libraryRootRenameButton"
                icon.name: "edit-rename"
                text: I18n.i18nc("@action:button", "Rename…")
                enabled: root.currentRoot !== null
                onClicked: root.renameCurrentRoot()
            }

            Button {
                objectName: "libraryRootRescanButton"
                icon.name: "view-refresh"
                text: I18n.i18nc("@action:button", "Rescan")
                enabled: root.currentRoot !== null
                onClicked: root.rescanCurrentRoot()
            }

            Button {
                objectName: "libraryRootRemoveButton"
                // Removing from the list: the files stay on disk.
                icon.name: "list-remove"
                text: I18n.i18nc("@action:button", "Remove")
                enabled: root.currentRoot !== null
                onClicked: root.removeCurrentRoot()
            }
        }
    }

    CheckBox {
        id: watchCheckBox
        objectName: "libraryWatchCheckBox"
        Kirigami.FormData.label: " "
        text: I18n.i18nc("@option:check", "Watch library folders for changes")
        checked: root.settings.value("libraryWatchEnabled")
        onToggled: root.settings.setValue("libraryWatchEnabled", checked)
    }

    Components.FormHelpText {
        objectName: "libraryWatchHelp"
        uiPalette: root.uiPalette
        indent: watchCheckBox.contentItem.x
        text: I18n.i18nc("@info", "New, changed and removed files appear without a rescan, and the folders "
            + "are rescanned at startup. Folders that can’t be watched, such as network shares, "
            + "are rescanned every 30 minutes instead.")
    }

    Components.DialogComboBox {
        objectName: "librarySortComboBox"
        Kirigami.FormData.label: I18n.i18nc("@label:listbox", "Sort albums by:")
        Layout.preferredWidth: Math.max(Kirigami.Units.gridUnit * 9, implicitWidth)
        model: [
            I18n.i18nc("@item:inlistbox album sort", "Year"),
            I18n.i18nc("@item:inlistbox album sort", "Title")
        ]
        currentIndex: Math.max(0, Math.min(1, root.settings.value("librarySortMode")))
        onActivated: function(index) { root.settings.setValue("librarySortMode", index) }
    }

    Menu {
        id: rootContextMenu
        MenuItem {
            text: I18n.i18nc("@action:inmenu %1 is the file manager", "Open in %1", root.uiBridge.fileBrowserName || "")
            icon.name: "system-file-manager"
            onTriggered: root.openCurrentRoot()
        }
        MenuItem {
            text: I18n.i18nc("@action:inmenu", "Rename…")
            icon.name: "edit-rename"
            onTriggered: root.renameCurrentRoot()
        }
        MenuItem {
            text: I18n.i18nc("@action:inmenu", "Rescan")
            icon.name: "view-refresh"
            onTriggered: root.rescanCurrentRoot()
        }
        MenuItem {
            objectName: "libraryRootRescanAllMenuItem"
            text: I18n.i18nc("@action:inmenu", "Rescan All Folders")
            onTriggered: root.uiBridge.rescanAllLibraryRoots()
        }
        MenuSeparator {}
        MenuItem {
            text: I18n.i18nc("@action:inmenu", "Remove")
            icon.name: "list-remove"
            onTriggered: root.removeCurrentRoot()
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
