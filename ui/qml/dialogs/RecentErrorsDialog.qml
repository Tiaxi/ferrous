// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/ColorUtils.js" as ColorUtils
import "../logic/ErrorUtils.js" as ErrorUtils
import "../logic/KeyUtils.js" as KeyUtils

// Recent Issues: a popup above the status bar's issues button, newest
// first. Each entry has a glyph, its category and time on a small line, a
// title that names the problem (semibold while new), the rest of the
// message and at most one action. The entries count as seen when it
// closes.
Popup {
    id: root

    required property var errorController
    required property var uiPalette
    required property var windowRoot
    // The actions of the entries, which the window carries out.
    required property var uiBridge
    required property var showInQueue
    required property var showInLibrary
    required property var openPreferencesPage
    required property var openDiagnostics

    readonly property int errorCount: root.errorController.count
    readonly property int unseenCount: root.errorController.unseenCount
    readonly property real hostWidth: (parent && parent.width > 0) ? parent.width : root.windowRoot.width
    readonly property real hostHeight: (parent && parent.height > 0) ? parent.height : root.windowRoot.height
    // Clear of the window's edges: 6 px from the right, and above the
    // status bar.
    readonly property real edgeMargin: Kirigami.Units.smallSpacing + 2
    // Between an entry's glyph and its text, and Open Diagnostics…' too.
    readonly property real glyphTextGap: Kirigami.Units.largeSpacing + 2
    // The rules under the header and over the footer: the frame's colour.
    readonly property color ruleColor: Kirigami.ColorUtils.linearInterpolation(
        root.uiPalette.chrome, root.uiPalette.text, Kirigami.Theme.frameContrast)
    readonly property real statusBarClearance: root.uiPalette.rowHeight + Kirigami.Units.smallSpacing + 2
    // The files of the entries that are in the queue now, by path. It is
    // worked out again whenever the queue's rows change, whatever their
    // count, and whenever an entry arrives, only while the popup shows. The
    // bridge answers each file from an index of the queue's paths, so this
    // costs the same whatever the queue's length.
    property var queuedSubjects: ({})

    function inQueue(path) {
        return path.length > 0 && root.uiBridge.queueContainsPath
            && root.uiBridge.queueContainsPath(path)
    }

    function refreshQueuedSubjects() {
        if (!root.visible) {
            return
        }
        const queued = ({})
        const entries = root.errorController.entries
        for (let i = 0; i < entries.count; ++i) {
            const subject = entries.get(i).subject
            if (root.inQueue(subject)) {
                queued[subject] = true
            }
        }
        root.queuedSubjects = queued
    }

    // A burst of row changes, such as a bulk insert, refreshes once.
    function scheduleQueueRefresh() {
        Qt.callLater(root.refreshQueuedSubjects)
    }

    // The action of an entry, as { kind, text }, or null.
    function entryAction(category, severity, subject) {
        if ((category === ErrorUtils.CategoryPlayback || category === ErrorUtils.CategoryAnalysis)
                && subject.length > 0) {
            return root.queuedSubjects[subject] === true
                ? { kind: "queue", text: I18n.i18nc("@action:button", "Show in Queue") }
                : { kind: "library", text: I18n.i18nc("@action:button", "Show in Library") }
        }
        if (category === ErrorUtils.CategoryLibrary) {
            return { kind: "library-settings", text: I18n.i18nc("@action:button", "Library Settings…") }
        }
        if (category === ErrorUtils.CategoryLastFm) {
            return { kind: "lastfm-settings", text: I18n.i18nc("@action:button", "Last.fm Settings…") }
        }
        return null
    }

    // The action chosen in the popup, which runs once it has closed: the
    // popup gives the keyboard back as it goes, which would take it from
    // the queue or a dialog the action opens.
    property var chosenAction: null

    function runAction(kind, subject) {
        // The queue may have changed since the row was made: a file that has
        // left it gets the action that fits, and the popup stays.
        if (kind === "queue" && !root.inQueue(subject)) {
            root.refreshQueuedSubjects()
            return
        }
        root.chosenAction = { kind: kind, subject: subject }
        root.close()
    }

    function runChosenAction() {
        const action = root.chosenAction
        root.chosenAction = null
        if (!action) {
            return
        }
        switch (action.kind) {
        case "queue":
            root.showInQueue(action.subject)
            break
        case "library":
            root.showInLibrary(action.subject)
            break
        case "library-settings":
            root.openPreferencesPage(0)
            break
        case "lastfm-settings":
            root.openPreferencesPage(4)
            break
        case "diagnostics":
            root.openDiagnostics()
            break
        }
    }

    // At its least: 460 px with the default font, which a larger one scales
    // into the most it takes.
    readonly property real leastWidth: 460
    readonly property real headerWidth: Math.ceil(errorHistoryHeader.implicitWidth
        + errorHistoryHeader.Layout.leftMargin + errorHistoryHeader.Layout.rightMargin)
    // The width it opened at, which it holds while it is open: an issue
    // that arrives wraps in it rather than moving the popup.
    property real openedWidth: root.leastWidth
    // Its rows exist from just before it shows, to be measured, until it
    // has hidden.
    property bool listed: false

    // What it holds needs across, with each text on one line and the room
    // of the list's scroll bar.
    function contentNeeds() {
        let rows = 0
        for (let i = 0; i < historyList.count; ++i) {
            const row = historyList.itemAt(i)
            if (row) {
                rows = Math.max(rows, row.naturalWidth)
            }
        }
        const bar = historyView.ScrollBar.vertical
        return Math.ceil(Math.max(rows + (bar ? bar.implicitWidth : 0),
            diagnosticsButton.Layout.leftMargin + diagnosticsButton.implicitWidth
                + errorHistoryHeader.Layout.rightMargin))
    }

    function showNewest() {
        historyView.contentItem.contentY = 0
    }

    // The control that has the keyboard as the popup opens from the
    // keyboard: the newest entry's action, otherwise Open Diagnostics; not
    // Clear All, which a stray Space would press. The list keeps no frame
    // around it then, and its scroll keys still work from either (see the
    // content's keys).
    function initialFocusItem() {
        for (let i = 0; i < historyList.count; ++i) {
            const row = historyList.itemAt(i)
            if (row && row.actionItem.visible) {
                return row.actionItem
            }
        }
        return diagnosticsButton
    }

    parent: Overlay.overlay
    // Modal so that a press outside only closes it, also on the issues
    // button that opened it; with no dim, as a plain popup.
    modal: true
    dim: false
    focus: true
    // Above the viewer popups.
    z: 100
    // 460 px with the default font. A larger one gives it what its content
    // needs, up to 460 px scaled with it, which a long path gets (it was
    // always that, about 1100 px at 24 pt for half as much content; a fixed
    // 460 px wrapped its heading after four words); or as wide as the
    // header needs, which puts its title, how many are new and Clear All
    // on one line; within the window.
    width: Math.min(Math.max(root.openedWidth, root.headerWidth), root.hostWidth - 2 * root.edgeMargin)
    height: Math.min(root.implicitHeight, root.hostHeight - root.statusBarClearance - root.edgeMargin)
    x: Math.round(root.hostWidth - root.width - root.edgeMargin)
    y: Math.round(root.hostHeight - root.height - root.statusBarClearance)
    padding: 0
    topPadding: Kirigami.Units.smallSpacing
    bottomPadding: Kirigami.Units.smallSpacing
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onAboutToShow: {
        // Reopened while it fades out: closed never comes for that close,
        // so the action chosen before it runs now.
        root.runChosenAction()
        root.errorController.dismissLatest()
        // Not visible yet: the refresh below would skip it.
        root.queuedSubjects = ({})
        root.listed = true
        root.openedWidth = Math.max(root.leastWidth,
            Math.min(root.uiPalette.scaledSize(root.leastWidth), root.contentNeeds()))
        root.showNewest()
    }
    onVisibleChanged: {
        if (!root.visible) {
            root.listed = false
        }
        root.refreshQueuedSubjects()
    }
    // Over a viewer the keyboard would otherwise stay with the viewer.
    // Opened from the keyboard, the first action has it, with its focus
    // ring, as Tab would give it. Opened with the pointer, no control
    // shows a ring, so none has the keyboard, and Enter presses no button
    // the user cannot see marked (a popup's focus reason drew no
    // ring on the action Enter pressed); the content does, whose keys
    // scroll the list, and Tab goes on to the controls. The control is
    // the list's focus within it all the same, which Tab into the list
    // gives the keyboard, rather than the list itself.
    onOpened: {
        const first = root.initialFocusItem()
        if (InputModality.keyboard) {
            first.forceActiveFocus(Qt.TabFocusReason)
        } else {
            first.focus = true
            popupContent.forceActiveFocus(Qt.PopupFocusReason)
        }
    }
    // Issues that arrived while it was open have been seen too.
    onClosed: {
        root.errorController.markSeen()
        root.runChosenAction()
    }
    onErrorCountChanged: {
        if (root.errorCount === 0) {
            root.close()
        }
    }

    // Its buttons take no click as it fades out: Clear All again would clear
    // an issue that arrived meanwhile, unseen.
    Components.PopupClosing {
        id: closingState
        popup: root
    }

    // Closing counts as having seen what arrived, so it must not arrive out
    // of view.
    Connections {
        target: root.errorController
        function onReported() {
            if (root.visible) {
                root.refreshQueuedSubjects()
                root.showNewest()
            }
        }
    }

    // The queue's rows: any edit, a whole new queue of the same length too.
    Connections {
        target: root.visible && root.uiBridge.queueRows
            && typeof root.uiBridge.queueRows.rowCount === "function"
            ? root.uiBridge.queueRows
            : null
        ignoreUnknownSignals: true
        function onModelReset() { root.scheduleQueueRefresh() }
        function onRowsInserted() { root.scheduleQueueRefresh() }
        function onRowsRemoved() { root.scheduleQueueRefresh() }
        function onRowsMoved() { root.scheduleQueueRefresh() }
        function onDataChanged() { root.scheduleQueueRefresh() }
        function onLayoutChanged() { root.scheduleQueueRefresh() }
    }

    // A bridge whose rows are a list rather than a model replaces the list.
    Connections {
        target: root.visible ? root.uiBridge : null
        ignoreUnknownSignals: true
        function onQueueRowsChanged() { root.scheduleQueueRefresh() }
    }

    contentItem: ColumnLayout {
        id: popupContent
        objectName: "errorHistoryContent"
        spacing: 0
        Accessible.role: Accessible.Dialog
        Accessible.name: I18n.i18nc("@title:window", "Recent Issues")

        // Space presses the focused button rather than playing or pausing:
        // the main window's shortcuts stay active under a fullscreen viewer.
        Keys.onShortcutOverride: function(event) {
            event.accepted = KeyUtils.isPlainSpace(event)
        }
        // The arrow, Page, Home and End keys scroll the list from any
        // control of the popup: a key from an entry's action reaches the
        // list itself, and one from the header or the footer arrives here.
        Keys.onPressed: function(event) {
            historyView.scrollForKey(event)
        }
        // Opened with the pointer, the content has the keyboard: Tab goes
        // to the control the keyboard would have opened it on.
        Keys.onTabPressed: function(event) {
            event.accepted = popupContent.Window.activeFocusItem === popupContent
            if (event.accepted) {
                root.initialFocusItem().forceActiveFocus(Qt.TabFocusReason)
            }
        }

        RowLayout {
            id: errorHistoryHeader
            objectName: "errorHistoryHeader"
            Layout.fillWidth: true
            // 32 px with the default font, and at a larger one as tall as
            // the title with 4 px above and below, and Clear All: at 24 pt,
            // the title ran into the first issue.
            Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit + 2 * Kirigami.Units.smallSpacing + 6,
                Math.ceil(errorHistoryTitle.implicitHeight) + 2 * Kirigami.Units.smallSpacing,
                Math.ceil(errorHistoryClearButton.implicitHeight))
            Layout.leftMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
            Layout.rightMargin: Kirigami.Units.smallSpacing + 2
            spacing: Kirigami.Units.largeSpacing

            Label {
                id: errorHistoryTitle
                objectName: "errorHistoryTitle"
                textFormat: Text.PlainText
                text: I18n.i18nc("@title", "Recent Issues")
                font.weight: Font.DemiBold
                color: root.uiPalette.text
                Accessible.role: Accessible.Heading
            }

            Label {
                objectName: "errorHistoryNewCount"
                visible: root.unseenCount > 0
                textFormat: Text.PlainText
                text: I18n.i18ncp("@info how many issues are new", "%1 new", "%1 new", root.unseenCount)
                font.pointSize: root.uiPalette.smallPointSize
                font.features: root.uiPalette.tabularFigures
                color: root.uiPalette.textTertiary
            }

            Item { Layout.fillWidth: true }

            Components.TextToolButton {
                id: errorHistoryClearButton
                objectName: "errorHistoryClearButton"
                text: I18n.i18nc("@action:button", "Clear All")
                // A glyph, as every dialog action has: without one it read
                // as a label of the header.
                icon.name: "edit-clear-history"
                flat: true
                enabled: !closingState.closing
                onClicked: root.errorController.clear()
            }
        }

        // A hairline under the header, as over the footer: the header is
        // the popup's, apart from the list it heads (with a rule only over
        // the footer, the title stood on the first issue).
        Rectangle {
            objectName: "errorHistoryHeaderLine"
            Layout.fillWidth: true
            implicitHeight: 1
            color: root.ruleColor
        }

        Components.KeyScrollView {
            id: historyView
            objectName: "errorHistoryView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: historyColumn.implicitHeight
            Accessible.role: Accessible.List
            Accessible.name: I18n.i18nc("@label", "Recent issues")
            Accessible.description: I18n.i18nc("@info", "Scroll with the arrow and Page keys")
            Accessible.focusable: true

            Column {
                id: historyColumn
                width: historyView.availableWidth

                // The history is short: every row exists, so Tab reaches them all.
                Repeater {
                    id: historyList
                    objectName: "errorHistoryList"
                    // Builds rows only while the popup is shown.
                    model: root.listed ? root.errorController.entries : null

                    delegate: Item {
                        id: historyRow
                        objectName: "errorHistoryRow"
                        required property int index
                        required property int category
                        required property int severity
                        required property string subject
                        required property string message
                        required property string detail
                        required property int count
                        required property double lastSeenMs
                        required property bool unseen
                        readonly property var entry: ({ category: historyRow.category, message: historyRow.message,
                                                        count: historyRow.count })
                        readonly property string categoryText: ErrorUtils.categoryLabel(historyRow.category)
                        readonly property string title: ErrorUtils.issueTitle(historyRow.entry)
                        readonly property string description: ErrorUtils.issueDescription(historyRow.entry)
                        readonly property string timeText: ErrorUtils.issueTimeText(historyRow.lastSeenMs)
                        readonly property int glyph: ErrorUtils.issueGlyph(historyRow.category, historyRow.severity)
                        readonly property var action: root.entryAction(
                            historyRow.category, historyRow.severity, historyRow.subject)
                        // The file the entry is about, when its text does not name it.
                        readonly property bool showsSubject: historyRow.subject.length > 0
                            && historyRow.message.indexOf(historyRow.subject.split("/").pop()) < 0
                        readonly property Item actionItem: actionButton
                        // Across with each of its texts on one line.
                        readonly property real naturalWidth: rowLayout.anchors.leftMargin + rowLayout.anchors.rightMargin
                            + severityGlyph.implicitWidth + rowLayout.spacing
                            + Math.max(categoryLabel.implicitWidth + categoryLine.spacing + timeLabel.implicitWidth,
                                headingLabel.implicitWidth,
                                historyRow.description.length > 0 ? messageLabel.implicitWidth : 0,
                                historyRow.showsSubject ? subjectLabel.implicitWidth : 0,
                                historyRow.detail.length > 0 ? detailLabel.implicitWidth : 0,
                                historyRow.action !== null ? actionButton.implicitWidth : 0)

                        width: historyColumn.width
                        implicitHeight: rowLayout.implicitHeight + Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing + 2
                        Accessible.role: Accessible.ListItem
                        Accessible.name: historyRow.description.length > 0
                            ? I18n.i18nc("@info issue title, category, time, description", "%1 (%2, %3): %4",
                                historyRow.title, historyRow.categoryText, historyRow.timeText, historyRow.description)
                            : I18n.i18nc("@info issue title, category, time", "%1 (%2, %3)",
                                historyRow.title, historyRow.categoryText, historyRow.timeText)
                        // The file, then the system's text, as the lines show them.
                        Accessible.description: [historyRow.subject, historyRow.detail]
                            .filter(function(text) { return text.length > 0 }).join("\n")

                        // A hairline over each entry but the first, from the
                        // text's left edge, past the glyph column, to the
                        // row's right margin: entries of several lines read
                        // apart without crossing the glyphs' column (space
                        // alone ran them together, while the header and the
                        // footer have rules). The seam's colour, a step
                        // quieter than those rules.
                        Rectangle {
                            objectName: "errorHistoryEntryLine"
                            visible: historyRow.index > 0
                            x: rowLayout.anchors.leftMargin + severityGlyph.width + rowLayout.spacing
                            width: historyRow.width - x - rowLayout.anchors.rightMargin
                            height: 1
                            color: root.uiPalette.seam
                        }

                        RowLayout {
                            id: rowLayout
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: Kirigami.Units.largeSpacing
                            anchors.leftMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
                            anchors.rightMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
                            spacing: root.glyphTextGap

                            // The entry's severity, as the status bar marks
                            // it: as large as a line of text holds, and
                            // centred on the title's lowercase, the middle
                            // of its x-height, beside the words the eye
                            // reads first (on the category's line above it,
                            // it hung beside the quietest text).
                            Kirigami.Icon {
                                id: severityGlyph
                                objectName: "errorHistoryGlyph"
                                Layout.alignment: Qt.AlignTop
                                Layout.topMargin: Math.round(headingLabel.y + headingLabel.baselineOffset
                                    - headingMetrics.xHeight / 2 - implicitHeight / 2)
                                implicitWidth: root.uiPalette.labelIconSize
                                implicitHeight: root.uiPalette.labelIconSize
                                // An error's ring with a cross in the negative
                                // colour, a warning's triangle in the
                                // secondary one: shape and colour both.
                                source: Qt.resolvedUrl("../assets/" + ErrorUtils.glyphAsset(historyRow.glyph))
                                isMask: true
                                color: ErrorUtils.glyphColor(historyRow.glyph, root.uiPalette)
                                Accessible.ignored: true
                            }

                            // At the row's top whatever the glyph's margin
                            // (centred, it moved down by what the glyph
                            // beside a short entry's title stood below it).
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignTop
                                spacing: 2

                                // What the issue is about and when, as a
                                // notification's header line: the quietest
                                // text of the entry.
                                RowLayout {
                                    id: categoryLine
                                    Layout.fillWidth: true
                                    spacing: Kirigami.Units.largeSpacing

                                    Label {
                                        id: categoryLabel
                                        objectName: "errorHistoryCategory"
                                        Layout.fillWidth: true
                                        text: historyRow.categoryText
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        font.pointSize: root.uiPalette.smallPointSize
                                        color: root.uiPalette.textTertiary
                                        Accessible.ignored: true

                                    }

                                    Label {
                                        id: timeLabel
                                        objectName: "errorHistoryTime"
                                        textFormat: Text.PlainText
                                        text: historyRow.timeText
                                        font.pointSize: root.uiPalette.smallPointSize
                                        font.features: root.uiPalette.tabularFigures
                                        color: root.uiPalette.textTertiary
                                        Accessible.ignored: true
                                    }
                                }

                                // The problem itself. A path or a system
                                // error in it wraps as a message's does.
                                Components.MessageLabel {
                                    id: headingLabel
                                    objectName: "errorHistoryHeading"
                                    Layout.fillWidth: true
                                    message: historyRow.title
                                    font.weight: historyRow.unseen ? Font.DemiBold : Font.Normal
                                    color: root.uiPalette.text
                                    Accessible.ignored: true

                                    FontMetrics {
                                        id: headingMetrics
                                        font: headingLabel.font
                                    }
                                }

                                Components.MessageLabel {
                                    id: messageLabel
                                    objectName: "errorHistoryMessage"
                                    Layout.fillWidth: true
                                    visible: message.length > 0
                                    message: historyRow.description
                                    font.pointSize: root.uiPalette.smallPointSize
                                    color: root.uiPalette.textSecondary
                                    Accessible.ignored: true
                                }

                                Label {
                                    id: subjectLabel
                                    objectName: "errorHistorySubject"
                                    Layout.fillWidth: true
                                    visible: historyRow.showsSubject
                                    text: historyRow.subject
                                    textFormat: Text.PlainText
                                    elide: Text.ElideMiddle
                                    font.pointSize: root.uiPalette.smallPointSize
                                    // What the action acts on: over the time
                                    // and the category (it had their ink).
                                    color: root.uiPalette.textSecondary
                                    Accessible.ignored: true
                                }

                                // The system's text, such as "failed to
                                // replace …: Permission denied (os error
                                // 13)", under the file, a step quieter than
                                // it: the title says in plain words what
                                // went wrong, and the file is what the
                                // action acts on (in the file's ink, the
                                // two read as one block).
                                Components.MessageLabel {
                                    id: detailLabel
                                    objectName: "errorHistoryDetail"
                                    Layout.fillWidth: true
                                    visible: historyRow.detail.length > 0
                                    message: historyRow.detail
                                    font.pointSize: root.uiPalette.smallPointSize
                                    color: root.uiPalette.textTertiary
                                    Accessible.ignored: true
                                }

                                ToolButton {
                                    id: actionButton
                                    objectName: "errorHistoryActionButton"
                                    visible: historyRow.action !== null
                                    enabled: !closingState.closing
                                    Layout.leftMargin: -leftPadding
                                    flat: true
                                    focusPolicy: Qt.StrongFocus
                                    text: historyRow.action ? historyRow.action.text : ""
                                    padding: Kirigami.Units.smallSpacing
                                    leftPadding: Kirigami.Units.smallSpacing + 2
                                    rightPadding: Kirigami.Units.smallSpacing + 2
                                    // Link-coloured small text on a flat button: the style
                                    // draws the text of its own background, so both are
                                    // drawn here, in the states Breeze gives the dialogs'
                                    // other flat actions, Preview, Apply, the Reverts and
                                    // this popup's own two: the accent's outline alone on
                                    // hover (it was a grey fill of its own), and pressed,
                                    // the accent at 30 % within it, at once, as theirs are.
                                    // The link is darker or lighter where that fill takes
                                    // it under 4.6:1 on the window colour, a margin over
                                    // 4.5:1 that the fill's rounding to pixels cannot take
                                    // away (at 4.5:1 it drew at 4.49:1 in Breeze Light).
                                    readonly property color tint: actionButton.down
                                        ? ColorUtils.withAlpha(root.uiPalette.accent, 0.3)
                                        : "transparent"
                                    contentItem: Label {
                                        textFormat: Text.PlainText
                                        text: actionButton.text
                                        font.pointSize: root.uiPalette.smallPointSize
                                        color: actionButton.tint.a > 0
                                            ? root.uiPalette.readableOn(root.uiPalette.link,
                                                [ColorUtils.drawnOver(actionButton.tint, root.uiPalette.chrome)], 4.6)
                                            : root.uiPalette.link
                                        Accessible.ignored: true
                                    }
                                    background: Rectangle {
                                        radius: Kirigami.Units.cornerRadius
                                        color: actionButton.tint
                                        border.width: actionButton.visualFocus
                                            ? 2
                                            : (actionButton.down || actionButton.hovered ? 1 : 0)
                                        border.color: actionButton.visualFocus
                                            ? root.uiPalette.focusRing
                                            : root.uiPalette.accent
                                    }
                                    onActiveFocusChanged: {
                                        if (activeFocus) {
                                            historyView.reveal(historyRow.y, historyRow.y + historyRow.height)
                                        }
                                    }
                                    onClicked: root.runAction(historyRow.action.kind, historyRow.subject)
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            objectName: "errorHistoryFooterLine"
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing
            Layout.bottomMargin: Kirigami.Units.smallSpacing
            implicitHeight: 1
            color: root.ruleColor
        }

        // On the header's x, with the glyph of Help's Diagnostics…, as Clear
        // All has its own (they were the only dialog actions with none).
        // Not ⓘ, which marks a notice in the list. Its glyph is on the
        // entries' glyphs, the same size, and its caption on their text, as
        // far from the glyph as theirs (at the button's own spacing it was
        // a second text column, 6 px left of the headings).
        Components.TextToolButton {
            id: diagnosticsButton
            objectName: "errorHistoryDiagnosticsButton"
            Layout.leftMargin: errorHistoryHeader.Layout.leftMargin - diagnosticsButton.leftPadding
            spacing: root.glyphTextGap
            flat: true
            text: I18n.i18nc("@action:button", "Open Diagnostics…")
            icon.name: "view-list-details"
            enabled: !closingState.closing
            onClicked: {
                root.chosenAction = { kind: "diagnostics", subject: "" }
                root.close()
            }
        }
    }
}
