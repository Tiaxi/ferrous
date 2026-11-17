// SPDX-License-Identifier: GPL-3.0-or-later
.pragma library

// The rows a ListView shows whole, as { first, last }, or -1 for both
// while it shows none. A row cut by the top or the bottom edge counts only
// when no row is whole.
function shownRange(view) {
    const count = view ? view.count : 0
    if (count <= 0 || view.height <= 0) {
        return { first: -1, last: -1 }
    }
    const top = view.contentY
    const bottom = view.contentY + view.height
    let first = view.indexAt(1, top)
    if (first < 0) {
        first = 0
    }
    const firstItem = view.itemAtIndex(first)
    if (firstItem && firstItem.y < top - 0.5 && first + 1 < count) {
        first += 1
    }
    let last = view.indexAt(1, bottom - 1)
    if (last < 0) {
        last = count - 1
    }
    const lastItem = view.itemAtIndex(last)
    if (lastItem && lastItem.y + lastItem.height > bottom + 0.5 && last - 1 >= first) {
        last -= 1
    }
    return { first: first, last: Math.max(first, last) }
}

// Whether row `index` is among them.
function isShown(view, index) {
    const range = shownRange(view)
    return index >= 0 && index >= range.first && index <= range.last
}
