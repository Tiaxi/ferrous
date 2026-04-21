// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramRenderUtils.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>

namespace SpectrogramGpu {

namespace {

DebugMode parseDebugMode(QByteArrayView raw) {
    const QByteArray normalized = QByteArray(raw).trimmed().toLower();
    if (normalized == "texture-node" || normalized == "texture") {
        return DebugMode::TextureNode;
    }
    return DebugMode::Full;
}

} // namespace

DebugMode parseDebugModeForTests(QByteArrayView raw) {
    return parseDebugMode(raw);
}

DebugMode currentDebugMode() {
    return parseDebugMode(qgetenv("FERROUS_GPU_DEBUG_MODE"));
}

const char *debugModeName(DebugMode mode) {
    switch (mode) {
    case DebugMode::Full:
        return "full";
    case DebugMode::TextureNode:
        return "texture-node";
    }
    return "full";
}

ColumnSpan resolveColumnSpanForPixel(
    int pixel_x,
    int source_columns,
    double effective_zoom) {
    if (pixel_x < 0 || source_columns <= 0) {
        return {};
    }

    const double columns_per_pixel = 1.0 / std::max(effective_zoom, 0.001);
    const double range_start = static_cast<double>(pixel_x) * columns_per_pixel;
    const double range_end = static_cast<double>(pixel_x + 1) * columns_per_pixel;
    const int first_column = std::min(
        static_cast<int>(std::floor(range_start)),
        source_columns - 1);
    const int last_column = std::min(
        static_cast<int>(std::ceil(range_end - 1.0)),
        source_columns - 1);
    const double interpolation_t = range_start - std::floor(range_start);
    const bool interpolate =
        effective_zoom > 1.001
        && interpolation_t > 0.001
        && first_column + 1 < source_columns;

    return ColumnSpan{
        .firstColumn = first_column,
        .lastColumn = interpolate ? first_column + 1 : std::max(first_column, last_column),
        .interpolationT = interpolate ? interpolation_t : 0.0,
        .interpolate = interpolate,
    };
}

} // namespace SpectrogramGpu
