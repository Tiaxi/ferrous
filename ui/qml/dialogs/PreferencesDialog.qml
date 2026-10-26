// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "../components" as Components
import "../preferences" as Preferences

Dialog {
    id: root

    required property var uiBridge
    required property var uiPalette
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

    // Moves `step` pages on, wrapping around, and focuses the page's tab.
    function stepPage(step) {
        const count = root.pageTitles.length
        root.pageIndex = (root.pageIndex + step + count) % count
        const tab = preferencesTabBar.itemAt(root.pageIndex)
        if (tab) {
            tab.forceActiveFocus(Qt.TabFocusReason)
        }
    }

    modal: true
    // Keyboard users start in the dialog, on its tabs.
    focus: true
    title: "Preferences"
    standardButtons: Dialog.Close
    width: Math.min(760, root.windowRoot.width - 80)
    height: Math.min(620, root.windowRoot.height - 80)
    enter: Components.PopupTransition { duration: root.popupTransitionMs }
    exit: Components.PopupTransition { duration: root.popupTransitionMs }

    contentItem: ColumnLayout {
        spacing: 14

        Shortcut {
            sequences: ["Ctrl+Tab", "Ctrl+PgDown"]
            enabled: root.opened
            onActivated: root.stepPage(1)
        }

        Shortcut {
            sequences: [StandardKey.PreviousChild, "Ctrl+PgUp"]
            enabled: root.opened
            onActivated: root.stepPage(-1)
        }

        // Tab and Shift+Tab reach the tabs, which Space opens; the arrow
        // keys and Ctrl+Tab move between them.
        TabBar {
            id: preferencesTabBar
            objectName: "preferencesTabBar"
            Layout.fillWidth: true
            currentIndex: root.pageIndex
            onCurrentIndexChanged: root.pageIndex = currentIndex

            Repeater {
                model: root.pageTitles

                delegate: TabButton {
                    required property int index
                    required property string modelData
                    objectName: "preferencesTab_" + index
                    text: modelData
                    Keys.onLeftPressed: root.stepPage(-1)
                    Keys.onRightPressed: root.stepPage(1)
                }
            }
        }

        StackLayout {
            objectName: "preferencesPages"
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.pageIndex

            Preferences.LibraryPage {
                Layout.fillWidth: true
                Layout.fillHeight: true
                uiBridge: root.uiBridge
                uiPalette: root.uiPalette
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
                uiPalette: root.uiPalette
                spectrogramFftChoices: root.spectrogramFftChoices
            }

            Preferences.DisplayPage {
                Layout.fillWidth: true
                Layout.fillHeight: true
                uiBridge: root.uiBridge
                uiPalette: root.uiPalette
            }

            Preferences.LastFmPage {
                Layout.fillWidth: true
                Layout.fillHeight: true
                uiBridge: root.uiBridge
                uiPalette: root.uiPalette
            }

            Preferences.SystemMediaPage {
                Layout.fillWidth: true
                Layout.fillHeight: true
                uiBridge: root.uiBridge
                uiPalette: root.uiPalette
            }
        }
    }
}
