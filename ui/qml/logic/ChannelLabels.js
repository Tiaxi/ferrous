// SPDX-License-Identifier: GPL-3.0-or-later
.pragma library

// Use the decoder's actual order. Count alone cannot identify surround speakers.
function label(count, index, positions) {
    if (positions && positions.length === count && positions[index]) {
        return positions[index]
    }
    if (count <= 1) return "M"
    if (count === 2) return index === 0 ? "L" : "R"
    return (index + 1).toString()
}
