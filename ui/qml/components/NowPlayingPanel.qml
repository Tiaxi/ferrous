// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "." as Components
import "../logic/FormatUtils.js" as FormatUtils

// The top of the sidebar: the playing track, on the view colour with no
// card.
//
// - Wide: the cover edge to edge, then the title, "Artist — Album" and
//   "Year · Genre · Track n" under it, inset 12 px.
// - Narrow (a sidebar under narrowWidth): a 96 px cover with four short
//   lines beside it, then 12 px before the library.
// - Nothing loaded: one row with the album glyph and "Nothing playing".
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

    // A track change cross-fades (#373 Motion), line by line: a line that
    // differs for the next track fades the one before out over it, in
    // place, and the fields at its start that the two share stay still; a
    // line that is the same stays still. The next track of the same album
    // fades over shortDuration; a track of another album, or with another
    // cover, fades the cover and the lines together on one curve over
    // longDuration. A line that comes to differ while the track change
    // fades (the metadata comes after the path) joins the fade from full
    // opacity, and the lines already fading go on from theirs. A cover
    // that changes later for the same track fades too: during a track
    // change it joins it, and every layer goes on from the opacity it has
    // onto the long curve; after one, the cover fades alone and the lines
    // change in place. Another track during a fade starts its fade from
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
    // How far the fade has come: 0 when it starts, 1 once the playing
    // track alone shows.
    property real handover: 1
    readonly property NumberAnimation handoverAnimation: NumberAnimation {
        objectName: "nowPlayingHandoverAnimation"
        target: root
        property: "handover"
        from: 0
        to: 1
        easing.type: Easing.OutCubic
        onFinished: root.outgoing = null
    }
    // The most layers a line, or the cover, fades out at once: those of
    // the two tracks before the playing one. Past that, a change adds no
    // layer, and none is dropped while it shows: the playing track's own
    // layer takes the new text, or cover, at the opacity it has, as late
    // metadata does, so no opacity jumps. Only a third track change within
    // one fade comes to that. A layer that has faded out is dropped.
    readonly property int maxOutgoingLayers: 2

    // A line that fades: the start of it that stays still (`still`, see
    // `_sharedStart`), set when the line starts to fade and only ever cut
    // shorter, a start of every text the line shows; the lines before that
    // fade out under the playing track's, oldest first (`layers`, see
    // `_layer`); the playing track's text past `still`, which fades in as
    // one, from 1 - `incomingFrom`; and whether that text is the playing track's
    // own (`own`), or still the one of the track before, which goes out
    // as a layer of its own once the playing track's comes. The parts are
    // cut where the text before has gaps, which the playing track's text
    // need not have there, so it is never cut but at `still`, the end of
    // the start all share.
    function _fadingLine(still, layers, incomingFrom, own) {
        return { still: still, layers: layers, incomingFrom: incomingFrom, own: own }
    }

    // A line before as it fades out (`text`), in parts that fade out from
    // the opacity each had when this fade started (`parts`, [{start,
    // from}], in order: each from the end of its `start`, a start of
    // `text`, to the next one's). The first starts at the line's still
    // start or past it: what is short of it shows in the still start, or
    // in a layer above.
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
        if (line === null)
            return 0
        return Math.max(...line.layers.map(layer => root._layerFrom(layer))) * (1 - root.handover)
    }

    function _layerFrom(layer) {
        return Math.max(...layer.parts.map(part => part.from))
    }

    // The line of `key` while it still fades, or null.
    function _shownLine(key) {
        return root._outgoingLineOpacity(key) > 0 ? root._outgoingLine(key) : null
    }

    // Whether `text` has a right-to-left character: its fields are then
    // not laid out from the left, so none of it stays still.
    function _hasRightToLeft(text) {
        return /[\u0590-\u08FF\uFB1D-\uFDFF\uFE70-\uFEFF\u200F\u202B\u202E\u2067]|[\uD802\uD803\uD83A\uD83B][\uDC00-\uDFFF]/
            .test(text)
    }

    // The start of two lines that can stay still while one fades into the
    // other: their whole leading fields that are the same, with the
    // separator after them when both go on. Empty when the first fields
    // differ, or when either has right-to-left text.
    function _sharedStart(before, after, separator) {
        if (root._hasRightToLeft(before) || root._hasRightToLeft(after))
            return ""
        if (separator.length === 0)
            return before === after ? before : ""
        const left = before.split(separator)
        const right = after.split(separator)
        let shared = 0
        while (shared < left.length && shared < right.length && left[shared] === right[shared])
            ++shared
        if (shared === 0)
            return ""
        const start = left.slice(0, shared).join(separator)
        return shared < left.length && shared < right.length ? start + separator.replace(/\s+$/, "") : start
    }

    // The line of `key` in a fade that starts now, as it shows when the
    // playing track's line goes from `before` to `after`; null when it
    // does not fade. Each layer goes on from the opacity each of its parts
    // has. During a track change (`track`), the playing track's line
    // before goes out as a layer of its own when it was the track
    // before's, from the opacity it has: whole in the still start, and
    // past it as it faded in so far; a line that did not fade, from full
    // opacity. A line that is the playing track's own changes in place,
    // as late metadata does, and so does any line once it fades out as
    // many layers as it can. A field of the still start that `after` no
    // longer shares fades out from full opacity, as it showed, and the
    // playing track's text there fades in with the rest of it.
    // `newTrack`: the fade is another track's, whose lines are still the
    // track before's.
    function _nextLine(key, before, after, track, newTrack) {
        const separator = root.lineSeparators[key] || ""
        const changes = before !== after
        const line = root._shownLine(key)
        if (line === null) {
            if (!track || !changes)
                return null
            const still = root._sharedStart(before, after, separator)
            return root._fadingLine(still, [root._layer(before, [{ start: still, from: 1 }])], 1, true)
        }
        const level = 1 - root.handover
        const shared = root._sharedStart(before, after, separator)
        const still = shared.length < line.still.length ? shared : line.still
        const layers = line.layers
            .map(layer => root._layer(layer.text, layer.parts.map(part => ({ start: part.start, from: part.from * level }))))
            .filter(layer => root._layerFrom(layer) > 0)
        const own = !newTrack && line.own
        let incomingFrom = line.incomingFrom * level
        if (changes && !own && layers.length < root.maxOutgoingLayers) {
            const shown = 1 - incomingFrom
            const parts = still !== line.still
                ? [{ start: still, from: 1 }, { start: line.still, from: shown }]
                : [{ start: still, from: shown }]
            layers.push(root._layer(before, parts))
            incomingFrom = 1
        } else if (still !== line.still) {
            // Every layer shares the still start: the newest draws it.
            layers[layers.length - 1].parts.unshift({ start: still, from: 1 })
        }
        const kept = layers
            .map(layer => root._layer(layer.text, root._mergedParts(layer.parts)))
            .filter(layer => root._layerFrom(layer) > 0)
        return kept.length > 0 ? root._fadingLine(still, kept, incomingFrom, own || changes) : null
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
        const level = 1 - root.handover
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
    // before's text, or one that no longer shares all of its still start.
    function _linesJoin(was, next) {
        for (const key in next.lines) {
            if (was.lines[key] === next.lines[key])
                continue
            const line = root._shownLine(key)
            if (line === null || !line.own)
                return true
            const separator = root.lineSeparators[key] || ""
            if (root._sharedStart(was.lines[key], next.lines[key], separator).length < line.still.length)
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

    // 15, 14 and 12 px with the default 13 px font.
    readonly property real titlePointSize: root.uiPalette.basePointSize * 15 / 13
    readonly property real narrowTitlePointSize: root.uiPalette.basePointSize * 14 / 13

    implicitHeight: root.layoutMode === "empty"
        ? emptyRow.height + 2 * Kirigami.Units.smallSpacing + root.textInset
        : (root.layoutMode === "narrow"
            ? root.narrowCoverSize + root.textInset
            : root.width + liveText.wideHeight + root.textInset + 14)

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
        lineHeightMode: owner.fixedLineHeight > 0 ? Text.FixedHeight : Text.ProportionalHeight
        lineHeight: owner.fixedLineHeight > 0 ? owner.fixedLineHeight : 1
    }

    // A line of the playing track, with its text as the accessible name.
    // While a track change fades it, the lines before fade out under it
    // past the fields at the start that all share; those stay still, drawn
    // by the playing track's own text, clipped, so nothing in them moves or
    // dims. Past them the playing track's text fades in as one, over the
    // lines before, the oldest at the bottom; each part of a line before
    // fades out on its own (see `_layer`), between edges in its own gaps.
    component FadingLine: Item {
        id: line

        // The line's key in `trackLines`.
        required property string lineKey
        required property string text
        // Its items' names: "nowPlaying<name>" for the playing track's
        // text, "nowPlayingIncoming<name>" for the part of it that fades in,
        // "nowPlayingOutgoing<name>Layer" for each line before, and
        // "nowPlayingOutgoing<name>Part" for each part of one, whose text
        // is "nowPlayingOutgoing<name>".
        required property string name
        property real pointSize: root.uiPalette.basePointSize
        property int weight: Font.Normal
        property var features: ({})
        property color color: root.uiPalette.textSecondary
        // A line height in pixels, or 0 for the font's.
        property real fixedLineHeight: 0
        readonly property bool truncated: stillText.truncated

        readonly property var before: root.outgoing !== null
            ? (root.outgoing.lines[line.lineKey] || null) : null
        readonly property bool fading: line.before !== null && root.handover < 1
        readonly property var layers: line.fading ? line.before.layers : []
        // The playing track's text past the still start, fading in.
        readonly property real incomingOpacity: line.fading
            ? 1 - line.before.incomingFrom * (1 - root.handover) : 1
        // How much of the line stays still, from the left: up to the middle
        // of the space after the still start, where none has a glyph; all
        // of it when nothing fades.
        readonly property real stillWidth: {
            if (!line.fading)
                return line.width
            const still = line.before.still
            if (still.length === 0)
                return 0
            const texts = line.layers.map(layer => layer.text).concat([line.text])
            if (texts.every(text => line._keeps(text, still)))
                return line._edgeAfter(still)
            // Elided within the still start: they look the same when all
            // are, and differ there otherwise.
            const elidedWithin = metrics.advanceWidth(still) + metrics.advanceWidth("\u2026") > line.width
            return elidedWithin && texts.every(text => line._cutOff(text)) ? line.width : 0
        }

        // Where each part of `layer` starts, then the end of the line. A
        // part from the still start starts where the line stops being
        // still; one that would start where its text is elided starts at
        // the end, so the ellipsis is in one part.
        function _partEdges(layer) {
            const still = line.before.still
            const edges = layer.parts.map(part => part.start.length <= still.length
                ? line.stillWidth
                : (line._keeps(layer.text, part.start)
                    ? Math.min(line.width, Math.max(line.stillWidth, line._edgeAfter(part.start)))
                    : line.width))
            edges.push(line.width)
            return edges
        }

        // The middle of the space after `start`, a start of a text the line
        // shows, as that text lays it out.
        function _edgeAfter(start) {
            // Read the font, for the edges to follow it.
            metrics.font
            return (metrics.advanceWidth(start) + metrics.advanceWidth(start + " ")) / 2
        }

        function _cutOff(text) {
            return metrics.advanceWidth(text) > line.width
        }

        // Whether `text`, elided to the line, still shows `start`, the
        // space after it and the glyph after that, so that the gap after
        // `start` is one with no ellipsis in it.
        function _keeps(text, start) {
            return !line._cutOff(text)
                || metrics.advanceWidth(text.slice(0, start.length + 2)) + metrics.advanceWidth("\u2026")
                    <= line.width
        }

        Layout.fillWidth: true
        implicitHeight: stillText.implicitHeight
        visible: line.text.length > 0 || line.layers.some(layer => layer.text.length > 0)

        FontMetrics {
            id: metrics
            font: stillText.font
        }

        // What stays still: the playing track's text up to stillWidth.
        Item {
            width: line.stillWidth
            height: line.height
            clip: line.fading

            LineText {
                id: stillText
                objectName: "nowPlaying" + line.name
                owner: line
                text: line.text
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
        }

        // What changes: the lines before, fading out part by part, the
        // oldest at the bottom, and over them the playing track's text past
        // stillWidth, fading in as one.
        Repeater {
            model: line.layers

            delegate: Item {
                id: lineBefore

                required property var modelData
                readonly property var edges: line._partEdges(lineBefore.modelData)

                objectName: "nowPlayingOutgoing" + line.name + "Layer"
                width: line.width
                height: line.height

                Repeater {
                    model: lineBefore.modelData.parts

                    delegate: Item {
                        id: part

                        required property var modelData
                        required property int index
                        readonly property real start: lineBefore.edges[part.index] || 0

                        objectName: "nowPlayingOutgoing" + line.name + "Part"
                        x: part.start
                        width: Math.max(0, (lineBefore.edges[part.index + 1] || 0) - part.start)
                        height: line.height
                        clip: true
                        opacity: part.modelData.from * (1 - root.handover)

                        LineText {
                            objectName: "nowPlayingOutgoing" + line.name
                            x: -part.start
                            owner: line
                            text: lineBefore.modelData.text
                            Accessible.ignored: true
                        }
                    }
                }
            }
        }
        Item {
            objectName: "nowPlayingIncoming" + line.name
            x: line.stillWidth
            z: 1
            width: Math.max(0, line.width - line.stillWidth)
            height: line.height
            clip: true
            visible: line.fading
            opacity: line.incomingOpacity

            LineText {
                x: -line.stillWidth
                owner: line
                text: line.text
                Accessible.ignored: true
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
        placeholderColor: Qt.rgba(root.uiPalette.text.r, root.uiPalette.text.g,
                                  root.uiPalette.text.b, 0.04)
        placeholderGlyphColor: root.uiPalette.textTertiary
        // The sidebar's, which shows around the cover.
        backdropColor: root.uiPalette.view
        replaceFromItunesAction: root.replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: root.currentTrackItunesArtworkDisabledReason
        openAlbumArtViewer: root.openAlbumArtViewer
        // The covers before, over this one while they fade out.
        outgoingSlots: root.maxOutgoingLayers
        outgoingCovers: root.outgoing !== null ? root.outgoing.covers : []
        outgoingLevel: 1 - root.handover
    }

    // The playing track's lines, each of which fades on its own over a
    // track change.
    Item {
        id: liveText

        readonly property real wideHeight: wideColumn.implicitHeight
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
                color: root.uiPalette.text
                fixedLineHeight: Math.round(root.uiPalette.rowHeight * 20 / 26)
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
                features: root.uiPalette.tabularFigures
                color: root.uiPalette.textTertiary
            }
        }

        // Narrow: four short lines beside the cover.
        ColumnLayout {
            id: narrowColumn
            objectName: "nowPlayingNarrowText"
            visible: root.layoutMode === "narrow"
            x: root.narrowCoverSize + root.textInset
            y: 10
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
                pointSize: root.narrowTitlePointSize
                weight: Font.DemiBold
                color: root.uiPalette.text
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
                features: root.uiPalette.tabularFigures
                color: root.uiPalette.textTertiary
            }
            FadingLine {
                id: narrowDetails
                lineKey: "narrowDetails"
                name: "NarrowDetails"
                text: root.trackLines.narrowDetails
                pointSize: root.uiPalette.smallPointSize
                features: root.uiPalette.tabularFigures
                color: root.uiPalette.textTertiary
            }
        }
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
