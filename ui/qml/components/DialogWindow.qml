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
    // Whether its default size follows its content as laid out at the
    // width it opens at. Content is laid out only once its window shows,
    // so such a window lays its content out before it takes its size.
    property bool sizesToLaidOutContent: false
    // The content's inset from the window's edges, and the buttons': a
    // grid unit with the default font, and more with a larger one, up to
    // 32 px (18 px around 24 pt text crowded the window's edge).
    readonly property real contentPadding: Math.min(32, root.uiPalette.scaledSize(Kirigami.Units.gridUnit))

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
    // The width it opens at: its default, within the screen's room and its
    // minimum. Its content is laid out at that width only once it shows,
    // so a default height that follows text that wraps measures the text
    // at this width rather than at the content's.
    readonly property real openingWidth: Math.ceil(Math.max(root.minimumWidth,
        Math.min(root.defaultWidth, root._roomWidth)))

    // `height` within the screen's room, and at least its minimum: the
    // height to give the window for content that wants `height`.
    function heightWithinScreen(height) {
        return Math.ceil(Math.max(root.minimumHeight, Math.min(height, root._roomHeight)))
    }

    function open() {
        if (!root.visible) {
            root._gone = false
            root.aboutToShow()
            if (!root._sized || root.sizesOnEachOpen) {
                root._sized = true
                root.width = root.openingWidth
                if (root.sizesToLaidOutContent) {
                    root._layOutContent(root.openingWidth, root._roomHeight)
                }
                root.height = root.heightWithinScreen(root.defaultHeight)
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

    // Lays the content out at `width` by `height`, as the window will show
    // it, for a default size that follows it. The window gives its content
    // its size only once it shows; until then, the layouts place their
    // items only when asked to. Each pass places what the one before it
    // sized, such as a help text that wraps to its column, until the
    // default height stays as it is.
    function _layOutContent(width, height) {
        const footerHeight = root.footer ? root.footer.implicitHeight : 0
        root.contentItem.width = width
        root.contentItem.height = Math.max(0, height - footerHeight)
        for (let pass = 0; pass < 4; ++pass) {
            const before = root.defaultHeight
            root._polishTree(root.contentItem)
            if (root.defaultHeight === before) {
                break
            }
        }
    }

    function _polishTree(item) {
        item.ensurePolished()
        const children = item.children
        for (let i = 0; i < children.length; ++i) {
            root._polishTree(children[i])
        }
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
