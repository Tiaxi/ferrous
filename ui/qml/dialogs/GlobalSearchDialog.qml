// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../components" as Components

// The search palette (#373): an 800 px palette 32 px under the transport,
// over a dim that covers the whole window. The field, then the results in
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
    readonly property string suggestion: root.controller.uiBridge.globalSearchSuggestion || ""
    readonly property bool resultsShown: root.queryText.length > 0 && !root.retained && resultsView.count > 0
    readonly property bool noResults: root.queryText.length > 0 && !root.searching && !root.retained
        && resultsView.count === 0
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

    // The amber of the playing glyph over a cover's scrim.
    readonly property color nowOnScrim: "#f2b035"
    // The palette's sizes at 1×: these with the default font, and taller
    // with a larger one, by its line.
    readonly property real fieldHeight: Math.max(44, Math.round(regularMetrics.height) + 20)
    readonly property real headingHeight: Math.max(28, Math.round(smallMetrics.height) + 8)
    readonly property real resultHeight: Math.max(32, Math.round(regularMetrics.height) + 12)
    // The footer's line of key hints, and the footer: 28 px, or as many
    // lines as the hints wrap to and 4 px above and below them.
    readonly property real footerLineHeight: Math.round(smallMetrics.height) + 4
    readonly property real footerHeight: Math.max(28, footerHints.implicitHeight + 8)
    // The line under the no-results title.
    readonly property real hintHeight: Math.max(20, Math.round(smallMetrics.height) + 4)
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
    // font, and 12 px clear of the context. The title takes up to 300 px
    // of what is left beside it, and the context the rest, so the three
    // never meet, whatever the font and the width of the palette.
    readonly property real lengthWidth: Math.max(44, Math.ceil(lengthMetrics.advanceWidth))
    readonly property real columnGap: 12
    readonly property real titleWidth: {
        const lengthLeft = root.width - root.sideInset - Kirigami.Units.largeSpacing - root.lengthWidth - root.columnGap
        const room = lengthLeft - root.columnGap - root.titleX
        return Math.max(0, Math.min(300, Math.round(room * 0.6)))
    }
    readonly property real contextX: root.titleX + root.titleWidth + root.columnGap
    // Where the palette's top is: set when it opens, from where the
    // transport is then.
    property real paletteTop: 32
    function updatePlacement() {
        root.paletteTop = root.anchorItem && root.parent
            ? Math.round(root.anchorItem.mapToItem(root.parent, 0, root.anchorItem.height).y) + 1 + 32
            : 32
    }
    // Counts the result rows' changes, for what depends on the selected
    // row's data rather than on its index.
    property int rowsRevision: 0

    modal: true
    dim: true
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
    width: Math.min(800, (root.parent ? root.parent.width : 800) - 2 * root.sideInset)
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
    }

    // The dim over the whole window, the pane too, fading with the palette.
    Overlay.modal: Rectangle {
        objectName: "globalSearchDim"
        color: root.uiPalette.dim
        Behavior on opacity {
            enabled: root.fadeDuration > 0
            NumberAnimation { duration: root.fadeDuration; easing.type: Easing.OutCubic }
        }
    }
    enter: Transition {
        NumberAnimation {
            property: "opacity"; from: 0; to: 1
            duration: root.fadeDuration; easing.type: Easing.OutCubic
        }
    }
    exit: Transition {
        NumberAnimation {
            property: "opacity"; from: 1; to: 0
            duration: root.fadeDuration; easing.type: Easing.OutCubic
        }
    }

    Component.onCompleted: root.controller.registerRefs(root, queryField, resultsView)
    onOpened: root.controller.handleDialogOpened(queryField.text || "")
    onClosed: root.controller.endOpen(true)

    function escaped(value) {
        return String(value || "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }
    function highlighted(value) {
        return root.controller.globalSearchModelApi.highlightText
            ? root.controller.globalSearchModelApi.highlightText(String(value || ""), queryField.text)
            : root.escaped(value)
    }
    function queryMatches(value) {
        return !!value && root.highlighted(value) !== root.escaped(value)
    }
    // A result's details after its title: who and where it is from, and
    // what else matched.
    function rowContext(row, full) {
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
        if (row.rootLabel && (full || row.showRoot || root.queryMatches(row.rootLabel))) parts.push(row.rootLabel)
        if (row.matchDetail) parts.push(row.matchDetail)
        return parts.filter(function(value) { return value.length > 0 }).join(" · ")
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
    // text of each, where it starts and how wide it may be. A run that does
    // not fit is elided and the runs after it are left out.
    readonly property int maxTitleRuns: 5
    function titleRuns(label, allEmphasized) {
        const text = String(label || "")
        let segments = allEmphasized || !root.controller.globalSearchModelApi.highlightSegments
            ? [{ text: text, match: allEmphasized }]
            : root.controller.globalSearchModelApi.highlightSegments(text, queryField.text)
        if (segments.length > root.maxTitleRuns) {
            // Matches past the fourth run stay regular.
            const tail = segments.slice(root.maxTitleRuns - 1).map(function(segment) { return segment.text }).join("")
            segments = segments.slice(0, root.maxTitleRuns - 1).concat([{ text: tail, match: false }])
        }
        let runs = []
        let x = 0
        for (let i = 0; i < segments.length; ++i) {
            const metrics = segments[i].match ? semiboldMetrics : regularMetrics
            const advance = metrics.advanceWidth(segments[i].text)
            const room = Math.max(0, root.titleWidth - x)
            runs.push({ text: segments[i].text, match: !!segments[i].match, x: Math.round(x), width: Math.min(advance, room) })
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
    // {key, action} pairs, with Esc last.
    readonly property var keyHints: {
        const kind = root.selectionKind
        const close = { key: I18n.i18nc("@info:shortcut key name", "Esc"), action: I18n.i18nc("@info:shortcut action", "Close") }
        const enter = I18n.i18nc("@info:shortcut key name", "Enter")
        if (kind === "artist" || kind === "album" || kind === "track") {
            return [
                { key: enter, action: kind === "artist" ? I18n.i18nc("@info:shortcut action", "Play Artist")
                    : (kind === "album" ? I18n.i18nc("@info:shortcut action", "Play Album")
                                        : I18n.i18nc("@info:shortcut action", "Play Track")) },
                { key: I18n.i18nc("@info:shortcut key name", "Ctrl+Enter"), action: I18n.i18nc("@info:shortcut action", "Add to Queue") },
                { key: I18n.i18nc("@info:shortcut key name", "Alt+Enter"), action: I18n.i18nc("@info:shortcut action", "Show in Library") },
                close
            ]
        }
        if (kind === "more") return [{ key: enter, action: I18n.i18nc("@info:shortcut action", "Show All") }, close]
        if (kind === "recent") return [{ key: enter, action: I18n.i18nc("@info:shortcut action", "Search") }, close]
        if (kind === "suggestion") return [{ key: enter, action: I18n.i18nc("@info:shortcut action", "Search Instead") }, close]
        return [close]
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
            root.rowsRevision += 1
            resultsView.cancelFlick()
            if (!root.controller.selectAfterRowsChanged()) {
                resultsView.positionViewAtBeginning()
            }
        }
    }

    contentItem: Item {
        id: contentColumn
        implicitWidth: 800
        implicitHeight: root.fieldHeight + 1 + body.implicitHeight + 1 + root.footerHeight

        // The field: a search glyph, the query, and the clear and help
        // buttons.
        Item {
            id: fieldRow
            width: parent.width
            height: root.fieldHeight

            Kirigami.Icon {
                objectName: "globalSearchFieldIcon"
                x: root.sideInset
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
                icon.color: root.uiPalette.textSecondary
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
            implicitHeight: root.resultsShown
                ? resultsView.contentHeight + 6
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
                    return root.rowsRevision >= 0 && index >= 0 && count > index
                        ? root.controller.globalSearchModelApi.rowDataAt(index) : null
                }
                currentIndex: root.controller.selectedDisplayIndex
                highlightFollowsCurrentItem: false
                keyNavigationEnabled: false
                boundsBehavior: Flickable.StopAtBounds
                flickDeceleration: root.snappyScrollFlickDeceleration
                maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                opacity: root.searching ? 0.5 : 1
                enabled: !root.searching
                ScrollBar.vertical: ScrollBar {
                    id: resultScrollBar
                    policy: ScrollBar.AsNeeded
                }
                // The lane the rows leave for the scrollbar, when it shows.
                readonly property real scrollbarLane: resultScrollBar.size < 1 ? Kirigami.Units.largeSpacing : 0
                Keys.onPressed: function(event) {
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
                    readonly property string context: isResult ? root.rowContext(model, false) : ""
                    // Asked for only when the shared tooltip shows.
                    function toolTipText() {
                        return model.label + "\n" + root.rowContext(model, true)
                    }
                    width: ListView.view.width
                    height: kind === "item" ? root.resultHeight : (kind === "" ? 0 : root.headingHeight)
                    Accessible.role: isResult ? Accessible.ListItem : (isMore ? Accessible.Link : Accessible.StaticText)
                    Accessible.name: isResult
                        ? I18n.i18nc("@info accessible name of a search result: type, name, details",
                                     "%1: %2, %3", root.typeLabel(rowType), model.label || "", root.rowContext(model, true))
                        : (kind === "section" ? (root.sectionTitles[rowType] || "")
                                              : root.moreText(kind, rowType, model.shownCount || 0, model.count || 0))
                    Accessible.selectable: selectable
                    Accessible.selected: selected
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
                        bottomPadding: 5
                        verticalAlignment: Text.AlignBottom
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
                        width: parent.width - x - root.sideInset - resultsView.scrollbarLane
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
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

                    // The lead: an artist's glyph, or the cover, with the
                    // amber glyph over it while its track plays. The glyphs
                    // are loaded only for the rows that show one, each with
                    // a source that never changes: a Kirigami icon looks its
                    // file up again on the GUI thread whenever its source
                    // changes, which recycled rows would do on every query.
                    Loader {
                        x: root.sideInset + (root.leadSize - root.glyphSize) / 2
                        y: Math.round((parent.height - root.glyphSize) / 2)
                        active: resultRow.isResult && resultRow.rowType === "artist"
                        sourceComponent: artistGlyph
                    }
                    Loader {
                        x: root.sideInset + (root.leadSize - root.glyphSize) / 2
                        y: Math.round((parent.height - root.glyphSize) / 2)
                        active: resultRow.isResult && resultRow.rowType !== "artist"
                            && (model.coverUrl || "").length === 0 && !resultRow.isNow
                        sourceComponent: resultRow.rowType === "album" ? albumPlaceholder : trackPlaceholder
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
                        visible: resultRow.isNow && thumbnail.visible
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
                            // Over the cover's dark scrim, the colour map's
                            // bright amber in both themes (fixed, like the
                            // plot's colours): the light theme's darker
                            // amber is under 3:1 there.
                            color: thumbnail.visible ? root.nowOnScrim : root.uiPalette.now
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
                        color: resultRow.isNow ? root.uiPalette.now : root.uiPalette.text
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
                        color: resultRow.isNow ? root.uiPalette.now : root.uiPalette.text
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
                        color: resultRow.isNow ? root.uiPalette.now : root.uiPalette.text
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
                        color: resultRow.isNow ? root.uiPalette.now : root.uiPalette.text
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
                        color: resultRow.isNow ? root.uiPalette.now : root.uiPalette.text
                    }
                    Text {
                        objectName: "globalSearchResultContext"
                        visible: resultRow.isResult
                        x: root.contextX
                        width: Math.max(0, (lengthText.visible ? lengthText.x - root.columnGap : parent.width - root.sideInset - resultsView.scrollbarLane) - x)
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        text: resultRow.context
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.smallPointSize
                        color: root.uiPalette.textSecondary
                    }
                    Text {
                        id: lengthText
                        objectName: "globalSearchResultLength"
                        visible: resultRow.isResult && resultRow.rowType === "track"
                        x: parent.width - root.sideInset - resultsView.scrollbarLane - width
                        // Wider for a length of an hour or more.
                        width: Math.max(root.lengthWidth, Math.ceil(implicitWidth))
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignRight
                        text: visible ? (model.lengthText || "") : ""
                        font.family: root.uiPalette.baseFont.family
                        font.pointSize: root.uiPalette.smallPointSize
                        font.features: root.uiPalette.tabularFigures
                        color: root.uiPalette.textSecondary
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        enabled: resultRow.selectable
                        hoverEnabled: true
                        cursorShape: resultRow.isMore ? Qt.PointingHandCursor : Qt.ArrowCursor
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onContainsMouseChanged: {
                            if (containsMouse && resultRow.isResult) root.hoveredResultRow = resultRow
                            else if (root.hoveredResultRow === resultRow) root.hoveredResultRow = null
                        }
                        onClicked: function(mouse) {
                            root.controller.selectDisplayIndex(resultRow.index)
                            if (resultRow.isMore && mouse.button === Qt.LeftButton) {
                                root.controller.activateSelection()
                            } else if (mouse.button === Qt.RightButton && resultRow.isResult) {
                                root.contextRowData = root.controller.globalSearchModelApi.rowDataAt(resultRow.index)
                                // Where the click was, from the event rather
                                // than the cursor's last known place.
                                resultMenu.popup(rowMouse, mouse.x, mouse.y)
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
                    bottomPadding: 5
                    verticalAlignment: Text.AlignBottom
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
                        spacing: 4
                        Text {
                            objectName: "globalSearchKeyHintKey"
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            text: modelData.key
                            font.family: root.uiPalette.baseFont.family
                            font.pointSize: root.uiPalette.smallPointSize
                            color: root.uiPalette.text
                        }
                        Text {
                            objectName: "globalSearchKeyHintAction"
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            text: modelData.action
                            font.family: root.uiPalette.baseFont.family
                            font.pointSize: root.uiPalette.smallPointSize
                            color: root.uiPalette.textSecondary
                        }
                        // 18 px to the next pair; a Flow's spacing would
                        // put it between the lines too.
                        Item {
                            visible: index < root.keyHints.length - 1
                            width: 18 - parent.spacing
                            height: 1
                        }
                    }
                }
            }
        }

        // One tooltip for the result under the pointer, rather than one for
        // each row.
        ToolTip {
            id: resultToolTip
            objectName: "globalSearchResultToolTip"
            parent: root.hoveredResultRow || resultsView
            visible: !!root.hoveredResultRow && root.hoveredResultRow.hovered
            delay: Kirigami.Units.toolTipDelay
            text: root.hoveredResultRow ? root.hoveredResultRow.toolTipText() : ""
        }

        Popup {
            id: searchHelp
            objectName: "globalSearchHelp"
            focus: true
            width: Math.min(480, root.width - 2 * root.sideInset)
            x: root.width - width - 8
            y: root.fieldHeight
            padding: 12
            contentItem: Label {
                // The filter names are search syntax, the same in every language.
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
        Menu {
            id: resultMenu
            MenuItem { text: I18n.i18nc("@action:inmenu", "Play"); icon.name: "media-playback-start"; onTriggered: root.controller.activateRow(root.contextRowData) }
            MenuItem { text: I18n.i18nc("@action:inmenu", "Add to Queue"); icon.name: "list-add"; onTriggered: root.controller.queueRow(root.contextRowData) }
            MenuItem { text: I18n.i18nc("@action:inmenu", "Show in Library"); icon.name: "go-jump"; onTriggered: root.controller.navigateSelectionToLibrary() }
            MenuSeparator {}
            MenuItem { text: I18n.i18nc("@action:inmenu", "Open Containing Folder"); icon.name: "document-open-folder"; onTriggered: root.controller.openRowInFileBrowser(root.contextRowData) }
        }
    }
    // The result row under the pointer, whose tooltip shows.
    property Item hoveredResultRow: null
    // The lead glyphs of the rows without a cover.
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
        id: albumPlaceholder
        Kirigami.Icon {
            objectName: "globalSearchCoverPlaceholder"
            width: root.glyphSize
            height: root.glyphSize
            source: "media-optical-audio"
            isMask: true
            color: root.uiPalette.textSecondary
        }
    }
    Component {
        id: trackPlaceholder
        Kirigami.Icon {
            objectName: "globalSearchCoverPlaceholder"
            width: root.glyphSize
            height: root.glyphSize
            source: "audio-x-generic"
            isMask: true
            color: root.uiPalette.textSecondary
        }
    }
}
