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

// Where a label on the view's right edge goes: at `x`, or to the left of the
// corner's controls if it would be under them.
inline int labelXClearOfCorner(
    int x, int y, int labelWidth, int labelHeight, int viewWidth, const QSizeF &reserved) {
    const int cornerLeft = viewWidth - static_cast<int>(std::ceil(reserved.width()));
    if (reserved.isEmpty() || y >= reserved.height() || y + labelHeight <= 0
        || x + labelWidth <= cornerLeft) {
        return x;
    }
    return std::max(0, cornerLeft - labelWidth);
}

} // namespace ViewCorner
