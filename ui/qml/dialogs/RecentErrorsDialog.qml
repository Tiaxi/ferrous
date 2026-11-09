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
// first. Each entry has a glyph, a title (semibold while new), its time, a
// description and at most one action. The entries count as seen when it
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
            root.openPreferencesPage(3)
            break
        case "diagnostics":
            root.openDiagnostics()
            break
        }
    }

    function showNewest() {
        historyView.contentItem.contentY = 0
    }

    parent: Overlay.overlay
    // Modal so that a press outside only closes it, also on the issues
    // button that opened it; with no dim, as a plain popup.
    modal: true
    dim: false
    focus: true
    // Above the viewer popups.
    z: 100
    width: Math.min(460, root.hostWidth - 2 * root.edgeMargin)
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
        root.showNewest()
    }
    onVisibleChanged: root.refreshQueuedSubjects()
    // Over a viewer the keyboard would otherwise stay with the viewer.
    onOpened: historyView.forceActiveFocus(Qt.PopupFocusReason)
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
        spacing: 0
        Accessible.role: Accessible.Dialog
        Accessible.name: I18n.i18nc("@title:window", "Recent Issues")

        // Space presses the focused button rather than playing or pausing:
        // the main window's shortcuts stay active under a fullscreen viewer.
        Keys.onShortcutOverride: function(event) {
            event.accepted = KeyUtils.isPlainSpace(event)
        }

        RowLayout {
            objectName: "errorHistoryHeader"
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit + 2 * Kirigami.Units.smallSpacing + 6
            Layout.leftMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
            Layout.rightMargin: Kirigami.Units.smallSpacing + 2
            spacing: Kirigami.Units.largeSpacing

            Label {
                objectName: "errorHistoryTitle"
                text: I18n.i18nc("@title", "Recent Issues")
                font.weight: Font.DemiBold
                color: root.uiPalette.text
                Accessible.role: Accessible.Heading
            }

            Label {
                objectName: "errorHistoryNewCount"
                visible: root.unseenCount > 0
                text: I18n.i18ncp("@info how many issues are new", "%1 new", "%1 new", root.unseenCount)
                font.pointSize: root.uiPalette.smallPointSize
                font.features: root.uiPalette.tabularFigures
                color: root.uiPalette.textTertiary
            }

            Item { Layout.fillWidth: true }

            ToolButton {
                objectName: "errorHistoryClearButton"
                text: I18n.i18nc("@action:button", "Clear All")
                flat: true
                enabled: !closingState.closing
                onClicked: root.errorController.clear()
            }
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
                    model: root.visible ? root.errorController.entries : null

                    delegate: Item {
                        id: historyRow
                        objectName: "errorHistoryRow"
                        required property int index
                        required property int category
                        required property int severity
                        required property string subject
                        required property string message
                        required property int count
                        required property double lastSeenMs
                        required property bool unseen
                        readonly property var entry: ({ category: historyRow.category, message: historyRow.message,
                                                        count: historyRow.count })
                        readonly property string title: ErrorUtils.issueTitle(historyRow.entry)
                        readonly property string description: ErrorUtils.issueDescription(historyRow.entry)
                        readonly property string timeText: ErrorUtils.issueTimeText(historyRow.lastSeenMs)
                        readonly property int glyph: ErrorUtils.issueGlyph(historyRow.category, historyRow.severity)
                        readonly property var action: root.entryAction(
                            historyRow.category, historyRow.severity, historyRow.subject)
                        // The file the entry is about, when its text does not name it.
                        readonly property bool showsSubject: historyRow.subject.length > 0
                            && historyRow.message.indexOf(historyRow.subject.split("/").pop()) < 0

                        width: historyColumn.width
                        implicitHeight: rowLayout.implicitHeight + Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing + 2
                        Accessible.role: Accessible.ListItem
                        Accessible.name: I18n.i18nc("@info issue title, time, description", "%1, %2: %3",
                            historyRow.title, historyRow.timeText, historyRow.description)
                        Accessible.description: historyRow.subject

                        RowLayout {
                            id: rowLayout
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: Kirigami.Units.largeSpacing
                            anchors.leftMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
                            anchors.rightMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
                            spacing: Kirigami.Units.largeSpacing + 2

                            Kirigami.Icon {
                                objectName: "errorHistoryGlyph"
                                Layout.alignment: Qt.AlignTop
                                Layout.topMargin: 2
                                implicitWidth: Kirigami.Units.iconSizes.small
                                implicitHeight: Kirigami.Units.iconSizes.small
                                source: historyRow.glyph === ErrorUtils.GlyphOffline
                                    ? Qt.resolvedUrl("../assets/status-offline.svg")
                                    : (historyRow.glyph === ErrorUtils.GlyphInformation
                                        ? "help-about"
                                        : Qt.resolvedUrl("../assets/status-warning.svg"))
                                isMask: true
                                color: historyRow.glyph === ErrorUtils.GlyphWarning
                                    ? root.uiPalette.negative
                                    : root.uiPalette.textSecondary
                                Accessible.ignored: true
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Kirigami.Units.largeSpacing

                                    Label {
                                        objectName: "errorHistoryHeading"
                                        Layout.fillWidth: true
                                        text: historyRow.title
                                        textFormat: Text.PlainText
                                        wrapMode: Text.Wrap
                                        font.weight: historyRow.unseen ? Font.DemiBold : Font.Normal
                                        color: root.uiPalette.text
                                        Accessible.ignored: true
                                    }

                                    Label {
                                        objectName: "errorHistoryTime"
                                        Layout.alignment: Qt.AlignTop
                                        text: historyRow.timeText
                                        font.pointSize: root.uiPalette.smallPointSize
                                        font.features: root.uiPalette.tabularFigures
                                        color: root.uiPalette.textTertiary
                                        Accessible.ignored: true
                                    }
                                }

                                Label {
                                    objectName: "errorHistoryMessage"
                                    Layout.fillWidth: true
                                    visible: text.length > 0
                                    text: historyRow.description
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    font.pointSize: root.uiPalette.smallPointSize
                                    color: root.uiPalette.textSecondary
                                    Accessible.ignored: true
                                }

                                Label {
                                    objectName: "errorHistorySubject"
                                    Layout.fillWidth: true
                                    visible: historyRow.showsSubject
                                    text: historyRow.subject
                                    textFormat: Text.PlainText
                                    elide: Text.ElideMiddle
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
                                    // drawn here.
                                    contentItem: Label {
                                        text: actionButton.text
                                        font.pointSize: root.uiPalette.smallPointSize
                                        color: root.uiPalette.link
                                        Accessible.ignored: true
                                    }
                                    background: Rectangle {
                                        radius: 3
                                        color: actionButton.down
                                            ? ColorUtils.withAlpha(root.uiPalette.text, 0.10)
                                            : (actionButton.hovered ? root.uiPalette.hover : "transparent")
                                        border.width: actionButton.visualFocus ? 2 : 0
                                        border.color: root.uiPalette.focusRing

                                        Behavior on color {
                                            enabled: Kirigami.Units.shortDuration > 0
                                            ColorAnimation {
                                                duration: Kirigami.Units.shortDuration
                                                easing.type: Easing.OutCubic
                                            }
                                        }
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
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing
            Layout.bottomMargin: Kirigami.Units.smallSpacing
            implicitHeight: 1
            color: Kirigami.ColorUtils.linearInterpolation(
                root.uiPalette.chrome, root.uiPalette.text, Kirigami.Theme.frameContrast)
        }

        ToolButton {
            objectName: "errorHistoryDiagnosticsButton"
            Layout.leftMargin: Kirigami.Units.smallSpacing + 2
            flat: true
            icon.name: "help-about"
            text: I18n.i18nc("@action:button", "Open Diagnostics…")
            enabled: !closingState.closing
            onClicked: {
                root.chosenAction = { kind: "diagnostics", subject: "" }
                root.close()
            }
        }
    }
}
