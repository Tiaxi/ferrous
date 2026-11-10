// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "." as Components
import "../logic/FormatUtils.js" as FormatUtils

// The top of the sidebar: the playing track, on the view colour with no
// card, and a seam at its foot, where the library starts.
//
// - Wide: the cover edge to edge, then the title, "Artist — Album" and
//   "Year · Genre · Track n" under it, inset 12 px, then regionGap before
//   the seam.
// - Narrow (a sidebar under narrowWidth): a 96 px cover with four short
//   lines beside it, or under it when there is no room beside it, then
//   12 px before the seam.
// - Nothing loaded: one row with the album glyph and "Nothing playing".
// - A track without a cover or without tags keeps the layout: the tile
//   on the view colour with a small mark, and a line of space for each
//   line it has no text for.
//
// The title is a step over the rest: DemiBold, larger; the artist and the
// album are in the text colour, and the details in the tertiary one.
//
// Lines that do not fit are elided, with the whole text in a tooltip.
Item {
    id: root

    required property var uiBridge
    required property var uiPalette
    required property var replaceFromItunesAction
    required property var currentTrackItunesArtworkDisabledReason
    required property var openAlbumArtViewer

    // The sidebar width under which the cover sits beside the text.
    readonly property real narrowWidth: 320
    readonly property bool narrow: root.width < root.narrowWidth
    readonly property int textInset: 12
    readonly property int narrowCoverSize: 96

    readonly property bool hasTrack: {
        const hasMetadata = root._trimmed(root.uiBridge.currentTrackTitle).length > 0
            || root._trimmed(root.uiBridge.currentTrackArtist).length > 0
            || root._trimmed(root.uiBridge.currentTrackAlbum).length > 0
        const hasActivePath = (root.uiBridge.playbackState || "") !== "Stopped"
            && root._trimmed(root.uiBridge.currentTrackPath).length > 0
        return hasMetadata || hasActivePath
    }
    readonly property string layoutMode: !root.hasTrack ? "empty" : (root.narrow ? "narrow" : "wide")

    readonly property string titleText: {
        const title = root._trimmed(root.uiBridge.currentTrackTitle)
        return title.length > 0
            ? title
            : FormatUtils.basenameFromPath(root._trimmed(root.uiBridge.currentTrackPath))
    }
    readonly property string artistText: root._trimmed(root.uiBridge.currentTrackArtist)
    readonly property string albumText: root._trimmed(root.uiBridge.currentTrackAlbum)
    readonly property string genreText: root._trimmed(root.uiBridge.currentTrackGenre)
    readonly property string yearText: {
        const year = root.uiBridge.currentTrackYear
        return year !== undefined && year !== null ? String(year).trim() : ""
    }
    // The total only when the backend knows it surely.
    readonly property string trackNumberText: {
        const number = root.uiBridge.currentTrackNumber
        const total = root.uiBridge.currentTrackTotal
        if (!(number > 0))
            return ""
        return total >= number
            ? I18n.i18nc("@info the playing track's number and the tracks on its album",
                         "Track %1 of %2", number, total)
            : I18n.i18nc("@info the playing track's number on its album", "Track %1", number)
    }
    // The separator between the fields of a line, for the lines that have
    // more than one.
    readonly property var lineSeparators: ({ artistAlbum: " — ", details: " · ", narrowDetails: " · " })

    // Missing values are left out rather than shown as dashes.
    readonly property string artistAlbumText: root._joined([root.artistText, root.albumText],
                                                          root.lineSeparators.artistAlbum)
    readonly property string detailsText: root._joined(
        [root.yearText, root.genreText, root.trackNumberText], root.lineSeparators.details)
    readonly property string narrowAlbumText: root.albumText.length > 0 && root.yearText.length > 0
        ? I18n.i18nc("@info album and its year", "%1 (%2)", root.albumText, root.yearText)
        : (root.albumText.length > 0 ? root.albumText : root.yearText)
    readonly property string narrowDetailsText: root._joined([root.genreText, root.trackNumberText],
                                                             root.lineSeparators.narrowDetails)

    // The lines the panel shows, wide and narrow.
    readonly property var trackLines: ({
        title: root.titleText,
        artistAlbum: root.artistAlbumText,
        details: root.detailsText,
        artist: root.artistText,
        narrowAlbum: root.narrowAlbumText,
        narrowDetails: root.narrowDetailsText
    })

    // A track change fades (#373 Motion), line by line: a line that differs
    // for the next track fades the one before out, in place, then the next
    // track's in, so that no frame shows both texts at once (#373 H1); where
    // only a word or a number of it changes, its fields, and the words of a
    // field in which only a number changes, that are the same for both and
    // do not move stay still (#537), and otherwise it fades whole (see
    // `_fadesWhole`, #373 H2); a line that is the same stays still. The next
    // track of the same album fades over shortDuration; a track of another
    // album, or with another cover, fades the cover and the lines together
    // over longDuration: the cover cross-fades over all of it, the lines go
    // out over its first linesOutSpan and come in over the rest. A line
    // that comes to differ while the track change fades (the metadata
    // comes after the path) joins the fade from full opacity, and the
    // lines already fading go on from theirs. A cover that changes later
    // for the same track fades too: during a track change it joins it, and
    // every layer goes on from the opacity it has onto the long curve;
    // after one, the cover fades alone and the lines change in place. Another track during a fade starts its fade from
    // what shows: every layer still fading goes on from its opacity, and
    // what the track before had faded in goes out from where it got to
    // (#463). What shows is always the playing track's; what the panel
    // showed before only fades out under or over it. Nothing fades into or
    // out of "Nothing playing": the first track after it settles its cover
    // and lines together, whatever order the bridge tells them in. With
    // animations off every change is instant.
    readonly property string trackKey: root.hasTrack
        ? root._trimmed(root.uiBridge.currentTrackPath) : ""
    readonly property string coverPath: root.uiBridge.currentTrackCoverPath || ""
    // The playing track as last seen.
    property var shown: null
    // Whether the track has just left "Nothing playing" and has had no
    // cover since: the first one it gets is part of that change.
    property bool settling: false
    // Whether a cover came while "Nothing playing" showed: the next
    // track's, told before its path.
    property bool coverCameWhileEmpty: false
    // What fades out, or null: `lines`, each line that fades by its key
    // in `trackLines` (see `_fadingLine`); `covers`, the covers before,
    // oldest first, each {coverPath, from}, fading out from `from`;
    // whether the fade is a track change (`track`), which lines that come
    // to differ join; and whether it runs over longDuration (`long`): for
    // another cover or another album, even when the two look alike.
    property var outgoing: null
    // How far the fade has come in its time: 0 when it starts, 1 once the
    // playing track alone shows. Each layer eases on its own part of it.
    property real handover: 1
    readonly property NumberAnimation handoverAnimation: NumberAnimation {
        objectName: "nowPlayingHandoverAnimation"
        target: root
        property: "handover"
        from: 0
        to: 1
        easing.type: Easing.Linear
        onFinished: root.outgoing = null
    }

    // The OutCubic curve of the motion, over a part of the fade's time.
    function _eased(progress) {
        const rest = 1 - Math.min(1, Math.max(0, progress))
        return 1 - rest * rest * rest
    }
    // The InQuad curve, which an exit takes: slow from the start, fast at
    // the end.
    function _accelerated(progress) {
        const done = Math.min(1, Math.max(0, progress))
        return done * done
    }
    // The part of the fade the lines before go out over; the playing
    // track's come in over the rest. The lines before hold, then go fast,
    // and the playing track's come fast, then settle, so the text block
    // is never empty for long, yet no frame shows two texts in a line.
    readonly property real linesOutSpan: 0.4
    // At `at` of the fade, how much of their opacity the lines before keep,
    // going out over linesOutSpan; how far the playing track's lines have
    // come in, over the rest; and how much of their opacity the covers
    // before keep, going out over all of it.
    function _linesOutLevel(at) {
        return 1 - root._accelerated(at / root.linesOutSpan)
    }
    function _linesInLevel(at) {
        return root._eased((at - root.linesOutSpan) / (1 - root.linesOutSpan))
    }
    function _coversOutLevel(at) {
        return 1 - root._eased(at)
    }
    readonly property real linesOutLevel: root._linesOutLevel(root.handover)
    readonly property real linesInLevel: root._linesInLevel(root.handover)
    readonly property real coversOutLevel: root._coversOutLevel(root.handover)
    // The most layers a line, or the cover, fades out at once: those of
    // the two tracks before the playing one. Past that, a change adds no
    // layer, and none is dropped while it shows: the playing track's own
    // layer takes the new text, or cover, at the opacity it has, as late
    // metadata does, so no opacity jumps. Only a third track change within
    // one fade comes to that. A layer that has faded out is dropped.
    readonly property int maxOutgoingLayers: 2

    // A line that fades: the words of it that stay still (`still`, see
    // `_stillRuns`), set when the line starts to fade and only ever cut
    // down, the same in every text the line shows and in the same place;
    // the lines before that fade out under the playing track's, oldest
    // first (`layers`, see `_layer`); the playing track's text but where
    // the line stays still, which fades in as one, from 1 - `incomingFrom`;
    // and whether that text is the playing track's own (`own`), or still
    // the one of the track before, which goes out as a layer of its own once
    // the playing track's comes. The parts of a line before are cut where
    // its own text has gaps, which the playing track's text need not have
    // there, so that is cut only at the edges of what stays still, in gaps
    // all share.
    function _fadingLine(still, layers, incomingFrom, own) {
        return { still: still, layers: layers, incomingFrom: incomingFrom, own: own }
    }

    // A line before as it fades out (`text`), in parts that fade out from
    // the opacity each had when this fade started (`parts`, [{at, from}],
    // in order, the first at 0). A part runs from the middle of the gap
    // after the first `at` characters of `text` (from the line's start for
    // 0) to where the next one starts. A part at 0 draws nothing. Where the
    // line stays still, the playing track's text draws instead.
    function _layer(text, parts) {
        return { text: text, parts: parts }
    }

    // The line before of `key` as it fades out, or null.
    function _outgoingLine(key) {
        return root.outgoing !== null ? (root.outgoing.lines[key] || null) : null
    }

    // The opacity of the part of the lines before of `key` that shows most,
    // on the fade's curve.
    function _outgoingLineOpacity(key) {
        const line = root._outgoingLine(key)
        if (line === null || line.layers.length === 0)
            return 0
        return Math.max(...line.layers.map(layer => root._layerFrom(layer)))
            * root._linesOutLevel(root.handover)
    }

    // How much of the playing track's text of `key` has still to come in.
    function _incomingLineRest(key) {
        const line = root._outgoingLine(key)
        return line === null ? 0 : line.incomingFrom * (1 - root._linesInLevel(root.handover))
    }

    function _layerFrom(layer) {
        return Math.max(...layer.parts.map(part => part.from))
    }

    // The line of `key` while it still fades, out or in, or null.
    function _shownLine(key) {
        return root._outgoingLineOpacity(key) > 0 || root._incomingLineRest(key) > 0
            ? root._outgoingLine(key) : null
    }

    // Whether `text` has a right-to-left character: its fields are then
    // not laid out from the left, so none of it stays still.
    function _hasRightToLeft(text) {
        return /[֐-ࣿיִ-﷿ﹰ-﻿‏‫‮⁧]|[\uD802\uD803\uD83A\uD83B][\uDC00-\uDFFF]/
            .test(text)
    }

    // The lines whose numbers are tabular figures, where a digit that
    // changes moves nothing after it.
    readonly property var tabularLines: ({ details: true, narrowAlbum: true, narrowDetails: true })

    // The words of `text` (what is between its spaces) that are not empty,
    // each as [start, end], with `offset` added.
    function _wordRanges(text, offset) {
        const ranges = []
        let start = offset
        for (const word of text.split(" ")) {
            if (word.length > 0)
                ranges.push([start, start + word.length])
            start += word.length + 1
        }
        return ranges
    }

    // Whether two words take the same width in tabular figures: the same
    // but for digits, each in place of another.
    function _sameWidth(left, right) {
        if (left.length !== right.length)
            return false
        for (let index = 0; index < left.length; ++index) {
            if (left[index] !== right[index]
                    && !(/[0-9]/.test(left[index]) && /[0-9]/.test(right[index])))
                return false
        }
        return true
    }

    // What stays still while a line goes from `before` to `after`: the
    // runs of words of `after`, as [start, end] in order, that both have,
    // the same, in the same place. A field that is the same stays still
    // while what is before it on the line keeps its width; within a field
    // in which only numbers change, so do the words that are the same. In
    // tabular figures (`tabular`), a number with as many digits keeps its
    // width. A field or word that moves fades, and so does all after it.
    // Nothing stays still in right-to-left text, whose fields are not laid
    // out from the left.
    function _stillRuns(before, after, separator, tabular) {
        if (root._hasRightToLeft(before) || root._hasRightToLeft(after))
            return []
        if (before === after)
            return before.length > 0 ? [[0, before.length]] : []
        const left = separator.length > 0 ? before.split(separator) : [before]
        const right = separator.length > 0 ? after.split(separator) : [after]
        const words = []
        let at = 0
        for (let field = 0; field < left.length && field < right.length; ++field) {
            // Both go on past the separator before this field, which is in
            // the same place in both.
            if (field > 0)
                words.push(...root._wordRanges(separator, at - separator.length))
            const was = left[field]
            const is = right[field]
            if (was !== is) {
                if (was.replace(/[0-9]+/g, "0") !== is.replace(/[0-9]+/g, "0"))
                    break
                const wasWords = was.split(" ")
                const isWords = is.split(" ")
                let offset = at
                let kept = true
                for (let word = 0; kept && word < isWords.length; ++word) {
                    if (wasWords[word] === isWords[word])
                        words.push(...root._wordRanges(isWords[word], offset))
                    else
                        kept = tabular && root._sameWidth(wasWords[word], isWords[word])
                    offset += isWords[word].length + 1
                }
                if (!kept)
                    break
            } else {
                words.push(...root._wordRanges(is, at))
            }
            at += is.length + separator.length
        }
        // Words with nothing but spaces between them stay still as one.
        const runs = []
        for (const word of words) {
            const last = runs.length > 0 ? runs[runs.length - 1] : null
            if (last !== null && /^ *$/.test(after.slice(last[1], word[0])))
                last[1] = word[1]
            else
                runs.push([word[0], word[1]])
        }
        return runs
    }

    function _stillRunsOf(key, before, after) {
        return root._stillRuns(before, after, root.lineSeparators[key] || "", root.tabularLines[key] === true)
    }

    // Whether the line of `key` fades whole, as one, going from `before` to
    // `after` while `still` of it stays still: when its structure changes,
    // as it has more or fewer fields, or a field with a number more or fewer
    // words ("Track 2" to "Track 2 of 8"); when what changes is in more than
    // one place of it; or when less than half of its glyphs would stay
    // still. A line in which only a word or a number changes fades just
    // that; otherwise the words that would stay would show alone while the
    // rest of the line is gone.
    function _fadesWhole(key, still, before, after) {
        const separator = root.lineSeparators[key] || ""
        const fieldsBefore = separator.length > 0 ? before.split(separator) : [before]
        const fieldsAfter = separator.length > 0 ? after.split(separator) : [after]
        if (fieldsBefore.length !== fieldsAfter.length)
            return true
        for (let field = 0; field < fieldsAfter.length; ++field) {
            if (/[0-9]/.test(fieldsBefore[field] + fieldsAfter[field])
                    && fieldsBefore[field].split(" ").length !== fieldsAfter[field].split(" ").length)
                return true
        }
        let glyphs = 0
        let stillGlyphs = 0
        let changes = 0
        let changing = false
        for (let index = 0; index < after.length; ++index) {
            if (after[index] === " ")
                continue
            ++glyphs
            if (still.some(run => index >= run[0] && index < run[1])) {
                ++stillGlyphs
                changing = false
            } else {
                changes += changing ? 0 : 1
                changing = true
            }
        }
        return changes > 1 || 2 * stillGlyphs < glyphs
    }

    // What stays still of the line of `key` going from `before` to `after`,
    // of `still`, what stayed still of it so far: no more, and nothing once
    // the line fades whole.
    function _keptStill(key, still, before, after) {
        const kept = root._commonRuns(still, root._stillRunsOf(key, before, after))
        return root._fadesWhole(key, kept, before, after) ? [] : kept
    }

    // The runs both `left` and `right` have.
    function _commonRuns(left, right) {
        const common = []
        for (const one of left) {
            for (const other of right) {
                const start = Math.max(one[0], other[0])
                const end = Math.min(one[1], other[1])
                if (start < end)
                    common.push([start, end])
            }
        }
        return common
    }

    function _sameRuns(left, right) {
        return left.length === right.length
            && left.every((run, index) => run[0] === right[index][0] && run[1] === right[index][1])
    }

    // What of `runs`, runs of words of `text`, is not in `kept`, a part of
    // them: from the gap before each piece to the gap after it, as [at,
    // at] in the terms of `_layer`.
    function _givenUp(runs, kept, text) {
        const spans = []
        const add = (start, end) => {
            while (start < end && text[start] === " ")
                ++start
            while (end > start && text[end - 1] === " ")
                --end
            if (start < end)
                spans.push([start === 0 ? 0 : start - 1, end])
        }
        for (const run of runs) {
            let start = run[0]
            for (const keep of kept) {
                if (keep[1] <= start || keep[0] >= run[1])
                    continue
                add(start, keep[0])
                start = Math.max(start, keep[1])
            }
            add(start, run[1])
        }
        return spans
    }

    // `parts` (see `_layer`), with `from` from `at` up to `until`.
    function _withFrom(parts, at, until, from) {
        const after = parts.filter(part => part.at <= until).pop().from
        const result = parts.filter(part => part.at < at)
        result.push({ at: at, from: from })
        if (!parts.some(part => part.at === until))
            result.push({ at: until, from: after })
        return result.concat(parts.filter(part => part.at >= until))
    }

    // The line of `key` in a fade that starts now, as it shows when the
    // playing track's line goes from `before` to `after`; null when it
    // does not fade. Each layer goes on from the opacity each of its parts
    // has. During a track change (`track`), the playing track's line
    // before goes out as a layer of its own when it was the track
    // before's, from the opacity it has, as far as it faded in; a line
    // that did not fade, from full opacity. A line that is the playing
    // track's own changes in place, as late metadata does, and so does any
    // line once it fades out as many layers as it can. What stayed still
    // and `after` no longer has in its place fades out from full opacity,
    // as it showed, in the newest layer, and the playing track's text there
    // fades in with the rest of it; so that no frame shows two texts in one
    // place, the playing track's line, once it has begun to come in, then
    // goes out as a layer of its own too, from where it had come in to,
    // and comes in again after.
    // `newTrack`: the fade is another track's, whose lines are still the
    // track before's.
    function _nextLine(key, before, after, track, newTrack) {
        const changes = before !== after
        const line = root._shownLine(key)
        if (line === null) {
            if (!track || !changes)
                return null
            const whole = [[0, Math.max(before.length, after.length)]]
            return root._fadingLine(root._keptStill(key, whole, before, after),
                                    [root._layer(before, [{ at: 0, from: 1 }])], 1, true)
        }
        const level = root._linesOutLevel(root.handover)
        const still = root._keptStill(key, line.still, before, after)
        const givenUp = root._givenUp(line.still, still, before)
        let layers = line.layers
            .map(layer => root._layer(layer.text, layer.parts.map(part => ({ at: part.at, from: part.from * level }))))
            .filter(layer => root._layerFrom(layer) > 0)
        const own = !newTrack && line.own
        let incomingFrom = root._incomingLineRest(key)
        // What stayed still and goes out needs a layer to go out in, and
        // must not show over the playing track's text where that has begun
        // to come in.
        const givesUp = givenUp.length > 0 && (incomingFrom < 1 || layers.length === 0)
        if (changes && (!own || givesUp) && layers.length < root.maxOutgoingLayers) {
            layers.push(root._layer(before, [{ at: 0, from: 1 - incomingFrom }]))
            incomingFrom = 1
        }
        // Every layer had what was still in common with the playing track's
        // text: the newest draws it, the others nothing there.
        layers = layers.map((layer, index) => root._layer(layer.text, root._mergedParts(givenUp.reduce(
            (parts, span) => root._withFrom(parts, span[0], span[1], index === layers.length - 1 ? 1 : 0),
            layer.parts))))
            .filter(layer => root._layerFrom(layer) > 0)
        // A line with nothing before it left to go out may still be coming
        // in: it goes on from where it is.
        return layers.length > 0 || incomingFrom > 0
            ? root._fadingLine(still, layers, incomingFrom, own || changes) : null
    }

    // Parts that fade alike are one.
    function _mergedParts(parts) {
        return parts.filter((part, index) => index === 0 || part.from !== parts[index - 1].from)
    }

    // The covers before in a fade that starts now, as they show when the
    // playing track's cover goes from `was`'s to `next`'s: each goes on
    // from the opacity it has, and the cover that showed whole goes out
    // from there, under them, over the playing track's, when that differs:
    // a cover over the same one adds nothing to an image and doubles the
    // tint of the translucent placeholder. A cover image right over the
    // same image adds nothing either, so the one it replaces goes.
    function _nextCovers(was, next) {
        const level = root._coversOutLevel(root.handover)
        const covers = root.outgoing === null ? [] : root.outgoing.covers
            .map(cover => ({ coverPath: cover.coverPath, from: cover.from * level }))
            .filter(cover => cover.from > 0)
        if (was.coverPath !== next.coverPath) {
            const newest = covers.length > 0 ? covers[covers.length - 1] : null
            if (newest !== null && was.coverPath.length > 0 && newest.coverPath === was.coverPath)
                covers.pop()
            if (covers.length < root.maxOutgoingLayers)
                covers.push({ coverPath: was.coverPath, from: 1 })
        }
        return covers
    }

    function followTrack() {
        const next = {
            trackKey: root.trackKey,
            album: root.albumText,
            coverPath: root.coverPath,
            lines: root.trackLines
        }
        const was = root.shown
        root.shown = next
        if (was === null || was.trackKey.length === 0 || next.trackKey.length === 0) {
            // Into or out of "Nothing playing", whose layout differs: at once.
            root.endHandover()
            const wasEmpty = was !== null && was.trackKey.length === 0
            if (next.trackKey.length === 0) {
                root.settling = false
                if (!wasEmpty) {
                    root.coverCameWhileEmpty = false
                } else if (was.coverPath !== next.coverPath) {
                    root.coverCameWhileEmpty = next.coverPath.length > 0
                }
            } else {
                // The track's cover is yet to come, unless it came first.
                root.settling = !root.coverCameWhileEmpty
                root.coverCameWhileEmpty = false
            }
            return
        }
        if (was.trackKey !== next.trackKey) {
            root.settling = false
            root.handOver(was, next, true, true)
            return
        }
        const coverChange = was.coverPath !== next.coverPath
        if (root.settling) {
            // The first track's metadata, in place.
            if (coverChange) {
                root.settling = false
            }
            return
        }
        const fading = root.outgoing
        if (coverChange || (fading !== null && !fading.long && was.album !== next.album)
                || (fading !== null && fading.track && root._linesJoin(was, next))) {
            // A cover that changed, an album named after the track itself
            // while only its lines fade, or a line of the track change
            // that comes to differ and has to join the fade.
            root.handOver(was, next, fading !== null && fading.track, false)
        }
    }

    // Whether a line changed from `was` to `next` that does not change in
    // place: one that does not fade, one that fades but shows the track
    // before's text, or one that no longer has all that stayed still of it
    // in place.
    function _linesJoin(was, next) {
        for (const key in next.lines) {
            if (was.lines[key] === next.lines[key])
                continue
            const line = root._shownLine(key)
            if (line === null || !line.own)
                return true
            if (!root._sameRuns(root._keptStill(key, line.still, was.lines[key], next.lines[key]), line.still))
                return true
        }
        return false
    }

    // The fade starts again from what shows, for another track (`newTrack`)
    // or, for the same one, its cover, album or a line that changed: every
    // layer goes on from the opacity it has, on the long curve once the
    // cover or the album changed. No layer jumps. A track change runs even before any
    // of its lines differs, for the lines its metadata changes once it
    // comes.
    function handOver(was, next, track, newTrack) {
        const lines = {}
        for (const key in next.lines) {
            const line = root._nextLine(key, was.lines[key], next.lines[key], track, newTrack)
            if (line !== null)
                lines[key] = line
        }
        const covers = root._nextCovers(was, next)
        const out = {
            lines: lines,
            covers: covers,
            track: track,
            long: covers.length > 0 || was.album !== next.album || (root.outgoing !== null && root.outgoing.long)
        }
        const duration = out.long ? Kirigami.Units.longDuration : Kirigami.Units.shortDuration
        const fades = out.track || out.long || Object.keys(out.lines).length > 0
        if (!fades || duration <= 0) {
            root.endHandover()
            return
        }
        root.handoverAnimation.stop()
        root.outgoing = out
        root.handoverAnimation.duration = duration
        root.handover = 0
        root.handoverAnimation.start()
    }

    function endHandover() {
        root.handoverAnimation.stop()
        root.outgoing = null
        root.handover = 1
    }

    onTrackLinesChanged: root.followTrack()
    onTrackKeyChanged: root.followTrack()
    onCoverPathChanged: root.followTrack()
    Component.onCompleted: root.followTrack()

    // The title, a step over the text under it: 12 pt with the default
    // 10 pt font, as a level 2 Kirigami.Heading is. It follows the font.
    readonly property real titlePointSize: root.uiPalette.basePointSize * 1.2

    // The space under the wide text block before the seam into the library,
    // which sets the two apart as regions of one sheet.
    readonly property int regionGap: 28
    // The 1 px seam at the panel's foot, where the library starts.
    readonly property int seamHeight: 1
    // The top of the lines beside the narrow cover.
    readonly property int narrowTextTop: 10
    // The least width of the lines beside the narrow cover: about 12
    // characters of the font. Under it, as with a large font in a narrow
    // window, they go under the cover, on the whole width.
    readonly property real narrowLineMinimum: 12 * baseMetrics.averageCharacterWidth
    readonly property bool narrowStacked: root.layoutMode === "narrow"
        && root.width - root.narrowCoverSize - 2 * root.textInset < root.narrowLineMinimum

    FontMetrics {
        id: baseMetrics
        font: root.uiPalette.baseFont
    }

    // The block follows its text: in narrow, at least the cover's height,
    // and more when the lines need it.
    implicitHeight: (root.layoutMode === "empty"
        ? emptyRow.height + 2 * Kirigami.Units.smallSpacing + root.textInset
        : (root.layoutMode === "narrow"
            ? (root.narrowStacked
                ? root.narrowCoverSize + root.textInset + liveText.narrowHeight
                : Math.max(root.narrowCoverSize, root.narrowTextTop + liveText.narrowHeight))
                + root.textInset
            : root.width + root.textInset + liveText.wideHeight + root.regionGap))
        + root.seamHeight

    Accessible.role: Accessible.Grouping
    Accessible.name: I18n.i18nc("@label", "Now playing")

    function _trimmed(value) {
        return String(value || "").trim()
    }

    function _joined(parts, separator) {
        return parts.filter(part => part.length > 0).join(separator)
    }

    // The text of a line, as each of its layers draws it: elided, on the
    // whole width of the line, so the layers lay it out alike.
    component LineText: Text {
        required property var owner
        width: owner.width
        elide: Text.ElideRight
        maximumLineCount: 1
        textFormat: Text.PlainText
        font.family: root.uiPalette.baseFont.family
        font.pointSize: owner.pointSize
        font.weight: owner.weight
        font.features: owner.features
        color: owner.color
    }

    // A line of the playing track, with its text as the accessible name.
    // While a track change fades it, the words that every text it shows has
    // the same, in the same place, stay still, drawn by the playing track's
    // own text, clipped, so nothing in them moves or dims. Elsewhere the
    // playing track's text fades in as one, over the lines before, the
    // oldest at the bottom; each part of a line before fades out on its own
    // (see `_layer`), between edges in its own gaps.
    component FadingLine: Item {
        id: line

        // The line's key in `trackLines`.
        required property string lineKey
        required property string text
        // Its items' names: "nowPlaying<name>" for the playing track's
        // text, "nowPlayingStill<name>" for each run of it that stays still,
        // "nowPlayingIncoming<name>" for the rest of it, which fades in, and
        // "nowPlayingIncoming<name>Window" for each piece of that,
        // "nowPlayingOutgoing<name>Layer" for each line before, and
        // "nowPlayingOutgoing<name>Part" for each piece of one, whose text
        // is "nowPlayingOutgoing<name>".
        required property string name
        property real pointSize: root.uiPalette.basePointSize
        property int weight: Font.Normal
        // Tabular figures where `_stillRuns` counts on them.
        readonly property var features: root.tabularLines[line.lineKey] === true
            ? root.uiPalette.tabularFigures : ({})
        property color color: root.uiPalette.text
        readonly property bool truncated: stillText.truncated

        readonly property var before: root.outgoing !== null
            ? (root.outgoing.lines[line.lineKey] || null) : null
        readonly property bool fading: line.before !== null && root.handover < 1
        readonly property var layers: line.fading ? line.before.layers : []
        // Every text the line shows, the playing track's last.
        readonly property var texts: line.layers.map(layer => layer.text).concat([line.text])
        // The playing track's text where it does not stay still, fading in.
        readonly property real incomingOpacity: line.fading
            ? 1 - line.before.incomingFrom * (1 - root.linesInLevel) : 1
        // Where the line stays still while it fades, [left, right] for each
        // run of `still`: from the middle of the gap before it to the middle
        // of the gap after it, where no text has a glyph. A run whose gap
        // after it the ellipsis takes in one of the texts stays still to the
        // end of the line when all are cut off within it, alike, and fades
        // otherwise, as does all past it.
        readonly property var stillWindows: {
            if (!line.fading)
                return []
            const windows = []
            for (const run of line.before.still) {
                const start = run[0] === 0 ? 0 : run[0] - 1
                if (start > 0 && !line.texts.every(text => line._keeps(text, start)))
                    break
                const left = start === 0 ? 0 : line._edgeAfter(line.text.slice(0, start))
                let right = line.width
                if (!line.texts.every(text => text.length === run[1])) {
                    if (line.texts.every(text => line._keeps(text, run[1]))) {
                        right = Math.min(line.width, line._edgeAfter(line.text.slice(0, run[1])))
                    } else {
                        const elidedWithin = metrics.advanceWidth(line.text.slice(0, run[1]))
                            + metrics.advanceWidth("…") > line.width
                        if (!elidedWithin || !line.texts.every(text => line._cutOff(text)))
                            break
                    }
                }
                if (right > left)
                    windows.push([left, right])
                if (right >= line.width)
                    break
            }
            return windows
        }
        // Where the line changes while it fades: all but stillWindows.
        readonly property var changingWindows: {
            if (!line.fading)
                return []
            const windows = []
            let from = 0
            for (const still of line.stillWindows) {
                if (still[0] > from)
                    windows.push([from, still[0]])
                from = Math.max(from, still[1])
            }
            if (from < line.width)
                windows.push([from, line.width])
            return windows
        }

        // What `layer` draws, where the line changes: each of its parts
        // that shows, {x, width, from}, cut where the line stays still.
        function _pieces(layer) {
            const pieces = []
            const parts = layer.parts
            for (let index = 0; index < parts.length; ++index) {
                if (parts[index].from <= 0)
                    continue
                const left = line._partEdge(layer.text, parts[index].at)
                const right = index + 1 < parts.length
                    ? line._partEdge(layer.text, parts[index + 1].at) : line.width
                for (const changing of line.changingWindows) {
                    const start = Math.max(left, changing[0])
                    const end = Math.min(right, changing[1])
                    if (start < end)
                        pieces.push({ x: start, width: end - start, from: parts[index].from })
                }
            }
            return pieces
        }

        // Where a part of `text` that starts at `at` starts on the line: in
        // the gap after the first `at` characters; at the end of the line
        // where `text` is elided, so the ellipsis is in one part.
        function _partEdge(text, at) {
            if (at === 0)
                return 0
            return line._keeps(text, at) ? Math.min(line.width, line._edgeAfter(text.slice(0, at))) : line.width
        }

        // The middle of the space after `start`, a start of a text the line
        // shows, as that text lays it out.
        function _edgeAfter(start) {
            // Read the font, for the edges to follow it.
            metrics.font
            return (metrics.advanceWidth(start) + metrics.advanceWidth(start + " ")) / 2
        }

        function _cutOff(text) {
            metrics.font
            return metrics.advanceWidth(text) > line.width
        }

        // Whether `text`, elided to the line, still shows its first `at`
        // characters, the space after them and the glyph after that, so that
        // the gap after them is one with no ellipsis in it.
        function _keeps(text, at) {
            return !line._cutOff(text)
                || metrics.advanceWidth(text.slice(0, at + 2)) + metrics.advanceWidth("…") <= line.width
        }

        Layout.fillWidth: true
        // An empty line keeps its place, a line of the font's height, so the
        // text block, and the library under it, stay where they are for a
        // track with tags missing (#373 H1).
        implicitHeight: stillText.implicitHeight

        FontMetrics {
            id: metrics
            font: stillText.font
        }

        // The playing track's text, whole while nothing fades; while the
        // line fades it draws through stillWindows and changingWindows.
        Item {
            width: line.fading ? 0 : line.width
            height: line.height
            clip: line.fading

            LineText {
                id: stillText
                objectName: "nowPlaying" + line.name
                owner: line
                text: line.text
                Accessible.role: Accessible.StaticText
                Accessible.name: text
                Accessible.ignored: text.length === 0
            }
        }

        // What stays still: the playing track's text in stillWindows.
        Repeater {
            model: line.stillWindows

            delegate: Item {
                id: stillWindow

                required property var modelData

                objectName: "nowPlayingStill" + line.name
                x: stillWindow.modelData[0]
                width: stillWindow.modelData[1] - stillWindow.modelData[0]
                height: line.height
                clip: true

                LineText {
                    x: -stillWindow.x
                    owner: line
                    text: line.text
                    Accessible.ignored: true
                }
            }
        }

        // What changes: the lines before, fading out part by part, the
        // oldest at the bottom, and over them the playing track's text in
        // changingWindows, fading in as one.
        Repeater {
            model: line.layers

            delegate: Item {
                id: lineBefore

                required property var modelData
                readonly property string text: lineBefore.modelData.text

                objectName: "nowPlayingOutgoing" + line.name + "Layer"
                width: line.width
                height: line.height

                Repeater {
                    model: line._pieces(lineBefore.modelData)

                    delegate: Item {
                        id: part

                        required property var modelData

                        objectName: "nowPlayingOutgoing" + line.name + "Part"
                        x: part.modelData.x
                        width: part.modelData.width
                        height: line.height
                        clip: true
                        opacity: part.modelData.from * root.linesOutLevel

                        LineText {
                            objectName: "nowPlayingOutgoing" + line.name
                            x: -part.x
                            owner: line
                            text: lineBefore.text
                            Accessible.ignored: true
                        }
                    }
                }
            }
        }
        Item {
            objectName: "nowPlayingIncoming" + line.name
            z: 1
            width: line.width
            height: line.height
            visible: line.fading
            opacity: line.incomingOpacity

            Repeater {
                model: line.changingWindows

                delegate: Item {
                    id: changingWindow

                    required property var modelData

                    objectName: "nowPlayingIncoming" + line.name + "Window"
                    x: changingWindow.modelData[0]
                    width: changingWindow.modelData[1] - changingWindow.modelData[0]
                    height: line.height
                    clip: true

                    LineText {
                        x: -changingWindow.x
                        owner: line
                        text: line.text
                        Accessible.ignored: true
                    }
                }
            }
        }
    }

    // Nothing loaded.
    Item {
        id: emptyRow
        objectName: "nowPlayingEmptyRow"
        visible: root.layoutMode === "empty"
        x: 0
        y: Kirigami.Units.smallSpacing
        width: root.width
        height: root.uiPalette.rowHeight

        Kirigami.Icon {
            x: root.textInset
            anchors.verticalCenter: parent.verticalCenter
            width: Kirigami.Units.iconSizes.small
            height: width
            source: "media-album-track"
            isMask: true
            color: root.uiPalette.textTertiary
        }

        Text {
            objectName: "nowPlayingEmptyLabel"
            x: root.textInset + Kirigami.Units.iconSizes.small + Kirigami.Units.largeSpacing
            width: Math.max(0, parent.width - x - root.textInset)
            anchors.verticalCenter: parent.verticalCenter
            text: I18n.i18nc("@info", "Nothing playing")
            elide: Text.ElideRight
            font: root.uiPalette.baseFont
            color: root.uiPalette.textSecondary
        }
    }

    Components.AlbumArtTile {
        id: cover
        objectName: "nowPlayingCover"
        visible: root.layoutMode !== "empty"
        x: 0
        y: 0
        width: root.layoutMode === "narrow" ? root.narrowCoverSize : root.width
        height: width
        uiBridge: root.uiBridge
        // No surface of its own: the sidebar's view colour, with the mark
        // in the tertiary text colour.
        placeholderColor: root.uiPalette.view
        placeholderGlyphColor: root.uiPalette.textTertiary
        // The sidebar's, which shows around the cover.
        backdropColor: root.uiPalette.view
        replaceFromItunesAction: root.replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: root.currentTrackItunesArtworkDisabledReason
        openAlbumArtViewer: root.openAlbumArtViewer
        // The covers before, over this one while they fade out.
        outgoingSlots: root.maxOutgoingLayers
        outgoingCovers: root.outgoing !== null ? root.outgoing.covers : []
        outgoingLevel: root.coversOutLevel
    }

    // The playing track's lines, each of which fades on its own over a
    // track change.
    Item {
        id: liveText

        readonly property real wideHeight: wideColumn.implicitHeight
        readonly property real narrowHeight: narrowColumn.implicitHeight
        readonly property bool truncated: root.layoutMode === "wide"
            ? (wideTitle.truncated || wideArtistAlbum.truncated || wideDetails.truncated)
            : (root.layoutMode === "narrow"
                && (narrowTitle.truncated || narrowArtist.truncated
                    || narrowAlbum.truncated || narrowDetails.truncated))
        readonly property bool hovered: wideHover.hovered || narrowHover.hovered
        readonly property Item wideItem: wideColumn
        readonly property Item narrowItem: narrowColumn

        visible: root.layoutMode !== "empty"
        x: 0
        y: 0
        width: root.width
        height: root.height

        // Wide: the text block under the cover.
        ColumnLayout {
            id: wideColumn
            objectName: "nowPlayingWideText"
            visible: root.layoutMode === "wide"
            x: root.textInset
            y: cover.height + root.textInset
            width: Math.max(0, root.width - 2 * root.textInset)
            spacing: 2

            HoverHandler {
                id: wideHover
            }

            FadingLine {
                id: wideTitle
                lineKey: "title"
                name: "Title"
                text: root.trackLines.title
                pointSize: root.titlePointSize
                weight: Font.DemiBold
            }
            FadingLine {
                id: wideArtistAlbum
                lineKey: "artistAlbum"
                name: "ArtistAlbum"
                text: root.trackLines.artistAlbum
            }
            FadingLine {
                id: wideDetails
                lineKey: "details"
                name: "Details"
                text: root.trackLines.details
                pointSize: root.uiPalette.smallPointSize
                color: root.uiPalette.textTertiary
            }
        }

        // Narrow: four short lines beside the cover, or under it when the
        // room beside it is too small for them.
        ColumnLayout {
            id: narrowColumn
            objectName: "nowPlayingNarrowText"
            visible: root.layoutMode === "narrow"
            x: root.narrowStacked ? root.textInset : root.narrowCoverSize + root.textInset
            y: root.narrowStacked ? root.narrowCoverSize + root.textInset : root.narrowTextTop
            width: Math.max(0, root.width - x - root.textInset)
            spacing: 0

            HoverHandler {
                id: narrowHover
            }

            FadingLine {
                id: narrowTitle
                lineKey: "title"
                name: "NarrowTitle"
                text: root.trackLines.title
                pointSize: root.titlePointSize
                weight: Font.DemiBold
            }
            FadingLine {
                id: narrowArtist
                lineKey: "artist"
                name: "NarrowArtist"
                text: root.trackLines.artist
                pointSize: root.uiPalette.smallPointSize
                Layout.topMargin: 2
            }
            FadingLine {
                id: narrowAlbum
                lineKey: "narrowAlbum"
                name: "NarrowAlbum"
                text: root.trackLines.narrowAlbum
                pointSize: root.uiPalette.smallPointSize
            }
            FadingLine {
                id: narrowDetails
                lineKey: "narrowDetails"
                name: "NarrowDetails"
                text: root.trackLines.narrowDetails
                pointSize: root.uiPalette.smallPointSize
                color: root.uiPalette.textTertiary
            }
        }
    }

    Rectangle {
        objectName: "nowPlayingSeam"
        x: 0
        y: root.height - root.seamHeight
        width: root.width
        height: root.seamHeight
        color: root.uiPalette.seam
    }

    // The whole text of the lines, when one of them is cut off.
    readonly property bool textTruncated: liveText.truncated

    ToolTip {
        parent: root.layoutMode === "wide" ? liveText.wideItem : liveText.narrowItem
        visible: liveText.hovered && root.textTruncated
        delay: Kirigami.Units.toolTipDelay
        text: root.layoutMode === "wide"
            ? root._joined([root.titleText, root.artistAlbumText, root.detailsText], "\n")
            : root._joined([root.titleText, root.artistText, root.narrowAlbumText,
                            root.narrowDetailsText], "\n")
    }
}
