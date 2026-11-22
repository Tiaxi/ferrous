// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami

// A short dialog in a window of its own, as KMessageBox and KDE's small
// dialogs are: a question, a message or a short form, at the dialogs'
// inset above the button row, with KWin's title bar for its title. The
// window opens as tall as what it holds, and follows it as it changes,
// such as text that wraps anew at a width the user gives it, within the
// screen: where the screen leaves too little room, the content scrolls
// above the buttons.
//
// Put the content in it as in a ColumnLayout, and the buttons, a
// DialogFooter or DialogButtons, in `footer`. The keyboard is in the
// content when it opens, or on `initialFocusItem`, and Enter presses the
// default button, as KDE's dialogs do.
DialogWindow {
    id: root

    default property alias content: body.data
    readonly property alias body: body
    readonly property real contentPadding: Kirigami.Units.gridUnit
    // Between paragraphs: 12 px with the default font, and more with a
    // larger one (12 px between 44 px lines ran them together at 24 pt).
    // The inset stays.
    readonly property real contentSpacing: root.uiPalette.scaledSize(Kirigami.Units.largeSpacing
        + Kirigami.Units.smallSpacing)
    // Where the keyboard goes as it opens.
    property Item initialFocusItem: body
    // The width the content needs whole, leaving out text that wraps:
    // a form's or a check box's, which do not wrap and would run past a
    // narrower window, as they did at large fonts.
    property real contentWidth: 0

    readonly property real _footerHeight: root.footer && root.footer.visible ? root.footer.implicitHeight : 0
    readonly property real _footerWidth: root.footer ? root.footer.implicitWidth : 0

    // The height that holds the content at the window's width.
    readonly property int fittedHeight: Math.ceil(2 * root.contentPadding + body.implicitHeight + root._footerHeight)

    // The measure of its text: about 46 characters of the body font, so
    // that a larger font keeps a lead on a line and a body of a few lines,
    // rather than a few words to the line (at 24 pt, 450 px held 28
    // characters). With the default font, 25 grid units, 450 px, hold more.
    readonly property int measureWidth: Math.ceil(46 * bodyMetrics.averageCharacterWidth + 2 * root.contentPadding)

    // Its minimum stays put: the window system takes a new one as a new
    // window. Its width is the measure, and the buttons' and the content's
    // at least, as it opens, within the screen.
    minimumWidth: Kirigami.Units.gridUnit * 18
    defaultWidth: Math.max(Kirigami.Units.gridUnit * 25, root.measureWidth, root._neededWidth)
    readonly property int _neededWidth: Math.ceil(Math.max(root._footerWidth,
        root.contentWidth > 0 ? root.contentWidth + 2 * root.contentPadding : 0))
    defaultHeight: root.fittedHeight

    // The window's height follows the content's rather than its minimum
    // and maximum, which the window system would take as a window the
    // user may not resize. Within the screen, as it opens: text that wraps
    // further, at a large font, ran the buttons off the screen.
    onFittedHeightChanged: {
        if (root.visible) {
            root.height = root.heightWithinScreen(root.fittedHeight)
        }
    }

    Connections {
        target: root
        function onAboutToShow() {
            // The width it had, or the buttons' and the content's, which
            // may have changed, within the screen.
            root.width = Math.max(root.width, Math.min(root._neededWidth, root._roomWidth))
            root.height = root.heightWithinScreen(root.fittedHeight)
            // From its start, whatever it was scrolled to when it closed.
            bodyView.contentY = 0
            if (root.initialFocusItem) {
                root.initialFocusItem.forceActiveFocus(Qt.PopupFocusReason)
            }
        }
    }

    FontMetrics {
        id: bodyMetrics
        font: root.uiPalette.baseFont
    }

    // The content, which scrolls only when the window is shorter than it,
    // on a screen with too little room or at a height the user gives it;
    // the buttons stay below it.
    Flickable {
        id: bodyView
        objectName: "dialogBodyView"
        anchors.fill: parent
        contentWidth: width
        contentHeight: body.implicitHeight + 2 * root.contentPadding
        clip: true
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {
            policy: bodyView.interactive ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        }

        ColumnLayout {
            id: body
            objectName: "dialogBody"
            x: root.contentPadding
            y: root.contentPadding
            width: Math.max(0, root.width - 2 * root.contentPadding)
            spacing: root.contentSpacing
            focus: true

            // Enter presses the default button, as in KDE's dialogs; a text
            // field takes its own.
            Keys.onPressed: function(event) {
                if (root.footer && root.footer.handleEnter) {
                    root.footer.handleEnter(event, Window.activeFocusItem, root.visible)
                }
            }
        }
    }
}
