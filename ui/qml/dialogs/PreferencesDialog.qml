// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../preferences" as Preferences
import "../logic/KeyUtils.js" as KeyUtils

// Configure Ferrous: a KDE page list beside the pages, in a window of its
// own, as KConfigDialog. The settings change on OK or Apply, Cancel drops
// what was changed since, and Defaults sets every page back to the
// defaults, to be applied in turn. The library folders and the Last.fm
// account are not settings: they change at once.
Components.DialogWindow {
    id: root

    required property var uiBridge
    required property var spectrogramFftChoices
    required property var promptAddLibraryRoot
    required property var openLibraryRootNameDialog

    property int pageIndex: 0
    readonly property var pages: [
        { title: I18n.i18nc("@title:tab preferences page", "Library"), icon: "folder" },
        { title: I18n.i18nc("@title:tab preferences page", "Spectrogram"), icon: "office-chart-bar" },
        { title: I18n.i18nc("@title:tab preferences page", "Appearance"), icon: "monitor" },
        { title: I18n.i18nc("@title:tab preferences page", "Last.fm"), icon: Qt.resolvedUrl("../assets/lastfm.svg") },
        { title: I18n.i18nc("@title:tab preferences page", "Media Controls"), icon: "multimedia-player" }
    ]
    // Kept for the callers that name a page.
    readonly property var pageTitles: root.pages.map(function(page) { return page.title })
    readonly property int libraryPageIndex: 0
    readonly property int lastFmPageIndex: 3

    // What the user changed and has yet to apply.
    readonly property alias settings: settingsState
    readonly property bool modified: settingsState.modified

    // Opens the dialog on page `index`.
    function openPage(index) {
        root.pageIndex = Math.max(0, Math.min(root.pages.length - 1, index))
        root.open()
    }

    function openLastFmPage() {
        root.openPage(root.lastFmPageIndex)
    }

    function openLibraryPage() {
        root.openPage(root.libraryPageIndex)
    }

    // Moves `step` pages on, wrapping around.
    function stepPage(step) {
        const count = root.pages.length
        root.pageIndex = (root.pageIndex + step + count) % count
    }

    function applyChanges() {
        settingsState.apply()
    }

    QtObject {
        id: settingsState
        objectName: "preferencesSettings"

        // The settings of the dialog, by the bridge's property name; each
        // has a setter named "set" and the name with a capital. Changes
        // wait in `pending` until they are applied.
        readonly property var keys: [
            "libraryWatchEnabled", "librarySortMode",
            "spectrogramViewMode", "spectrogramDisplayMode", "fftSize", "dbRange",
            "logScale", "spectrogramFrequencyCap", "showSpectrogramScale",
            "showSpectrogramCrosshair", "showFps", "spectrogramZoomEnabled",
            "channelButtonsVisibility",
            "viewerFullscreenMode", "showFullscreenTitleStrip", "preventDisplaySleepInFullscreen",
            "showLevelMeter", "stretchShortTracks",
            "lastFmScrobblingEnabled", "lastFmShowScrobbleProgress",
            "systemMediaControlsEnabled"
        ]
        property var pending: ({})
        readonly property bool modified: Object.keys(settingsState.pending).length > 0
        // The values of a new install, from the bridge.
        readonly property var defaults: root.uiBridge.settingDefaults
            ? root.uiBridge.settingDefaults()
            : ({})
        // Whether every setting shows its default, so Defaults would change
        // nothing.
        readonly property bool atDefaults: {
            for (let i = 0; i < settingsState.keys.length; ++i) {
                const key = settingsState.keys[i]
                if (settingsState.defaults[key] !== undefined
                        && !settingsState.same(settingsState.value(key), settingsState.defaults[key])) {
                    return false
                }
            }
            return true
        }

        function same(a, b) {
            return typeof a === "number" || typeof b === "number"
                ? Math.abs(Number(a) - Number(b)) < 1e-6
                : a === b
        }

        function bridgeValue(key) {
            return root.uiBridge[key]
        }

        // What the dialog shows for `key`: the change waiting to be
        // applied, or the setting as it is.
        function value(key) {
            return settingsState.pending.hasOwnProperty(key)
                ? settingsState.pending[key]
                : settingsState.bridgeValue(key)
        }

        function setValue(key, newValue) {
            const changed = Object.assign({}, settingsState.pending)
            if (settingsState.same(newValue, settingsState.bridgeValue(key))) {
                delete changed[key]
            } else {
                changed[key] = newValue
            }
            settingsState.pending = changed
        }

        function apply() {
            const changes = settingsState.pending
            // Cleared first: the bridge takes each value at once, so the
            // controls go on showing it.
            settingsState.pending = ({})
            for (const key in changes) {
                const setter = "set" + key.charAt(0).toUpperCase() + key.slice(1)
                root.uiBridge[setter](changes[key])
            }
        }

        function discard() {
            settingsState.pending = ({})
        }

        function restoreDefaults() {
            for (let i = 0; i < settingsState.keys.length; ++i) {
                const key = settingsState.keys[i]
                if (settingsState.defaults[key] !== undefined) {
                    settingsState.setValue(key, settingsState.defaults[key])
                }
            }
        }
    }

    // KConfigDialog's caption: "Configure", with the application's name.
    title: I18n.i18nc("@title:window %1 is the application name", "Configure — %1", "Ferrous")
    // The page list's rows: the icon and the title, with its insets.
    readonly property real pageRowLeftPadding: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
    readonly property real pageRowSpacing: Kirigami.Units.largeSpacing + 2
    // The page list: 176 px, or as wide as its longest title needs.
    readonly property real pageListWidth: {
        let widest = 0
        for (let i = 0; i < root.pages.length; ++i) {
            widest = Math.max(widest, pageTitleMetrics.advanceWidth(root.pages[i].title))
        }
        return Math.max(176, Math.ceil(root.pageRowLeftPadding + Kirigami.Units.iconSizes.small
            + root.pageRowSpacing + widest + root.pageRowLeftPadding))
    }
    FontMetrics {
        id: pageTitleMetrics
        font: root.uiPalette.baseFont
    }

    // 49 by 32 grid units, or the page list and 39 of them for the pages
    // when the list is wider, or wider for every page's wide layout; and
    // the tallest page whole at the default font, or taller for the page
    // it opens on. A larger font makes the window larger, within the
    // screen; it lays its pages out for the room it has, and the user may
    // make it larger.
    defaultWidth: Math.max(Kirigami.Units.gridUnit * 49, root.pageListWidth + 1
        + Math.max(Kirigami.Units.gridUnit * 39, pagesLoader.item ? pagesLoader.item.wideWidth : 0))
    defaultHeight: Math.max(Kirigami.Units.gridUnit * 32,
        preferencesContent.implicitHeight + preferencesFooter.implicitHeight)
    // The page list, and a page with room for its widest control.
    minimumWidth: Math.ceil(root.pageListWidth + 1 + Math.max(Kirigami.Units.gridUnit * 20,
        pagesLoader.item ? pagesLoader.item.minimumWidth : 0))
    minimumHeight: Math.ceil(Math.min(root.defaultHeight, Kirigami.Units.gridUnit * 18))

    onAboutToShow: {
        settingsState.discard()
        pagesLoader.active = true
        // Keyboard users start on the page list.
        pageList.forceActiveFocus(Qt.PopupFocusReason)
    }
    // Cancel, Escape and the close button drop what was not applied.
    onRejected: settingsState.discard()
    onAccepted: settingsState.apply()

    // Defaults on the left; OK, Apply and Cancel in KDE's order on the
    // right. A seam between the pages and the buttons.
    footer: Control {
        id: preferencesFooter
        objectName: "preferencesButtons"
        padding: 0
        background: Item {
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: root.uiPalette.seam
            }
        }

        // Enter on Defaults presses it; on the other buttons, the row takes it.
        Keys.onPressed: function(event) {
            if (KeyUtils.isEnter(event) && defaultsButton.activeFocus) {
                event.accepted = true
                if (defaultsButton.enabled) {
                    defaultsButton.clicked()
                }
            }
        }

        contentItem: RowLayout {
            spacing: 0

            Button {
                id: defaultsButton
                objectName: "preferencesDefaultsButton"
                Layout.leftMargin: Kirigami.Units.largeSpacing * 2
                text: I18n.i18nc("@action:button", "Defaults")
                icon.name: "document-revert"
                enabled: !settingsState.atDefaults
                onClicked: settingsState.restoreDefaults()
            }

            Item { Layout.fillWidth: true }

            Components.DialogButtons {

                id: dialogButtons
                primary: preferencesOkButton
                Button {
                    id: preferencesOkButton
                    objectName: "preferencesOkButton"
                    text: I18n.i18nc("@action:button", "OK")
                    icon.name: "dialog-ok"
                    DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                    onClicked: root.accept()
                }
                Button {
                    objectName: "preferencesApplyButton"
                    text: I18n.i18nc("@action:button", "Apply")
                    icon.name: "dialog-ok-apply"
                    enabled: settingsState.modified
                    DialogButtonBox.buttonRole: DialogButtonBox.ApplyRole
                    onClicked: settingsState.apply()
                }
                Button {
                    objectName: "preferencesCancelButton"
                    text: I18n.i18nc("@action:button", "Cancel")
                    icon.name: "dialog-cancel"
                    DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                    onClicked: root.reject()
                }
            }
        }
    }

    RowLayout {
        id: preferencesContent
        anchors.fill: parent

        // Enter presses the default button, as in KDE's dialogs. The pages'
        // combo boxes pass it on from here too.
        function handleDialogEnter(event, focusItem) {
            return dialogButtons.handleEnter(event, focusItem, root.visible)
        }
        Keys.onPressed: function(event) { handleDialogEnter(event, Window.activeFocusItem) }

        spacing: 0

        Shortcut {
            sequences: ["Ctrl+Tab", "Ctrl+PgDown"]
            enabled: root.visible
            onActivated: root.stepPage(1)
        }

        Shortcut {
            sequences: [StandardKey.PreviousChild, "Ctrl+PgUp"]
            enabled: root.visible
            onActivated: root.stepPage(-1)
        }

        // The page list: Tab reaches it, and the arrow keys move between
        // the pages.
        Rectangle {
            Layout.preferredWidth: root.pageListWidth
            Layout.fillHeight: true
            implicitHeight: pageList.anchors.topMargin + pageList.contentHeight
            color: root.uiPalette.view

            ListView {
                id: pageList
                objectName: "preferencesPageList"
                anchors.fill: parent
                anchors.topMargin: Kirigami.Units.largeSpacing
                model: root.pages
                currentIndex: root.pageIndex
                activeFocusOnTab: true
                keyNavigationEnabled: true
                keyNavigationWraps: false
                interactive: contentHeight > height
                boundsBehavior: Flickable.StopAtBounds
                Accessible.role: Accessible.PageTabList
                Accessible.name: I18n.i18nc("@label", "Pages")
                onCurrentIndexChanged: root.pageIndex = currentIndex

                delegate: ItemDelegate {
                    id: pageRow
                    required property int index
                    required property var modelData
                    objectName: "preferencesPage_" + index
                    width: ListView.view.width
                    implicitHeight: root.uiPalette.rowHeight + Kirigami.Units.smallSpacing
                    padding: 0
                    leftPadding: root.pageRowLeftPadding
                    focusPolicy: Qt.NoFocus
                    text: pageRow.modelData.title
                    highlighted: ListView.isCurrentItem
                    Accessible.role: Accessible.PageTab
                    Accessible.name: pageRow.modelData.title
                    Accessible.selected: pageRow.highlighted
                    onClicked: {
                        root.pageIndex = pageRow.index
                        pageList.forceActiveFocus(Qt.MouseFocusReason)
                    }

                    background: Components.RowBackground {
                        uiPalette: root.uiPalette
                        scrollbarLane: 0
                        hovered: pageRow.hovered
                        selected: pageRow.highlighted
                        active: pageList.activeFocus
                        focused: pageRow.highlighted && pageList.activeFocus
                    }

                    contentItem: RowLayout {
                        spacing: root.pageRowSpacing

                        Kirigami.Icon {
                            implicitWidth: Kirigami.Units.iconSizes.small
                            implicitHeight: Kirigami.Units.iconSizes.small
                            source: pageRow.modelData.icon
                            color: root.uiPalette.text
                            isMask: true
                            Accessible.ignored: true
                        }

                        Label {
                            Layout.fillWidth: true
                            text: pageRow.text
                            color: root.uiPalette.text
                            elide: Text.ElideRight
                            Accessible.ignored: true
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            color: root.uiPalette.seam
        }

        // The pages are made when the dialog first opens, rather than with
        // the window, which then shows sooner.
        Loader {
            id: pagesLoader
            objectName: "preferencesPagesLoader"
            Layout.fillWidth: true
            Layout.fillHeight: true
            active: false
            sourceComponent: Component {
                StackLayout {
                    id: pagesStack
                    objectName: "preferencesPages"
                    // The width that fits every page, and the width for
                    // every page's wide layout.
                    readonly property real minimumWidth: pagesStack.widest("minimumWidth")
                    readonly property real wideWidth: pagesStack.widest("wideWidth")
                    // The labels' column of every page: the widest label of
                    // them all, up to a limit, past which a page with a
                    // longer one has a wider column of its own.
                    readonly property real labelWidth: Math.min(pagesStack.widest("labelWidth"),
                        Kirigami.Units.gridUnit * 14)

                    function widest(property) {
                        let widest = 0
                        for (let i = 0; i < pagesStack.children.length; ++i) {
                            widest = Math.max(widest, pagesStack.children[i][property] || 0)
                        }
                        return widest
                    }
                    currentIndex: root.pageIndex

                    Preferences.LibraryPage {
                        sharedLabelWidth: pagesStack.labelWidth
                        objectName: "preferencesLibraryPage"
                        uiBridge: root.uiBridge
                        uiPalette: root.uiPalette
                        settings: settingsState
                        windowRoot: root
                        promptAddLibraryRoot: root.promptAddLibraryRoot
                        openLibraryRootNameDialog: root.openLibraryRootNameDialog
                    }

                    Preferences.SpectrogramPage {
                        sharedLabelWidth: pagesStack.labelWidth
                        objectName: "preferencesSpectrogramPage"
                        uiPalette: root.uiPalette
                        settings: settingsState
                        spectrogramFftChoices: root.spectrogramFftChoices
                    }

                    Preferences.DisplayPage {
                        sharedLabelWidth: pagesStack.labelWidth
                        objectName: "preferencesAppearancePage"
                        uiPalette: root.uiPalette
                        settings: settingsState
                    }

                    Preferences.LastFmPage {
                        sharedLabelWidth: pagesStack.labelWidth
                        objectName: "preferencesLastFmPage"
                        uiBridge: root.uiBridge
                        uiPalette: root.uiPalette
                        settings: settingsState
                    }

                    Preferences.SystemMediaPage {
                        sharedLabelWidth: pagesStack.labelWidth
                        objectName: "preferencesMediaControlsPage"
                        uiPalette: root.uiPalette
                        settings: settingsState
                    }
                }
            }
        }
    }
}
