// SPDX-License-Identifier: GPL-3.0-or-later
.pragma library

// Channel count does not identify a surround speaker layout or its ordering.
// Keep numbered decoder channels until the protocol carries actual positions.
function label(count, index) {
    if (count <= 1) return "M"
    if (count === 2) return index === 0 ? "L" : "R"
    return (index + 1).toString()
}
