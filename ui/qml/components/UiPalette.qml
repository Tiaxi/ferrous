// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import org.kde.kirigami 2.20 as Kirigami
import "../logic/ColorUtils.js" as ColorUtils

QtObject {
    id: root

    required property var windowRoot

    readonly property bool themeIsDark: ColorUtils.colorLuma(root.windowRoot.palette.window) < 0.45

    // The colour tokens of the #373 design. Every one comes from the
    // window's palette and the Kirigami theme, except the amber of `now`
    // and the spectrogram's plot set, which are not theme colours. The mix
    // amounts are per theme brightness; the comments give what Breeze Dark
    // and Breeze Light produce.

    // Window background: the menu bar, transport, status bar and dialogs'
    // button rows. #202326 / #eff0f1.
    readonly property color chrome: root.windowRoot.palette.window
    // View background: all content. #141618 / #ffffff.
    readonly property color view: root.windowRoot.palette.base
    // View text. #fcfcfc / #232629.
    readonly property color text: Kirigami.Theme.textColor
    // The theme's inactive text, which the lines and the quieter text
    // colours are mixed from. #a1a9b1 / #707d8a.
    readonly property color _inactiveText: Kirigami.Theme.disabledTextColor
    readonly property color accent: Kirigami.Theme.highlightColor
    // Between inactive text and text. #aeb7bd (7.75:1 on chrome) / #4f5a63
    // (7.1:1 on view).
    readonly property color textSecondary: ColorUtils.mixColor(
        root._inactiveText, root.text, root.themeIsDark ? 0.15 : 0.41)
    // Counts, years and track numbers. #91989f / #59626d: lightened or
    // darkened from the mix to be 4.6:1 on the view's hover tint, a margin over
    // 4.5:1 that rendering cannot round away (6.2:1 on view in both).
    // On a row, use textTertiaryOn(selected, hovered): on a selected row
    // it is 3.7:1 on the focused tint, and short of rowFigureContrast on a
    // hovered one.
    readonly property color textTertiary: root.readableOn(root.themeIsDark
        ? ColorUtils.mixColor(root._inactiveText, root.view, 0.134)
        : ColorUtils.mixColor(root._inactiveText, root.text, 0.185),
        [root.view, ColorUtils.drawnOver(root.hover, root.view)], 4.6)
    // The contrast of a row's tertiary figures on a hovered or selected
    // row's tint. Small figures drawn with subpixel antialiasing reach a
    // strongest pixel lighter than their colour, by up to 14% in contrast
    // (in Breeze Light a selected row's 5.08:1 "1" drew at 4.39:1, a
    // hovered row's 4.64:1 "3 tracks" at 4.48:1): at 5.4:1 they
    // draw at 4.5:1 or more. They stay well under the text's contrast.
    readonly property real rowFigureContrast: 5.4
    // Tertiary text on a hovered list row: lightened or darkened to be
    // rowFigureContrast on it, only there, so the rest keeps its step below
    // the secondary colour.
    readonly property color textTertiaryHovered: root.readableOn(root.textTertiary, [root.hoveredRow],
        root.rowFigureContrast)
    // Tertiary text on a selected row, the pointer over it or not, and the
    // list focused or not: the secondary colour, darkened or lightened to
    // be rowFigureContrast on each selection tint.
    readonly property color textTertiarySelected: root.readableOn(root.textSecondary,
        [root.selection, root.selectionInactive, root.selectionHovered, root.selectionInactiveHovered],
        root.rowFigureContrast)
    // A field's placeholder, such as Edit Tags' "Mixed", which
    // tells what saving does: the theme's own placeholder colour, its
    // inactive text, darkened or lightened where it is under 4.6:1 on the
    // view, a margin over 4.5:1. #a1a9b1 / #697582 (Breeze Light's own is
    // 4.2:1 on white).
    readonly property color placeholderText: root.readableOn(root._inactiveText, [root.view], 4.6)
    // The artist and album cells of the queue: darkened or lightened where
    // they fall short of 4.5:1 on a selected row under the pointer, as on
    // light. #c2c2c3 / #555759 (#5a5c5e as mixed).
    readonly property color textColumn: root.readableOn(ColorUtils.mixColor(root.text, root.view, 0.25),
        [root.view, root.selection, root.selectionInactive, root.selectionHovered, root.selectionInactiveHovered], 4.5)
    // The 1 px line where two regions meet. #383b40 / #d6d9dc.
    readonly property color seam: ColorUtils.mixColor(
        root.chrome, root._inactiveText, root.themeIsDark ? 0.18 : 0.20)
    // The 1 px line under a header row. #26292c / #ebedef.
    readonly property color hairline: ColorUtils.mixColor(
        root.view, root._inactiveText, root.themeIsDark ? 0.128 : 0.137)
    // The 1 px line between chrome and content. #383b40 / #c9cdd1.
    readonly property color chromeLine: ColorUtils.mixColor(
        root.chrome, root._inactiveText, root.themeIsDark ? 0.18 : 0.30)
    // Text over the row: the hover tint. The least of it, in whole
    // percent, that is 1.3:1 on the view, so a hovered row stands out in
    // any theme: 10% (#2b2d2f) / 14% (#e0e1e1). It stays nearer to the view
    // than the selection, which is further from it in hue as well.
    readonly property color hover: root._tintOf(root.text, root.view, 1.3)
    // A pressed button: text over it, a clear step past the hover tint, so
    // a press reads apart from a hover (at 10% it read as the hover).
    // The least, in whole percent, that is 1.65:1 on the view: 17% / 25%.
    readonly property color pressed: root._tintOf(root.text, root.view, 1.65)
    // The hover tint of the main window's lists, the library's and the
    // queue's: text over the row, the least of it in whole percent that is
    // 1.44:1 on a dark view, a step stronger than a button's, so the row
    // under the pointer reads across a wide list (at 1.3:1 it was faint),
    // and 1.31:1 on a light one, where 1.44:1 outweighed the pale selection
    // and the row under the pointer looked the more selected: 13%
    // (#323436) / 14% (#e0e1e1). It stays under the selection in contrast
    // and apart from it in hue: it is neutral, the selection blue.
    readonly property color rowHover: root._tintOf(root.text, root.view, root.themeIsDark ? 1.44 : 1.31)
    // A hovered row of the library or the queue: rowHover on the view.
    readonly property color hoveredRow: ColorUtils.drawnOver(root.rowHover, root.view)
    // The view mixed with the accent: 1.8:1 / 1.4:1 on the view, a clear
    // step past a hovered row in weight as well as in hue (at 22 %,
    // #d4edfa, the light one was 1.2:1, under the hover), and on dark past
    // the selection of a list without focus too (at 26 %, #1f3e4e, the two
    // were 1.04:1 apart, and nothing said which list takes Enter or
    // Delete after a click). #21475b / #b5e0f7.
    readonly property color selection: ColorUtils.mixColor(
        root.view, root.accent, root.themeIsDark ? 0.32 : 0.38)
    // The selection of a list without keyboard focus: quieter than the
    // selection, yet further from the view than a hovered row: 1.52:1 /
    // 1.34:1 on the view, between rowHover's 1.45:1 / 1.31:1 and the
    // selection's 1.8:1 / 1.4:1 (at 20 % and 27 %, #1c3442 and #cbe9f9,
    // it was 1.40:1 / 1.27:1, under the hover). It differs
    // from the selection by its colour as well as by its weight: the view
    // mixed with 24 % / 33 % of the accent, then at 60 % of that mix's
    // saturation and the same luminance, so it keeps its step over the
    // hover and reads as a selection at rest rather than a second active
    // one (at full saturation, #1e3a4a / #bfe4f8, it was some 1.5 / 2.8 ΔE
    // from the selection). The playing amber is 6.2:1 /
    // 5.4:1 on it. #283a43 / #cde2ed.
    readonly property color selectionInactive: root._desaturated(
        ColorUtils.mixColor(root.view, root.accent, root.themeIsDark ? 0.24 : 0.33), 0.6)
    // A selected queue row under the pointer: the text colour over the
    // selection, active or inactive, so a selected row answers the pointer
    // as the others do. See _selectionHoverShare: 4 % / 6 % in Breeze,
    // #2a4e61 / #acd5eb. The secondary and column text, the amber and the
    // warning mark are made readable on it.
    readonly property color selectionHovered: ColorUtils.mixColor(
        root.selection, root.text, root._selectionHoverShare)
    readonly property color selectionInactiveHovered: ColorUtils.mixColor(
        root.selectionInactive, root.text, root._selectionHoverShare)
    readonly property var _selectedRows: [root.selection, root.selectionInactive,
                                          root.selectionHovered, root.selectionInactiveHovered]
    // The secondary text on a selected row: lightened or darkened where it
    // is under 4.5:1 on one, as on dark under the pointer.
    // #b3bcc1 / #4f5a63.
    readonly property color textSecondarySelected: root.readableOn(root.textSecondary, root._selectedRows, 4.5)
    // An unreadable file's mark on a selected row: the negative colour, 3:1
    // on each. #ef6c7b / #c4293b.
    readonly property color negativeSelected: root.readableOn(root.negative, root._selectedRows, 3)
    // The 1 px ring on the focused row: the accent, lighter on dark and
    // darker on light, to stand out from the selection tint. #6cc5f2 / #1a74a8.
    readonly property color focusRing: root.themeIsDark
        ? root._withHslLightness(root.accent, 1.03, 0.686)
        : root._withHslLightness(root.accent, 0.89, 0.38)
    // #3daee9 / #1b73a1: the accent, darkened on light to read as text, and
    // at least 4.5:1 on both the window colour (dialogs, Recent Issues) and
    // the view: a theme whose accent falls short on either has the link's
    // lightness moved away from them until it reaches that.
    readonly property color link: root.readableOn(root.themeIsDark ? root.accent : root.focusRing,
                                                  [root.chrome, root.view], 4.5)
    // #ed5a6a / #c4293b: the theme's negative text, lightened on dark and
    // darkened on light for text contrast.
    readonly property color negative: root.themeIsDark
        ? root._withHslLightness(Kirigami.Theme.negativeTextColor, 1.18, 0.639)
        : root._withHslLightness(Kirigami.Theme.negativeTextColor, 0.96, 0.465)
    // The seek bar's waveform. #b9e1f5 / #2c566c.
    readonly property color wave: ColorUtils.mixColor(root.text, root.accent, 0.35)
    // The visualization pane's waveform: in a light theme the seek bar's;
    // in a dark one quieter, the accent's hue at 60 % of its saturation and
    // 68 % lightness, #87bad4, some 8.6:1 on the view (the seek bar's
    // #b9e1f5, 13:1 over some 29 % of the pane, was the brightest mass of
    // the dark window). One tone: no played part apart.
    readonly property color paneWave: root.themeIsDark ? root._withHslLightness(root.accent, 0.6, 0.68) : root.wave
    // Fixed: the spectrogram colour map's amber. It marks only what is
    // playing: the playing row's glyph and title, the playing tree row, the
    // name of a collapsed tree row that holds it, and the seek playhead.
    // The light one is darkened to be 4.5:1 on a hovered and a selected
    // row as well, and on a selected queue row under the pointer.
    // #f2b035 / #814900.
    readonly property color now: root.themeIsDark
        ? "#f2b035"
        : root.readableOn(root._nowLight, [root.view, root.hoveredRow, root.selection,
                                           root.selectionHovered, root.selectionInactiveHovered], 4.5)
    readonly property color _nowLight: "#955500"
    // The opacity of the amber name of a collapsed tree row that holds the
    // playing track while it is paused: 70%, or more where that is under
    // 4.5:1 on a row's background, plain, hovered or selected, so the name
    // stays as readable as the queue's amber title.
    readonly property real nowPausedOpacity: {
        const backgrounds = [root.view, root.hoveredRow, root.selection, root.selectionInactive]
        let opacity = 0.7
        for (let i = 0; i < backgrounds.length; ++i) {
            opacity = Math.max(opacity, ColorUtils.opacityForContrast(root.now, backgrounds[i], 0.7, 4.5))
        }
        return opacity
    }
    // The scrim behind the search palette: black, 65% on dark, where less
    // left the window behind it nearly as bright as the palette,
    // and 25% on light, where black reads much sooner.
    readonly property color dim: Qt.rgba(0, 0, 0, root.themeIsDark ? 0.65 : 0.25)

    // The hover and pressed tints on another opaque `background` than the
    // view, such as the transport's Header chrome: as strong on it as
    // `hover` and `pressed` are on the view.
    function hoverOn(background) {
        return root._tintOf(root.text, background, 1.3)
    }
    function pressedOn(background) {
        return root._tintOf(root.text, background, 1.65)
    }

    // `color`, or, where it is under `ratio` on any of the opaque
    // `backgrounds`, the nearest HSL lightness of it that reaches it on all
    // of them: darker on light backgrounds, lighter on dark ones.
    function readableOn(color, backgrounds, ratio) {
        const worst = function(candidate) {
            let lowest = Infinity
            for (let i = 0; i < backgrounds.length; ++i) {
                lowest = Math.min(lowest, ColorUtils.contrastRatio(candidate, backgrounds[i]))
            }
            return lowest
        }
        if (worst(color) >= ratio) {
            return color
        }
        const darker = ColorUtils.relativeLuminance(backgrounds[0]) > 0.18
        for (let step = 1; step <= 100; ++step) {
            const lightness = color.hslLightness + (darker ? -step : step) / 100
            if (lightness < 0 || lightness > 1) {
                break
            }
            const candidate = root._withHslLightness(color, 1, lightness)
            if (worst(candidate) >= ratio) {
                return candidate
            }
        }
        return darker ? Qt.rgba(0, 0, 0, 1) : Qt.rgba(1, 1, 1, 1)
    }

    // `color` at the least opacity, in whole percent from 5% up to 30%,
    // that is `ratio` on the opaque `background`.
    function _tintOf(color, background, ratio) {
        for (let percent = 5; percent < 30; ++percent) {
            const tint = ColorUtils.withAlpha(color, percent / 100)
            if (ColorUtils.contrastRatio(ColorUtils.drawnOver(tint, background), background) >= ratio) {
                return tint
            }
        }
        return ColorUtils.withAlpha(color, 0.3)
    }

    // The share of the text colour over the selection, active or inactive,
    // under the pointer: the most, in whole percent up to 4 % on dark and
    // 6 % on light, that leaves the text 4.5:1 on both. Any more on dark
    // brings the inactive one up to the active. One share for both, so they
    // step alike. The other texts and the marks of a selected row are made
    // readable on them in turn (`textSecondarySelected`, `textColumn`,
    // `now`, `negativeSelected`).
    readonly property real _selectionHoverShare: {
        const selections = [root.selection, root.selectionInactive]
        for (let percent = root.themeIsDark ? 4 : 6; percent > 0; --percent) {
            let readable = true
            for (let s = 0; readable && s < selections.length; ++s) {
                const candidate = ColorUtils.mixColor(selections[s], root.text, percent / 100)
                readable = ColorUtils.contrastRatio(root.text, candidate) >= 4.5
            }
            if (readable) {
                return percent / 100
            }
        }
        return 0
    }

    // Tertiary text steps up to the selected row's own on a selected row,
    // and to the hovered row's own on a hovered one.
    function textTertiaryOn(selected, hovered) {
        return selected ? root.textTertiarySelected : (hovered ? root.textTertiaryHovered : root.textTertiary)
    }

    // `base` with its saturation scaled by `saturationScale` and its HSL
    // lightness moved so that its relative luminance, and so its contrast
    // with any colour, stays as it was.
    function _desaturated(base, saturationScale) {
        const target = ColorUtils.relativeLuminance(base)
        let low = 0
        let high = 1
        for (let i = 0; i < 24; ++i) {
            const middle = (low + high) / 2
            if (ColorUtils.relativeLuminance(root._withHslLightness(base, saturationScale, middle)) < target) {
                low = middle
            } else {
                high = middle
            }
        }
        return root._withHslLightness(base, saturationScale, (low + high) / 2)
    }

    // `base` with its saturation scaled by `saturationScale` and its HSL
    // lightness set to `lightness`, as an RGB colour like the others.
    function _withHslLightness(base, saturationScale, lightness) {
        const hsl = Qt.hsla(Math.max(0, base.hslHue),
                            ColorUtils.clamp01(base.hslSaturation * saturationScale),
                            lightness, 1)
        return Qt.rgba(hsl.r, hsl.g, hsl.b, 1)
    }

    // The colours of everything drawn on a visualization: they come from
    // the plot it sits on, never from the window.
    component PlotColors: QtObject {
        // The floor, where the plot has nothing to draw.
        required property color plotBg
        // A label plate, with a 1 px edge outside it.
        required property color plotPlate
        required property color plotPlateEdge
        // A viewer's title strip over the plot: opaque, so nothing under it,
        // such as the playhead or the plot itself, shows through.
        required property color plotStrip
        required property color plotText
        required property color plotTextSecondary
        // Text.Outline around scale labels.
        required property color plotOutline
        // The 1 px line between lanes.
        required property color plotLaneSeam
        required property color plotTick
        required property color plotLink
        // The level meter: its track, the bar on it, the gaps under its
        // lamp and its peak tick, and its scale's ticks. On the
        // spectrogram, the fill is the colour map's.
        required property color meterTrough
        required property color meterFill
        required property color meterSeam
        required property color meterPeak
        required property color meterClipOff
        required property color meterTick
        // The meter before any audio, while the pane is at rest: its track
        // in the plot's floor and its marks in the rest lines' colour, so
        // it is of one piece with the pane waiting (a lit trough stood out
        // as the one instrument switched on).
        required property color meterRestTrough
        required property color meterRestTick
        // A checked tool button on a plate: a solid fill, at least 3:1 on
        // the plate, and its glyph or letter, at least 4.5:1 on the fill.
        required property color checkedFill
        required property color checkedGlyph
        // The 1 px edge inside a chosen choice on a plate, as the mode
        // switch's chosen segment and a viewer's info toggle while on, over
        // their neutral fill (the fill alone was 1.4 to 1.7:1): at least
        // 3:1 on the plate and on the fill.
        required property color segmentEdge
        // A tool button's keyboard focus ring, at least 3:1 on the plate.
        required property color plotFocusRing
    }

    // The plot's floor: the deep indigo of the spectrogram colour map's
    // quiet end, fixed, the same in both themes. It is what the spectrogram
    // shows where it has no column to draw: never played, while a track is
    // analysed, and the part of a rolling view not yet filled (black there
    // read as a hole in the window, most of all in a light theme, and
    // unlike the pane once it plays). The now-playing panel's
    // stand-in for a track without a cover is of the same colour, as the
    // plot's own.
    readonly property color plotFloor: "#14062e"
    // A 1 px edge around a tile of the floor that stands for a cover, as
    // the now-playing stand-in's in a light theme and the library's no-art
    // thumbnail's: lighter than the floor, towards the app icon's rim, as
    // the icon's tile has one, so it reads as a cover rather than a hole.
    readonly property color plotFloorEdge: ColorUtils.mixColor(root.plotFloor, Qt.color("#6a4ad0"), 0.45)
    // The seam between the lanes at rest, never played: a cut, the plot's
    // deep violet under the floor, as the level meter's seams are (a
    // lighter seam, #372b4d at 1.45:1, read as one more of the rest lines,
    // white 20 % at 1.78:1, which it is now 1.87:1 from), in the plot's own
    // hue (the playing seam's neutral grey stood apart from the violet
    // floor and its lines). Playing, the seam is
    // plotLaneSeam's neutral one, which shows between lanes of silence.
    readonly property color plotRestLaneSeam: root.spectrogramPlot.meterSeam
    // The top 1 px of that cut, lit in the floor's violet: 2.6:1 on the
    // floor, twice the rest lines' 1.3:1, so the cut reads as the lanes'
    // boundary and not as one more line (dark alone, at 1.05:1, the seam
    // all but vanished between the louder lines; a single lighter seam read
    // as a rest line; the edge over its dark groove is a shape the lines
    // don't have).
    readonly property color plotRestLaneSeamEdge: "#5d4d80"
    // The never-played waveform's centre lines, one to a lane: the text
    // over the view, the least in whole percent that is 1.3:1 on it, as the
    // spectrogram's rest lines are faint on its floor.
    readonly property color waveformRestLine: ColorUtils.drawnOver(
        root._tintOf(root.text, root.view, 1.3), root.view)

    // The spectrogram is the same dark colour map in both themes, so its
    // set is fixed, on the plot's floor. Silence within a track is still
    // the colour map's lowest entry, black (SpectrogramItem::
    // rebuildPalette). What is drawn on it is neutral.
    readonly property PlotColors spectrogramPlot: PlotColors {
        id: spectrogramColors
        plotBg: root.plotFloor
        // A neutral dark over the floor.
        plotPlate: Qt.rgba(20 / 255, 20 / 255, 20 / 255, 0.88)
        plotPlateEdge: Qt.rgba(1, 1, 1, 0.14)
        // The plate over black, solid and neutral: #121212.
        plotStrip: Qt.tint("#000000", spectrogramColors.plotPlate)
        plotText: "#ececec"
        plotTextSecondary: Qt.rgba(236 / 255, 236 / 255, 236 / 255, 0.72)
        plotOutline: "#000000"
        // Neutral, as all that is drawn on the plot, and lighter than the
        // floor: 1.41:1 on it, so the seam between lanes reads (at 1.18:1
        // it did not), and it shows between lanes of black
        // silence too.
        plotLaneSeam: "#2e2e2e"
        plotTick: Qt.rgba(236 / 255, 236 / 255, 236 / 255, 0.75)
        plotLink: "#7cc8f0"
        // In the colour map's violet, not neutral grey, so the meter reads
        // as part of the plot. The trough is 1.3:1 on the floor; the rungs
        // are the app icon's harmonic magenta, 3.2:1 on it (the colour
        // map's capped #c20055 was 2.3:1) and clearly not the
        // clip lamp's red.
        meterTrough: "#30244a"
        meterFill: "#c149a0"
        meterSeam: "#0a0318"
        meterPeak: "#f4f4f6"
        meterClipOff: Qt.rgba(1, 1, 1, 0.2)
        meterTick: Qt.rgba(1, 1, 1, 0.4)
        // The floor, and on it marks in white 20 %, #433858: a step over
        // the rest lines' 11 %, as a mark a few pixels long needs more
        // than a line across the lane to show (a meter without marks read
        // as a scrollbar's track).
        meterRestTrough: root.plotFloor
        meterRestTick: ColorUtils.drawnOver(Qt.rgba(1, 1, 1, 0.2), root.plotFloor)
        checkedFill: root.accent
        checkedGlyph: "#000000"
        // The dark theme's tertiary text, as the plot is dark in both.
        segmentEdge: "#8f959a"
        // The dark theme's focus ring, as the plot is dark in both.
        plotFocusRing: root._withHslLightness(root.accent, 1.03, 0.686)
    }

    // The waveform is drawn on the view colour, so its set follows the
    // theme.
    readonly property PlotColors waveformPlot: PlotColors {
        id: waveformColors
        plotBg: root.view
        plotPlate: ColorUtils.withAlpha(root.view, 0.94)
        plotPlateEdge: root.seam
        plotStrip: root.view
        plotText: root.text
        plotTextSecondary: root.textSecondary
        plotOutline: root.view
        plotLaneSeam: root.hairline
        plotTick: root.textSecondary
        plotLink: root.link
        // The meter in the waveform's colour, so it reads as the plot's own
        // instrument rather than as a piece of the queue's scrollbar above
        // it, whose greys it had: the waveform's colour mixed about 35 %
        // toward the view, less where that would be under 3:1 on the
        // trough (no seam across its top: under the queue's seam it made
        // one 2 px thick over the meter's column). From the pane's waveform
        // colour, so it stays the plot's own. The trough frames the bar
        // (LevelMeterItem's `framed`), a step off the view, so the column
        // reads as an instrument and not as the waveform's tail (at 1.2:1
        // it did not show).
        meterTrough: root.themeIsDark ? "#2b2d2f" : "#e8e9ea"
        meterFill: root._waveformMeterFill(ColorUtils.drawnOver(waveformColors.meterTrough, root.view))
        meterSeam: root.view
        meterPeak: ColorUtils.withAlpha(root.text, 0.95)
        // The unlit clip lamp, over the trough: in a light theme text at
        // 27 %, some 1.7:1 on it (at 18 %, 1.4:1, the lamp all but vanished
        // and its gap read as a notch at the bar's top).
        meterClipOff: ColorUtils.withAlpha(root.text, root.themeIsDark ? 0.18 : 0.27)
        // The meter's marks: the text colour, faint.
        meterTick: ColorUtils.withAlpha(root.text, 0.3)
        // The view, and the waveform's rest lines on it.
        meterRestTrough: root.view
        meterRestTick: root.waveformRestLine
        // The accent is 2.4:1 on white: the light fill is the focus ring's
        // darker blue. #3daee9 / #1a74a8.
        checkedFill: root.themeIsDark ? root.accent : root.focusRing
        checkedGlyph: root.themeIsDark ? root.view : "#ffffff"
        segmentEdge: root.textTertiary
        plotFocusRing: root.focusRing
    }

    // The line between the waveform's lanes in a viewer: in a light theme
    // #cdd2d6, a step stronger than the pane's hairline, which on a white
    // screen all but vanished between the channels; in a dark one the
    // pane's.
    readonly property color viewerWaveformLaneSeam: root.themeIsDark
        ? root.waveformPlot.plotLaneSeam : "#cdd2d6"

    // The waveform's colour toward the view: 35 %, or by 5 % steps less,
    // the most of it that is 3:1 on `trough`.
    function _waveformMeterFill(trough) {
        for (let amount = 0.35; amount > 0; amount -= 0.05) {
            const fill = ColorUtils.mixColor(root.paneWave, root.view, amount)
            if (ColorUtils.contrastRatio(fill, trough) >= 3)
                return fill
        }
        return root.paneWave
    }

    // The plot set for a visualization mode: 0 is the spectrogram, 1 the
    // waveform.
    function plotColors(visualizationMode) {
        return visualizationMode === 1 ? root.waveformPlot : root.spectrogramPlot
    }

    // A media surface, such as the album art viewer: dark in both themes,
    // as a cover reads best on black and a light theme's white around it
    // would outshine it. It is the spectrogram's set on a
    // black floor, so both viewers are of one family: the art on black,
    // the title strip and the details on the set's strip colour.
    readonly property PlotColors mediaPlot: PlotColors {
        id: mediaColors
        plotBg: "#000000"
        plotPlate: spectrogramColors.plotPlate
        plotPlateEdge: spectrogramColors.plotPlateEdge
        plotStrip: spectrogramColors.plotStrip
        plotText: spectrogramColors.plotText
        plotTextSecondary: spectrogramColors.plotTextSecondary
        plotOutline: spectrogramColors.plotOutline
        // Nothing is darker than the floor, so the seam is a little lighter.
        plotLaneSeam: "#1a1a1a"
        plotTick: spectrogramColors.plotTick
        plotLink: spectrogramColors.plotLink
        meterTrough: spectrogramColors.meterTrough
        meterFill: spectrogramColors.meterFill
        meterSeam: spectrogramColors.meterSeam
        meterPeak: spectrogramColors.meterPeak
        meterClipOff: spectrogramColors.meterClipOff
        meterTick: spectrogramColors.meterTick
        meterRestTrough: spectrogramColors.meterRestTrough
        meterRestTick: spectrogramColors.meterRestTick
        checkedFill: spectrogramColors.checkedFill
        checkedGlyph: spectrogramColors.checkedGlyph
        segmentEdge: spectrogramColors.segmentEdge
        plotFocusRing: spectrogramColors.plotFocusRing
    }

    // The colours before #373's tokens. They stay until every user has
    // moved to the tokens above.
    readonly property color uiPaneColor: root.windowRoot.palette.window
    readonly property color uiSurfaceColor: root.windowRoot.palette.window
    readonly property color uiSurfaceAltColor: root.windowRoot.palette.alternateBase
    readonly property color uiSurfaceRaisedColor: root.windowRoot.palette.base
    readonly property color uiHeaderColor: ColorUtils.mixColor(
        uiSurfaceAltColor,
        Kirigami.Theme.highlightColor,
        themeIsDark ? 0.12 : 0.10)
    readonly property color uiSectionColor: ColorUtils.mixColor(
        uiSurfaceAltColor,
        Kirigami.Theme.highlightColor,
        themeIsDark ? 0.18 : 0.16)
    readonly property color uiColumnsColor: ColorUtils.mixColor(
        uiSurfaceAltColor,
        Kirigami.Theme.highlightColor,
        themeIsDark ? 0.11 : 0.09)
    readonly property color uiBorderColor: ColorUtils.mixColor(
        uiSurfaceColor,
        Kirigami.Theme.textColor,
        themeIsDark ? 0.30 : 0.18)
    readonly property color uiTextColor: Kirigami.Theme.textColor
    readonly property color uiMutedTextColor: ColorUtils.mixColor(
        Kirigami.Theme.disabledTextColor,
        Kirigami.Theme.textColor,
        themeIsDark ? 0.12 : 0.06)
    readonly property color uiSelectionColor: ColorUtils.mixColor(
        Kirigami.Theme.highlightColor,
        uiSurfaceColor,
        themeIsDark ? 0.08 : 0.06)
    readonly property color uiSelectionTextColor: Kirigami.Theme.highlightedTextColor
    readonly property color uiActiveIndicatorColor: ColorUtils.mixColor(
        Kirigami.Theme.highlightColor,
        Kirigami.Theme.positiveTextColor,
        0.35)
    readonly property color uiAccentColor: Kirigami.Theme.highlightColor
    readonly property color uiAccentTextColor: Kirigami.Theme.highlightedTextColor

    // The font of the window's text, which sizes the rows. A window root
    // that is not a window has no font of its own.
    readonly property font baseFont: root.windowRoot.font !== undefined
        ? root.windowRoot.font
        : Qt.application.font
    // The base font, bold. List rows draw plain Text with these fonts and
    // this palette's colours rather than styled controls, which each set up
    // a Kirigami theme of their own.
    // A font's pointSize in QML is its size in points even when it was set
    // in pixels.
    readonly property font boldFont: Qt.font({
        family: root.baseFont.family,
        pointSize: root.baseFont.pointSize,
        bold: true
    })
    // The height of a list row: a line of text and a small spacing above
    // and below it, 26 px with the default 10 pt font (a grid unit and
    // 8 px). The rows and the scrolling, paging and drop positions that
    // count in rows all use it, so they follow the font size.
    readonly property real rowHeight: Math.round(root._baseFontMetrics.height)
        + 2 * Kirigami.Units.smallSpacing
    // Kirigami's grid unit on the window's font: the height of a line of
    // it, made even, as Kirigami.Units.gridUnit is of the platform's font;
    // 18 px with the default 10 pt font, 44 px at 24 pt.
    readonly property real gridUnit: 2 * Math.ceil(root._baseFontMetrics.height / 2)
    // The base font's size in points, for text sized relative to it.
    readonly property real basePointSize: root.baseFont.pointSize > 0
        ? root.baseFont.pointSize
        : root.baseFont.pixelSize * 0.75
    // The small text of the design, for headers, counts and the plot's
    // labels: 12 px with the default 10 pt font. Set it as font.pointSize:
    // Qt.font() would round it to whole points.
    readonly property real smallPointSize: root.basePointSize * 0.9
    // The smallest text of the design, for the labels of the plot's short
    // lanes: 11 px with the default 10 pt font.
    readonly property real compactPointSize: root.basePointSize * 0.825
    // The size of a Kirigami.Heading of `level` on the window's font, for
    // plain text that is to be one: 1.35, 1.2, 1.15, 1.1 and 1.05 times it
    // for levels 1 to 5.
    function headingPointSize(level) {
        const factors = [1.35, 1.20, 1.15, 1.10, 1.05]
        return root.basePointSize * (factors[level - 1] || 1)
    }
    // The text of a viewer over the whole screen, its title strip and the
    // album art's details alike: a step larger, 1.2 times the window's
    // (so the strip and the details are on one type scale).
    readonly property real fullscreenTextScale: 1.2
    // Old-style figures line up in columns with tabular figures.
    readonly property var tabularFigures: ({ "tnum": 1 })

    // The design's glyphs and small buttons are sized for the default 10 pt
    // font: a larger font scales them, as it does the transport's, and a
    // smaller one keeps them.
    readonly property real fontScale: Math.max(1, root.basePointSize / 10)
    function scaledSize(size) {
        return Math.max(size, Math.round(size * root.fontScale))
    }
    // The glyph of a small tool button, 16 px, and the button, 24 px, or
    // the glyph and 4 px around it.
    readonly property int toolIconSize: root.scaledSize(Kirigami.Units.iconSizes.small)
    readonly property int toolButtonSize: Math.max(24, root.toolIconSize + 8)
    // A glyph beside a line of text, a list row's or a button caption's:
    // the largest icon size a line of the font holds, 16 px with the
    // default 10 pt font and 32 px at 24 pt. The dialogs' measures that
    // bound text, tuned at 10 pt, follow the font through scaledSize(), so
    // the default font draws every dialog as it was (unscaled, glyphs and
    // widths stayed at their 10 pt sizes at 24 pt).
    readonly property int labelIconSize: Kirigami.Units.iconSizes.sizeForLabels

    readonly property FontMetrics _baseFontMetrics: FontMetrics {
        font: root.baseFont
    }
}
