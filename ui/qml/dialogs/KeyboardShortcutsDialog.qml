// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import org.kde.kirigami 2.20 as Kirigami
import org.kde.kquickcontrols as KQuickControls
import FerrousUi 1.0
import "../components" as Components
import "." as Dialogs
import "../logic/KeyUtils.js" as KeyUtils

// Configure Keyboard Shortcuts, laid out as KDE's KShortcutsDialog: a
// search field, the actions by menu with their shortcut and alternate, and
// under them the selected action's Default and Custom choice and its two
// keys. The changes wait until OK or Apply, which send the whole set of the
// shortcuts the user changed (only those that differ from the defaults);
// Cancel drops them, and Defaults sets every action back to its defaults,
// to be applied in turn. A standard key's defaults follow Plasma's
// Shortcuts settings.
//
// A key another action has is moved to the new one only once the user
// agrees, as in KDE's editor; a key the window, its lists or its text
// fields keep is refused, saying what keeps it. KeySequenceItem checks
// KDE's global and standard shortcuts itself, before this dialog sees the
// key.
Components.DialogWindow {
    id: root

    required property var uiBridge
    // Main.qml's shortcutActionGroups: { name, title, actions }.
    required property var actionGroups
    // The actions the viewers repeat, where their own keys count too.
    property var viewerActions: []
    // The shortcuts KeySequenceItem checks a new key against, and asks
    // about: KDE's global and standard ones. Tests, which have no KDE
    // session, check none.
    property int keyConflictChecks: KQuickControls.ShortcutType.GlobalShortcuts
        | KQuickControls.ShortcutType.StandardShortcuts

    readonly property var actions: root.actionGroups.reduce(
        function(all, group) { return all.concat(group.actions) }, [])
    // Without a prototype, so that an id such as "constructor" names no
    // property of Object's.
    readonly property var actionsById: {
        const byId = Object.create(null)
        for (let i = 0; i < root.actions.length; ++i) {
            byId[root.actions[i].shortcutId] = root.actions[i]
        }
        return byId
    }

    // The bindings the user changed and has yet to apply, by shortcutId:
    // each action's whole list, as portable text, the first its shortcut.
    property var pending: ({})
    // A search by key follows the keys as they change.
    onPendingChanged: {
        if (searchField.text.trim().length > 0) {
            root.rebuildRows(true)
        }
    }
    readonly property bool modified: Object.keys(root.pending).length > 0
    readonly property bool atDefaults: {
        for (let i = 0; i < root.actions.length; ++i) {
            const action = root.actions[i]
            if (!root.sameBindings(root.bindingsOf(action), action.defaultKeyBindings)) {
                return false
            }
        }
        return true
    }

    // The row the editor under the list is for.
    property string currentShortcutId: ""
    readonly property var currentAction: root.actionsById[root.currentShortcutId] || null
    readonly property var currentBindings: root.currentAction ? root.bindingsOf(root.currentAction) : []
    readonly property bool currentAtDefaults: !root.currentAction
        || root.sameBindings(root.currentBindings, root.currentAction.defaultKeyBindings)
    // Whether the user chose Custom for an action that still has its
    // defaults, to give it keys of its own.
    property bool customChosen: false

    // Why the latest key was refused, or that the shortcuts could not be
    // saved; empty for none.
    property string refusal: ""
    property bool saveFailed: false
    // The bindings waiting for the user to agree to take keys from other
    // actions: { action, bindings, moves: [{ key, owner }] }.
    property var pendingReassignment: null

    // The action's name, as the menus show it, without its mnemonic.
    function nameOf(action) {
        const text = action.shortcutText.length > 0 ? action.shortcutText : action.text
        return text.replace(/&&/g, "\u0000").replace(/&/g, "").replace(/\u0000/g, "&")
    }

    function iconOf(action) {
        return action.icon.name.length > 0 ? action.icon.name : action.icon.source
    }

    // What the dialog shows for `action`: the change waiting to be
    // applied, or its bindings as they are.
    function bindingsOf(action) {
        const waiting = root.pending[action.shortcutId]
        return waiting !== undefined ? waiting : action.keyBindings
    }

    function sameBindings(a, b) {
        if (a.length !== b.length) {
            return false
        }
        for (let i = 0; i < a.length; ++i) {
            if (a[i] !== b[i]) {
                return false
            }
        }
        return true
    }

    function setBindings(action, bindings) {
        const changed = Object.assign({}, root.pending)
        if (root.sameBindings(bindings, action.keyBindings)) {
            delete changed[action.shortcutId]
        } else {
            changed[action.shortcutId] = bindings
        }
        root.pending = changed
    }

    // `action`'s bindings with `key` as its shortcut (slot 0) or its
    // alternate (slot 1), or without that one where `key` is empty. A new
    // alternate replaces every binding after the first, such as a standard
    // key's third; a shortcut cleared leaves the alternate the shortcut.
    function withKey(bindings, slot, key) {
        let next
        if (slot === 0) {
            next = (key.length > 0 ? [key] : []).concat(bindings.slice(1))
        } else {
            next = bindings.slice(0, 1).concat(key.length > 0 ? [key] : [])
        }
        return next.filter(function(binding, index) { return next.indexOf(binding) === index })
    }

    // The action other than `except` that has `key`, if one has.
    function ownerOf(key, except) {
        for (let i = 0; i < root.actions.length; ++i) {
            const action = root.actions[i]
            if (action !== except && root.bindingsOf(action).indexOf(key) >= 0) {
                return action
            }
        }
        return null
    }

    // Why `action` may not take `key`, in a sentence; empty if it may.
    function refusalFor(action, key) {
        const name = StandardKeys.nativeText(key)
        switch (StandardKeys.keptKeyRole(key)) {
        case "activate":
            return I18n.i18nc("@info %1 is a key",
                "‘%1’ cannot be a shortcut: it plays the selection in the library and the queue, and presses the default button of a dialog.", name)
        case "delete":
            return I18n.i18nc("@info %1 is a key",
                "‘%1’ cannot be a shortcut: it removes the selected tracks from the queue, and deletes text in text fields.", name)
        case "focus":
            return I18n.i18nc("@info %1 is a key",
                "‘%1’ cannot be a shortcut: it moves the keyboard focus between the controls.", name)
        case "cancel":
            return I18n.i18nc("@info %1 is a key",
                "‘%1’ cannot be a shortcut: it closes menus, dialogs and the viewers.", name)
        case "navigate":
            return I18n.i18nc("@info %1 is a key",
                "‘%1’ cannot be a shortcut: it moves the selection in the lists and the cursor in text fields.", name)
        case "contextMenu":
            return I18n.i18nc("@info %1 is a key",
                "‘%1’ cannot be a shortcut: it opens the context menu.", name)
        case "menuBar":
            return I18n.i18nc("@info %1 is a key",
                "‘%1’ cannot be a shortcut: KDE applications use it to show or hide the menu bar.", name)
        }
        // The album art viewer's own key, where the viewers repeat the
        // action.
        if (key === "I" && root.viewerActions.indexOf(action) >= 0) {
            return I18n.i18nc("@info %1 is a key",
                "‘%1’ cannot be a shortcut of this action: the album art viewer uses it to show or hide the details.", name)
        }
        return ""
    }

    // The first of `bindings` that the library and text fields take for
    // typing while they have the keyboard focus, if any.
    function typedKeyOf(bindings) {
        for (let i = 0; i < bindings.length; ++i) {
            if (StandardKeys.typesCharacter(bindings[i])) {
                return bindings[i]
            }
        }
        return ""
    }

    // Takes the key `editor` captured as the current action's shortcut
    // (slot 0) or alternate (slot 1), or its clearing, once it passes the
    // checks.
    function takeKey(slot, editor) {
        const action = root.currentAction
        if (!action) {
            return
        }
        const key = StandardKeys.portableText(editor.keySequence)
        root.refusal = ""
        root.customChosen = false
        const bindings = root.bindingsOf(action)
        if (key.length === 0) {
            root.setBindings(action, root.withKey(bindings, slot, ""))
            root.syncEditors()
            return
        }
        const refusal = root.refusalFor(action, key)
        if (refusal.length > 0) {
            root.refusal = refusal
            root.syncEditors()
            return
        }
        // Until the user agrees, the editors show the keys as they were.
        root.syncEditors()
        root.giveBindings(action, root.withKey(bindings, slot, key))
    }

    // Gives `action` `bindings`, asking first if another action has one of
    // those it does not have yet, which that action then loses.
    function giveBindings(action, bindings) {
        const had = root.bindingsOf(action)
        const moves = []
        for (let i = 0; i < bindings.length; ++i) {
            const owner = had.indexOf(bindings[i]) < 0 ? root.ownerOf(bindings[i], action) : null
            if (owner) {
                moves.push({ key: bindings[i], owner: owner })
            }
        }
        if (moves.length === 0) {
            root.setBindings(action, bindings)
            return
        }
        root.pendingReassignment = { action: action, bindings: bindings, moves: moves }
        reassignDialog.open()
    }

    function reassign() {
        const change = root.pendingReassignment
        root.pendingReassignment = null
        if (!change) {
            return
        }
        for (let i = 0; i < change.moves.length; ++i) {
            const move = change.moves[i]
            root.setBindings(move.owner, root.bindingsOf(move.owner).filter(function(binding) {
                return binding !== move.key
            }))
        }
        root.setBindings(change.action, change.bindings)
    }

    // The keys and the actions a reassignment takes them from, as the
    // question names them.
    function movedKeysText(change) {
        return change.moves.map(function(move) { return StandardKeys.nativeText(move.key) })
            .join(I18n.i18nc("@info separator of a list of keys", ", "))
    }

    function movedFromText(change) {
        const names = []
        for (let i = 0; i < change.moves.length; ++i) {
            const name = root.nameOf(change.moves[i].owner)
            if (names.indexOf(name) < 0) {
                names.push(name)
            }
        }
        return names.join(I18n.i18nc("@info separator of a list of actions", ", "))
    }

    // Default: its defaults, which another action may have been given
    // since.
    function setCurrentToDefaults() {
        if (root.currentAction) {
            root.refusal = ""
            root.customChosen = false
            root.giveBindings(root.currentAction, root.currentAction.defaultKeyBindings)
        }
    }

    // The custom keys: an action at its defaults starts from them, and the
    // shortcut's capture starts.
    function chooseCustom() {
        if (root.currentAction && root.currentAtDefaults) {
            root.customChosen = true
            shortcutEditor.startCapturing()
        }
    }

    function restoreDefaults() {
        root.refusal = ""
        root.customChosen = false
        for (let i = 0; i < root.actions.length; ++i) {
            root.setBindings(root.actions[i], root.actions[i].defaultKeyBindings)
        }
    }

    // The shortcuts the user changed, as the bridge keeps them: those of
    // the actions whose bindings differ from their defaults, empty for
    // none, and those of actions this build does not have, as they were.
    function overrides() {
        const result = Object.create(null)
        const stored = root.uiBridge.keyboardShortcuts || ({})
        for (const id of Object.keys(stored)) {
            if (root.actionsById[id] === undefined) {
                result[id] = stored[id]
            }
        }
        for (let i = 0; i < root.actions.length; ++i) {
            const action = root.actions[i]
            const bindings = root.bindingsOf(action)
            if (!root.sameBindings(bindings, action.defaultKeyBindings)) {
                result[action.shortcutId] = StandardKeys.joinSequences(bindings)
            }
        }
        return result
    }

    // Sends the changes; whether the bridge took them.
    function applyChanges() {
        if (!root.modified) {
            root.saveFailed = false
            return true
        }
        if (!root.uiBridge.setKeyboardShortcuts(root.overrides())) {
            root.saveFailed = true
            return false
        }
        root.saveFailed = false
        root.pending = ({})
        return true
    }

    function discardChanges() {
        root.pending = ({})
        root.refusal = ""
        root.saveFailed = false
        root.customChosen = false
        root.pendingReassignment = null
    }

    // The editors show the current action's keys. KeySequenceItem keeps
    // what it captured until told otherwise, so a key refused, or not yet
    // agreed to, is put back here.
    function syncEditors() {
        const bindings = root.currentBindings
        shortcutEditor.keySequence = bindings.length > 0 ? bindings[0] : ""
        alternateEditor.keySequence = bindings.length > 1 ? bindings[1] : ""
    }
    onCurrentBindingsChanged: root.syncEditors()

    function matches(action, query) {
        if (query.length === 0) {
            return true
        }
        if (root.nameOf(action).toLowerCase().indexOf(query) >= 0) {
            return true
        }
        const bindings = root.bindingsOf(action)
        for (let i = 0; i < bindings.length; ++i) {
            if (bindings[i].toLowerCase().indexOf(query) >= 0
                    || StandardKeys.nativeText(bindings[i]).toLowerCase().indexOf(query) >= 0) {
                return true
            }
        }
        return false
    }

    // The rows that match the search, by menu; the current row stays
    // current while it shows, or the first one becomes current. After an
    // edit (`keepCurrent`), the row edited stays, though its new keys may
    // no longer match.
    function rebuildRows(keepCurrent) {
        const query = searchField.text.trim().toLowerCase()
        const currentId = keepCurrent ? root.currentShortcutId : ""
        rowsModel.clear()
        let currentRow = -1
        for (let g = 0; g < root.actionGroups.length; ++g) {
            const group = root.actionGroups[g]
            for (let a = 0; a < group.actions.length; ++a) {
                const action = group.actions[a]
                if (action.shortcutId === currentId || root.matches(action, query)) {
                    if (action.shortcutId === root.currentShortcutId) {
                        currentRow = rowsModel.count
                    }
                    rowsModel.append({ shortcutId: action.shortcutId, groupTitle: group.title })
                }
            }
        }
        if (currentRow < 0 && rowsModel.count > 0) {
            currentRow = 0
        }
        actionList.currentIndex = currentRow
        root.selectRow(currentRow)
    }

    // Makes the row of `shortcutId` current, as a click on it does; whether
    // the list shows it.
    function selectAction(shortcutId) {
        for (let row = 0; row < rowsModel.count; ++row) {
            if (rowsModel.get(row).shortcutId === shortcutId) {
                actionList.currentIndex = row
                return true
            }
        }
        return false
    }

    function selectRow(row) {
        const id = row >= 0 && row < rowsModel.count ? rowsModel.get(row).shortcutId : ""
        if (id !== root.currentShortcutId) {
            root.refusal = ""
            root.customChosen = false
            root.currentShortcutId = id
        }
        root.syncEditors()
    }

    ListModel {
        id: rowsModel
    }

    title: I18n.i18nc("@title:window %1 is the application name", "Configure Keyboard Shortcuts — %1", "Ferrous")
    // The columns of the keys, as wide as a long one such as
    // Ctrl+Shift+Space, scaled with the font.
    readonly property real keyColumnWidth: Math.ceil(Math.max(Kirigami.Units.gridUnit * 9,
        keyMetrics.advanceWidth("Ctrl+Shift+Backspace") + 2 * Kirigami.Units.largeSpacing))
    readonly property real iconSize: root.uiPalette.labelIconSize
    readonly property real rowLeftPadding: Kirigami.Units.largeSpacing
    readonly property real iconSpacing: Kirigami.Units.largeSpacing
    defaultWidth: root.uiPalette.scaledSize(Kirigami.Units.gridUnit * 40)
    defaultHeight: root.uiPalette.scaledSize(Kirigami.Units.gridUnit * 38)
    minimumWidth: Math.ceil(Kirigami.Units.gridUnit * 14 + 2 * root.keyColumnWidth + 2 * Kirigami.Units.gridUnit)
    minimumHeight: Kirigami.Units.gridUnit * 24

    FontMetrics {
        id: keyMetrics
        font: root.uiPalette.baseFont
    }

    onAboutToShow: {
        root.discardChanges()
        searchField.text = ""
        root.currentShortcutId = ""
        root.rebuildRows()
        // As in KDE's dialog, the keyboard starts in the search field.
        searchField.forceActiveFocus(Qt.PopupFocusReason)
    }
    // Cancel, Escape and the close button drop what was not applied.
    onRejected: root.discardChanges()

    // Defaults on the left; OK, Apply and Cancel in KDE's order on the
    // right, below a seam, as in Configure Ferrous.
    footer: Control {
        id: shortcutsFooter
        objectName: "keyboardShortcutsButtons"
        leftPadding: 0
        rightPadding: 0
        topPadding: dialogButtons.inset
        bottomPadding: 0
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
                objectName: "keyboardShortcutsDefaultsButton"
                Layout.leftMargin: dialogButtons.inset
                Layout.bottomMargin: dialogButtons.bottomPadding
                icon.width: dialogButtons.glyphSize
                icon.height: dialogButtons.glyphSize
                text: I18n.i18nc("@action:button", "Defaults")
                icon.name: "document-revert"
                enabled: !root.atDefaults
                onClicked: root.restoreDefaults()
            }

            Item { Layout.fillWidth: true }

            Components.DialogButtons {
                id: dialogButtons
                primary: okButton
                Button {
                    id: okButton
                    objectName: "keyboardShortcutsOkButton"
                    text: I18n.i18nc("@action:button", "OK")
                    icon.name: "dialog-ok"
                    DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                    onClicked: {
                        if (root.applyChanges()) {
                            root.accept()
                        }
                    }
                }
                Button {
                    objectName: "keyboardShortcutsApplyButton"
                    text: I18n.i18nc("@action:button", "Apply")
                    icon.name: "dialog-ok-apply"
                    enabled: root.modified
                    DialogButtonBox.buttonRole: DialogButtonBox.ApplyRole
                    onClicked: root.applyChanges()
                }
                Button {
                    objectName: "keyboardShortcutsCancelButton"
                    text: I18n.i18nc("@action:button", "Cancel")
                    icon.name: "dialog-cancel"
                    DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                    onClicked: root.reject()
                }
            }
        }
    }

    ColumnLayout {
        id: shortcutsContent
        anchors.fill: parent
        anchors.leftMargin: Kirigami.Units.gridUnit
        anchors.rightMargin: Kirigami.Units.gridUnit
        anchors.topMargin: Kirigami.Units.gridUnit
        anchors.bottomMargin: Kirigami.Units.gridUnit
        spacing: Kirigami.Units.largeSpacing

        // Enter presses the default button, as in KDE's dialogs.
        Keys.onPressed: function(event) { dialogButtons.handleEnter(event, Window.activeFocusItem, root.visible) }

        Kirigami.SearchField {
            id: searchField
            objectName: "keyboardShortcutsSearchField"
            Layout.fillWidth: true
            placeholderText: I18n.i18nc("@info:placeholder", "Search by action or shortcut…")
            // Breeze's is under 4.5:1 on the light view colour.
            placeholderTextColor: root.uiPalette.placeholderText
            Accessible.name: I18n.i18nc("@label accessible name of the search field", "Search")
            onTextChanged: root.rebuildRows()
            // The arrow keys move through the list, as in KDE's dialog.
            Keys.onUpPressed: actionList.moveCurrent(-1)
            Keys.onDownPressed: actionList.moveCurrent(1)
        }

        // The list, in a frame, under its column headings.
        Rectangle {
            id: listFrame
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: Kirigami.Units.gridUnit * 8
            color: root.uiPalette.view
            border.width: 1
            border.color: root.uiPalette.seam

            RowLayout {
                id: columnHeadings
                objectName: "keyboardShortcutsColumnHeadings"
                x: 1
                y: 1
                width: parent.width - 2
                height: Math.ceil(root.uiPalette.rowHeight + 2 * Kirigami.Units.smallSpacing)
                spacing: 0

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: root.rowLeftPadding
                    text: I18n.i18nc("@title:column", "Action")
                    elide: Text.ElideRight
                    Accessible.role: Accessible.ColumnHeader
                }
                Label {
                    Layout.preferredWidth: root.keyColumnWidth
                    leftPadding: Kirigami.Units.largeSpacing
                    text: I18n.i18nc("@title:column", "Shortcut")
                    elide: Text.ElideRight
                    Accessible.role: Accessible.ColumnHeader
                }
                Label {
                    Layout.preferredWidth: root.keyColumnWidth + actionList.scrollbarLane
                    leftPadding: Kirigami.Units.largeSpacing
                    text: I18n.i18nc("@title:column", "Alternate")
                    elide: Text.ElideRight
                    Accessible.role: Accessible.ColumnHeader
                }
            }

            Rectangle {
                anchors.top: columnHeadings.bottom
                x: 1
                width: parent.width - 2
                height: 1
                color: root.uiPalette.seam
            }

            ListView {
                id: actionList
                objectName: "keyboardShortcutsList"
                anchors.fill: parent
                anchors.margins: 1
                anchors.topMargin: columnHeadings.height + 2
                clip: true
                model: rowsModel
                activeFocusOnTab: true
                keyNavigationEnabled: true
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0
                Accessible.role: Accessible.List
                Accessible.name: I18n.i18nc("@label", "Actions")
                readonly property real scrollbarLane: listScrollBar.visible ? listScrollBar.width : 0
                ScrollBar.vertical: ScrollBar {
                    id: listScrollBar
                    policy: ScrollBar.AsNeeded
                }

                function moveCurrent(step) {
                    if (count === 0) {
                        return
                    }
                    currentIndex = Math.max(0, Math.min(count - 1, currentIndex + step))
                }
                // The current row stays in view as a message under the
                // list makes it shorter.
                onHeightChanged: {
                    if (currentIndex >= 0) {
                        positionViewAtIndex(currentIndex, ListView.Contain)
                    }
                }
                onCurrentIndexChanged: {
                    if (currentIndex >= 0) {
                        positionViewAtIndex(currentIndex, ListView.Contain)
                    }
                    root.selectRow(currentIndex)
                }

                section.property: "groupTitle"
                section.criteria: ViewSection.FullString
                section.delegate: Item {
                    required property string section
                    width: ListView.view.width
                    height: Math.ceil(root.uiPalette.rowHeight + 2 * Kirigami.Units.largeSpacing)

                    Label {
                        objectName: "keyboardShortcutsGroupHeading"
                        x: root.rowLeftPadding
                        width: parent.width - x - root.rowLeftPadding
                        anchors.bottom: parent.bottom
                        bottomPadding: Kirigami.Units.smallSpacing
                        text: parent.section
                        font.weight: Font.DemiBold
                        color: root.uiPalette.textSecondary
                        elide: Text.ElideRight
                        Accessible.role: Accessible.Heading
                    }
                }

                delegate: ItemDelegate {
                    id: actionRow
                    required property int index
                    required property string shortcutId
                    readonly property var rowAction: root.actionsById[actionRow.shortcutId] || null
                    readonly property var bindings: actionRow.rowAction ? root.bindingsOf(actionRow.rowAction) : []
                    objectName: "keyboardShortcutsRow_" + actionRow.shortcutId
                    width: ListView.view.width
                    implicitHeight: root.uiPalette.rowHeight + Kirigami.Units.smallSpacing * 2
                    padding: 0
                    topPadding: 0
                    bottomPadding: 0
                    leftPadding: root.rowLeftPadding
                    rightPadding: 0
                    focusPolicy: Qt.NoFocus
                    highlighted: ListView.isCurrentItem
                    text: actionRow.rowAction ? root.nameOf(actionRow.rowAction) : ""
                    Accessible.role: Accessible.ListItem
                    Accessible.name: actionRow.text
                    Accessible.description: actionRow.bindings.map(function(key) { return StandardKeys.nativeText(key) }).join(", ")
                    Accessible.selected: actionRow.highlighted
                    onClicked: {
                        actionList.currentIndex = actionRow.index
                        actionList.forceActiveFocus(Qt.MouseFocusReason)
                    }
                    onDoubleClicked: {
                        actionList.currentIndex = actionRow.index
                        shortcutEditor.startCapturing()
                    }

                    background: Components.RowBackground {
                        uiPalette: root.uiPalette
                        scrollbarLane: actionList.scrollbarLane
                        hovered: actionRow.hovered
                        selected: actionRow.highlighted
                        active: actionList.activeFocus
                        focused: actionRow.highlighted && actionList.activeFocus
                    }

                    contentItem: RowLayout {
                        spacing: 0

                        Kirigami.Icon {
                            Layout.preferredWidth: root.iconSize
                            Layout.preferredHeight: root.iconSize
                            Layout.rightMargin: root.iconSpacing
                            source: actionRow.rowAction ? root.iconOf(actionRow.rowAction) : ""
                            // The app's own icon keeps its colours.
                            isMask: actionRow.rowAction !== null && actionRow.rowAction.icon.name.length > 0
                            color: root.uiPalette.text
                            Accessible.ignored: true
                        }
                        Label {
                            objectName: "keyboardShortcutsRowName"
                            Layout.fillWidth: true
                            text: actionRow.text
                            elide: Text.ElideRight
                            Accessible.ignored: true
                        }
                        Label {
                            objectName: "keyboardShortcutsRowShortcut"
                            Layout.preferredWidth: root.keyColumnWidth
                            leftPadding: Kirigami.Units.largeSpacing
                            text: actionRow.bindings.length > 0 ? StandardKeys.nativeText(actionRow.bindings[0]) : ""
                            elide: Text.ElideRight
                            Accessible.ignored: true
                        }
                        Label {
                            objectName: "keyboardShortcutsRowAlternate"
                            Layout.preferredWidth: root.keyColumnWidth
                            Layout.rightMargin: actionList.scrollbarLane
                            leftPadding: Kirigami.Units.largeSpacing
                            text: actionRow.bindings.slice(1).map(function(key) { return StandardKeys.nativeText(key) }).join(", ")
                            elide: Text.ElideRight
                            Accessible.ignored: true
                        }
                    }
                }

                Label {
                    objectName: "keyboardShortcutsNoMatches"
                    anchors.centerIn: parent
                    width: parent.width - 2 * Kirigami.Units.gridUnit
                    visible: actionList.count === 0
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: root.uiPalette.textSecondary
                    text: I18n.i18nc("@info", "No action or shortcut matches the search.")
                }
            }
        }

        // The selected action's keys: its defaults, or keys of its own.
        GridLayout {
            id: editorGrid
            objectName: "keyboardShortcutsEditor"
            Layout.fillWidth: true
            enabled: root.currentAction !== null
            columns: 3
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: Kirigami.Units.smallSpacing

            Label {
                Layout.columnSpan: 3
                Layout.fillWidth: true
                Layout.bottomMargin: Kirigami.Units.smallSpacing
                objectName: "keyboardShortcutsEditorTitle"
                text: root.currentAction
                    ? I18n.i18nc("@title:group %1 is an action", "Shortcuts for ‘%1’", root.nameOf(root.currentAction))
                    : I18n.i18nc("@title:group", "Shortcuts")
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }

            RadioButton {
                id: defaultRadio
                objectName: "keyboardShortcutsDefaultRadio"
                // Each follows the action's keys, which a click changes.
                autoExclusive: false
                Layout.columnSpan: 3
                checked: root.currentAtDefaults && !root.customChosen
                text: {
                    const defaults = root.currentAction ? root.currentAction.defaultKeyBindings : []
                    return defaults.length > 0
                        ? I18n.i18nc("@option:radio %1 is the default keys", "Default: %1",
                            defaults.map(function(key) { return StandardKeys.nativeText(key) }).join(", "))
                        : I18n.i18nc("@option:radio no default keys", "Default: None")
                }
                onClicked: {
                    root.setCurrentToDefaults()
                    checked = Qt.binding(function() { return root.currentAtDefaults && !root.customChosen })
                }
            }

            RadioButton {
                id: customRadio
                objectName: "keyboardShortcutsCustomRadio"
                autoExclusive: false
                Layout.alignment: Qt.AlignTop
                checked: !root.currentAtDefaults || root.customChosen
                text: I18n.i18nc("@option:radio", "Custom:")
                onClicked: {
                    root.chooseCustom()
                    checked = Qt.binding(function() { return !root.currentAtDefaults || root.customChosen })
                }
            }

            // The shortcut and the alternate, one key combination each.
            GridLayout {
                Layout.columnSpan: 2
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Kirigami.Units.largeSpacing
                rowSpacing: Kirigami.Units.smallSpacing

                Label {
                    text: I18n.i18nc("@label:chooser", "Shortcut:")
                    Layout.alignment: Qt.AlignRight
                }
                KQuickControls.KeySequenceItem {
                    id: shortcutEditor
                    objectName: "keyboardShortcutsShortcutEditor"
                    multiKeyShortcutsAllowed: false
                    patterns: KQuickControls.ShortcutPattern.Key | KQuickControls.ShortcutPattern.ModifierAndKey
                    checkForConflictsAgainst: root.keyConflictChecks
                    Accessible.name: I18n.i18nc("@label:chooser", "Shortcut")
                    onKeySequenceModified: root.takeKey(0, shortcutEditor)
                }
                Label {
                    text: I18n.i18nc("@label:chooser", "Alternate:")
                    Layout.alignment: Qt.AlignRight
                }
                KQuickControls.KeySequenceItem {
                    id: alternateEditor
                    objectName: "keyboardShortcutsAlternateEditor"
                    multiKeyShortcutsAllowed: false
                    patterns: KQuickControls.ShortcutPattern.Key | KQuickControls.ShortcutPattern.ModifierAndKey
                    checkForConflictsAgainst: root.keyConflictChecks
                    Accessible.name: I18n.i18nc("@label:chooser", "Alternate")
                    onKeySequenceModified: root.takeKey(1, alternateEditor)
                }
            }
        }

        // What keeps a key the user tried, or that the library and the
        // text fields type a key this action has.
        Label {
            objectName: "keyboardShortcutsRefusal"
            Layout.fillWidth: true
            visible: root.refusal.length > 0
            text: root.refusal
            wrapMode: Text.Wrap
            color: root.uiPalette.negative
        }
        Label {
            readonly property string typedKey: root.typedKeyOf(root.currentBindings)
            objectName: "keyboardShortcutsTypedKeyNote"
            Layout.fillWidth: true
            visible: typedKey.length > 0 && root.refusal.length === 0
            text: I18n.i18nc("@info %1 is a key",
                "While the library or a text field has the keyboard focus, ‘%1’ types there instead.",
                StandardKeys.nativeText(typedKey))
            wrapMode: Text.Wrap
            color: root.uiPalette.textSecondary
        }

        // That the bridge refused the set, which stays to change or cancel.
        Label {
            objectName: "keyboardShortcutsSaveFailed"
            Layout.fillWidth: true
            visible: root.saveFailed
            wrapMode: Text.Wrap
            color: root.uiPalette.negative
            text: I18n.i18nc("@info", "Ferrous could not take these shortcuts. Change some back, or cancel to keep the ones you had.")
        }
    }

    // Asks before a key moves from another action, as KDE's editor does.
    Dialogs.ConfirmDialog {
        id: reassignDialog
        objectName: "keyboardShortcutsReassignDialog"
        uiPalette: root.uiPalette
        windowRoot: root
        title: I18n.i18nc("@title:window", "Shortcut Conflict")
        message: root.pendingReassignment
            ? I18n.i18nc("@info %1 is a key, or a list of them, %2 an action", "Reassign ‘%1’ to ‘%2’?",
                root.movedKeysText(root.pendingReassignment), root.nameOf(root.pendingReassignment.action))
            : ""
        details: root.pendingReassignment
            ? (root.pendingReassignment.moves.length === 1
                ? I18n.i18nc("@info %1 is a key, %2 an action",
                    "The ‘%1’ key combination is already assigned to the ‘%2’ action, which will no longer have it.",
                    root.movedKeysText(root.pendingReassignment), root.movedFromText(root.pendingReassignment))
                : I18n.i18nc("@info %1 is a list of keys, %2 of actions",
                    "The ‘%1’ key combinations are already assigned to ‘%2’, which will no longer have them.",
                    root.movedKeysText(root.pendingReassignment), root.movedFromText(root.pendingReassignment)))
            : ""
        confirmText: I18n.i18nc("@action:button", "Reassign")
        onConfirmed: root.reassign()
        onRejected: root.pendingReassignment = null
    }
}
