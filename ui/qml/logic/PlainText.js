// SPDX-License-Identifier: GPL-3.0-or-later

.pragma library
.import QtQuick as Quick

// For text in controls of the style or of Kirigami that give their users no
// textFormat: their own labels default to AutoText, which draws what looks
// like markup as markup, images included. Makes every text under `item`
// plain. Call it before the control is given its text.
function apply(item) {
    if (!item) {
        return
    }
    if (item.textFormat !== undefined) {
        item.textFormat = Quick.Text.PlainText
    }
    const children = item.children
    for (let i = 0; i < children.length; ++i) {
        apply(children[i])
    }
}
