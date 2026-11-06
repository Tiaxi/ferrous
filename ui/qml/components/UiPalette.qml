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
    // Counts, years and track numbers. #8f959a (6.0:1 on view) / #646d77
    // (5.3:1 on view, 4.5:1 on a hovered row).
    // On a selected row, use textTertiaryOn(true): it is 3.7:1 on the
    // focused tint.
    readonly property color textTertiary: root.themeIsDark
        ? ColorUtils.mixColor(root._inactiveText, root.view, 0.134)
        : ColorUtils.mixColor(root._inactiveText, root.text, 0.185)
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
    // Text over the row: the hover tint.
    readonly property color hover: ColorUtils.withAlpha(root.text, root.themeIsDark ? 0.065 : 0.08)
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
    // #3daee9 / #1a74a8: the accent, darkened on light to read as text.
    readonly property color link: root.themeIsDark ? root.accent : root.focusRing
    // #ed5a6a / #c4293b: the theme's negative text, lightened on dark and
    // darkened on light for text contrast.
    readonly property color negative: root.themeIsDark
        ? root._withHslLightness(Kirigami.Theme.negativeTextColor, 1.18, 0.639)
        : root._withHslLightness(Kirigami.Theme.negativeTextColor, 0.96, 0.465)
    // The seek bar's waveform. #b9e1f5 / #2c566c.
    readonly property color wave: ColorUtils.mixColor(root.text, root.accent, 0.35)
    // Fixed: the spectrogram colour map's amber. It marks only what is
    // playing: the playing row's glyph and title, the playing tree row and
    // the seek playhead.
    readonly property color now: root.themeIsDark ? "#f2b035" : "#955500"
    // The scrim behind the search palette.
    readonly property color dim: Qt.rgba(0, 0, 0, root.themeIsDark ? 0.5 : 0.25)

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
        // A tool button's keyboard focus ring, at least 3:1 on the plate.
        required property color plotFocusRing
    }

    // The spectrogram is the same dark colour map in both themes, so its
    // set is fixed.
    readonly property PlotColors spectrogramPlot: PlotColors {
        id: spectrogramColors
        plotBg: "#14062e"
        plotPlate: Qt.rgba(20 / 255, 6 / 255, 46 / 255, 0.88)
        plotPlateEdge: Qt.rgba(1, 1, 1, 0.14)
        plotText: "#ece8f4"
        plotTextSecondary: Qt.rgba(236 / 255, 232 / 255, 244 / 255, 0.72)
        plotOutline: "#14062e"
        plotLaneSeam: "#0a0318"
        plotTick: Qt.rgba(236 / 255, 232 / 255, 244 / 255, 0.75)
        plotLink: "#7cc8f0"
        meterTrough: "#2b1f43"
        meterFill: "#a39bb3"
        meterPeak: "#f4f4f6"
        meterClipOff: Qt.rgba(1, 1, 1, 0.2)
        checkedFill: root.accent
        checkedGlyph: spectrogramColors.plotBg
        // The dark theme's focus ring, as the plot is dark in both.
        plotFocusRing: root._withHslLightness(root.accent, 1.03, 0.686)
    }

    // The waveform is drawn on the view colour, so its set follows the
    // theme.
    readonly property PlotColors waveformPlot: PlotColors {
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
        // #7d8790 / #5b6670.
        meterFill: root.themeIsDark
            ? ColorUtils.mixColor(root._inactiveText, root.view, 0.231)
            : ColorUtils.mixColor(root._inactiveText, root.text, 0.267)
        meterPeak: ColorUtils.withAlpha(root.text, 0.95)
        meterClipOff: ColorUtils.withAlpha(root.text, 0.18)
        // The accent is 2.4:1 on white: the light fill is the focus ring's
        // darker blue. #3daee9 / #1a74a8.
        checkedFill: root.themeIsDark ? root.accent : root.focusRing
        checkedGlyph: root.themeIsDark ? root.view : "#ffffff"
        plotFocusRing: root.focusRing
    }

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
    readonly property color uiWaveformColor: root.wave
    // The seek bar's playhead: the text colour, outlined in the opposite
    // brightness of the text colour itself (not of the palette's
    // background): black when the text is light and white when it is dark.
    // Whatever is behind them, the waveform or the bar, one of the two
    // stands out.
    readonly property color uiPlayheadColor: uiTextColor
    readonly property color uiPlayheadOutlineColor: ColorUtils.colorLuma(uiTextColor) >= 0.5
        ? Qt.rgba(0, 0, 0, 1)
        : Qt.rgba(1, 1, 1, 1)

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
    // The height of a list row: a line of text and a third of a line of
    // margin. The rows and the scrolling, paging and drop positions that
    // count in rows all use it, so they follow the font size.
    readonly property real rowHeight: Math.round(root._baseFontMetrics.height * 4 / 3)
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

    readonly property FontMetrics _baseFontMetrics: FontMetrics {
        font: root.baseFont
    }
}
