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

// Over the whole screen, the title strip's track text and the first lane's
// label at rest share the top left corner, and take it in turn on the
// viewer's controls' one level, 1 shown and 0 hidden: as they hide, the
// text has gone by STRIP_TEXT_OUT, and from LABEL_MOVES on the label
// cross-fades from under the strip to its place at rest; as they show, the
// other way round. The label is always whole between its two places, and
// nothing has a time of its own (both fading over the level, the chip came
// in under the title's first letters, the corner doubled for some 200 ms).
// The two levels suit both the 250 ms hide, which eases in and out, and
// the 100 ms show, which eases out: either leaves both fades frames of
// their own.
var STRIP_TEXT_OUT = 0.65
var LABEL_MOVES = 0.55

// How much of the strip's track text shows, besides the strip's own level.
function stripTextLevel(level) {
    return Math.max(0, Math.min(1, (level - STRIP_TEXT_OUT) / (1 - STRIP_TEXT_OUT)))
}

// How much of the label is in its place under the strip; the rest of it is
// in its place at rest.
function labelUnderStripLevel(level) {
    return Math.max(0, Math.min(1, level / LABEL_MOVES))
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
