// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import org.kde.kirigami 2.20 as Kirigami
import "../logic/ColorUtils.js" as ColorUtils

QtObject {
    id: root

    required property var windowRoot

    readonly property bool themeIsDark: ColorUtils.colorLuma(Kirigami.Theme.backgroundColor) < 0.45
    readonly property color uiPaneColor: themeIsDark
        ? ColorUtils.mixColor(Kirigami.Theme.backgroundColor, "#ffffff", 0.08)
        : ColorUtils.mixColor(Kirigami.Theme.backgroundColor, "#ffffff", 0.20)
    readonly property color uiSurfaceColor: themeIsDark
        ? ColorUtils.mixColor(Kirigami.Theme.backgroundColor, "#ffffff", 0.14)
        : "#ffffff"
    readonly property color uiSurfaceAltColor: themeIsDark
        ? ColorUtils.mixColor(uiSurfaceColor, Kirigami.Theme.textColor, 0.08)
        : ColorUtils.mixColor(uiSurfaceColor, Kirigami.Theme.textColor, 0.07)
    readonly property color uiSurfaceRaisedColor: themeIsDark
        ? ColorUtils.mixColor(uiSurfaceColor, "#ffffff", 0.08)
        : "#ffffff"
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
        themeIsDark ? 0.22 : 0.18)
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
}
