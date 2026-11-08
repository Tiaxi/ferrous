// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ViewCorner.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QRect>
#include <QSize>
#include <QSizeF>
#include <QString>

#include <algorithm>
#include <utility>

// A crosshair's readout on the axes of a plot, as the waveform and the
// spectrogram show it: the time on the bottom edge at the vertical line, and
// the value (a level, or a frequency and a level) on the right edge at the
// horizontal line. Each is a plate drawn on the plot that the pointer moving
// near an edge clamps inside it.
namespace CrosshairReadout {

// Between a plate's edge and its text.
constexpr int kPadding = 3;
// Between the value's plate and the plot's right edge, and between the
// time's plate and its bottom edge.
constexpr int kEdgeMargin = 2;
// Between the value's plate and the time's, and a scale along the right edge.
constexpr int kGap = 4;
constexpr int kRadius = 3;

// A plate's colours. A plate with an edge has it 1 px wide around its fill,
// as PlotPlate draws one.
struct Colors {
    QColor fill;
    QColor text;
    QColor edge{Qt::transparent};

    bool hasEdge() const { return edge.alpha() > 0; }
};

inline QFont font() {
    QFont font;
    font.setPixelSize(10);
    return font;
}

// The size of a plate for text `textWidth` wide.
inline QSize plateSize(int textWidth, const QFontMetrics &metrics, const Colors &colors) {
    const int edge = colors.hasEdge() ? 2 : 0;
    return {textWidth + 2 * kPadding + edge, metrics.height() + 2 * kPadding + edge};
}

// A scale along a plot's right edge: how far its labels reach from the
// edge, and whether one is beside the bottom edge, where the time's plate
// goes.
struct RightScale {
    int width{0};
    bool reachesBottom{false};
};

// Where the value's and the time's plates go on a plot `width` by `height`,
// for the crosshair's lines at `x` and `y`; an empty size leaves its plate
// out. The value's plate is centred on the horizontal line at the right
// edge, or left of the scale along it, and keeps out from under the
// corner's controls (see ViewCorner). The time's plate is centred on the
// vertical line at the bottom edge, and left of the scale where one of its
// labels is beside it. Where the two would meet, the value goes above the
// time, or beside it in a lane too short for that.
inline std::pair<QRect, QRect> plateRects(
    int width, int height, int x, int y,
    const QSize &valuePlate, const QSize &timePlate,
    const QSizeF &reservedCorner = QSizeF(), const RightScale &scale = RightScale()) {
    const int leftOfScale = width - scale.width - kGap;
    QRect time;
    if (!timePlate.isEmpty()) {
        const int right = scale.width > 0 && scale.reachesBottom ? leftOfScale : width;
        time = QRect(
            std::clamp(x - timePlate.width() / 2, 0, std::max(0, right - timePlate.width())),
            std::max(0, height - kEdgeMargin - timePlate.height()),
            timePlate.width(), timePlate.height());
    }
    if (valuePlate.isEmpty()) {
        return {QRect(), time};
    }
    const int valueRight = scale.width > 0 ? leftOfScale : width - kEdgeMargin;
    const auto clearOfCorner = [&](const QRect &plate) {
        return plate.translated(
            ViewCorner::labelXClearOfCorner(
                plate.left(), plate.top(), plate.width(), plate.height(), width, reservedCorner)
                - plate.left(),
            0);
    };
    const auto meetsTime = [&](const QRect &plate) {
        return time.isValid() && plate.adjusted(-kGap, -kGap, kGap, kGap).intersects(time);
    };
    const int top = std::clamp(y - valuePlate.height() / 2, kEdgeMargin,
                               std::max(kEdgeMargin, height - kEdgeMargin - valuePlate.height()));
    QRect value = clearOfCorner(QRect(
        QPoint(std::max(0, valueRight - valuePlate.width()), top), valuePlate));
    if (!meetsTime(value)) {
        return {value, time};
    }
    // Above the time, where the lane is tall enough.
    const int above = time.top() - kGap - valuePlate.height();
    if (above >= 0) {
        return {clearOfCorner(QRect(QPoint(value.left(), above), valuePlate)), time};
    }
    // Otherwise beside it, on its line: to the left, or else to the right.
    const QRect left = clearOfCorner(QRect(
        QPoint(std::min(value.left(), time.left() - kGap - valuePlate.width()), top), valuePlate));
    if (left.left() >= 0 && !meetsTime(left)) {
        return {left, time};
    }
    const QRect right(QPoint(time.right() + 1 + kGap, top), valuePlate);
    if (right.right() < valueRight && clearOfCorner(right) == right) {
        return {right, time};
    }
    // A plot too narrow for both side by side.
    return {QRect(QPoint(std::max(0, left.left()), top), valuePlate), time};
}

// Draws a plate at `rect` with `text` in `font`.
inline void drawPlate(QPainter &painter, const QRect &rect, const QString &text,
                      const QFont &font, const Colors &colors) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    QRectF fill(rect);
    if (colors.hasEdge()) {
        // The edge's pixels are the ring around the fill.
        painter.setPen(QPen(colors.edge, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(fill.adjusted(0.5, 0.5, -0.5, -0.5), kRadius + 1, kRadius + 1);
        fill = fill.adjusted(1.0, 1.0, -1.0, -1.0);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.fill);
    painter.drawRoundedRect(fill, kRadius, kRadius);
    painter.setFont(font);
    painter.setPen(colors.text);
    const QFontMetrics metrics(font);
    painter.drawText(QPointF(fill.left() + kPadding, fill.top() + kPadding + metrics.ascent()), text);
    painter.restore();
}

} // namespace CrosshairReadout
