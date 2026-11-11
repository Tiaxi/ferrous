// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QRectF>
#include <QSizeF>

#include <algorithm>
#include <cmath>

// A visualization view's top right corner holds controls drawn over it:
// `reserved` is how far they reach from its right and top edges. The view
// keeps what it draws itself out from under them.
namespace ViewCorner {

// The FPS counter ends beside the corner's controls, whether they are shown
// or not and however wide its text is, and so never moves them or itself.
inline QRectF fpsCounterRect(double viewWidth, const QSizeF &reserved, const QSizeF &textSize) {
    constexpr double edgeMargin = 8.0;
    constexpr double top = 4.0;
    const double right = viewWidth - std::max(edgeMargin, reserved.width());
    return QRectF(std::max(4.0, right - textSize.width()), top, textSize.width(), textSize.height());
}

// How far a label on the view keeps from the corner's controls, so that it
// reads as apart from their plates rather than butting against them.
constexpr int kLabelGap = 6;

// Where a label on the view's right edge goes: at `x`, or kLabelGap left of
// the corner's controls if it would be under them or within kLabelGap of
// them, beside or below.
inline int labelXClearOfCorner(
    int x, int y, int labelWidth, int labelHeight, int viewWidth, const QSizeF &reserved) {
    const int cornerLeft = viewWidth - static_cast<int>(std::ceil(reserved.width()));
    const int cornerBottom = static_cast<int>(std::ceil(reserved.height()));
    if (reserved.isEmpty() || y >= cornerBottom + kLabelGap || y + labelHeight <= 0
        || x + labelWidth + kLabelGap <= cornerLeft) {
        return x;
    }
    return std::max(0, cornerLeft - kLabelGap - labelWidth);
}

} // namespace ViewCorner
