// SPDX-License-Identifier: GPL-3.0-or-later

#include "WaveformEditorItem.h"

#include "FerrousBridgeFfi.h"

#include <QFontMetrics>
#include <QFutureWatcher>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
constexpr int kWindowHeaderBytes = 36;
constexpr double kZoomStep = 1.25;
constexpr QColor kBackground(5, 9, 7);
constexpr QColor kWaveform(54, 225, 161);
constexpr QColor kMutedWaveform(74, 104, 92);
constexpr QColor kGrid(20, 82, 49);
constexpr QColor kCenterLine(108, 42, 45);
constexpr QColor kPlayhead(234, 83, 83);

double readF64(const char *data) {
    quint64 bits = qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(data));
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float readF32(const char *data) {
    quint32 bits = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data));
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

QString formatTime(double seconds, double span) {
    seconds = std::max(0.0, seconds);
    const int minutes = static_cast<int>(seconds) / 60;
    const double remainder = seconds - static_cast<double>(minutes * 60);
    if (span < 0.1) {
        return QStringLiteral("%1:%2").arg(minutes).arg(remainder, 6, 'f', 3, QLatin1Char('0'));
    }
    if (span < 10.0) {
        return QStringLiteral("%1:%2").arg(minutes).arg(remainder, 5, 'f', 2, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(static_cast<int>(remainder), 2, 10, QLatin1Char('0'));
}

double selectTimeInterval(double span, int width) {
    static constexpr double candidates[] = {
        0.00001, 0.00002, 0.00005, 0.0001, 0.0002, 0.0005,
        0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
        1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0
    };
    const double target = span * 82.0 / static_cast<double>(std::max(1, width));
    for (double candidate : candidates) {
        if (candidate >= target) {
            return candidate;
        }
    }
    return candidates[std::size(candidates) - 1];
}
}

WaveformEditorItem::WaveformEditorItem(QQuickItem *parent)
    : QQuickPaintedItem(parent) {
    setAntialiasing(false);
    setOpaquePainting(true);
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::RightButton | Qt::MiddleButton);
    m_requestTimer.setSingleShot(true);
    m_requestTimer.setInterval(90);
    connect(&m_requestTimer, &QTimer::timeout, this, &WaveformEditorItem::requestDetailWindow);
}

QString WaveformEditorItem::sourcePath() const { QMutexLocker lock(&m_stateMutex); return m_sourcePath; }
QByteArray WaveformEditorItem::overviewData() const { QMutexLocker lock(&m_stateMutex); return m_overviewData; }
double WaveformEditorItem::positionSeconds() const { QMutexLocker lock(&m_stateMutex); return m_positionSeconds; }
double WaveformEditorItem::durationSeconds() const { QMutexLocker lock(&m_stateMutex); return m_durationSeconds; }
double WaveformEditorItem::zoomLevel() const { QMutexLocker lock(&m_stateMutex); return m_zoomLevel; }
bool WaveformEditorItem::zoomEnabled() const { QMutexLocker lock(&m_stateMutex); return m_zoomEnabled; }
bool WaveformEditorItem::gridEnabled() const { QMutexLocker lock(&m_stateMutex); return m_gridEnabled; }
bool WaveformEditorItem::crosshairEnabled() const { QMutexLocker lock(&m_stateMutex); return m_crosshairEnabled; }
int WaveformEditorItem::viewMode() const { QMutexLocker lock(&m_stateMutex); return m_viewMode; }
qulonglong WaveformEditorItem::mutedChannelsMask() const { QMutexLocker lock(&m_stateMutex); return m_mutedChannelsMask; }
int WaveformEditorItem::soloedChannel() const { QMutexLocker lock(&m_stateMutex); return m_soloedChannel; }
int WaveformEditorItem::channelCountHint() const { QMutexLocker lock(&m_stateMutex); return m_channelCountHint; }
int WaveformEditorItem::channelCount() const { QMutexLocker lock(&m_stateMutex); return displayedChannelCountLocked(); }
int WaveformEditorItem::sampleRateHz() const { QMutexLocker lock(&m_stateMutex); return m_sampleRateHz; }
bool WaveformEditorItem::samplePointsVisible() const { QMutexLocker lock(&m_stateMutex); return samplePointsVisibleLocked(); }

void WaveformEditorItem::setSourcePath(const QString &value) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_sourcePath == value) return;
        m_sourcePath = value;
        ++m_requestGeneration;
        clearPendingRequestLocked();
        clearDetailLocked();
        invalidateCacheLocked();
    }
    emit sourcePathChanged();
    emit channelCountChanged();
    emit sampleRateHzChanged();
    emit samplePointsVisibleChanged();
    scheduleDetailRequest();
    update();
}

void WaveformEditorItem::setOverviewData(const QByteArray &value) {
    { QMutexLocker lock(&m_stateMutex); if (m_overviewData == value) return; m_overviewData = value; invalidateCacheLocked(); }
    emit overviewDataChanged(); update();
}

void WaveformEditorItem::setPositionSeconds(double value) {
    bool request = false;
    bool cacheNeedsUpdate = false;
    {
        QMutexLocker lock(&m_stateMutex);
        value = std::clamp(value, 0.0, std::max(0.0, m_durationSeconds));
        if (std::abs(m_positionSeconds - value) < 0.0001) return;
        const auto previousRange = visibleRangeLocked();
        m_positionSeconds = value;
        const auto [start, end] = visibleRangeLocked();
        request = !detailOrPendingRequestCoversLocked(start, end);
        const bool rangeMoved = std::abs(previousRange.first - start) > 0.0001
            || std::abs(previousRange.second - end) > 0.0001;
        cacheNeedsUpdate = rangeMoved
            && (m_detail.pointCount == 0
                || start < m_cacheStartSeconds
                || end > m_cacheEndSeconds);
        if (cacheNeedsUpdate) invalidateCacheLocked();
    }
    emit positionSecondsChanged();
    if (request) scheduleDetailRequest();
    update();
}

void WaveformEditorItem::setDurationSeconds(double value) {
    { QMutexLocker lock(&m_stateMutex); value = std::max(0.0, value); if (std::abs(m_durationSeconds - value) < 0.0001) return; m_durationSeconds = value; m_zoomLevel = std::clamp(m_zoomLevel, 1.0, maximumZoomLevelLocked()); ++m_requestGeneration; clearPendingRequestLocked(); clearDetailLocked(); invalidateCacheLocked(); }
    emit durationSecondsChanged(); emit zoomLevelChanged(); emit samplePointsVisibleChanged(); scheduleDetailRequest(); update();
}

void WaveformEditorItem::setZoomLevel(double value) {
    {
        QMutexLocker lock(&m_stateMutex);
        value = std::clamp(value, 1.0, maximumZoomLevelLocked());
        if (std::abs(m_zoomLevel - value) < 0.0001) return;
        m_zoomLevel = value;
        ++m_requestGeneration;
        clearPendingRequestLocked();
        invalidateCacheLocked();
    }
    emit zoomLevelChanged(); emit samplePointsVisibleChanged(); scheduleDetailRequest(); update();
}

void WaveformEditorItem::setZoomEnabled(bool value) { { QMutexLocker lock(&m_stateMutex); if (m_zoomEnabled == value) return; m_zoomEnabled = value; } emit zoomEnabledChanged(); }
void WaveformEditorItem::setGridEnabled(bool value) { { QMutexLocker lock(&m_stateMutex); if (m_gridEnabled == value) return; m_gridEnabled = value; invalidateCacheLocked(); } emit gridEnabledChanged(); update(); }
void WaveformEditorItem::setCrosshairEnabled(bool value) { { QMutexLocker lock(&m_stateMutex); if (m_crosshairEnabled == value) return; m_crosshairEnabled = value; } emit crosshairEnabledChanged(); update(); }
void WaveformEditorItem::setViewMode(int value) { value = std::clamp(value, 0, 1); { QMutexLocker lock(&m_stateMutex); if (m_viewMode == value) return; m_viewMode = value; invalidateCacheLocked(); } emit viewModeChanged(); emit channelCountChanged(); update(); }
void WaveformEditorItem::setMutedChannelsMask(qulonglong value) { { QMutexLocker lock(&m_stateMutex); if (m_mutedChannelsMask == value) return; m_mutedChannelsMask = value; invalidateCacheLocked(); } emit mutedChannelsMaskChanged(); update(); }
void WaveformEditorItem::setSoloedChannel(int value) { { QMutexLocker lock(&m_stateMutex); if (m_soloedChannel == value) return; m_soloedChannel = value; invalidateCacheLocked(); } emit soloedChannelChanged(); update(); }
void WaveformEditorItem::setChannelCountHint(int value) {
    value = std::clamp(value, 1, 64);
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_channelCountHint == value) return;
        m_channelCountHint = value;
        invalidateCacheLocked();
    }
    emit channelCountHintChanged();
    emit channelCountChanged();
    update();
}

void WaveformEditorItem::resetZoom() { setZoomLevel(1.0); }
double WaveformEditorItem::maximumZoomLevel() const { QMutexLocker lock(&m_stateMutex); return maximumZoomLevelLocked(); }

void WaveformEditorItem::setHoverPosition(double x, double y, bool active) {
    {
        QMutexLocker lock(&m_stateMutex);
        m_hoverActive = active;
        m_hoverPosition = QPointF(x, y);
    }
    if (crosshairEnabled()) update();
}

void WaveformEditorItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        {
            QMutexLocker lock(&m_stateMutex);
            ++m_requestGeneration;
            clearPendingRequestLocked();
            invalidateCacheLocked();
        }
        scheduleDetailRequest();
    }
}

QByteArray WaveformEditorItem::decodeWindow(const QString &path, double startSeconds,
                                             double endSeconds, int maxPoints) {
    const QByteArray encodedPath = path.toUtf8();
    std::size_t length = 0;
    std::uint8_t *data = ferrous_ffi_waveform_window(
        reinterpret_cast<const std::uint8_t *>(encodedPath.constData()),
        static_cast<std::size_t>(encodedPath.size()), startSeconds, endSeconds,
        static_cast<std::uint32_t>(std::max(1, maxPoints)), &length);
    if (data == nullptr || length == 0) return {};
    const QByteArray result(reinterpret_cast<const char *>(data), static_cast<qsizetype>(length));
    ferrous_ffi_waveform_window_free(data, length);
    return result;
}

bool WaveformEditorItem::parseWindow(const QByteArray &bytes, DetailWindow *window) {
    if (window == nullptr || bytes.size() < kWindowHeaderBytes || bytes.first(4) != QByteArrayLiteral("WVF1")) return false;
    const char *data = bytes.constData();
    const quint32 sampleRate = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data + 4));
    const quint16 channels = qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(data + 8));
    const double start = readF64(data + 12);
    const double end = readF64(data + 20);
    const quint32 framesPerPoint = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data + 28));
    const quint32 pointCount = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data + 32));
    const quint64 valueCount = static_cast<quint64>(channels) * pointCount * 2U;
    const quint64 expected = static_cast<quint64>(kWindowHeaderBytes) + valueCount * 4U;
    if (sampleRate == 0 || channels == 0 || pointCount == 0 || expected != static_cast<quint64>(bytes.size())) return false;
    window->sampleRateHz = static_cast<int>(sampleRate);
    window->channelCount = static_cast<int>(channels);
    window->startSeconds = start;
    window->endSeconds = end;
    window->framesPerPoint = framesPerPoint;
    window->pointCount = static_cast<int>(pointCount);
    window->extrema.resize(static_cast<std::size_t>(valueCount));
    for (quint64 index = 0; index < valueCount; ++index) {
        window->extrema[static_cast<std::size_t>(index)] = readF32(data + kWindowHeaderBytes + static_cast<qsizetype>(index * 4U));
    }
    return true;
}

void WaveformEditorItem::scheduleDetailRequest() {
    if (thread() == QThread::currentThread() && !m_requestTimer.isActive()) {
        m_requestTimer.start();
    }
}

void WaveformEditorItem::requestDetailWindow() {
    QString path;
    double requestStart = 0.0;
    double requestEnd = 0.0;
    int points = 0;
    quint64 generation = 0;
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_sourcePath.isEmpty() || m_durationSeconds <= 0.0 || width() < 2.0) return;
        const auto [visibleStart, visibleEnd] = visibleRangeLocked();
        const double visibleSpan = visibleEnd - visibleStart;
        requestStart = std::max(0.0, visibleStart - visibleSpan);
        requestEnd = std::min(m_durationSeconds, visibleEnd + visibleSpan);
        path = m_sourcePath;
        points = std::clamp(static_cast<int>(std::ceil(width() * 3.0)), 64, 16'384);
        generation = ++m_requestGeneration;
        m_requestInFlight = true;
        m_requestedStartSeconds = requestStart;
        m_requestedEndSeconds = requestEnd;
        m_requestedMaxPoints = points;
    }
    auto *watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher, generation]() {
        const QByteArray bytes = watcher->result();
        watcher->deleteLater();
        DetailWindow next;
        const bool parsed = parseWindow(bytes, &next);
        bool channelsChanged = false;
        bool rateChanged = false;
        bool pointsChanged = false;
        {
            QMutexLocker lock(&m_stateMutex);
            if (generation != m_requestGeneration) return;
            clearPendingRequestLocked();
            if (!parsed) return;
            const bool oldPointsVisible = samplePointsVisibleLocked();
            channelsChanged = m_channelCount != next.channelCount;
            rateChanged = m_sampleRateHz != next.sampleRateHz;
            m_channelCount = next.channelCount;
            m_sampleRateHz = next.sampleRateHz;
            m_detail = std::move(next);
            pointsChanged = oldPointsVisible != samplePointsVisibleLocked();
            invalidateCacheLocked();
        }
        if (channelsChanged) emit channelCountChanged();
        if (rateChanged) emit sampleRateHzChanged();
        if (pointsChanged) emit samplePointsVisibleChanged();
        update();
    });
    watcher->setFuture(QtConcurrent::run(&WaveformEditorItem::decodeWindow, path, requestStart, requestEnd, points));
}

void WaveformEditorItem::clearDetailLocked() { m_detail = DetailWindow{}; m_channelCount = 0; m_sampleRateHz = 0; }
void WaveformEditorItem::clearPendingRequestLocked() {
    m_requestInFlight = false;
    m_requestedStartSeconds = 0.0;
    m_requestedEndSeconds = 0.0;
    m_requestedMaxPoints = 0;
}
bool WaveformEditorItem::detailOrPendingRequestCoversLocked(
    double startSeconds, double endSeconds) const {
    const bool detailCovers = m_detail.pointCount > 0
        && startSeconds >= m_detail.startSeconds
        && endSeconds <= m_detail.endSeconds;
    const bool pendingCovers = m_requestInFlight
        && startSeconds >= m_requestedStartSeconds
        && endSeconds <= m_requestedEndSeconds;
    return detailCovers || pendingCovers;
}
std::pair<double, double> WaveformEditorItem::visibleRangeLocked() const {
    if (m_durationSeconds <= 0.0) return {0.0, 0.0};
    const double span = m_durationSeconds / std::max(1.0, m_zoomLevel);
    if (span >= m_durationSeconds * 0.9999) return {0.0, m_durationSeconds};
    double start = m_positionSeconds - span * 0.5;
    start = std::clamp(start, 0.0, m_durationSeconds - span);
    return {start, start + span};
}

double WaveformEditorItem::maximumZoomLevelLocked() const {
    const double pixels = std::max(1.0, width());
    const double rate = static_cast<double>(std::max(1, m_sampleRateHz > 0 ? m_sampleRateHz : 48'000));
    return std::max(1.0, m_durationSeconds * rate / pixels * 8.0);
}

bool WaveformEditorItem::samplePointsVisibleLocked() const {
    if (m_detail.framesPerPoint != 1 || m_detail.pointCount <= 1) return false;
    const auto [start, end] = visibleRangeLocked();
    const double visiblePoints = (end - start) * static_cast<double>(std::max(1, m_detail.sampleRateHz));
    return width() / std::max(1.0, visiblePoints) >= 4.0;
}

void WaveformEditorItem::invalidateCacheLocked() { m_cacheDirty = true; }
int WaveformEditorItem::displayedChannelCountLocked() const {
    const int sourceChannels = m_channelCount > 0 ? m_channelCount : m_channelCountHint;
    return m_viewMode == 0 ? 1 : std::max(1, sourceChannels);
}
bool WaveformEditorItem::channelIsMutedLocked(int channel) const {
    if (m_viewMode == 0) return false;
    if (m_soloedChannel >= 0) return channel != m_soloedChannel;
    return channel < 64 && (m_mutedChannelsMask & (qulonglong{1} << channel)) != 0;
}

void WaveformEditorItem::paint(QPainter *painter) {
    QImage cache;
    double visibleStart = 0.0;
    double visibleEnd = 0.0;
    QPointF hover;
    bool hoverActive = false;
    bool crosshair = false;
    double position = 0.0;
    double cacheStart = 0.0;
    double cacheEnd = 0.0;
    {
        QMutexLocker lock(&m_stateMutex);
        const int canvasWidth = std::max(1, static_cast<int>(std::floor(width())));
        const int canvasHeight = std::max(1, static_cast<int>(std::floor(height())));
        rebuildCacheLocked(canvasWidth, canvasHeight);
        cache = m_cache;
        std::tie(visibleStart, visibleEnd) = visibleRangeLocked();
        hover = m_hoverPosition;
        hoverActive = m_hoverActive;
        crosshair = m_crosshairEnabled;
        position = m_positionSeconds;
        cacheStart = m_cacheStartSeconds;
        cacheEnd = m_cacheEndSeconds;
    }
    const int canvasWidth = std::max(1, static_cast<int>(std::floor(width())));
    const int canvasHeight = std::max(1, static_cast<int>(std::floor(height())));
    const double cacheSpan = cacheEnd - cacheStart;
    if (cacheSpan > 0.0 && visibleEnd > visibleStart) {
        const double sourceX = (visibleStart - cacheStart) / cacheSpan * cache.width();
        const double sourceWidth = (visibleEnd - visibleStart) / cacheSpan * cache.width();
        painter->drawImage(
            QRectF(0.0, 0.0, canvasWidth, canvasHeight), cache,
            QRectF(sourceX, 0.0, sourceWidth, cache.height()));
    } else {
        painter->fillRect(QRect(0, 0, canvasWidth, canvasHeight), kBackground);
    }
    const double span = visibleEnd - visibleStart;
    if (span > 0.0 && position >= visibleStart && position <= visibleEnd) {
        const int x = static_cast<int>(std::round((position - visibleStart) / span * (canvasWidth - 1)));
        painter->setPen(QPen(kPlayhead, 1.0));
        painter->drawLine(x, 0, x, canvasHeight - 1);
    }
    if (crosshair && hoverActive) {
        QMutexLocker lock(&m_stateMutex);
        m_hoverPosition = hover;
        drawCrosshair(*painter, canvasWidth, canvasHeight, visibleStart, visibleEnd);
    }
}

void WaveformEditorItem::rebuildCacheLocked(int width, int height) {
    if (!m_cacheDirty && m_cachedViewportWidth == width && m_cachedViewportHeight == height) return;
    const auto [visibleStart, visibleEnd] = visibleRangeLocked();
    double renderStart = visibleStart;
    double renderEnd = visibleEnd;
    if (m_detail.pointCount > 0
        && visibleStart >= m_detail.startSeconds
        && visibleEnd <= m_detail.endSeconds) {
        renderStart = m_detail.startSeconds;
        renderEnd = m_detail.endSeconds;
    }
    const double visibleSpan = std::max(0.000'000'001, visibleEnd - visibleStart);
    const double renderSpan = std::max(visibleSpan, renderEnd - renderStart);
    const int renderWidth = std::clamp(
        static_cast<int>(std::ceil(static_cast<double>(width) * renderSpan / visibleSpan)),
        width, width * 4);
    m_cache = QImage(renderWidth, height, QImage::Format_RGB32);
    m_cache.fill(kBackground);
    QPainter painter(&m_cache);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const int channels = displayedChannelCountLocked();
    if (m_detail.pointCount > 0 && visibleStart >= m_detail.startSeconds && visibleEnd <= m_detail.endSeconds) {
        drawDetailLocked(painter, renderWidth, height, renderStart, renderEnd, channels);
    } else {
        drawOverviewLocked(painter, renderWidth, height, renderStart, renderEnd, channels);
    }
    drawGridLocked(painter, renderWidth, height, renderStart, renderEnd, channels);
    painter.end();
    m_cachedViewportWidth = width;
    m_cachedViewportHeight = height;
    m_cacheStartSeconds = renderStart;
    m_cacheEndSeconds = renderEnd;
    m_cacheDirty = false;
}

void WaveformEditorItem::drawGridLocked(QPainter &painter, int width, int height,
                                         double visibleStart, double visibleEnd, int channels) const {
    QFont font;
    font.setPixelSize(9);
    painter.setFont(font);
    const QFontMetrics metrics(font);
    painter.setPen(QPen(kGrid, 1.0));
    const double span = visibleEnd - visibleStart;
    if (span > 0.0) {
        const double interval = selectTimeInterval(span, width);
        for (double time = std::ceil(visibleStart / interval) * interval; time <= visibleEnd; time += interval) {
            const int x = static_cast<int>(std::round((time - visibleStart) / span * (width - 1)));
            if (m_gridEnabled) painter.drawLine(x, 0, x, height - 1);
            const QString label = formatTime(time, span);
            const int labelX = std::clamp(
                x - metrics.horizontalAdvance(label) / 2,
                2,
                std::max(2, width - metrics.horizontalAdvance(label) - 2));
            painter.setPen(QColor(152, 165, 157));
            painter.drawText(labelX, height - metrics.descent() - 2, label);
            painter.setPen(QPen(kGrid, 1.0));
        }
    }
    static constexpr double dbTicks[] = {0.0, -1.0, -3.0, -6.0, -9.0, -12.0, -18.0, -24.0};
    for (int channel = 0; channel < channels; ++channel) {
        const double top = static_cast<double>(channel) * height / channels;
        const double bottom = static_cast<double>(channel + 1) * height / channels;
        const double center = (top + bottom) * 0.5;
        const double half = (bottom - top) * 0.5;
        painter.setPen(QPen(kCenterLine, 1.0));
        painter.drawLine(0, static_cast<int>(center), width - 1, static_cast<int>(center));
        const QString silenceLabel = QStringLiteral("-∞");
        painter.setPen(QColor(152, 165, 157));
        painter.drawText(
            width - metrics.horizontalAdvance(silenceLabel) - 3,
            static_cast<int>(center) - 2,
            silenceLabel);
        painter.setPen(QPen(kGrid, 1.0));
        // Samples remain linear on screen. Converting dB ticks back to
        // amplitude makes their spacing nonlinear, matching editor-style
        // waveform rulers (dense near 0 dB and widening toward silence).
        for (double db : dbTicks) {
            const double amplitude = std::pow(10.0, db / 20.0);
            const int upper = static_cast<int>(std::round(center - amplitude * (half - 1.0)));
            const int lower = static_cast<int>(std::round(center + amplitude * (half - 1.0)));
            if (m_gridEnabled) {
                painter.drawLine(0, upper, width - 1, upper);
                painter.drawLine(0, lower, width - 1, lower);
            }
            const QString text = db == 0.0 ? QStringLiteral("0") : QString::number(static_cast<int>(db));
            painter.setPen(QColor(152, 165, 157));
            painter.drawText(width - metrics.horizontalAdvance(text) - 3, upper + metrics.ascent(), text);
            painter.drawText(width - metrics.horizontalAdvance(text) - 3, lower - 2, text);
            painter.setPen(QPen(kGrid, 1.0));
        }
    }
}

void WaveformEditorItem::drawOverviewLocked(QPainter &painter, int width, int height,
                                             double visibleStart, double visibleEnd, int channels) const {
    if (m_overviewData.isEmpty() || m_durationSeconds <= 0.0) return;
    const auto *peaks = reinterpret_cast<const uchar *>(m_overviewData.constData());
    const int count = m_overviewData.size();
    const double span = visibleEnd - visibleStart;
    for (int channel = 0; channel < channels; ++channel) {
        const double top = static_cast<double>(channel) * height / channels;
        const double bottom = static_cast<double>(channel + 1) * height / channels;
        const double center = (top + bottom) * 0.5;
        const double half = (bottom - top) * 0.5 - 1.0;
        painter.setPen(QPen(channelIsMutedLocked(channel) ? kMutedWaveform : kWaveform, 1.0));
        for (int x = 0; x < width; ++x) {
            const double time = visibleStart + (static_cast<double>(x) / std::max(1, width - 1)) * span;
            const int index = std::clamp(static_cast<int>(time / m_durationSeconds * count), 0, count - 1);
            const double peak = static_cast<double>(peaks[index]) / 255.0;
            painter.drawLine(x, static_cast<int>(center - peak * half), x, static_cast<int>(center + peak * half));
        }
    }
}

void WaveformEditorItem::drawDetailLocked(QPainter &painter, int width, int height,
                                           double visibleStart, double visibleEnd, int channels) const {
    const double detailSpan = m_detail.endSeconds - m_detail.startSeconds;
    const double visibleSpan = visibleEnd - visibleStart;
    if (detailSpan <= 0.0 || visibleSpan <= 0.0) return;
    const bool points = samplePointsVisibleLocked();
    for (int displayChannel = 0; displayChannel < channels; ++displayChannel) {
        const double top = static_cast<double>(displayChannel) * height / channels;
        const double bottom = static_cast<double>(displayChannel + 1) * height / channels;
        const double center = (top + bottom) * 0.5;
        const double half = (bottom - top) * 0.5 - 1.0;
        const QColor color = channelIsMutedLocked(displayChannel) ? kMutedWaveform : kWaveform;
        painter.setPen(QPen(color, points ? 1.2 : 1.0));
        QPolygonF polyline;
        for (int point = 0; point < m_detail.pointCount; ++point) {
            const double time = m_detail.startSeconds + (static_cast<double>(point) + 0.5) / m_detail.pointCount * detailSpan;
            if (time < visibleStart || time > visibleEnd) continue;
            const int x = static_cast<int>(std::round((time - visibleStart) / visibleSpan * (width - 1)));
            float minimum = 1.0F;
            float maximum = -1.0F;
            const int firstChannel = m_viewMode == 0 ? 0 : displayChannel;
            const int lastChannel = m_viewMode == 0 ? m_detail.channelCount : displayChannel + 1;
            for (int sourceChannel = firstChannel; sourceChannel < lastChannel; ++sourceChannel) {
                const std::size_t index = (static_cast<std::size_t>(point) * m_detail.channelCount + sourceChannel) * 2U;
                minimum = std::min(minimum, m_detail.extrema[index]);
                maximum = std::max(maximum, m_detail.extrema[index + 1]);
            }
            const double yMinimum = center - static_cast<double>(minimum) * half;
            const double yMaximum = center - static_cast<double>(maximum) * half;
            if (points) {
                polyline.append(QPointF(x, yMinimum));
            } else {
                painter.drawLine(x, static_cast<int>(std::round(yMinimum)), x, static_cast<int>(std::round(yMaximum)));
            }
        }
        if (points && polyline.size() > 1) {
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.drawPolyline(polyline);
            painter.setBrush(color);
            for (const QPointF &point : polyline) painter.drawRect(QRectF(point.x() - 1.5, point.y() - 1.5, 3.0, 3.0));
            painter.setRenderHint(QPainter::Antialiasing, false);
        }
    }
}

void WaveformEditorItem::drawCrosshair(QPainter &painter, int width, int height,
                                        double visibleStart, double visibleEnd) const {
    const int x = std::clamp(static_cast<int>(m_hoverPosition.x()), 0, width - 1);
    const int y = std::clamp(static_cast<int>(m_hoverPosition.y()), 0, height - 1);
    QPen pen(QColor(235, 241, 237, 150)); pen.setStyle(Qt::DotLine); painter.setPen(pen);
    painter.drawLine(x, 0, x, height - 1); painter.drawLine(0, y, width - 1, y);
    const double time = visibleStart + static_cast<double>(x) / std::max(1, width - 1) * (visibleEnd - visibleStart);
    const int channels = displayedChannelCountLocked();
    const double channelHeight = static_cast<double>(height) / channels;
    const double localY = std::fmod(static_cast<double>(y), channelHeight);
    const double amplitude = std::clamp(std::abs((channelHeight * 0.5 - localY) / (channelHeight * 0.5)), 0.0, 1.0);
    const QString dbText = amplitude <= 0.00001 ? QStringLiteral("-∞ dB") : QStringLiteral("%1 dB").arg(20.0 * std::log10(amplitude), 0, 'f', 1);
    const QString timeText = formatTime(time, visibleEnd - visibleStart);
    QFont font; font.setPixelSize(10); painter.setFont(font); const QFontMetrics metrics(font);
    painter.setPen(QColor(234, 240, 236)); painter.setBrush(QColor(0, 0, 0, 190));
    const QRect dbRect(width - metrics.horizontalAdvance(dbText) - 10, std::clamp(y - metrics.height(), 0, height - metrics.height() - 4), metrics.horizontalAdvance(dbText) + 8, metrics.height() + 4);
    painter.drawRect(dbRect); painter.drawText(dbRect.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignVCenter, dbText);
    const QRect timeRect(std::clamp(x - metrics.horizontalAdvance(timeText) / 2 - 4, 0, width - metrics.horizontalAdvance(timeText) - 8), height - metrics.height() - 4, metrics.horizontalAdvance(timeText) + 8, metrics.height() + 4);
    painter.drawRect(timeRect); painter.drawText(timeRect.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignVCenter, timeText);
}

void WaveformEditorItem::hoverMoveEvent(QHoverEvent *event) { { QMutexLocker lock(&m_stateMutex); m_hoverActive = true; m_hoverPosition = event->position(); } if (m_crosshairEnabled) update(); }
void WaveformEditorItem::hoverEnterEvent(QHoverEvent *event) { hoverMoveEvent(event); }
void WaveformEditorItem::hoverLeaveEvent(QHoverEvent *) { { QMutexLocker lock(&m_stateMutex); m_hoverActive = false; } update(); }

void WaveformEditorItem::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::MiddleButton && zoomEnabled()) { event->accept(); resetZoom(); return; }
    if (event->button() != Qt::RightButton || !crosshairEnabled()) { event->ignore(); return; }
    double seconds = 0.0;
    { QMutexLocker lock(&m_stateMutex); const auto [start, end] = visibleRangeLocked(); seconds = start + std::clamp(event->position().x() / std::max(1.0, width()), 0.0, 1.0) * (end - start); }
    event->accept(); emit seekRequested(seconds);
}

void WaveformEditorItem::wheelEvent(QWheelEvent *event) {
    if (!zoomEnabled()) { event->ignore(); return; }
    const double steps = event->angleDelta().y() / 120.0;
    if (std::abs(steps) < 0.01) { event->ignore(); return; }
    event->accept(); setZoomLevel(zoomLevel() * std::pow(kZoomStep, steps));
}
