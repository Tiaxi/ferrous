// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components
import "../logic/ColorUtils.js" as ColorUtils

// The search palette (#373): an 800 px palette 32 px under the transport,
// over a dim that covers the whole window; wider with a larger font, as its
// text is (UiPalette.scaledSize). The field, then the results in
// sections in the order of their best match, then the keys for the
// selected row.
Dialog {
    id: root
    objectName: "globalSearchDialog"
    required property var controller
    required property var uiPalette
    required property var windowRoot
    // The transport: the palette opens 32 px under it (and the seam after
    // it). Without one, 32 px under the top of the window.
    property Item anchorItem: null
    required property real snappyScrollFlickDeceleration
    required property real snappyScrollMaxFlickVelocity
    // How long the palette and the dim fade in and out: none when Plasma's
    // animations are off.
    property int fadeDuration: Kirigami.Units.shortDuration
    property var contextRowData: ({})
    readonly property bool searching: !!root.controller.uiBridge.globalSearchBusy
    readonly property bool retained: !!root.controller.uiBridge.globalSearchModelRetained
    readonly property string queryText: queryField.text.trim()
    // The query the rows were found by, which they show their matches of
    // until the rows of a newer one replace them.
    property string resultsQuery: ""
    readonly property string suggestion: root.controller.uiBridge.globalSearchSuggestion || ""
    // The result rows, from the model rather than the view: the view's
    // count notifies when the view next lays itself out, which the
    // palette's height, bound to it, sets off when it scrolls the view
    // back into bounds (#619).
    readonly property int rowCount: root.controller.globalSearchModelApi.count || 0
    readonly property bool resultsShown: root.queryText.length > 0 && !root.retained && root.rowCount > 0
    readonly property bool noResults: root.queryText.length > 0 && !root.searching && !root.retained
        && root.rowCount === 0
    // The rows outside the results: the recent searches before typing, and
    // a suggested spelling when nothing was found. One is selected, as a
    // result would be.
    readonly property var auxRows: {
        if (root.queryText.length === 0) {
            return root.controller.recentQueries.map(function(query) { return { kind: "recent", text: query } })
        }
        if (root.noResults && root.suggestion.length > 0) {
            return [{ kind: "suggestion", text: root.suggestion }]
        }
        return []
    }
    property int auxIndex: 0
    onAuxRowsChanged: root.auxIndex = root.auxRows.length > 0 ? 0 : -1
    readonly property var selectedAuxRow: root.auxIndex >= 0 && root.auxIndex < root.auxRows.length
        ? root.auxRows[root.auxIndex] : null

    // The clear glyph at rest. Breeze's is a solid shape of three times
    // the search and help glyphs' ink, so in their colour it was the
    // heaviest mark in the field's row: their colour nearly half way to
    // the view, and still 3:1 on it.
    readonly property color clearGlyphRest: root.uiPalette.readableOn(
        ColorUtils.mixColor(root.uiPalette.textSecondary, root.uiPalette.view, 0.45), [root.uiPalette.view], 3.1)
    // The amber of the playing glyph over a cover's scrim.
    readonly property color nowOnScrim: "#f2b035"
    // The palette's sizes at 1×: these with the default font, and taller
    // with a larger one, by its line.
    readonly property real fieldHeight: Math.max(44, Math.round(regularMetrics.height) + 20)
    readonly property real headingHeight: Math.max(28, Math.round(smallMetrics.height) + 8)
    // Over a heading that follows a section, 8 px more, scaled: some twice
    // the space above it as under it, so it starts its section (30 px both
    // ways, less than two results' 32, the sections ran on as one list).
    readonly property real sectionGap: root.uiPalette.scaledSize(8)
    readonly property int headingBottomPadding: 4
    // A result row's height: the list's height adds up the same ones.
    function rowHeightAt(kind, index) {
        if (kind === "item") return root.resultHeight
        if (kind === "") return 0
        return kind === "section" && index > 0 ? root.headingHeight + root.sectionGap : root.headingHeight
    }
    readonly property real resultHeight: Math.max(32, Math.round(regularMetrics.height) + 12)
    // The footer's line of key hints, and the footer: 28 px, or as many
    // lines as the hints wrap to and 4 px above and below them.
    readonly property real footerLineHeight: Math.round(smallMetrics.height) + 4
    readonly property real footerHeight: Math.max(28, footerHints.implicitHeight + 8)
    // The line under the no-results title.
    readonly property real hintHeight: Math.max(20, Math.round(smallMetrics.height) + 4)
    // The footer's gaps: between a key and its action, half again a word
    // space, so "Ctrl+Enter Add to Queue" reads as a key and an action, not
    // as one phrase; between pairs three times that, so the pairs read
    // apart (they were 4 px, a word space, and 22 px). 6 and 18 px at
    // 10 pt; they grow with the font.
    readonly property real hintKeyGap: Math.max(Kirigami.Units.mediumSpacing,
                                                Math.round(1.5 * smallMetrics.advanceWidth(" ")))
    readonly property real hintPairGap: 3 * root.hintKeyGap
    readonly property real sideInset: 16
    // The glyphs, the lead and the field's buttons are sized for the
    // default font and grow with a larger one, as the transport's do
    // (UiPalette.scaledSize): the glyphs 16 px, the playing glyph 12 px,
    // the lead (a cover or a glyph's room) 24 px, and the clear and help
    // buttons 28 px. The lead keeps 4 px clear of its row's edges, and the
    // buttons 8 px of the field's. All are even, so a glyph centres on
    // whole pixels in its button and over a cover.
    readonly property int glyphSize: 2 * Math.round(root.uiPalette.toolIconSize / 2)
    readonly property int nowGlyphSize: 2 * Math.round(root.uiPalette.scaledSize(12) / 2)
    readonly property int leadSize: 2 * Math.floor(Math.min(root.uiPalette.scaledSize(24), root.resultHeight - 8) / 2)
    readonly property int fieldButtonSize: 2 * Math.floor(Math.min(root.uiPalette.scaledSize(28), root.fieldHeight - 16) / 2)
    readonly property real titleX: root.sideInset + root.leadSize + 12
    // A track's length at the right: 44 px, or as wide as "00:00" in its
    // font, and 12 px clear of the context. Of the room left beside it the
    // title takes up to 380 px and the context the rest: in the 800 px
    // palette, 276 px. The title grows past 300 px only into room the
    // context can spare over those 276, and takes at most 60 % in a
    // narrower palette. All follow the font, as the palette's width does,
    // and the three never meet.
    readonly property real lengthWidth: Math.max(44, Math.ceil(lengthMetrics.advanceWidth))
    readonly property real columnGap: 12
    readonly property real titleWidth: {
        const lengthLeft = root.width - root.sideInset - Kirigami.Units.largeSpacing - root.lengthWidth - root.columnGap
        const room = lengthLeft - root.columnGap - root.titleX
        const least = Math.min(root.uiPalette.scaledSize(300), Math.round(room * 0.6))
        const spared = room - root.uiPalette.scaledSize(276)
        return Math.max(0, Math.min(root.uiPalette.scaledSize(380), Math.max(least, spared)))
    }
    readonly property real contextX: root.titleX + root.titleWidth + root.columnGap
    // Where the palette's top is: set when it opens, and kept while it is
    // open, so typing never moves the field. 32 px under the transport;
    // higher where the window is too short to show five results there
    // (at a large font in a small window it showed three), as far as the
    // field, the results' footer, two headings and five results then fit
    // above the window's 32 px bottom margin, but at least 8 px under the
    // menu bar.
    property real paletteTop: 32
    readonly property int leastResultRows: 5
    function updatePlacement() {
        const windowItem = root.parent
        if (!windowItem) {
            root.paletteTop = 32
            return
        }
        const underTransport = root.anchorItem
            ? Math.round(root.anchorItem.mapToItem(windowItem, 0, root.anchorItem.height).y) + 1 + 32
            : 32
        const resultsFooterHeight = Math.max.apply(null, ["artist", "album", "track"].map(function(kind) {
            return root.footerHeightFor(root.hintsFor(kind))
        }))
        const leastHeight = root.fieldHeight + 1 + root.rowHeightAt("section", 0) + root.rowHeightAt("section", 1)
            + root.leastResultRows * root.resultHeight + 1 + resultsFooterHeight
        const menuBar = root.windowRoot && root.windowRoot.menuBar && root.windowRoot.menuBar.visible
            ? root.windowRoot.menuBar : null
        const highest = (menuBar ? Math.ceil(menuBar.mapToItem(windowItem, 0, menuBar.height).y) : 0) + 8
        root.paletteTop = Math.max(highest, Math.min(underTransport, Math.floor(windowItem.height - 32 - leastHeight)))
    }
    // Counts the result rows' changes, for what depends on the selected
    // row's data rather than on its index.
    property int rowsRevision: 0

    // Closed from when its fade out starts. Qt holds back the window's
    // shortcuts, Ctrl+Z among them, while a popup that is modal or closes
    // on Escape is still shown, so a closing palette is neither (#617).
    readonly property bool closing: exitFade.running
    modal: !root.closing
    // A press outside closes it on the dim.
    closePolicy: root.closing ? Popup.NoAutoClose : Popup.CloseOnEscape
    // Qt drops its dim at once when modal changes, so the palette draws its
    // own, under its background.
    dim: false
    focus: true
    parent: Overlay.overlay
    // Placed by x and y, not centred as the style's dialogs are; kept
    // inside the window.
    anchors.centerIn: undefined
    margins: 0
    title: I18n.i18nc("@title:window", "Search Library")
    padding: 0
    topPadding: 0
    bottomPadding: 0
    leftPadding: 0
    rightPadding: 0
    // 800 px with the default font, wider with a larger one so its columns
    // keep their measure, and 16 px clear of the window's sides.
    readonly property int designWidth: root.uiPalette.scaledSize(800)
    width: Math.min(root.designWidth, (root.parent ? root.parent.width : root.designWidth) - 2 * root.sideInset)
    height: Math.min(contentColumn.implicitHeight, root.maximumHeight)
    // At most 16 result rows tall, as a palette rather than a page (and
    // as many rows as a query has to bind as it changes), and 32 px clear
    // of the window's bottom.
    readonly property real maximumBodyHeight: 16 * root.resultHeight
    readonly property real maximumHeight: Math.max(root.fieldHeight + root.footerHeight + 2,
        Math.min(root.fieldHeight + 1 + root.maximumBodyHeight + 1 + root.footerHeight,
                 (root.parent ? root.parent.height : 600) - root.paletteTop - 32))
    x: Math.round(((root.parent ? root.parent.width : root.width) - root.width) / 2)
    y: Math.round(root.paletteTop)
    header: null
    footer: null
    onAboutToShow: root.updatePlacement()

    background: Kirigami.ShadowedRectangle {
        objectName: "globalSearchBackground"
        color: root.uiPalette.view
        radius: 6
        // The seam where two regions meet, all round: it parts the palette
        // from the dimmed window, which on dark is nearly its colour.
        border.width: 1
        border.color: root.uiPalette.seam
        shadow.xOffset: 0
        shadow.yOffset: 4
        shadow.size: 8
        shadow.color: Qt.rgba(0, 0, 0, 0.3)

        // The dim over the whole window, the pane too, under the palette and
        // fading with it. While the palette is open, the window under it
        // takes no hover or presses, as under a modal popup's dim, and a
        // press outside the palette closes it.
        Rectangle {
            objectName: "globalSearchDim"
            z: -1
            x: -root.x
            y: -root.y
            width: root.parent ? root.parent.width : 0
            height: root.parent ? root.parent.height : 0
            color: root.uiPalette.dim

            MouseArea {
                anchors.fill: parent
                enabled: !root.closing
                acceptedButtons: Qt.AllButtons
                hoverEnabled: true
                cursorShape: Qt.ArrowCursor
                onPressed: function(mouse) {
                    // A press on the palette is the palette's.
                    const point = mapToItem(root.background, mouse.x, mouse.y)
                    mouse.accepted = !root.background.contains(point)
                    if (mouse.accepted) {
                        root.reject()
                    }
                }
            }
        }
    }

    enter: Transition {
        NumberAnimation {
            property: "opacity"; from: 0; to: 1
            duration: root.fadeDuration; easing.type: Easing.OutCubic
        }
    }
    exit: Transition {
        id: exitFade
        NumberAnimation {
            property: "opacity"; from: 1; to: 0
            duration: root.fadeDuration; easing.type: Easing.OutCubic
        }
    }

    Component.onCompleted: root.controller.registerRefs(root, queryField, resultsView)
    onOpened: root.controller.handleDialogOpened()
    onClosed: root.controller.endOpen(true)

    function escaped(value) {
        return String(value || "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }
    // `value` cut where the matches of the results' query start and end.
    function querySegments(value) {
        const text = String(value || "")
        return root.controller.globalSearchModelApi.highlightSegments
            ? root.controller.globalSearchModelApi.highlightSegments(text, root.resultsQuery)
            : [{ text: text, match: false }]
    }
    // From the segments rather than by comparing markup: the model escapes
    // a quote, so text with one always differed from escaped(), a match.
    function queryMatches(value) {
        return !!value && root.querySegments(value).some(function(segment) { return segment.match })
    }
    readonly property string contextSeparator: " · "
    // Where a result's tooltip of `tipWidth` by `tipHeight` goes, in the
    // palette's coordinates, for a row at `rowTop`. Outside the palette
    // where the window has the room, so the results around the row stay
    // readable (under its row it covered the next two, the selected one
    // among them): beside the palette, level with the row, or else under
    // the palette. Otherwise under its row, or over it where the footer is
    // nearer, or else over the palette. Never over the field or the
    // footer: null where the window has no such place.
    function toolTipPlace(rowTop, rowHeight, tipWidth, tipHeight, margin) {
        const windowWidth = root.parent ? root.parent.width : root.width
        const windowHeight = root.parent ? root.parent.height : root.height
        const outsideGap = Kirigami.Units.smallSpacing
        const rowGap = 3
        if (root.x + root.width + outsideGap + tipWidth + margin <= windowWidth) {
            const level = Math.round(root.y + rowTop + (rowHeight - tipHeight) / 2)
            const inWindow = Math.max(margin, Math.min(windowHeight - margin - tipHeight, level))
            return Qt.point(root.width + outsideGap, inWindow - root.y)
        }
        const centred = Math.round((root.width - tipWidth) / 2)
        if (root.y + root.height + outsideGap + tipHeight + margin <= windowHeight) {
            return Qt.point(centred, root.height + outsideGap)
        }
        const underRow = rowTop + rowHeight + rowGap
        if (underRow + tipHeight <= root.height - root.footerHeight - 1) return Qt.point(centred, underRow)
        const overRow = rowTop - rowGap - tipHeight
        if (overRow >= root.fieldHeight + 1) return Qt.point(centred, overRow)
        if (root.y - outsideGap - tipHeight >= margin) return Qt.point(centred, -outsideGap - tipHeight)
        return null
    }
    // A result's details after its title, in parts: who and where it is
    // from, and what else matched.
    function rowContextParts(row, full, oneFolder) {
        let parts = []
        if (row.rowType === "track") {
            const artistAlbum = [row.artist || "", row.album || ""].filter(function(value) { return value.length > 0 })
            parts.push(artistAlbum.join(" — "))
        } else if (row.rowType === "album") {
            parts.push(row.artist || "")
            if (row.year !== undefined && row.year !== null && row.year > 0) parts.push(String(row.year))
        } else if (row.rowType === "artist") {
            const count = row.count || 0
            if (count > 0) parts.push(I18n.i18ncp("@info:status", "%1 album", "%1 albums", count))
        }
        if (row.genre && (full || root.queryMatches(row.genre))) parts.push(row.genre)
        if (row.rootLabel && ((full && !oneFolder) || row.showRoot || root.queryMatches(row.rootLabel))) parts.push(row.rootLabel)
        if (row.matchDetail) parts.push(row.matchDetail)
        return parts.filter(function(value) { return value.length > 0 })
    }
    function rowContext(row, full) {
        return root.rowContextParts(row, full).join(root.contextSeparator)
    }
    // What a result's tooltip says after the title: the whole context, but
    // a folder's name only where it tells folders apart.
    function toolTipContext(row) {
        const oneFolder = (root.controller.uiBridge.libraryRootCount || 0) <= 1
        return root.rowContextParts(row, true, oneFolder).join(root.contextSeparator)
    }
    // Why a row whose title has no match was found: its context as styled
    // text, the matches in the text colour at the regular weight, the
    // metadata escaped. "" for any other row. An artist's count of albums
    // is counted, not searched, so it is no match.
    function matchedContext(row) {
        if (root.queryMatches(row.label)) return ""
        const parts = root.rowContextParts(row, false)
        const counted = row.rowType === "artist" && (row.count || 0) > 0 ? 1 : 0
        const open = '<font color="' + root.uiPalette.text + '">'
        let matched = false
        const styled = parts.map(function(part, index) {
            if (index < counted) return root.escaped(part)
            return root.querySegments(part).map(function(segment) {
                matched = matched || !!segment.match
                return segment.match ? open + root.escaped(segment.text) + "</font>" : root.escaped(segment.text)
            }).join("")
        })
        return matched ? styled.join(root.contextSeparator) : ""
    }
    // Translated once rather than in every row.
    readonly property var typeLabels: ({
        album: I18n.i18nc("@label search result type", "Album"),
        artist: I18n.i18nc("@label search result type", "Artist"),
        track: I18n.i18nc("@label search result type", "Track")
    })
    readonly property var sectionTitles: ({
        album: I18n.i18nc("@title:group search results", "Albums"),
        artist: I18n.i18nc("@title:group search results", "Artists"),
        track: I18n.i18nc("@title:group search results", "Tracks")
    })
    function typeLabel(rowType) {
        return root.typeLabels[rowType] || root.typeLabels.track
    }
    // "Show all 26 tracks", or "Showing 2,000 of 5,312 tracks".
    function moreText(kind, rowType, shown, total) {
        if (kind === "more") {
            if (total <= 2000) {
                if (rowType === "artist") return I18n.i18ncp("@action:button", "Show all %1 artist", "Show all %1 artists", total)
                if (rowType === "album") return I18n.i18ncp("@action:button", "Show all %1 album", "Show all %1 albums", total)
                return I18n.i18ncp("@action:button", "Show all %1 track", "Show all %1 tracks", total)
            }
            const most = Number(2000).toLocaleString(Qt.locale(), "f", 0)
            if (rowType === "artist") return I18n.i18ncp("@action:button", "Show %2 of %1 artist", "Show %2 of %1 artists", total, most)
            if (rowType === "album") return I18n.i18ncp("@action:button", "Show %2 of %1 album", "Show %2 of %1 albums", total, most)
            return I18n.i18ncp("@action:button", "Show %2 of %1 track", "Show %2 of %1 tracks", total, most)
        }
        const shownText = Number(shown).toLocaleString(Qt.locale(), "f", 0)
        if (rowType === "artist") return I18n.i18ncp("@info:status", "Showing %2 of %1 artist. Add words to narrow the search.", "Showing %2 of %1 artists. Add words to narrow the search.", total, shownText)
        if (rowType === "album") return I18n.i18ncp("@info:status", "Showing %2 of %1 album. Add words to narrow the search.", "Showing %2 of %1 albums. Add words to narrow the search.", total, shownText)
        return I18n.i18ncp("@info:status", "Showing %2 of %1 track. Add words to narrow the search.", "Showing %2 of %1 tracks. Add words to narrow the search.", total, shownText)
    }

    // The title of a result in up to five runs, the matches semibold: the
    // text of each, where it starts, how wide it may be and whether that
    // cuts it. A run that does not fit is elided and the runs after it are
    // left out.
    readonly property int maxTitleRuns: 5
    function titleRuns(label, allEmphasized) {
        const text = String(label || "")
        const segments = allEmphasized ? [{ text: text, match: true }] : root.querySegments(text)
        return root.layOutRuns(segments, root.maxTitleRuns, root.titleWidth, regularMetrics, semiboldMetrics)
    }
    // A result's semibold matches are in its title only: its context, such
    // as the artist an album was found by, stays whole and regular, so it
    // never outweighs the title. Without a match in the title, the match
    // in the context is in the text colour (matchedContext).
    // `segments` as at most `maxRuns` runs side by side in `room`: matches
    // past the last run but one stay regular in the last.
    function layOutRuns(segments, maxRuns, room, regular, semibold) {
        if (segments.length > maxRuns) {
            const tail = segments.slice(maxRuns - 1).map(function(segment) { return segment.text }).join("")
            segments = segments.slice(0, maxRuns - 1).concat([{ text: tail, match: false }])
        }
        let runs = []
        let x = 0
        for (let i = 0; i < segments.length; ++i) {
            const metrics = segments[i].match ? semibold : regular
            const advance = metrics.advanceWidth(segments[i].text)
            runs.push({ text: segments[i].text, match: !!segments[i].match, x: Math.round(x),
                        width: Math.min(advance, Math.max(0, room - x)), cut: x + advance > room + 0.5 })
            x += advance
        }
        return runs
    }
    FontMetrics { id: regularMetrics; font.family: root.uiPalette.baseFont.family; font.pointSize: root.uiPalette.basePointSize }
    FontMetrics { id: semiboldMetrics; font.family: root.uiPalette.baseFont.family; font.pointSize: root.uiPalette.basePointSize; font.weight: Font.DemiBold }
    FontMetrics { id: smallMetrics; font.family: root.uiPalette.baseFont.family; font.pointSize: root.uiPalette.smallPointSize }
    TextMetrics {
        id: lengthMetrics
        text: "00:00"
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.smallPointSize
        font.features: root.uiPalette.tabularFigures
    }

    // What the keys act on: the selected result's type, "more", "recent",
    // "suggestion", or "none".
    readonly property string selectionKind: {
        if (root.resultsShown) {
            const kind = resultsView.selectedRow ? (resultsView.selectedRow.kind || "") : ""
            if (kind === "item") return resultsView.selectedRow.rowType || "track"
            return kind === "more" ? "more" : "none"
        }
        return root.selectedAuxRow ? root.selectedAuxRow.kind : "none"
    }
    // What Enter, Ctrl+Enter and Alt+Enter do for the selected row, as
    // {key, action} pairs, with Esc last. Esc is left out where the pairs
    // would wrap with it and fit one line without it: the line it took at
    // a large font was a result's room, and Esc closes every dialog.
    readonly property var keyHints: root.hintsFor(root.selectionKind)
    function hintsFor(kind) {
        const pairs = root.actionHintsFor(kind)
        const close = { key: I18n.i18nc("@info:shortcut key name", "Esc"), action: I18n.i18nc("@info:shortcut action", "Close") }
        const all = pairs.concat([close])
        return pairs.length > 0 && root.hintLines(all) > 1 && root.hintLines(pairs) === 1 ? pairs : all
    }
    // The width of a pair in the footer's line, with the gap after it
    // unless it is the last, as its Row is laid out.
    function hintPairWidth(hint, last) {
        return Math.ceil(smallMetrics.advanceWidth(hint.key)) + root.hintKeyGap
            + Math.ceil(smallMetrics.advanceWidth(hint.action)) + (last ? 0 : root.hintPairGap)
    }
    // How many lines `hints` wrap to in the footer, as its Flow wraps them.
    readonly property real footerHintsWidth: root.width - root.titleX - root.sideInset
    function hintLines(hints) {
        let lines = hints.length > 0 ? 1 : 0
        let x = 0
        for (let i = 0; i < hints.length; ++i) {
            const width = root.hintPairWidth(hints[i], i === hints.length - 1)
            if (x > 0 && x + width > root.footerHintsWidth) {
                lines += 1
                x = 0
            }
            x += width
        }
        return lines
    }
    function footerHeightFor(hints) {
        return Math.max(28, root.hintLines(hints) * root.footerLineHeight + 8)
    }
    // The pairs for a row of `kind`, without Esc.
    function actionHintsFor(kind) {
        const enter = I18n.i18nc("@info:shortcut key name", "Enter")
        if (kind === "artist" || kind === "album" || kind === "track") {
            return [
                { key: enter, action: kind === "artist" ? I18n.i18nc("@info:shortcut action", "Play Artist")
                    : (kind === "album" ? I18n.i18nc("@info:shortcut action", "Play Album")
                                        : I18n.i18nc("@info:shortcut action", "Play Track")) },
                { key: I18n.i18nc("@info:shortcut key name", "Ctrl+Enter"), action: I18n.i18nc("@info:shortcut action", "Add to Queue") },
                { key: I18n.i18nc("@info:shortcut key name", "Alt+Enter"), action: I18n.i18nc("@info:shortcut action", "Show in Library") }
            ]
        }
        if (kind === "more") return [{ key: enter, action: I18n.i18nc("@info:shortcut action", "Show All") }]
        if (kind === "recent") return [{ key: enter, action: I18n.i18nc("@info:shortcut action", "Search") }]
        if (kind === "suggestion") return [{ key: enter, action: I18n.i18nc("@info:shortcut action", "Search Instead") }]
        return []
    }

    function moveAuxSelection(step, wrap) {
        const count = root.auxRows.length
        if (count === 0) return
        let next = root.auxIndex + step
        if (wrap) next = (next + count) % count
        root.auxIndex = Math.max(0, Math.min(count - 1, next))
    }
    function activateAuxRow(index) {
        const row = index >= 0 && index < root.auxRows.length ? root.auxRows[index] : null
        if (row) root.controller.searchInstead(row.text)
    }
    // The keys of the field and the results.
    function handleResultKeys(event) {
        const enterKey = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
        const upDown = event.key === Qt.Key_Down || event.key === Qt.Key_Up
        const page = event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp
        if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_F) {
            root.controller.focusQueryField(true)
        } else if (!root.resultsShown && root.auxRows.length > 0 && (enterKey || upDown || page)) {
            if (enterKey && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier))) {
                root.activateAuxRow(root.auxIndex)
            } else if (upDown) {
                root.moveAuxSelection(event.key === Qt.Key_Down ? 1 : -1, true)
            } else if (page) {
                root.moveAuxSelection(event.key === Qt.Key_PageDown ? root.auxRows.length : -root.auxRows.length, false)
            }
        } else if (enterKey) {
            if (event.modifiers & Qt.AltModifier) root.controller.navigateSelectionToLibrary()
            else if (event.modifiers & Qt.ControlModifier) root.controller.queueSelection()
            else root.controller.activateSelection()
        } else if (upDown) {
            const next = root.controller.nextSelectableIndex(root.controller.selectedDisplayIndex, event.key === Qt.Key_Down ? 1 : -1, true)
            if (next >= 0) root.controller.selectDisplayIndex(next)
        } else if (page) {
            root.controller.moveSelectionByPage(event.key === Qt.Key_PageDown ? 1 : -1)
        } else return false
        event.accepted = true
        return true
    }
    Connections {
        target: root.controller.globalSearchModelApi
        ignoreUnknownSignals: true
        function onSearchRowsChanged() {
            root.resultsQuery = queryField.text
            root.rowsRevision += 1
            resultsView.cancelFlick()
            if (!root.controller.selectAfterRowsChanged()) {
                resultsView.positionViewAtBeginning()
            }
        }
    }

    contentItem: Item {
        id: contentColumn
        implicitWidth: root.designWidth
        implicitHeight: root.fieldHeight + 1 + body.implicitHeight + 1 + root.footerHeight

        // The field: a search glyph, the query, and the clear and help
        // buttons.
        Item {
            id: fieldRow
            width: parent.width
            height: root.fieldHeight

            // Centred on the results' lead column, on one axis with the
            // artists' glyphs and the covers.
            Kirigami.Icon {
                objectName: "globalSearchFieldIcon"
                x: root.sideInset + (root.leadSize - width) / 2
                y: Math.round((parent.height - height) / 2)
                width: root.glyphSize
                height: root.glyphSize
                source: "search"
                isMask: true
                color: root.uiPalette.textSecondary
            }
            TextField {
                id: queryField
                objectName: "globalSearchQueryField"
                x: root.titleX
                width: Math.max(0, (clearButton.visible ? clearButton.x : helpButton.x) - x - 4)
                height: parent.height
                leftPadding: 0
                rightPadding: 0
                topPadding: 0
                bottomPadding: 0
                verticalAlignment: TextInput.AlignVCenter
                background: null
                font.family: root.uiPalette.baseFont.family
                font.pointSize: root.uiPalette.basePointSize
                color: root.uiPalette.text
                placeholderTextColor: root.uiPalette.textSecondary
                placeholderText: I18n.i18nc("@info:placeholder", "Search artists, albums, tracks, tags or file names")
                Accessible.name: I18n.i18nc("@label accessible name of the search field", "Search")
                onTextChanged: {
                    root.controller.pendingExpansion = null
                    root.controller.selectedDisplayIndex = -1
                    root.controller.uiBridge.setGlobalSearchQuery(text)
                    if (!root.searching) root.controller.selectFirstItem()
                }
                Keys.onPressed: function(event) { root.handleResultKeys(event) }
            }
            ToolButton {
                id: clearButton
                objectName: "globalSearchClearButton"
                x: helpButton.x - width
                y: Math.round((parent.height - height) / 2)
                width: root.fieldButtonSize
                height: root.fieldButtonSize
                visible: queryField.text.length > 0
                display: AbstractButton.IconOnly
                icon.name: "edit-clear"
                icon.width: root.glyphSize
                icon.height: root.glyphSize
                // As the other glyphs under the pointer or with the focus.
                property color glyphColor: clearButton.hovered || clearButton.visualFocus
                    ? root.uiPalette.textSecondary : root.clearGlyphRest
                icon.color: clearButton.glyphColor

                Behavior on glyphColor {
                    enabled: Kirigami.Units.shortDuration > 0
                    ColorAnimation {
                        duration: Kirigami.Units.shortDuration
                        easing.type: Easing.OutCubic
                    }
                }
                text: I18n.i18nc("@action:button clear the search field", "Clear")
                Accessible.name: text
                ToolTip.visible: hovered || visualFocus
                ToolTip.delay: Kirigami.Units.toolTipDelay
                ToolTip.text: text
                onClicked: {
                    queryField.clear()
                    root.controller.focusQueryField(false)
                }
            }
            ToolButton {
                id: helpButton
                objectName: "globalSearchHelpButton"
                x: parent.width - 8 - width
                y: Math.round((parent.height - height) / 2)
                width: root.fieldButtonSize
                height: root.fieldButtonSize
                display: AbstractButton.IconOnly
                icon.name: "question"
                icon.width: root.glyphSize
                icon.height: root.glyphSize
                icon.color: root.uiPalette.textSecondary
                text: I18n.i18nc("@action:button", "Search Syntax and Keys")
                Accessible.name: text
                ToolTip.visible: hovered || visualFocus
                ToolTip.delay: Kirigami.Units.toolTipDelay
                ToolTip.text: text
                onClicked: searchHelp.open()
            }
        }
        Rectangle {
            objectName: "globalSearchFieldSeam"
            y: root.fieldHeight
            width: parent.width
            height: 1
            color: root.uiPalette.seam
        }

        // The results, or the rows and messages without them.
        Item {
            id: body
            objectName: "globalSearchBody"
            y: root.fieldHeight + 1
            width: parent.width
            height: Math.max(0, contentColumn.height - y - 1 - root.footerHeight)
            clip: true
            // The results' height: all of them, and 6 px under the last,
            // where the palette has the room; otherwise as many rows, from
            // the first, as fit it whole, and the rest scroll (the
            // list ended in half a row). From the rows' kinds, not from the
            // view's content height, which is an estimate while only some
            // of its rows have items. A heading counts only with the row
            // under it: where both don't fit, the list ends over the
            // heading, and the scrollbar leads on to them (a heading that
            // ended the list started a section with nothing in it).
            readonly property real resultsHeight: {
                const room = root.maximumHeight - root.fieldHeight - 2 - root.footerHeight
                const rows = root.controller.globalSearchModelApi
                let used = 0
                for (let i = 0; root.rowsRevision >= 0 && i < root.rowCount; ++i) {
                    const kind = rows.rowDataAt(i).kind || ""
                    const rowHeight = root.rowHeightAt(kind, i)
                    const firstRowHeight = kind === "section" && i + 1 < root.rowCount
                        ? root.rowHeightAt(rows.rowDataAt(i + 1).kind || "", i + 1) : 0
                    if (used + rowHeight + firstRowHeight > room) {
                        return used
                    }
                    used += rowHeight
                }
                return used + resultsView.bottomMargin <= room ? used + resultsView.bottomMargin : used
            }
            implicitHeight: root.resultsShown
                ? body.resultsHeight
                : (noResultsMessage.visible ? noResultsMessage.y + noResultsMessage.height : 0)
                    + (auxColumn.visible ? auxColumn.implicitHeight : 0)
                    + (noResultsMessage.visible || auxColumn.visible ? 8 : 0)

            ListView {
                id: resultsView
                objectName: "globalSearchResultsView"
                width: parent.width
                height: parent.height
                visible: root.resultsShown
                clip: true
                model: root.controller.uiBridge.globalSearchModel || []
                reuseItems: true
                // Each query makes only the rows in view; rows scrolled in
                // come from the reuse pool.
                cacheBuffer: 0
                bottomMargin: 6
                readonly property real resultRowHeight: root.resultHeight
                // The selected row's data, for the key hints.
                readonly property var selectedRow: {
                    const index = root.controller.selectedDisplayIndex
                    return root.rowsRevision >= 0 && index >= 0 && root.rowCount > index
                        ? root.controller.globalSearchModelApi.rowDataAt(index) : null
                }
                currentIndex: root.controller.selectedDisplayIndex
                highlightFollowsCurrentItem: false
                keyNavigationEnabled: false
                boundsBehavior: Flickable.StopAtBounds
                flickDeceleration: root.snappyScrollFlickDeceleration
                maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                // The rows a running search is to replace take no keys,
                // clicks or scrolling. The view stays enabled: a disabled
                // Kirigami icon draws dimmed, so the glyphs dimmed on each
                // keystroke.
                interactive: !root.searching
                // The busy indicator of a slow search: the rows it is to
                // replace dim. A keystroke's search takes tens of
                // milliseconds, and dimming for it flashed the list, so the
                // indicator has a threshold, the animation's pause: it
                // shows for a search still running 250 ms after its query,
                // each query starting the pause again, and goes as the
                // search ends.
                readonly property string runningQuery: root.searching ? root.queryText : ""
                onRunningQueryChanged: {
                    if (resultsView.runningQuery.length === 0) {
                        slowSearchDim.stop()
                        resultsView.opacity = 1
                    } else if (resultsView.opacity === 1) {
                        slowSearchDim.restart()
                    }
                }
                SequentialAnimation {
                    id: slowSearchDim
                    objectName: "globalSearchSlowSearchDim"
                    PauseAnimation { duration: 250 }
                    NumberAnimation {
                        target: resultsView
                        property: "opacity"
                        to: 0.5
                        duration: Kirigami.Units.shortDuration
                        easing.type: Easing.OutCubic
                    }
                }
                // The app's lane scrollbar, as the library's and the
                // queue's (Breeze's own was another hand here):
                // alone in the lane at the right, where their tints end
                // while it shows (it cut a pale piece out of the selected
                // row).
                ScrollBar.vertical: Components.LaneScrollBar {
                    id: resultScrollBar
                    objectName: "globalSearchScrollBar"
                    uiPalette: root.uiPalette
                    interactive: !root.searching
                }
                // The lane the rows' tints leave for the scrollbar, when it
                // shows. Their text leaves it always, so that the lengths
                // and where the context is cut stay put as typing brings
                // the scrollbar and takes it away.
                readonly property real scrollbarLane: resultScrollBar.visible ? resultScrollBar.lane : 0
                readonly property real reservedLane: resultScrollBar.lane
                Keys.onPressed: function(event) {
                    if (root.searching) return
                    if (root.handleResultKeys(event)) return
                    if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                        root.controller.selectDisplayIndex(event.key === Qt.Key_Home
                            ? root.controller.searchFirstSelectableIndex() : root.controller.searchLastSelectableIndex())
                        event.accepted = true
                    }
                }
                // The rows are plain Text and Rectangles with the shared
                // palette's colours rather than styled controls, which
                // each set up a Kirigami theme of their own.
                delegate: Item {
                    id: resultRow
                    required property int index
                    required property var model
                    readonly property string kind: model.kind || ""
                    readonly property string rowType: model.rowType || "track"
                    readonly property bool isResult: kind === "item"
                    readonly property bool isMore: kind === "more"
                    readonly property bool selectable: isResult || isMore
                    readonly property bool selected: selectable && index === root.controller.selectedDisplayIndex
                    readonly property bool hovered: selectable && rowMouse.containsMouse
                    // The more link underlines on hover rather than taking
                    // the hover tint, on which the light link is under
                    // 4.5:1.
                    // The track that plays or is paused now.
                    readonly property bool isNow: isResult && rowType === "track"
                        && (model.trackPath || "").length > 0
                        && model.trackPath === (root.controller.uiBridge.currentTrackPath || "")
                        && (root.controller.uiBridge.playbackState || "Stopped") !== "Stopped"
                    readonly property var titleRuns: isResult ? root.titleRuns(model.label, isNow) : []
                    // On the selection the secondary marks and the amber
                    // step up to hold their contrast on its tint, as the
                    // library's and the queue's do.
                    readonly property color titleColor: !isNow ? root.uiPalette.text
                        : (selected ? root.uiPalette.nowSelected : root.uiPalette.now)
                    readonly property color secondaryColor: selected
                        ? root.uiPalette.textSecondarySelected : root.uiPalette.textSecondary
                    readonly property string context: isResult ? root.rowContext(model, false) : ""
                    readonly property string matchedContext: isResult ? root.matchedContext(model) : ""
                    // The context's room: up to the length's column, or the
                    // row's end.
                    readonly property real contextWidth: Math.max(0, (lengthText.visible
                        ? lengthText.x - root.columnGap
                        : width - root.sideInset - resultsView.reservedLane) - root.contextX)
                    // Asked for only when the shared tooltip shows.
                    function toolTipText() {
                        return [model.label, root.toolTipContext(model)].filter(function(line) {
                            return !!line
                        }).join("\n")
                    }
                    // Whether the tooltip says what the row does not: the
                    // row is elided, or there is more.
                    function toolTipAdds() {
                        return resultContext.truncated
                            || resultRow.titleRuns.some(function(run) { return run.cut })
                            || root.toolTipContext(model) !== resultRow.context
                    }
                    width: ListView.view.width
                    height: root.rowHeightAt(kind, index)
                    Accessible.role: isResult ? Accessible.ListItem : (isMore ? Accessible.Link : Accessible.StaticText)
                    Accessible.name: isResult
                        ? I18n.i18nc("@info accessible name of a search result: type, name, details",
                                     "%1: %2, %3", root.typeLabel(rowType), model.label || "", root.rowContext(model, true))
                        : (kind === "section" ? (root.sectionTitles[rowType] || "")
                                              : root.moreText(kind, rowType, model.shownCount || 0, model.count || 0))
                    Accessible.selectable: selectable
                    Accessible.selected: selected
                    // The result under the pointer has the tooltip, also
                    // when new results turn the row under a still pointer
                    // into a result or out of one.
                    readonly property bool ownsToolTip: isResult && rowMouse.containsMouse
                    onOwnsToolTipChanged: {
                        if (ownsToolTip) root.hoveredResultRow = resultRow
                        else if (root.hoveredResultRow === resultRow) root.hoveredResultRow = null
                    }
                    ListView.onPooled: {
                        if (root.hoveredResultRow === resultRow) root.hoveredResultRow = null
                    }

                    Components.RowBackground {
                        uiPalette: root.uiPalette
                        visible: resultRow.selectable
                        hovered: resultRow.hovered && resultRow.isResult
                        selected: resultRow.selected
                        active: true
                        focused: false
                        scrollbarLane: resultsView.scrollbarLane
                    }

                    // A section's heading.
                    Text {
                        objectName: "globalSearchSectionHeading"
                        visible: resultRow.kind === "section"
                        x: root.titleX
                        width: parent.width - x - root.sideInset
                        height: parent.height
                        bottomPadding: root.headingBottomPadding
                        verticalAlignment: Text.AlignBottom
                        textFormat: Text.PlainText
                        text: visible ? (root.sectionTitles[resultRow.rowType] || "") : ""
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.smallPointSize
                        font.weight: Font.DemiBold
                        color: root.uiPalette.textSecondary
                        elide: Text.ElideRight
                    }

                    // "Show all 26 tracks", or how many of them show.
                    Text {
                        objectName: resultRow.isMore ? "globalSearchMoreLink" : "globalSearchTruncatedNote"
                        visible: resultRow.isMore || resultRow.kind === "truncated"
                        x: root.titleX
                        width: parent.width - x - root.sideInset - resultsView.reservedLane
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        textFormat: Text.PlainText
                        text: visible ? root.moreText(resultRow.kind, resultRow.rowType, model.shownCount || 0, model.count || 0) : ""
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.smallPointSize
                        font.underline: resultRow.isMore && resultRow.hovered
                        // The link colour, but the text colour on the
                        // selection, where the light link is under 4.5:1.
                        color: !resultRow.isMore ? root.uiPalette.textSecondary
                            : (resultRow.selected ? root.uiPalette.text : root.uiPalette.link)
                        elide: Text.ElideRight
                    }

                    // The lead: an artist's glyph, or the cover or the
                    // library's no-cover tile, with the amber glyph over it
                    // while its track plays. The glyphs and the tile are
                    // loaded only for the rows that show one, each with
                    // a source that never changes: a Kirigami icon looks its
                    // file up again on the GUI thread whenever its source
                    // changes, which recycled rows would do on every query.
                    Loader {
                        id: artistGlyphLoader
                        x: root.sideInset + (root.leadSize - root.glyphSize) / 2
                        y: Math.round((parent.height - root.glyphSize) / 2)
                        active: resultRow.isResult && resultRow.rowType === "artist"
                        sourceComponent: artistGlyph
                    }
                    // The artist's glyph in the row's secondary colour,
                    // selected too.
                    Binding {
                        target: artistGlyphLoader.item
                        property: "color"
                        value: resultRow.secondaryColor
                        when: artistGlyphLoader.status === Loader.Ready
                    }
                    Loader {
                        x: root.sideInset
                        y: Math.round((parent.height - root.leadSize) / 2)
                        active: resultRow.isResult && resultRow.rowType !== "artist"
                            && (model.coverUrl || "").length === 0
                        sourceComponent: noCoverTile
                    }
                    Kirigami.ShadowedImage {
                        id: thumbnail
                        objectName: "globalSearchThumbnail"
                        visible: resultRow.isResult && resultRow.rowType !== "artist" && (model.coverUrl || "").length > 0
                        x: root.sideInset
                        y: Math.round((parent.height - height) / 2)
                        width: root.leadSize
                        height: root.leadSize
                        radius: 2
                        color: "transparent"
                        source: visible ? model.coverUrl : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize.width: Math.ceil(root.leadSize * Screen.devicePixelRatio)
                        sourceSize.height: Math.ceil(root.leadSize * Screen.devicePixelRatio)
                    }
                    Rectangle {
                        objectName: "globalSearchNowScrim"
                        visible: resultRow.isNow
                        x: thumbnail.x
                        y: thumbnail.y
                        width: root.leadSize
                        height: root.leadSize
                        radius: 2
                        color: Qt.rgba(0, 0, 0, 0.55)
                    }
                    Loader {
                        x: root.sideInset + (root.leadSize - root.nowGlyphSize) / 2
                        y: Math.round((parent.height - root.nowGlyphSize) / 2)
                        active: resultRow.isNow
                        sourceComponent: Kirigami.Icon {
                            objectName: "globalSearchNowGlyph"
                            width: root.nowGlyphSize
                            height: root.nowGlyphSize
                            source: (root.controller.uiBridge.playbackState || "") === "Paused"
                                ? "media-playback-pause" : "media-playback-start"
                            isMask: true
                            // Over the dark scrim on the cover or the tile,
                            // the colour map's bright amber in both themes
                            // (fixed, like the plot's colours): the light
                            // theme's darker amber is under 3:1 there.
                            color: root.nowOnScrim
                        }
                    }

                    // The title in runs (root.maxTitleRuns of them), the
                    // matches semibold; all amber and semibold while it
                    // plays. Plain items rather than a Repeater, which
                    // would make each row slower to create.
                    Text {
                        readonly property var run: resultRow.titleRuns.length > 0 ? resultRow.titleRuns[0] : null
                        objectName: "globalSearchResultTitle"
                        visible: !!run && run.width > 0
                        x: root.titleX + (run ? run.x : 0)
                        width: run ? run.width : 0
                        height: resultRow.height
                        verticalAlignment: Text.AlignVCenter
                        text: run ? run.text : ""
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.basePointSize
                        font.weight: run && run.match ? Font.DemiBold : Font.Normal
                        color: resultRow.titleColor
                    }
                    Text {
                        readonly property var run: resultRow.titleRuns.length > 1 ? resultRow.titleRuns[1] : null
                        objectName: "globalSearchResultTitle"
                        visible: !!run && run.width > 0
                        x: root.titleX + (run ? run.x : 0)
                        width: run ? run.width : 0
                        height: resultRow.height
                        verticalAlignment: Text.AlignVCenter
                        text: run ? run.text : ""
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.basePointSize
                        font.weight: run && run.match ? Font.DemiBold : Font.Normal
                        color: resultRow.titleColor
                    }
                    Text {
                        readonly property var run: resultRow.titleRuns.length > 2 ? resultRow.titleRuns[2] : null
                        objectName: "globalSearchResultTitle"
                        visible: !!run && run.width > 0
                        x: root.titleX + (run ? run.x : 0)
                        width: run ? run.width : 0
                        height: resultRow.height
                        verticalAlignment: Text.AlignVCenter
                        text: run ? run.text : ""
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.basePointSize
                        font.weight: run && run.match ? Font.DemiBold : Font.Normal
                        color: resultRow.titleColor
                    }
                    Text {
                        readonly property var run: resultRow.titleRuns.length > 3 ? resultRow.titleRuns[3] : null
                        objectName: "globalSearchResultTitle"
                        visible: !!run && run.width > 0
                        x: root.titleX + (run ? run.x : 0)
                        width: run ? run.width : 0
                        height: resultRow.height
                        verticalAlignment: Text.AlignVCenter
                        text: run ? run.text : ""
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.basePointSize
                        font.weight: run && run.match ? Font.DemiBold : Font.Normal
                        color: resultRow.titleColor
                    }
                    Text {
                        readonly property var run: resultRow.titleRuns.length > 4 ? resultRow.titleRuns[4] : null
                        objectName: "globalSearchResultTitle"
                        visible: !!run && run.width > 0
                        x: root.titleX + (run ? run.x : 0)
                        width: run ? run.width : 0
                        height: resultRow.height
                        verticalAlignment: Text.AlignVCenter
                        text: run ? run.text : ""
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.basePointSize
                        font.weight: run && run.match ? Font.DemiBold : Font.Normal
                        color: resultRow.titleColor
                    }
                    // The context, whole and regular; where the title has
                    // no match, its matches are in the text colour.
                    Text {
                        id: resultContext
                        objectName: "globalSearchResultContext"
                        visible: resultRow.isResult
                        x: root.contextX
                        width: resultRow.contextWidth
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        // Always styled and always escaped: a format that
                        // followed the text changed after it, so a reused
                        // row read its metadata as markup for a moment.
                        text: resultRow.matchedContext || root.escaped(resultRow.context)
                        textFormat: Text.StyledText
                        elide: Text.ElideRight
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.smallPointSize
                        color: resultRow.secondaryColor
                    }
                    Text {
                        id: lengthText
                        objectName: "globalSearchResultLength"
                        visible: resultRow.isResult && resultRow.rowType === "track"
                        x: parent.width - root.sideInset - resultsView.reservedLane - width
                        // Wider for a length of an hour or more.
                        width: Math.max(root.lengthWidth, Math.ceil(implicitWidth))
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignRight
                        textFormat: Text.PlainText
                        text: visible ? (model.lengthText || "") : ""
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.smallPointSize
                        font.features: root.uiPalette.tabularFigures
                        color: resultRow.secondaryColor
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        enabled: resultRow.selectable
                        hoverEnabled: true
                        cursorShape: resultRow.isMore ? Qt.PointingHandCursor : Qt.ArrowCursor
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onPressed: function(mouse) {
                            mouse.accepted = !root.searching
                        }
                        onClicked: function(mouse) {
                            if (root.searching) return
                            root.controller.selectDisplayIndex(resultRow.index)
                            if (resultRow.isMore && mouse.button === Qt.LeftButton) {
                                root.controller.activateSelection()
                            } else if (mouse.button === Qt.RightButton && resultRow.isResult) {
                                root.contextRowData = root.controller.globalSearchModelApi.rowDataAt(resultRow.index)
                                // Where the click was, from the event rather
                                // than the cursor's last known place.
                                resultMenu.popupAt(rowMouse, mouse.x, mouse.y)
                            } else resultsView.forceActiveFocus()
                        }
                        onDoubleClicked: function(mouse) {
                            if (mouse.button === Qt.LeftButton && resultRow.isResult) root.controller.activateSelection()
                        }
                    }
                }
            }

            // When nothing was found: what was searched, and a suggested
            // spelling or a hint.
            Item {
                id: noResultsMessage
                objectName: "globalSearchNoResults"
                visible: root.noResults
                y: 8
                width: parent.width
                height: root.suggestion.length > 0 ? root.resultHeight : root.resultHeight + root.hintHeight
                Text {
                    objectName: "globalSearchNoResultsTitle"
                    x: root.titleX
                    width: parent.width - x - root.sideInset
                    height: root.resultHeight
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    text: I18n.i18nc("@info", "No matches for “%1”", root.queryText)
                    font.family: root.uiPalette.baseFont.family
                    font.pointSize: root.uiPalette.basePointSize
                    color: root.uiPalette.text
                }
                Text {
                    objectName: "globalSearchNoResultsHint"
                    visible: root.suggestion.length === 0
                    x: root.titleX
                    y: root.resultHeight - 6
                    width: parent.width - x - root.sideInset
                    height: root.hintHeight
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    text: I18n.i18nc("@info", "Check the spelling, or try fewer words.")
                    font.family: root.uiPalette.baseFont.family
                    font.pointSize: root.uiPalette.smallPointSize
                    color: root.uiPalette.textSecondary
                }
            }

            // The recent searches, or the suggested spelling.
            Column {
                id: auxColumn
                objectName: "globalSearchAuxRows"
                visible: !root.resultsShown && root.auxRows.length > 0
                y: noResultsMessage.visible ? noResultsMessage.y + noResultsMessage.height : 0
                width: parent.width

                Text {
                    objectName: "globalSearchRecentHeading"
                    visible: root.queryText.length === 0
                    x: root.titleX
                    width: parent.width - x - root.sideInset
                    height: visible ? root.headingHeight : 0
                    bottomPadding: root.headingBottomPadding
                    verticalAlignment: Text.AlignBottom
                    textFormat: Text.PlainText
                    text: I18n.i18nc("@title:group search palette", "Recent Searches")
                    font.family: root.uiPalette.baseFont.family
                    font.pointSize: root.uiPalette.smallPointSize
                    font.weight: Font.DemiBold
                    color: root.uiPalette.textSecondary
                }
                Repeater {
                    model: auxColumn.visible ? root.auxRows : []
                    delegate: Item {
                        id: auxRow
                        objectName: modelData.kind === "recent" ? "globalSearchRecentRow" : "globalSearchSuggestionRow"
                        required property var modelData
                        required property int index
                        readonly property bool selected: index === root.auxIndex
                        width: auxColumn.width
                        height: root.resultHeight
                        Accessible.role: Accessible.ListItem
                        Accessible.name: auxTitle.text
                        Accessible.selectable: true
                        Accessible.selected: selected

                        Components.RowBackground {
                            uiPalette: root.uiPalette
                            hovered: auxMouse.containsMouse
                            selected: auxRow.selected
                            active: true
                            focused: false
                            scrollbarLane: 0
                        }
                        Kirigami.Icon {
                            x: root.sideInset + (root.leadSize - width) / 2
                            y: Math.round((parent.height - height) / 2)
                            width: root.glyphSize
                            height: root.glyphSize
                            source: auxRow.modelData.kind === "recent" ? "view-history" : "search"
                            isMask: true
                            color: root.uiPalette.textSecondary
                        }
                        Text {
                            id: auxTitle
                            objectName: "globalSearchAuxTitle"
                            x: root.titleX
                            width: parent.width - x - root.sideInset
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: auxRow.modelData.kind === "recent"
                                ? auxRow.modelData.text
                                : I18n.i18nc("@action:button %1 is a search query", "Search ‘%1’ instead", auxRow.modelData.text)
                            font.family: root.uiPalette.baseFont.family
                            font.pointSize: root.uiPalette.basePointSize
                            color: root.uiPalette.text
                        }
                        MouseArea {
                            id: auxMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                root.auxIndex = auxRow.index
                                root.activateAuxRow(auxRow.index)
                            }
                        }
                    }
                }
            }
        }

        // The keys for the selected row.
        // Not when the footer meets the field's seam.
        Rectangle {
            objectName: "globalSearchFooterHairline"
            visible: body.height > 0
            y: footerBar.y - 1
            width: parent.width
            height: 1
            color: root.uiPalette.hairline
        }
        Item {
            id: footerBar
            objectName: "globalSearchFooter"
            y: parent.height - height
            width: parent.width
            height: root.footerHeight
            // The pairs wrap inside the palette when they don't fit on one
            // line, as with a large font.
            Flow {
                id: footerHints
                x: root.titleX
                y: Math.round((parent.height - height) / 2)
                width: parent.width - x - root.sideInset
                Repeater {
                    model: root.keyHints
                    delegate: Row {
                        required property var modelData
                        required property int index
                        objectName: "globalSearchKeyHint"
                        readonly property string keyText: modelData.key
                        readonly property string actionText: modelData.action
                        height: root.footerLineHeight
                        spacing: root.hintKeyGap
                        Text {
                            objectName: "globalSearchKeyHintKey"
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            textFormat: Text.PlainText
                            text: modelData.key
                            font.family: root.uiPalette.baseFont.family
                            font.pointSize: root.uiPalette.smallPointSize
                            color: root.uiPalette.text
                        }
                        Text {
                            objectName: "globalSearchKeyHintAction"
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            textFormat: Text.PlainText
                            text: modelData.action
                            font.family: root.uiPalette.baseFont.family
                            font.pointSize: root.uiPalette.smallPointSize
                            color: root.uiPalette.textSecondary
                        }
                        // The gap to the next pair; a Flow's spacing
                        // would put it between the lines too.
                        Item {
                            visible: index < root.keyHints.length - 1
                            width: root.hintPairGap - parent.spacing
                            height: 1
                        }
                    }
                }
            }
        }

        // One tooltip for the result under the pointer, rather than one for
        // each row, where toolTipPlace finds it room: the style's place,
        // above the row, covered the query field. It keeps its place, and
        // what it said, as it fades out: following the rows and the
        // palette's height there, it moved over the field.
        Components.PlainToolTip {
            id: resultToolTip
            objectName: "globalSearchResultToolTip"
            readonly property var place: root.toolTipRow
                ? root.toolTipPlace(body.y + root.toolTipRow.y - resultsView.contentY, root.toolTipRow.height,
                                    resultToolTip.implicitWidth, resultToolTip.implicitHeight,
                                    Math.max(0, resultToolTip.margins))
                : null
            readonly property bool placing: !!root.toolTipTarget && !!resultToolTip.place
            // As wide as its longest line, up to 320 px, scaled, and to the
            // room beside the palette where a line can be 160 px there. The
            // style wraps at 252 px whatever the font: at 24 pt, nine lines
            // that covered the list and fitted nowhere in a small window.
            readonly property real textWidthLimit: {
                const sides = resultToolTip.leftPadding + resultToolTip.rightPadding
                const beside = (root.parent ? root.parent.width : root.width) - root.x - root.width
                    - Kirigami.Units.smallSpacing - Math.max(0, resultToolTip.margins) - sides
                const readable = root.uiPalette.scaledSize(160)
                const room = beside >= readable ? beside : root.width - 2 * root.sideInset - sides
                return Math.floor(Math.min(root.uiPalette.scaledSize(320), room))
            }
            contentItem: Item {
                implicitWidth: toolTipLabel.width
                implicitHeight: toolTipLabel.implicitHeight
                Label {
                    id: toolTipLabel
                    objectName: "globalSearchResultToolTipText"
                    width: Math.min(Math.ceil(toolTipNaturalSize.implicitWidth), resultToolTip.textWidthLimit)
                    textFormat: Text.PlainText
                    text: resultToolTip.text
                    wrapMode: Text.Wrap
                    font: resultToolTip.font
                    color: Kirigami.Theme.textColor
                    Kirigami.Theme.colorSet: resultToolTip.Kirigami.Theme.colorSet
                }
                // The lines unwrapped, measured apart: the label's own width
                // would follow from itself.
                Text {
                    id: toolTipNaturalSize
                    visible: false
                    textFormat: Text.PlainText
                    text: resultToolTip.text
                    font: resultToolTip.font
                    renderType: toolTipLabel.renderType
                }
            }
            parent: contentColumn
            Binding on x {
                when: resultToolTip.placing
                value: resultToolTip.place ? resultToolTip.place.x : 0
                restoreMode: Binding.RestoreNone
            }
            Binding on y {
                when: resultToolTip.placing
                value: resultToolTip.place ? resultToolTip.place.y : 0
                restoreMode: Binding.RestoreNone
            }
            visible: resultToolTip.placing
            delay: Kirigami.Units.toolTipDelay
            message: root.toolTipText
        }

        Popup {
            id: searchHelp
            objectName: "globalSearchHelp"
            focus: true
            width: Math.min(root.uiPalette.scaledSize(480), root.width - 2 * root.sideInset)
            x: root.width - width - 8
            y: root.fieldHeight
            padding: 12
            contentItem: Label {
                // The filter names are search syntax, the same in every language.
                textFormat: Text.PlainText
                text: I18n.i18nc("@info search help", "Use \"quotes\" for phrases. Combine words and filters to narrow results.")
                    + '\n\ntype:album · type:track · type:artist\n'
                    + 'title: · artist: · album: · albumartist: · genre: · year: · date:\n'
                    + 'composer: · conductor: · performer: · label: · comment: · lyrics:\n'
                    + 'root: · path: · track: · disc:\n\n'
                    + I18n.i18nc("@info search help; keep the query as it is", "Example: %1", "signify year:1996 type:album")
                    + '\n\n'
                    + I18n.i18nc("@info search help",
                        "Up/Down: select · Page Up/Down: move by page\nEnter: play · Ctrl+Enter: add to the queue · Alt+Enter: show in the library\nCtrl+F: edit the query · Esc: close")
                wrapMode: Text.Wrap
            }
        }
        // The Menu key and Shift+F10 open the selected result's menu under
        // its row, as the queue's and the library's do. A shortcut, so the
        // query field's own text menu doesn't open too; with no result
        // selected, the keys stay the field's.
        Shortcut {
            sequences: ["Menu", "Shift+F10"]
            context: Qt.WindowShortcut
            enabled: root.opened && root.resultsShown && !root.searching && !resultMenu.visible
                && !!resultsView.selectedRow && resultsView.selectedRow.kind === "item"
            onActivated: {
                const index = root.controller.selectedDisplayIndex
                root.contextRowData = resultsView.selectedRow
                resultsView.positionViewAtIndex(index, ListView.Contain)
                const row = resultsView.itemAtIndex(index)
                if (row) resultMenu.popupAt(row, 0, row.height)
                else resultMenu.popupAt(resultsView, 0, 0)
            }
        }
        Components.AppMenu {
            id: resultMenu
            objectName: "globalSearchResultMenu"
            readonly property string rowType: root.contextRowData.rowType || "track"

            // One of the app's context menus: what it acts on in the
            // library's words and icons, with the footer's keys, and Show
            // in Library with the folder, as in the queue's.
            Components.KeyHintMenuItem {
                text: resultMenu.rowType === "artist" ? I18n.i18nc("@action:inmenu", "Play Artist")
                    : (resultMenu.rowType === "album" ? I18n.i18nc("@action:inmenu", "Play Album")
                                                      : I18n.i18nc("@action:inmenu", "Play Track"))
                icon.name: "media-playback-start"
                keyHint: I18n.i18nc("@info:shortcut key name", "Enter")
                onTriggered: root.controller.activateRow(root.contextRowData)
            }
            Components.KeyHintMenuItem {
                text: resultMenu.rowType === "artist" ? I18n.i18nc("@action:inmenu", "Add Artist to Queue")
                    : (resultMenu.rowType === "album" ? I18n.i18nc("@action:inmenu", "Add Album to Queue")
                                                      : I18n.i18nc("@action:inmenu", "Add Track to Queue"))
                icon.name: "media-playlist-append"
                keyHint: I18n.i18nc("@info:shortcut key name", "Ctrl+Enter")
                onTriggered: root.controller.queueRow(root.contextRowData)
            }
            MenuSeparator {}
            Components.KeyHintMenuItem {
                text: I18n.i18nc("@action:inmenu", "Show in Library")
                icon.name: "go-jump"
                keyHint: I18n.i18nc("@info:shortcut key name", "Alt+Enter")
                onTriggered: root.controller.navigateSelectionToLibrary()
            }
            MenuItem {
                text: resultMenu.rowType === "track" ? I18n.i18nc("@action:inmenu", "Open Containing Folder")
                                                     : I18n.i18nc("@action:inmenu", "Open Folder")
                icon.name: "folder-open"
                onTriggered: root.controller.openRowInFileBrowser(root.contextRowData)
            }
        }
    }
    // The result row under the pointer.
    property Item hoveredResultRow: null
    // The row whose tooltip shows, none when it would add nothing or while
    // a search is to replace the rows; and the last row that had one, with
    // what it said, for the fade out.
    readonly property Item toolTipTarget: root.hoveredResultRow && root.hoveredResultRow.hovered
        && !root.searching && root.hoveredResultRow.toolTipAdds() ? root.hoveredResultRow : null
    readonly property string toolTipTargetText: root.toolTipTarget ? root.toolTipTarget.toolTipText() : ""
    property Item toolTipRow: null
    property string toolTipText: ""
    onToolTipTargetChanged: if (root.toolTipTarget) root.toolTipRow = root.toolTipTarget
    onToolTipTargetTextChanged: if (root.toolTipTargetText.length > 0) root.toolTipText = root.toolTipTargetText
    // The leads of the rows without a cover: an artist's glyph, and for an
    // album or a track the library's tile, so the app has one way to say
    // "no cover". (Breeze's album and track glyphs were no mark at a large
    // font: their larger drawings, a green disc and a solid sheet, masked
    // to a blob at 24 pt.)
    Component {
        id: artistGlyph
        Kirigami.Icon {
            objectName: "globalSearchArtistGlyph"
            width: root.glyphSize
            height: root.glyphSize
            source: "view-media-artist"
            isMask: true
            color: root.uiPalette.textSecondary
        }
    }
    Component {
        id: noCoverTile
        Components.NoCoverTile {
            objectName: "globalSearchCoverPlaceholder"
            uiPalette: root.uiPalette
            size: root.leadSize
            // A thumbnail's here.
            radius: 2
        }
    }
}
