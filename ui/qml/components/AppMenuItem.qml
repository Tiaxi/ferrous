// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// An item of the app's menus, the menu bar's and the context menus alike,
// whose glyph grows with the font as the menu's words do: 16 px with the
// default 10 pt font, 32 px at 24 pt, and no larger. Every menu item of
// the app is one, or a component made from one, so the size is set here
// alone, through the item's own icon.width and icon.height.
//
// Breeze's item draws its glyph at a small icon's size, 16 px, whatever
// icon.width and icon.height say, beside words that grow with the font.
// Its row is laid out here as Breeze lays it out, with the glyph at the
// item's icon size: the check mark's column, the glyph, the words and the
// shortcut, with the style's spacing between them. The item keeps the
// rest of the style's: its padding and background, its check and radio
// marks, its submenu arrow, and its Alt mnemonics.
//
// In a menu of check marks and glyphs where no item has both, as View's
// and Playback's, the marks and the glyphs share one column, as in
// Breeze's widget menus: as wide as the wider of the two, each centred in
// it, where the glyphs of a menu without marks are. Breeze's item gives
// each a column of its own, which left the marks a glyph's width and two
// spacings from their words. A menu with an item that has both keeps the
// two columns.
MenuItem {
    id: root

    // Whether this item has a glyph of its own.
    readonly property bool hasGlyph: root.icon.name !== "" || root.icon.source.toString() !== ""
    // Whether the menu has check marks, and glyphs: Breeze's menu finds
    // out as its items show.
    readonly property bool menuHasMarks: (root.ListView.view !== null && root.ListView.view.hasCheckables)
        || root.checkable
    readonly property bool menuHasGlyphs: (root.ListView.view !== null && root.ListView.view.hasIcons)
        || root.hasGlyph
    // Whether the marks and the glyphs share the glyphs' column.
    readonly property bool marksShareGlyphColumn: root.menuHasMarks && root.menuHasGlyphs
        && !(root.checkable && root.hasGlyph)
        && !(root.menu !== null && root.menu.hasMarkedGlyphs === true)
    readonly property real markWidth: root.indicator !== null ? root.indicator.width : 0

    // A glyph beside a line of the menu's text: the largest icon size a
    // line of the font holds, at most a medium icon's 32 px.
    readonly property int glyphSize: Math.min(Kirigami.Units.iconSizes.medium,
                                              Kirigami.Units.iconSizes.sizeForLabels)
    icon.width: root.glyphSize
    icon.height: root.glyphSize

    // `mnemonicLabel`, the words with "&" before the mnemonic and "&&" for
    // an ampersand, as styled text: every character escaped, and the
    // mnemonic underlined when `underlined`. Kirigami's rich text label
    // leaves the words' markup as markup.
    function styledLabel(mnemonicLabel, underlined) {
        const escaped = function(character) {
            switch (character) {
            case "&": return "&amp;"
            case "<": return "&lt;"
            case ">": return "&gt;"
            case "\"": return "&quot;"
            default: return character
            }
        }
        const text = String(mnemonicLabel || "")
        let styled = ""
        for (let i = 0; i < text.length; ++i) {
            const character = text[i]
            if (character === "&" && i + 1 < text.length) {
                const next = text[++i]
                styled += next !== "&" && underlined
                    ? "<u>" + escaped(next) + "</u>"
                    : escaped(next)
            } else {
                styled += escaped(character)
            }
        }
        return styled
    }

    contentItem: RowLayout {
        objectName: "appMenuItemRow"

        // The check mark's own column, in every item of a menu that has
        // marks that do not share the glyphs' column.
        Item {
            Layout.preferredWidth: root.menuHasMarks && !root.marksShareGlyphColumn
                ? root.markWidth
                : Kirigami.Units.smallSpacing
        }
        // The glyph's column, in every item of a menu that has glyphs, and
        // the mark's too where they share it.
        Item {
            id: glyphColumn
            objectName: "appMenuItemGlyphColumn"
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: root.marksShareGlyphColumn
                ? Math.max(root.markWidth, root.icon.width)
                : root.icon.width
            Layout.preferredHeight: root.icon.height
            visible: root.menuHasGlyphs

            Kirigami.Icon {
                objectName: "appMenuItemIcon"
                anchors.centerIn: parent
                width: root.icon.width
                height: root.icon.height
                source: root.icon.name !== "" ? root.icon.name : root.icon.source
                color: root.icon.color
                selected: root.pressed
            }
        }
        Label {
            id: label
            objectName: "appMenuItemLabel"
            Layout.alignment: Qt.AlignVCenter
            Layout.fillWidth: true
            // The words, with the mnemonic underlined while Alt is held,
            // as Breeze shows them; the words' own markup shows as text.
            textFormat: Text.StyledText
            text: root.styledLabel(root.Kirigami.MnemonicData.mnemonicLabel,
                                   root.Kirigami.MnemonicData.active)
            font: root.font
            color: root.pressed ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
            elide: Text.ElideRight
            visible: root.text.length > 0
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignVCenter
        }
        // The action's shortcut, in the platform's words.
        Label {
            objectName: "appMenuItemShortcut"
            Layout.alignment: Qt.AlignVCenter
            visible: root.action !== null && root.action.shortcut !== undefined
            textFormat: Text.PlainText
            text: visible ? itemShortcut.nativeText : ""
            font: root.font
            color: label.color
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter

            Shortcut {
                id: itemShortcut
                sequence: root.action !== null && root.action.shortcut !== undefined ? root.action.shortcut : ""
                enabled: false
            }
        }
        Item {
            Layout.preferredWidth: Kirigami.Units.smallSpacing
        }
    }

    // The style's mark, centred in the column it shares with the glyphs.
    Binding {
        target: root.indicator
        property: "x"
        when: root.marksShareGlyphColumn && root.indicator !== null
        value: root.contentItem.x + glyphColumn.x + (glyphColumn.width - root.markWidth) / 2
    }
}
