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
MenuItem {
    id: root

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

        // The check mark's column, in every item of a menu that has one.
        Item {
            Layout.preferredWidth: (root.ListView.view && root.ListView.view.hasCheckables) || root.checkable
                ? root.indicator.width
                : Kirigami.Units.smallSpacing
        }
        // The glyph's column, in every item of a menu that has glyphs.
        Kirigami.Icon {
            objectName: "appMenuItemIcon"
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: root.icon.width
            Layout.preferredHeight: root.icon.height
            visible: (root.ListView.view && root.ListView.view.hasIcons)
                || root.icon.name !== "" || root.icon.source.toString() !== ""
            source: root.icon.name !== "" ? root.icon.name : root.icon.source
            color: root.icon.color
            selected: root.pressed
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
}
