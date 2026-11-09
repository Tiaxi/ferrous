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

// How wide `text` can be with other digits in it: as wide as it is with each
// digit the font's widest. A readout's text changes its digits as the
// pointer moves, so whether its plate fits is decided by this width, not by
// the text's own.
inline int widestDigitsWidth(const QString &text, const QFontMetrics &metrics) {
    QChar widest(u'0');
    for (char16_t digit = u'1'; digit <= u'9'; ++digit) {
        if (metrics.horizontalAdvance(QChar(digit)) > metrics.horizontalAdvance(widest)) {
            widest = QChar(digit);
        }
    }
    QString widened = text;
    for (QChar &character : widened) {
        if (character.isDigit()) {
            character = widest;
        }
    }
    return metrics.horizontalAdvance(widened);
}

// A widest width (see widestDigitsWidth), measured in the readout's font and
// kept until that font, or what it is the width of (`key`), changes: the
// application's font, which the readout's takes its family and style from,
// can change while the app runs. The width is of a pixel-sized font, as the
// readout's text is measured, so the device pixel ratio doesn't change it.
class WidestWidth {
public:
    template <typename Measure>
    int get(const QFont &font, double key, Measure &&measure) {
        if (!m_measured || font != m_font || key != m_key) {
            m_width = measure(QFontMetrics(font));
            m_font = font;
            m_key = key;
            m_measured = true;
        }
        return m_width;
    }

private:
    QFont m_font;
    double m_key{0.0};
    int m_width{0};
    bool m_measured{false};
};

// A scale along a plot's right edge: how far its labels reach from the
// edge, and whether one is beside the bottom edge, where the time's plate
// goes.
struct RightScale {
    int width{0};
    bool reachesBottom{false};
};

// The rows a plate's top can be on: `first` to `last`, none where
// `last < first`.
struct Rows {
    int first{0};
    int last{-1};

    bool isEmpty() const { return last < first; }
    int count() const { return last - first + 1; }
};

// The part of `rows` where a plate `plateHeight` tall keeps `kGap` clear of
// `head`'s rows: the larger of the parts below and above it, below on a tie.
inline Rows rowsClearOf(const Rows &rows, const QRect &head, int plateHeight) {
    const Rows below{std::max(rows.first, head.bottom() + 1 + kGap), rows.last};
    const Rows above{rows.first, std::min(rows.last, head.top() - kGap - plateHeight)};
    if (below.isEmpty()) {
        return above;
    }
    return above.isEmpty() || below.count() >= above.count() ? below : above;
}

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
// Neither plate goes under `laneHead` or out of the plot. A plot too narrow
// for both leaves a plate out, the same one wherever the pointer is: the
// value where the time doesn't fit beside it or above it, and the time where
// it doesn't fit right of `laneHead` on its own. Over the lane head, which is
// drawn above the plot, the time would be hidden and cover its buttons.
// Whether a plate fits is decided by `widestValue` and `widestTime`, the
// widths of the widest plates the readout shows (see widestDigitsWidth), so
// a plate doesn't come and go, or move the other, as its text changes.
inline std::pair<QRect, QRect> plateRects(
    int width, int height, int x, int y,
    const QSize &valuePlate, const QSize &timePlate,
    const QSizeF &reservedCorner = QSizeF(), const RightScale &scale = RightScale(),
    const QRect &laneHead = QRect(), int widestValue = 0, int widestTime = 0) {
    const int leftOfScale = width - scale.width - kGap;
    const int valueRoom = std::max(valuePlate.width(), widestValue);
    const int timeRoom = std::max(timePlate.width(), widestTime);

    // The time's band along the bottom edge, between the lane head and the
    // scale; a time that doesn't fit in it is left out.
    int timeTop = height;
    int timeLeft = 0;
    int timeRight = width;
    bool hasTime = !timePlate.isEmpty();
    if (hasTime) {
        timeTop = std::max(0, height - kEdgeMargin - timePlate.height());
        if (laneHead.isValid() && laneHead.bottom() + kGap >= timeTop) {
            timeLeft = laneHead.right() + 1 + kGap;
        }
        if (scale.width > 0 && scale.reachesBottom) {
            timeRight = leftOfScale;
        }
        hasTime = timeRight - timeLeft >= timeRoom;
    }

    QRect value;
    if (!valuePlate.isEmpty()) {
        const int right = scale.width > 0 ? leftOfScale : width - kEdgeMargin;
        const int home = right - valuePlate.width();
        // Where the widest value goes at the top of the lane, out from under
        // the corner: the leftmost spot of any, which places the time, so
        // that doesn't move as the pointer moves.
        const int widestHome = right - valueRoom;
        const int leftmost = ViewCorner::labelXClearOfCorner(
            widestHome, kEdgeMargin, valueRoom, valuePlate.height(), width, reservedCorner);
        const bool besideHead = laneHead.isValid() && leftmost < laneHead.right() + 1 + kGap;
        const auto clearOfHead = [&](const Rows &rows) {
            return besideHead ? rowsClearOf(rows, laneHead, valuePlate.height()) : rows;
        };
        // Above the time where there is room, beside it where the time
        // fits between the lane head and the value, and otherwise left out.
        const Rows above = hasTime
            ? clearOfHead({kEdgeMargin, timeTop - kGap - valuePlate.height()})
            : Rows();
        const Rows anywhere = clearOfHead(
            {kEdgeMargin, std::max(kEdgeMargin, height - kEdgeMargin - valuePlate.height())});
        const bool beside = above.isEmpty();
        const Rows rows = !beside ? above
            : !hasTime || leftmost - kGap - timeLeft >= timeRoom ? anywhere
            : Rows();
        if (widestHome >= 0 && !rows.isEmpty()) {
            const int top = std::clamp(y - valuePlate.height() / 2, rows.first, rows.last);
            value = QRect(
                std::max(0, ViewCorner::labelXClearOfCorner(
                    home, top, valuePlate.width(), valuePlate.height(), width, reservedCorner)),
                top, valuePlate.width(), valuePlate.height());
            if (hasTime && beside) {
                timeRight = std::min(timeRight, leftmost - kGap);
            }
        }
    }

    QRect time;
    if (hasTime) {
        time = QRect(
            std::clamp(x - timePlate.width() / 2, timeLeft, timeRight - timePlate.width()),
            timeTop, timePlate.width(), timePlate.height());
    }
    return {value, time};
}

// Draws a plate at `rect` with `text` in `font`.
inline void drawPlate(QPainter &painter, const QRect &rect, const QString &text,
                      const QFont &font, const Colors &colors) {
    // A plate left out has no rect, and draws nothing.
    if (rect.isEmpty()) {
        return;
    }
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
