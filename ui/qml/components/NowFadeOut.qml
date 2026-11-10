// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import org.kde.kirigami 2.20 as Kirigami

// The hand-over of amber on a track change. The row that was now keeps its
// amber glyph and title for a moment and fades them out. This holds what
// fades: `target`, which the list matches its rows against, and `level`,
// the share of its amber left, from 1 down to 0 over shortDuration.
//
// The row that becomes now is amber at once, from its own state, unless
// the list sets `fadesIn`: then it takes its amber in as the old row gives
// it up, at `levelIn`, so the two together never have more than one row's
// amber.
//
// One row fades at a time: a later track change moves the fade to the row
// that left then. With animations off nothing fades: the old row is plain
// and the new one amber at once.
QtObject {
    id: root

    property int duration: Kirigami.Units.shortDuration
    // Whether the row that becomes now fades its amber in.
    property bool fadesIn: false
    // What fades, as the list describes its row, or null.
    property var target: null
    // The share of the target's amber left.
    property real level: 0
    // The share of its amber the row that is now has.
    readonly property real levelIn: root.fadesIn && root.target !== null ? 1 - root.level : 1

    readonly property NumberAnimation animation: NumberAnimation {
        objectName: "nowFadeOutAnimation"
        target: root
        property: "level"
        from: 1
        to: 0
        duration: root.duration
        easing.type: Easing.OutCubic
        // Its row is plain again; the rows stop matching it.
        onFinished: root.target = null
    }

    function start(next) {
        root.animation.stop()
        if (root.duration <= 0 || !next) {
            root.target = null
            root.level = 0
            return
        }
        root.target = next
        root.level = 1
        root.animation.start()
    }
}
