// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ViewCorner.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QPainter>
#include <QPen>
#include <QRect>
#include <QSize>
#include <QSizeF>
#include <QString>

#include <algorithm>
#include <cmath>
#include <utility>

// A crosshair's readout on the axes of a plot, as the waveform and the
// spectrogram show it: the time on the bottom edge at the vertical line, and
// the value (a level, or a frequency and a level) on the right edge at the
// horizontal line. Each is a plate drawn on the plot that the pointer moving
// near an edge clamps inside it.
namespace CrosshairReadout {

// Between the value's plate and the plot's right edge, and between the
// time's plate and its bottom edge: the plot's plates' 5 px to their fill
// (PlotLayer's plateInset), their 1 px edge outside it, which the rects here
// hold (at 2 px, the readouts were nearly on the screen's edge).
constexpr int kEdgeMargin = 4;
// Between the value's plate and the time's, and a scale along the right edge.
constexpr int kGap = 4;
// Between the value's plate and the corner's controls beside it: a gap
// between groups, so that where a lane is too short for the value to go
// under the corner, it doesn't read as one more plate in the corner's row,
// as it did ViewCorner::kLabelGap from it.
constexpr int kCornerGap = 16;
constexpr int kRadius = 3;

// The lines' dashes: how long, in logical pixels, and how far apart.
constexpr double kDash = 4.0;
constexpr double kDashGap = 3.0;

// How near the playhead, in logical pixels, the crosshair's vertical line
// gives way to it: the playhead is drawn above the crosshair, so that a seek
// on the plot, which lands it under the pointer, shows. Its time stays.
constexpr double kYieldsWithin = 1.0;

// What is between the dashes, over data as light as they are, such as the
// waveform's fill or a bright harmonic, where they were lost (1.4:1): the
// plot's floor at `opacity`, so nothing on the floor itself, and the line no
// thicker.
inline QColor dashGapColor(const QColor &floor, double opacity) {
    QColor gap = floor;
    gap.setAlphaF(static_cast<float>(floor.alphaF() * opacity));
    return gap;
}

// Sets `pen`'s pattern for the dashes, or with `gaps` for what is between
// them, with `unit` of its widths to a logical pixel.
inline void setDashes(QPen &pen, bool gaps, double unit = 1.0) {
    const double on = (gaps ? kDashGap : kDash) * unit;
    const double off = (gaps ? kDash : kDashGap) * unit;
    pen.setDashPattern({on, off});
    pen.setDashOffset(gaps ? on : 0.0);
}

// A plate's colours. A plate with an edge has it 1 px wide around its fill,
// as PlotPlate draws one.
struct Colors {
    QColor fill;
    QColor text;
    QColor edge{Qt::transparent};

    bool hasEdge() const { return edge.alpha() > 0; }
};

// The application's font's size, in points.
inline double applicationPoints() {
    const QFont font;
    return font.pointSizeF() > 0.0 ? font.pointSizeF() : font.pixelSize() * 0.75;
}

// How much larger than at the default 10 pt what goes with bound text is
// drawn: UiPalette.fontScale.
inline double fontScale() {
    return std::max(1.0, applicationPoints() / 10.0);
}

// `textScale` below is how much larger than the window's a plot's text is:
// in a viewer over the whole screen, its strip's type scale
// (UiPalette.fullscreenTextScale), as its lane heads' labels; 1 elsewhere.

// Between a plate's edge and its text at either side: a lane label's, 6 px
// at the default font (UiPalette.scaledSize(6)), larger with a larger one
// and with its text's scale.
inline int padding(double textScale = 1.0) {
    return std::max(6, static_cast<int>(std::lround(6.0 * fontScale() * textScale)));
}

// The readout's text: the application's font at the small size of the
// design (UiPalette.smallPointSize), as the plot's lane labels, so it grows
// with a larger font: 12 px at the default 10 pt, 14 px on a scale of 1.2.
// It is sized in pixels, at Qt's logical 96 dpi as QML's text is, so the
// plates it is measured for and the images it is drawn in agree whatever
// the device pixel ratio.
inline QFont font(double textScale = 1.0) {
    QFont font;
    font.setPixelSize(std::max(
        1, static_cast<int>(std::lround(applicationPoints() * 0.9 * textScale * 96.0 / 72.0))));
    return font;
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

// How tall a plate's fill is for text `textHeight` tall: as a lane label's
// (PlotPlate.labelHeight), its capitals in its font at `textScale` and the
// room around them, 20 px at the default font; never less than the text's
// line.
inline int plateHeight(int textHeight, double textScale = 1.0) {
    QFont label = font(textScale);
    label.setWeight(QFont::DemiBold);
    // Measured once a font: a crosshair lays its plates out every frame.
    thread_local WidestWidth capitals;
    const int capitalHeight = capitals.get(label, 0.0, [&label](const QFontMetrics &) {
        return static_cast<int>(std::ceil(QFontMetricsF(label).tightBoundingRect(QStringLiteral("E")).height()));
    });
    const int labelHeight = capitalHeight + 2 * static_cast<int>(std::lround(4.0 * fontScale()));
    return std::max({20, labelHeight, textHeight});
}

// The size of a plate for text `textWidth` wide and `textHeight` tall, in
// the readout's font at `textScale`.
inline QSize plateSize(int textWidth, int textHeight, const Colors &colors, double textScale = 1.0) {
    const int edge = colors.hasEdge() ? 2 : 0;
    return {textWidth + 2 * padding(textScale) + edge, plateHeight(textHeight, textScale) + edge};
}

inline QSize plateSize(int textWidth, const QFontMetrics &metrics, const Colors &colors,
                       double textScale = 1.0) {
    return plateSize(textWidth, metrics.height(), colors, textScale);
}

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
//   controls, kCornerGap from them (see ViewCorner). Where the lane is tall enough, it stays
//   above the time's.
// - The time's plate is centred on the vertical line at the bottom edge,
//   kEdgeMargin in from the plot's ends as from that edge. It
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
    int timeLeft = kEdgeMargin;
    int timeRight = width - kEdgeMargin;
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
            widestHome, kEdgeMargin, valueRoom, valuePlate.height(), width, reservedCorner, kCornerGap);
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
                    home, top, valuePlate.width(), valuePlate.height(), width, reservedCorner,
                    kCornerGap)),
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

// Draws a plate at `rect`, plateSize's for `text` in `font`, with the text
// centred in it, as a lane label's is.
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
    painter.drawText(QPointF(fill.left() + std::floor((fill.width() - metrics.horizontalAdvance(text)) / 2.0),
                             fill.top() + std::floor((fill.height() - metrics.height()) / 2.0) + metrics.ascent()),
                     text);
    painter.restore();
}

// A readout's number with `decimals` decimals, its minus the typographic
// minus sign (U+2212), as the plots' scale labels: "−12.5", "0.0", "3.0".
// A value that rounds to zero has no sign.
inline QString signedNumber(double value, int decimals) {
    const double scale = std::pow(10.0, decimals);
    const double rounded = std::round(value * scale) / scale;
    const QString digits = QString::number(std::abs(rounded), 'f', decimals);
    return rounded < 0.0 ? QStringLiteral("\u2212") + digits : digits;
}

// The crosshair's time, "1:46.0", with `decimals` decimals, 1 to 3.
inline QString formatCrosshairTime(double seconds, int decimals) {
    decimals = std::clamp(decimals, 1, 3);
    const qint64 unit = decimals == 1 ? 10 : (decimals == 2 ? 100 : 1000);
    // Halfway rounds up, also a rounding error short of it: a pan works out
    // the one time it holds afresh, from another start, on each move.
    constexpr double roundingError = 1e-6;
    const qint64 ticks = std::llround(
        std::max(0.0, seconds) * static_cast<double>(unit) + roundingError);
    const qint64 whole = ticks / unit;
    const qint64 hours = whole / 3600;
    const QString rest = QStringLiteral("%1.%2")
        .arg(whole % 60, 2, 10, QLatin1Char('0'))
        .arg(ticks % unit, decimals, 10, QLatin1Char('0'));
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3").arg(hours).arg((whole / 60) % 60, 2, 10, QLatin1Char('0')).arg(rest);
    }
    return QStringLiteral("%1:%2").arg(whole / 60).arg(rest);
}

// Its decimals, one rule for both plots: one at the default zoom; zoomed in,
// as many as tell a column of `secondsPerColumn` from the next, up to
// milliseconds. A column is what the plot resolves: the waveform's device
// pixel, and the spectrogram's device pixel or its analysis column,
// whichever is longer.
inline int crosshairTimeDecimals(double secondsPerColumn, double zoomLevel) {
    if (zoomLevel <= 1.0001 || !(secondsPerColumn > 0.0)) return 1;
    return std::clamp(static_cast<int>(std::ceil(-std::log10(secondsPerColumn) - 1e-9)), 1, 3);
}

// The level of silence: "−∞".
inline QString minusInfinity() {
    return QStringLiteral("\u2212\u221e");
}

} // namespace CrosshairReadout
