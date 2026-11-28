// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

// The search palette (#373), in the frame Find in Queue shares
// (Components.SearchPalette): the field, then the results in sections in
// the order of their best match, then the keys for the selected row.
Components.SearchPalette {
    id: root
    objectName: "globalSearchDialog"
    namePrefix: "globalSearch"
    required property var controller
    property var contextRowData: ({})
    searching: !!root.controller.uiBridge.globalSearchBusy
    readonly property bool retained: !!root.controller.uiBridge.globalSearchModelRetained
    readonly property string suggestion: root.controller.uiBridge.globalSearchSuggestion || ""
    rowCount: root.controller.globalSearchModelApi.count || 0
    resultsShown: root.queryText.length > 0 && !root.retained && root.rowCount > 0
    noResults: root.queryText.length > 0 && !root.searching && !root.retained && root.rowCount === 0
    highlighter: root.controller.globalSearchModelApi
    playbackPaused: (root.controller.uiBridge.playbackState || "") === "Paused"
    resultMenu: resultMenu
    keyHints: root.hintsFor(root.selectionKind)
    title: I18n.i18nc("@title:window", "Search Library")
    fieldPlaceholderText: I18n.i18nc("@info:placeholder", "Search artists, albums, tracks, tags or file names")
    fieldAccessibleName: I18n.i18nc("@label accessible name of the search field", "Search")
    // The filter names are search syntax, the same in every language.
    helpText: I18n.i18nc("@info search help", "Use \"quotes\" for phrases. Combine words and filters to narrow results.")
        + '\n\ntype:album · type:track · type:artist\n'
        + 'title: · artist: · album: · albumartist: · genre: · year: · date:\n'
        + 'composer: · conductor: · performer: · label: · comment: · lyrics:\n'
        + 'root: · path: · track: · disc:\n\n'
        + I18n.i18nc("@info search help; keep the query as it is", "Example: %1", "signify year:1996 type:album")
        + '\n\n'
        + I18n.i18nc("@info search help",
            "Up/Down: select · Page Up/Down: move by page\nEnter: play · Ctrl+Enter: add to the queue · Alt+Enter: show in the library\nCtrl+F: edit the query · Esc: close")
    // The palette opens where five results fit under two headings, with
    // the footer of the tallest result's keys.
    leastBodyHeight: root.rowHeightAt("section", 0) + root.rowHeightAt("section", 1)
        + root.leastResultRows * root.resultHeight
    leastFooterHeight: Math.max.apply(null, ["artist", "album", "track"].map(function(kind) {
        return root.footerHeightFor(root.hintsFor(kind))
    }))
    bodyHeight: root.resultsShown
        ? root.resultsHeight
        : (noResultsMessage.visible ? noResultsMessage.y + noResultsMessage.height : 0)
            + (auxColumn.visible ? auxColumn.implicitHeight : 0)
            + (noResultsMessage.visible || auxColumn.visible ? root.messageGap : 0)

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

    readonly property real headingHeight: Math.max(28, Math.round(root.smallMetrics.height) + 8)
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
    // The glyphs in the lead column (an artist's, a recent search's, the
    // suggestion's): about 3/4 of the lead at a large font, so they fill
    // it as a cover does, and the field's 16 px at the default font. They
    // and the field's glyph draw at their own size (roundToIconSize:
    // false): rounded down to an icon size, they were 32 px at 24 pt.
    readonly property int leadGlyphSize: Math.min(root.glyphSize, 2 * Math.round(root.leadSize * 0.75 / 2))
    // Counts the result rows' changes, for what depends on the selected
    // row's data rather than on its index.
    property int rowsRevision: 0

    // The results' height: all of them, and 6 px under the last, where the
    // palette has the room; otherwise as many rows, from the first, as fit
    // it whole, and the rest scroll (the list ended in half a row). From
    // the rows' kinds, not from the view's content height, which is an
    // estimate while only some of its rows have items. A heading counts
    // only with the row under it: where both don't fit, the list ends over
    // the heading, and the scrollbar leads on to them (a heading that ended
    // the list started a section with nothing in it).
    readonly property real resultsHeight: {
        const room = root.bodyRoom
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

    Component.onCompleted: root.controller.registerRefs(root, root.queryField, resultsView)
    onOpened: root.controller.handleDialogOpened()
    onAboutToHide: root.controller.pendingAction = ""
    onClosed: root.controller.endOpen(true)
    onQueryEdited: {
        root.controller.pendingAction = ""
        root.controller.pendingExpansion = null
        root.controller.selectedDisplayIndex = -1
        root.controller.uiBridge.setGlobalSearchQuery(root.queryField.text)
        if (!root.searching) root.controller.selectFirstItem()
    }
    onFieldKeyPressed: function(event) { root.handleResultKeys(event) }

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
    // Why a row whose title has no match was found: its context in runs
    // (root.maxContextRuns of them), the matches semibold in the row's own
    // secondary colour, so weight alone marks them and the titles stay the
    // brightest column. [] for any other row, whose context is one text,
    // and for the playing track, whose title is all semibold. An artist's
    // count of albums is counted, not searched, so it is no match. The last
    // run drawn takes the rest of the room, so the context's column ends
    // where a whole context's does.
    readonly property int maxContextRuns: 5
    function contextRuns(row, room, isNow) {
        if (isNow || root.queryMatches(row.label)) return []
        const parts = root.rowContextParts(row, false)
        const counted = row.rowType === "artist" && (row.count || 0) > 0 ? 1 : 0
        let segments = []
        let matched = false
        // Text next to text joins one run.
        const add = function(text, match) {
            if (text.length === 0) return
            const last = segments.length > 0 ? segments[segments.length - 1] : null
            if (last && !last.match && !match) {
                last.text += text
            } else {
                segments.push({ text: text, match: match })
            }
            matched = matched || match
        }
        parts.forEach(function(part, index) {
            if (index > 0) add(root.contextSeparator, false)
            if (index < counted) {
                add(part, false)
                return
            }
            root.querySegments(part).forEach(function(segment) { add(String(segment.text), !!segment.match) })
        })
        if (!matched) return []
        const runs = root.layOutRuns(segments, root.maxContextRuns, room, root.smallMetrics, smallSemiboldMetrics)
        for (let i = runs.length - 1; i >= 0; --i) {
            if (runs[i].width > 0) {
                runs[i].width = Math.max(0, room - runs[i].x)
                break
            }
        }
        return runs
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

    // A result's semibold matches are in its title: its context, such as
    // the artist an album was found by, stays whole and regular. Without a
    // match in the title, the match in the context is semibold
    // (contextRuns).
    FontMetrics { id: smallSemiboldMetrics; font.family: root.uiPalette.baseFont.family; font.pointSize: root.uiPalette.smallPointSize; font.weight: Font.DemiBold }

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
    // What Enter, Ctrl+Enter and Alt+Enter do for the selected row, and Esc.
    function hintsFor(kind) {
        return root.keyHintsWith(root.actionHintsFor(kind))
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
            root.controller.pendingAction = ""
            const next = root.controller.nextSelectableIndex(root.controller.selectedDisplayIndex, event.key === Qt.Key_Down ? 1 : -1, true)
            if (next >= 0) root.controller.selectDisplayIndex(next)
        } else if (page) {
            root.controller.pendingAction = ""
            root.controller.moveSelectionByPage(event.key === Qt.Key_PageDown ? 1 : -1)
        } else return false
        event.accepted = true
        return true
    }
    Connections {
        target: root.controller.globalSearchModelApi
        ignoreUnknownSignals: true
        function onSearchRowsChanged() {
            root.resultsQuery = root.queryField.text
            root.rowsRevision += 1
            resultsView.cancelFlick()
            if (!root.controller.selectAfterRowsChanged()) {
                resultsView.positionViewAtBeginning()
            }
        }
    }

    // The results, or the rows and messages without them, in the body.
    Components.SearchPaletteResultsView {
        id: resultsView
        searchPalette: root
        model: root.controller.uiBridge.globalSearchModel || []
        readonly property real resultRowHeight: root.resultHeight
        // The selected row's data, for the key hints.
        readonly property var selectedRow: {
            const index = root.controller.selectedDisplayIndex
            return root.rowsRevision >= 0 && index >= 0 && root.rowCount > index
                ? root.controller.globalSearchModelApi.rowDataAt(index) : null
        }
        currentIndex: root.controller.selectedDisplayIndex
        // The rows a running search is to replace take no keys either.
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
            // The cover as a property of its own, not read through
            // `model`: rows the results took away wait in the reuse
            // pool, culled but not hidden, where `model` reads
            // undefined, and showing the palette again had their
            // thumbnails read it (#772). A pooled row keeps its last
            // value here until it is reused.
            required property string coverUrl
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
            // The context's room: up to the length's column, or the
            // row's end.
            readonly property real contextWidth: Math.max(0, (lengthText.visible
                ? lengthText.x - root.columnGap
                : width - root.sideInset - resultsView.reservedLane) - root.contextX)
            readonly property var contextRuns: isResult ? root.contextRuns(model, contextWidth, isNow) : []
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
                    || resultRow.contextRuns.some(function(run) { return run.cut })
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
                active: root.selectionActive
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
            // library's no-cover tile (Components.SearchPaletteCoverLead).
            // The glyph is loaded only for the rows that show one, with a
            // source that never changes.
            Loader {
                id: artistGlyphLoader
                x: root.leadX + (root.leadSize - root.leadGlyphSize) / 2
                y: Math.round((parent.height - root.leadGlyphSize) / 2)
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
            Components.SearchPaletteCoverLead {
                searchPalette: root
                coverUrl: resultRow.coverUrl
                showsCover: resultRow.isResult && resultRow.rowType !== "artist"
                isNow: resultRow.isNow
            }

            // The title in runs (root.maxTitleRuns of them), the
            // matches semibold; all amber and semibold while it
            // plays.
            Components.SearchPaletteTitleRun {
                searchPalette: root
                run: resultRow.titleRuns.length > 0 ? resultRow.titleRuns[0] : null
                color: resultRow.titleColor
            }
            Components.SearchPaletteTitleRun {
                searchPalette: root
                run: resultRow.titleRuns.length > 1 ? resultRow.titleRuns[1] : null
                color: resultRow.titleColor
            }
            Components.SearchPaletteTitleRun {
                searchPalette: root
                run: resultRow.titleRuns.length > 2 ? resultRow.titleRuns[2] : null
                color: resultRow.titleColor
            }
            Components.SearchPaletteTitleRun {
                searchPalette: root
                run: resultRow.titleRuns.length > 3 ? resultRow.titleRuns[3] : null
                color: resultRow.titleColor
            }
            Components.SearchPaletteTitleRun {
                searchPalette: root
                run: resultRow.titleRuns.length > 4 ? resultRow.titleRuns[4] : null
                color: resultRow.titleColor
            }
            // The context, whole and regular; where the title has
            // no match, in runs, its matches semibold
            // (root.contextRuns). The first run, or the whole.
            Text {
                id: resultContext
                readonly property var run: resultRow.contextRuns.length > 0 ? resultRow.contextRuns[0] : null
                objectName: "globalSearchResultContext"
                visible: resultRow.isResult && (!run || run.width > 0)
                x: root.contextX + (run ? run.x : 0)
                width: run ? run.width : resultRow.contextWidth
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                text: run ? run.text : resultRow.context
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.family: root.uiPalette.baseFont.family
                font.pointSize: root.uiPalette.smallPointSize
                font.weight: run && run.match ? Font.DemiBold : Font.Normal
                color: resultRow.secondaryColor
            }
            ContextRun {
                run: resultRow.contextRuns.length > 1 ? resultRow.contextRuns[1] : null
                x: root.contextX + (run ? run.x : 0)
                height: parent.height
                font.family: root.uiPalette.baseFont.family
                font.pointSize: root.uiPalette.smallPointSize
                color: resultRow.secondaryColor
            }
            ContextRun {
                run: resultRow.contextRuns.length > 2 ? resultRow.contextRuns[2] : null
                x: root.contextX + (run ? run.x : 0)
                height: parent.height
                font.family: root.uiPalette.baseFont.family
                font.pointSize: root.uiPalette.smallPointSize
                color: resultRow.secondaryColor
            }
            ContextRun {
                run: resultRow.contextRuns.length > 3 ? resultRow.contextRuns[3] : null
                x: root.contextX + (run ? run.x : 0)
                height: parent.height
                font.family: root.uiPalette.baseFont.family
                font.pointSize: root.uiPalette.smallPointSize
                color: resultRow.secondaryColor
            }
            ContextRun {
                run: resultRow.contextRuns.length > 4 ? resultRow.contextRuns[4] : null
                x: root.contextX + (run ? run.x : 0)
                height: parent.height
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
    Components.SearchPaletteNoResults {
        id: noResultsMessage
        searchPalette: root
        titleText: I18n.i18nc("@info", "No matches for “%1”", root.queryText)
        hintShown: root.suggestion.length === 0
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
                    active: root.selectionActive
                    focused: false
                    scrollbarLane: 0
                }
                Kirigami.Icon {
                    objectName: "globalSearchAuxGlyph"
                    x: root.leadX + (root.leadSize - width) / 2
                    y: Math.round((parent.height - height) / 2)
                    width: root.leadGlyphSize
                    height: root.leadGlyphSize
                    source: auxRow.modelData.kind === "recent" ? "view-history" : "search"
                    isMask: true
                    roundToIconSize: false
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

    // One tooltip for the result under the pointer, rather than one for
    // each row, where toolTipPlace finds it room: the style's place,
    // above the row, covered the query field. It keeps its place, and
    // what it said, as it fades out: following the rows and the
    // palette's height there, it moved over the field.
    Components.PlainToolTip {
        id: resultToolTip
        objectName: "globalSearchResultToolTip"
        readonly property var place: root.toolTipRow
            ? root.toolTipPlace(root.body.y + root.toolTipRow.y - resultsView.contentY, root.toolTipRow.height,
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
        // In the palette's coordinates, over the body's clip.
        parent: root.contentItem
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
            icon.name: "view-list-tree"
            keyHint: I18n.i18nc("@info:shortcut key name", "Alt+Enter")
            onTriggered: root.controller.navigateSelectionToLibrary()
        }
        Components.AppMenuItem {
            text: resultMenu.rowType === "track" ? I18n.i18nc("@action:inmenu", "Open Containing Folder")
                                                 : I18n.i18nc("@action:inmenu", "Open Folder")
            icon.name: "folder-open"
            onTriggered: root.controller.openRowInFileBrowser(root.contextRowData)
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
    // A run of a result's context after its first.
    component ContextRun: Text {
        property var run: null
        objectName: "globalSearchResultContext"
        visible: !!run && run.width > 0
        width: run ? run.width : 0
        verticalAlignment: Text.AlignVCenter
        text: run ? run.text : ""
        textFormat: Text.PlainText
        elide: Text.ElideRight
        font.weight: run && run.match ? Font.DemiBold : Font.Normal
    }
    // The leads of artists, which have no cover: Breeze's glyph for an
    // artist. (For an album or a track without one, the library's tile,
    // so the app has one way to say "no cover": Breeze's album and track
    // glyphs were no mark at a large font, their larger drawings, a green
    // disc and a solid sheet, masked to a blob at 24 pt.)
    Component {
        id: artistGlyph
        Kirigami.Icon {
            objectName: "globalSearchArtistGlyph"
            width: root.leadGlyphSize
            height: root.leadGlyphSize
            source: "view-media-artist"
            isMask: true
            roundToIconSize: false
            color: root.uiPalette.textSecondary
        }
    }
}
