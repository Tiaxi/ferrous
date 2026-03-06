#include "SpectrogramItem.h"

#include <QMutexLocker>
#include <QPainter>
#include <QQuickWindow>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace {
constexpr double kMinFreqHz = 25.0;
constexpr int kMaxPendingColumns = 512;
constexpr int kPendingBacklogTarget = 48;
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
    // Keep stable Image render path by default; allow FBO only via explicit opt-in.
    const bool useFboTarget = qEnvironmentVariableIsSet("FERROUS_UI_PAINT_FBO");
    if (useFboTarget) {
        setRenderTarget(QQuickPaintedItem::FramebufferObject);
    }
    m_forceFpsOverlay = qEnvironmentVariableIsSet("FERROUS_UI_SHOW_FPS");
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    m_forceFpsOverlay = m_forceFpsOverlay
        || qEnvironmentVariableIsSet("FERROUS_PROFILE_UI")
        || qEnvironmentVariableIsSet("FERROUS_PROFILE");
#endif
    m_showFpsOverlay = m_forceFpsOverlay;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    m_profileEnabled = qEnvironmentVariableIsSet("FERROUS_PROFILE_UI")
        || qEnvironmentVariableIsSet("FERROUS_PROFILE");
    if (m_profileEnabled) {
        m_profileLast = std::chrono::steady_clock::now();
    }
#endif
    rebuildPalette();
    connect(this, &QQuickItem::windowChanged, this, &SpectrogramItem::bindWindowFpsTracking);
    bindWindowFpsTracking(window());
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

bool SpectrogramItem::showFpsOverlay() const {
    return m_showFpsOverlay;
}

void SpectrogramItem::setShowFpsOverlay(bool value) {
    const bool next = value || m_forceFpsOverlay;
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_showFpsOverlay == next) {
            return;
        }
        m_showFpsOverlay = next;
    }
    emit showFpsOverlayChanged();
    bindWindowFpsTracking(window());
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
    m_pendingColumns.clear();
    m_pendingPhase = 0.0;
    m_rowRateInitialized = false;
    m_estimatedRowsPerSecond = 0.0;
    m_animationTickInitialized = false;
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

    int rowsAdded = 0;
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
        m_pendingColumns.emplace_back(std::move(mapped));
        rowsAdded++;
    }

    while (static_cast<int>(m_pendingColumns.size()) > kMaxPendingColumns) {
        m_pendingColumns.pop_front();
    }
    if (rowsAdded <= 0) {
        return;
    }
    noteIncomingRowsLocked(rowsAdded);
    if (m_columns.empty()) {
        consumePendingColumnsLocked(1);
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
        m_pendingColumns.emplace_back(std::move(col));
        appended++;
    }
    while (static_cast<int>(m_pendingColumns.size()) > kMaxPendingColumns) {
        m_pendingColumns.pop_front();
    }
    if (appended <= 0) {
        return;
    }
    noteIncomingRowsLocked(appended);
    if (m_columns.empty()) {
        consumePendingColumnsLocked(1);
    }
    update();
}

void SpectrogramItem::paint(QPainter *painter) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const auto paint_start = std::chrono::steady_clock::now();
#endif
    QMutexLocker lock(&m_stateMutex);
    const int w = std::max(1, static_cast<int>(std::floor(width())));
    const int h = std::max(1, static_cast<int>(std::floor(height())));

    painter->fillRect(QRect(0, 0, w, h), QColor(0x0b, 0x0b, 0x0f));
    if (!m_columns.empty() && m_binsPerColumn > 0) {
        ensureCanvas(w, h);
        if (!m_canvas.isNull()) {
            const int drawCols = std::min(m_canvasFilledCols, m_canvas.width());
            if (drawCols > 0) {
                const int srcStart = (m_canvasWriteX - drawCols + m_canvas.width()) % m_canvas.width();
                const double scrollOffset = std::clamp(m_pendingPhase, 0.0, 0.999);
                const double drawX = static_cast<double>(w - drawCols) - scrollOffset;
                const int firstWidth = std::min(m_canvas.width() - srcStart, drawCols);
                const QRectF targetFirst(drawX, 0.0, static_cast<double>(firstWidth), m_canvas.height());
                const QRect sourceFirst(srcStart, 0, firstWidth, m_canvas.height());
                painter->drawImage(targetFirst, m_canvas, sourceFirst);
                const int remaining = drawCols - firstWidth;
                if (remaining > 0) {
                    const QRectF targetSecond(
                        drawX + static_cast<double>(firstWidth),
                        0.0,
                        static_cast<double>(remaining),
                        m_canvas.height());
                    const QRect sourceSecond(0, 0, remaining, m_canvas.height());
                    painter->drawImage(targetSecond, m_canvas, sourceSecond);
                }
                if (scrollOffset > 0.0 && m_canvasFilledCols > 0) {
                    const int latestX = (m_canvasWriteX - 1 + m_canvas.width()) % m_canvas.width();
                    const QRect sourceLatest(latestX, 0, 1, m_canvas.height());
                    const QRectF targetLatest(
                        static_cast<double>(w) - scrollOffset,
                        0.0,
                        scrollOffset,
                        m_canvas.height());
                    painter->drawImage(targetLatest, m_canvas, sourceLatest);
                }
            }
        }
    }

    drawFpsOverlay(painter);

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileEnabled) {
        const auto paint_end = std::chrono::steady_clock::now();
        m_profilePaints += 1;
        m_profilePaintMs += std::chrono::duration<double, std::milli>(paint_end - paint_start).count();
        const double elapsed = std::chrono::duration<double>(paint_end - m_profileLast).count();
        if (elapsed >= 1.0) {
            std::fprintf(
                stderr,
                "[ui-spectrogram] paints/s=%llu paint_ms/s=%.2f avg_ms=%.3f cols=%zu bins=%d\n",
                static_cast<unsigned long long>(m_profilePaints),
                m_profilePaintMs,
                m_profilePaints > 0 ? (m_profilePaintMs / static_cast<double>(m_profilePaints)) : 0.0,
                static_cast<size_t>(m_columns.size()),
                m_binsPerColumn);
            m_profileLast = paint_end;
            m_profilePaints = 0;
            m_profilePaintMs = 0.0;
        }
    }
#endif
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
    m_canvasWriteX = 0;
    m_canvasFilledCols = 0;
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
    m_canvasWriteX = 0;
    m_canvasFilledCols = 0;
    const int cols = m_canvas.width();
    if (m_columns.empty() || cols <= 0) {
        m_canvasDirty = false;
        return;
    }
    const int available = static_cast<int>(m_columns.size());
    const int drawCols = std::min(cols, available);
    const int start = available - drawCols;
    for (int i = 0; i < drawCols; ++i) {
        drawColumnAt(m_canvasWriteX, m_columns[static_cast<size_t>(start + i)]);
        m_canvasWriteX = (m_canvasWriteX + 1) % cols;
        m_canvasFilledCols = std::min(cols, m_canvasFilledCols + 1);
    }
    m_canvasDirty = false;
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

bool SpectrogramItem::consumePendingColumnsLocked(int requested) {
    if (requested <= 0 || m_pendingColumns.empty()) {
        return false;
    }
    const int toConsume = std::min(requested, static_cast<int>(m_pendingColumns.size()));
    if (toConsume <= 0) {
        return false;
    }

    const int w = std::max(1, static_cast<int>(std::floor(width())));
    const int h = std::max(1, static_cast<int>(std::floor(height())));
    ensureCanvas(w, h);
    if (!m_canvas.isNull() && m_canvasDirty) {
        rebuildCanvasFromColumns();
    }

    bool consumed = false;
    for (int i = 0; i < toConsume; ++i) {
        std::vector<quint8> col = std::move(m_pendingColumns.front());
        m_pendingColumns.pop_front();
        if (static_cast<int>(col.size()) != m_binsPerColumn) {
            continue;
        }
        m_columns.emplace_back(std::move(col));
        while (static_cast<int>(m_columns.size()) > m_maxColumns) {
            m_columns.pop_front();
        }
        if (!m_canvas.isNull()) {
            drawColumnAt(m_canvasWriteX, m_columns.back());
            m_canvasWriteX = (m_canvasWriteX + 1) % m_canvas.width();
            m_canvasFilledCols = std::min(m_canvas.width(), m_canvasFilledCols + 1);
        }
        consumed = true;
    }
    return consumed;
}

bool SpectrogramItem::advanceAnimationLocked(double elapsedSeconds) {
    double dt = elapsedSeconds;
    if (!std::isfinite(dt) || dt <= 0.0 || dt > 0.25) {
        const double fallbackFps = m_fpsValue > 0 ? static_cast<double>(m_fpsValue) : 60.0;
        dt = 1.0 / std::max(30.0, fallbackFps);
    }

    double rowsPerSecond = m_estimatedRowsPerSecond;
    if (!m_rowRateInitialized || !std::isfinite(rowsPerSecond) || rowsPerSecond <= 1.0) {
        if (m_pendingPhase > 0.0) {
            m_pendingPhase = 0.0;
            return true;
        }
        return false;
    }
    rowsPerSecond = std::clamp(rowsPerSecond, 30.0, 400.0);

    const double prevPhase = m_pendingPhase;
    m_pendingPhase += rowsPerSecond * dt;
    const int backlog = static_cast<int>(m_pendingColumns.size());
    if (backlog > kPendingBacklogTarget) {
        m_pendingPhase += static_cast<double>(backlog - kPendingBacklogTarget) * 0.25;
    }

    bool consumed = false;
    const int ready = static_cast<int>(std::floor(m_pendingPhase));
    if (ready > 0 && backlog > 0) {
        const int consume = std::min(ready, backlog);
        consumed = consumePendingColumnsLocked(consume);
        if (consumed) {
            m_pendingPhase = std::max(0.0, m_pendingPhase - static_cast<double>(consume));
        }
    }

    if (m_pendingColumns.empty()) {
        const double idleSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - m_lastRowAppendTime).count();
        if (idleSeconds > 0.30) {
            m_pendingPhase = 0.0;
        } else {
            m_pendingPhase = std::clamp(m_pendingPhase, 0.0, 0.999);
        }
    }

    const bool phaseChanged = std::abs(m_pendingPhase - prevPhase) > 0.0001;
    return consumed || phaseChanged;
}

void SpectrogramItem::noteIncomingRowsLocked(int rowCount) {
    if (rowCount <= 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_rowRateInitialized) {
        const double elapsed = std::chrono::duration<double>(now - m_lastRowAppendTime).count();
        if (elapsed > 0.0005) {
            const double instantRate = std::clamp(
                static_cast<double>(rowCount) / elapsed,
                1.0,
                1200.0);
            constexpr double alpha = 0.20;
            m_estimatedRowsPerSecond = (alpha * instantRate) + ((1.0 - alpha) * m_estimatedRowsPerSecond);
        }
    } else {
        m_estimatedRowsPerSecond = std::clamp(static_cast<double>(rowCount) * 60.0, 30.0, 400.0);
        m_rowRateInitialized = true;
        m_pendingPhase = std::max(0.0, m_pendingPhase);
    }
    m_lastRowAppendTime = now;
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

void SpectrogramItem::bindWindowFpsTracking(QQuickWindow *window) {
    if (m_frameSwapConnection) {
        disconnect(m_frameSwapConnection);
        m_frameSwapConnection = QMetaObject::Connection{};
    }
    QMutexLocker lock(&m_stateMutex);
    m_fpsInitialized = false;
    m_fpsValue = 0;
    m_fpsAccumFrames = 0;
    m_fpsAccumSeconds = 0.0;
    m_animationTickInitialized = false;
    lock.unlock();

    if (window == nullptr) {
        return;
    }
    m_frameSwapConnection = connect(
        window,
        &QQuickWindow::frameSwapped,
        this,
        &SpectrogramItem::handleWindowFrameSwapped,
        Qt::QueuedConnection);
}

void SpectrogramItem::handleWindowFrameSwapped() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();

    bool advanced = false;
    bool pending = false;
    QMutexLocker lock(&m_stateMutex);
    double elapsed = 0.0;
    if (m_animationTickInitialized) {
        elapsed = std::chrono::duration<double>(now - m_lastAnimationTick).count();
    }
    m_lastAnimationTick = now;
    m_animationTickInitialized = true;

    const int prev = m_fpsValue;
    bool changed = false;
    if (m_showFpsOverlay) {
        updateFpsEstimateLocked();
        changed = m_fpsValue != prev;
    }
    advanced = advanceAnimationLocked(elapsed);
    pending = !m_pendingColumns.empty();
    lock.unlock();
    if (changed || advanced || pending) {
        update();
    }
}

void SpectrogramItem::updateFpsEstimateLocked() {
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
    if (!m_showFpsOverlay || !painter || m_fpsValue <= 0) {
        return;
    }

    QFont font = painter->font();
    font.setPixelSize(10);
    painter->setFont(font);
    painter->setPen(QColor(190, 190, 200, 150));
    painter->drawText(QPointF(8.0, 14.0), QStringLiteral("%1 fps").arg(m_fpsValue));
}
