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
// accepted(), rejected() and closed() signals, and `opened`. Escape and the
// title bar's close button reject it, or, in a dialog that asks first how
// it closes, send closeRequested(); a combo box's open list, or a dialog
// over this one, takes Escape first. It hides at once, and what it holds
// takes no click or key from then until it opens again, so a button
// clicked twice, or Enter pressed twice, answers once.
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
    // Escape and the title bar's close button send closeRequested() rather
    // than reject it: for a dialog that asks first, such as about unsaved
    // edits.
    property bool asksBeforeClosing: false
    // Whether it opens at its default size each time, for a dialog whose
    // default follows what it shows; otherwise it keeps the size it had.
    property bool sizesOnEachOpen: false

    // As the window shows: the moment to set up what it shows.
    signal aboutToShow()
    signal accepted()
    signal rejected()
    // As it hides, by an answer or for good.
    signal closed()
    signal closeRequested()

    property bool _sized: false
    // Set from the moment it hides until it opens again: the window system
    // may still deliver a click or a key that was on its way.
    property bool _gone: false
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
            root._gone = false
            root.aboutToShow()
            if (!root._sized || root.sizesOnEachOpen) {
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

    // Hides it with no answer, for a dialog whose buttons act through
    // their own signals, or that the app takes away. Not close(), which
    // Cancels, as the title bar's close button does.
    function dismiss() {
        root._hide()
    }

    // Hides the window and gives the keyboard back to the window it is
    // for; whether it was shown.
    function _hide() {
        if (!root.visible) {
            return false
        }
        root._gone = true
        root.visible = false
        if (root.windowRoot && root.windowRoot.visible) {
            root.windowRoot.requestActivate()
        }
        root.closed()
        return true
    }

    // Escape and the title bar's close button.
    function _cancel() {
        if (root.asksBeforeClosing) {
            root.closeRequested()
        } else {
            root.reject()
        }
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
        root._cancel()
    }

    // The window's own root item, which holds the content and the header
    // and footer too: the content item's topmost ancestor.
    function _rootItemOf(item) {
        while (item && item.parent) {
            item = item.parent
        }
        return item
    }

    Binding {
        target: root._rootItemOf(root.contentItem)
        property: "enabled"
        value: !root._gone
    }

    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: root.visible
        onActivated: root._cancel()
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
