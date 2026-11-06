// SPDX-License-Identifier: GPL-3.0-or-later
.pragma library

// The lanes of a visualization `height` pixels tall: `count` lanes with a
// 1 px seam between them, on the rows at k * height / count, as the
// waveform draws its seams and LevelMeterItem::laneRows lays out its
// columns. Each lane starts on the row after its seam.
function top(index, count, height) {
    const lanes = Math.max(1, count)
    return index > 0 ? Math.round(index * height / lanes) + 1 : 0
}

function bottom(index, count, height) {
    const lanes = Math.max(1, count)
    return index + 1 < lanes ? Math.round((index + 1) * height / lanes) : height
}

function laneHeight(index, count, height) {
    return Math.max(0, bottom(index, count, height) - top(index, count, height))
}

// The lane at `y`, or -1 off the lanes.
function laneAt(y, count, height) {
    for (let index = 0; index < Math.max(1, count); ++index) {
        if (y >= top(index, count, height) && y < bottom(index, count, height)) {
            return index
        }
    }
    return -1
}
