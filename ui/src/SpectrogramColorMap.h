// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QRgb>
#include <algorithm>
#include <cmath>
#include <numbers>

// The spectrogram's colour map and how it places a level on it, shared with
// the level meter, whose fill takes the map's colour of its level (#373 H7
// P11), so that the meter reads as part of the plot.
namespace SpectrogramColorMap {

// The colour at `x`, from 0 (silence, black) to 1 (full scale, white).
// SoX's default spectrogram palette (spectrogram.c, perm=0): procedural
// sine and linear ramps from black through dark blue, purple, magenta, red,
// orange and yellow to white, as Adobe Audition 3.0's spectral display.
inline QRgb colorAt(double x) {
    x = std::clamp(x, 0.0, 1.0);
    constexpr double kQuarterTurn = std::numbers::pi / 2.0;
    // Red: a sine ramp from 0.13 to 0.73, then saturated.
    const double red = x < 0.13 ? 0.0 : (x < 0.73 ? std::sin((x - 0.13) / 0.60 * kQuarterTurn) : 1.0);
    // Green: a sine ramp from 0.60 to 0.91, then saturated.
    const double green = x < 0.60 ? 0.0 : (x < 0.91 ? std::sin((x - 0.60) / 0.31 * kQuarterTurn) : 1.0);
    // Blue: a half sine up to 0.60, none to 0.78, then a linear ramp.
    const double blue = x < 0.60
        ? 0.5 * std::sin(x / 0.60 * std::numbers::pi)
        : (x < 0.78 ? 0.0 : (x - 0.78) / 0.22);
    const auto channel = [](double value) {
        return std::clamp(static_cast<int>(0.5 + 255.0 * value), 0, 255);
    };
    return qRgb(channel(red), channel(green), channel(blue));
}

// The view's range in dB below full scale, as Preferences set it, within
// the limits the spectrogram draws with.
inline double clampedDbRange(double dbRange) {
    return std::clamp(dbRange, 50.0, 150.0);
}

// Where a level of `db` dBFS falls on the map, from 0 to 1, in a view of
// `dbRange` dB: linear in dB over the range, then curved so that the quiet
// part of the range takes less of the map.
inline double position(double db, double dbRange) {
    const double range = clampedDbRange(dbRange);
    if (!std::isfinite(db)) {
        return 0.0;
    }
    const double normalized = std::clamp(db + range, 0.0, range) / range;
    constexpr double kCurve = 1.3;
    return normalized > 0.0 ? std::pow(normalized, 2.0 - kCurve * normalized) : 0.0;
}

// The colour of a level of `db` dBFS in a view of `dbRange` dB.
inline QRgb colorOfLevel(double db, double dbRange) {
    return colorAt(position(db, dbRange));
}

} // namespace SpectrogramColorMap
