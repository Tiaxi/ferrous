// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import org.kde.kirigami 2.20 as Kirigami
import "../logic/ColorUtils.js" as ColorUtils

QtObject {
    id: root

    required property var windowRoot

    readonly property bool themeIsDark: ColorUtils.colorLuma(root.windowRoot.palette.window) < 0.45
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
    readonly property color uiWaveformColor: ColorUtils.mixColor(
        Kirigami.Theme.textColor,
        Kirigami.Theme.highlightColor,
        0.35)

    // The font of the window's text, which sizes the rows. A window root
    // that is not a window has no font of its own.
    readonly property font baseFont: root.windowRoot.font !== undefined
        ? root.windowRoot.font
        : Qt.application.font
    // The height of a list row: a line of text and a third of a line of
    // margin. The rows and the scrolling, paging and drop positions that
    // count in rows all use it, so they follow the font size.
    readonly property real rowHeight: Math.round(root._baseFontMetrics.height * 4 / 3)
    // The base font's size in points, for text sized relative to it.
    readonly property real basePointSize: root.baseFont.pointSize > 0
        ? root.baseFont.pointSize
        : root.baseFont.pixelSize * 0.75

    readonly property FontMetrics _baseFontMetrics: FontMetrics {
        font: root.baseFont
    }
}
