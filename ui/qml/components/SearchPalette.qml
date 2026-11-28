// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import FerrousUi 1.0
import "../logic/ColorUtils.js" as ColorUtils

// The search palette's frame (#373), shared by global search and Find in
// Queue (#822): an 800 px palette 32 px under the transport, over a dim
// that covers the whole window; wider with a larger font, as its text is
// (UiPalette.scaledSize). The field, then the palette's own rows in the
// body, then the keys for the selected row. A palette puts its results,
// rows and menus in the body (its children go there), and sets what the
// frame shows from them: searching, the rows, keyHints, bodyHeight.
Dialog {
    id: root
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
    // What the names of the frame's parts start with, as "globalSearch" in
    // "globalSearchQueryField".
    property string namePrefix: ""
    property string fieldPlaceholderText: ""
    property string fieldAccessibleName: ""
    // What the help button's popup says.
    property string helpText: ""

    // Set by the palette: whether a search runs for the query, and its
    // rows, from the model rather than the view: the view's count notifies
    // when the view next lays itself out, which the palette's height, bound
    // to it, sets off when it scrolls the view back into bounds (#619).
    property bool searching: false
    property int rowCount: 0
    property bool resultsShown: false
    property bool noResults: false
    readonly property TextField queryField: field
    readonly property string queryText: field.text.trim()
    // The query the rows were found by, which they show their matches of
    // until the rows of a newer one replace them.
    property string resultsQuery: ""
    // What cuts a value where the matches of resultsQuery start and end:
    // the results' model, with highlightSegments(text, query).
    property var highlighter: null
    // Whether the playing track is paused, for the playing glyph of its row.
    property bool playbackPaused: false
    // The selected result's menu.
    property Menu resultMenu: null
    // The body, which holds the palette's children, and its height.
    readonly property Item body: bodyArea
    default property alias bodyData: bodyArea.data
    property real bodyHeight: 0
    // The {key, action} pairs of the footer (keyHintsWith).
    property var keyHints: []
    // The field's text changed, and a key was pressed in it.
    signal queryEdited()
    signal fieldKeyPressed(var event)

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
    readonly property real fieldHeight: Math.max(44, Math.round(root.regularMetrics.height) + 20)
    readonly property real resultHeight: Math.max(32, Math.round(root.regularMetrics.height) + 12)
    // The footer's line of key hints, and the footer: 28 px, or as many
    // lines as the hints wrap to and 4 px above and below them.
    readonly property real footerLineHeight: Math.round(root.smallMetrics.height) + 4
    readonly property real footerHeight: Math.max(28, footerHints.implicitHeight + 8)
    // The line under the no-results title.
    readonly property real hintHeight: Math.max(20, Math.round(root.smallMetrics.height) + 4)
    // Over the no-results message, and under it or the recent searches:
    // 8 px, scaled, as the rows' text is.
    readonly property real messageGap: root.uiPalette.scaledSize(8)
    // The footer's gaps: between a key and its action, half again a word
    // space, so "Ctrl+Enter Add to Queue" reads as a key and an action, not
    // as one phrase; between pairs three times that, so the pairs read
    // apart (they were 4 px, a word space, and 22 px). 6 and 18 px at
    // 10 pt; they grow with the font.
    readonly property real hintKeyGap: Math.max(Kirigami.Units.mediumSpacing,
                                                Math.round(1.5 * root.smallMetrics.advanceWidth(" ")))
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
    readonly property real columnGap: 12
    // The lead column, where the rows' covers and glyphs and the field's
    // search glyph are; the title 12 px after it.
    property real leadX: root.sideInset
    readonly property real titleX: root.leadX + root.leadSize + root.columnGap
    // A track's length at the right: 44 px, or as wide as "00:00" in its
    // font, and 12 px clear of the context. Of the room left beside it the
    // title takes up to 380 px and the context the rest: in the 800 px
    // palette, 276 px. The title grows past 300 px only into room the
    // context can spare over those 276, and takes at most 60 % in a
    // narrower palette. All follow the font, as the palette's width does,
    // and the three never meet.
    readonly property real lengthWidth: Math.max(44, Math.ceil(root.lengthMetrics.advanceWidth))
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
    // field, five results with what else the palette shows over them
    // (leastBodyHeight) and its footer for a result (leastFooterHeight)
    // then fit above the window's 32 px bottom margin, but at least 8 px
    // under the menu bar.
    property real paletteTop: 32
    readonly property int leastResultRows: 5
    property real leastBodyHeight: root.leastResultRows * root.resultHeight
    property real leastFooterHeight: root.footerHeight
    function updatePlacement() {
        const windowItem = root.parent
        if (!windowItem) {
            root.paletteTop = 32
            return
        }
        const underTransport = root.anchorItem
            ? Math.round(root.anchorItem.mapToItem(windowItem, 0, root.anchorItem.height).y) + 1 + 32
            : 32
        const leastHeight = root.fieldHeight + 1 + root.leastBodyHeight + 1 + root.leastFooterHeight
        const menuBar = root.windowRoot && root.windowRoot.menuBar && root.windowRoot.menuBar.visible
            ? root.windowRoot.menuBar : null
        const highest = (menuBar ? Math.ceil(menuBar.mapToItem(windowItem, 0, menuBar.height).y) : 0) + 8
        root.paletteTop = Math.max(highest, Math.min(underTransport, Math.floor(windowItem.height - 32 - leastHeight)))
    }
    // The selection is the inactive one while a result's menu is open,
    // which has the keys then, as the library's and the queue's is while
    // theirs is, until the menu has closed.
    readonly property bool selectionActive: !root.resultMenu || !root.resultMenu.visible

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
    // The room the body has for whole rows.
    readonly property real bodyRoom: root.maximumHeight - root.fieldHeight - 2 - root.footerHeight
    x: Math.round(((root.parent ? root.parent.width : root.width) - root.width) / 2)
    y: Math.round(root.paletteTop)
    header: null
    footer: null
    onAboutToShow: root.updatePlacement()

    background: Kirigami.ShadowedRectangle {
        objectName: root.namePrefix + "Background"
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
            objectName: root.namePrefix + "Dim"
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

    function focusQueryField(selectAll) {
        field.forceActiveFocus()
        if (selectAll) field.selectAll()
        else field.cursorPosition = field.text.length
    }

    // `value` cut where the matches of the results' query start and end.
    function querySegments(value) {
        const text = String(value || "")
        return root.highlighter && root.highlighter.highlightSegments
            ? root.highlighter.highlightSegments(text, root.resultsQuery)
            : [{ text: text, match: false }]
    }
    // Whether `value` has a match of the results' query, from its segments.
    function queryMatches(value) {
        return !!value && root.querySegments(value).some(function(segment) { return segment.match })
    }
    readonly property string contextSeparator: " · "
    // The title of a result in up to five runs, the matches semibold: the
    // text of each, where it starts, how wide it may be and whether that
    // cuts it. A run that does not fit is elided and the runs after it are
    // left out.
    readonly property int maxTitleRuns: 5
    function titleRuns(label, allEmphasized) {
        const text = String(label || "")
        const segments = allEmphasized ? [{ text: text, match: true }] : root.querySegments(text)
        return root.layOutRuns(segments, root.maxTitleRuns, root.titleWidth, root.regularMetrics, root.semiboldMetrics)
    }
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
    readonly property FontMetrics regularMetrics: FontMetrics {
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.basePointSize
    }
    readonly property FontMetrics semiboldMetrics: FontMetrics {
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.basePointSize
        font.weight: Font.DemiBold
    }
    readonly property FontMetrics smallMetrics: FontMetrics {
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.smallPointSize
    }
    readonly property TextMetrics lengthMetrics: TextMetrics {
        text: "00:00"
        font.family: root.uiPalette.baseFont.family
        font.pointSize: root.uiPalette.smallPointSize
        font.features: root.uiPalette.tabularFigures
    }

    // The footer's pairs: `pairs`, what Enter, Ctrl+Enter and Alt+Enter do
    // for the selected row, with Esc last. Esc is left out where the pairs
    // would wrap with it and fit one line without it: the line it took at
    // a large font was a result's room, and Esc closes every dialog.
    function keyHintsWith(pairs) {
        const close = { key: I18n.i18nc("@info:shortcut key name", "Esc"), action: I18n.i18nc("@info:shortcut action", "Close") }
        const all = pairs.concat([close])
        return pairs.length > 0 && root.hintLines(all) > 1 && root.hintLines(pairs) === 1 ? pairs : all
    }
    // The width of a pair in the footer's line, with the gap after it
    // unless it is the last, as its Row is laid out.
    function hintPairWidth(hint, last) {
        return Math.ceil(root.smallMetrics.advanceWidth(hint.key)) + root.hintKeyGap
            + Math.ceil(root.smallMetrics.advanceWidth(hint.action)) + (last ? 0 : root.hintPairGap)
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

    contentItem: Item {
        id: contentColumn
        implicitWidth: root.designWidth
        implicitHeight: root.fieldHeight + 1 + bodyArea.implicitHeight + 1 + root.footerHeight

        // The field: a search glyph, the query, and the clear and help
        // buttons.
        Item {
            id: fieldRow
            width: parent.width
            height: root.fieldHeight

            // Centred on the results' lead column, on one axis with the
            // rows' glyphs and the covers.
            Kirigami.Icon {
                objectName: root.namePrefix + "FieldIcon"
                x: root.leadX + (root.leadSize - width) / 2
                y: Math.round((parent.height - height) / 2)
                width: root.glyphSize
                height: root.glyphSize
                source: "search"
                isMask: true
                roundToIconSize: false
                color: root.uiPalette.textSecondary
            }
            TextField {
                id: field
                objectName: root.namePrefix + "QueryField"
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
                placeholderText: root.fieldPlaceholderText
                Accessible.name: root.fieldAccessibleName
                onTextChanged: root.queryEdited()
                Keys.onPressed: function(event) { root.fieldKeyPressed(event) }
            }
            ToolButton {
                id: clearButton
                objectName: root.namePrefix + "ClearButton"
                x: helpButton.x - width
                y: Math.round((parent.height - height) / 2)
                width: root.fieldButtonSize
                height: root.fieldButtonSize
                visible: field.text.length > 0
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
                    field.clear()
                    root.focusQueryField(false)
                }
            }
            ToolButton {
                id: helpButton
                objectName: root.namePrefix + "HelpButton"
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
            objectName: root.namePrefix + "FieldSeam"
            y: root.fieldHeight
            width: parent.width
            height: 1
            color: root.uiPalette.seam
        }

        // The palette's results, or its rows and messages without them.
        Item {
            id: bodyArea
            objectName: root.namePrefix + "Body"
            y: root.fieldHeight + 1
            width: parent.width
            height: Math.max(0, contentColumn.height - y - 1 - root.footerHeight)
            clip: true
            implicitHeight: root.bodyHeight
        }

        // The keys for the selected row.
        // Not when the footer meets the field's seam.
        Rectangle {
            objectName: root.namePrefix + "FooterHairline"
            visible: bodyArea.height > 0
            y: footerBar.y - 1
            width: parent.width
            height: 1
            color: root.uiPalette.hairline
        }
        Item {
            id: footerBar
            objectName: root.namePrefix + "Footer"
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
                        objectName: root.namePrefix + "KeyHint"
                        readonly property string keyText: modelData.key
                        readonly property string actionText: modelData.action
                        height: root.footerLineHeight
                        spacing: root.hintKeyGap
                        Text {
                            objectName: root.namePrefix + "KeyHintKey"
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            textFormat: Text.PlainText
                            text: modelData.key
                            font.family: root.uiPalette.baseFont.family
                            font.pointSize: root.uiPalette.smallPointSize
                            color: root.uiPalette.text
                        }
                        Text {
                            objectName: root.namePrefix + "KeyHintAction"
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

        Popup {
            id: searchHelp
            objectName: root.namePrefix + "Help"
            focus: true
            width: Math.min(root.uiPalette.scaledSize(480), root.width - 2 * root.sideInset)
            x: root.width - width - 8
            y: root.fieldHeight
            padding: 12
            contentItem: Label {
                textFormat: Text.PlainText
                text: root.helpText
                wrapMode: Text.Wrap
            }
        }
    }
}
