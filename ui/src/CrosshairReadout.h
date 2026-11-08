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
// out. Each follows its own line and nothing else, so neither jumps:
// - The value's plate is centred on the horizontal line at the right edge,
//   or left of the scale along it, and keeps out from under the corner's
//   controls (see ViewCorner). Where the lane is tall enough, it stays
//   above the time's.
// - The time's plate is centred on the vertical line at the bottom edge. It
//   stops left of the scale where one of its labels is beside it, right of
//   `laneHead` (the lowest lane's label and buttons) where that reaches down
//   beside it, and in a lane too short to have the value above it, left of
//   the value.
inline std::pair<QRect, QRect> plateRects(
    int width, int height, int x, int y,
    const QSize &valuePlate, const QSize &timePlate,
    const QSizeF &reservedCorner = QSizeF(), const RightScale &scale = RightScale(),
    const QRect &laneHead = QRect()) {
    const int leftOfScale = width - scale.width - kGap;
    const bool hasTime = !timePlate.isEmpty();
    const int timeTop = hasTime ? std::max(0, height - kEdgeMargin - timePlate.height()) : height;
    // How low the value's plate goes: above the time where there is room.
    const int valueLowest = std::max(kEdgeMargin, height - kEdgeMargin - valuePlate.height());
    const int valueAboveTime = timeTop - kGap - valuePlate.height();
    const bool valueAbove = hasTime && valueAboveTime >= kEdgeMargin;

    QRect value;
    if (!valuePlate.isEmpty()) {
        const int right = scale.width > 0 ? leftOfScale : width - kEdgeMargin;
        const int top = std::clamp(y - valuePlate.height() / 2, kEdgeMargin,
                                   valueAbove ? valueAboveTime : valueLowest);
        const int left = std::max(0, right - valuePlate.width());
        value = QRect(
            std::max(0, ViewCorner::labelXClearOfCorner(
                left, top, valuePlate.width(), valuePlate.height(), width, reservedCorner)),
            top, valuePlate.width(), valuePlate.height());
    }

    QRect time;
    if (hasTime) {
        int right = scale.width > 0 && scale.reachesBottom ? leftOfScale : width;
        if (value.isValid() && !valueAbove) {
            right = std::min(right, value.left() - kGap);
        }
        const int left = laneHead.isValid() && laneHead.bottom() + kGap >= timeTop
            ? laneHead.right() + 1 + kGap
            : 0;
        time = QRect(
            std::clamp(x - timePlate.width() / 2, left, std::max(left, right - timePlate.width())),
            timeTop, timePlate.width(), timePlate.height());
    }
    return {value, time};
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
