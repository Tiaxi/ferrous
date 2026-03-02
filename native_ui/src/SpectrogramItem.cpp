#include "SpectrogramItem.h"

#include <QMutexLocker>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {
std::array<quint8, 3> ddbColor(double norm) {
    static const std::array<std::array<int, 3>, 7> kColors{{
        {{255, 255, 255}},
        {{255, 255, 255}},
        {{255, 247, 0}},
        {{242, 54, 0}},
        {{176, 0, 91}},
        {{48, 0, 115}},
        {{4, 1, 71}},
    }};

    const double clamped = std::clamp(norm, 0.0, 1.0);
    const double pos = (1.0 - clamped) * static_cast<double>(kColors.size() - 1);
    const int i0 = static_cast<int>(std::floor(pos));
    const int i1 = std::min<int>(kColors.size() - 1, i0 + 1);
    const double t = pos - static_cast<double>(i0);

    std::array<quint8, 3> out{};
    for (int c = 0; c < 3; ++c) {
        const double v = static_cast<double>(kColors[static_cast<size_t>(i0)][static_cast<size_t>(c)])
            + (static_cast<double>(kColors[static_cast<size_t>(i1)][static_cast<size_t>(c)])
               - static_cast<double>(kColors[static_cast<size_t>(i0)][static_cast<size_t>(c)]))
                * t;
        out[static_cast<size_t>(c)] = static_cast<quint8>(std::clamp<int>(static_cast<int>(std::lround(v)), 0, 255));
    }
    return out;
}
} // namespace

SpectrogramItem::SpectrogramItem(QQuickItem *parent)
    : QQuickPaintedItem(parent) {
    setAntialiasing(false);
    setOpaquePainting(true);
    rebuildPalette();
}

double SpectrogramItem::dbRange() const {
    return m_dbRange;
}

void SpectrogramItem::setDbRange(double value) {
    QMutexLocker lock(&m_stateMutex);
    const double clamped = std::clamp(value, 50.0, 120.0);
    if (std::abs(m_dbRange - clamped) < 0.001) {
        return;
    }
    m_dbRange = clamped;
    emit dbRangeChanged();
    update();
}

bool SpectrogramItem::logScale() const {
    return m_logScale;
}

void SpectrogramItem::setLogScale(bool value) {
    QMutexLocker lock(&m_stateMutex);
    if (m_logScale == value) {
        return;
    }
    m_logScale = value;
    emit logScaleChanged();
    invalidateMapping();
    invalidateCanvas();
    update();
}

int SpectrogramItem::sampleRateHz() const {
    return m_sampleRateHz;
}

void SpectrogramItem::setSampleRateHz(int value) {
    QMutexLocker lock(&m_stateMutex);
    const int clamped = std::max(1000, value);
    if (m_sampleRateHz == clamped) {
        return;
    }
    m_sampleRateHz = clamped;
    emit sampleRateHzChanged();
    invalidateMapping();
    invalidateCanvas();
    update();
}

int SpectrogramItem::maxColumns() const {
    return m_maxColumns;
}

void SpectrogramItem::setMaxColumns(int value) {
    QMutexLocker lock(&m_stateMutex);
    const int clamped = std::clamp(value, 128, 4096);
    if (m_maxColumns == clamped) {
        return;
    }
    m_maxColumns = clamped;
    emit maxColumnsChanged();
    while (static_cast<int>(m_columns.size()) > m_maxColumns) {
        m_columns.pop_front();
    }
    invalidateCanvas();
    update();
}

void SpectrogramItem::reset() {
    QMutexLocker lock(&m_stateMutex);
    m_columns.clear();
    m_binsPerColumn = 0;
    invalidateMapping();
    invalidateCanvas();
    update();
}

void SpectrogramItem::appendRows(const QVariantList &rows) {
    QMutexLocker lock(&m_stateMutex);
    if (rows.isEmpty()) {
        return;
    }

    bool anyAdded = false;
    for (const QVariant &rowVar : rows) {
        const QVariantList row = rowVar.toList();
        if (row.isEmpty()) {
            continue;
        }
        std::vector<quint8> mapped = rowToIntensity(row);
        if (mapped.empty()) {
            continue;
        }
        if (m_binsPerColumn <= 0) {
            m_binsPerColumn = static_cast<int>(mapped.size());
            invalidateMapping();
        }
        if (static_cast<int>(mapped.size()) != m_binsPerColumn) {
            continue;
        }
        appendColumnAndRender(std::move(mapped));
        anyAdded = true;
    }

    if (!anyAdded) {
        return;
    }

    update();
}

void SpectrogramItem::appendPackedRows(const QByteArray &packedRows, int rowCount, int binsPerRow) {
    QMutexLocker lock(&m_stateMutex);
    if (packedRows.isEmpty() || rowCount <= 0 || binsPerRow <= 0) {
        return;
    }
    const qsizetype expected = static_cast<qsizetype>(rowCount) * static_cast<qsizetype>(binsPerRow);
    if (packedRows.size() < expected) {
        return;
    }

    if (m_binsPerColumn <= 0) {
        m_binsPerColumn = binsPerRow;
        invalidateMapping();
    }
    if (m_binsPerColumn != binsPerRow) {
        return;
    }

    int appended = 0;
    const auto *src = reinterpret_cast<const quint8 *>(packedRows.constData());
    for (int r = 0; r < rowCount; ++r) {
        std::vector<quint8> col(static_cast<size_t>(binsPerRow));
        std::copy_n(
            src + static_cast<qsizetype>(r) * static_cast<qsizetype>(binsPerRow),
            binsPerRow,
            col.begin());
        m_columns.emplace_back(std::move(col));
        appended++;
    }
    while (static_cast<int>(m_columns.size()) > m_maxColumns) {
        m_columns.pop_front();
    }
    if (appended <= 0) {
        return;
    }

    const int w = std::max(1, static_cast<int>(std::floor(width())));
    const int h = std::max(1, static_cast<int>(std::floor(height())));
    ensureCanvas(w, h);
    if (m_canvas.isNull()) {
        update();
        return;
    }

    const int shift = std::min(appended, m_canvas.width());
    if (shift >= m_canvas.width()) {
        rebuildCanvasFromColumns();
    } else {
        shiftCanvasLeft(shift);
        const int sourceStart = static_cast<int>(m_columns.size()) - shift;
        const int xStart = m_canvas.width() - shift;
        for (int i = 0; i < shift; ++i) {
            drawColumnAt(xStart + i, m_columns[static_cast<size_t>(sourceStart + i)]);
        }
    }
    update();
}

void SpectrogramItem::paint(QPainter *painter) {
    QMutexLocker lock(&m_stateMutex);
    const int w = std::max(1, static_cast<int>(std::floor(width())));
    const int h = std::max(1, static_cast<int>(std::floor(height())));

    painter->fillRect(QRect(0, 0, w, h), QColor(0x0b, 0x0b, 0x0f));
    if (m_columns.empty() || m_binsPerColumn <= 0) {
        return;
    }

    ensureCanvas(w, h);
    if (m_canvas.isNull()) {
        return;
    }
    const int drawX = w - m_canvas.width();
    painter->drawImage(QPoint(drawX, 0), m_canvas);
}

void SpectrogramItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        QMutexLocker lock(&m_stateMutex);
        invalidateMapping();
        invalidateCanvas();
    }
}

void SpectrogramItem::rebuildPalette() {
    for (int i = 0; i < 256; ++i) {
        m_palette[static_cast<size_t>(i)] = ddbColor(static_cast<double>(i) / 255.0);
        const auto &rgb = m_palette[static_cast<size_t>(i)];
        m_palette32[static_cast<size_t>(i)] = qRgb(rgb[0], rgb[1], rgb[2]);
    }
}

void SpectrogramItem::invalidateMapping() {
    m_yToBin.clear();
    m_yToBinHeight = -1;
}

void SpectrogramItem::invalidateCanvas() {
    m_canvas = QImage();
    m_canvasDirty = true;
}

void SpectrogramItem::ensureMapping(int height) {
    if (height <= 0 || m_binsPerColumn <= 0) {
        return;
    }
    if (m_yToBinHeight == height && static_cast<int>(m_yToBin.size()) == height) {
        return;
    }

    m_yToBin.resize(static_cast<size_t>(height));
    if (m_logScale) {
        const double minFreq = 25.0;
        const double nyquist = std::max(0.5 * static_cast<double>(m_sampleRateHz), minFreq * 1.1);
        const double logStep = (std::log2(nyquist) - std::log2(minFreq)) / std::max(1, height);
        const double freqRes = std::max(1.0, static_cast<double>(m_sampleRateHz)
                                               / (2.0 * std::max(1, m_binsPerColumn - 1)));
        for (int y = 0; y < height; ++y) {
            const int i = height - 1 - y;
            const double freq = std::pow(2.0, static_cast<double>(i) * logStep + std::log2(minFreq));
            const int bin = static_cast<int>(std::lround(freq / freqRes));
            m_yToBin[static_cast<size_t>(y)] = std::clamp(bin, 0, m_binsPerColumn - 1);
        }
    } else {
        for (int y = 0; y < height; ++y) {
            const int i = height - 1 - y;
            const int bin = static_cast<int>(std::floor((static_cast<double>(i) / std::max(1, height - 1))
                                                        * static_cast<double>(m_binsPerColumn - 1)));
            m_yToBin[static_cast<size_t>(y)] = std::clamp(bin, 0, m_binsPerColumn - 1);
        }
    }

    m_yToBinHeight = height;
}

void SpectrogramItem::ensureCanvas(int width, int height) {
    if (width <= 0 || height <= 0 || m_binsPerColumn <= 0) {
        return;
    }

    const int cols = std::max(1, std::min(width, m_maxColumns));
    ensureMapping(height);
    if (m_canvas.width() != cols || m_canvas.height() != height || m_canvas.format() != QImage::Format_RGB32) {
        m_canvas = QImage(cols, height, QImage::Format_RGB32);
        m_canvas.fill(Qt::black);
        m_canvasDirty = true;
    }
    if (m_canvasDirty) {
        rebuildCanvasFromColumns();
    }
}

void SpectrogramItem::rebuildCanvasFromColumns() {
    if (m_canvas.isNull()) {
        return;
    }
    m_canvas.fill(Qt::black);
    const int cols = m_canvas.width();
    if (m_columns.empty() || cols <= 0) {
        m_canvasDirty = false;
        return;
    }
    const int available = static_cast<int>(m_columns.size());
    const int drawCols = std::min(cols, available);
    const int start = available - drawCols;
    const int xStart = cols - drawCols;
    for (int i = 0; i < drawCols; ++i) {
        drawColumnAt(xStart + i, m_columns[static_cast<size_t>(start + i)]);
    }
    m_canvasDirty = false;
}

void SpectrogramItem::shiftCanvasLeft(int columns) {
    if (m_canvas.isNull() || columns <= 0) {
        return;
    }
    const int cols = m_canvas.width();
    if (columns >= cols) {
        m_canvas.fill(Qt::black);
        return;
    }

    const int bytesPerPixel = static_cast<int>(sizeof(QRgb));
    const int movePixels = cols - columns;
    const int moveBytes = movePixels * bytesPerPixel;
    for (int y = 0; y < m_canvas.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(m_canvas.scanLine(y));
        std::memmove(line, line + columns, static_cast<size_t>(moveBytes));
        std::fill(line + movePixels, line + cols, qRgb(0, 0, 0));
    }
}

void SpectrogramItem::drawColumnAt(int x, const std::vector<quint8> &col) {
    if (m_canvas.isNull() || x < 0 || x >= m_canvas.width() || col.empty()) {
        return;
    }

    const int maxBin = std::max(0, m_binsPerColumn - 1);
    for (int y = 0; y < m_canvas.height(); ++y) {
        const int bin = std::clamp(m_yToBin[static_cast<size_t>(y)], 0, maxBin);
        const quint8 idx = col[static_cast<size_t>(bin)];
        auto *line = reinterpret_cast<QRgb *>(m_canvas.scanLine(y));
        line[x] = m_palette32[static_cast<size_t>(idx)];
    }
}

void SpectrogramItem::appendColumnAndRender(std::vector<quint8> &&col) {
    if (static_cast<int>(col.size()) != m_binsPerColumn) {
        return;
    }

    m_columns.emplace_back(std::move(col));
    while (static_cast<int>(m_columns.size()) > m_maxColumns) {
        m_columns.pop_front();
    }

    const int w = std::max(1, static_cast<int>(std::floor(width())));
    const int h = std::max(1, static_cast<int>(std::floor(height())));
    ensureCanvas(w, h);
    if (m_canvas.isNull()) {
        return;
    }
    shiftCanvasLeft(1);
    drawColumnAt(m_canvas.width() - 1, m_columns.back());
}

std::vector<quint8> SpectrogramItem::rowToIntensity(const QVariantList &row) const {
    std::vector<quint8> out;
    out.reserve(static_cast<size_t>(row.size()));

    const double dbRange = std::clamp(m_dbRange, 50.0, 120.0);
    const double dbScale = 10.0 / std::log(10.0);

    for (const QVariant &value : row) {
        const double v = value.toDouble();
        int idx = 0;
        if (v >= 0.0 && v <= 255.0 && std::floor(v) == v) {
            idx = static_cast<int>(v);
        } else {
            const double db = v > 0.0 ? (dbScale * std::log(v)) : -200.0;
            const double xdb = std::clamp(db + dbRange - 63.0, 0.0, dbRange);
            idx = static_cast<int>(std::lround((xdb / dbRange) * 255.0));
        }
        out.push_back(static_cast<quint8>(std::clamp(idx, 0, 255)));
    }

    return out;
}
