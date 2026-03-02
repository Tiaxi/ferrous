#include "SpectrogramItem.h"

#include <QMutexLocker>
#include <QPainter>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {
constexpr double kMinFreqHz = 25.0;
constexpr std::array<std::array<int, 3>, 7> kGradientColors16{{
    {{65535, 65535, 65535}},
    {{65535, 65535, 65535}},
    {{65535, 63479, 0}},
    {{62194, 13878, 0}},
    {{45232, 0, 23387}},
    {{12336, 0, 29555}},
    {{1027, 256, 18247}},
}};

double linearInterpolate(double y1, double y2, double mu) {
    return y1 * (1.0 - mu) + y2 * mu;
}

quint8 spectrogramGetValue(const std::vector<quint8> &row, int start, int end) {
    if (row.empty()) {
        return 0;
    }
    const int last = static_cast<int>(row.size()) - 1;
    const int endClamped = std::clamp(end, 0, last);
    const int startClamped = std::clamp(start, 0, endClamped);
    if (startClamped >= endClamped) {
        return row[static_cast<size_t>(endClamped)];
    }

    quint8 value = 0;
    for (int i = startClamped; i < endClamped; ++i) {
        value = std::max(value, row[static_cast<size_t>(i)]);
    }
    return value;
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
    if (!m_columns.empty() && m_binsPerColumn > 0) {
        ensureCanvas(w, h);
        if (!m_canvas.isNull()) {
            const int drawX = w - m_canvas.width();
            painter->drawImage(QPoint(drawX, 0), m_canvas);
        }
    }

    updateFpsEstimate();
    drawFpsOverlay(painter);
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
    constexpr double scale = 255.0 / 65535.0;
    constexpr int numSegments = static_cast<int>(kGradientColors16.size()) - 1;
    for (int i = 0; i < kGradientTableSize; ++i) {
        const double position = static_cast<double>(i) / static_cast<double>(kGradientTableSize);
        const double m = static_cast<double>(numSegments) * position;
        const int n = std::clamp(static_cast<int>(std::floor(m)), 0, numSegments);
        const double f = std::clamp(m - static_cast<double>(n), 0.0, 1.0);
        const int n1 = std::min(numSegments, n + 1);

        const int r = static_cast<int>(
            (static_cast<double>(kGradientColors16[static_cast<size_t>(n)][0]) * scale)
            + f * ((static_cast<double>(kGradientColors16[static_cast<size_t>(n1)][0]) * scale)
                   - (static_cast<double>(kGradientColors16[static_cast<size_t>(n)][0]) * scale)));
        const int g = static_cast<int>(
            (static_cast<double>(kGradientColors16[static_cast<size_t>(n)][1]) * scale)
            + f * ((static_cast<double>(kGradientColors16[static_cast<size_t>(n1)][1]) * scale)
                   - (static_cast<double>(kGradientColors16[static_cast<size_t>(n)][1]) * scale)));
        const int b = static_cast<int>(
            (static_cast<double>(kGradientColors16[static_cast<size_t>(n)][2]) * scale)
            + f * ((static_cast<double>(kGradientColors16[static_cast<size_t>(n1)][2]) * scale)
                   - (static_cast<double>(kGradientColors16[static_cast<size_t>(n)][2]) * scale)));

        m_palette32[static_cast<size_t>(i)] = qRgb(
            std::clamp(r, 0, 255),
            std::clamp(g, 0, 255),
            std::clamp(b, 0, 255));
    }
}

void SpectrogramItem::invalidateMapping() {
    m_iToBin.clear();
    m_mappingHeight = -1;
    m_lowResEnd = 0;
}

void SpectrogramItem::invalidateCanvas() {
    m_canvas = QImage();
    m_canvasDirty = true;
}

void SpectrogramItem::ensureMapping(int height) {
    if (height <= 0 || m_binsPerColumn <= 0) {
        return;
    }
    if (m_mappingHeight == height && static_cast<int>(m_iToBin.size()) == height) {
        return;
    }

    m_iToBin.resize(static_cast<size_t>(height));
    m_lowResEnd = 0;
    if (m_logScale) {
        const double nyquist = std::max(0.5 * static_cast<double>(m_sampleRateHz), kMinFreqHz * 1.1);
        const double logStep = (std::log2(nyquist) - std::log2(kMinFreqHz)) / std::max(1, height);
        const double freqRes = std::max(1.0, static_cast<double>(m_sampleRateHz)
                                               / (2.0 * std::max(1, m_binsPerColumn - 1)));
        for (int i = 0; i < height; ++i) {
            const double freq = std::pow(2.0, static_cast<double>(i) * logStep + std::log2(kMinFreqHz));
            const int bin = std::clamp(static_cast<int>(std::lround(freq / freqRes)), 0, m_binsPerColumn - 1);
            m_iToBin[static_cast<size_t>(i)] = bin;
            if (i > 0 && m_iToBin[static_cast<size_t>(i - 1)] == bin) {
                m_lowResEnd = i;
            }
        }
    } else {
        for (int i = 0; i < height; ++i) {
            const int bin = static_cast<int>(std::floor((static_cast<double>(i) / std::max(1, height - 1))
                                                        * static_cast<double>(m_binsPerColumn - 1)));
            m_iToBin[static_cast<size_t>(i)] = std::clamp(bin, 0, m_binsPerColumn - 1);
        }
    }

    m_mappingHeight = height;
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

    const int height = m_canvas.height();
    const int srcBins = std::max(1, m_binsPerColumn);
    const int maxBin = srcBins - 1;
    const int ratio = std::clamp(
        static_cast<int>(std::lround(static_cast<double>(srcBins) / static_cast<double>(std::max(1, height)))),
        0,
        1023);

    for (int y = 0; y < height; ++y) {
        const int i = height - 1 - y;

        int bin0 = 0;
        int bin1 = 0;
        int bin2 = 0;
        if (m_logScale && static_cast<int>(m_iToBin.size()) == height) {
            bin0 = m_iToBin[static_cast<size_t>(std::clamp(i - 1, 0, height - 1))];
            bin1 = m_iToBin[static_cast<size_t>(i)];
            bin2 = m_iToBin[static_cast<size_t>(std::clamp(i + 1, 0, height - 1))];
        } else {
            bin0 = (i - 1) * ratio;
            bin1 = i * ratio;
            bin2 = (i + 1) * ratio;
        }

        int index0 = bin0 + static_cast<int>(std::lround(static_cast<double>(bin1 - bin0) / 2.0));
        if (index0 == bin0) {
            index0 = bin1;
        }
        int index1 = bin1 + static_cast<int>(std::lround(static_cast<double>(bin2 - bin1) / 2.0));
        if (index1 == bin2) {
            index1 = bin1;
        }
        index0 = std::clamp(index0, 0, maxBin);
        index1 = std::clamp(index1, 0, maxBin);

        double intensity = static_cast<double>(spectrogramGetValue(col, index0, index1));

        if (m_logScale && static_cast<int>(m_iToBin.size()) == height && i <= m_lowResEnd) {
            const int target = m_iToBin[static_cast<size_t>(i)];
            int j = 0;
            while (i + j < height && m_iToBin[static_cast<size_t>(i + j)] == target) {
                ++j;
            }

            const int nextI = std::min(i + j, height - 1);
            const int nextBin = std::clamp(m_iToBin[static_cast<size_t>(nextI)], 0, maxBin);
            const double v0 = intensity;
            const double v1 = static_cast<double>(col[static_cast<size_t>(nextBin)]);

            int k = 0;
            int span = j;
            while (i + k >= 0 && m_iToBin[static_cast<size_t>(i + k)] == target) {
                ++span;
                --k;
            }

            if (span > 1) {
                const double mu = (1.0 / static_cast<double>(span - 1))
                    * static_cast<double>((-k) - 1);
                intensity = linearInterpolate(v0, v1, std::clamp(mu, 0.0, 1.0));
            }
        }

        int paletteIndex = kGradientTableSize
            - static_cast<int>(std::lround((static_cast<double>(kGradientTableSize) / 255.0) * intensity));
        paletteIndex = std::clamp(paletteIndex, 0, kGradientTableSize - 1);
        auto *line = reinterpret_cast<QRgb *>(m_canvas.scanLine(y));
        line[x] = m_palette32[static_cast<size_t>(paletteIndex)];
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

void SpectrogramItem::updateFpsEstimate() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    if (!m_fpsInitialized) {
        m_fpsInitialized = true;
        m_lastFrameTime = now;
        m_fpsValue = 0;
        m_fpsAccumFrames = 0;
        m_fpsAccumSeconds = 0.0;
        return;
    }

    const double elapsed = std::chrono::duration<double>(now - m_lastFrameTime).count();
    m_lastFrameTime = now;
    if (elapsed <= 0.0) {
        return;
    }

    m_fpsAccumFrames += 1;
    m_fpsAccumSeconds += elapsed;
    if (m_fpsAccumSeconds < 0.20) {
        return;
    }

    const double fps = static_cast<double>(m_fpsAccumFrames) / m_fpsAccumSeconds;
    m_fpsValue = std::clamp(static_cast<int>(std::lround(fps)), 0, 999);
    m_fpsAccumFrames = 0;
    m_fpsAccumSeconds = 0.0;
}

void SpectrogramItem::drawFpsOverlay(QPainter *painter) const {
    if (!painter || m_fpsValue <= 0) {
        return;
    }

    QFont font = painter->font();
    font.setPixelSize(10);
    painter->setFont(font);
    painter->setPen(QColor(190, 190, 200, 150));
    painter->drawText(QPointF(8.0, 14.0), QStringLiteral("%1 fps").arg(m_fpsValue));
}
