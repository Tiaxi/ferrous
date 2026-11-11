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
    // darkened from the mix to be 4.6:1 on a hovered row, a margin over
    // 4.5:1 that rendering cannot round away (6.2:1 on view in both).
    // On a selected row, use textTertiaryOn(true): it is 3.7:1 on the
    // focused tint.
    readonly property color textTertiary: root.readableOn(root.themeIsDark
        ? ColorUtils.mixColor(root._inactiveText, root.view, 0.134)
        : ColorUtils.mixColor(root._inactiveText, root.text, 0.185), [root.view, root.hoveredRow], 4.6)
    // The artist and album cells of the queue. #c2c2c3 / #5a5c5e.
    readonly property color textColumn: ColorUtils.mixColor(root.text, root.view, 0.25)
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
    // a press reads apart from a hover (#373 H5: 10% read as the hover).
    // The least, in whole percent, that is 1.65:1 on the view: 17% / 25%.
    readonly property color pressed: root._tintOf(root.text, root.view, 1.65)
    // A hovered row: the hover tint on the view.
    readonly property color hoveredRow: ColorUtils.drawnOver(root.hover, root.view)
    // The view mixed with the accent. #1f3e4e / #d4edfa.
    readonly property color selection: ColorUtils.mixColor(
        root.view, root.accent, root.themeIsDark ? 0.26 : 0.22)
    // The selection of a list without keyboard focus.
    readonly property color selectionInactive: ColorUtils.mixColor(
        root.view, root.accent, root.themeIsDark ? 0.20 : 0.16)
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
    // Fixed: the spectrogram colour map's amber. It marks only what is
    // playing: the playing row's glyph and title, the playing tree row and
    // the seek playhead. The light one is darkened to be 4.5:1 on a hovered
    // row as well. #f2b035 / #905200.
    readonly property color now: root.themeIsDark
        ? "#f2b035"
        : root.readableOn(root._nowLight, [root.view, root.hoveredRow, root.selection], 4.5)
    readonly property color _nowLight: "#955500"
    // The opacity of a quiet amber mark, as of a collapsed tree row that
    // holds the playing track: 60%, or more where that is under 3:1 on a
    // row's background, plain, hovered or selected.
    readonly property real nowMarkOpacity: {
        const backgrounds = [root.view, root.hoveredRow, root.selection, root.selectionInactive]
        let opacity = 0.6
        for (let i = 0; i < backgrounds.length; ++i) {
            opacity = Math.max(opacity, ColorUtils.opacityForContrast(root.now, backgrounds[i], 0.6, 3))
        }
        return opacity
    }
    // The scrim behind the search palette.
    readonly property color dim: Qt.rgba(0, 0, 0, root.themeIsDark ? 0.5 : 0.25)

    // The link on `background`, such as a hover tint drawn over the window
    // colour: the link, darker or lighter where it falls short of 4.5:1 on it.
    function linkOn(background) {
        return root.readableOn(root.link, [background], 4.5)
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

    // Tertiary text steps up to secondary on a selected row.
    function textTertiaryOn(selected) {
        return selected ? root.textSecondary : root.textTertiary
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
        // The floor.
        required property color plotBg
        // A label plate, with a 1 px edge outside it.
        required property color plotPlate
        required property color plotPlateEdge
        required property color plotText
        required property color plotTextSecondary
        // Text.Outline around scale labels.
        required property color plotOutline
        // The 1 px line between lanes.
        required property color plotLaneSeam
        required property color plotTick
        required property color plotLink
        required property color meterTrough
        required property color meterFill
        required property color meterPeak
        required property color meterClipOff
        // A checked tool button on a plate: a solid fill, at least 3:1 on
        // the plate, and its glyph or letter, at least 4.5:1 on the fill.
        required property color checkedFill
        required property color checkedGlyph
        // A checked flat button's tint, as on a viewer's title strip: the
        // selection's mix on the plot's floor.
        required property color checkedTint
        // A tool button's keyboard focus ring, at least 3:1 on the plate.
        required property color plotFocusRing
    }

    // The spectrogram is the same dark colour map in both themes, so its
    // set is fixed, from the colour map's floor: black, the lowest entry of
    // the colour and the greyscale map (SpectrogramItem::rebuildPalette),
    // which silence is drawn in. What is drawn on it is neutral.
    readonly property PlotColors spectrogramPlot: PlotColors {
        id: spectrogramColors
        plotBg: "#000000"
        // A neutral dark lifted from the floor.
        plotPlate: Qt.rgba(20 / 255, 20 / 255, 20 / 255, 0.88)
        plotPlateEdge: Qt.rgba(1, 1, 1, 0.14)
        plotText: "#ececec"
        plotTextSecondary: Qt.rgba(236 / 255, 236 / 255, 236 / 255, 0.72)
        plotOutline: "#000000"
        // Nothing is darker than the floor, so the seam is a little lighter.
        plotLaneSeam: "#1a1a1a"
        plotTick: Qt.rgba(236 / 255, 236 / 255, 236 / 255, 0.75)
        plotLink: "#7cc8f0"
        meterTrough: "#2a2a2a"
        meterFill: "#a3a3a3"
        meterPeak: "#f4f4f6"
        meterClipOff: Qt.rgba(1, 1, 1, 0.2)
        checkedFill: root.accent
        checkedGlyph: spectrogramColors.plotBg
        // The dark theme's selection, on the floor.
        checkedTint: ColorUtils.mixColor(spectrogramColors.plotBg, root.accent, 0.26)
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
        plotText: root.text
        plotTextSecondary: root.textSecondary
        plotOutline: root.view
        plotLaneSeam: root.hairline
        plotTick: root.textSecondary
        plotLink: root.link
        meterTrough: ColorUtils.withAlpha(root.text, 0.10)
        // #7d8790 / #5b6670: the inactive text, toward the view or the text.
        // A theme whose inactive text is too close to its view to reach 3:1
        // on the trough gets the text toward the view instead.
        meterFill: ColorUtils.contrastRatio(root._waveformMeterFill,
                                            ColorUtils.drawnOver(waveformColors.meterTrough, root.view)) >= 3
            ? root._waveformMeterFill
            : ColorUtils.mixColor(root.text, root.view, root.themeIsDark ? 0.45 : 0.27)
        meterPeak: ColorUtils.withAlpha(root.text, 0.95)
        meterClipOff: ColorUtils.withAlpha(root.text, 0.18)
        // The accent is 2.4:1 on white: the light fill is the focus ring's
        // darker blue. #3daee9 / #1a74a8.
        checkedFill: root.themeIsDark ? root.accent : root.focusRing
        checkedGlyph: root.themeIsDark ? root.view : "#ffffff"
        checkedTint: root.selection
        plotFocusRing: root.focusRing
    }

    readonly property color _waveformMeterFill: root.themeIsDark
        ? ColorUtils.mixColor(root._inactiveText, root.view, 0.231)
        : ColorUtils.mixColor(root._inactiveText, root.text, 0.267)

    // The plot set for a visualization mode: 0 is the spectrogram, 1 the
    // waveform.
    function plotColors(visualizationMode) {
        return visualizationMode === 1 ? root.waveformPlot : root.spectrogramPlot
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
    // The base font's size in points, for text sized relative to it.
    readonly property real basePointSize: root.baseFont.pointSize > 0
        ? root.baseFont.pointSize
        : root.baseFont.pixelSize * 0.75
    // The small text of the design, for headers, counts and the plot's
    // labels: 12 px with the default 10 pt font. Set it as font.pointSize:
    // Qt.font() would round it to whole points.
    readonly property real smallPointSize: root.basePointSize * 0.9
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

    readonly property FontMetrics _baseFontMetrics: FontMetrics {
        font: root.baseFont
    }
}
