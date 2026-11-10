// SPDX-License-Identifier: GPL-3.0-or-later

#include "WaveformEditorItem.h"
#include "WaveformSceneGraph.h"

#include "CrosshairReadout.h"
#include "FerrousBridgeFfi.h"
#include "ViewCorner.h"

#include <KLocalizedString>
#include <QDataStream>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QQuickWindow>
#include <QScreen>
#include <QSGRendererInterface>
#include <QThreadPool>
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
constexpr double kSampleCurvePixelsPerSample = 0.055;
constexpr double kSampleMarkerPixelsPerSample = 2.0;
// Samples with markers are drawn directly, as geometry: playing, a view
// of them moves on by most of its width a frame, and rasterizing its
// markers into playback tiles again each frame costs several times as much.
constexpr double kDirectSamplePixelsPerSample = kSampleMarkerPixelsPerSample;
constexpr double kMinimumSampleMarkerSize = 1.5;
constexpr double kMaximumSampleMarkerSize = 3.0;
constexpr double kSampleCurveSubpixelScale = 256.0;
constexpr int kSampleCurveBoundaryMarginPixels = 8;
// The samples either side of a stroke's own that its segments' joins and
// curves take shape from: a curve between two samples from the samples
// either side of them, and its normals at its ends from the curves either
// side of it.
constexpr int kSampleCurveContextSamples = 3;
constexpr double kMinimumDetailMarginSeconds = 0.75;
// While playing, the share of a detail request's margins that lies behind
// the view: the view moves on into the rest, ahead of it.
constexpr double kPlaybackDetailMarginBehind = 0.1;
// How much of a margin may be used up before more detail is asked for.
constexpr double kDetailMarginSafety = 0.6;
constexpr int kStagedCacheColumnsPerPaint = 128;
constexpr int kPlaybackTileWidth = 64;
// The tiles a paint makes ahead of the view, besides those it shows.
constexpr int kPlaybackTilesPerPaint = 2;
constexpr int kPlaybackTilePrefetch = 2;
// How far ahead of the view, in playback, workers make playback tiles.
constexpr double kPlaybackTileLookaheadSeconds = 0.05;
constexpr double kGridAlignmentEpsilon = 1.0e-9;
constexpr QColor kGrid(20, 82, 49);
constexpr QColor kOverlay(190, 190, 200, 180);

QFont fpsCounterFont() {
    QFont font;
    font.setPixelSize(10);
    return font;
}

// The workers that make playback tiles: half the processors, so that they
// leave room for decoding and the render thread. At exit, it waits for the
// tiles it is making.
struct PlaybackTilePool : QThreadPool {
    PlaybackTilePool() { setMaxThreadCount(std::max(2, QThread::idealThreadCount() / 2)); }
};
Q_GLOBAL_STATIC(PlaybackTilePool, playbackTilePool)

QString fpsCounterText(int fps) {
    static const QString format = i18nc("@item:intext frames per second", "%1 fps");
    return format.arg(fps);
}

// How far short of a column's edge a time is still on it: a fraction of a
// pixel, as a time mapped there and back can be.
constexpr double kColumnEpsilon = 1e-6;

// The view's time axis: `width` logical pixels from `start` to `end`, which
// is how the body is drawn. The playhead, the grid, the crosshair and seeks
// use the device columns, `scale` to a logical pixel: a time is shown on the
// column it falls in, at that column's left edge, and the pointer over a
// column shows and seeks to the time there.
struct TimeAxis {
    double start{0.0};
    double end{0.0};
    int width{1};
    double scale{1.0};

    int columns() const {
        return std::max(1, static_cast<int>(std::ceil(width * scale - 1e-6)));
    }
    // The column at `x` logical pixels; a fraction of a pixel short of a
    // column's edge, as a time mapped there and back can be, is on it.
    int columnAt(double x) const {
        return std::clamp(
            static_cast<int>(std::floor(x * scale + kColumnEpsilon)), 0, columns() - 1);
    }
    int columnOf(double time) const {
        return columnAt((time - start) / (end - start) * width);
    }
    // Where a column starts, in logical pixels, and the time there.
    double x(int column) const { return column / scale; }
    double timeAt(int column) const {
        return start + x(column) / width * (end - start);
    }
};

bool rasterCoversRange(double rasterStart, double rasterEnd,
                       double visibleStart, double visibleEnd) {
    // Converting time to a pixel grid and back can round a covered endpoint
    // just outside the raster. Allow a few floating-point rounding steps so
    // stationary views do not rebuild every frame; real pixel gaps still fail.
    const double magnitude = std::max({1.0, std::abs(rasterStart),
        std::abs(rasterEnd), std::abs(visibleStart), std::abs(visibleEnd)});
    const double tolerance = 8.0 * std::numeric_limits<double>::epsilon() * magnitude;
    return visibleStart >= rasterStart - tolerance
        && visibleEnd <= rasterEnd + tolerance;
}

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
    : QQuickItem(parent) {
    m_playbackTileWorkers->item = this;
    setFlag(ItemHasContents, true);
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::RightButton | Qt::MiddleButton);
    m_requestTimer.setSingleShot(true);
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
    connect(this, &QQuickItem::visibleChanged, this, &WaveformEditorItem::shownChanged);
    // An item made with a parent in a window already has it; no change follows.
    if (window()) bindWindowFrameLoop(window());
}

WaveformEditorItem::~WaveformEditorItem() {
    // ~QQuickItem takes the item out of its window, which emits
    // windowChanged once this item's members are gone: its handler, which
    // rebinds the window connections and reads the state, must not run then.
    disconnect(this, nullptr, this, nullptr);
    disconnect(m_frameSwappedConnection);
    disconnect(m_windowVisibilityConnection);
    disconnect(m_windowPresenceConnection);
    disconnect(m_screenConnection);
    disconnect(m_refreshRateConnection);
    cancelPlaybackFrame();
    if (m_decodeCancelled) m_decodeCancelled->store(true);
    {
        // A worker that has made a tile hands it over under this lock.
        QMutexLocker lock(&m_playbackTileWorkers->mutex);
        m_playbackTileWorkers->item = nullptr;
        m_playbackTileWorkers->generation.fetch_add(1);
    }
}

void WaveformEditorItem::invalidateDetailRequestLocked() {
    ++m_requestGeneration;
    if (m_decodeCancelled) m_decodeCancelled->store(true);
}

QString WaveformEditorItem::sourcePath() const { QMutexLocker lock(&m_stateMutex); return m_sourcePath; }
QByteArray WaveformEditorItem::overviewData() const { QMutexLocker lock(&m_stateMutex); return m_overviewData; }
PlaybackClock *WaveformEditorItem::clock() const { return m_clock; }
double WaveformEditorItem::positionSeconds() const { QMutexLocker lock(&m_stateMutex); return displayedPositionSecondsLocked(); }
double WaveformEditorItem::durationSeconds() const { QMutexLocker lock(&m_stateMutex); return m_durationSeconds; }
double WaveformEditorItem::zoomLevel() const { QMutexLocker lock(&m_stateMutex); return m_zoomLevel; }
bool WaveformEditorItem::playing() const { QMutexLocker lock(&m_stateMutex); return m_playing; }
bool WaveformEditorItem::zoomEnabled() const { QMutexLocker lock(&m_stateMutex); return m_zoomEnabled; }
bool WaveformEditorItem::gridEnabled() const { QMutexLocker lock(&m_stateMutex); return m_gridEnabled; }
bool WaveformEditorItem::crosshairEnabled() const { QMutexLocker lock(&m_stateMutex); return m_crosshairEnabled; }
bool WaveformEditorItem::showFpsOverlay() const { QMutexLocker lock(&m_stateMutex); return m_showFpsOverlay; }
QSizeF WaveformEditorItem::reservedCorner() const { QMutexLocker lock(&m_stateMutex); return m_reservedCorner; }
int WaveformEditorItem::viewMode() const { QMutexLocker lock(&m_stateMutex); return m_viewMode; }
qulonglong WaveformEditorItem::mutedChannelsMask() const { QMutexLocker lock(&m_stateMutex); return m_mutedChannelsMask; }
bool WaveformEditorItem::channelDrawnMuted(int channel) const { QMutexLocker lock(&m_stateMutex); return channelIsMutedLocked(channel); }
qulonglong WaveformEditorItem::soloedChannelsMask() const { QMutexLocker lock(&m_stateMutex); return m_soloedChannelsMask; }
int WaveformEditorItem::channelCountHint() const { QMutexLocker lock(&m_stateMutex); return m_channelCountHint; }
int WaveformEditorItem::channelCount() const { QMutexLocker lock(&m_stateMutex); return displayedChannelCountLocked(); }
int WaveformEditorItem::sampleRateHz() const { QMutexLocker lock(&m_stateMutex); return m_sampleRateHz; }
bool WaveformEditorItem::samplePointsVisible() const { QMutexLocker lock(&m_stateMutex); return samplePointsVisibleLocked(); }

void WaveformEditorItem::setSourcePath(const QString &value) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_sourcePath == value) return;
        m_sourcePath = value;
        invalidateDetailRequestLocked();
        clearPendingRequestLocked();
        clearDetailLocked();
        m_presentedZoomLevel = m_zoomLevel;
        m_zoomOutHandoffPending = false;
        m_zoomFallbackToOverview = false;
        invalidateCacheLocked();
    }
    // After its request for the previous file was cancelled.
    updateReaderLease();
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

double WaveformEditorItem::overviewCoverageSeconds() const {
    QMutexLocker lock(&m_stateMutex);
    return m_overviewCoverageSeconds;
}

bool WaveformEditorItem::overviewComplete() const {
    QMutexLocker lock(&m_stateMutex);
    return m_overviewComplete;
}

void WaveformEditorItem::setOverviewCoverageSeconds(double value) {
    value = std::isfinite(value) ? std::max(0.0, value) : 0.0;
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_overviewCoverageSeconds == value) return;
        m_overviewCoverageSeconds = value;
        if (!m_zoomOutHandoffPending) invalidateCacheLocked();
    }
    emit overviewCoverageChanged();
    update();
}

void WaveformEditorItem::setOverviewComplete(bool value) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_overviewComplete == value) return;
        m_overviewComplete = value;
        if (!m_zoomOutHandoffPending) invalidateCacheLocked();
    }
    emit overviewCoverageChanged();
    update();
}

void WaveformEditorItem::setClock(PlaybackClock *value) {
    if (m_clock == value) return;
    disconnect(m_clockConnection);
    m_clock = value;
    if (m_clock) {
        m_clockConnection = connect(m_clock, &PlaybackClock::stateChanged,
                                    this, &WaveformEditorItem::clockStateChanged);
    }
    emit clockChanged();
    clockStateChanged();
}

void WaveformEditorItem::clockStateChanged() {
    const PlaybackClockState state = m_clock ? m_clock->state() : PlaybackClockState{};
    bool request = false;
    bool cacheNeedsUpdate = false;
    bool courseCorrection = false;
    qint64 frameDelayNs = 0;
    const qint64 nowNs = m_nowNs();
    {
        QMutexLocker lock(&m_stateMutex);
        // A sample that only corrects the course of playback moves nothing
        // at once: the playback frames follow it.
        courseCorrection = m_playing
            && state.advancing()
            && m_clockState.advancing()
            && state.trackToken == m_clockState.trackToken
            && state.epoch == m_clockState.epoch
            && state.durationSeconds == m_clockState.durationSeconds;
        // A jump moves the view at once, and the detail of the old range is
        // no longer needed.
        const bool explicitSeek = state.epoch != m_clockState.epoch
            || state.trackToken != m_clockState.trackToken;
        const auto previousRange = visibleRangeLocked();
        m_clockState = state;
        const auto [start, end] = visibleRangeLocked();
        const auto [requestStart, requestEnd] = detailRequestVisibleRangeLocked();
        request = !detailOrPendingRequestCoversLocked(requestStart, requestEnd);
        if (explicitSeek && request) {
            invalidateDetailRequestLocked();
            clearPendingRequestLocked();
        }
        const bool rangeMoved = std::abs(previousRange.first - start) > 0.0001
            || std::abs(previousRange.second - end) > 0.0001;
        const bool tiledPlayback = playbackTilesEligibleLocked(start, end);
        // A jump, a pause or a resume redraws a cache that does not cover
        // the view at once; playback moving it on, as cacheRedrawDueLocked
        // says.
        cacheNeedsUpdate = !tiledPlayback
            && rangeMoved
            && (courseCorrection
                ? cacheRedrawDueLocked(start, end)
                : !m_zoomOutHandoffPending && !cacheCoversRangeLocked(start, end));
        if (cacheNeedsUpdate) invalidateCacheLocked();
        if (courseCorrection) frameDelayNs = playbackFrameDelayNsLocked(nowNs);
    }
    emit positionSecondsChanged();
    if (request) scheduleDetailRequest();
    if (courseCorrection) {
        schedulePlaybackFrame(frameDelayNs, nowNs);
    } else {
        cancelPlaybackFrame();
        requestPlaybackFrame();
    }
}

void WaveformEditorItem::setPlaying(bool value) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_playing == value) return;
        m_playing = value;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        if (value) m_profile.frameInitialized = false;
#endif
    }
    if (!value) cancelPlaybackFrame();
    emit playingChanged();
    update();
}

void WaveformEditorItem::setDurationSeconds(double value) {
    { QMutexLocker lock(&m_stateMutex); value = std::max(0.0, value); if (std::abs(m_durationSeconds - value) < 0.0001) return; m_durationSeconds = value; m_zoomLevel = std::clamp(m_zoomLevel, 1.0, maximumZoomLevelLocked()); m_presentedZoomLevel = m_zoomLevel; m_zoomOutHandoffPending = false; invalidateDetailRequestLocked(); clearPendingRequestLocked(); clearDetailLocked(); m_zoomFallbackToOverview = false; invalidateCacheLocked(); }
    emit durationSecondsChanged(); emit zoomLevelChanged(); emit samplePointsVisibleChanged(); scheduleDetailRequest(); update();
}

void WaveformEditorItem::setZoomLevel(double value) {
    bool presentationChanged = false;
    {
        QMutexLocker lock(&m_stateMutex);
        value = std::clamp(value, 1.0, maximumZoomLevelLocked());
        if (std::abs(m_zoomLevel - value) < 0.0001) return;
        const bool previousSampleCurve = sampleCurveVisibleLocked();
        const bool previousSamplePoints = samplePointsVisibleLocked();
        const double previousPresentedZoom = m_presentedZoomLevel;
        m_zoomLevel = value;
        invalidateDetailRequestLocked();
        clearPendingRequestLocked();
        clearStagedCacheLocked();
        const auto [visibleStart, visibleEnd] = visibleRangeForZoomLocked(value);
        const bool detailReady = detailCoversRangeLocked(visibleStart, visibleEnd)
            && detailResolutionCoversLocked(visibleStart, visibleEnd);
        const auto [presentedStart, presentedEnd] = visibleRangeLocked();
        // The view as it is stays on screen until the detail for the zoom
        // arrives: drawn from the cache, or while playing from the playback
        // tiles, which leave no cache.
        const bool canHoldPresentation = cacheCoversRangeLocked(
            presentedStart, presentedEnd)
            || playbackTilesEligibleLocked(presentedStart, presentedEnd);
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
            && cacheCoversRangeLocked(visibleStart, visibleEnd);
        const bool samplePresentationChanged = !deferZoomOut
            && (previousSampleCurve != sampleCurveVisibleLocked()
                || previousSamplePoints != samplePointsVisibleLocked());
        // A raw-sample curve is rasterized at the presented zoom's exact
        // seconds-per-pixel scale.  Magnifying that raster at the next zoom
        // produces soft stair steps even though the underlying detail still
        // contains every sample, so each sample-curve zoom needs a fresh
        // scale-specific cache.
        const bool sampleCurveScaleChanged = !deferZoomOut
            && presentationChanged
            && sampleCurveVisibleLocked();
        if (!deferZoomOut && value < previousPresentedZoom) {
            m_zoomFallbackToOverview = !detailReady;
        } else if (!deferZoomOut && detailReady) {
            m_zoomFallbackToOverview = false;
        }
        if (!deferZoomOut
            && (!zoomingInInsideCache || samplePresentationChanged
                || sampleCurveScaleChanged)) {
            invalidateCacheLocked();
        }
    }
    emit zoomLevelChanged();
    if (presentationChanged) emit samplePointsVisibleChanged();
    scheduleDetailRequest();
    update();
}

void WaveformEditorItem::setReservedCorner(const QSizeF &value) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_reservedCorner == value) return;
        m_reservedCorner = value;
    }
    emit reservedCornerChanged();
    update();
}

QRectF WaveformEditorItem::laneHeadBounds() const { QMutexLocker lock(&m_stateMutex); return m_laneHeadBounds; }

void WaveformEditorItem::setLaneHeadBounds(const QRectF &value) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_laneHeadBounds == value) return;
        m_laneHeadBounds = value;
    }
    emit laneHeadBoundsChanged();
    update();
}
QRectF WaveformEditorItem::fpsCounterRect() const {
    QMutexLocker lock(&m_stateMutex);
    if (!m_showFpsOverlay || m_fpsValue <= 0) return QRectF();
    const QFontMetrics metrics(fpsCounterFont());
    return ViewCorner::fpsCounterRect(
        width(), m_reservedCorner,
        QSizeF(metrics.horizontalAdvance(fpsCounterText(m_fpsValue)), metrics.height()));
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
QColor WaveformEditorItem::backgroundColor() const { QMutexLocker lock(&m_stateMutex); return m_colors.background; }
void WaveformEditorItem::setBackgroundColor(const QColor &value) { setColor(&Colors::background, value); }
QColor WaveformEditorItem::waveformColor() const { QMutexLocker lock(&m_stateMutex); return m_colors.waveform; }
void WaveformEditorItem::setWaveformColor(const QColor &value) { setColor(&Colors::waveform, value); }
QColor WaveformEditorItem::mutedWaveformColor() const { QMutexLocker lock(&m_stateMutex); return m_colors.mutedWaveform; }
void WaveformEditorItem::setMutedWaveformColor(const QColor &value) { setColor(&Colors::mutedWaveform, value); }
QColor WaveformEditorItem::centerLineColor() const { QMutexLocker lock(&m_stateMutex); return m_colors.centerLine; }
void WaveformEditorItem::setCenterLineColor(const QColor &value) { setColor(&Colors::centerLine, value); }
QColor WaveformEditorItem::laneSeamColor() const { QMutexLocker lock(&m_stateMutex); return m_colors.laneSeam; }
void WaveformEditorItem::setLaneSeamColor(const QColor &value) { setColor(&Colors::laneSeam, value); }
QColor WaveformEditorItem::playheadColor() const { QMutexLocker lock(&m_stateMutex); return m_colors.playhead; }
void WaveformEditorItem::setPlayheadColor(const QColor &value) { setColor(&Colors::playhead, value); }
QColor WaveformEditorItem::playheadCasingColor() const { QMutexLocker lock(&m_stateMutex); return m_colors.playheadCasing; }
void WaveformEditorItem::setPlayheadCasingColor(const QColor &value) { setColor(&Colors::playheadCasing, value); }

double WaveformEditorItem::markerSeconds() const { QMutexLocker lock(&m_stateMutex); return m_markerSeconds; }

void WaveformEditorItem::setMarkerSeconds(double value) {
    if (!std::isfinite(value) || value < 0.0) value = -1.0;
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_markerSeconds == value) return;
        m_markerSeconds = value;
    }
    emit markerSecondsChanged();
    update();
}

void WaveformEditorItem::publishMarkerX(double x) {
    QMutexLocker lock(&m_stateMutex);
    // NaN, the marker not drawn, is not equal to itself: compare by kind.
    if ((std::isnan(x) && std::isnan(m_markerXLaidOut)) || x == m_markerXLaidOut) return;
    m_markerXLaidOut = x;
    // Drawn on the render thread; the view binds on the GUI thread.
    QMetaObject::invokeMethod(this, [this, x] {
        if ((std::isnan(x) && std::isnan(m_markerX)) || x == m_markerX) return;
        m_markerX = x;
        emit markerXChanged();
    }, Qt::QueuedConnection);
}

void WaveformEditorItem::setColor(QColor Colors::*member, const QColor &value) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_colors.*member == value) return;
        m_colors.*member = value;
        // The cached rasters and tiles are drawn in the old colours.
        invalidateCacheLocked();
    }
    emit colorsChanged();
    update();
}

void WaveformEditorItem::setViewMode(int value) { value = std::clamp(value, 0, 1); { QMutexLocker lock(&m_stateMutex); if (m_viewMode == value) return; m_viewMode = value; invalidateCacheLocked(); } emit viewModeChanged(); emit channelCountChanged(); update(); }
void WaveformEditorItem::setMutedChannelsMask(qulonglong value) { { QMutexLocker lock(&m_stateMutex); if (m_mutedChannelsMask == value) return; m_mutedChannelsMask = value; invalidateCacheLocked(); } emit mutedChannelsChanged(); update(); }

QList<bool> WaveformEditorItem::mutedChannels() const {
    const qulonglong mask = mutedChannelsMask();
    QList<bool> muted(64);
    for (int channel = 0; channel < 64; ++channel) {
        muted[channel] = (mask & (qulonglong{1} << channel)) != 0;
    }
    return muted;
}

void WaveformEditorItem::setMutedChannels(const QList<bool> &value) {
    qulonglong mask = 0;
    for (qsizetype channel = 0; channel < std::min<qsizetype>(value.size(), 64); ++channel) {
        if (value[channel]) mask |= qulonglong{1} << channel;
    }
    setMutedChannelsMask(mask);
}
void WaveformEditorItem::setSoloedChannelsMask(qulonglong value) { { QMutexLocker lock(&m_stateMutex); if (m_soloedChannelsMask == value) return; m_soloedChannelsMask = value; invalidateCacheLocked(); } emit soloedChannelsChanged(); update(); }

QList<bool> WaveformEditorItem::soloedChannels() const {
    const qulonglong mask = soloedChannelsMask();
    QList<bool> soloed(64);
    for (int channel = 0; channel < 64; ++channel) {
        soloed[channel] = (mask & (qulonglong{1} << channel)) != 0;
    }
    return soloed;
}

void WaveformEditorItem::setSoloedChannels(const QList<bool> &value) {
    qulonglong mask = 0;
    for (qsizetype channel = 0; channel < std::min<qsizetype>(value.size(), 64); ++channel) {
        if (value[channel]) mask |= qulonglong{1} << channel;
    }
    setSoloedChannelsMask(mask);
}
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

void WaveformEditorItem::itemChange(ItemChange change, const ItemChangeData &value) {
    QQuickItem::itemChange(change, value);
    if (change != ItemDevicePixelRatioHasChanged) {
        return;
    }
    // Moving to a screen with another scale, or the scale changing: the
    // next paint draws the caches at the new device pixels, once. A staged
    // cache or playback tiles at the old ones would land at the wrong size.
    {
        QMutexLocker lock(&m_stateMutex);
        invalidateCacheLocked();
    }
    update();
}

void WaveformEditorItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        {
            QMutexLocker lock(&m_stateMutex);
            m_presentedZoomLevel = m_zoomLevel;
            m_zoomOutHandoffPending = false;
            invalidateDetailRequestLocked();
            clearPendingRequestLocked();
            invalidateCacheLocked();
        }
        scheduleDetailRequest();
    }
}

QByteArray WaveformEditorItem::decodeWindow(const QString &path, double startSeconds,
                                             double endSeconds, int maxPoints, const std::shared_ptr<std::atomic_bool> &cancelled) {
    const QByteArray encodedPath = path.toUtf8();
    std::size_t length = 0;
    std::uint8_t *data = ferrous_ffi_waveform_window_cancellable(
        reinterpret_cast<const std::uint8_t *>(encodedPath.constData()),
        static_cast<std::size_t>(encodedPath.size()), startSeconds, endSeconds,
        static_cast<std::uint32_t>(std::max(1, maxPoints)), &length,
        [](const void *context) { return static_cast<const std::atomic_bool *>(context)->load(); },
        cancelled.get());
    if (data == nullptr || length == 0) return {};
    const QByteArray result(reinterpret_cast<const char *>(data), static_cast<qsizetype>(length));
    ferrous_ffi_waveform_window_free(data, length);
    return result;
}

bool WaveformEditorItem::parseWindow(const QByteArray &bytes, DetailWindow *window) {
    if (window == nullptr || bytes.size() < kWindowHeaderBytes || bytes.first(4) != QByteArrayLiteral("WVF2")) return false;
    const char *data = bytes.constData();
    const quint32 sampleRate = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data + 4));
    const quint16 channels = qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(data + 8));
    const double start = readF64(data + 12);
    const double end = readF64(data + 20);
    const quint32 framesPerPoint = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data + 28));
    const quint32 pointCount = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data + 32));
    const quint64 valueCount = static_cast<quint64>(channels) * pointCount * 2U;
    const quint64 downmixCount = static_cast<quint64>(pointCount) * 2U;
    const quint64 expected = static_cast<quint64>(kWindowHeaderBytes) + (valueCount + downmixCount) * 4U;
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
    window->downmixExtrema.resize(static_cast<std::size_t>(downmixCount));
    for (quint64 index = 0; index < downmixCount; ++index) {
        window->downmixExtrema[static_cast<std::size_t>(index)] = readF32(
            data + kWindowHeaderBytes + static_cast<qsizetype>((valueCount + index) * 4U));
    }
    return true;
}

void WaveformEditorItem::scheduleDetailRequest(bool immediate) {
    // Showing it asks for what it misses then.
    if (!shownOnScreen()) return;
    const int delayMs = immediate ? 0 : 40;
    if (thread() == QThread::currentThread()
        && (!m_requestTimer.isActive()
            || m_requestTimer.remainingTime() > delayMs)) {
        m_requestTimer.start(delayMs);
    }
}

void WaveformEditorItem::requestDetailWindow() {
    QString path;
    double requestStart = 0.0;
    double requestEnd = 0.0;
    int points = 0;
    quint64 generation = 0;
    std::shared_ptr<std::atomic_bool> cancelled;
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
        if (!shownOnScreen()) return;
        if (m_decodeActive) {
            // A request still in flight for part of the view stays: its
            // arrival asks for what it misses. Replacing it instead would
            // starve a view that moves on before any decode completes, such
            // as playback at a deep zoom.
            const auto [visibleStart, visibleEnd] = detailRequestVisibleRangeLocked();
            if (m_requestInFlight
                && m_requestedStartSeconds < visibleEnd
                && m_requestedEndSeconds > visibleStart) {
                return;
            }
            invalidateDetailRequestLocked();
            return;
        }
        m_decodeActive = true;
        cancelled = std::make_shared<std::atomic_bool>(false);
        m_decodeCancelled = cancelled;
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
    using Watcher = QFutureWatcher<std::shared_ptr<DecodedWindow>>;
    auto *watcher = new Watcher(this);
    connect(watcher, &Watcher::finished, this, [
        this, watcher, generation
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        , profileRequestStarted
#endif
    ]() {
        watcher->deleteLater();
        {
            QMutexLocker lock(&m_stateMutex);
            m_decodeActive = false;
            m_decodeCancelled.reset();
            if (generation != m_requestGeneration) {
                clearPendingRequestLocked();
                scheduleDetailRequest();
                return;
            }
        }
        // Parsed on the worker: a window of samples of a surround track
        // holds megabytes of them.
        const std::shared_ptr<DecodedWindow> decoded = watcher->result();
        DetailWindow next = std::move(decoded->window);
        const bool parsed = decoded->parsed;
        bool channelsChanged = false;
        bool rateChanged = false;
        bool pointsChanged = false;
        bool zoomChanged = false;
        bool requestFollowup = false;
        bool stale = false;
        bool parseFailed = false;
        // Whether the detail changes how the view is drawn, as a zoom
        // does, rather than only adding to what playback shows.
        bool presentationChanged = false;
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
                ++m_detailArrivals;
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
                    if (deferredZoomDrawnByPlaybackLocked(m_nowNs())) {
                        // Shown now, as playback draws any frame: a raster
                        // drawn for it first, a part a frame, would be left
                        // behind by a view that moves on by its width in a
                        // few frames, and drawn again, and again.
                        invalidateCacheLocked();
                        pointsChanged = oldPointsVisible != samplePointsVisibleLocked();
                    } else {
                        beginStagedCacheForRangeLocked(
                            visibleStart, visibleEnd, true);
                    }
                }
                const auto [presentedStart, presentedEnd] = visibleRangeLocked();
                const bool cacheCoversViewport = cacheCoversRangeLocked(
                    presentedStart, presentedEnd);
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
                presentationChanged = firstDetail || channelsChanged || rateChanged
                    || zoomChanged || pointsChanged || replaceOverviewFallback
                    || deferredZoomStage || stageReplacement;
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
                static_cast<long long>(decoded->bytes),
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
        // A zoom, a track's first detail or a change of how it is drawn
        // shows at once; more of the same detail as playback moves on
        // shows with the next frame of playback.
        if (presentationChanged) {
            update();
        } else {
            requestArrivalFrame();
        }
    });
    watcher->setFuture(QtConcurrent::run(
        [decode = m_decodeWindow, path, requestStart, requestEnd, points, cancelled]() {
            auto decoded = std::make_shared<DecodedWindow>();
            const QByteArray bytes = decode(path, requestStart, requestEnd, points, cancelled);
            decoded->bytes = bytes.size();
            decoded->parsed = parseWindow(bytes, &decoded->window);
            return decoded;
        }));
}

quint64 WaveformEditorItem::nextDetailGeneration() {
    static std::atomic<quint64> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

void WaveformEditorItem::clearDetailLocked() {
    m_detail = DetailWindow{};
    ++m_detailSourceEpoch;
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

double WaveformEditorItem::displayedPositionSecondsLocked(qint64 nowNs) const {
    return std::clamp(
        m_clockState.positionAt(nowNs), 0.0, std::max(0.0, m_durationSeconds));
}

double WaveformEditorItem::displayedPositionSecondsLocked() const {
    return displayedPositionSecondsLocked(m_nowNs());
}

bool WaveformEditorItem::detailOrPendingRequestCoversLocked(
    double startSeconds, double endSeconds) const {
    const double visibleSpan = std::max(0.0, endSeconds - startSeconds);
    const auto [behind, ahead] = detailRequestMarginsLocked(visibleSpan);
    const double coveredStart = std::max(0.0, startSeconds - behind * kDetailMarginSafety);
    const double coveredEnd = std::min(m_durationSeconds, endSeconds + ahead * kDetailMarginSafety);
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
    if (sampleCurveRequestedForPixelSpanLocked(
            pixelWidth, startSeconds, endSeconds)
        && m_detail.framesPerPoint != 1) {
        return false;
    }
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
double WaveformEditorItem::deviceColumnsLocked() const {
    // As TimeAxis counts them over the canvas.
    const int canvasWidth = std::max(1, static_cast<int>(std::floor(width())));
    return static_cast<double>(canvasWidth) * renderScaleLocked();
}

double WaveformEditorItem::followingStartLocked(double span, double position) const {
    const double columns = deviceColumnsLocked();
    const double column = span / columns;
    // As TimeAxis::columnAt, so that the playhead's column in the view is
    // its column of the track less the view's first.
    const double playheadColumn = std::floor(position / column + kColumnEpsilon);
    const double middleColumn = std::floor(columns * 0.5 + kColumnEpsilon);
    return (playheadColumn - middleColumn) * column;
}

double WaveformEditorItem::visibleSpanForZoomLocked(double zoomLevel) const {
    if (m_durationSeconds <= 0.0) return 0.0;
    const double span = m_durationSeconds / std::max(1.0, zoomLevel);
    return span >= m_durationSeconds * 0.9999 ? m_durationSeconds : span;
}

std::pair<double, double> WaveformEditorItem::visibleRangeForZoomLocked(
    double zoomLevel, qint64 nowNs) const {
    if (m_durationSeconds <= 0.0) return {0.0, 0.0};
    const double span = visibleSpanForZoomLocked(zoomLevel);
    if (span >= m_durationSeconds) return {0.0, m_durationSeconds};
    const double start = std::clamp(
        followingStartLocked(span, displayedPositionSecondsLocked(nowNs)),
        0.0, m_durationSeconds - span);
    return {start, start + span};
}

std::pair<double, double> WaveformEditorItem::visibleRangeForZoomLocked(
    double zoomLevel) const {
    return visibleRangeForZoomLocked(zoomLevel, m_nowNs());
}

std::pair<double, double> WaveformEditorItem::visibleRangeLocked(qint64 nowNs) const {
    return visibleRangeForZoomLocked(m_presentedZoomLevel, nowNs);
}

std::pair<double, double> WaveformEditorItem::visibleRangeLocked() const {
    return visibleRangeLocked(m_nowNs());
}

std::pair<double, double> WaveformEditorItem::detailRequestVisibleRangeLocked() const {
    return m_zoomOutHandoffPending
        ? visibleRangeForZoomLocked(m_zoomLevel)
        : visibleRangeLocked();
}

std::pair<double, double> WaveformEditorItem::requestRangeLocked(
    double visibleStart, double visibleEnd) const {
    const double visibleSpan = std::max(0.0, visibleEnd - visibleStart);
    const auto [behind, ahead] = detailRequestMarginsLocked(visibleSpan);
    return {
        std::max(0.0, visibleStart - behind),
        std::min(m_durationSeconds, visibleEnd + ahead),
    };
}

std::pair<double, double> WaveformEditorItem::detailRequestMarginsLocked(
    double visibleSpan) const {
    const double margin = detailRequestMarginLocked(visibleSpan);
    if (!m_playing) return {margin, margin};
    // Playback uses up the margin ahead of the view, and the next window
    // must arrive before it has. At sample resolution a window spans at
    // most 65,534 frames, 0.68 s at 96 kHz: split evenly, a reply would
    // have about 0.2 s.
    return {
        2.0 * margin * kPlaybackDetailMarginBehind,
        2.0 * margin * (1.0 - kPlaybackDetailMarginBehind),
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
        const bool rawSampleCurve = sampleCurveRequestedForPixelSpanLocked(
            renderPixelWidthLocked(), 0.0, visibleSpan);
        const double requiredPoints = requiredVisibleDetailPointsLocked(
            visibleSpan);
        const double visibleFrames = visibleSpan
            * static_cast<double>(m_sampleRateHz);
        const double targetFramesPerPoint = rawSampleCurve
            ? 1.0
            : std::exp2(std::floor(std::log2(std::max(1.0, visibleFrames / requiredPoints))));
        // Keep the requested frame count inside the bin size selected for the
        // viewport.  Otherwise a capped request can cross the next integer bin
        // boundary and remain permanently too coarse for the current zoom.
        // Match Rust's power-of-two summary levels without losing pixel density.
        const int pointCapacity = kMaximumDetailPoints
            - (rawSampleCurve ? 2 : 1);
        const double quantizedSpan = static_cast<double>(pointCapacity)
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
        if (sampleCurveRequestedForPixelSpanLocked(
                renderPixelWidthLocked(), visibleStart, visibleEnd)) {
            requestedPoints = std::max(
                requestedPoints, static_cast<int>(requestFrames));
        } else {
            const double targetFramesPerPoint = std::exp2(std::floor(std::log2(std::max(
                1.0, requestFrames / std::max(1.0, required)))));
            requestedPoints = std::max(
                requestedPoints,
                static_cast<int>(std::ceil(
                    requestFrames / targetFramesPerPoint)));
        }
    }
    return std::clamp(requestedPoints, 64, kMaximumDetailPoints);
}

double WaveformEditorItem::renderScaleLocked() const {
    return window() != nullptr ? window()->effectiveDevicePixelRatio() : 1.0;
}

int WaveformEditorItem::renderPixelWidthLocked() const {
    return std::max(1, static_cast<int>(std::ceil(width() * renderScaleLocked())));
}

int WaveformEditorItem::renderPixelHeightLocked() const {
    return std::max(1, static_cast<int>(std::ceil(
        std::floor(height()) * renderScaleLocked() - 1e-6)));
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

    // A raster pixel is a device column of the view, as the view scrolls
    // by (see followingStartLocked), so that a step of the view moves it by
    // whole pixels; the view may end in a fraction of one, which the
    // raster's whole pixels cover.
    const int viewportWidth = renderPixelWidthLocked();
    const double secondsPerPixel = visibleSpan / deviceColumnsLocked();
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
    invalidateDetailRequestLocked();
    clearPendingRequestLocked();
    invalidateCacheLocked();
    return targetChanged;
}

double WaveformEditorItem::sampleSpacingPixels(
    const DetailWindow &detail, int pixelWidth, double visibleStart, double visibleEnd) {
    if (detail.framesPerPoint != 1 || detail.pointCount <= 1) return 0.0;
    const double visiblePoints = (visibleEnd - visibleStart)
        * static_cast<double>(std::max(1, detail.sampleRateHz));
    return static_cast<double>(std::max(1, pixelWidth))
        / std::max(1.0, visiblePoints);
}

double WaveformEditorItem::sampleSpacingPixelsLocked(
    int pixelWidth, double visibleStart, double visibleEnd) const {
    return sampleSpacingPixels(m_detail, pixelWidth, visibleStart, visibleEnd);
}

double WaveformEditorItem::sampleSpacingPixelsLocked() const {
    const auto [start, end] = visibleRangeLocked();
    return sampleSpacingPixelsLocked(
        std::max(1, static_cast<int>(std::floor(width()))), start, end);
}

bool WaveformEditorItem::sampleCurveRequestedForPixelSpanLocked(
    int pixelWidth, double visibleStart, double visibleEnd) const {
    const int sampleRate = m_sampleRateHz > 0
        ? m_sampleRateHz
        : m_detail.sampleRateHz;
    if (sampleRate <= 0 || visibleEnd <= visibleStart) return false;
    const double visibleSamples = (visibleEnd - visibleStart)
        * static_cast<double>(sampleRate);
    return static_cast<double>(std::max(1, pixelWidth))
            / std::max(1.0, visibleSamples)
            + kGridAlignmentEpsilon
        >= kSampleCurvePixelsPerSample;
}

bool WaveformEditorItem::samplePointsVisibleLocked() const {
    return sampleSpacingPixelsLocked() + kGridAlignmentEpsilon
        >= kSampleMarkerPixelsPerSample;
}

bool WaveformEditorItem::sampleCurveVisibleLocked() const {
    return sampleSpacingPixelsLocked() + kGridAlignmentEpsilon
        >= kSampleCurvePixelsPerSample;
}

bool WaveformEditorItem::renderDetailDirectlyLocked(
    double visibleStart, double visibleEnd) const {
    // The frame's own view: it reads the clock once.
    const int pixelWidth = std::max(1, static_cast<int>(std::floor(width())));
    if (sampleSpacingPixelsLocked(pixelWidth, visibleStart, visibleEnd) + kGridAlignmentEpsilon
            < kDirectSamplePixelsPerSample
        || !detailResolutionCoversLocked(visibleStart, visibleEnd)) {
        return false;
    }
    if (detailCoversRangeLocked(visibleStart, visibleEnd)) return true;
    // Playback can move the view on past its detail before the next window
    // comes. The samples the detail holds are drawn until it does, the
    // rest of the view left empty, as playback tiles are kept (see
    // playbackTilesEligibleLocked): a raster of the view in their place
    // would be drawn and uploaded for every frame.
    return m_playing
        && visibleStart < m_detail.endSeconds
        && visibleEnd > m_detail.startSeconds;
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

double WaveformEditorItem::detailPointTime(const DetailWindow &detail, int point) {
    if (detail.sampleRateHz <= 0 || detail.framesPerPoint == 0) {
        return detail.startSeconds;
    }
    const double framesPerPoint = static_cast<double>(detail.framesPerPoint);
    const double centerFrame = static_cast<double>(point) * framesPerPoint
        + (framesPerPoint - 1.0) * 0.5;
    return detail.startSeconds
        + centerFrame / static_cast<double>(detail.sampleRateHz);
}

double WaveformEditorItem::detailPointTimeLocked(int point) const {
    return detailPointTime(m_detail, point);
}

std::pair<int, int> WaveformEditorItem::detailPointRange(
    const DetailWindow &detail, double visibleStart, double visibleEnd) {
    if (detail.pointCount <= 0
        || detail.sampleRateHz <= 0
        || detail.framesPerPoint == 0
        || visibleEnd < visibleStart) {
        return {0, 0};
    }
    const double step = static_cast<double>(detail.framesPerPoint)
        / static_cast<double>(detail.sampleRateHz);
    const double firstPointTime = detailPointTime(detail, 0);
    const double first = (visibleStart - firstPointTime) / step;
    const double last = (visibleEnd - firstPointTime) / step;
    return {
        std::clamp(
            static_cast<int>(std::floor(first)), 0, detail.pointCount),
        std::clamp(
            static_cast<int>(std::ceil(last)) + 1, 0, detail.pointCount),
    };
}

std::pair<int, int> WaveformEditorItem::detailPointRangeLocked(
    double visibleStart, double visibleEnd) const {
    return detailPointRange(m_detail, visibleStart, visibleEnd);
}

std::shared_ptr<const WaveformEditorItem::DetailWindow>
WaveformEditorItem::detailSnapshotLocked(double startSeconds, double endSeconds) const {
    auto snapshot = std::make_shared<DetailWindow>();
    const auto [first, last] = detailPointRangeLocked(startSeconds, endSeconds);
    if (last <= first || m_detail.sampleRateHz <= 0) return snapshot;
    const auto channels = static_cast<std::size_t>(m_detail.channelCount);
    const double secondsPerPoint = static_cast<double>(m_detail.framesPerPoint)
        / static_cast<double>(m_detail.sampleRateHz);
    snapshot->sampleRateHz = m_detail.sampleRateHz;
    snapshot->channelCount = m_detail.channelCount;
    snapshot->framesPerPoint = m_detail.framesPerPoint;
    snapshot->pointCount = last - first;
    // The same points at the same times: see detailPointTime.
    snapshot->startSeconds = m_detail.startSeconds + first * secondsPerPoint;
    snapshot->endSeconds = snapshot->startSeconds + snapshot->pointCount * secondsPerPoint;
    snapshot->extrema.assign(
        m_detail.extrema.begin() + static_cast<std::ptrdiff_t>(first * channels * 2),
        m_detail.extrema.begin() + static_cast<std::ptrdiff_t>(last * channels * 2));
    if (!m_detail.downmixExtrema.empty()) {
        snapshot->downmixExtrema.assign(
            m_detail.downmixExtrema.begin() + static_cast<std::ptrdiff_t>(first) * 2,
            m_detail.downmixExtrema.begin() + static_cast<std::ptrdiff_t>(last) * 2);
    }
    return snapshot;
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
    disconnect(m_windowPresenceConnection);
    disconnect(m_screenConnection);
    m_frameSwappedConnection = {};
    m_windowVisibilityConnection = {};
    m_windowPresenceConnection = {};
    m_screenConnection = {};
    m_windowPresence = WindowPresence::of(window);
    // The new window draws a frame of its own, which schedules the next
    // one with the new window's views.
    cancelPlaybackFrame();
    m_frameScheduler = nullptr;
    updateRefreshRate();
    if (window == nullptr) return;
    m_screenConnection = connect(
        window, &QWindow::screenChanged, this, &WaveformEditorItem::updateRefreshRate);
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
    m_windowPresenceConnection = connect(
        m_windowPresence, &WindowPresence::shownChanged,
        this, &WaveformEditorItem::shownChanged);
    // The new window may be on screen or off it already: as if it had
    // just become so, cancel or ask for the detail and the frame.
    shownChanged();
}

bool WaveformEditorItem::shownOnScreen() const {
    return isVisible() && (window() == nullptr || (m_windowPresence && m_windowPresence->shown()));
}

void WaveformEditorItem::shownChanged() {
    if (!shownOnScreen()) {
        // Nothing would see them: no frames and no detail decoding until
        // it is shown again. The detail it has stays.
        cancelPlaybackFrame();
        m_requestTimer.stop();
        {
            QMutexLocker lock(&m_stateMutex);
            if (m_decodeActive || m_requestInFlight) {
                invalidateDetailRequestLocked();
                clearPendingRequestLocked();
            }
        }
        // The service keeps the coarse summaries of the current track,
        // which show the whole track again at once, and frees the rest.
        updateReaderLease();
        return;
    }
    updateReaderLease();
    bool request = false;
    {
        QMutexLocker lock(&m_stateMutex);
        const auto [requestStart, requestEnd] = detailRequestVisibleRangeLocked();
        request = !detailOrPendingRequestCoversLocked(requestStart, requestEnd);
    }
    // The detail of where playback is now, if it has not got it.
    if (request) scheduleDetailRequest(true);
    // One frame shows playback as it is now, also a seek made while it
    // was hidden; the frames that follow it ask for their own.
    requestPlaybackFrame();
}

void WaveformEditorItem::updateReaderLease() {
    m_readerLease.setPath(shownOnScreen() ? sourcePath() : QString());
}

void WaveformEditorItem::handleWindowFrameSwapped() {
    if (!isVisible()) return;
    bool request = false;
    qint64 frameDelayNs = 0;
    const qint64 nowNs = m_nowNs();
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
        if (!direct && !tiledPlayback && cacheRedrawDueLocked(start, end)) {
            invalidateCacheLocked();
        }
        // Detail arriving asks for its own frame.
        frameDelayNs = playbackFrameDelayNsLocked(nowNs);
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
    // At the first raw-sample zoom, the point cap can leave less than 40 ms
    // of headroom when prefetch becomes due. Do not spend it on zoom debounce.
    if (request) scheduleDetailRequest(true);
    schedulePlaybackFrame(frameDelayNs, nowNs);
}

double WaveformEditorItem::frameChangeSecondsLocked(
    double visibleStart, double visibleEnd, double position, int width) const {
    const double span = visibleEnd - visibleStart;
    if (span <= 0.0 || width < 2) return std::numeric_limits<double>::infinity();
    // The view follows the playhead a device column at a time, unless it
    // shows the whole track or is held at an end of it; see
    // visibleRangeForZoomLocked. Following, or held at the start, which
    // is on the track's grid, both the view and the playhead change when
    // the playhead reaches its next column of the track.
    const bool wholeTrack = span >= m_durationSeconds * 0.9999;
    if (!wholeTrack
        && followingStartLocked(span, position) < m_durationSeconds - span) {
        const double column = span / deviceColumnsLocked();
        const double playheadColumn = std::floor(position / column + kColumnEpsilon);
        return (playheadColumn + 1.0 - kColumnEpsilon) * column;
    }
    // Held still, the playhead moves a device column at a time, and leaves
    // the view after its end: see paint.
    const TimeAxis axis{visibleStart, visibleEnd, width, renderScaleLocked()};
    double change = std::numeric_limits<double>::infinity();
    if (position < visibleStart) {
        change = visibleStart;
    } else if (position <= visibleEnd) {
        change = std::min(
            axis.timeAt(axis.columnOf(position) + 1),
            std::nextafter(visibleEnd, std::numeric_limits<double>::infinity()));
    }
    return change;
}

qint64 WaveformEditorItem::playbackFrameDelayNsLocked(qint64 nowNs) const {
    if (m_zoomOutHandoffPending || !m_stagedCache.isNull()) return 0;
    // From the frame on screen: once playback has passed where it stops
    // being current, the next frame is due, even before this is called.
    const double change = m_paintedFrameChangeSeconds;
    if (std::isnan(change)) return 0;
    if (std::isinf(change)) return -1;
    const std::optional<qint64> changesAt = m_clockState.reachesAt(change);
    if (!changesAt) return -1;
    // A view that scrolls too fast to follow waits for its next capped
    // frame, however soon it changes.
    return std::max(*changesAt > nowNs ? *changesAt - nowNs : 0, cappedFrameDelayNsLocked(nowNs));
}

int WaveformEditorItem::fastScrollRefreshDivisor(double refreshRateHz) {
    if (!std::isfinite(refreshRateHz) || refreshRateHz <= 0.0) return 1;
    return std::max(1, static_cast<int>(std::lround(refreshRateHz / kFastScrollFramesPerSecond)));
}

double WaveformEditorItem::scrollPixelsPerSecondLocked(int width) const {
    const double span = visibleSpanForZoomLocked(m_presentedZoomLevel);
    return span > 0.0 ? static_cast<double>(width) / span : 0.0;
}

bool WaveformEditorItem::fastScrollCappedLocked(int width) const {
    const double speed = scrollPixelsPerSecondLocked(width);
    return m_fastScrollCapped ? speed >= kFastScrollUncapPixelsPerSecond
                              : speed > kFastScrollCapPixelsPerSecond;
}

qint64 WaveformEditorItem::cappedFrameIntervalNsLocked() const {
    if (!m_fastScrollCapped || !m_playing || !m_clockState.advancing()) return 0;
    const int divisor = fastScrollRefreshDivisor(m_refreshRateHz);
    if (divisor <= 1) return 0;
    return std::llround(divisor * 1e9 / m_refreshRateHz);
}

qint64 WaveformEditorItem::cappedFrameDelayNsLocked(qint64 nowNs) const {
    // A zoom being handed over shows its frames at once.
    if (m_zoomOutHandoffPending || !m_stagedCache.isNull()) return 0;
    if (cappedFrameIntervalNsLocked() <= 0) return 0;
    return std::max<qint64>(0, m_nextCappedFrameNs - nowNs);
}

void WaveformEditorItem::requestArrivalFrame() {
    const qint64 nowNs = m_nowNs();
    qint64 delayNs = 0;
    {
        QMutexLocker lock(&m_stateMutex);
        delayNs = cappedFrameDelayNsLocked(nowNs);
    }
    if (delayNs <= 0 || !shownOnScreen()) {
        update();
        return;
    }
    // A frame asked for by then shows it too.
    PlaybackFrameScheduler *scheduler = frameScheduler();
    const qint64 dueNs = scheduler->dueNs(this);
    if (dueNs < 0 || dueNs > nowNs + delayNs) scheduler->schedule(this, nowNs + delayNs, nowNs);
}

double WaveformEditorItem::screenRefreshRate(const QScreen &screen) {
    return screen.refreshRate();
}

void WaveformEditorItem::updateRefreshRate() {
    QQuickWindow *view = window();
    QScreen *screen = view != nullptr ? view->screen() : nullptr;
    disconnect(m_refreshRateConnection);
    m_refreshRateConnection = {};
    if (screen != nullptr) {
        m_refreshRateConnection = connect(
            screen, &QScreen::refreshRateChanged, this, &WaveformEditorItem::updateRefreshRate);
    }
    const double rate = screen != nullptr ? m_screenRefreshRate(*screen) : 0.0;
    QMutexLocker lock(&m_stateMutex);
    m_refreshRateHz = rate;
}

bool WaveformEditorItem::clipSampleChunksLocked(double column) const {
    // A frame that moved on by the view's width or more shows none of the
    // last frame's chunks, nor will the next frame its own.
    const double moved = std::abs(column - m_lastSampleChunkColumn);
    if (std::isnan(moved)) return false;
    const double columns = deviceColumnsLocked();
    return moved >= (m_sampleChunksClipped ? kSampleChunkWholeViewFraction * columns : columns);
}

void WaveformEditorItem::schedulePlaybackFrame(qint64 delayNs, qint64 nowNs) {
    if (!shownOnScreen()) {
        // Showing it asks for its frame.
        cancelPlaybackFrame();
    } else if (delayNs == 0) {
        cancelPlaybackFrame();
        requestPlaybackFrame();
    } else if (delayNs < 0) {
        // Nothing moves until the clock changes course, which reschedules.
        cancelPlaybackFrame();
    } else {
        frameScheduler()->schedule(this, nowNs + delayNs, nowNs);
    }
}

void WaveformEditorItem::requestPlaybackFrame() {
    if (!shownOnScreen()) {
        // A view no one sees keeps its state up to date without frames;
        // showing it asks for one.
        cancelPlaybackFrame();
        return;
    }
    ++m_playbackFrameRequests;
    update();
}

void WaveformEditorItem::cancelPlaybackFrame() {
    if (m_frameScheduler != nullptr) m_frameScheduler->cancel(this);
}

PlaybackFrameScheduler *WaveformEditorItem::frameScheduler() {
    if (m_frameScheduler == nullptr) {
        QObject *owner = window();
        m_frameScheduler = PlaybackFrameScheduler::of(owner != nullptr ? owner : this);
    }
    return m_frameScheduler;
}

bool WaveformEditorItem::cacheCoversRangeLocked(double start, double end) const {
    return !m_cacheDirty
        && rasterCoversRange(m_cacheStartSeconds, m_cacheEndSeconds, start, end);
}

bool WaveformEditorItem::cacheRedrawDueLocked(
    double visibleStart, double visibleEnd) const {
    if (m_zoomOutHandoffPending || cacheCoversRangeLocked(visibleStart, visibleEnd)) {
        return false;
    }
    // Only playback moving the view on past the cache waits for another
    // window: paused, or with the clock held, the view stays where it is,
    // and the cache is drawn for it at once, from the overview if nothing
    // better comes.
    if (!m_playing || !m_clockState.advancing()) return true;
    return m_cacheDetailArrivals != m_detailArrivals
        // No window will come.
        || m_sourcePath.isEmpty()
        || (detailCoversRangeLocked(visibleStart, visibleEnd)
            && detailResolutionCoversLocked(visibleStart, visibleEnd));
}

bool WaveformEditorItem::deferredZoomDrawnByPlaybackLocked(qint64 nowNs) const {
    // As playbackTilesEligibleLocked and renderDetailDirectlyLocked take
    // the zoom once it shows; the whole track is drawn in a raster, which
    // playback does not move.
    if (!m_zoomOutHandoffPending || !m_playing || m_zoomLevel <= 1.0001) return false;
    const auto [start, end] = visibleRangeForZoomLocked(m_zoomLevel, nowNs);
    return detailCoversRangeLocked(start, end)
        && detailResolutionCoversLocked(start, end)
        && m_detail.framesPerPoint > 0;
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
    m_stagedCacheViewportWidth = 0;
    m_stagedCacheViewportHeight = 0;
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
    const int renderHeight = renderPixelHeightLocked();
    m_stagedCache = QImage(grid.width, renderHeight, QImage::Format_RGB32);
    m_stagedCacheViewportWidth = renderPixelWidthLocked();
    m_stagedCacheViewportHeight = renderHeight;
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

bool WaveformEditorItem::advanceStagedCacheLocked(qint64 nowNs) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    m_profile.lastStagedCacheMs = 0.0;
    m_profile.lastStagedCacheColumns = 0;
    const auto profileStageStarted = std::chrono::steady_clock::now();
#endif
    if (m_stagedCache.isNull()) return false;
    if (m_stagedCacheCommitsDeferredZoom && deferredZoomDrawnByPlaybackLocked(nowNs)) {
        // Playback started while the zoom's raster was drawn: it draws
        // the zoom now, and the view would move on past the raster.
        invalidateCacheLocked();
        return true;
    }
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
            m_colors.background);
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
            lastX,
            renderScaleLocked());
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
        ? visibleRangeForZoomLocked(m_zoomLevel, nowNs)
        : visibleRangeLocked(nowNs);
    bool presentationCommitted = false;
    if (rasterCoversRange(m_stagedCacheStartSeconds, m_stagedCacheEndSeconds,
            visibleStart, visibleEnd)) {
        m_cache = std::move(m_stagedCache);
        m_cacheStartSeconds = m_stagedCacheStartSeconds;
        m_cacheEndSeconds = m_stagedCacheEndSeconds;
        m_cacheSecondsPerPixel = m_stagedCacheSecondsPerPixel;
        m_cacheFramesPerPoint = m_stagedCacheFramesPerPoint;
        m_cacheDisplayedChannels = m_stagedCacheDisplayedChannels;
        // For the viewport it was begun for: at another scale since, the
        // next paint draws a cache at that one.
        m_cachedViewportWidth = m_stagedCacheViewportWidth;
        m_cachedViewportHeight = m_stagedCacheViewportHeight;
        m_cacheDetailArrivals = m_detailArrivals;
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

bool WaveformEditorItem::advanceStagedCacheLocked() {
    return advanceStagedCacheLocked(m_nowNs());
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
bool WaveformEditorItem::LaneStyle::muted(int channel) const {
    if (viewMode == 0 || channel >= 64) return false;
    const qulonglong bit = qulonglong{1} << channel;
    return (mutedMask & bit) != 0
        || (soloedMask != 0 && (soloedMask & bit) == 0);
}

WaveformEditorItem::LaneStyle WaveformEditorItem::laneStyleLocked() const {
    return {m_viewMode, m_mutedChannelsMask, m_soloedChannelsMask,
        m_straightSamplePixelsPerSample, m_colors.background, m_colors.waveform,
        m_colors.mutedWaveform};
}

bool WaveformEditorItem::channelIsMutedLocked(int channel) const {
    return laneStyleLocked().muted(channel);
}

double WaveformEditorItem::sampleChunkColumnLocked(double visibleStart) const {
    const double span = visibleSpanForZoomLocked(m_presentedZoomLevel);
    const double columns = deviceColumnsLocked();
    if (span <= 0.0 || columns <= 0.0) return std::numeric_limits<double>::quiet_NaN();
    const double viewColumn = visibleStart / (span / columns);
    const double nearest = std::round(viewColumn);
    return std::abs(viewColumn - nearest) < 1e-6 ? nearest : std::floor(viewColumn);
}

std::optional<WaveformSceneGraph::Chunks> WaveformEditorItem::sampleChunksLocked(
    double visibleStart, int width, int height, int channels, bool clipToView) const {
    // The span the zoom gives, not the view's end less its start, which
    // differs by a rounding from frame to frame.
    const double span = visibleSpanForZoomLocked(m_presentedZoomLevel);
    const double scale = renderScaleLocked();
    const double columns = deviceColumnsLocked();
    const int rate = m_detail.sampleRateHz;
    if (span <= 0.0 || columns <= 0.0 || rate <= 0 || m_detail.framesPerPoint != 1) return std::nullopt;
    // Chunks lie on the track's grid of device columns, which the view
    // follows playback on (see followingStartLocked), or from the track's
    // start; at its end, the view is a fraction of a column past one,
    // and the chunks are too while it stays there.
    const double column = span / columns;
    const double viewColumn = visibleStart / column;
    const double nearest = std::round(viewColumn);
    const bool onGrid = std::abs(viewColumn - nearest) < 1e-6;
    const double firstColumn = onGrid ? nearest : std::floor(viewColumn);
    const double phase = onGrid ? 0.0 : viewColumn - firstColumn;
    const double spacing = static_cast<double>(width) / (span * rate);
    const double chunkColumns = kSampleChunkColumns;
    // A sample's marker and its segment to the next reach a few pixels
    // either side of it: the chunks of samples just outside the view show
    // in it.
    const double before = std::ceil((spacing + 4.0) * scale);
    const double after = std::ceil(4.0 * scale);
    WaveformSceneGraph::Chunks chunks;
    chunks.first = static_cast<qint64>(std::floor((firstColumn - before) / chunkColumns));
    chunks.last = static_cast<qint64>(std::floor(
        (firstColumn + std::ceil(columns - kColumnEpsilon) - 1.0 + after) / chunkColumns));
    const LaneStyle style = laneStyleLocked();
    {
        QDataStream stream(&chunks.key, QIODevice::WriteOnly);
        stream << quint8(1) << width << height << scale << span << channels << style.viewMode
               << quint64{style.mutedMask} << quint64{style.soloedMask}
               << style.straightSamplePixelsPerSample << phase << kSampleChunkColumns
               << style.waveform.rgba() << style.mutedWaveform.rgba() << rate << m_detail.channelCount
               << m_detail.framesPerPoint << !m_detail.downmixExtrema.empty() << m_detailSourceEpoch
               << m_clockState.trackToken << m_clockState.epoch;
    }
    // Where a chunk starts on the track, and the first frame it owns: by
    // frame, so that every frame has one owner, whatever the detail
    // window holding it.
    const auto origin = [column, phase, chunkColumns](qint64 index) {
        return (static_cast<double>(index) * chunkColumns + phase) * column;
    };
    // The first frame at or after a column of the grid.
    const auto frameAt = [column, phase, rate](double gridColumn) {
        return static_cast<qint64>(std::ceil((gridColumn + phase) * column * rate - 1e-6));
    };
    const auto boundary = [frameAt, chunkColumns](qint64 index) {
        return frameAt(static_cast<double>(index) * chunkColumns);
    };
    // The frames whose samples the view shows, as the chunks are chosen.
    const qint64 viewFirst = clipToView ? frameAt(firstColumn - before)
                                        : std::numeric_limits<qint64>::min();
    const qint64 viewEnd = clipToView
        ? frameAt(firstColumn + std::ceil(columns - kColumnEpsilon) + after)
        : std::numeric_limits<qint64>::max();
    // The frames a chunk owns, those of them the view shows.
    const auto owned = [boundary, viewFirst, viewEnd](qint64 index) {
        const qint64 first = std::max(boundary(index), viewFirst);
        return SampleOwnership{first, std::max(first, std::min(boundary(index + 1), viewEnd))};
    };
    chunks.offset = [firstColumn, scale, chunkColumns](qint64 index) {
        return (static_cast<double>(index) * chunkColumns - firstColumn) / scale;
    };
    // A chunk holds for the next detail window while this one holds every
    // sample it takes shape from; one at an edge of the window holds only
    // for the window. The frames it owns tell one built whole from one
    // built for a view.
    chunks.stamp = [this, owned, rate](qint64 index) -> WaveformSceneGraph::Chunks::Stamp {
        const SampleOwnership frames = owned(index);
        const qint64 detailFirst = std::llround(m_detail.startSeconds * rate);
        const qint64 detailEnd = detailFirst + m_detail.pointCount;
        const bool fromStart = detailFirst <= 0
            || frames.firstFrame - kSampleCurveContextSamples >= detailFirst;
        const bool toEnd = frames.endFrame + kSampleCurveContextSamples <= detailEnd
            || m_detail.endSeconds + 2.0 / rate >= m_durationSeconds;
        return {fromStart && toEnd ? 0 : m_detail.generation, frames.firstFrame, frames.endFrame};
    };
    chunks.paint = [this, origin, owned, span, width, height, channels, scale, style](
                       QPainter &painter, qint64 index) {
        // A view of the same span and width starting where the chunk does.
        const double start = origin(index);
        const SampleOwnership frames = owned(index);
        drawDetailSlice(painter, m_detail, style, width, height, start, start + span, channels,
            0, width, 1.0, scale, &frames);
    };
    return chunks;
}

QSGNode *WaveformEditorItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) {
    // Qt asks a hidden item for its node too, whenever it changes or enters
    // a window, as the waveform the spectrogram replaces does with it when
    // the fullscreen viewer opens. Hidden, it keeps what it has and draws
    // nothing; shown again, it asks for a frame (shownChanged).
    if (!isVisible()) {
        return oldNode;
    }
    bool rasterize = false;
    if (window() && window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
        QMutexLocker lock(&m_stateMutex);
        const auto [start, end] = visibleRangeLocked();
        // The software renderer draws no geometry: samples drawn directly,
        // and the envelope that stands in for playback tiles still being
        // made, go into one raster.
        rasterize = renderDetailDirectlyLocked(start, end)
            || playbackTilesEligibleLocked(start, end);
    }
    return WaveformSceneGraph::render(oldNode, window(),
        QSize(std::max(1, static_cast<int>(std::floor(width()))),
              std::max(1, static_cast<int>(std::floor(height())))),
        [this](QPainter *painter) { paint(painter); }, rasterize);
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
    double deviceColumns = 1.0;
    int channels = 1;
    int fpsValue = 0;
    bool showFps = false;
    QSizeF reservedCorner;
    bool directDetail = false;
    bool tiledPlayback = false;
    bool playbackTilesComplete = false;
    int playbackTilesRendered = 0;
    std::vector<std::pair<int, int>> playbackTileGaps;
    std::vector<PlaybackTilePaint> playbackTilePaints;
    bool presentationCommitted = false;
    bool stagingContinues = false;
    double scale = 1.0;
    Colors colors;
    double markerSeconds = -1.0;
    const int canvasWidth = std::max(1, static_cast<int>(std::floor(width())));
    const int canvasHeight = std::max(1, static_cast<int>(std::floor(height())));
    // Replace body pixels rather than blending with the previous raster; the
    // overlays switch back to source-over below for their intended alpha.
    painter->setCompositionMode(QPainter::CompositionMode_Source);
    {
        QMutexLocker lock(&m_stateMutex);
        // One time for the whole frame: the view and the playhead move with
        // the clock, and must not move apart while the frame is drawn.
        const qint64 frameNs = m_nowNs();
        colors = m_colors;
        markerSeconds = m_markerSeconds;
        presentationCommitted = advanceStagedCacheLocked(frameNs);
        std::tie(visibleStart, visibleEnd) = visibleRangeLocked(frameNs);
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
                || m_cachedViewportHeight != renderPixelHeightLocked());
#endif
        if (!directDetail) {
            m_lastSampleChunkColumn = std::numeric_limits<double>::quiet_NaN();
            m_sampleChunksClipped = false;
        }
        if (directDetail) {
            painter->fillRect(QRect(0, 0, canvasWidth, canvasHeight), colors.background);
            // The scene graph keeps the samples' geometry, in chunks; any
            // other painter draws them.
            const int lanes = displayedChannelCountLocked();
            const double column = sampleChunkColumnLocked(visibleStart);
            const bool clipToView = clipSampleChunksLocked(column);
            const auto chunks = sampleChunksLocked(
                visibleStart, canvasWidth, canvasHeight, lanes, clipToView);
            if (chunks && WaveformSceneGraph::drawChunks(*painter, *chunks)) {
                m_lastSampleChunkColumn = column;
                m_sampleChunksClipped = clipToView;
            } else {
                m_lastSampleChunkColumn = std::numeric_limits<double>::quiet_NaN();
                m_sampleChunksClipped = false;
                drawDetailLocked(
                    *painter, canvasWidth, canvasHeight,
                    visibleStart, visibleEnd, lanes, 1.0,
                    renderScaleLocked());
            }
        } else {
            tiledPlayback = playbackTilesEligibleLocked(
                visibleStart, visibleEnd);
            const bool currentDetailReady =
                detailCoversRangeLocked(visibleStart, visibleEnd)
                && detailResolutionCoversLocked(visibleStart, visibleEnd)
                && m_detail.framesPerPoint > 0;
            if (tiledPlayback && currentDetailReady) {
                preparePlaybackTilesLocked(
                    visibleStart, visibleEnd, renderPixelHeightLocked());
                playbackTilesRendered = renderMissingPlaybackTilesLocked(
                    visibleStart,
                    visibleEnd,
                    renderPixelHeightLocked(),
                    playbackTileRenderBudgetLocked(
                        visibleStart, visibleEnd)
                        + playbackTilesAheadLocked(visibleStart, visibleEnd));
            }
            if (tiledPlayback) {
                playbackTilesComplete = playbackTilesCoverLocked(
                    visibleStart, visibleEnd);
                playbackTilePaints = playbackTilePaintsLocked(
                    visibleStart, visibleEnd, canvasWidth, canvasHeight);
                // Tiles still being made show as the detail's envelope
                // until they come, rather than a cache drawn now for them.
                if (!playbackTilesComplete) {
                    playbackTileGaps = playbackTileGapsLocked(
                        visibleStart, visibleEnd, canvasWidth);
                }
            } else {
                // A cache that playback left while the view had no detail
                // is drawn again once the view stops moving, or another
                // window comes: whatever the frame that asked for this one.
                if (!m_cacheDirty && cacheRedrawDueLocked(visibleStart, visibleEnd)) {
                    invalidateCacheLocked();
                }
                rebuildCacheLocked(
                    renderPixelWidthLocked(), renderPixelHeightLocked(),
                    visibleStart, visibleEnd);
                cache = m_cache;
            }
        }
        hover = m_hoverPosition;
        hoverActive = m_hoverActive;
        crosshair = m_crosshairEnabled;
        scale = renderScaleLocked();
        position = displayedPositionSecondsLocked(frameNs);
        // Tiles made here, a few at a time, come in the next frames; tiles
        // from the workers ask for their own frame.
        const bool tilesContinue = m_playbackTilesInline
            && playbackTilesRendered > 0 && !playbackTilesComplete;
        m_paintedFrameChangeSeconds = tilesContinue
            ? std::numeric_limits<double>::quiet_NaN()
            : frameChangeSecondsLocked(visibleStart, visibleEnd, position, canvasWidth);
        m_fastScrollCapped = fastScrollCappedLocked(canvasWidth);
        if (const qint64 interval = cappedFrameIntervalNsLocked(); interval > 0) {
            // A frame drawn when the last one asked for it, or within a
            // refresh after, as a timer wakes late, keeps to the grid;
            // any other, such as one drawn at once for a seek or a zoom,
            // starts it again from itself.
            const qint64 refreshNs = interval / fastScrollRefreshDivisor(m_refreshRateHz);
            const qint64 due = m_nextCappedFrameNs;
            const bool onGrid = due > 0 && frameNs >= due && frameNs - due < refreshNs;
            m_nextCappedFrameNs = (onGrid ? due : frameNs) + interval;
        } else {
            m_nextCappedFrameNs = 0;
        }
        cacheStart = m_cacheStartSeconds;
        cacheSecondsPerPixel = m_cacheSecondsPerPixel;
        deviceColumns = deviceColumnsLocked();
        channels = displayedChannelCountLocked();
        showFps = m_showFpsOverlay;
        reservedCorner = m_reservedCorner;
        stagingContinues = !m_stagedCache.isNull();
        if (showFps) updateFpsEstimateLocked();
        fpsValue = m_fpsValue;
    }
    if (!directDetail) {
        bool cachePaintCoversCanvas = false;
        bool cachePainted = false;
        if (tiledPlayback) {
            painter->fillRect(
                QRect(0, 0, canvasWidth, canvasHeight), colors.background);
            cachePaintCoversCanvas = true;
            cachePainted = true;
        } else if (cacheSecondsPerPixel > 0.0 && visibleEnd > visibleStart) {
            // The cache's pixels over the view's width: the view's device
            // columns, which may end in a fraction of one.
            double sourceX = (visibleStart - cacheStart)
                / cacheSecondsPerPixel;
            const double viewPixels = (visibleEnd - visibleStart)
                / cacheSecondsPerPixel;
            double sourceWidth = viewPixels;
            double destinationScale = static_cast<double>(canvasWidth)
                / viewPixels;
            if (std::abs(viewPixels - deviceColumns) < kColumnEpsilon
                && std::abs(sourceX - std::round(sourceX)) < kColumnEpsilon) {
                // A cache on the view's grid is drawn a pixel to a device
                // pixel, from whole pixels of it, so that a step of the view
                // moves the drawing by one: a fraction of a pixel, at an
                // edge or from rounding, would make a renderer stretch it.
                // A last pixel partly outside the view is clipped.
                sourceX = std::round(sourceX);
                sourceWidth = std::ceil(deviceColumns - kColumnEpsilon);
                destinationScale = static_cast<double>(canvasWidth) / deviceColumns;
            }
            const QRectF requestedSource(
                sourceX, 0.0, sourceWidth, cache.height());
            const QRectF clippedSource = requestedSource.intersected(
                QRectF(0.0, 0.0, cache.width(), cache.height()));
            if (!clippedSource.isEmpty()) {
                cachePaintCoversCanvas = clippedSource == requestedSource;
                if (!cachePaintCoversCanvas) {
                    painter->fillRect(
                        QRect(0, 0, canvasWidth, canvasHeight), colors.background);
                }
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
                QRect(0, 0, canvasWidth, canvasHeight), colors.background);
        }
        for (const PlaybackTilePaint &tile : playbackTilePaints) {
            painter->drawImage(tile.target, tile.image, tile.source);
        }
        if (!playbackTileGaps.empty()) {
            QMutexLocker lock(&m_stateMutex);
            for (const auto &[from, to] : playbackTileGaps) {
                drawDetailEnvelope(*painter, m_detail, laneStyleLocked(), canvasWidth,
                    canvasHeight, visibleStart, visibleEnd, channels, from, to);
            }
        }
    }
    painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
    {
        QMutexLocker lock(&m_stateMutex);
        drawGridLocked(
            *painter, canvasWidth, canvasHeight, visibleStart, visibleEnd, channels);
        drawChannelSeparators(*painter, canvasWidth, canvasHeight, channels, colors.laneSeam);
    }
    publishMarkerX(markerSeconds >= 0.0 && visibleEnd > visibleStart
        ? std::round((markerSeconds - visibleStart) / (visibleEnd - visibleStart) * canvasWidth)
        : std::numeric_limits<double>::quiet_NaN());
    if (visibleEnd > visibleStart && position >= visibleStart && position <= visibleEnd) {
        const TimeAxis axis{visibleStart, visibleEnd, canvasWidth, scale};
        const double x = axis.x(axis.columnOf(position));
        // A device pixel's core cased in one on each side: the column's
        // left edge is on one, and cosmetic pens draw a device pixel wide.
        const double pixel = 1.0 / scale;
        const auto line = [&](double at, const QColor &color) {
            QPen pen(color);
            pen.setWidth(0);
            pen.setCapStyle(Qt::FlatCap);
            painter->setPen(pen);
            painter->drawLine(QLineF(at, 0.0, at, canvasHeight - 1.0));
        };
        line(x - pixel, colors.playheadCasing);
        line(x + pixel, colors.playheadCasing);
        line(x, colors.playhead);
    }
    if (crosshair && hoverActive) {
        QMutexLocker lock(&m_stateMutex);
        m_hoverPosition = hover;
        drawCrosshair(*painter, canvasWidth, canvasHeight, visibleStart, visibleEnd);
    }
    if (showFps && fpsValue > 0) drawFpsOverlay(*painter, canvasWidth, fpsValue, reservedCorner);
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

    // Keep compatible tiles even if playback overtakes their right edge while
    // detail is in flight. Discarding the overlap forces a fullscreen raster
    // rebuild, which delays the next result and repeatedly exhausts lookahead.
    // Absolute tile coordinates keep retained pixels valid across nearby seeks;
    // source, scale and appearance changes invalidate them separately.
    // Device columns of the view, as the cache's pixels.
    const double secondsPerPixel = (visibleEnd - visibleStart) / deviceColumnsLocked();
    const double tolerance = std::max(1.0e-12, secondsPerPixel * 1.0e-7);
    const bool compatible = !m_playbackTiles.empty()
        && std::abs(m_playbackTileSecondsPerPixel - secondsPerPixel)
            <= tolerance
        && m_playbackTileHeight == renderPixelHeightLocked()
        && m_playbackTileDisplayedChannels == displayedChannelCountLocked();
    if (!compatible) return false;
    const double tileDuration = secondsPerPixel * kPlaybackTileWidth;
    const qint64 first = static_cast<qint64>(std::floor(
        visibleStart / tileDuration + kGridAlignmentEpsilon));
    const auto tile = m_playbackTiles.lower_bound(first);
    return tile != m_playbackTiles.end()
        && static_cast<double>(tile->first) * tileDuration < visibleEnd;
}

void WaveformEditorItem::clearPlaybackTilesLocked() {
    m_playbackTiles.clear();
    // Tiles workers are making for these are not taken.
    ++m_playbackTileGeneration;
    m_playbackTileWorkers->generation.store(m_playbackTileGeneration);
    m_playbackTilesPending.clear();
    m_playbackTileSecondsPerPixel = 0.0;
    m_playbackTileHeight = 0;
    m_playbackTileFramesPerPoint = 0;
    m_playbackTileDisplayedChannels = 0;
}

void WaveformEditorItem::preparePlaybackTilesLocked(
    double visibleStart, double visibleEnd, int height) {
    // Device columns of the view, as the cache's pixels.
    const double secondsPerPixel = (visibleEnd - visibleStart) / deviceColumnsLocked();
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

QThreadPool *WaveformEditorItem::playbackTileThreadPool() {
    return playbackTilePool();
}

WaveformEditorItem::PlaybackTile WaveformEditorItem::renderPlaybackTile(
    const DetailWindow &detail, const LaneStyle &style, int height, double tileStart,
    double tileEnd, int channels, int firstX, int lastX, double pixelScale) {
    // Drawn strip by strip, so that the scene graph takes each as a texture
    // as it is, rather than copying a taller one into textures in the frame.
    PlaybackTile tile;
    for (int top = 0; top < height; top += WaveformSceneGraph::kTextureSize) {
        QImage strip(kPlaybackTileWidth, std::min(WaveformSceneGraph::kTextureSize, height - top),
            QImage::Format_RGB32);
        strip.fill(style.background);
        QPainter painter(&strip);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.translate(0, -top);
        drawDetailSlice(painter, detail, style, kPlaybackTileWidth, height,
            tileStart, tileEnd, channels, firstX, lastX, pixelScale);
        painter.end();
        tile.strips.push_back(std::move(strip));
    }
    return tile;
}

int WaveformEditorItem::playbackTilesAheadLocked(
    double visibleStart, double visibleEnd) const {
    if (!m_playing || m_playbackTileSecondsPerPixel <= 0.0) return 0;
    const double tileDuration = m_playbackTileSecondsPerPixel
        * static_cast<double>(kPlaybackTileWidth);
    // Where playback takes the view while the workers make its tiles, up
    // to a view's width.
    const double visibleTiles = std::ceil((visibleEnd - visibleStart) / tileDuration);
    return static_cast<int>(std::min(
        visibleTiles, std::ceil(kPlaybackTileLookaheadSeconds / tileDuration)));
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
    const int ahead = playbackTilesAheadLocked(visibleStart, visibleEnd);
    const qint64 firstWanted = firstVisible - kPlaybackTilePrefetch;
    const qint64 lastWanted = lastVisible + std::max(ahead, kPlaybackTilePrefetch);

    for (auto iterator = m_playbackTiles.begin();
         iterator != m_playbackTiles.end();) {
        if (iterator->first < firstWanted - kPlaybackTilePrefetch
            || iterator->first > lastWanted + kPlaybackTilePrefetch) {
            iterator = m_playbackTiles.erase(iterator);
        } else {
            ++iterator;
        }
    }
    // Workers skip the tiles the view has left before they start them.
    m_playbackTileWorkers->firstWanted.store(firstWanted - kPlaybackTilePrefetch);
    m_playbackTileWorkers->lastWanted.store(lastWanted + kPlaybackTilePrefetch);

    // The view's tiles first, then those playback reaches next, then the
    // rest of the margins.
    std::vector<qint64> candidates;
    candidates.reserve(static_cast<std::size_t>(
        std::max<qint64>(0, lastWanted - firstWanted + 1)));
    for (qint64 tile = firstVisible; tile <= lastVisible; ++tile) {
        candidates.push_back(tile);
    }
    for (int margin = 1; margin <= ahead; ++margin) {
        candidates.push_back(lastVisible + margin);
    }
    for (int margin = 1; margin <= kPlaybackTilePrefetch; ++margin) {
        candidates.push_back(firstVisible - margin);
        if (margin > ahead) candidates.push_back(lastVisible + margin);
    }

    struct Job {
        qint64 index;
        double start;
        double end;
        int firstX;
        int lastX;
    };
    std::vector<Job> jobs;
    for (const qint64 tileIndex : candidates) {
        if (static_cast<int>(jobs.size()) >= tileBudget) break;
        if (m_playbackTiles.contains(tileIndex)
            || m_playbackTilesPending.contains(tileIndex)) {
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
        jobs.push_back({tileIndex, tileStart, tileEnd, firstX, lastX});
    }
    if (jobs.empty()) return 0;

    const LaneStyle style = laneStyleLocked();
    const int channels = m_playbackTileDisplayedChannels;
    const double scale = renderScaleLocked();
    if (m_playbackTilesInline) {
        for (const Job &job : jobs) {
            m_playbackTiles.emplace(job.index, renderPlaybackTile(
                m_detail, style, height, job.start, job.end, channels,
                job.firstX, job.lastX, scale));
        }
        return static_cast<int>(jobs.size());
    }

    // The workers draw from a copy of the points the tiles show, with the
    // margin a sample curve draws past a tile's edges.
    double first = jobs.front().start;
    double last = jobs.front().end;
    for (const Job &job : jobs) {
        first = std::min(first, job.start);
        last = std::max(last, job.end);
    }
    const double margin = (kSampleCurveBoundaryMarginPixels + 2) * m_playbackTileSecondsPerPixel;
    const std::shared_ptr<const DetailWindow> detail = detailSnapshotLocked(
        first - margin, last + margin);
    const quint64 generation = m_playbackTileGeneration;
    const std::shared_ptr<PlaybackTileWorkers> workers = m_playbackTileWorkers;
    for (const Job &job : jobs) {
        m_playbackTilesPending.insert(job.index);
        playbackTilePool()->start([workers, generation, detail, style, height, job,
                                   channels, scale]() {
            PlaybackTile tile;
            const auto started = std::chrono::steady_clock::now();
            // A tile of tiles since dropped, or one the view has left, is
            // not made; its request still ends.
            if (workers->generation.load() == generation
                && job.index >= workers->firstWanted.load()
                && job.index <= workers->lastWanted.load()) {
                tile = renderPlaybackTile(*detail, style, height, job.start, job.end,
                    channels, job.firstX, job.lastX, scale);
            }
            const double renderMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            QMutexLocker lock(&workers->mutex);
            if (workers->item != nullptr) {
                workers->item->deliverPlaybackTile(
                    generation, job.index, std::move(tile), renderMs);
            }
        });
    }
    return static_cast<int>(jobs.size());
}

void WaveformEditorItem::deliverPlaybackTile(
    quint64 generation, qint64 index, PlaybackTile tile, double renderMs) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (generation != m_playbackTileGeneration) return;
        m_playbackTilesPending.erase(index);
        if (tile.strips.empty()) return;
        m_playbackTiles.insert_or_assign(index, std::move(tile));
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        if (m_profile.enabled) {
            ++m_profile.tileRenders;
            m_profile.tileRenderMs += renderMs;
            m_profile.maximumTileRenderMs = std::max(m_profile.maximumTileRenderMs, renderMs);
        }
#else
        Q_UNUSED(renderMs);
#endif
    }
    // On the item's thread: the tile shows in the next frame. Called while
    // the workers' lock keeps the item alive.
    if (!m_playbackTileWorkers->updateQueued.exchange(true)) {
        QMetaObject::invokeMethod(this, [this]() {
            m_playbackTileWorkers->updateQueued.store(false);
            requestArrivalFrame();
        }, Qt::QueuedConnection);
    }
}

std::vector<std::pair<int, int>> WaveformEditorItem::playbackTileGapsLocked(
    double visibleStart, double visibleEnd, int canvasWidth) const {
    std::vector<std::pair<int, int>> gaps;
    if (m_playbackTileSecondsPerPixel <= 0.0 || visibleEnd <= visibleStart) {
        gaps.emplace_back(0, canvasWidth);
        return gaps;
    }
    const double tileDuration = m_playbackTileSecondsPerPixel
        * static_cast<double>(kPlaybackTileWidth);
    const qint64 first = static_cast<qint64>(std::floor(
        visibleStart / tileDuration + kGridAlignmentEpsilon));
    const qint64 last = static_cast<qint64>(std::floor(
        std::max(visibleStart, visibleEnd - 1.0e-12) / tileDuration));
    const double pixelsPerSecond = static_cast<double>(canvasWidth) / (visibleEnd - visibleStart);
    for (qint64 tile = first; tile <= last; ++tile) {
        if (m_playbackTiles.contains(tile)) continue;
        // Whole canvas pixels around the tile, which its neighbours cover
        // again where they are there.
        const double start = static_cast<double>(tile) * tileDuration;
        const int from = std::max(0, static_cast<int>(std::floor(
            (start - visibleStart) * pixelsPerSecond)));
        const int to = std::min(canvasWidth, static_cast<int>(std::ceil(
            (start + tileDuration - visibleStart) * pixelsPerSecond)));
        if (to <= from) continue;
        if (!gaps.empty() && gaps.back().second >= from) {
            gaps.back().second = std::max(gaps.back().second, to);
        } else {
            gaps.emplace_back(from, to);
        }
    }
    return gaps;
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
    // Every tile the view shows: at a deep zoom, playback moves the view
    // on by more than its width a frame, and a tile it does not make now
    // would leave its part of the view empty. Tiles shown before cost
    // nothing; the rest of the budget makes tiles ahead of the view.
    int missing = 0;
    for (qint64 tile = first; tile <= last; ++tile) {
        if (!m_playbackTiles.contains(tile)) ++missing;
    }
    return std::max(missing, kPlaybackTilesPerPaint);
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
    double visibleStart, double visibleEnd, int canvasWidth, int canvasHeight) const {
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
    // Logical pixels to a tile's pixel, a device column of the view.
    const double pixelsPerColumn = m_playbackTileSecondsPerPixel / visibleSpan
        * static_cast<double>(canvasWidth);
    for (qint64 tileIndex = first; tileIndex <= last; ++tileIndex) {
        const auto iterator = m_playbackTiles.find(tileIndex);
        if (iterator == m_playbackTiles.end()) continue;
        const double tileStart = static_cast<double>(tileIndex) * tileDuration;
        // Tiles and a view that follows the playhead are on the track's
        // grid of device columns: a tile starts on a column of the view,
        // short of which rounding can place it, leaving that column empty.
        double column = (tileStart - visibleStart) / m_playbackTileSecondsPerPixel;
        if (std::abs(column - std::round(column)) < kColumnEpsilon) column = std::round(column);
        const QRectF target(
            column * pixelsPerColumn,
            0.0,
            static_cast<double>(kPlaybackTileWidth) * pixelsPerColumn,
            static_cast<double>(canvasHeight));
        // Only the part on the canvas.
        const QRectF shown = target.intersected(
            QRectF(0.0, 0.0, canvasWidth, canvasHeight));
        if (shown.isEmpty()) continue;
        const double pixelsPerUnit = static_cast<double>(kPlaybackTileWidth) / target.width();
        // Each strip over its rows of the canvas.
        const std::vector<QImage> &strips = iterator->second.strips;
        int tileHeight = 0;
        for (const QImage &strip : strips) tileHeight += strip.height();
        const double rowsPerUnit = static_cast<double>(std::max(1, tileHeight)) / canvasHeight;
        int top = 0;
        for (const QImage &strip : strips) {
            paints.push_back(PlaybackTilePaint{
                strip,
                QRectF(shown.left(), top / rowsPerUnit, shown.width(), strip.height() / rowsPerUnit),
                QRectF(
                    (shown.left() - target.left()) * pixelsPerUnit,
                    0.0,
                    shown.width() * pixelsPerUnit,
                    static_cast<double>(strip.height())),
            });
            top += strip.height();
        }
    }
    return paints;
}

void WaveformEditorItem::rebuildCacheLocked(
    int width, int height, double visibleStart, double visibleEnd) {
    if (!m_cacheDirty && m_cachedViewportWidth == width && m_cachedViewportHeight == height) return;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const auto profileRebuildStarted = std::chrono::steady_clock::now();
#endif
    const bool detailCoversViewport = detailCoversRangeLocked(
        visibleStart, visibleEnd);
    const bool detailReady = detailCoversViewport
        && detailResolutionCoversLocked(visibleStart, visibleEnd);
    const bool detailOverlapsViewport = m_detail.pointCount > 0
        && visibleStart < m_detail.endSeconds
        && visibleEnd > m_detail.startSeconds;
    // The overview is one envelope of every channel, which the lanes of the
    // per-channel view do not show (drawOverviewLocked): there the detail
    // the view has, of another zoom, stands in for the view's own where it
    // reaches, rather than nothing.
    const bool detailStandsIn = m_viewMode == 1
        && displayedChannelCountLocked() > 1
        && detailOverlapsViewport;
    const bool useOverview = !detailStandsIn
        && (m_zoomFallbackToOverview
            || (detailCoversViewport && !detailReady)
            || !detailOverlapsViewport);
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
    m_cache.fill(m_colors.background);
    QPainter painter(&m_cache);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const int channels = displayedChannelCountLocked();
    const double scale = renderScaleLocked();
    if (detailReady) {
        drawDetailLocked(
            painter, grid.width, height,
            grid.startSeconds, grid.endSeconds, channels, scale);
    } else if (useOverview) {
        drawOverviewLocked(
            painter, grid.width, height,
            grid.startSeconds, grid.endSeconds, channels, scale);
    } else {
        drawDetailLocked(
            painter, grid.width, height,
            grid.startSeconds, grid.endSeconds, channels, scale);
    }
    painter.end();
    m_cachedViewportWidth = width;
    m_cachedViewportHeight = height;
    m_cacheStartSeconds = grid.startSeconds;
    m_cacheEndSeconds = grid.endSeconds;
    m_cacheSecondsPerPixel = grid.secondsPerPixel;
    m_cacheFramesPerPoint = detailReady ? m_detail.framesPerPoint : 0;
    m_cacheDisplayedChannels = channels;
    m_cacheDetailArrivals = m_detailArrivals;
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

void WaveformEditorItem::rebuildCacheLocked(int width, int height) {
    const auto [visibleStart, visibleEnd] = visibleRangeLocked();
    rebuildCacheLocked(width, height, visibleStart, visibleEnd);
}

void WaveformEditorItem::drawChannelSeparators(
    QPainter &painter, int width, int height, int channels, const QColor &color) {
    if (channels < 2) return;
    painter.setPen(QPen(color, 1.0));
    for (int channel = 1; channel < channels; ++channel) {
        const int y = static_cast<int>(std::round(
            static_cast<double>(channel) * height / channels));
        painter.drawLine(0, y, width - 1, y);
    }
}

void WaveformEditorItem::drawContrastingLine(
    QPainter &painter, const QLineF &line,
    const QColor &foreground, const QColor &contrast) {
    painter.save();
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    QPen contrastPen(contrast);
    contrastPen.setWidth(0);
    contrastPen.setCapStyle(Qt::FlatCap);
    painter.setPen(contrastPen);
    painter.drawLine(line);

    QPen foregroundPen(foreground);
    foregroundPen.setWidth(0);
    foregroundPen.setCapStyle(Qt::FlatCap);
    foregroundPen.setStyle(Qt::CustomDashLine);
    foregroundPen.setDashPattern({4.0, 4.0});
    painter.setPen(foregroundPen);
    painter.drawLine(line);
    painter.restore();
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
    int valueTextWidth, int timeTextWidth, int textHeight,
    const QSizeF &reservedCorner, const QRect &laneHead,
    int widestValueTextWidth, int widestTimeTextWidth) {
    // The spectrogram's plates, without an edge.
    constexpr int padding = CrosshairReadout::kPadding;
    return CrosshairReadout::plateRects(
        width, height, x, y,
        QSize(valueTextWidth + 2 * padding, textHeight + 2 * padding),
        QSize(timeTextWidth + 2 * padding, textHeight + 2 * padding),
        reservedCorner, CrosshairReadout::RightScale(), laneHead,
        widestValueTextWidth + 2 * padding, widestTimeTextWidth + 2 * padding);
}

void WaveformEditorItem::drawGridLocked(QPainter &painter, int width, int height,
                                         double visibleStart, double visibleEnd, int channels) const {
    for (int channel = 0; channel < channels; ++channel) {
        const double top = static_cast<double>(channel) * height / channels;
        const double bottom = static_cast<double>(channel + 1) * height / channels;
        const int y = static_cast<int>((top + bottom) * 0.5);
        painter.setPen(QPen(m_colors.centerLine, 0.0));
        painter.drawLine(QLine(0, y, width - 1, y));
    }
    if (!m_gridEnabled) return;

    QFont font;
    font.setPixelSize(9);
    painter.setFont(font);
    const QFontMetrics metrics(font);
    painter.setPen(QPen(kGrid, 1.0));
    const auto drawEdgeLabel = [&](const QString &text, int rightMargin, int baseline) {
        const int textWidth = metrics.horizontalAdvance(text);
        painter.drawText(
            ViewCorner::labelXClearOfCorner(
                width - textWidth - rightMargin, baseline - metrics.ascent(),
                textWidth, metrics.height(), width, m_reservedCorner),
            baseline,
            text);
    };
    const double span = visibleEnd - visibleStart;
    if (span > 0.0) {
        const TimeAxis axis{visibleStart, visibleEnd, width, renderScaleLocked()};
        const double interval = selectTimeInterval(span, width);
        for (double time = std::ceil(visibleStart / interval) * interval; time <= visibleEnd; time += interval) {
            const double x = axis.x(axis.columnOf(time));
            painter.drawLine(QLineF(x, 0.0, x, height - 1.0));
            const QString label = formatTime(time, span);
            const int labelX = std::clamp(
                static_cast<int>(x) - metrics.horizontalAdvance(label) / 2,
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
        drawEdgeLabel(silenceLabel, 3, static_cast<int>(center) - 2);
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
            drawEdgeLabel(text, 3, upper + metrics.ascent());
            drawEdgeLabel(text, 3, lower - 2);
            painter.setPen(QPen(kGrid, 1.0));
        }
    }
}

void WaveformEditorItem::drawOverviewLocked(QPainter &painter, int width, int height,
                                             double visibleStart, double visibleEnd, int channels,
                                             double pixelScale) const {
    // The overview is a combined peak envelope, with no channel identity.
    if (m_overviewData.isEmpty() || m_durationSeconds <= 0.0
        || (m_viewMode == 1 && channels > 1)) return;
    const double coverage = m_overviewComplete ? m_durationSeconds
        : std::min(m_durationSeconds, m_overviewCoverageSeconds);
    if (coverage <= 0.0) return;
    const auto *peaks = reinterpret_cast<const uchar *>(m_overviewData.constData());
    const int count = m_overviewData.size();
    const double span = visibleEnd - visibleStart;
    const double secondsPerPixel = span
        / static_cast<double>(std::max(1, width));
    for (int channel = 0; channel < channels; ++channel) {
        const double top = static_cast<double>(channel) * height / channels;
        const double bottom = static_cast<double>(channel + 1) * height / channels;
        const double center = (top + bottom) * 0.5;
        const double half = (bottom - top) * 0.5 - pixelScale;
        painter.setPen(QPen(laneStyleLocked().waveformOf(channel), 1.0));
        for (int x = 0; x < width; ++x) {
            const double start = std::max(0.0, visibleStart + x * secondsPerPixel);
            const double end = std::min(coverage, visibleStart + (x + 1) * secondsPerPixel);
            if (start >= end) continue;
            const int first = std::clamp(static_cast<int>(std::floor(start / coverage * count)), 0, count - 1);
            const int last = std::clamp(static_cast<int>(std::ceil(end / coverage * count)), first + 1, count);
            const double peak = static_cast<double>(*std::max_element(peaks + first, peaks + last)) / 255.0;
            painter.drawLine(x, static_cast<int>(center - peak * half), x, static_cast<int>(center + peak * half));
        }
    }
}

void WaveformEditorItem::drawDetailLocked(QPainter &painter, int width, int height,
                                           double visibleStart, double visibleEnd, int channels,
                                           double pixelScale, double deviceScale) const {
    drawDetailSliceLocked(painter, width, height, visibleStart, visibleEnd, channels,
        0, width, pixelScale, deviceScale);
}

void WaveformEditorItem::drawDetailSliceLocked(
    QPainter &painter, int width, int height,
    double visibleStart, double visibleEnd, int channels,
    int firstX, int lastX, double pixelScale, double deviceScale) const {
    drawDetailSlice(painter, m_detail, laneStyleLocked(), width, height,
        visibleStart, visibleEnd, channels, firstX, lastX, pixelScale, deviceScale);
}

QRectF WaveformEditorItem::sampleDot(QPointF point, double size, double deviceScale) {
    const double pixels = std::max(1.0, std::round(size * deviceScale));
    // Halves round up, left of the origin too, so that a marker moved by
    // whole device pixels, as a chunk is, snaps as it would in place.
    const double left = std::floor(point.x() * deviceScale - pixels * 0.5 + 0.5);
    const double top = std::floor(point.y() * deviceScale - pixels * 0.5 + 0.5);
    return {left / deviceScale, top / deviceScale, pixels / deviceScale, pixels / deviceScale};
}

void WaveformEditorItem::drawDetailSlice(
    QPainter &painter, const DetailWindow &detail, const LaneStyle &style,
    int width, int height, double visibleStart, double visibleEnd, int channels,
    int firstX, int lastX, double pixelScale, double deviceScale,
    const SampleOwnership *ownership) {
    const double detailSpan = detail.endSeconds - detail.startSeconds;
    const double visibleSpan = visibleEnd - visibleStart;
    firstX = std::clamp(firstX, 0, width);
    lastX = std::clamp(lastX, firstX, width);
    if (detailSpan <= 0.0 || visibleSpan <= 0.0 || firstX >= lastX) return;
    // Owned samples are counted in frames: samples, at a point each.
    if (ownership && (detail.framesPerPoint != 1 || detail.sampleRateHz <= 0)) return;
    const double sampleSpacing = sampleSpacingPixels(
        detail, width, visibleStart, visibleEnd);
    const bool sampleCurve = sampleSpacing + kGridAlignmentEpsilon
        >= kSampleCurvePixelsPerSample;
    // Markers follow the spacing in logical pixels, whatever the painter's
    // scale, as drawing samples directly does: a cache or a playback tile
    // in device pixels draws none where the view is not drawn directly.
    const bool sampleMarkers = sampleSpacing / pixelScale + kGridAlignmentEpsilon
        >= kSampleMarkerPixelsPerSample;
    const double sampleMarkerSize = std::clamp(
        sampleSpacing * 0.75,
        kMinimumSampleMarkerSize * pixelScale,
        kMaximumSampleMarkerSize * pixelScale);
    const bool straightSegments = sampleSpacing / pixelScale + kGridAlignmentEpsilon
        < style.straightSamplePixelsPerSample;
    const double secondsPerPixel = visibleSpan
        / static_cast<double>(std::max(1, width));
    // Curves take in the samples whose segments, or their neighbours'
    // joins, reach into the slice, so that it shows them as a wider one
    // would: three samples and a marker beyond its edges.
    const int boundaryMargin = sampleCurve
        ? std::max(kSampleCurveBoundaryMarginPixels,
            static_cast<int>(std::ceil(sampleSpacing * kSampleCurveContextSamples + 3.0)))
        : 1;
    // A point of an envelope stands for its frames: a point that ends in
    // the slice is in it however far before it its middle is.
    const double pointSeconds = detail.sampleRateHz > 0
        ? static_cast<double>(detail.framesPerPoint) / static_cast<double>(detail.sampleRateHz)
        : 0.0;
    const double pointMargin = sampleCurve ? 0.0 : pointSeconds;
    const double sliceStart = visibleStart
        + static_cast<double>(firstX - boundaryMargin) * secondsPerPixel - pointMargin;
    const double sliceEnd = visibleStart
        + static_cast<double>(lastX + boundaryMargin) * secondsPerPixel + pointMargin;
    auto [firstPoint, lastPoint] = detailPointRange(detail,
        sliceStart, sliceEnd);
    // The owned samples' points, and as their polyline's indices.
    int ownedFirst = 0;
    int ownedEnd = 0;
    if (ownership) {
        const qint64 detailFirstFrame = std::llround(detail.startSeconds * detail.sampleRateHz);
        const auto point = [&detail, detailFirstFrame](qint64 frame) {
            return static_cast<int>(std::clamp<qint64>(frame - detailFirstFrame, 0, detail.pointCount));
        };
        ownedFirst = point(ownership->firstFrame);
        ownedEnd = std::max(ownedFirst, point(ownership->endFrame));
        firstPoint = point(ownership->firstFrame - kSampleCurveContextSamples);
        lastPoint = std::max(firstPoint, point(ownership->endFrame + kSampleCurveContextSamples));
        ownedFirst -= firstPoint;
        ownedEnd -= firstPoint;
    }
    const int sliceWidth = lastX - firstX;
    for (int displayChannel = 0; displayChannel < channels; ++displayChannel) {
        const double top = static_cast<double>(displayChannel) * height / channels;
        const double bottom = static_cast<double>(displayChannel + 1) * height / channels;
        const double center = (top + bottom) * 0.5;
        const double half = (bottom - top) * 0.5 - pixelScale;
        const QColor color = style.waveformOf(displayChannel);
        // Columns are a pixel wide; curves keep their logical width.
        painter.setPen(QPen(color, sampleCurve ? (sampleMarkers ? 1.2 : 1.0) * pixelScale : 1.0));
        QPolygonF polyline;
        std::vector<float> pixelMinima;
        std::vector<float> pixelMaxima;
        std::vector<bool> pixelSeen;
        if (sampleCurve) {
            polyline.reserve(lastPoint - firstPoint);
        } else {
            pixelMinima.assign(static_cast<std::size_t>(sliceWidth), 1.0F);
            pixelMaxima.assign(static_cast<std::size_t>(sliceWidth), -1.0F);
            pixelSeen.assign(static_cast<std::size_t>(sliceWidth), false);
        }
        for (int point = firstPoint; point < lastPoint; ++point) {
            const double time = detailPointTime(detail, point);
            // An envelope's point is of the pixels whose middles are in its
            // frames, and a point narrower than a pixel of the pixel its
            // frames are in the middle of: points further apart than a
            // pixel, as when a zoom shows the detail of a wider view until
            // its own arrives, cover the pixels of their frames.
            int firstPixel = 0;
            int endPixel = 0;
            if (!sampleCurve) {
                const double pointStart = detail.startSeconds
                    + static_cast<double>(point) * pointSeconds;
                const double startPosition = (pointStart - visibleStart) / secondsPerPixel;
                const double endPosition =
                    (pointStart + pointSeconds - visibleStart) / secondsPerPixel;
                firstPixel = static_cast<int>(std::ceil(startPosition - 0.5 - kGridAlignmentEpsilon));
                endPixel = static_cast<int>(std::ceil(endPosition - 0.5 - kGridAlignmentEpsilon));
                if (endPixel <= firstPixel) {
                    if (time < visibleStart || time >= visibleEnd) {
                        continue;
                    }
                    firstPixel = static_cast<int>(std::floor(
                        (time - visibleStart) / secondsPerPixel + kGridAlignmentEpsilon));
                    endPixel = firstPixel + 1;
                }
                firstPixel = std::max(firstPixel, firstX);
                endPixel = std::min(endPixel, lastX);
                if (endPixel <= firstPixel) {
                    continue;
                }
            }
            // From the slice's start: the same for a view and for a chunk
            // that starts whole device pixels from it, but for the
            // difference.
            const double pixelPosition = sampleCurve
                ? std::round(
                    (time - visibleStart) / secondsPerPixel
                    * kSampleCurveSubpixelScale)
                    / kSampleCurveSubpixelScale
                : (time - visibleStart) / secondsPerPixel;
            if (sampleCurve
                && !ownership
                && (pixelPosition < firstX - boundaryMargin
                    || pixelPosition >= lastX + boundaryMargin)) {
                continue;
            }
            float minimum = 1.0F;
            float maximum = -1.0F;
            const int firstChannel = style.viewMode == 0 ? 0 : displayChannel;
            const int lastChannel = style.viewMode == 0 ? detail.channelCount : displayChannel + 1;
            for (int sourceChannel = firstChannel; sourceChannel < lastChannel; ++sourceChannel) {
                const std::size_t index = (static_cast<std::size_t>(point) * detail.channelCount + sourceChannel) * 2U;
                minimum = std::min(minimum, detail.extrema[index]);
                maximum = std::max(maximum, detail.extrema[index + 1]);
            }
            if (style.viewMode == 0 && !detail.downmixExtrema.empty()) {
                const auto index = static_cast<std::size_t>(point) * 2U;
                minimum = detail.downmixExtrema[index];
                maximum = detail.downmixExtrema[index + 1];
            }
            if (sampleCurve) {
                polyline.append(QPointF(
                    pixelPosition,
                    center - static_cast<double>(minimum) * half));
            } else {
                for (int x = firstPixel; x < endPixel; ++x) {
                    const auto pixel = static_cast<std::size_t>(x - firstX);
                    pixelMinima[pixel] = std::min(pixelMinima[pixel], minimum);
                    pixelMaxima[pixel] = std::max(pixelMaxima[pixel], maximum);
                    pixelSeen[pixel] = true;
                }
            }
        }
        if (!sampleCurve) {
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
        if (sampleCurve && polyline.size() > 1) {
            painter.save();
            painter.setClipRect(
                QRect(firstX, 0, sliceWidth, height),
                Qt::IntersectClip);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setBrush(Qt::NoBrush);
            // A chunk's segments, from its samples, with the others to join
            // them to; every point is on its polyline.
            if (ownership) WaveformSceneGraph::strokeOnlySegments(painter, ownedFirst, ownedEnd);
            const qsizetype markersFirst = ownership ? ownedFirst : 0;
            const qsizetype markersEnd = ownership ? ownedEnd : polyline.size();
            if (straightSegments) {
                painter.drawPolyline(polyline);
            } else {
                painter.drawPath(buildSamplePath(polyline));
            }
            if (sampleMarkers && straightSegments) {
                // The squares the pen and fill would cover, in whole device
                // pixels: all the same size, with edges that need no
                // antialiasing. One batch, which the scene graph makes one
                // quad each.
                const double dotSize = sampleMarkerSize + painter.pen().widthF();
                QVector<QRectF> dots;
                dots.reserve(markersEnd - markersFirst);
                for (qsizetype index = markersFirst; index < markersEnd; ++index) {
                    dots.push_back(sampleDot(polyline.at(index), dotSize, deviceScale));
                }
                painter.setRenderHint(QPainter::Antialiasing, false);
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                painter.drawRects(dots.constData(), static_cast<int>(dots.size()));
            } else if (sampleMarkers) {
                painter.setBrush(color);
                const double markerOffset = sampleMarkerSize * 0.5;
                for (qsizetype index = markersFirst; index < markersEnd; ++index) {
                    const QPointF point = polyline.at(index);
                    painter.drawRect(QRectF(
                        point.x() - markerOffset,
                        point.y() - markerOffset,
                        sampleMarkerSize,
                        sampleMarkerSize));
                }
            }
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.restore();
        }
    }
}

void WaveformEditorItem::drawDetailEnvelope(
    QPainter &painter, const DetailWindow &detail, const LaneStyle &style,
    int width, int height, double visibleStart, double visibleEnd, int channels,
    int firstX, int lastX) {
    const double visibleSpan = visibleEnd - visibleStart;
    firstX = std::clamp(firstX, 0, width);
    lastX = std::clamp(lastX, firstX, width);
    if (detail.pointCount <= 0 || visibleSpan <= 0.0 || firstX >= lastX) return;
    const double secondsPerPixel = visibleSpan / static_cast<double>(std::max(1, width));
    // A point either side, so that the lines into the slice's edges show.
    const auto [firstPoint, lastPoint] = detailPointRange(
        detail,
        visibleStart + static_cast<double>(firstX - 1) * secondsPerPixel,
        visibleStart + static_cast<double>(lastX + 1) * secondsPerPixel);
    const int sliceWidth = lastX - firstX;
    std::vector<float> minima(static_cast<std::size_t>(sliceWidth));
    std::vector<float> maxima(static_cast<std::size_t>(sliceWidth));
    QList<QRectF> columns;
    columns.reserve(sliceWidth);
    for (int displayChannel = 0; displayChannel < channels; ++displayChannel) {
        const double top = static_cast<double>(displayChannel) * height / channels;
        const double bottom = static_cast<double>(displayChannel + 1) * height / channels;
        const double center = (top + bottom) * 0.5;
        const double half = (bottom - top) * 0.5 - 1.0;
        std::fill(minima.begin(), minima.end(), std::numeric_limits<float>::max());
        std::fill(maxima.begin(), maxima.end(), std::numeric_limits<float>::lowest());
        const int firstChannel = style.viewMode == 0 ? 0 : displayChannel;
        const int lastChannel = style.viewMode == 0 ? detail.channelCount : displayChannel + 1;
        bool previous = false;
        double previousX = 0.0;
        float previousMinimum = 0.0F;
        float previousMaximum = 0.0F;
        for (int point = firstPoint; point < lastPoint; ++point) {
            float minimum = 1.0F;
            float maximum = -1.0F;
            if (style.viewMode == 0 && !detail.downmixExtrema.empty()) {
                minimum = detail.downmixExtrema[static_cast<std::size_t>(point) * 2U];
                maximum = detail.downmixExtrema[static_cast<std::size_t>(point) * 2U + 1U];
            } else {
                for (int channel = firstChannel; channel < lastChannel; ++channel) {
                    const std::size_t index = (static_cast<std::size_t>(point) * detail.channelCount
                        + static_cast<std::size_t>(channel)) * 2U;
                    minimum = std::min(minimum, detail.extrema[index]);
                    maximum = std::max(maximum, detail.extrema[index + 1]);
                }
            }
            const double x = (detailPointTime(detail, point) - visibleStart) / secondsPerPixel;
            // The columns from the previous point to this one hold the line
            // between them, so that the envelope has no gaps.
            const int from = static_cast<int>(std::floor(previous ? previousX : x));
            const int to = static_cast<int>(std::floor(x));
            const float low = previous ? std::min(minimum, previousMinimum) : minimum;
            const float high = previous ? std::max(maximum, previousMaximum) : maximum;
            for (int column = std::max(from, firstX); column <= std::min(to, lastX - 1); ++column) {
                const auto pixel = static_cast<std::size_t>(column - firstX);
                minima[pixel] = std::min(minima[pixel], low);
                maxima[pixel] = std::max(maxima[pixel], high);
            }
            previous = true;
            previousX = x;
            previousMinimum = minimum;
            previousMaximum = maximum;
        }
        columns.clear();
        for (int x = firstX; x < lastX; ++x) {
            const auto pixel = static_cast<std::size_t>(x - firstX);
            if (minima[pixel] > maxima[pixel]) continue;
            // The whole column, at any scale, from the top of its highest
            // pixel to the bottom of its lowest.
            const double top = std::round(center - static_cast<double>(maxima[pixel]) * half);
            const double bottom = std::round(center - static_cast<double>(minima[pixel]) * half);
            columns.append(QRectF(x, top, 1.0, bottom - top + 1.0));
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(style.waveformOf(displayChannel));
        // One call: the scene graph makes one geometry of a batch of them.
        if (!columns.isEmpty()) {
            painter.drawRects(columns.constData(), static_cast<int>(columns.size()));
        }
    }
    painter.setBrush(Qt::NoBrush);
}

QRect WaveformEditorItem::lowestLaneHeadLocked(int height, int lanes) const {
    if (m_laneHeadBounds.isEmpty()) return QRect();
    // The lanes' rows, as PlotLanes lays them out.
    const int count = std::max(1, lanes);
    const int top = count > 1
        ? static_cast<int>(std::lround(static_cast<double>(count - 1) * height / count)) + 1
        : 0;
    return m_laneHeadBounds.translated(0.0, top).toAlignedRect();
}

namespace {

// The crosshair's readout of a level at `amplitude` of full scale:
// "-12.5 dB", or "-∞ dB" for silence.
QString crosshairLevel(double amplitude) {
    static const QString format = i18nc("@item:intext level in decibels", "%1 dB");
    return amplitude <= 0.00001
        ? format.arg(QStringLiteral("-∞"))
        : format.arg(20.0 * std::log10(amplitude), 0, 'f', 1);
}

} // namespace

int WaveformEditorItem::crosshairWidestLevelWidthLocked() {
    return m_crosshairWidestLevel.get(
        CrosshairReadout::font(), 0.0, [](const QFontMetrics &metrics) {
            // Silence, and the lowest level above it, -100 dB: a louder level
            // has no more digits.
            return std::max(
                CrosshairReadout::widestDigitsWidth(crosshairLevel(0.0), metrics),
                CrosshairReadout::widestDigitsWidth(
                    crosshairLevel(std::nextafter(0.00001, 1.0)), metrics));
        });
}

void WaveformEditorItem::drawCrosshair(QPainter &painter, int width, int height,
                                        double visibleStart, double visibleEnd) {
    // On the column under the pointer, showing the time a click there
    // seeks to.
    const TimeAxis axis{visibleStart, visibleEnd, width, renderScaleLocked()};
    const int column = axis.columnAt(m_hoverPosition.x());
    const double lineX = axis.x(column);
    const int x = static_cast<int>(lineX);
    const int y = std::clamp(static_cast<int>(m_hoverPosition.y()), 0, height - 1);
    QPen pen(QColor(255, 255, 255, 140));
    pen.setStyle(Qt::DotLine);
    pen.setWidthF(1.0);
    painter.setPen(pen);
    painter.drawLine(QLineF(lineX, 0.0, lineX, height - 1.0));
    painter.drawLine(0, y, width - 1, y);
    const double time = axis.timeAt(column);
    const int channels = displayedChannelCountLocked();
    const double channelHeight = static_cast<double>(height) / channels;
    const double localY = std::fmod(static_cast<double>(y), channelHeight);
    const double amplitude = std::clamp(std::abs((channelHeight * 0.5 - localY) / (channelHeight * 0.5)), 0.0, 1.0);
    const QString dbText = crosshairLevel(amplitude);
    const QString timeText = formatCrosshairTime(time);
    const QFont font = CrosshairReadout::font();
    const QFontMetrics metrics(font);
    // Whether a plate shows goes by the widest text it can have, so it
    // doesn't come and go as the level and time under the pointer change.
    const auto [dbRect, timeRect] = crosshairLabelRects(
        width, height, x, y, metrics.horizontalAdvance(dbText),
        metrics.horizontalAdvance(timeText), metrics.height(), m_reservedCorner,
        lowestLaneHeadLocked(height, channels), crosshairWidestLevelWidthLocked(),
        CrosshairReadout::widestDigitsWidth(timeText, metrics));
    // Translucent dark plates without an edge.
    const CrosshairReadout::Colors colors{
        QColor(0, 0, 10, 160), QColor(255, 255, 255, 220), QColor(Qt::transparent)};
    CrosshairReadout::drawPlate(painter, dbRect, dbText, font, colors);
    CrosshairReadout::drawPlate(painter, timeRect, timeText, font, colors);
}

void WaveformEditorItem::drawFpsOverlay(
    QPainter &painter, int width, int fpsValue, const QSizeF &reservedCorner) {
    const QFont font = fpsCounterFont();
    painter.setFont(font);
    const QString text = fpsCounterText(fpsValue);
    const QFontMetrics metrics(font);
    const QRectF counter = ViewCorner::fpsCounterRect(
        width, reservedCorner, QSizeF(metrics.horizontalAdvance(text), metrics.height()));
    painter.setPen(kOverlay);
    painter.drawText(QPointF(counter.x(), counter.y() + metrics.ascent()), text);
}

void WaveformEditorItem::hoverMoveEvent(QHoverEvent *event) { { QMutexLocker lock(&m_stateMutex); m_hoverActive = true; m_hoverPosition = event->position(); } if (m_crosshairEnabled) update(); }
void WaveformEditorItem::hoverEnterEvent(QHoverEvent *event) { hoverMoveEvent(event); }
void WaveformEditorItem::hoverLeaveEvent(QHoverEvent *) { { QMutexLocker lock(&m_stateMutex); m_hoverActive = false; } update(); }

void WaveformEditorItem::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::MiddleButton && zoomEnabled()) { event->accept(); resetZoom(); return; }
    if (event->button() != Qt::RightButton || !crosshairEnabled()) { event->ignore(); return; }
    double seconds = 0.0;
    {
        QMutexLocker lock(&m_stateMutex);
        const auto [start, end] = visibleRangeLocked();
        // Where the crosshair is: see drawCrosshair.
        const TimeAxis axis{
            start, end, std::max(1, static_cast<int>(std::floor(width()))), renderScaleLocked()};
        seconds = end > start ? axis.timeAt(axis.columnAt(event->position().x())) : start;
    }
    event->accept(); emit seekRequested(seconds);
}

void WaveformEditorItem::wheelEvent(QWheelEvent *event) {
    if (!zoomEnabled()) { event->ignore(); return; }
    const double steps = event->angleDelta().y() / 120.0;
    if (std::abs(steps) < 0.01) { event->ignore(); return; }
    event->accept(); setZoomLevel(zoomLevel() * std::pow(kZoomStep, steps));
}
