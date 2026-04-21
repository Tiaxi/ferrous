// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArrayView>

namespace SpectrogramGpu {

enum class DebugMode {
    Full,
    TextureNode,
};

struct ColumnSpan {
    int firstColumn{-1};
    int lastColumn{-1};
    double interpolationT{0.0};
    bool interpolate{false};
};

DebugMode parseDebugModeForTests(QByteArrayView raw);
DebugMode currentDebugMode();
const char *debugModeName(DebugMode mode);

ColumnSpan resolveColumnSpanForPixel(
    int pixel_x,
    int source_columns,
    double effective_zoom);

} // namespace SpectrogramGpu
