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
#include <QQuickWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <tuple>

namespace {
constexpr int kWindowHeaderBytes = 36;
constexpr int kMaximumDetailPoints = 65'536;
constexpr double kZoomStep = 1.25;
constexpr double kDetailPointsPerPixel = 4.0;
constexpr double kMinimumDetailMarginSeconds = 0.75;
constexpr int kStagedCacheColumnsPerPaint = 128;
constexpr int kPlaybackTileWidth = 64;
constexpr int kPlaybackTilesPerPaint = 2;
constexpr int kMaximumPlaybackTilesPerPaint = 8;
constexpr int kPlaybackTilePrefetch = 2;
constexpr double kGridAlignmentEpsilon = 1.0e-9;
constexpr double kPositionRegressionToleranceSeconds = 0.03;
constexpr double kPositionSeekJumpSeconds = 0.75;
constexpr double kPositionServoMaximumErrorSeconds = 0.15;
constexpr double kPositionServoAlpha = 0.25;
constexpr QColor kBackground(5, 9, 7);
constexpr QColor kWaveform(54, 225, 161);
constexpr QColor kMutedWaveform(74, 104, 92);
constexpr QColor kGrid(20, 82, 49);
constexpr QColor kCenterLine(108, 42, 45);
constexpr QColor kChannelSeparator(79, 94, 86);
constexpr QColor kPlayhead(190, 190, 200, 150);
constexpr QColor kOverlay(190, 190, 200, 180);

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

QString formatTimePrecise(double seconds) {
    seconds = std::max(0.0, seconds);
    const int totalMs = static_cast<int>(std::round(seconds * 1000.0));
    const int milliseconds = totalMs % 1000;
    const int totalSeconds = totalMs / 1000;
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds % 3600) / 60;
    const int secondsPart = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3.%4")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secondsPart, 2, 10, QLatin1Char('0'))
            .arg(milliseconds, 3, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2.%3")
        .arg(minutes)
        .arg(secondsPart, 2, 10, QLatin1Char('0'))
        .arg(milliseconds, 3, 10, QLatin1Char('0'));
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

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
bool shouldLogProfileSpike(
    std::chrono::steady_clock::time_point *last,
    std::chrono::steady_clock::time_point now,
    double cooldownSeconds = 0.25) {
    if (last == nullptr) return false;
    if (*last != std::chrono::steady_clock::time_point{}
        && std::chrono::duration<double>(now - *last).count() < cooldownSeconds) {
        return false;
    }
    *last = now;
    return true;
}
#endif
}

WaveformEditorItem::WaveformEditorItem(QQuickItem *parent)
    : QQuickPaintedItem(parent) {
    setAntialiasing(false);
    setOpaquePainting(true);
    setFillColor(kBackground);
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::RightButton | Qt::MiddleButton);
    m_requestTimer.setSingleShot(true);
    m_requestTimer.setInterval(40);
    m_positionUpdatedAt = std::chrono::steady_clock::now();
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    m_profile.enabled = qEnvironmentVariableIsSet("FERROUS_PROFILE_WAVEFORM")
        || qEnvironmentVariableIsSet("FERROUS_PROFILE_UI")
        || qEnvironmentVariableIsSet("FERROUS_PROFILE");
    if (m_profile.enabled) {
        m_profile.lastSummary = std::chrono::steady_clock::now();
    }
#endif
    connect(&m_requestTimer, &QTimer::timeout, this, &WaveformEditorItem::requestDetailWindow);
    connect(this, &QQuickItem::windowChanged, this, &WaveformEditorItem::bindWindowFrameLoop);
}

QString WaveformEditorItem::sourcePath() const { QMutexLocker lock(&m_stateMutex); return m_sourcePath; }
QByteArray WaveformEditorItem::overviewData() const { QMutexLocker lock(&m_stateMutex); return m_overviewData; }
double WaveformEditorItem::positionSeconds() const { QMutexLocker lock(&m_stateMutex); return m_positionSeconds; }
double WaveformEditorItem::durationSeconds() const { QMutexLocker lock(&m_stateMutex); return m_durationSeconds; }
double WaveformEditorItem::zoomLevel() const { QMutexLocker lock(&m_stateMutex); return m_zoomLevel; }
bool WaveformEditorItem::playing() const { QMutexLocker lock(&m_stateMutex); return m_playing; }
bool WaveformEditorItem::zoomEnabled() const { QMutexLocker lock(&m_stateMutex); return m_zoomEnabled; }
bool WaveformEditorItem::gridEnabled() const { QMutexLocker lock(&m_stateMutex); return m_gridEnabled; }
bool WaveformEditorItem::crosshairEnabled() const { QMutexLocker lock(&m_stateMutex); return m_crosshairEnabled; }
bool WaveformEditorItem::showFpsOverlay() const { QMutexLocker lock(&m_stateMutex); return m_showFpsOverlay; }
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
        m_presentedZoomLevel = m_zoomLevel;
        m_zoomOutHandoffPending = false;
        m_zoomFallbackToOverview = false;
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
    { QMutexLocker lock(&m_stateMutex); if (m_overviewData == value) return; m_overviewData = value; if (!m_zoomOutHandoffPending) invalidateCacheLocked(); }
    emit overviewDataChanged(); update();
}

void WaveformEditorItem::setPositionSeconds(double value) {
    bool request = false;
    bool cacheNeedsUpdate = false;
    {
        QMutexLocker lock(&m_stateMutex);
        value = std::clamp(value, 0.0, std::max(0.0, m_durationSeconds));
        if (std::abs(m_positionSeconds - value) < 0.0001) return;
        const auto now = std::chrono::steady_clock::now();
        const auto previousRange = visibleRangeLocked();
        if (m_playing) {
            const double displayedPosition = displayedPositionSecondsLocked();
            const double error = value - displayedPosition;
            const bool heartbeatRegression =
                error < -kPositionRegressionToleranceSeconds
                && error > -kPositionSeekJumpSeconds;
            if (heartbeatRegression) {
                value = displayedPosition;
            } else if (std::abs(error) < kPositionServoMaximumErrorSeconds) {
                value = displayedPosition + kPositionServoAlpha * error;
            }
        }
        m_positionSeconds = value;
        m_positionUpdatedAt = now;
        const auto [start, end] = visibleRangeLocked();
        const auto [requestStart, requestEnd] = detailRequestVisibleRangeLocked();
        request = !detailOrPendingRequestCoversLocked(requestStart, requestEnd);
        const bool rangeMoved = std::abs(previousRange.first - start) > 0.0001
            || std::abs(previousRange.second - end) > 0.0001;
        const bool tiledPlayback = playbackTilesEligibleLocked(start, end);
        cacheNeedsUpdate = !m_zoomOutHandoffPending
            && !tiledPlayback
            && rangeMoved
            && (m_cacheDirty
                || start < m_cacheStartSeconds
                || end > m_cacheEndSeconds);
        if (cacheNeedsUpdate) invalidateCacheLocked();
    }
    emit positionSecondsChanged();
    if (request) scheduleDetailRequest();
    update();
}

void WaveformEditorItem::setPlaying(bool value) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_playing == value) return;
        const auto now = std::chrono::steady_clock::now();
        if (m_playing) m_positionSeconds = displayedPositionSecondsLocked();
        m_playing = value;
        m_positionUpdatedAt = now;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        if (value) m_profile.frameInitialized = false;
#endif
    }
    emit playingChanged();
    update();
}

void WaveformEditorItem::setDurationSeconds(double value) {
    { QMutexLocker lock(&m_stateMutex); value = std::max(0.0, value); if (std::abs(m_durationSeconds - value) < 0.0001) return; m_durationSeconds = value; m_zoomLevel = std::clamp(m_zoomLevel, 1.0, maximumZoomLevelLocked()); m_presentedZoomLevel = m_zoomLevel; m_zoomOutHandoffPending = false; ++m_requestGeneration; clearPendingRequestLocked(); clearDetailLocked(); m_zoomFallbackToOverview = false; invalidateCacheLocked(); }
    emit durationSecondsChanged(); emit zoomLevelChanged(); emit samplePointsVisibleChanged(); scheduleDetailRequest(); update();
}

void WaveformEditorItem::setZoomLevel(double value) {
    bool presentationChanged = false;
    {
        QMutexLocker lock(&m_stateMutex);
        value = std::clamp(value, 1.0, maximumZoomLevelLocked());
        if (std::abs(m_zoomLevel - value) < 0.0001) return;
        const double previousPresentedZoom = m_presentedZoomLevel;
        m_zoomLevel = value;
        ++m_requestGeneration;
        clearPendingRequestLocked();
        clearStagedCacheLocked();
        const auto [visibleStart, visibleEnd] = visibleRangeForZoomLocked(value);
        const bool detailReady = detailCoversRangeLocked(visibleStart, visibleEnd)
            && detailResolutionCoversLocked(visibleStart, visibleEnd);
        const auto [presentedStart, presentedEnd] = visibleRangeLocked();
        const bool canHoldPresentation = !m_cacheDirty
            && presentedStart >= m_cacheStartSeconds
            && presentedEnd <= m_cacheEndSeconds;
        const bool deferZoomOut = value < previousPresentedZoom
            && !detailReady
            && canHoldPresentation;
        m_zoomOutHandoffPending = deferZoomOut;
        if (deferZoomOut) {
            m_zoomFallbackToOverview = false;
        } else {
            m_presentedZoomLevel = value;
            presentationChanged = std::abs(
                m_presentedZoomLevel - previousPresentedZoom) >= 0.0001;
        }
        const bool zoomingInInsideCache = !deferZoomOut
            && value > previousPresentedZoom
            && !m_cacheDirty
            && visibleStart >= m_cacheStartSeconds
            && visibleEnd <= m_cacheEndSeconds;
        if (!deferZoomOut && value < previousPresentedZoom) {
            m_zoomFallbackToOverview = !detailReady;
        } else if (!deferZoomOut && detailReady) {
            m_zoomFallbackToOverview = false;
        }
        if (!deferZoomOut && !zoomingInInsideCache) invalidateCacheLocked();
    }
    emit zoomLevelChanged();
    if (presentationChanged) emit samplePointsVisibleChanged();
    scheduleDetailRequest();
    update();
}

void WaveformEditorItem::setZoomEnabled(bool value) { { QMutexLocker lock(&m_stateMutex); if (m_zoomEnabled == value) return; m_zoomEnabled = value; } emit zoomEnabledChanged(); }
void WaveformEditorItem::setGridEnabled(bool value) { { QMutexLocker lock(&m_stateMutex); if (m_gridEnabled == value) return; m_gridEnabled = value; } emit gridEnabledChanged(); update(); }
void WaveformEditorItem::setCrosshairEnabled(bool value) { { QMutexLocker lock(&m_stateMutex); if (m_crosshairEnabled == value) return; m_crosshairEnabled = value; } emit crosshairEnabledChanged(); update(); }
void WaveformEditorItem::setShowFpsOverlay(bool value) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_showFpsOverlay == value) return;
        m_showFpsOverlay = value;
        m_fpsInitialized = false;
        m_fpsValue = 0;
        m_fpsAccumFrames = 0;
        m_fpsAccumSeconds = 0.0;
    }
    emit showFpsOverlayChanged();
    update();
}
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
            m_presentedZoomLevel = m_zoomLevel;
            m_zoomOutHandoffPending = false;
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
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    bool profileEnabled = false;
    double profileZoom = 1.0;
    double profileVisibleStart = 0.0;
    double profileVisibleEnd = 0.0;
    int profileRenderWidth = 0;
    int profileWidth = 0;
    int profileHeight = 0;
#endif
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_sourcePath.isEmpty() || m_durationSeconds <= 0.0 || width() < 2.0) return;
        const auto [visibleStart, visibleEnd] = detailRequestVisibleRangeLocked();
        std::tie(requestStart, requestEnd) = requestRangeLocked(visibleStart, visibleEnd);
        path = m_sourcePath;
        points = detailRequestPointCountLocked(requestStart, requestEnd);
        generation = ++m_requestGeneration;
        m_requestInFlight = true;
        m_requestedStartSeconds = requestStart;
        m_requestedEndSeconds = requestEnd;
        m_requestedMaxPoints = points;
        m_requestedRenderWidth = renderPixelWidthLocked();
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        profileEnabled = m_profile.enabled;
        profileZoom = m_zoomLevel;
        profileVisibleStart = visibleStart;
        profileVisibleEnd = visibleEnd;
        profileRenderWidth = m_requestedRenderWidth;
        profileWidth = static_cast<int>(std::floor(width()));
        profileHeight = static_cast<int>(std::floor(height()));
        if (profileEnabled) ++m_profile.detailRequests;
#endif
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const auto profileRequestStarted = std::chrono::steady_clock::now();
    if (profileEnabled) {
        std::fprintf(
            stderr,
            "[ui-waveform-editor] detail_request gen=%llu zoom=%.3f visible=%.6f..%.6f request=%.6f..%.6f max_points=%d render_px=%d size=%dx%d\n",
            static_cast<unsigned long long>(generation),
            profileZoom,
            profileVisibleStart,
            profileVisibleEnd,
            requestStart,
            requestEnd,
            points,
            profileRenderWidth,
            profileWidth,
            profileHeight);
    }
#endif
    auto *watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [
        this, watcher, generation
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        , profileRequestStarted
#endif
    ]() {
        const QByteArray bytes = watcher->result();
        watcher->deleteLater();
        DetailWindow next;
        const bool parsed = parseWindow(bytes, &next);
        bool channelsChanged = false;
        bool rateChanged = false;
        bool pointsChanged = false;
        bool zoomChanged = false;
        bool requestFollowup = false;
        bool stale = false;
        bool parseFailed = false;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        const double profileDecodeMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - profileRequestStarted).count();
        const double profileReturnedStart = next.startSeconds;
        const double profileReturnedEnd = next.endSeconds;
        const int profileReturnedPoints = next.pointCount;
        const quint32 profileReturnedFramesPerPoint = next.framesPerPoint;
        const int profileReturnedChannels = next.channelCount;
        const int profileReturnedRate = next.sampleRateHz;
        bool profileEnabled = false;
        double profileZoom = 1.0;
        bool profileFallback = false;
        int profileStagedWidth = 0;
        double profileStagedSecondsPerPixel = 0.0;
#endif
        {
            QMutexLocker lock(&m_stateMutex);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            profileEnabled = m_profile.enabled;
#endif
            stale = generation != m_requestGeneration;
            if (!stale) {
                clearPendingRequestLocked();
                parseFailed = !parsed;
            }
            if (!stale && !parseFailed) {
                const bool oldPointsVisible = samplePointsVisibleLocked();
                const bool firstDetail = m_detail.pointCount == 0;
                const quint32 previousFramesPerPoint = m_detail.framesPerPoint;
                channelsChanged = m_channelCount != next.channelCount;
                rateChanged = m_sampleRateHz != next.sampleRateHz;
                clearStagedCacheLocked();
                m_channelCount = next.channelCount;
                m_sampleRateHz = next.sampleRateHz;
                m_detail = std::move(next);
                zoomChanged = clampZoomToMaximumLocked();
                pointsChanged = oldPointsVisible != samplePointsVisibleLocked();
                const auto [visibleStart, visibleEnd] = detailRequestVisibleRangeLocked();
                const bool detailReady = detailCoversRangeLocked(
                    visibleStart, visibleEnd)
                    && detailResolutionCoversLocked(visibleStart, visibleEnd);
                const bool replaceOverviewFallback = m_zoomFallbackToOverview
                    && detailReady;
                if (replaceOverviewFallback) m_zoomFallbackToOverview = false;
                const bool deferredZoomStage = m_zoomOutHandoffPending
                    && detailReady;
                if (deferredZoomStage) {
                    beginStagedCacheForRangeLocked(
                        visibleStart, visibleEnd, true);
                }
                const auto [presentedStart, presentedEnd] = visibleRangeLocked();
                const bool cacheCoversViewport = !m_cacheDirty
                    && presentedStart >= m_cacheStartSeconds
                    && presentedEnd <= m_cacheEndSeconds;
                const bool stageReplacement = !deferredZoomStage
                    && !m_zoomOutHandoffPending
                    && m_playing
                    && detailReady
                    && !samplePointsVisibleLocked()
                    && !playbackTilesEligibleLocked(
                        presentedStart, presentedEnd)
                    && cacheCoversViewport;
                if (stageReplacement) beginStagedCacheLocked();
                if (!deferredZoomStage
                    && !stageReplacement
                    && detailResultRequiresImmediateCacheRefreshLocked(
                        firstDetail,
                        channelsChanged,
                        zoomChanged,
                        replaceOverviewFallback,
                        previousFramesPerPoint,
                        detailReady)) {
                    invalidateCacheLocked();
                }
                requestFollowup = !detailOrPendingRequestCoversLocked(
                    visibleStart, visibleEnd);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                profileStagedWidth = m_stagedCache.width();
                profileStagedSecondsPerPixel = m_stagedCacheSecondsPerPixel;
                if (profileEnabled) {
                    ++m_profile.detailHandoffs;
                    m_profile.decodeMs += profileDecodeMs;
                    m_profile.maximumDecodeMs = std::max(
                        m_profile.maximumDecodeMs, profileDecodeMs);
                }
#endif
            }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            profileZoom = m_zoomLevel;
            profileFallback = m_zoomFallbackToOverview;
#endif
        }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        if (profileEnabled) {
            const char *status = stale ? "stale" : (parseFailed ? "parse_failed" : "accepted");
            std::fprintf(
                stderr,
                "[ui-waveform-editor] detail_result gen=%llu status=%s elapsed_ms=%.3f bytes=%lld returned=%.6f..%.6f points=%d frames_per_point=%u channels=%d rate=%d zoom=%.3f overview_fallback=%d followup=%d stage_width=%d stage_seconds_per_px=%.9f\n",
                static_cast<unsigned long long>(generation),
                status,
                profileDecodeMs,
                static_cast<long long>(bytes.size()),
                profileReturnedStart,
                profileReturnedEnd,
                profileReturnedPoints,
                profileReturnedFramesPerPoint,
                profileReturnedChannels,
                profileReturnedRate,
                profileZoom,
                profileFallback ? 1 : 0,
                requestFollowup ? 1 : 0,
                profileStagedWidth,
                profileStagedSecondsPerPixel);
        }
#endif
        if (stale || parseFailed) return;
        if (channelsChanged) emit channelCountChanged();
        if (rateChanged) emit sampleRateHzChanged();
        if (zoomChanged) emit zoomLevelChanged();
        if (pointsChanged) emit samplePointsVisibleChanged();
        if (requestFollowup) scheduleDetailRequest();
        update();
    });
    watcher->setFuture(QtConcurrent::run(&WaveformEditorItem::decodeWindow, path, requestStart, requestEnd, points));
}

void WaveformEditorItem::clearDetailLocked() {
    m_detail = DetailWindow{};
    m_channelCount = 0;
    m_sampleRateHz = 0;
}
void WaveformEditorItem::clearPendingRequestLocked() {
    m_requestInFlight = false;
    m_requestedStartSeconds = 0.0;
    m_requestedEndSeconds = 0.0;
    m_requestedMaxPoints = 0;
    m_requestedRenderWidth = 0;
}

double WaveformEditorItem::displayedPositionSecondsLocked() const {
    double position = m_positionSeconds;
    if (m_playing) {
        position += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_positionUpdatedAt).count();
    }
    return std::clamp(position, 0.0, std::max(0.0, m_durationSeconds));
}

bool WaveformEditorItem::detailOrPendingRequestCoversLocked(
    double startSeconds, double endSeconds) const {
    const double visibleSpan = std::max(0.0, endSeconds - startSeconds);
    const double requestMargin = detailRequestMarginLocked(visibleSpan);
    const double safetyMargin = requestMargin * 0.6;
    const double coveredStart = std::max(0.0, startSeconds - safetyMargin);
    const double coveredEnd = std::min(m_durationSeconds, endSeconds + safetyMargin);
    const bool detailCovers = detailCoversRangeLocked(coveredStart, coveredEnd)
        && detailResolutionCoversLocked(startSeconds, endSeconds);
    const bool pendingCovers = m_requestInFlight
        && coveredStart >= m_requestedStartSeconds
        && coveredEnd <= m_requestedEndSeconds
        && m_requestedRenderWidth >= renderPixelWidthLocked();
    return detailCovers || pendingCovers;
}

bool WaveformEditorItem::detailCoversRangeLocked(
    double startSeconds, double endSeconds) const {
    const double sampleTolerance = 2.0
        / static_cast<double>(std::max(1, m_detail.sampleRateHz));
    const double tolerance = std::max(0.000'001, sampleTolerance);
    return m_detail.pointCount > 0
        && startSeconds + tolerance >= m_detail.startSeconds
        && endSeconds <= m_detail.endSeconds + tolerance;
}
bool WaveformEditorItem::detailResolutionCoversLocked(
    double startSeconds, double endSeconds) const {
    return detailResolutionCoversPixelSpanLocked(
        startSeconds, endSeconds, renderPixelWidthLocked());
}

bool WaveformEditorItem::detailResolutionCoversPixelSpanLocked(
    double startSeconds, double endSeconds, int pixelWidth) const {
    const double detailSpan = m_detail.endSeconds - m_detail.startSeconds;
    const double visibleSpan = endSeconds - startSeconds;
    if (m_detail.pointCount <= 0 || detailSpan <= 0.0 || visibleSpan <= 0.0) return false;
    const double visiblePoints = static_cast<double>(m_detail.pointCount)
        * visibleSpan / detailSpan;
    double requiredPoints = static_cast<double>(std::max(1, pixelWidth))
        * kDetailPointsPerPixel;
    if (m_detail.sampleRateHz > 0) {
        const double availableSamples = visibleSpan
            * static_cast<double>(m_detail.sampleRateHz);
        requiredPoints = std::max(
            1.0, std::min(requiredPoints, availableSamples));
    }
    return visiblePoints >= requiredPoints * 0.9;
}
std::pair<double, double> WaveformEditorItem::visibleRangeForZoomLocked(
    double zoomLevel) const {
    if (m_durationSeconds <= 0.0) return {0.0, 0.0};
    const double span = m_durationSeconds / std::max(1.0, zoomLevel);
    if (span >= m_durationSeconds * 0.9999) return {0.0, m_durationSeconds};
    double start = displayedPositionSecondsLocked() - span * 0.5;
    start = std::clamp(start, 0.0, m_durationSeconds - span);
    return {start, start + span};
}

std::pair<double, double> WaveformEditorItem::visibleRangeLocked() const {
    return visibleRangeForZoomLocked(m_presentedZoomLevel);
}

std::pair<double, double> WaveformEditorItem::detailRequestVisibleRangeLocked() const {
    return m_zoomOutHandoffPending
        ? visibleRangeForZoomLocked(m_zoomLevel)
        : visibleRangeLocked();
}

std::pair<double, double> WaveformEditorItem::requestRangeLocked(
    double visibleStart, double visibleEnd) const {
    const double visibleSpan = std::max(0.0, visibleEnd - visibleStart);
    const double margin = detailRequestMarginLocked(visibleSpan);
    return {
        std::max(0.0, visibleStart - margin),
        std::min(m_durationSeconds, visibleEnd + margin),
    };
}

double WaveformEditorItem::detailRequestMarginLocked(double visibleSpan) const {
    const double preferredMargin = std::max(
        visibleSpan, kMinimumDetailMarginSeconds);
    const double densityLimitedSpan = visibleSpan
        * static_cast<double>(kMaximumDetailPoints)
        / requiredVisibleDetailPointsLocked(visibleSpan);
    double maximumRequestSpan = densityLimitedSpan;
    if (m_sampleRateHz > 0) {
        const double requiredPoints = requiredVisibleDetailPointsLocked(
            visibleSpan);
        const double visibleFrames = visibleSpan
            * static_cast<double>(m_sampleRateHz);
        const double targetFramesPerPoint = std::max(
            1.0, std::floor(visibleFrames / requiredPoints));
        // Keep the requested frame count inside the bin size selected for the
        // viewport.  Otherwise a capped request can cross the next integer bin
        // boundary and remain permanently too coarse for the current zoom.
        const double quantizedSpan = static_cast<double>(
            kMaximumDetailPoints - 1)
            * targetFramesPerPoint / static_cast<double>(m_sampleRateHz);
        maximumRequestSpan = std::min(maximumRequestSpan, quantizedSpan);
    }
    const double maximumMargin = std::max(
        0.0, (maximumRequestSpan - visibleSpan) * 0.5);
    return std::min(preferredMargin, maximumMargin);
}

double WaveformEditorItem::requiredVisibleDetailPointsLocked(
    double visibleSpan) const {
    const double pixelPoints = static_cast<double>(renderPixelWidthLocked())
        * kDetailPointsPerPixel;
    if (m_sampleRateHz <= 0) return pixelPoints;
    const double availableSamples = visibleSpan
        * static_cast<double>(m_sampleRateHz);
    return std::max(1.0, std::min(pixelPoints, availableSamples));
}

int WaveformEditorItem::detailRequestPointCountLocked(
    double requestStart, double requestEnd) const {
    const auto [visibleStart, visibleEnd] = detailRequestVisibleRangeLocked();
    const double visibleSpan = std::max(0.000'000'001, visibleEnd - visibleStart);
    const double requestSpan = std::max(visibleSpan, requestEnd - requestStart);
    const double required = requiredVisibleDetailPointsLocked(visibleSpan)
        * requestSpan / visibleSpan;
    int requestedPoints = static_cast<int>(std::ceil(required));
    if (m_sampleRateHz > 0) {
        const double requestFrames = std::ceil(
            requestSpan * static_cast<double>(m_sampleRateHz));
        const double targetFramesPerPoint = std::max(
            1.0, std::floor(requestFrames / std::max(1.0, required)));
        requestedPoints = std::max(
            requestedPoints,
            static_cast<int>(std::ceil(requestFrames / targetFramesPerPoint)));
    }
    return std::clamp(requestedPoints, 64, kMaximumDetailPoints);
}

int WaveformEditorItem::renderPixelWidthLocked() const {
    const double scale = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1.0;
    return std::max(1, static_cast<int>(std::ceil(width() * scale)));
}

WaveformEditorItem::CacheGrid WaveformEditorItem::cacheGridForRangeLocked(
    double visibleStart, double visibleEnd,
    double sourceStart, double sourceEnd) const {
    const double visibleSpan = std::max(
        0.000'000'001, visibleEnd - visibleStart);
    const double precedingSpan = m_playing ? visibleSpan * 0.25 : visibleSpan;
    const double followingSpan = m_playing ? visibleSpan * 1.75 : visibleSpan;
    double desiredStart = std::max(
        sourceStart, visibleStart - precedingSpan);
    double desiredEnd = std::min(
        sourceEnd, visibleEnd + followingSpan);
    desiredStart = std::min(desiredStart, visibleStart);
    desiredEnd = std::max(desiredEnd, visibleEnd);

    const int viewportWidth = renderPixelWidthLocked();
    const double secondsPerPixel = visibleSpan
        / static_cast<double>(viewportWidth);
    // Keep the raster origin on a track-absolute lattice.  A moving cache can
    // then translate its overlap by whole pixels without regrouping peaks.
    const double firstPixel = std::floor(
        desiredStart / secondsPerPixel + kGridAlignmentEpsilon);
    const double renderStart = firstPixel * secondsPerPixel;
    const int requiredWidth = std::max(
        1,
        static_cast<int>(std::ceil(
            (desiredEnd - renderStart) / secondsPerPixel
            - kGridAlignmentEpsilon)));
    const int visibleWidth = std::max(
        1,
        static_cast<int>(std::ceil(
            (visibleEnd - renderStart) / secondsPerPixel
            - kGridAlignmentEpsilon)));
    const int renderWidth = std::clamp(
        std::max(requiredWidth, visibleWidth),
        visibleWidth,
        viewportWidth * 3 + 1);
    return {
        renderStart,
        renderStart + static_cast<double>(renderWidth) * secondsPerPixel,
        secondsPerPixel,
        renderWidth,
    };
}

double WaveformEditorItem::maximumZoomLevelLocked() const {
    const double pixels = std::max(1.0, width());
    const double rate = static_cast<double>(std::max(1, m_sampleRateHz > 0 ? m_sampleRateHz : 48'000));
    return std::max(1.0, m_durationSeconds * rate / pixels * 8.0);
}

bool WaveformEditorItem::clampZoomToMaximumLocked() {
    const double maximum = maximumZoomLevelLocked();
    const double clampedTarget = std::clamp(m_zoomLevel, 1.0, maximum);
    const double clampedPresentation = std::clamp(
        m_presentedZoomLevel, 1.0, maximum);
    const bool targetChanged = std::abs(m_zoomLevel - clampedTarget) >= 0.0001;
    const bool presentationChanged = std::abs(
        m_presentedZoomLevel - clampedPresentation) >= 0.0001;
    if (!targetChanged && !presentationChanged) return false;
    m_zoomLevel = clampedTarget;
    m_presentedZoomLevel = clampedPresentation;
    if (m_zoomLevel >= m_presentedZoomLevel) m_zoomOutHandoffPending = false;
    ++m_requestGeneration;
    clearPendingRequestLocked();
    invalidateCacheLocked();
    return targetChanged;
}

bool WaveformEditorItem::samplePointsVisibleLocked() const {
    if (m_detail.framesPerPoint != 1 || m_detail.pointCount <= 1) return false;
    const auto [start, end] = visibleRangeLocked();
    const double visiblePoints = (end - start) * static_cast<double>(std::max(1, m_detail.sampleRateHz));
    return width() / std::max(1.0, visiblePoints) >= 4.0;
}

bool WaveformEditorItem::renderDetailDirectlyLocked(
    double visibleStart, double visibleEnd) const {
    if (!detailCoversRangeLocked(visibleStart, visibleEnd)) return false;
    return samplePointsVisibleLocked()
        && detailResolutionCoversLocked(visibleStart, visibleEnd);
}

bool WaveformEditorItem::detailResultRequiresImmediateCacheRefreshLocked(
    bool firstDetail, bool channelsChanged, bool zoomChanged,
    bool replaceOverviewFallback, quint32 previousFramesPerPoint,
    bool detailReady) const {
    const bool preciseDetailReplacesOverview = detailReady
        && m_cacheFramesPerPoint == 0;
    return firstDetail
        || channelsChanged
        || zoomChanged
        || replaceOverviewFallback
        || m_detail.framesPerPoint < previousFramesPerPoint
        || preciseDetailReplacesOverview;
}

double WaveformEditorItem::detailPointTimeLocked(int point) const {
    if (m_detail.sampleRateHz <= 0 || m_detail.framesPerPoint == 0) {
        return m_detail.startSeconds;
    }
    const double framesPerPoint = static_cast<double>(m_detail.framesPerPoint);
    const double centerFrame = static_cast<double>(point) * framesPerPoint
        + (framesPerPoint - 1.0) * 0.5;
    return m_detail.startSeconds
        + centerFrame / static_cast<double>(m_detail.sampleRateHz);
}

std::pair<int, int> WaveformEditorItem::detailPointRangeLocked(
    double visibleStart, double visibleEnd) const {
    if (m_detail.pointCount <= 0
        || m_detail.sampleRateHz <= 0
        || m_detail.framesPerPoint == 0
        || visibleEnd < visibleStart) {
        return {0, 0};
    }
    const double step = static_cast<double>(m_detail.framesPerPoint)
        / static_cast<double>(m_detail.sampleRateHz);
    const double firstPointTime = detailPointTimeLocked(0);
    const double first = (visibleStart - firstPointTime) / step;
    const double last = (visibleEnd - firstPointTime) / step;
    return {
        std::clamp(
            static_cast<int>(std::floor(first)), 0, m_detail.pointCount),
        std::clamp(
            static_cast<int>(std::ceil(last)) + 1, 0, m_detail.pointCount),
    };
}

void WaveformEditorItem::updateFpsEstimateLocked() {
    const auto now = std::chrono::steady_clock::now();
    if (!m_fpsInitialized) {
        m_fpsInitialized = true;
        m_lastFrameTime = now;
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - m_lastFrameTime).count();
    m_lastFrameTime = now;
    if (elapsed <= 0.0) return;
    ++m_fpsAccumFrames;
    m_fpsAccumSeconds += elapsed;
    if (m_fpsAccumSeconds < 0.2) return;
    m_fpsValue = std::clamp(
        static_cast<int>(std::lround(m_fpsAccumFrames / m_fpsAccumSeconds)), 0, 999);
    m_fpsAccumFrames = 0;
    m_fpsAccumSeconds = 0.0;
}

void WaveformEditorItem::bindWindowFrameLoop(QQuickWindow *window) {
    if (m_frameSwappedConnection) disconnect(m_frameSwappedConnection);
    if (m_windowVisibilityConnection) disconnect(m_windowVisibilityConnection);
    m_frameSwappedConnection = {};
    m_windowVisibilityConnection = {};
    if (window == nullptr) return;
    m_frameSwappedConnection = connect(
        window,
        &QQuickWindow::frameSwapped,
        this,
        &WaveformEditorItem::handleWindowFrameSwapped,
        Qt::QueuedConnection);
    m_windowVisibilityConnection = connect(
        window,
        &QWindow::visibleChanged,
        this,
        [this](bool visible) {
            if (visible && playing()) update();
        },
        Qt::QueuedConnection);
    if (playing()) update();
}

void WaveformEditorItem::handleWindowFrameSwapped() {
    if (!isVisible()) return;
    bool request = false;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const auto profileNow = std::chrono::steady_clock::now();
    bool profileLogGap = false;
    double profileGapMs = 0.0;
    double profileZoom = 1.0;
    double profileSpan = 0.0;
    bool profileDirect = false;
    bool profileCacheDirty = false;
    double profileCacheStart = 0.0;
    double profileCacheEnd = 0.0;
#endif
    {
        QMutexLocker lock(&m_stateMutex);
        if (!m_playing) return;
        const auto [start, end] = visibleRangeLocked();
        const auto [requestStart, requestEnd] = detailRequestVisibleRangeLocked();
        request = !detailOrPendingRequestCoversLocked(requestStart, requestEnd);
        const bool direct = renderDetailDirectlyLocked(start, end);
        const bool tiledPlayback = playbackTilesEligibleLocked(start, end);
        if (!m_zoomOutHandoffPending
            && !direct
            && !tiledPlayback
            && (m_cacheDirty
                || start < m_cacheStartSeconds
                || end > m_cacheEndSeconds)) {
            invalidateCacheLocked();
        }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        if (m_profile.enabled) {
            if (m_profile.frameInitialized) {
                profileGapMs = std::chrono::duration<double, std::milli>(
                    profileNow - m_profile.lastFrame).count();
                m_profile.maximumFrameGapMs = std::max(
                    m_profile.maximumFrameGapMs, profileGapMs);
                profileLogGap = profileGapMs >= 8.0
                    && shouldLogProfileSpike(
                        &m_profile.lastFrameGapSpike, profileNow);
            } else {
                m_profile.frameInitialized = true;
            }
            m_profile.lastFrame = profileNow;
            profileZoom = m_presentedZoomLevel;
            profileSpan = end - start;
            profileDirect = direct;
            profileCacheDirty = m_cacheDirty;
            profileCacheStart = m_cacheStartSeconds;
            profileCacheEnd = m_cacheEndSeconds;
        }
#endif
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (profileLogGap) {
        std::fprintf(
            stderr,
            "[ui-waveform-editor] frame_gap ms=%.3f zoom=%.3f span=%.6f mode=%s cache_dirty=%d cache=%.6f..%.6f request=%d\n",
            profileGapMs,
            profileZoom,
            profileSpan,
            profileDirect ? "direct" : "cached",
            profileCacheDirty ? 1 : 0,
            profileCacheStart,
            profileCacheEnd,
            request ? 1 : 0);
    }
#endif
    if (request) scheduleDetailRequest();
    update();
}

void WaveformEditorItem::invalidateCacheLocked() {
    if (m_zoomOutHandoffPending) {
        const auto [targetStart, targetEnd] = visibleRangeForZoomLocked(
            m_zoomLevel);
        m_presentedZoomLevel = m_zoomLevel;
        m_zoomOutHandoffPending = false;
        m_zoomFallbackToOverview = !detailCoversRangeLocked(
            targetStart, targetEnd)
            || !detailResolutionCoversLocked(targetStart, targetEnd);
    }
    m_cacheDirty = true;
    clearStagedCacheLocked();
    clearPlaybackTilesLocked();
}

void WaveformEditorItem::clearStagedCacheLocked() {
    m_stagedCache = QImage{};
    m_stagedCacheStartSeconds = 0.0;
    m_stagedCacheEndSeconds = 0.0;
    m_stagedCacheSecondsPerPixel = 0.0;
    m_stagedCacheFramesPerPoint = 0;
    m_stagedCacheDisplayedChannels = 0;
    m_stagedCacheNextX = 0;
    m_stagedCacheCommitsDeferredZoom = false;
}

void WaveformEditorItem::beginStagedCacheLocked() {
    const auto [visibleStart, visibleEnd] = visibleRangeLocked();
    beginStagedCacheForRangeLocked(visibleStart, visibleEnd, false);
}

void WaveformEditorItem::beginStagedCacheForRangeLocked(
    double visibleStart, double visibleEnd,
    bool commitsDeferredZoom) {
    if (!detailCoversRangeLocked(visibleStart, visibleEnd)
        || !detailResolutionCoversLocked(visibleStart, visibleEnd)) {
        clearStagedCacheLocked();
        return;
    }
    const CacheGrid grid = cacheGridForRangeLocked(
        visibleStart, visibleEnd,
        m_detail.startSeconds, m_detail.endSeconds);
    const int renderHeight = std::max(
        1, static_cast<int>(std::floor(height())));
    m_stagedCache = QImage(grid.width, renderHeight, QImage::Format_RGB32);
    m_stagedCacheStartSeconds = grid.startSeconds;
    m_stagedCacheEndSeconds = grid.endSeconds;
    m_stagedCacheSecondsPerPixel = grid.secondsPerPixel;
    m_stagedCacheFramesPerPoint = m_detail.framesPerPoint;
    m_stagedCacheDisplayedChannels = displayedChannelCountLocked();
    // Render every replacement from decoded extrema. Mixing copied pixels
    // from an older cache with newly rendered columns caused the raster
    // corruption introduced by the overlap-reuse optimization.
    m_stagedCacheNextX = 0;
    m_stagedCacheCommitsDeferredZoom = commitsDeferredZoom;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profile.enabled) ++m_profile.stagedCacheStarts;
#endif
}

bool WaveformEditorItem::advanceStagedCacheLocked() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    m_profile.lastStagedCacheMs = 0.0;
    m_profile.lastStagedCacheColumns = 0;
    const auto profileStageStarted = std::chrono::steady_clock::now();
#endif
    if (m_stagedCache.isNull()) return false;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    int processedColumns = 0;
#endif
    if (m_stagedCacheNextX < m_stagedCache.width()) {
        QPainter painter(&m_stagedCache);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const int firstX = m_stagedCacheNextX;
        const int lastX = std::min(
            m_stagedCache.width(), firstX + kStagedCacheColumnsPerPaint);
        painter.fillRect(
            QRect(firstX, 0, lastX - firstX, m_stagedCache.height()),
            kBackground);
        painter.setClipRect(
            QRect(firstX, 0, lastX - firstX, m_stagedCache.height()),
            Qt::ReplaceClip);
        drawDetailSliceLocked(
            painter,
            m_stagedCache.width(),
            m_stagedCache.height(),
            m_stagedCacheStartSeconds,
            m_stagedCacheEndSeconds,
            m_stagedCacheDisplayedChannels,
            firstX,
            lastX);
        m_stagedCacheNextX = lastX;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        processedColumns = lastX - firstX;
#endif
        painter.end();
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profile.enabled) {
        const double stageMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - profileStageStarted).count();
        m_profile.stagedCacheMs += stageMs;
        m_profile.maximumStagedCacheMs = std::max(
            m_profile.maximumStagedCacheMs, stageMs);
        m_profile.lastStagedCacheMs = stageMs;
        m_profile.lastStagedCacheColumns = processedColumns;
    }
#endif
    if (m_stagedCacheNextX < m_stagedCache.width()) {
        return false;
    }

    const bool commitsDeferredZoom = m_stagedCacheCommitsDeferredZoom;
    const auto [visibleStart, visibleEnd] = commitsDeferredZoom
        ? visibleRangeForZoomLocked(m_zoomLevel)
        : visibleRangeLocked();
    bool presentationCommitted = false;
    if (visibleStart >= m_stagedCacheStartSeconds
        && visibleEnd <= m_stagedCacheEndSeconds) {
        m_cache = std::move(m_stagedCache);
        m_cacheStartSeconds = m_stagedCacheStartSeconds;
        m_cacheEndSeconds = m_stagedCacheEndSeconds;
        m_cacheSecondsPerPixel = m_stagedCacheSecondsPerPixel;
        m_cacheFramesPerPoint = m_stagedCacheFramesPerPoint;
        m_cacheDisplayedChannels = m_stagedCacheDisplayedChannels;
        m_cachedViewportWidth = renderPixelWidthLocked();
        m_cachedViewportHeight = m_cache.height();
        m_cacheDirty = false;
        if (commitsDeferredZoom) {
            m_presentedZoomLevel = m_zoomLevel;
            m_zoomOutHandoffPending = false;
            m_zoomFallbackToOverview = false;
            presentationCommitted = true;
        }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        if (m_profile.enabled) ++m_profile.stagedCacheSwaps;
#endif
    }
    clearStagedCacheLocked();
    if (commitsDeferredZoom
        && !presentationCommitted
        && m_zoomOutHandoffPending
        && detailCoversRangeLocked(visibleStart, visibleEnd)
        && detailResolutionCoversLocked(visibleStart, visibleEnd)) {
        beginStagedCacheForRangeLocked(visibleStart, visibleEnd, true);
    }
    return presentationCommitted;
}

void WaveformEditorItem::queueGuiContinuationFromPaint(
    bool notifySamplePointsChanged, bool requestRepaint) {
    if (notifySamplePointsChanged) {
        m_samplePointsNotificationPending.store(true, std::memory_order_release);
    }
    if (requestRepaint) {
        m_guiRepaintPending.store(true, std::memory_order_release);
    }
    bool expected = false;
    if (!m_guiContinuationQueued.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    // paint() runs on Qt Quick's render thread. Queue all QObject and item
    // activity back to this item's GUI thread instead of mutating Qt Quick's
    // frame lifecycle while the scene graph is recording the current frame.
    QMetaObject::invokeMethod(
        this,
        [this]() {
            m_guiContinuationQueued.store(false, std::memory_order_release);
            if (m_samplePointsNotificationPending.exchange(
                    false, std::memory_order_acq_rel)) {
                emit samplePointsVisibleChanged();
            }
            if (m_guiRepaintPending.exchange(false, std::memory_order_acq_rel)) {
                update();
            }
        },
        Qt::QueuedConnection);
}

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
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const auto profilePaintStarted = std::chrono::steady_clock::now();
    bool profileEnabled = false;
    bool profileRebuiltCache = false;
    double profileZoom = 1.0;
    double profileSpan = 0.0;
    int profileVisiblePoints = 0;
    int profileDetailPoints = 0;
    quint32 profileFramesPerPoint = 0;
    bool profileOverviewFallback = false;
    double profileStagedCacheMs = 0.0;
    int profileStagedCacheColumns = 0;
#endif
    QImage cache;
    double visibleStart = 0.0;
    double visibleEnd = 0.0;
    QPointF hover;
    bool hoverActive = false;
    bool crosshair = false;
    double position = 0.0;
    double cacheStart = 0.0;
    double cacheSecondsPerPixel = 0.0;
    int channels = 1;
    int fpsValue = 0;
    bool showFps = false;
    bool directDetail = false;
    bool playbackTilesComplete = false;
    [[maybe_unused]] int playbackTilesRendered = 0;
    std::vector<PlaybackTilePaint> playbackTilePaints;
    bool presentationCommitted = false;
    bool stagingContinues = false;
    const int canvasWidth = std::max(1, static_cast<int>(std::floor(width())));
    const int canvasHeight = std::max(1, static_cast<int>(std::floor(height())));
    // Replace body pixels rather than blending with the previous raster; the
    // overlays switch back to source-over below for their intended alpha.
    painter->setCompositionMode(QPainter::CompositionMode_Source);
    {
        QMutexLocker lock(&m_stateMutex);
        presentationCommitted = advanceStagedCacheLocked();
        std::tie(visibleStart, visibleEnd) = visibleRangeLocked();
        directDetail = renderDetailDirectlyLocked(visibleStart, visibleEnd);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        profileEnabled = m_profile.enabled;
        profileZoom = m_presentedZoomLevel;
        profileSpan = visibleEnd - visibleStart;
        const auto [firstPoint, lastPoint] = detailPointRangeLocked(
            visibleStart, visibleEnd);
        profileVisiblePoints = lastPoint - firstPoint;
        profileDetailPoints = m_detail.pointCount;
        profileFramesPerPoint = m_detail.framesPerPoint;
        profileOverviewFallback = m_zoomFallbackToOverview;
        profileStagedCacheMs = m_profile.lastStagedCacheMs;
        profileStagedCacheColumns = m_profile.lastStagedCacheColumns;
        profileRebuiltCache = !directDetail
            && (m_cacheDirty
                || m_cachedViewportWidth != renderPixelWidthLocked()
                || m_cachedViewportHeight != canvasHeight);
#endif
        if (directDetail) {
            painter->fillRect(QRect(0, 0, canvasWidth, canvasHeight), kBackground);
            drawDetailLocked(
                *painter, canvasWidth, canvasHeight,
                visibleStart, visibleEnd, displayedChannelCountLocked());
        } else {
            const bool tiledPlayback = playbackTilesEligibleLocked(
                visibleStart, visibleEnd);
            const bool currentDetailReady =
                detailCoversRangeLocked(visibleStart, visibleEnd)
                && detailResolutionCoversLocked(visibleStart, visibleEnd)
                && m_detail.framesPerPoint > 0;
            if (tiledPlayback && currentDetailReady) {
                preparePlaybackTilesLocked(
                    visibleStart, visibleEnd, canvasHeight);
                playbackTilesRendered = renderMissingPlaybackTilesLocked(
                    visibleStart,
                    visibleEnd,
                    canvasHeight,
                    playbackTileRenderBudgetLocked(
                        visibleStart, visibleEnd));
            }
            if (tiledPlayback) {
                playbackTilesComplete = playbackTilesCoverLocked(
                    visibleStart, visibleEnd);
                playbackTilePaints = playbackTilePaintsLocked(
                    visibleStart, visibleEnd, canvasWidth);
            }
            if (!playbackTilesComplete) {
                rebuildCacheLocked(renderPixelWidthLocked(), canvasHeight);
                cache = m_cache;
            }
        }
        hover = m_hoverPosition;
        hoverActive = m_hoverActive;
        crosshair = m_crosshairEnabled;
        position = displayedPositionSecondsLocked();
        cacheStart = m_cacheStartSeconds;
        cacheSecondsPerPixel = m_cacheSecondsPerPixel;
        channels = displayedChannelCountLocked();
        showFps = m_showFpsOverlay;
        stagingContinues = !m_stagedCache.isNull();
        if (showFps) updateFpsEstimateLocked();
        fpsValue = m_fpsValue;
    }
    if (!directDetail) {
        bool cachePaintCoversCanvas = false;
        bool cachePainted = false;
        if (playbackTilesComplete) {
            painter->fillRect(
                QRect(0, 0, canvasWidth, canvasHeight), kBackground);
            cachePaintCoversCanvas = true;
            cachePainted = true;
        } else if (cacheSecondsPerPixel > 0.0 && visibleEnd > visibleStart) {
            const double sourceX = (visibleStart - cacheStart)
                / cacheSecondsPerPixel;
            const double sourceWidth = (visibleEnd - visibleStart)
                / cacheSecondsPerPixel;
            const QRectF requestedSource(
                sourceX, 0.0, sourceWidth, cache.height());
            const QRectF clippedSource = requestedSource.intersected(
                QRectF(0.0, 0.0, cache.width(), cache.height()));
            if (!clippedSource.isEmpty()) {
                cachePaintCoversCanvas = clippedSource == requestedSource;
                if (!cachePaintCoversCanvas) {
                    painter->fillRect(
                        QRect(0, 0, canvasWidth, canvasHeight), kBackground);
                }
                const double destinationScale = static_cast<double>(canvasWidth)
                    / sourceWidth;
                const QRectF destination(
                    (clippedSource.left() - sourceX) * destinationScale,
                    0.0,
                    clippedSource.width() * destinationScale,
                    canvasHeight);
                painter->drawImage(destination, cache, clippedSource);
                cachePainted = true;
            }
        }
        if (!cachePainted) {
            painter->fillRect(
                QRect(0, 0, canvasWidth, canvasHeight), kBackground);
        }
        for (const PlaybackTilePaint &tile : playbackTilePaints) {
            painter->drawImage(tile.target, tile.image);
        }
    }
    painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
    {
        QMutexLocker lock(&m_stateMutex);
        drawGridLocked(
            *painter, canvasWidth, canvasHeight, visibleStart, visibleEnd, channels);
        drawChannelSeparators(*painter, canvasWidth, canvasHeight, channels);
    }
    const double span = visibleEnd - visibleStart;
    if (span > 0.0 && position >= visibleStart && position <= visibleEnd) {
        const int x = static_cast<int>(std::round((position - visibleStart) / span * (canvasWidth - 1)));
        QPen playheadPen(kPlayhead);
        playheadPen.setWidth(0);
        painter->setPen(playheadPen);
        painter->setCompositionMode(QPainter::CompositionMode_Difference);
        painter->drawLine(x, 0, x, canvasHeight - 1);
        painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    if (crosshair && hoverActive) {
        QMutexLocker lock(&m_stateMutex);
        m_hoverPosition = hover;
        drawCrosshair(*painter, canvasWidth, canvasHeight, visibleStart, visibleEnd);
    }
    if (showFps && fpsValue > 0) drawFpsOverlay(*painter, canvasWidth, fpsValue);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (profileEnabled) {
        const auto profilePaintEnded = std::chrono::steady_clock::now();
        const double paintMs = std::chrono::duration<double, std::milli>(
            profilePaintEnded - profilePaintStarted).count();
        bool logSpike = false;
        bool logSummary = false;
        double summarySeconds = 0.0;
        quint64 summaryPaints = 0;
        quint64 summaryDirectPaints = 0;
        quint64 summaryCachedPaints = 0;
        quint64 summaryCacheRebuilds = 0;
        quint64 summaryStagedCacheStarts = 0;
        quint64 summaryStagedCacheSwaps = 0;
        quint64 summaryDetailRequests = 0;
        quint64 summaryDetailHandoffs = 0;
        double summaryPaintMs = 0.0;
        double summaryMaximumPaintMs = 0.0;
        double summaryCacheRebuildMs = 0.0;
        double summaryMaximumCacheRebuildMs = 0.0;
        double summaryStagedCacheMs = 0.0;
        double summaryMaximumStagedCacheMs = 0.0;
        double summaryMaximumFrameGapMs = 0.0;
        double summaryDecodeMs = 0.0;
        double summaryMaximumDecodeMs = 0.0;
        quint64 summaryTileRenders = 0;
        double summaryTileRenderMs = 0.0;
        double summaryMaximumTileRenderMs = 0.0;
        double profileRebuildMs = 0.0;
        int profileCacheWidth = 0;
        int profileCacheHeight = 0;
        {
            QMutexLocker lock(&m_stateMutex);
            ++m_profile.paints;
            if (directDetail) {
                ++m_profile.directPaints;
            } else {
                ++m_profile.cachedPaints;
            }
            m_profile.paintMs += paintMs;
            m_profile.maximumPaintMs = std::max(
                m_profile.maximumPaintMs, paintMs);
            logSpike = paintMs >= 3.0
                && shouldLogProfileSpike(
                    &m_profile.lastPaintSpike, profilePaintEnded);
            summarySeconds = std::chrono::duration<double>(
                profilePaintEnded - m_profile.lastSummary).count();
            logSummary = summarySeconds >= 1.0;
            profileRebuildMs = profileRebuiltCache
                ? m_profile.lastCacheRebuildMs
                : 0.0;
            profileCacheWidth = m_cache.width();
            profileCacheHeight = m_cache.height();
            if (logSummary) {
                summaryPaints = m_profile.paints;
                summaryDirectPaints = m_profile.directPaints;
                summaryCachedPaints = m_profile.cachedPaints;
                summaryCacheRebuilds = m_profile.cacheRebuilds;
                summaryStagedCacheStarts = m_profile.stagedCacheStarts;
                summaryStagedCacheSwaps = m_profile.stagedCacheSwaps;
                summaryDetailRequests = m_profile.detailRequests;
                summaryDetailHandoffs = m_profile.detailHandoffs;
                summaryPaintMs = m_profile.paintMs;
                summaryMaximumPaintMs = m_profile.maximumPaintMs;
                summaryCacheRebuildMs = m_profile.cacheRebuildMs;
                summaryMaximumCacheRebuildMs = m_profile.maximumCacheRebuildMs;
                summaryStagedCacheMs = m_profile.stagedCacheMs;
                summaryMaximumStagedCacheMs = m_profile.maximumStagedCacheMs;
                summaryMaximumFrameGapMs = m_profile.maximumFrameGapMs;
                summaryDecodeMs = m_profile.decodeMs;
                summaryMaximumDecodeMs = m_profile.maximumDecodeMs;
                summaryTileRenders = m_profile.tileRenders;
                summaryTileRenderMs = m_profile.tileRenderMs;
                summaryMaximumTileRenderMs = m_profile.maximumTileRenderMs;
                m_profile.lastSummary = profilePaintEnded;
                m_profile.paints = 0;
                m_profile.directPaints = 0;
                m_profile.cachedPaints = 0;
                m_profile.cacheRebuilds = 0;
                m_profile.stagedCacheStarts = 0;
                m_profile.stagedCacheSwaps = 0;
                m_profile.detailRequests = 0;
                m_profile.detailHandoffs = 0;
                m_profile.paintMs = 0.0;
                m_profile.maximumPaintMs = 0.0;
                m_profile.cacheRebuildMs = 0.0;
                m_profile.maximumCacheRebuildMs = 0.0;
                m_profile.stagedCacheMs = 0.0;
                m_profile.maximumStagedCacheMs = 0.0;
                m_profile.maximumFrameGapMs = 0.0;
                m_profile.decodeMs = 0.0;
                m_profile.maximumDecodeMs = 0.0;
                m_profile.tileRenders = 0;
                m_profile.tileRenderMs = 0.0;
                m_profile.maximumTileRenderMs = 0.0;
            }
        }
        if (logSpike) {
            std::fprintf(
                stderr,
                "[ui-waveform-editor] paint_spike ms=%.3f zoom=%.3f span=%.6f mode=%s visible_points=%d detail_points=%d frames_per_point=%u fallback=%d cache_rebuilt=%d rebuild_ms=%.3f stage_ms=%.3f stage_columns=%d tiles_rendered=%d cache=%dx%d viewport=%dx%d\n",
                paintMs,
                profileZoom,
                profileSpan,
                directDetail ? "direct" : "cached",
                profileVisiblePoints,
                profileDetailPoints,
                profileFramesPerPoint,
                profileOverviewFallback ? 1 : 0,
                profileRebuiltCache ? 1 : 0,
                profileRebuildMs,
                profileStagedCacheMs,
                profileStagedCacheColumns,
                playbackTilesRendered,
                profileCacheWidth,
                profileCacheHeight,
                canvasWidth,
                canvasHeight);
        }
        if (logSummary) {
            std::fprintf(
                stderr,
                "[ui-waveform-editor] summary seconds=%.3f paints=%llu direct=%llu cached=%llu paint_ms=%.3f avg_ms=%.3f max_ms=%.3f rebuilds=%llu rebuild_ms=%.3f rebuild_max_ms=%.3f stage_starts=%llu stage_swaps=%llu stage_ms=%.3f stage_max_ms=%.3f tile_renders=%llu tile_ms=%.3f tile_max_ms=%.3f frame_gap_max_ms=%.3f requests=%llu handoffs=%llu decode_ms=%.3f decode_max_ms=%.3f zoom=%.3f span=%.6f\n",
                summarySeconds,
                static_cast<unsigned long long>(summaryPaints),
                static_cast<unsigned long long>(summaryDirectPaints),
                static_cast<unsigned long long>(summaryCachedPaints),
                summaryPaintMs,
                summaryPaints > 0
                    ? summaryPaintMs / static_cast<double>(summaryPaints)
                    : 0.0,
                summaryMaximumPaintMs,
                static_cast<unsigned long long>(summaryCacheRebuilds),
                summaryCacheRebuildMs,
                summaryMaximumCacheRebuildMs,
                static_cast<unsigned long long>(summaryStagedCacheStarts),
                static_cast<unsigned long long>(summaryStagedCacheSwaps),
                summaryStagedCacheMs,
                summaryMaximumStagedCacheMs,
                static_cast<unsigned long long>(summaryTileRenders),
                summaryTileRenderMs,
                summaryMaximumTileRenderMs,
                summaryMaximumFrameGapMs,
                static_cast<unsigned long long>(summaryDetailRequests),
                static_cast<unsigned long long>(summaryDetailHandoffs),
                summaryDecodeMs,
                summaryMaximumDecodeMs,
                profileZoom,
                profileSpan);
        }
    }
#endif
    if (presentationCommitted || stagingContinues) {
        queueGuiContinuationFromPaint(presentationCommitted, stagingContinues);
    }
}

bool WaveformEditorItem::playbackTilesEligibleLocked(
    double visibleStart, double visibleEnd) const {
    if (!m_playing
        || m_presentedZoomLevel <= 1.0001
        || renderDetailDirectlyLocked(visibleStart, visibleEnd)) {
        return false;
    }
    const bool detailReady = detailCoversRangeLocked(visibleStart, visibleEnd)
        && detailResolutionCoversLocked(visibleStart, visibleEnd)
        && m_detail.framesPerPoint > 0;
    if (detailReady) return true;

    // Keep complete presentation tiles alive while the next detail window is
    // in flight. Falling back to the monolithic cache here would briefly show
    // its coarser level between two otherwise compatible tile presentations.
    const int renderWidth = renderPixelWidthLocked();
    const double secondsPerPixel = (visibleEnd - visibleStart)
        / static_cast<double>(std::max(1, renderWidth));
    const double tolerance = std::max(1.0e-12, secondsPerPixel * 1.0e-7);
    return !m_playbackTiles.empty()
        && std::abs(m_playbackTileSecondsPerPixel - secondsPerPixel)
            <= tolerance
        && m_playbackTileHeight
            == std::max(1, static_cast<int>(std::floor(height())))
        && m_playbackTileDisplayedChannels == displayedChannelCountLocked()
        && playbackTilesCoverLocked(visibleStart, visibleEnd);
}

void WaveformEditorItem::clearPlaybackTilesLocked() {
    m_playbackTiles.clear();
    m_playbackTileSecondsPerPixel = 0.0;
    m_playbackTileHeight = 0;
    m_playbackTileFramesPerPoint = 0;
    m_playbackTileDisplayedChannels = 0;
}

void WaveformEditorItem::preparePlaybackTilesLocked(
    double visibleStart, double visibleEnd, int height) {
    const int renderWidth = renderPixelWidthLocked();
    const double secondsPerPixel = (visibleEnd - visibleStart)
        / static_cast<double>(std::max(1, renderWidth));
    const int channels = displayedChannelCountLocked();
    const double tolerance = std::max(
        1.0e-12, secondsPerPixel * 1.0e-7);
    const bool compatible = !m_playbackTiles.empty()
        && std::abs(m_playbackTileSecondsPerPixel - secondsPerPixel)
            <= tolerance
        && m_playbackTileHeight == height
        && m_playbackTileFramesPerPoint == m_detail.framesPerPoint
        && m_playbackTileDisplayedChannels == channels;
    if (!compatible) {
        clearPlaybackTilesLocked();
        m_playbackTileSecondsPerPixel = secondsPerPixel;
        m_playbackTileHeight = height;
        m_playbackTileFramesPerPoint = m_detail.framesPerPoint;
        m_playbackTileDisplayedChannels = channels;
    }
}

int WaveformEditorItem::renderMissingPlaybackTilesLocked(
    double visibleStart, double visibleEnd, int height, int tileBudget) {
    if (tileBudget <= 0 || m_playbackTileSecondsPerPixel <= 0.0) {
        return 0;
    }
    const double tileDuration = m_playbackTileSecondsPerPixel
        * static_cast<double>(kPlaybackTileWidth);
    if (tileDuration <= 0.0) return 0;

    const qint64 firstVisible = static_cast<qint64>(std::floor(
        visibleStart / tileDuration + kGridAlignmentEpsilon));
    const qint64 lastVisible = static_cast<qint64>(std::floor(
        std::max(visibleStart, visibleEnd - 1.0e-12) / tileDuration));
    const qint64 firstWanted = firstVisible - kPlaybackTilePrefetch;
    const qint64 lastWanted = lastVisible + kPlaybackTilePrefetch;

    for (auto iterator = m_playbackTiles.begin();
         iterator != m_playbackTiles.end();) {
        if (iterator->first < firstWanted - kPlaybackTilePrefetch
            || iterator->first > lastWanted + kPlaybackTilePrefetch) {
            iterator = m_playbackTiles.erase(iterator);
        } else {
            ++iterator;
        }
    }

    std::vector<qint64> candidates;
    candidates.reserve(static_cast<std::size_t>(
        std::max<qint64>(0, lastWanted - firstWanted + 1)));
    for (qint64 tile = firstVisible; tile <= lastVisible; ++tile) {
        candidates.push_back(tile);
    }
    for (int margin = 1; margin <= kPlaybackTilePrefetch; ++margin) {
        candidates.push_back(firstVisible - margin);
        candidates.push_back(lastVisible + margin);
    }

    int rendered = 0;
    for (const qint64 tileIndex : candidates) {
        if (rendered >= tileBudget
            || m_playbackTiles.contains(tileIndex)) {
            continue;
        }
        const double tileStart = static_cast<double>(tileIndex) * tileDuration;
        const double tileEnd = tileStart + tileDuration;
        const double contentStart = std::max(0.0, tileStart);
        const double contentEnd = std::min(m_durationSeconds, tileEnd);
        if (contentEnd <= contentStart
            || !detailCoversRangeLocked(contentStart, contentEnd)) {
            continue;
        }

        const int firstX = std::clamp(
            static_cast<int>(std::floor(
                (contentStart - tileStart) / tileDuration
                * static_cast<double>(kPlaybackTileWidth))),
            0,
            kPlaybackTileWidth);
        const int lastX = std::clamp(
            static_cast<int>(std::ceil(
                (contentEnd - tileStart) / tileDuration
                * static_cast<double>(kPlaybackTileWidth))),
            firstX,
            kPlaybackTileWidth);
        const int contentPixelWidth = std::max(1, lastX - firstX);
        if (!detailResolutionCoversPixelSpanLocked(
                contentStart, contentEnd, contentPixelWidth)) {
            continue;
        }

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        const auto tileStarted = std::chrono::steady_clock::now();
#endif
        QImage tileImage(
            kPlaybackTileWidth, height, QImage::Format_RGB32);
        tileImage.fill(kBackground);
        QPainter tilePainter(&tileImage);
        tilePainter.setRenderHint(QPainter::Antialiasing, false);
        drawDetailSliceLocked(
            tilePainter,
            kPlaybackTileWidth,
            height,
            tileStart,
            tileEnd,
            m_playbackTileDisplayedChannels,
            firstX,
            lastX);
        tilePainter.end();
        m_playbackTiles.emplace(
            tileIndex, PlaybackTile{std::move(tileImage)});
        ++rendered;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        if (m_profile.enabled) {
            const double tileMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tileStarted).count();
            ++m_profile.tileRenders;
            m_profile.tileRenderMs += tileMs;
            m_profile.maximumTileRenderMs = std::max(
                m_profile.maximumTileRenderMs, tileMs);
        }
#endif
    }
    return rendered;
}

int WaveformEditorItem::playbackTileRenderBudgetLocked(
    double visibleStart, double visibleEnd) const {
    if (m_playbackTileSecondsPerPixel <= 0.0 || visibleEnd <= visibleStart) {
        return kPlaybackTilesPerPaint;
    }
    const double tileDuration = m_playbackTileSecondsPerPixel
        * static_cast<double>(kPlaybackTileWidth);
    const qint64 first = static_cast<qint64>(std::floor(
        visibleStart / tileDuration + kGridAlignmentEpsilon));
    const qint64 last = static_cast<qint64>(std::floor(
        std::max(visibleStart, visibleEnd - 1.0e-12) / tileDuration));
    int missing = 0;
    for (qint64 tile = first; tile <= last; ++tile) {
        if (!m_playbackTiles.contains(tile)) ++missing;
    }
    return std::clamp(
        missing, kPlaybackTilesPerPaint, kMaximumPlaybackTilesPerPaint);
}

bool WaveformEditorItem::playbackTilesCoverLocked(
    double visibleStart, double visibleEnd) const {
    if (m_playbackTileSecondsPerPixel <= 0.0 || visibleEnd <= visibleStart) {
        return false;
    }
    const double tileDuration = m_playbackTileSecondsPerPixel
        * static_cast<double>(kPlaybackTileWidth);
    const qint64 first = static_cast<qint64>(std::floor(
        visibleStart / tileDuration + kGridAlignmentEpsilon));
    const qint64 last = static_cast<qint64>(std::floor(
        std::max(visibleStart, visibleEnd - 1.0e-12) / tileDuration));
    for (qint64 tile = first; tile <= last; ++tile) {
        if (!m_playbackTiles.contains(tile)) return false;
    }
    return true;
}

std::vector<WaveformEditorItem::PlaybackTilePaint>
WaveformEditorItem::playbackTilePaintsLocked(
    double visibleStart, double visibleEnd, int canvasWidth) const {
    std::vector<PlaybackTilePaint> paints;
    if (m_playbackTileSecondsPerPixel <= 0.0
        || visibleEnd <= visibleStart || canvasWidth <= 0) {
        return paints;
    }
    const double visibleSpan = visibleEnd - visibleStart;
    const double tileDuration = m_playbackTileSecondsPerPixel
        * static_cast<double>(kPlaybackTileWidth);
    const qint64 first = static_cast<qint64>(std::floor(
        visibleStart / tileDuration + kGridAlignmentEpsilon));
    const qint64 last = static_cast<qint64>(std::floor(
        std::max(visibleStart, visibleEnd - 1.0e-12) / tileDuration));
    paints.reserve(static_cast<std::size_t>(std::max<qint64>(0, last - first + 1)));
    for (qint64 tileIndex = first; tileIndex <= last; ++tileIndex) {
        const auto iterator = m_playbackTiles.find(tileIndex);
        if (iterator == m_playbackTiles.end()) continue;
        const double tileStart = static_cast<double>(tileIndex) * tileDuration;
        paints.push_back(PlaybackTilePaint{
            iterator->second.image,
            QRectF(
                (tileStart - visibleStart) / visibleSpan
                    * static_cast<double>(canvasWidth),
                0.0,
                tileDuration / visibleSpan
                    * static_cast<double>(canvasWidth),
                static_cast<double>(m_playbackTileHeight)),
        });
    }
    return paints;
}

void WaveformEditorItem::rebuildCacheLocked(int width, int height) {
    if (!m_cacheDirty && m_cachedViewportWidth == width && m_cachedViewportHeight == height) return;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const auto profileRebuildStarted = std::chrono::steady_clock::now();
#endif
    const auto [visibleStart, visibleEnd] = visibleRangeLocked();
    const bool detailCoversViewport = detailCoversRangeLocked(
        visibleStart, visibleEnd);
    const bool detailReady = detailCoversViewport
        && detailResolutionCoversLocked(visibleStart, visibleEnd);
    const bool detailOverlapsViewport = m_detail.pointCount > 0
        && visibleStart < m_detail.endSeconds
        && visibleEnd > m_detail.startSeconds;
    const bool useOverview = m_zoomFallbackToOverview
        || (detailCoversViewport && !detailReady)
        || !detailOverlapsViewport;
    CacheGrid grid;
    if (detailReady) {
        grid = cacheGridForRangeLocked(
            visibleStart, visibleEnd,
            m_detail.startSeconds, m_detail.endSeconds);
    } else if (useOverview) {
        grid = cacheGridForRangeLocked(
            visibleStart, visibleEnd, 0.0, m_durationSeconds);
    } else {
        grid = cacheGridForRangeLocked(
            visibleStart, visibleEnd,
            m_detail.startSeconds, m_detail.endSeconds);
    }
    if (m_cache.size() != QSize(grid.width, height)
        || m_cache.format() != QImage::Format_RGB32) {
        m_cache = QImage(grid.width, height, QImage::Format_RGB32);
    }
    m_cache.fill(kBackground);
    QPainter painter(&m_cache);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const int channels = displayedChannelCountLocked();
    if (detailReady) {
        drawDetailLocked(
            painter, grid.width, height,
            grid.startSeconds, grid.endSeconds, channels);
    } else if (useOverview) {
        drawOverviewLocked(
            painter, grid.width, height,
            grid.startSeconds, grid.endSeconds, channels);
    } else {
        drawDetailLocked(
            painter, grid.width, height,
            grid.startSeconds, grid.endSeconds, channels);
    }
    painter.end();
    m_cachedViewportWidth = width;
    m_cachedViewportHeight = height;
    m_cacheStartSeconds = grid.startSeconds;
    m_cacheEndSeconds = grid.endSeconds;
    m_cacheSecondsPerPixel = grid.secondsPerPixel;
    m_cacheFramesPerPoint = detailReady ? m_detail.framesPerPoint : 0;
    m_cacheDisplayedChannels = channels;
    m_cacheDirty = false;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profile.enabled) {
        const double rebuildMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - profileRebuildStarted).count();
        ++m_profile.cacheRebuilds;
        m_profile.cacheRebuildMs += rebuildMs;
        m_profile.maximumCacheRebuildMs = std::max(
            m_profile.maximumCacheRebuildMs, rebuildMs);
        m_profile.lastCacheRebuildMs = rebuildMs;
    }
#endif
}

void WaveformEditorItem::drawChannelSeparators(
    QPainter &painter, int width, int height, int channels) {
    if (channels < 2) return;
    painter.setPen(QPen(kChannelSeparator, 1.0));
    for (int channel = 1; channel < channels; ++channel) {
        const int y = static_cast<int>(std::round(
            static_cast<double>(channel) * height / channels));
        painter.drawLine(0, y, width - 1, y);
    }
}

QPainterPath WaveformEditorItem::buildSamplePath(const QPolygonF &samples) {
    QPainterPath path;
    if (samples.isEmpty()) return path;
    path.moveTo(samples.constFirst());
    if (samples.size() == 1) return path;
    for (qsizetype index = 0; index + 1 < samples.size(); ++index) {
        const QPointF p0 = index > 0 ? samples.at(index - 1) : samples.at(index);
        const QPointF p1 = samples.at(index);
        const QPointF p2 = samples.at(index + 1);
        const QPointF p3 = index + 2 < samples.size()
            ? samples.at(index + 2)
            : samples.at(index + 1);
        const QPointF control1 = p1 + (p2 - p0) / 6.0;
        const QPointF control2 = p2 - (p3 - p1) / 6.0;
        path.cubicTo(control1, control2, p2);
    }
    return path;
}

QString WaveformEditorItem::formatCrosshairTime(double seconds) {
    return formatTimePrecise(seconds);
}

std::pair<QRect, QRect> WaveformEditorItem::crosshairLabelRects(
    int width, int height, int x, int y,
    int valueTextWidth, int timeTextWidth, int textHeight) {
    constexpr int padding = 3;
    const QRect valueRect(
        width - valueTextWidth - 2 * padding - 2,
        std::clamp(y - textHeight / 2, 0, height - textHeight - 2 * padding),
        valueTextWidth + 2 * padding,
        textHeight + 2 * padding);
    const QRect timeRect(
        std::clamp(x - timeTextWidth / 2, 0, width - timeTextWidth - 2 * padding),
        height - textHeight - 2 * padding - 2,
        timeTextWidth + 2 * padding,
        textHeight + 2 * padding);
    return {valueRect, timeRect};
}

void WaveformEditorItem::drawGridLocked(QPainter &painter, int width, int height,
                                         double visibleStart, double visibleEnd, int channels) const {
    const QPainter::CompositionMode originalComposition =
        painter.compositionMode();
    painter.setCompositionMode(QPainter::CompositionMode_Difference);
    painter.setPen(QPen(kCenterLine, 1.0));
    for (int channel = 0; channel < channels; ++channel) {
        const double top = static_cast<double>(channel) * height / channels;
        const double bottom = static_cast<double>(channel + 1) * height / channels;
        painter.drawLine(
            0,
            static_cast<int>((top + bottom) * 0.5),
            width - 1,
            static_cast<int>((top + bottom) * 0.5));
    }
    painter.setCompositionMode(originalComposition);
    if (!m_gridEnabled) return;

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
            painter.drawLine(x, 0, x, height - 1);
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
            painter.drawLine(0, upper, width - 1, upper);
            painter.drawLine(0, lower, width - 1, lower);
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
    const double secondsPerPixel = span
        / static_cast<double>(std::max(1, width));
    for (int channel = 0; channel < channels; ++channel) {
        const double top = static_cast<double>(channel) * height / channels;
        const double bottom = static_cast<double>(channel + 1) * height / channels;
        const double center = (top + bottom) * 0.5;
        const double half = (bottom - top) * 0.5 - 1.0;
        painter.setPen(QPen(channelIsMutedLocked(channel) ? kMutedWaveform : kWaveform, 1.0));
        for (int x = 0; x < width; ++x) {
            const double time = visibleStart
                + (static_cast<double>(x) + 0.5) * secondsPerPixel;
            const int index = std::clamp(static_cast<int>(time / m_durationSeconds * count), 0, count - 1);
            const double peak = static_cast<double>(peaks[index]) / 255.0;
            painter.drawLine(x, static_cast<int>(center - peak * half), x, static_cast<int>(center + peak * half));
        }
    }
}

void WaveformEditorItem::drawDetailLocked(QPainter &painter, int width, int height,
                                           double visibleStart, double visibleEnd, int channels) const {
    drawDetailSliceLocked(
        painter, width, height, visibleStart, visibleEnd, channels, 0, width);
}

void WaveformEditorItem::drawDetailSliceLocked(
    QPainter &painter, int width, int height,
    double visibleStart, double visibleEnd, int channels,
    int firstX, int lastX) const {
    const double detailSpan = m_detail.endSeconds - m_detail.startSeconds;
    const double visibleSpan = visibleEnd - visibleStart;
    firstX = std::clamp(firstX, 0, width);
    lastX = std::clamp(lastX, firstX, width);
    if (detailSpan <= 0.0 || visibleSpan <= 0.0 || firstX >= lastX) return;
    const bool points = samplePointsVisibleLocked();
    const double secondsPerPixel = visibleSpan
        / static_cast<double>(std::max(1, width));
    const double sliceStart = visibleStart
        + static_cast<double>(std::max(0, firstX - 1)) * secondsPerPixel;
    const double sliceEnd = visibleStart
        + static_cast<double>(std::min(width, lastX + 1)) * secondsPerPixel;
    const auto [firstPoint, lastPoint] = detailPointRangeLocked(
        sliceStart, sliceEnd);
    const int sliceWidth = lastX - firstX;
    for (int displayChannel = 0; displayChannel < channels; ++displayChannel) {
        const double top = static_cast<double>(displayChannel) * height / channels;
        const double bottom = static_cast<double>(displayChannel + 1) * height / channels;
        const double center = (top + bottom) * 0.5;
        const double half = (bottom - top) * 0.5 - 1.0;
        const QColor color = channelIsMutedLocked(displayChannel) ? kMutedWaveform : kWaveform;
        painter.setPen(QPen(color, points ? 1.2 : 1.0));
        QPolygonF polyline;
        std::vector<float> pixelMinima;
        std::vector<float> pixelMaxima;
        std::vector<bool> pixelSeen;
        if (points) {
            polyline.reserve(lastPoint - firstPoint);
        } else {
            pixelMinima.assign(static_cast<std::size_t>(sliceWidth), 1.0F);
            pixelMaxima.assign(static_cast<std::size_t>(sliceWidth), -1.0F);
            pixelSeen.assign(static_cast<std::size_t>(sliceWidth), false);
        }
        for (int point = firstPoint; point < lastPoint; ++point) {
            const double time = detailPointTimeLocked(point);
            if (time < visibleStart || time >= visibleEnd) continue;
            const double pixelPosition = (time - visibleStart)
                / secondsPerPixel;
            const int x = std::clamp(
                points
                    ? static_cast<int>(std::round(pixelPosition))
                    : static_cast<int>(std::floor(
                        pixelPosition + kGridAlignmentEpsilon)),
                0, width - 1);
            if (x < firstX || x >= lastX) continue;
            float minimum = 1.0F;
            float maximum = -1.0F;
            const int firstChannel = m_viewMode == 0 ? 0 : displayChannel;
            const int lastChannel = m_viewMode == 0 ? m_detail.channelCount : displayChannel + 1;
            for (int sourceChannel = firstChannel; sourceChannel < lastChannel; ++sourceChannel) {
                const std::size_t index = (static_cast<std::size_t>(point) * m_detail.channelCount + sourceChannel) * 2U;
                minimum = std::min(minimum, m_detail.extrema[index]);
                maximum = std::max(maximum, m_detail.extrema[index + 1]);
            }
            if (points) {
                polyline.append(QPointF(
                    x, center - static_cast<double>(minimum) * half));
            } else {
                const auto pixel = static_cast<std::size_t>(x - firstX);
                pixelMinima[pixel] = std::min(pixelMinima[pixel], minimum);
                pixelMaxima[pixel] = std::max(pixelMaxima[pixel], maximum);
                pixelSeen[pixel] = true;
            }
        }
        if (!points) {
            for (int x = firstX; x < lastX; ++x) {
                const auto pixel = static_cast<std::size_t>(x - firstX);
                if (!pixelSeen[pixel]) continue;
                painter.drawLine(
                    x,
                    static_cast<int>(std::round(
                        center - static_cast<double>(pixelMinima[pixel]) * half)),
                    x,
                    static_cast<int>(std::round(
                        center - static_cast<double>(pixelMaxima[pixel]) * half)));
            }
        }
        if (points && polyline.size() > 1) {
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(buildSamplePath(polyline));
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
    QPen pen(QColor(255, 255, 255, 140));
    pen.setStyle(Qt::DotLine);
    pen.setWidthF(1.0);
    painter.setPen(pen);
    painter.drawLine(x, 0, x, height - 1); painter.drawLine(0, y, width - 1, y);
    const double time = visibleStart + static_cast<double>(x) / std::max(1, width - 1) * (visibleEnd - visibleStart);
    const int channels = displayedChannelCountLocked();
    const double channelHeight = static_cast<double>(height) / channels;
    const double localY = std::fmod(static_cast<double>(y), channelHeight);
    const double amplitude = std::clamp(std::abs((channelHeight * 0.5 - localY) / (channelHeight * 0.5)), 0.0, 1.0);
    const QString dbText = amplitude <= 0.00001 ? QStringLiteral("-∞ dB") : QStringLiteral("%1 dB").arg(20.0 * std::log10(amplitude), 0, 'f', 1);
    const QString timeText = formatCrosshairTime(time);
    QFont font;
    font.setPixelSize(10);
    painter.setFont(font);
    const QFontMetrics metrics(font);
    const int dbWidth = metrics.horizontalAdvance(dbText);
    const int timeWidth = metrics.horizontalAdvance(timeText);
    const int textHeight = metrics.height();
    const auto [dbRect, timeRect] = crosshairLabelRects(
        width, height, x, y, dbWidth, timeWidth, textHeight);
    constexpr int padding = 3;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 10, 160));
    painter.drawRoundedRect(dbRect, 3, 3);
    painter.drawRoundedRect(timeRect, 3, 3);
    painter.setPen(QColor(255, 255, 255, 220));
    painter.drawText(
        dbRect.x() + padding,
        dbRect.y() + padding + metrics.ascent(),
        dbText);
    painter.drawText(
        timeRect.x() + padding,
        timeRect.bottom() - padding - metrics.descent(),
        timeText);
}

void WaveformEditorItem::drawFpsOverlay(QPainter &painter, int width, int fpsValue) {
    QFont font;
    font.setPixelSize(10);
    painter.setFont(font);
    const QString text = QStringLiteral("%1 fps").arg(fpsValue);
    const QFontMetrics metrics(font);
    painter.setPen(kOverlay);
    painter.drawText(
        std::max(4, width - metrics.horizontalAdvance(text) - 8),
        metrics.ascent() + 4,
        text);
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
