// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami

// The form of a Configure page, as Kirigami's FormLayout lays one out: each
// row's field with its Kirigami.FormData.label, right-aligned beside it in
// the wide layout and above it in the narrow one, and a mnemonic that
// gives the field the keyboard. Its rows are its own children, placed by a
// grid of Qt's, which leaves out only the rows hidden themselves: a page out
// of view keeps the height of its rows, and Configure takes its height from
// the tallest page (#585). Kirigami's form hides each row's place with its
// field, so a page out of view had no height. FormData.isSection and
// twinFormLayouts are not supported.
GridLayout {
    id: root

    objectName: "preferencesForm"

    // The labels beside the fields, or above them.
    property bool wideMode: true
    // The labels' column, in the wide layout: as wide as every page's.
    property real labelColumnWidth: 0
    // The rows, in order: the children that are not the form's labels.
    readonly property var rows: root._rows

    property var _rows: []
    // The rows given a label so far. A label is a child too, so making one
    // runs relayout() again before it returns.
    property var _labelled: []

    // The form's last row: nothing to see, but the one field that takes
    // any width left over. Every other field has a maximum, and without it
    // the grid would give what they leave to the labels' column too, and
    // move the fields right of where the other pages' start.
    Item {
        objectName: "preferencesFieldColumnFill"
        readonly property bool formFill: true
        implicitWidth: 0
        implicitHeight: 0
        Layout.fillWidth: true
        Layout.row: root.wideMode ? root.rows.length : 2 * root.rows.length
        Layout.column: root.wideMode ? 1 : 0
        Accessible.ignored: true
    }

    rowSpacing: Kirigami.Units.smallSpacing
    columnSpacing: Kirigami.Units.largeSpacing
    Accessible.role: Accessible.Form

    // Gives each new row its label, and every row its place.
    function relayout() {
        const rows = []
        const children = root.children
        for (let i = 0; i < children.length; ++i) {
            const item = children[i]
            if (item.formLabel !== true && item.formFill !== true && !(item instanceof Repeater)) {
                rows.push(item)
            }
        }
        root._labelled = root._labelled.filter(function(item) { return rows.indexOf(item) !== -1 })
        // Set only as the rows change: what the page measures of them
        // decides the layout, which runs this again.
        const same = rows.length === root._rows.length
            && rows.every(function(item, i) { return root._rows[i] === item })
        if (!same) {
            root._rows = rows
        }
        for (let i = 0; i < rows.length; ++i) {
            const item = rows[i]
            item.Layout.row = root.wideMode ? i : 2 * i + 1
            item.Layout.column = root.wideMode ? 1 : 0
            if (root._labelled.indexOf(item) === -1) {
                root._labelled = root._labelled.concat([item])
                labelComponent.createObject(root, { item: item })
            }
        }
    }

    onChildrenChanged: root.relayout()
    onWideModeChanged: root.relayout()
    Component.onCompleted: root.relayout()

    Component {
        id: labelComponent

        Kirigami.Heading {
            id: label

            // The row it names.
            property Item item
            // Marks the labels, which are not rows.
            readonly property bool formLabel: true
            readonly property int index: root.rows.indexOf(label.item)
            // What the label stands beside: the field, or a line of it.
            readonly property Item buddy: label.item
                ? (label.item.Kirigami.FormData.buddyFor || label.item)
                : null

            level: 5
            // Where Kirigami's form puts the field's label.
            Layout.row: root.wideMode ? label.index : 2 * label.index
            Layout.column: 0
            // With its field. While the page is out of view, every field
            // reads as hidden: the label keeps its place, as a field
            // hidden only with its page keeps its own.
            visible: label.text.length > 0 && label.item !== null
                && (label.item.visible || !root.visible)
            enabled: label.buddy ? label.buddy.enabled : false

            Kirigami.MnemonicData.enabled: {
                const buddy = label.buddy
                if (buddy && buddy.enabled && buddy.visible && buddy.activeFocusOnTab) {
                    // Unless the field has a mnemonic of its own.
                    const own = buddy.Kirigami.MnemonicData
                    return !own.label || !own.enabled
                }
                return false
            }
            Kirigami.MnemonicData.controlType: Kirigami.MnemonicData.FormLabel
            Kirigami.MnemonicData.label: label.item ? label.item.Kirigami.FormData.label : ""
            text: label.Kirigami.MnemonicData.richTextLabel
            Accessible.name: label.Kirigami.MnemonicData.plainTextLabel
            wrapMode: Text.Wrap

            // Wide: right-aligned in the labels' column, on the middle of
            // its field, or of the line it names. Narrow: on a line of its
            // own above the field, apart from the row above.
            Layout.preferredWidth: root.wideMode
                ? Math.ceil(Math.max(root.labelColumnWidth, label.implicitWidth))
                : -1
            Layout.minimumWidth: root.wideMode
                ? Math.ceil(Math.max(root.labelColumnWidth, label.implicitWidth))
                : 0
            Layout.fillWidth: !root.wideMode
            horizontalAlignment: root.wideMode ? Text.AlignRight : Text.AlignLeft
            Layout.alignment: root.wideMode ? Qt.AlignRight | Qt.AlignTop : Qt.AlignLeft | Qt.AlignBottom
            verticalAlignment: root.wideMode ? Text.AlignVCenter : Text.AlignBottom
            Layout.preferredHeight: {
                if (!root.wideMode || !label.buddy || label.buddy instanceof TextEdit) {
                    return label.implicitHeight
                }
                const field = label.buddy === label.item
                    ? (label.item.Layout.preferredHeight > 0 ? label.item.Layout.preferredHeight : label.item.implicitHeight)
                    : label.buddy.height
                return Math.max(label.implicitHeight, field)
            }
            Layout.topMargin: {
                if (root.wideMode) {
                    return label.buddy && label.buddy !== label.item ? label.buddy.y : 0
                }
                return label.index > 0 ? Kirigami.Units.largeSpacing * 2 : 0
            }

            Component.onCompleted: {
                if (label.item.Accessible.hasOwnProperty("labelledBy")) {
                    label.item.Accessible.labelledBy = label
                }
            }

            onItemChanged: {
                if (!label.item) {
                    label.destroy()
                }
            }

            Shortcut {
                sequence: label.Kirigami.MnemonicData.sequence
                enabled: label.Kirigami.MnemonicData.enabled
                onActivated: {
                    const buddy = label.buddy
                    if (buddy instanceof AbstractButton) {
                        buddy.animateClick()
                    } else {
                        buddy.forceActiveFocus(Qt.ShortcutFocusReason)
                    }
                }
            }
        }
    }
}
