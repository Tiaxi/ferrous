// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami

// A dialog in a window of its own, as KConfigDialog and
// KAboutApplicationDialog are: KWin draws its title bar and close button,
// and it moves and resizes on its own, beyond the main window's edges. It
// stays above the window it is for, which it blocks while it is open.
//
// It answers as a Dialog does: open(), accept() and reject(), the
// accepted() and rejected() signals, and `opened`. Escape and the title
// bar's close button reject it; a combo box's open list, or a dialog over
// this one, takes Escape first. It hides at once, so a button clicked twice
// answers once.
ApplicationWindow {
    id: root

    required property var uiPalette
    // The window it is for: it stays above it and gives it the keyboard
    // back when it closes.
    required property var windowRoot
    // The size it first opens at, within the screen; it keeps its size
    // after that, which the user may change.
    property real defaultWidth: root.minimumWidth
    property real defaultHeight: root.minimumHeight
    readonly property bool opened: root.visible

    // As the window shows: the moment to set up what it shows.
    signal aboutToShow()
    signal accepted()
    signal rejected()

    property bool _sized: false
    // The screen's room: its size, less the panels where the desktop says
    // so, and a margin.
    readonly property real _roomWidth: root.screen
        ? Math.min(root.screen.width, root.screen.desktopAvailableWidth) - 2 * Kirigami.Units.gridUnit
        : root.defaultWidth
    readonly property real _roomHeight: root.screen
        ? Math.min(root.screen.height, root.screen.desktopAvailableHeight) - 2 * Kirigami.Units.gridUnit
        : root.defaultHeight

    function open() {
        if (!root.visible) {
            root.aboutToShow()
            if (!root._sized) {
                root._sized = true
                root.width = Math.ceil(Math.max(root.minimumWidth, Math.min(root.defaultWidth, root._roomWidth)))
                root.height = Math.ceil(Math.max(root.minimumHeight, Math.min(root.defaultHeight, root._roomHeight)))
            }
            root.show()
        }
        // A window manager raises a transient window it activates.
        root.requestActivate()
    }

    function accept() {
        if (root._hide()) {
            root.accepted()
        }
    }

    function reject() {
        if (root._hide()) {
            root.rejected()
        }
    }

    // Hides the window and gives the keyboard back to the window it is
    // for; whether it was shown.
    function _hide() {
        if (!root.visible) {
            return false
        }
        root.visible = false
        if (root.windowRoot && root.windowRoot.visible) {
            root.windowRoot.requestActivate()
        }
        return true
    }

    transientParent: root.windowRoot
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    visible: false
    color: root.uiPalette.chrome

    // The title bar's close button, and close() from the app: a Cancel.
    // The window hides rather than closes, so it opens again as it was.
    onClosing: function(close) {
        close.accepted = false
        root.reject()
    }

    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: root.visible
        onActivated: root.reject()
    }

    // Qt gives a window's shortcuts to the windows above it too, such as
    // the main window's Space for Play/Pause, which a check box here would
    // lose to it. A modal popup blocks a window's shortcuts, as this dialog
    // did when it was one: an empty one, undimmed, in the window it is for,
    // which the dialog blocks anyway, while it shows.
    Popup {
        objectName: "dialogWindowShortcutBlocker"
        parent: root.windowRoot ? root.windowRoot.contentItem : null
        visible: root.visible
        modal: true
        dim: false
        focus: false
        closePolicy: Popup.NoAutoClose
        width: 0
        height: 0
        padding: 0
        background: null
        enter: null
        exit: null
    }
}
