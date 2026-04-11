// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramItem.h"

#include "SpectrogramSeekTrace.h"

#include <QFontMetrics>
#include <QDateTime>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QMutexLocker>
#include <QPainter>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGSimpleRectNode>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
#define FERROUS_SPECTROGRAM_LOGF(...) std::fprintf(__VA_ARGS__)
#else
#define FERROUS_SPECTROGRAM_LOGF(...) \
    do {                              \
    } while (false)
#endif

namespace {
constexpr double kMinFreqHz = 25.0;
constexpr double kReferenceHopSamples = 1024.0;
constexpr double kPositionJumpHoldThresholdSeconds = 0.75;
constexpr double kPositionHeartbeatRegressionToleranceSeconds = 0.001;
// Last-resort fallback only: the hold is normally released by
// applyPrecomputedResetLocked() when the reset data arrives (~200–400 ms
// after a seek or track change).  The timeout must be long enough that the
// data-driven release always wins, while still being short enough to
// recover if the backend never sends a reset (e.g. stream interruption).
constexpr double kPositionJumpHoldTimeoutSeconds = 2.0;
constexpr int kMaxTileFragments = 96;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
constexpr qint64 kSeekTraceWindowMs = 1800;
constexpr qint64 kSmoothnessWindowMs = 2000;
constexpr qint64 kSmoothnessIdleMs = 450;
#endif
const QColor kBackgroundColor(0, 0, 0);
const QColor kOverlayColor(190, 190, 200, 150);

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

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
bool shouldLogProfileSpike(
    std::chrono::steady_clock::time_point *lastLog,
    std::chrono::steady_clock::time_point now,
    double minIntervalSeconds = 0.20) {
    if (lastLog == nullptr) {
        return true;
    }
    if (lastLog->time_since_epoch().count() != 0) {
        const double elapsed = std::chrono::duration<double>(now - *lastLog).count();
        if (elapsed < minIntervalSeconds) {
            return false;
        }
    }
    *lastLog = now;
    return true;
}

bool seekTraceLooksIncident(
    int gapFrames,
    int stallClusters,
    int regressionCount) {
    return gapFrames >= 3 || stallClusters >= 2 || regressionCount > 0;
}

bool smoothnessLooksIncident(
    int gapFrames,
    int severeGapFrames,
    int stallClusters,
    int regressionCount,
    int paintSpikeCount) {
    return gapFrames >= 4
        || severeGapFrames >= 2
        || stallClusters >= 2
        || regressionCount > 0
        || paintSpikeCount >= 2;
}
#endif

struct SpectrogramSceneNode final : public QSGNode {
    SpectrogramSceneNode() {
        background = new QSGSimpleRectNode();
        tilesRoot = new QSGNode();
        latest = new QSGSimpleTextureNode();
        playhead = new QSGSimpleRectNode();
        freqGridOverlay = new QSGSimpleTextureNode();
        timeGridOverlay = new QSGSimpleTextureNode();
        crosshairOverlay = new QSGSimpleTextureNode();
        overlay = new QSGSimpleTextureNode();
        appendChildNode(background);
        appendChildNode(tilesRoot);
        appendChildNode(latest);
        appendChildNode(playhead);
        appendChildNode(freqGridOverlay);
        appendChildNode(timeGridOverlay);
        appendChildNode(crosshairOverlay);
        appendChildNode(overlay);
        // Visible segments can outnumber source tiles when the ring buffer wraps inside a tile.
        tileFragments.reserve(kMaxTileFragments);
        for (int i = 0; i < kMaxTileFragments; ++i) {
            auto *tileNode = new QSGSimpleTextureNode();
            tilesRoot->appendChildNode(tileNode);
            tileFragments.push_back(tileNode);
        }
    }

    ~SpectrogramSceneNode() override {
        qDeleteAll(tileTextures);
        delete overlayTexture;
        delete freqGridOverlayTexture;
        delete timeGridOverlayTexture;
        delete crosshairOverlayTexture;
        delete placeholderTexture;
    }

    QSGSimpleRectNode *background{nullptr};
    QSGNode *tilesRoot{nullptr};
    QSGSimpleTextureNode *latest{nullptr};
    QSGSimpleRectNode *playhead{nullptr};
    QSGSimpleTextureNode *freqGridOverlay{nullptr};
    QSGSimpleTextureNode *timeGridOverlay{nullptr};
    QSGSimpleTextureNode *crosshairOverlay{nullptr};
    QSGSimpleTextureNode *overlay{nullptr};
    QVector<QSGSimpleTextureNode *> tileFragments;
    QVector<QSGTexture *> tileTextures;
    QSGTexture *overlayTexture{nullptr};
    QSGTexture *freqGridOverlayTexture{nullptr};
    QSGTexture *timeGridOverlayTexture{nullptr};
    QSGTexture *crosshairOverlayTexture{nullptr};
    QSGTexture *placeholderTexture{nullptr};
    quintptr ownerWindowId{0};
    quint64 generation{0};
};

QImage placeholderImage() {
    QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    return image;
}

void configureTextureNode(
    QSGSimpleTextureNode *node,
    QSGTexture *texture,
    const QRectF &target,
    const QRect &source,
    QSGTexture *placeholderTexture) {
    if (node == nullptr) {
        return;
    }
    if (texture == nullptr || target.isEmpty() || source.isEmpty()) {
        if (placeholderTexture != nullptr) {
            node->setTexture(placeholderTexture);
        }
        node->setRect(QRectF());
        node->setSourceRect(QRectF(0.0, 0.0, 1.0, 1.0));
        return;
    }

    node->setTexture(texture);
    node->setFiltering(QSGTexture::Nearest);
    node->setRect(target);
    node->setSourceRect(QRectF(source));
}
QString formatFrequencyLabel(double hz) {
    if (hz < 0.0) {
        return {};
    }
    if (hz >= 1000.0) {
        const double kHz = hz / 1000.0;
        if (kHz >= 10.0) {
            return QStringLiteral("%1 kHz").arg(qRound(kHz));
        }
        return QStringLiteral("%1 kHz").arg(kHz, 0, 'f', 1);
    }
    return QStringLiteral("%1 Hz").arg(qRound(hz));
}

QString formatTimeLabel(double seconds) {
    if (seconds < 0.0) {
        return {};
    }
    const int totalSeconds = static_cast<int>(std::floor(seconds));
    const int h = totalSeconds / 3600;
    const int m = (totalSeconds % 3600) / 60;
    const int s = totalSeconds % 60;
    if (h > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(m)
        .arg(s, 2, 10, QLatin1Char('0'));
}

// Precise frequency label for crosshair: always 1 Hz resolution.
QString formatFrequencyLabelPrecise(double hz) {
    if (hz < 0.0) {
        return {};
    }
    if (hz >= 1000.0) {
        return QStringLiteral("%1 Hz").arg(qRound(hz));
    }
    return QStringLiteral("%1 Hz").arg(qRound(hz));
}

// Precise time label for crosshair: (HH:)MM:SS.mmm with milliseconds.
QString formatTimeLabelPrecise(double seconds) {
    if (seconds < 0.0) {
        return {};
    }
    const int totalMs = static_cast<int>(std::round(seconds * 1000.0));
    const int ms = totalMs % 1000;
    const int totalSec = totalMs / 1000;
    const int h = totalSec / 3600;
    const int m = (totalSec % 3600) / 60;
    const int s = totalSec % 60;
    if (h > 0) {
        return QStringLiteral("%1:%2:%3.%4")
            .arg(h)
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'))
            .arg(ms, 3, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2.%3")
        .arg(m)
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

// Convert widget pixel X to track time, accounting for sub-pixel scroll phase.
// drawX is the widget X where canvas column displayLeft is rendered.
double pixelToTimeSeconds(
    double pixelX,
    qint64 displayLeft,
    bool rollingMode,
    qint64 rollingEpoch,
    double columnsPerSecond,
    double drawX,
    double zoomLevel = 1.0) {
    if (columnsPerSecond <= 0.0) {
        return -1.0;
    }
    const double columnsPerPixel = 1.0 / zoomLevel;
    const double columnF =
        static_cast<double>(displayLeft) + (pixelX - drawX) * columnsPerPixel;
    double trackColumn = columnF;
    if (rollingMode) {
        trackColumn -= static_cast<double>(rollingEpoch);
    }
    return trackColumn / columnsPerSecond;
}

// Convert track time to widget pixel X, accounting for sub-pixel scroll phase.
double timeToPixelX(
    double timeSeconds,
    qint64 displayLeft,
    bool rollingMode,
    qint64 rollingEpoch,
    double columnsPerSecond,
    double drawX,
    double zoomLevel = 1.0) {
    double column = timeSeconds * columnsPerSecond;
    if (rollingMode) {
        column += static_cast<double>(rollingEpoch);
    }
    return drawX + (column - static_cast<double>(displayLeft)) * zoomLevel;
}

// Select the smallest grid interval that keeps at least minPixelSpacing
// pixels between adjacent lines.
double selectGridInterval(
    const double *candidates,
    int candidateCount,
    double unitsPerPixel,
    int minPixelSpacing) {
    for (int i = 0; i < candidateCount; ++i) {
        if (candidates[i] / unitsPerPixel >= static_cast<double>(minPixelSpacing)) {
            return candidates[i];
        }
    }
    return candidates[candidateCount - 1];
}

} // namespace

SpectrogramItem::SpectrogramItem(QQuickItem *parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::RightButton | Qt::MiddleButton);
    setClip(true);
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
    m_zoomDebounceTimer = new QTimer(this);
    m_zoomDebounceTimer->setSingleShot(true);
    m_zoomDebounceTimer->setInterval(150);
    connect(m_zoomDebounceTimer, &QTimer::timeout, this, [this]() {
        emit backendZoomRequested(m_pendingBackendZoom);
    });
}

double SpectrogramItem::dbRange() const {
    return m_dbRange;
}

void SpectrogramItem::setDbRange(double value) {
    QMutexLocker lock(&m_stateMutex);
    const double clamped = std::clamp(value, 50.0, 150.0);
    if (std::abs(m_dbRange - clamped) < 0.001) {
        return;
    }
    m_dbRange = clamped;
    emit dbRangeChanged();
    invalidateCanvas();
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
    m_freqGridDirty = true;
    m_timeGridDirty = true;
    m_crosshairDirty = true;
    update();
}

bool SpectrogramItem::showFpsOverlay() const {
    return m_showFpsOverlay;
}

bool SpectrogramItem::forceFpsOverlay() const {
    return m_forceFpsOverlay;
}

void SpectrogramItem::setShowFpsOverlay(bool value) {
    const bool next = value;
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_showFpsOverlay == next) {
            return;
        }
        m_showFpsOverlay = next;
        m_overlayDirty = true;
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
    m_freqGridDirty = true;
    m_timeGridDirty = true;
    m_crosshairDirty = true;
    update();
}

int SpectrogramItem::maxColumns() const {
    return m_maxColumns;
}

void SpectrogramItem::setMaxColumns(int value) {
    QMutexLocker lock(&m_stateMutex);
    const int clamped = std::clamp(value, 128, 8192);
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

double SpectrogramItem::positionSeconds() const {
    QMutexLocker lock(&m_stateMutex);
    return m_positionSeconds;
}

void SpectrogramItem::setPositionSeconds(double value) {
    using Clock = std::chrono::steady_clock;
    // Apply gapless offset: translates the new track's GStreamer
    // position (which resets to 0) into the spectrogram's continuous
    // coordinate space.
    const double clamped = std::max(0.0, value + m_gaplessPositionOffset);
    bool changed = false;
    {
        QMutexLocker lock(&m_stateMutex);
        const auto now = Clock::now();
        const double currentPosition = currentRenderPositionSecondsLocked(now);
        const bool largeJump = m_precomputedReady
            && m_playing
            && std::abs(clamped - currentPosition) >= kPositionJumpHoldThresholdSeconds;
        // In centered mode, only hold for gapless transitions (position
        // resets to near zero from deep in the track), not same-track
        // seeks.  Seeks must apply immediately because the full-track
        // ring already has data at the target position.
        // Suppress the exemption briefly after a centered gapless
        // transition so that any stale GStreamer positions still in the
        // old track's coordinate space are held rather than applied.
        const bool recentCenteredGapless =
            m_centeredGaplessTransitionAt.time_since_epoch().count() != 0
            && std::chrono::duration<double>(
                   now - m_centeredGaplessTransitionAt).count() < 2.0;
        const bool centeredSeek = m_displayMode == 1
            && clamped >= 1.0
            && !recentCenteredGapless;
        if (largeJump && !centeredSeek) {
            // Update the target position unconditionally, but only stamp the
            // start time on the *first* activation.  Without this guard, each
            // position heartbeat (~100 ms) during a natural track transition
            // resets the timer, which prevents the 2-second fallback timeout
            // from ever expiring and permanently freezes the display on the
            // old track's spectrogram until the reset data arrives.
            m_positionJumpHoldSeconds = clamped;
            if (!m_positionJumpHoldActive) {
                m_positionJumpHoldActive = true;
                m_positionJumpHoldStartedAt = now;
            }
            return;
        }
        // In rolling mode, ignore small backward heartbeat jitter to avoid
        // scroll stutter.  In centered mode, backward jumps are real seeks
        // that must be applied — the spectrogram stays in the ring buffer.
        const bool regressedDuringPlayback =
            m_displayMode == 0
            && m_playing
            && m_positionAnchorInitialized
            && clamped + kPositionHeartbeatRegressionToleranceSeconds < currentPosition;
        // Soft PLL: for small errors (normal heartbeat jitter ~10-20ms),
        // blend toward the GStreamer position to smooth jitter while
        // still converging to prevent drift.  For large errors (initial
        // position set, post-seek correction), use the value directly.
        const double error = clamped - currentPosition;
        constexpr double kServoAlpha = 0.25;
        constexpr double kServoMaxErrorSeconds = 0.15;
        const bool smallCorrection = std::abs(error) < kServoMaxErrorSeconds;
        const double effectivePosition = regressedDuringPlayback
            ? currentPosition
            : (smallCorrection ? (currentPosition + kServoAlpha * error) : clamped);
        if (m_positionAnchorInitialized
            && std::abs(m_positionSeconds - effectivePosition) < 0.0001) {
            return;
        }
        m_positionJumpHoldActive = false;
        setPositionAnchorLocked(effectivePosition, now);
        changed = true;
    }
    if (changed) {
        emit positionSecondsChanged();
        update();
    }
    // update() wakes the item for anchor changes such as seeks; the
    // steady-state precomputed render loop is still driven by
    // handleWindowAfterAnimating().
}

bool SpectrogramItem::isPlaying() const {
    QMutexLocker lock(&m_stateMutex);
    return m_playing;
}

void SpectrogramItem::setPlaying(bool value) {
    using Clock = std::chrono::steady_clock;
    bool changed = false;
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_playing == value) {
            return;
        }
        syncPositionAnchorLocked(Clock::now());
        m_playing = value;
        if (!m_playing) {
            m_positionJumpHoldActive = false;
        }
        changed = true;
    }
    if (changed) {
        emit playingChanged();
        update();
    }
}

bool SpectrogramItem::precomputedReady() const {
    return m_precomputedReady;
}

int SpectrogramItem::displayMode() const {
    return m_displayMode;
}

void SpectrogramItem::setDisplayMode(int value) {
    const int clamped = std::clamp(value, 0, 1);
    if (m_displayMode == clamped) {
        return;
    }
    m_displayMode = clamped;
    emit displayModeChanged();
    if (m_precomputedReady) {
        m_precomputedLastRightCol = -1;
        invalidateCanvas();
        update();
    }
}

bool SpectrogramItem::crosshairEnabled() const {
    return m_crosshairEnabled;
}

void SpectrogramItem::setCrosshairEnabled(bool value) {
    if (m_crosshairEnabled == value) {
        return;
    }
    {
        QMutexLocker lock(&m_stateMutex);
        m_crosshairEnabled = value;
        m_crosshairDirty = true;
    }
    emit crosshairEnabledChanged();
    update();
}

bool SpectrogramItem::gridEnabled() const {
    return m_gridEnabled;
}

void SpectrogramItem::setGridEnabled(bool value) {
    if (m_gridEnabled == value) {
        return;
    }
    {
        QMutexLocker lock(&m_stateMutex);
        m_gridEnabled = value;
        m_freqGridDirty = true;
        m_timeGridDirty = true;
    }
    emit gridEnabledChanged();
    update();
}

bool SpectrogramItem::showTimeLabels() const {
    return m_showTimeLabels;
}

void SpectrogramItem::setShowTimeLabels(bool value) {
    if (m_showTimeLabels == value) {
        return;
    }
    {
        QMutexLocker lock(&m_stateMutex);
        m_showTimeLabels = value;
        m_timeGridDirty = true;
        m_crosshairDirty = true;
    }
    emit showTimeLabelsChanged();
    update();
}

double SpectrogramItem::crosshairSharedX() const {
    return m_crosshairSharedX;
}

void SpectrogramItem::setCrosshairSharedX(double value) {
    if (qFuzzyCompare(m_crosshairSharedX + 1.0, value + 1.0)) {
        return;
    }
    {
        QMutexLocker lock(&m_stateMutex);
        m_crosshairSharedX = value;
        if (m_crosshairEnabled) {
            m_crosshairDirty = true;
        }
    }
    emit crosshairSharedXChanged();
    if (m_crosshairEnabled) {
        update();
    }
}

bool SpectrogramItem::channelMuted() const {
    return m_channelMuted;
}

void SpectrogramItem::setChannelMuted(bool muted) {
    if (m_channelMuted == muted) {
        return;
    }
    m_channelMuted = muted;
    emit channelMutedChanged();
    {
        QMutexLocker lock(&m_stateMutex);
        markAllTilesDirtyLocked();
        m_canvasDirty = true;
        m_precomputedCanvasDirty = true;
    }
    update();
}

double SpectrogramItem::zoomLevel() const {
    return m_zoomLevel;
}

double SpectrogramItem::effectiveZoomLocked() const {
    if (m_precomputedHopSize <= 0) {
        return m_zoomLevel;
    }
    return m_zoomLevel * static_cast<double>(m_precomputedHopSize)
           / kReferenceHopSamples;
}

double SpectrogramItem::minimumZoomLevelLocked() const {
    const int w = static_cast<int>(width());
    if (w <= 0) {
        return 0.05;
    }
    const qint64 totalCols = m_precomputedMaxColumnIndex >= 0
        ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
        : std::max(static_cast<qint64>(m_precomputedTotalColumnsEstimate),
                   static_cast<qint64>(1));
    if (totalCols <= 0) {
        return 0.05;
    }
    const double minZoom =
        static_cast<double>(w) / static_cast<double>(totalCols);
    return std::max(0.05, minZoom);
}

double SpectrogramItem::minimumZoomLevel() const {
    QMutexLocker lock(&m_stateMutex);
    return minimumZoomLevelLocked();
}

void SpectrogramItem::setZoomLevel(double value) {
    QMutexLocker lock(&m_stateMutex);
    const double minZoom = minimumZoomLevelLocked();
    value = std::clamp(value, minZoom, 16.0);
    if (std::abs(m_zoomLevel - value) < 0.0001) {
        return;
    }
    const double oldZoom = m_zoomLevel;
    m_zoomLevel = value;
    m_precomputedCanvasDirty = true;
    m_crosshairDirty = true;
    m_timeGridDirty = true;
    lock.unlock();
    emit zoomLevelChanged();
    // Only notify backend when zooming above 1.0 or resetting from above 1.0.
    // Zoom-out (within the <=1.0 range) is handled entirely in the frontend
    // renderer via decimation — no backend session restart needed.
    if (m_zoomLevel > 1.001 || std::abs(oldZoom - 1.0) > 0.001) {
        m_pendingBackendZoom = static_cast<float>(m_zoomLevel);
        m_zoomDebounceTimer->start(); // restarts the 150ms timer
    }
    update();
}

bool SpectrogramItem::zoomEnabled() const {
    return m_zoomEnabled;
}

void SpectrogramItem::setZoomEnabled(bool value) {
    if (m_zoomEnabled == value) {
        return;
    }
    m_zoomEnabled = value;
    emit zoomEnabledChanged();
    // Reset zoom to 1.0 when disabling so the user isn't stuck
    // at a non-default zoom with no way to reset.
    if (!value && std::abs(m_zoomLevel - 1.0) > 0.001) {
        emit zoomResetRequested();
    }
}

void SpectrogramItem::feedPrecomputedChunk(
    const QByteArray &data, int bins, int channelIndex,
    int columns, int startIndex, int totalEstimate,
    int sampleRate, int hopSize, bool /*complete*/,
    bool bufferReset, quint64 trackToken, bool clearHistoryOnReset) {
    using Clock = std::chrono::steady_clock;
    QMutexLocker lock(&m_stateMutex);

    FERROUS_SPECTROGRAM_LOGF(stderr,
        "[Qt-feed] chIdx=%d cols=%d start=%d total=%d bins=%d sr=%d hop=%d tok=%llu ready=%d reset=%d clear=%d\n",
        channelIndex, columns, startIndex, totalEstimate, bins,
        sampleRate, hopSize, static_cast<unsigned long long>(trackToken),
        m_precomputedReady ? 1 : 0, bufferReset ? 1 : 0,
        clearHistoryOnReset ? 1 : 0);

    if (totalEstimate <= 0 || bins <= 0) {
        return;
    }

    // Record the committed token on buffer_reset so we can drop stale
    // chunks from a superseded session that arrive out of order.
    if (bufferReset && trackToken != 0) {
        m_precomputedCommittedToken = trackToken;
    }
    // Drop chunks from superseded sessions: tokens older than the most
    // recent reset are stale (from a rapid double track change).  Tokens
    // newer than committed are allowed — they come from gapless
    // transitions which advance the token without a buffer_reset.
    if (m_precomputedCommittedToken != 0
        && trackToken != 0
        && trackToken < m_precomputedCommittedToken) {
        return;
    }

    // Determine the number of channels from the packed data size.
    // For metadata-only chunks (columns==0, e.g. buffer_reset), channelCount
    // is unknown from data alone, so we allow any channelIndex through for
    // the epoch/reset handling below, and guard only the data-writing section.
    const int totalDataSize = data.size();
    const int channelCount = (columns > 0 && bins > 0)
        ? std::max(1, totalDataSize / (columns * bins))
        : 0;  // 0 = unknown, don't gate on it

    if (channelCount > 0 && (channelIndex < 0 || channelIndex >= channelCount)) {
        return;
    }

    // If this widget has no ring and receives data (not a reset), it was
    // created/recycled after the reset went to a different widget instance.
    // Apply an implicit reset so the widget can accept the arriving data.
    bool appliedImplicitReset = false;
    if (!bufferReset && columns > 0 && m_ringCapacity == 0 && !m_precomputedResetPending) {
        applyPrecomputedResetLocked(startIndex, bins, trackToken, true);
        appliedImplicitReset = true;
    }

    bool appliedReset = false;
    if (bufferReset && columns <= 0) {
        // Delay the reset handoff until the first data-bearing post-seek
        // chunk arrives. Resetting on the metadata-only frame makes the
        // item repaint against an empty/new timeline before there is any
        // matching data, which shows up as a blank flash on backward seeks.
        m_precomputedResetPending = true;
        m_precomputedPendingResetStartIndex = startIndex;
        m_precomputedPendingResetBins = bins;
        m_precomputedPendingResetTrackToken = trackToken;
        m_precomputedPendingResetClearHistory = clearHistoryOnReset;
    } else if (bufferReset) {
        applyPrecomputedResetLocked(
            startIndex, bins, trackToken, clearHistoryOnReset);
        m_precomputedResetPending = false;
        m_precomputedPendingResetStartIndex = 0;
        m_precomputedPendingResetBins = 0;
        m_precomputedPendingResetTrackToken = 0;
        m_precomputedPendingResetClearHistory = false;
        appliedReset = true;
    } else if (m_precomputedResetPending
        && columns > 0
        && bins == m_precomputedPendingResetBins
        && (m_precomputedPendingResetTrackToken == 0
            || trackToken == 0
            || trackToken == m_precomputedPendingResetTrackToken)) {
        FERROUS_SPECTROGRAM_LOGF(stderr,
            "[Qt-deferred-reset] pendingTok=%llu dataTok=%llu clear=%d writeSeqBefore=%lld\n",
            static_cast<unsigned long long>(m_precomputedPendingResetTrackToken),
            static_cast<unsigned long long>(trackToken),
            m_precomputedPendingResetClearHistory ? 1 : 0,
            static_cast<long long>(m_ringWriteSeq));
        applyPrecomputedResetLocked(
            m_precomputedPendingResetStartIndex,
            m_precomputedPendingResetBins,
            m_precomputedPendingResetTrackToken != 0
                ? m_precomputedPendingResetTrackToken
                : trackToken,
            m_precomputedPendingResetClearHistory);
        m_precomputedResetPending = false;
        m_precomputedPendingResetStartIndex = 0;
        m_precomputedPendingResetBins = 0;
        m_precomputedPendingResetTrackToken = 0;
        m_precomputedPendingResetClearHistory = false;
        appliedReset = true;
    }

    if (trackToken != 0
        && m_precomputedTrackToken != 0
        && trackToken != m_precomputedTrackToken
        && !bufferReset
        && !appliedReset
        && !appliedImplicitReset) {
        // New track token — reset per-track max column index so it
        // reflects the new track's range, not the old one's.
        m_precomputedMaxColumnIndex = -1;

        // Gapless rolling transition: accumulate a position offset so
        // the GStreamer position (which resets to 0) gets translated
        // into the spectrogram's continuous coordinate space.  This
        // makes setPositionSeconds(0.0) arrive as setPosition(331.18)
        // — no large jump, no hold, no epoch remap, no phase
        // discontinuity.  The position model and ring buffer stay
        // perfectly continuous.
        if (m_displayMode == 0) {
            const auto now = Clock::now();
            m_gaplessPositionOffset =
                currentRenderPositionSecondsLocked(now);
            // Clear any jump hold that was activated by a
            // setPositionSeconds(0.0) arriving before the offset was
            // set.  Without this, the hold expires after 2 seconds
            // and snaps the anchor to 0.0 in the wrong coordinate
            // space, freezing the display.
            if (m_positionJumpHoldActive) {
                m_positionJumpHoldActive = false;
            }
        } else {
            // Centered mode: the new track uses 0-based position
            // coordinates.  Snap the anchor to 0 immediately so the
            // display jumps to column 0 instead of lingering at the
            // old track's position until GStreamer reports >= 1 s.
            const auto now = Clock::now();
            setPositionAnchorLocked(0.0, now);
            m_positionJumpHoldActive = false;
            m_centeredGaplessTransitionAt = now;
        }
        m_precomputedLastRightCol = -1;
        m_precomputedLastDisplaySeq = -1;
        m_timeGridDirty = true;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        m_debugLastTransitionFeedAt = Clock::now();
        {
            const qint64 validCount = std::max<qint64>(0, m_ringWriteSeq - m_ringOldestSeq);
            FERROUS_SPECTROGRAM_LOGF(
                stderr,
                "[Qt-gapless-transition] chIdx=%d oldTok=%llu newTok=%llu startIdx=%d "
                "ringWriteSeq=%lld epoch=%lld anchor=%.3f offset=%.3f "
                "ringValid=%lld/%d totalEst=%d\n",
                channelIndex,
                static_cast<unsigned long long>(m_precomputedTrackToken),
                static_cast<unsigned long long>(trackToken),
                startIndex,
                static_cast<long long>(m_ringWriteSeq),
                static_cast<long long>(m_rollingEpoch),
                m_positionAnchorSeconds,
                m_gaplessPositionOffset,
                static_cast<long long>(validCount), m_ringCapacity,
                m_precomputedTotalColumnsEstimate);
        }
#endif
    }

    if (trackToken != 0 && !(bufferReset && columns <= 0)) {
        m_precomputedTrackToken = trackToken;
    }

    m_precomputedBinsPerColumn = bins;
    m_precomputedTotalColumnsEstimate = totalEstimate;

    // Only update rate/hop from chunks that carry actual column data.
    if (columns > 0) {
        if (sampleRate > 0) {
            m_precomputedSampleRateHz = sampleRate;
        }
        if (hopSize > 0) {
            m_precomputedHopSize = hopSize;
        }
        if (appliedReset && m_precomputedSampleRateHz > 0 && m_precomputedHopSize > 0) {
            const double seekPositionSeconds =
                static_cast<double>(startIndex * m_precomputedHopSize)
                / static_cast<double>(m_precomputedSampleRateHz);
            m_positionJumpHoldActive = false;
            setPositionAnchorLocked(seekPositionSeconds, Clock::now());
        }
    }

    // Dynamically size the ring buffer based on widget width and display mode.
    if (columns > 0 && bins > 0) {
        const double colsPerSecond =
            static_cast<double>(m_precomputedSampleRateHz)
            / static_cast<double>(m_precomputedHopSize);
        const int screenWidth = std::max(static_cast<int>(width()), 1920);
        const int extraSeconds = 10;
        int neededCapacity;
        if (m_displayMode == 1) {
            // Centered: size to fit the entire track so the full
            // spectrogram is available for seeking and display.
            neededCapacity = std::max(
                static_cast<int>(m_precomputedTotalColumnsEstimate) + 256,
                screenWidth + screenWidth / 2
                    + static_cast<int>(extraSeconds * colsPerSecond));
        } else {
            // Rolling: need screen width / effectiveZoom of history + lookahead.
            const double ez = effectiveZoomLocked();
            const int zoomAdjustedWidth = static_cast<int>(
                static_cast<double>(screenWidth) / std::max(0.05, ez));
            neededCapacity = zoomAdjustedWidth
                + static_cast<int>(extraSeconds * colsPerSecond);
        }
        // Add some margin.
        neededCapacity = std::max(neededCapacity, 1024);

        // Check if we need to (re)allocate: either more columns, or
        // bins_per_column changed (e.g. FFT size switch).
        const qint64 neededBytes =
            static_cast<qint64>(neededCapacity) * bins;
        const bool mustRealloc =
            neededBytes > m_ringBuffer.size()
            || neededCapacity > m_ringCapacity;

        if (mustRealloc) {
            QByteArray newBuf(static_cast<int>(neededBytes), '\0');
            std::vector<qint32> newColId(
                static_cast<size_t>(neededCapacity), -1);
            std::vector<qint64> newSeqId(
                static_cast<size_t>(neededCapacity), -1);
            std::vector<quint64> newTrackToken(
                static_cast<size_t>(neededCapacity), 0);
            QHash<quint64, QHash<qint32, qint64>> newTrackColumnToSeqByToken;

            // Copy existing valid columns by write sequence so rolling
            // history stays contiguous after growth.
            const bool binsMatch =
                m_ringCapacity > 0
                && m_ringBuffer.size()
                    == static_cast<qint64>(m_ringCapacity) * bins;
            if (binsMatch
                && !m_ringColumnId.empty()
                && !m_ringSequenceId.empty()
                && !m_ringTrackToken.empty()) {
                for (qint64 seq = m_ringOldestSeq; seq < m_ringWriteSeq; ++seq) {
                    const int oldSlot = static_cast<int>(seq % m_ringCapacity);
                    if (oldSlot < 0 || oldSlot >= m_ringCapacity) {
                        continue;
                    }
                    if (m_ringSequenceId[static_cast<size_t>(oldSlot)] != seq) {
                        continue;
                    }
                    const qint32 trackCol = m_ringColumnId[static_cast<size_t>(oldSlot)];
                    const quint64 token = m_ringTrackToken[static_cast<size_t>(oldSlot)];
                    if (trackCol < 0) {
                        continue;
                    }
                    const int newSlot = static_cast<int>(seq % neededCapacity);
                    memcpy(newBuf.data() + newSlot * bins,
                        m_ringBuffer.constData() + oldSlot * bins,
                        static_cast<size_t>(bins));
                    newColId[static_cast<size_t>(newSlot)] = trackCol;
                    newSeqId[static_cast<size_t>(newSlot)] = seq;
                    newTrackToken[static_cast<size_t>(newSlot)] = token;
                    newTrackColumnToSeqByToken[token].insert(trackCol, seq);
                }
            }
            m_ringBuffer = std::move(newBuf);
            m_ringColumnId = std::move(newColId);
            m_ringSequenceId = std::move(newSeqId);
            m_ringTrackToken = std::move(newTrackToken);
            m_trackColumnToSeqByToken = std::move(newTrackColumnToSeqByToken);
            m_ringCapacity = neededCapacity;
        }
    }

    // Write columns into the ring buffer.
    if (columns > 0 && m_ringCapacity > 0) {
        const int stridePerColumn = channelCount * bins;
        const int channelOffset = channelIndex * bins;
        const auto *srcData =
            reinterpret_cast<const char *>(data.constData());

        for (int col = 0; col < columns; ++col) {
            const int srcOff = col * stridePerColumn + channelOffset;
            if (srcOff + bins > totalDataSize) {
                break;
            }
            const qint32 trackCol = static_cast<qint32>(startIndex + col);
            if (trackCol < 0) {
                continue;
            }
            const int slot =
                static_cast<int>(m_ringWriteSeq % m_ringCapacity);
            if (!m_ringSequenceId.empty()) {
                const qint64 previousSeq = m_ringSequenceId[static_cast<size_t>(slot)];
                if (previousSeq >= m_ringOldestSeq && previousSeq < m_ringWriteSeq) {
                    const qint32 previousTrackCol = m_ringColumnId[static_cast<size_t>(slot)];
                    const quint64 previousToken = !m_ringTrackToken.empty()
                        ? m_ringTrackToken[static_cast<size_t>(slot)]
                        : 0;
                    if (previousTrackCol >= 0) {
                        auto tokenIt = m_trackColumnToSeqByToken.find(previousToken);
                        if (tokenIt != m_trackColumnToSeqByToken.end()
                            && tokenIt->value(previousTrackCol, -1) == previousSeq) {
                            tokenIt->remove(previousTrackCol);
                            if (tokenIt->isEmpty()) {
                                m_trackColumnToSeqByToken.erase(tokenIt);
                            }
                        }
                    }
                }
            }
            const quint64 effectiveTrackToken =
                trackToken != 0 ? trackToken : m_precomputedTrackToken;
            memcpy(m_ringBuffer.data() + slot * bins,
                   srcData + srcOff,
                   static_cast<size_t>(bins));
            m_ringColumnId[static_cast<size_t>(slot)] = trackCol;
            if (!m_ringSequenceId.empty()) {
                m_ringSequenceId[static_cast<size_t>(slot)] = m_ringWriteSeq;
            }
            if (!m_ringTrackToken.empty()) {
                m_ringTrackToken[static_cast<size_t>(slot)] = effectiveTrackToken;
            }
            m_trackColumnToSeqByToken[effectiveTrackToken].insert(trackCol, m_ringWriteSeq);
            if (trackCol > m_precomputedMaxColumnIndex) {
                m_precomputedMaxColumnIndex = trackCol;
            }
            m_ringWriteSeq++;
        }
        m_ringOldestSeq = std::max<qint64>(0, m_ringWriteSeq - m_ringCapacity);
        FERROUS_SPECTROGRAM_LOGF(stderr,
            "[Qt-ring] tok=%llu writeSeq=%lld oldestSeq=%lld cap=%d valid=%lld maxCol=%d\n",
            static_cast<unsigned long long>(trackToken),
            static_cast<long long>(m_ringWriteSeq),
            static_cast<long long>(m_ringOldestSeq),
            m_ringCapacity,
            static_cast<long long>(m_ringWriteSeq - m_ringOldestSeq),
            static_cast<int>(m_precomputedMaxColumnIndex));
    }

    const bool wasReady = m_precomputedReady;
    m_precomputedReady =
        m_precomputedTotalColumnsEstimate > 0
        && m_precomputedBinsPerColumn > 0;

    m_precomputedLastRightCol = -1;

    // In centered mode, only mark the canvas dirty when the new columns
    // fall within the currently displayed range.  When the worker is
    // decoding far ahead of the playhead (full-speed centered decode),
    // those columns are not yet visible and repaint would cause twitching
    // from continuous full-canvas rebuilds at frame rate.
    if (m_displayMode == 1
        && m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft) {
        const qint32 chunkEnd = static_cast<qint32>(startIndex) + columns - 1;
        const bool overlapsVisible =
            static_cast<qint32>(startIndex) <= static_cast<qint32>(m_precomputedCanvasDisplayRight)
            && chunkEnd >= static_cast<qint32>(m_precomputedCanvasDisplayLeft);
        if (overlapsVisible) {
            m_precomputedCanvasDirty = true;
        }
    } else {
        m_precomputedCanvasDirty = true;
    }

    if (m_precomputedReady && !wasReady) {
        emit precomputedReadyChanged();
    }
    update();
}

void SpectrogramItem::clearPrecomputed() {
    FERROUS_SPECTROGRAM_LOGF(stderr, "[Qt-clearPrecomputed] ringCap=%d ready=%d\n",
        m_ringCapacity, m_precomputedReady ? 1 : 0);
    QMutexLocker lock(&m_stateMutex);
    m_ringBuffer.clear();
    m_ringColumnId.clear();
    m_ringSequenceId.clear();
    m_ringTrackToken.clear();
    m_trackColumnToSeqByToken.clear();
    m_ringCapacity = 0;
    m_ringOldestSeq = 0;
    m_ringWriteSeq = 0;
    m_trackEpochSeq = 0;
    m_rollingEpoch = 0;
    m_precomputedMaxColumnIndex = -1;
    m_precomputedCommittedToken = 0;
    m_precomputedBinsPerColumn = 0;
    m_precomputedTotalColumnsEstimate = 0;
    m_precomputedResetPending = false;
    m_precomputedPendingResetStartIndex = 0;
    m_precomputedPendingResetBins = 0;
    m_precomputedPendingResetTrackToken = 0;
    m_precomputedPendingResetClearHistory = false;
    m_precomputedLastRightCol = -1;
    m_precomputedLastDisplaySeq = -1;
    m_precomputedTrackToken = 0;
    m_gaplessPositionOffset = 0.0;
    m_positionJumpHoldActive = false;
    m_centeredGaplessTransitionAt = {};
    m_timeGridDirty = true;
    const bool wasReady = m_precomputedReady;
    m_precomputedReady = false;
    invalidateCanvas();
    if (wasReady) {
        emit precomputedReadyChanged();
    }
    update();
}

void SpectrogramItem::applyPrecomputedResetLocked(
    int startIndex,
    int bins,
    quint64 trackToken,
    bool clearHistoryOnReset) {
    Q_UNUSED(trackToken);
    // A reset (seek or manual track change) breaks the continuous
    // gapless coordinate space — clear the accumulated offset,
    // per-track column tracking, and canvas display range so the
    // centered-mode monotonic clamp doesn't stick at old positions.
    m_gaplessPositionOffset = 0.0;
    m_centeredGaplessTransitionAt = {};
    m_precomputedMaxColumnIndex = -1;
    m_precomputedCanvasDisplayLeft = 0;
    m_precomputedCanvasDisplayRight = -1;
    const bool preserveRollingHistory =
        !clearHistoryOnReset &&
        m_displayMode == 0
        && m_ringCapacity > 0
        && m_precomputedBinsPerColumn > 0
        && bins == m_precomputedBinsPerColumn;
    if (!preserveRollingHistory) {
        m_ringOldestSeq = 0;
        m_ringWriteSeq = 0;
        m_trackEpochSeq = 0;
        m_rollingEpoch = 0;
        m_ringBuffer.clear();
        m_ringColumnId.clear();
        m_ringSequenceId.clear();
        m_ringTrackToken.clear();
        m_trackColumnToSeqByToken.clear();
        m_ringCapacity = 0;
    } else {
        // Preserve only the history that has actually been shown on screen.
        // Any speculative lookahead beyond the current playback head is stale
        // after a seek/non-gapless reset; keeping it and remapping to the
        // write head makes the spectrogram jump forward and "speed up".
        const auto now = std::chrono::steady_clock::now();
        const qint64 displayRight = currentRollingDisplayRightLocked(now);
        truncateRollingTailLocked(displayRight + 1);
        m_rollingEpoch = displayRight - static_cast<qint64>(startIndex);
    }
    m_precomputedLastRightCol = -1;
    m_precomputedLastDisplaySeq = -1;
    m_timeGridDirty = true;
    invalidateCanvas();
}

qint64 SpectrogramItem::currentRollingDisplayRightLocked(
    std::chrono::steady_clock::time_point now) const {
    if (m_ringWriteSeq <= 0) {
        return 0;
    }
    if (m_precomputedSampleRateHz <= 0 || m_precomputedHopSize <= 0) {
        return std::clamp(m_ringWriteSeq - 1, m_ringOldestSeq, m_ringWriteSeq - 1);
    }
    const double colsPerSecond =
        static_cast<double>(m_precomputedSampleRateHz)
        / static_cast<double>(m_precomputedHopSize);
    const double renderPositionSeconds =
        std::max(0.0, currentRenderPositionSecondsLocked(now));
    const qint64 nowCol = static_cast<qint64>(std::floor(renderPositionSeconds * colsPerSecond));
    const qint64 displaySeq = m_rollingEpoch + std::max<qint64>(nowCol, 0);
    return std::clamp(displaySeq, m_ringOldestSeq, m_ringWriteSeq - 1);
}

void SpectrogramItem::truncateRollingTailLocked(qint64 newWriteSeq) {
    if (m_ringCapacity <= 0) {
        m_ringWriteSeq = 0;
        m_ringOldestSeq = 0;
        return;
    }

    const qint64 clampedWriteSeq = std::clamp(newWriteSeq, m_ringOldestSeq, m_ringWriteSeq);
    if (clampedWriteSeq >= m_ringWriteSeq) {
        return;
    }

    for (qint64 seq = clampedWriteSeq; seq < m_ringWriteSeq; ++seq) {
        const int slot = static_cast<int>(seq % m_ringCapacity);
        if (slot < 0 || slot >= m_ringCapacity) {
            continue;
        }
        if (!m_ringSequenceId.empty()
            && m_ringSequenceId[static_cast<size_t>(slot)] == seq) {
            const qint32 trackCol = !m_ringColumnId.empty()
                ? m_ringColumnId[static_cast<size_t>(slot)]
                : -1;
            const quint64 trackToken = !m_ringTrackToken.empty()
                ? m_ringTrackToken[static_cast<size_t>(slot)]
                : 0;
            if (trackCol >= 0) {
                auto tokenIt = m_trackColumnToSeqByToken.find(trackToken);
                if (tokenIt != m_trackColumnToSeqByToken.end()
                    && tokenIt->value(trackCol, -1) == seq) {
                    tokenIt->remove(trackCol);
                    if (tokenIt->isEmpty()) {
                        m_trackColumnToSeqByToken.erase(tokenIt);
                    }
                }
            }
        }
        if (!m_ringColumnId.empty()) {
            m_ringColumnId[static_cast<size_t>(slot)] = -1;
        }
        if (!m_ringSequenceId.empty()) {
            m_ringSequenceId[static_cast<size_t>(slot)] = -1;
        }
        if (!m_ringTrackToken.empty()) {
            m_ringTrackToken[static_cast<size_t>(slot)] = 0;
        }
    }

    m_ringWriteSeq = clampedWriteSeq;
    if (m_ringOldestSeq > m_ringWriteSeq) {
        m_ringOldestSeq = m_ringWriteSeq;
    }
}

double SpectrogramItem::currentRenderPositionSecondsLocked(
    std::chrono::steady_clock::time_point now) const {
    const double anchor = m_positionAnchorInitialized
        ? m_positionAnchorSeconds
        : m_positionSeconds;
    if (!m_playing || !m_positionAnchorInitialized) {
        return std::max(0.0, anchor);
    }
    const double elapsedSeconds =
        std::chrono::duration<double>(now - m_positionAnchorUpdatedAt).count();
    return std::max(0.0, anchor + std::max(0.0, elapsedSeconds));
}

void SpectrogramItem::setPositionAnchorLocked(
    double value,
    std::chrono::steady_clock::time_point now) {
    const double clamped = std::max(0.0, value);
    m_positionSeconds = clamped;
    m_positionAnchorSeconds = clamped;
    m_positionAnchorUpdatedAt = now;
    m_positionAnchorInitialized = true;
}

void SpectrogramItem::syncPositionAnchorLocked(
    std::chrono::steady_clock::time_point now) {
    setPositionAnchorLocked(currentRenderPositionSecondsLocked(now), now);
}

void SpectrogramItem::reset() {
    QMutexLocker lock(&m_stateMutex);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    resetSmoothnessProfileLocked();
    resetSeekProfileLocked();
    m_lastIncomingRowsAtMs = 0;
#endif
    m_columns.clear();
    m_seedHistoryOnNextAppend = true;
    m_animationTickInitialized = false;

    // When precomputed mode is active, don't destroy the canvas or
    // mapping — the canvas will be rebuilt from the atlas on the next
    // frame.  Destroying it causes a visible gap/flash.
    if (!m_precomputedReady) {
        m_binsPerColumn = 0;
        invalidateMapping();
        invalidateCanvas();
    }
    update();
}

void SpectrogramItem::halt() {
    QMutexLocker lock(&m_stateMutex);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    resetSmoothnessProfileLocked();
    resetSeekProfileLocked();
    m_lastIncomingRowsAtMs = 0;
#endif
    m_animationTickInitialized = false;
    update();
}

void SpectrogramItem::appendRows(const QVariantList &rows) {
    QMutexLocker lock(&m_stateMutex);
    if (rows.isEmpty()) {
        return;
    }

    const int w = std::max(1, static_cast<int>(std::floor(width())));
    const int h = std::max(1, static_cast<int>(std::floor(height())));
    ensureCanvas(w, h);
    if (!m_canvas.isNull() && m_canvasDirty) {
        rebuildCanvasFromColumns();
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
        m_columns.emplace_back(std::move(mapped));
        while (static_cast<int>(m_columns.size()) > m_maxColumns) {
            m_columns.pop_front();
        }
        if (!m_canvas.isNull()) {
            drawColumnAt(m_canvasWriteX, m_columns.back());
            m_canvasWriteX = (m_canvasWriteX + 1) % m_canvas.width();
            m_canvasFilledCols = std::min(m_canvas.width(), m_canvasFilledCols + 1);
        }
        rowsAdded++;
    }

    if (rowsAdded <= 0) {
        return;
    }
    m_seedHistoryOnNextAppend = false;
    lock.unlock();
    update();
}

QSGNode *SpectrogramItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const auto paintStart = std::chrono::steady_clock::now();
#endif
    const auto renderNow = std::chrono::steady_clock::now();
    auto *node = static_cast<SpectrogramSceneNode *>(oldNode);
    QQuickWindow *currentWindow = window();
    const quintptr windowId = reinterpret_cast<quintptr>(currentWindow);
    if (node != nullptr
        && (node->ownerWindowId != windowId || node->generation != m_sceneGraphGeneration)) {
        delete node;
        node = nullptr;
    }
    if (node == nullptr) {
        node = new SpectrogramSceneNode();
        node->ownerWindowId = windowId;
        node->generation = m_sceneGraphGeneration;
    }
    if (node->placeholderTexture == nullptr && currentWindow != nullptr) {
        node->placeholderTexture = currentWindow->createTextureFromImage(placeholderImage());
        if (node->placeholderTexture != nullptr) {
            node->placeholderTexture->setFiltering(QSGTexture::Nearest);
        }
    }

    const int w = std::max(1, static_cast<int>(std::floor(width())));
    const int h = std::max(1, static_cast<int>(std::floor(height())));
    node->background->setRect(0.0, 0.0, static_cast<double>(w), static_cast<double>(h));
    node->background->setColor(kBackgroundColor);

    bool hasCanvas = false;
    QSize canvasSize;
    int drawCols = 0;
    int srcStart = 0;
    int latestX = 0;
    double scrollOffset = 0.0;
    double drawX = 0.0;
    bool showPlayhead = false;
    QRectF playheadRect;
    QVector<QImage> tileImages;
    QVector<int> tileDirtyIndexes;
    int tileCount = 0;
    bool showOverlay = false;
    bool overlayChanged = false;
    QImage overlayImage;
    QSize overlaySize;

    bool showFreqGridOverlay = false;
    bool freqGridOverlayChanged = false;
    QImage freqGridOverlayImage;
    QSize freqGridOverlaySize;

    bool showTimeGridOverlay = false;
    bool timeGridOverlayChanged = false;
    QImage timeGridOverlayImage;
    int timeGridSourceShift = 0;
    int timeGridImageHeight = 0;

    bool showCrosshairOverlay = false;
    bool crosshairOverlayChanged = false;
    QImage crosshairOverlayImage;
    QSize crosshairOverlaySize;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    size_t profilePendingColumns = 0;
    size_t profileColumnCount = 0;
    int profileBinsPerColumn = 0;
#endif

    {
        QMutexLocker lock(&m_stateMutex);
        const double renderPositionSeconds = currentRenderPositionSecondsLocked(renderNow);
        const bool usePrecomputed = m_precomputedReady
            && m_precomputedBinsPerColumn > 0
            && m_precomputedTotalColumnsEstimate > 0;

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        // Detect significant position jumps between paint frames — the
        // exact moment a gapless transition becomes visible to the user.
        {
            const double posDelta = renderPositionSeconds - m_debugPrevRenderPos;
            const bool tokenChanged = m_precomputedTrackToken != m_debugPrevTrackToken
                && m_debugPrevTrackToken != 0;
            if (tokenChanged || posDelta < -1.0 || posDelta > 2.0) {
                const qint64 validCount = std::max<qint64>(0, m_ringWriteSeq - m_ringOldestSeq);
                const auto msSinceTransitionFeed =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        renderNow - m_debugLastTransitionFeedAt).count();
                FERROUS_SPECTROGRAM_LOGF(
                    stderr,
                    "[Qt-transition-frame@%p] prevPos=%.3f pos=%.3f delta=%.3f "
                    "prevTok=%llu tok=%llu ringValid=%lld/%d totalEst=%d "
                    "anchor=%.3f usePre=%d usSinceFeed=%lld\n",
                    static_cast<const void *>(this),
                    m_debugPrevRenderPos, renderPositionSeconds, posDelta,
                    static_cast<unsigned long long>(m_debugPrevTrackToken),
                    static_cast<unsigned long long>(m_precomputedTrackToken),
                    static_cast<long long>(validCount), m_ringCapacity,
                    m_precomputedTotalColumnsEstimate,
                    m_positionAnchorSeconds,
                    usePrecomputed ? 1 : 0,
                    static_cast<long long>(msSinceTransitionFeed));
            }
            m_debugPrevRenderPos = renderPositionSeconds;
            m_debugPrevTrackToken = m_precomputedTrackToken;
        }
#endif

        // Debug: log precomputed state periodically (per-instance).
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        {
            m_debugPaintCounter++;
            if (m_debugPaintCounter % 120 == 1) {
                const qint64 validCount = std::max<qint64>(0, m_ringWriteSeq - m_ringOldestSeq);
                FERROUS_SPECTROGRAM_LOGF(
                    stderr,
                    "[Qt-paint@%p] usePre=%d ready=%d bins=%d totalEst=%d pos=%.2f sr=%d hop=%d ringValid=%lld/%d streaming=%d\n",
                    static_cast<const void *>(this),
                    usePrecomputed ? 1 : 0,
                    m_precomputedReady ? 1 : 0,
                    m_precomputedBinsPerColumn,
                    m_precomputedTotalColumnsEstimate,
                    renderPositionSeconds,
                    m_precomputedSampleRateHz,
                    m_precomputedHopSize,
                    static_cast<long long>(validCount),
                    m_ringCapacity,
                    static_cast<int>(m_columns.size()));
            }
        }
#endif

        if (usePrecomputed) {
            bool needInvalidateMapping = false;
            if (m_binsPerColumn != m_precomputedBinsPerColumn) {
                m_binsPerColumn = m_precomputedBinsPerColumn;
                needInvalidateMapping = true;
            }
            if (m_precomputedSampleRateHz > 0
                && m_sampleRateHz != m_precomputedSampleRateHz) {
                m_sampleRateHz = m_precomputedSampleRateHz;
                needInvalidateMapping = true;
                m_freqGridDirty = true;
                m_crosshairDirty = true;
            }
            if (needInvalidateMapping) {
                invalidateMapping();
            }
            ensureMapping(h);
            const double columnsPerSecond =
                static_cast<double>(m_precomputedSampleRateHz) / static_cast<double>(m_precomputedHopSize);
            const double effectiveZoom = effectiveZoomLocked();
            const double clampedPositionSeconds =
                std::max(0.0, renderPositionSeconds);
            double columnF = clampedPositionSeconds * columnsPerSecond;
            const int nowCol = static_cast<int>(std::floor(columnF));
            double columnPhase = std::clamp(columnF - std::floor(columnF), 0.0, 0.999);

            qint64 displayLeft, displayRight;
            int playheadPixel;
            bool rollingMode;
            qint64 writeHeadSeq = -1;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            qint64 unclampedDisplaySeq = 0;
            qint64 writeHeadHeadroom = 0;
            bool writeHeadClamped = false;
#endif

            if (m_displayMode == 1) {
                rollingMode = false;
                const int visibleWindowCols = static_cast<int>(
                    std::ceil(static_cast<double>(w) / effectiveZoom));
                const int halfWindowCols = visibleWindowCols / 2;
                const qint64 totalCols = m_precomputedMaxColumnIndex >= 0
                    ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
                    : std::max(static_cast<qint64>(m_precomputedTotalColumnsEstimate),
                               static_cast<qint64>(1));
                displayLeft = std::max(static_cast<qint64>(0),
                    static_cast<qint64>(nowCol) - halfWindowCols);
                displayRight = std::min(
                    totalCols - 1,
                    displayLeft + static_cast<qint64>(visibleWindowCols) - 1);
                displayLeft = std::max<qint64>(
                    0, displayRight - static_cast<qint64>(visibleWindowCols) + 1);

                // Jitter prevention: only apply when zoom hasn't changed
                const bool isSeekJump =
                    m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft
                    && displayLeft < m_precomputedCanvasDisplayLeft - 2;
                if (!isSeekJump
                    && m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft
                    && std::abs(effectiveZoom - m_precomputedCanvasZoomLevel) < 0.001) {
                    displayLeft = std::max(displayLeft, m_precomputedCanvasDisplayLeft);
                    displayRight = std::max(displayRight, m_precomputedCanvasDisplayRight);
                }

                playheadPixel = static_cast<int>(std::round(
                    static_cast<double>(nowCol - displayLeft) * effectiveZoom));
            } else {
                rollingMode = true;
                const int visibleWindowCols = static_cast<int>(
                    std::ceil(static_cast<double>(w) / effectiveZoom));
                const qint64 displaySeq =
                    m_rollingEpoch + static_cast<qint64>(std::max(nowCol, 0));
                writeHeadSeq = m_ringWriteSeq - 1;
                displayRight = std::min(displaySeq, writeHeadSeq);
                displayLeft = std::max(m_ringOldestSeq,
                    displayRight - static_cast<qint64>(visibleWindowCols) + 1);
                playheadPixel = -1;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                unclampedDisplaySeq = displaySeq;
                writeHeadHeadroom = writeHeadSeq - displaySeq;
                writeHeadClamped = displaySeq > writeHeadSeq;
#endif
            }

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            if (m_profileEnabled
                && rollingMode
                && usePrecomputed
                && (writeHeadClamped || writeHeadHeadroom <= 8)
                && shouldLogProfileSpike(&m_profileLastWriteHeadClampSpike, renderNow, 0.10)) {
                const auto usSinceTransitionFeed =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        renderNow - m_debugLastTransitionFeedAt).count();
                FERROUS_SPECTROGRAM_LOGF(
                    stderr,
                    "[Qt-write-head@%p] pos=%.3f unclamped=%lld display=%lld write=%lld "
                    "headroom=%lld tok=%llu usSinceFeed=%lld\n",
                    static_cast<const void *>(this),
                    renderPositionSeconds,
                    static_cast<long long>(unclampedDisplaySeq),
                    static_cast<long long>(displayRight),
                    static_cast<long long>(writeHeadSeq),
                    static_cast<long long>(writeHeadHeadroom),
                    static_cast<unsigned long long>(m_precomputedTrackToken),
                    static_cast<long long>(usSinceTransitionFeed));
            }
#endif

            // The decoder covers the full track minus ~2 columns at
            // EOF (STFT window effect).  Don't clamp the scroll — at
            // most 1-2 background pixels appear at the right edge for
            // a few frames, which is imperceptible.  Clamping caused a
            // visible backward-snap stutter when the playback position
            // intermittently crossed the ring write head.

            const int visibleCols = std::min(
                w,
                static_cast<int>(std::max<qint64>(0, displayRight - displayLeft + 1)));

            const bool hasCanvasRange =
                m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft;
            const bool rangeChanged =
                !hasCanvasRange
                || displayLeft != m_precomputedCanvasDisplayLeft
                || (visibleCols > 0
                    ? displayLeft + static_cast<qint64>(visibleCols) - 1
                    : displayLeft - 1) != m_precomputedCanvasDisplayRight
                || rollingMode != m_precomputedCanvasRolling;
            const bool needsFullRebuild =
                visibleCols > 0
                && (m_canvas.isNull()
                    || m_canvas.width() != w
                    || m_canvas.height() != h
                    || rollingMode != m_precomputedCanvasRolling
                    || !hasCanvasRange
                    || displayLeft < m_precomputedCanvasDisplayLeft
                    || displayRight < m_precomputedCanvasDisplayRight
                    || std::abs(effectiveZoom - m_precomputedCanvasZoomLevel) > 0.001);

            if (visibleCols > 0) {
                if (needsFullRebuild || m_precomputedCanvasDirty) {
                    rebuildPrecomputedCanvasLocked(w, h, displayLeft, displayRight, rollingMode);
                } else if (rangeChanged) {
                    if (!advancePrecomputedCanvasLocked(displayLeft, displayRight, rollingMode)) {
                        rebuildPrecomputedCanvasLocked(w, h, displayLeft, displayRight, rollingMode);
                    }
                }
                m_precomputedLastRightCol = nowCol;
                m_precomputedLastDisplaySeq = rollingMode
                    ? displayRight
                    : static_cast<qint64>(nowCol);
            }

            if (!m_canvas.isNull() && m_canvasFilledCols > 0) {
                hasCanvas = true;
                canvasSize = m_canvas.size();
                drawCols = std::min(m_canvasFilledCols, canvasSize.width());
                srcStart = (m_canvasWriteX - drawCols + canvasSize.width()) % canvasSize.width();
                scrollOffset = columnPhase * effectiveZoom;
                if (rollingMode) {
                    drawX = static_cast<double>(w - drawCols) - columnPhase * effectiveZoom;
                } else {
                    const qint64 totalColsForScroll = m_precomputedMaxColumnIndex >= 0
                        ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
                        : std::max(static_cast<qint64>(m_precomputedTotalColumnsEstimate),
                                   static_cast<qint64>(1));
                    const bool centeredScrolling =
                        displayLeft > 0
                        && displayRight < totalColsForScroll - 1;
                    drawX = centeredScrolling ? -columnPhase * effectiveZoom : 0.0;
                }
                latestX = (m_canvasWriteX - 1 + canvasSize.width()) % canvasSize.width();
                tileCount = static_cast<int>(m_dirtyTiles.size());
                const bool refreshAllTiles = node->tileTextures.size() != tileCount;
                tileImages.reserve(tileCount);
                tileDirtyIndexes.reserve(tileCount);
                for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex) {
                    if (!refreshAllTiles
                        && !m_dirtyTiles[static_cast<size_t>(tileIndex)]
                        && node->tileTextures.value(tileIndex) != nullptr) {
                        continue;
                    }
                    const int tileStart = tileIndex * kCanvasTileWidth;
                    const int tileWidth = std::min(kCanvasTileWidth, canvasSize.width() - tileStart);
                    if (tileWidth <= 0) {
                        continue;
                    }
                    tileDirtyIndexes.push_back(tileIndex);
                    tileImages.push_back(m_canvas.copy(tileStart, 0, tileWidth, canvasSize.height()));
                    m_dirtyTiles[static_cast<size_t>(tileIndex)] = 0;
                }
            }

            if (!rollingMode && playheadPixel >= 0) {
                showPlayhead = true;
                playheadRect = QRectF(
                    static_cast<double>(std::clamp(playheadPixel, 0, w - 1)),
                    0.0,
                    1.0,
                    static_cast<double>(h));
            }

            // Frequency grid — only depends on static parameters.
            // Never rebuilt during normal playback.
            if (m_gridEnabled && m_binsPerColumn > 1) {
                const bool freqStale = m_freqGridDirty
                    || m_freqGridImage.isNull()
                    || m_freqGridImage.width() != w
                    || m_freqGridImage.height() != h
                    || m_sampleRateHz != m_freqGridCachedSampleRateHz
                    || m_binsPerColumn != m_freqGridCachedBinsPerColumn
                    || node->freqGridOverlayTexture == nullptr;
                if (freqStale) {
                    updateFreqGridOverlayLocked(w, h);
                    freqGridOverlayImage = m_freqGridImage;
                    freqGridOverlaySize = m_freqGridImage.size();
                    freqGridOverlayChanged = true;
                } else {
                    freqGridOverlaySize = m_freqGridImage.size();
                }
                showFreqGridOverlay = !m_freqGridImage.isNull();
            }

            // Time grid — vertical lines + labels (bottom only).
            // Rendered wider than the widget with right-side padding.
            // On each frame the source rect shifts by the combined
            // displayLeft + drawX delta — a free coordinate change.
            // Only rebuilt when the shift exhausts the padding.
            if (m_gridEnabled && m_binsPerColumn > 1) {
                constexpr int kTimeGridPadding = 400;
                // The pixel shift accounts for both displayLeft advancement
                // and drawX changes (critical in rolling mode where drawX
                // varies during the initial canvas fill phase).
                const double rawShift =
                    static_cast<double>(displayLeft - m_timeGridRenderDisplayLeft)
                    - (drawX - m_timeGridRenderDrawX);
                const int shift = static_cast<int>(std::round(rawShift));
                const bool timeStale = m_timeGridDirty
                    || m_timeGridImage.isNull()
                    || m_timeGridImage.height() != h
                    || shift < 0 || shift > m_timeGridPadding
                    || rollingMode != m_timeGridCachedRollingMode
                    || node->timeGridOverlayTexture == nullptr;
                if (timeStale) {
                    const double cps =
                        static_cast<double>(m_precomputedSampleRateHz)
                        / static_cast<double>(m_precomputedHopSize);
                    updateTimeGridOverlayLocked(w, h, kTimeGridPadding, displayLeft, rollingMode, cps, drawX);
                    timeGridOverlayImage = m_timeGridImage;
                    timeGridOverlayChanged = true;
                    timeGridSourceShift = 0;
                } else {
                    timeGridSourceShift = shift;
                }
                timeGridImageHeight = m_timeGridImage.height();
                showTimeGridOverlay = !m_timeGridImage.isNull();
            }

            // Crosshair overlay — rebuild on mouse move or display parameter change.
            // Renders on the hovered pane (full crosshair) and on non-hovered
            // panes when crosshairSharedX >= 0 (vertical line + bottom time label).
            if (m_crosshairEnabled && (m_hoverActive || m_crosshairSharedX >= 0.0)) {
                bool crosshairTimeStale = false;
                if (m_showTimeLabels) {
                    crosshairTimeStale =
                        displayLeft != m_crosshairCachedDisplayLeft
                        || !qFuzzyCompare(drawX + 1.0, m_crosshairCachedDrawX + 1.0)
                        || rollingMode != m_crosshairCachedRollingMode;
                }
                const bool crosshairStale = m_crosshairDirty
                    || m_crosshairImage.isNull()
                    || m_crosshairImage.width() != w
                    || m_crosshairImage.height() != h
                    || crosshairTimeStale
                    || m_binsPerColumn != m_crosshairCachedBinsPerColumn
                    || node->crosshairOverlayTexture == nullptr;
                if (crosshairStale) {
                    const double cps =
                        static_cast<double>(m_precomputedSampleRateHz)
                        / static_cast<double>(m_precomputedHopSize);
                    updateCrosshairOverlayLocked(w, h, displayLeft, rollingMode, cps, drawX);
                    crosshairOverlayImage = m_crosshairImage;
                    crosshairOverlaySize = m_crosshairImage.size();
                    crosshairOverlayChanged = true;
                } else {
                    crosshairOverlaySize = m_crosshairImage.size();
                }
                showCrosshairOverlay = !m_crosshairImage.isNull();
            }
        } else if (!m_columns.empty() && m_binsPerColumn > 0) {
            ensureCanvas(w, h);
            if (!m_canvas.isNull() && m_canvasDirty) {
                rebuildCanvasFromColumns();
            }
            if (!m_canvas.isNull()) {
                resizeDirtyTilesLocked();
                drawCols = std::min(m_canvasFilledCols, m_canvas.width());
                if (drawCols > 0) {
                    hasCanvas = true;
                    canvasSize = m_canvas.size();
                    srcStart = (m_canvasWriteX - drawCols + m_canvas.width()) % m_canvas.width();
                    scrollOffset = 0.0;
                    drawX = static_cast<double>(w - drawCols) - scrollOffset;
                    latestX = (m_canvasWriteX - 1 + m_canvas.width()) % m_canvas.width();
                    tileCount = static_cast<int>(m_dirtyTiles.size());
                    const bool refreshAllTiles = node->tileTextures.size() != tileCount;
                    tileImages.reserve(tileCount);
                    tileDirtyIndexes.reserve(tileCount);
                    for (int tileIndex = 0; tileIndex < tileCount; ++tileIndex) {
                        if (!refreshAllTiles
                            && !m_dirtyTiles[static_cast<size_t>(tileIndex)]
                            && node->tileTextures.value(tileIndex) != nullptr) {
                            continue;
                        }
                        const int tileStart = tileIndex * kCanvasTileWidth;
                        const int tileWidth = std::min(kCanvasTileWidth, m_canvas.width() - tileStart);
                        if (tileWidth <= 0) {
                            continue;
                        }
                        tileDirtyIndexes.push_back(tileIndex);
                        tileImages.push_back(m_canvas.copy(tileStart, 0, tileWidth, m_canvas.height()));
                        m_dirtyTiles[static_cast<size_t>(tileIndex)] = 0;
                    }
                }
            }
        }

        showOverlay = m_showFpsOverlay && m_fpsValue > 0;
        if (showOverlay) {
            if (m_overlayDirty || node->overlayTexture == nullptr) {
                updateOverlayImageLocked();
                overlayImage = m_overlayImage;
                overlaySize = m_overlayImage.size();
                overlayChanged = true;
            } else {
                overlaySize = m_overlayImage.size();
            }
        }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        profilePendingColumns = 0;
        profileColumnCount = m_columns.size();
        profileBinsPerColumn = m_binsPerColumn;
#endif
    }

    QVector<QSGTexture *> retiredTileTextures;
    retiredTileTextures.reserve(std::max(tileCount, static_cast<int>(node->tileTextures.size())));
    QSGTexture *oldOverlayTexture = nullptr;
    QSGTexture *oldFreqGridOverlayTexture = nullptr;
    QSGTexture *oldTimeGridOverlayTexture = nullptr;
    QSGTexture *oldCrosshairOverlayTexture = nullptr;

    if (node->tileTextures.size() > tileCount) {
        for (int i = tileCount; i < node->tileTextures.size(); ++i) {
            if (node->tileTextures[i] != nullptr) {
                retiredTileTextures.push_back(node->tileTextures[i]);
            }
        }
    }
    node->tileTextures.resize(tileCount);

    for (int i = 0; i < tileDirtyIndexes.size(); ++i) {
        const int tileIndex = tileDirtyIndexes[i];
        if (tileIndex < 0 || tileIndex >= node->tileTextures.size()) {
            continue;
        }
        QSGTexture *newTexture = nullptr;
        if (!tileImages[i].isNull() && currentWindow != nullptr) {
            newTexture = currentWindow->createTextureFromImage(tileImages[i]);
            if (newTexture != nullptr) {
                newTexture->setFiltering(QSGTexture::Nearest);
            }
        }
        if (node->tileTextures[tileIndex] != nullptr) {
            retiredTileTextures.push_back(node->tileTextures[tileIndex]);
        }
        node->tileTextures[tileIndex] = newTexture;
    }
    if (overlayChanged) {
        oldOverlayTexture = node->overlayTexture;
        node->overlayTexture = nullptr;
        if (!overlayImage.isNull() && currentWindow != nullptr) {
            node->overlayTexture = currentWindow->createTextureFromImage(overlayImage);
            if (node->overlayTexture != nullptr) {
                node->overlayTexture->setFiltering(QSGTexture::Linear);
            }
        }
    }

    if (freqGridOverlayChanged) {
        oldFreqGridOverlayTexture = node->freqGridOverlayTexture;
        node->freqGridOverlayTexture = nullptr;
        if (!freqGridOverlayImage.isNull() && currentWindow != nullptr) {
            node->freqGridOverlayTexture = currentWindow->createTextureFromImage(freqGridOverlayImage);
            if (node->freqGridOverlayTexture != nullptr) {
                node->freqGridOverlayTexture->setFiltering(QSGTexture::Linear);
            }
        }
    }

    if (timeGridOverlayChanged) {
        oldTimeGridOverlayTexture = node->timeGridOverlayTexture;
        node->timeGridOverlayTexture = nullptr;
        if (!timeGridOverlayImage.isNull() && currentWindow != nullptr) {
            node->timeGridOverlayTexture = currentWindow->createTextureFromImage(timeGridOverlayImage);
            if (node->timeGridOverlayTexture != nullptr) {
                node->timeGridOverlayTexture->setFiltering(QSGTexture::Linear);
            }
        }
    }

    if (crosshairOverlayChanged) {
        oldCrosshairOverlayTexture = node->crosshairOverlayTexture;
        node->crosshairOverlayTexture = nullptr;
        if (!crosshairOverlayImage.isNull() && currentWindow != nullptr) {
            node->crosshairOverlayTexture = currentWindow->createTextureFromImage(crosshairOverlayImage);
            if (node->crosshairOverlayTexture != nullptr) {
                node->crosshairOverlayTexture->setFiltering(QSGTexture::Linear);
            }
        }
    }

    for (QSGSimpleTextureNode *tileNode : node->tileFragments) {
        configureTextureNode(
            tileNode,
            nullptr,
            QRectF(),
            QRect(),
            node->placeholderTexture);
    }

    if (hasCanvas) {
        int fragmentCursor = 0;
        auto configureSegment = [&](int canvasOffset, int length, double targetX) {
            int remainingLength = length;
            int segmentOffset = canvasOffset;
            double segmentTargetX = targetX;
            while (remainingLength > 0) {
                const int tileIndex = segmentOffset / kCanvasTileWidth;
                if (tileIndex < 0
                    || tileIndex >= node->tileTextures.size()
                    || fragmentCursor >= node->tileFragments.size()) {
                    break;
                }
                const int tileStart = tileIndex * kCanvasTileWidth;
                const int tileWidth = std::min(kCanvasTileWidth, canvasSize.width() - tileStart);
                const int withinTile = segmentOffset - tileStart;
                const int span = std::min(remainingLength, tileWidth - withinTile);
                QSGTexture *tileTexture = node->tileTextures[tileIndex];
                configureTextureNode(
                    node->tileFragments[fragmentCursor++],
                    tileTexture,
                    QRectF(
                        segmentTargetX,
                        0.0,
                        static_cast<double>(span),
                        static_cast<double>(canvasSize.height())),
                    QRect(withinTile, 0, span, canvasSize.height()),
                    node->placeholderTexture);
                segmentOffset += span;
                segmentTargetX += static_cast<double>(span);
                remainingLength -= span;
            }
        };

        const int firstLength = std::min(canvasSize.width() - srcStart, drawCols);
        const int secondLength = drawCols - firstLength;
        configureSegment(srcStart, firstLength, drawX);
        if (secondLength > 0) {
            configureSegment(0, secondLength, drawX + static_cast<double>(firstLength));
        }

        int latestTileIndex = latestX / kCanvasTileWidth;
        if (scrollOffset > 0.0
            && latestTileIndex >= 0
            && latestTileIndex < node->tileTextures.size()) {
            QSGTexture *latestTexture = node->tileTextures[latestTileIndex];
            const int latestWithinTile = latestX - (latestTileIndex * kCanvasTileWidth);
            configureTextureNode(
                node->latest,
                latestTexture,
                QRectF(
                    drawX + static_cast<double>(drawCols),
                    0.0,
                    scrollOffset,
                    static_cast<double>(canvasSize.height())),
                QRect(latestWithinTile, 0, 1, canvasSize.height()),
                node->placeholderTexture);
        } else {
            configureTextureNode(
                node->latest,
                nullptr,
                QRectF(),
                QRect(),
                node->placeholderTexture);
        }
    } else {
        for (QSGSimpleTextureNode *tileNode : node->tileFragments) {
            configureTextureNode(
                tileNode,
                nullptr,
                QRectF(),
                QRect(),
                node->placeholderTexture);
        }
        configureTextureNode(
            node->latest,
            nullptr,
            QRectF(),
            QRect(),
            node->placeholderTexture);
    }

    if (showPlayhead) {
        node->playhead->setRect(playheadRect);
        node->playhead->setColor(QColor(255, 255, 255, 128));
    } else {
        node->playhead->setRect(QRectF());
    }

    // Frequency grid overlay — covers full widget.
    if (showFreqGridOverlay && node->freqGridOverlayTexture != nullptr && !freqGridOverlaySize.isEmpty()) {
        const QRectF target(0.0, 0.0,
                            static_cast<double>(freqGridOverlaySize.width()),
                            static_cast<double>(freqGridOverlaySize.height()));
        configureTextureNode(
            node->freqGridOverlay,
            node->freqGridOverlayTexture,
            target,
            QRect(0, 0, freqGridOverlaySize.width(), freqGridOverlaySize.height()),
            node->placeholderTexture);
    } else {
        configureTextureNode(
            node->freqGridOverlay, nullptr, QRectF(), QRect(), node->placeholderTexture);
    }

    // Time grid overlay — covers full widget, source rect shifted by scroll delta.
    if (showTimeGridOverlay && node->timeGridOverlayTexture != nullptr && timeGridImageHeight > 0) {
        configureTextureNode(
            node->timeGridOverlay,
            node->timeGridOverlayTexture,
            QRectF(0.0, 0.0, static_cast<double>(w), static_cast<double>(timeGridImageHeight)),
            QRect(timeGridSourceShift, 0, w, timeGridImageHeight),
            node->placeholderTexture);
    } else {
        configureTextureNode(
            node->timeGridOverlay, nullptr, QRectF(), QRect(), node->placeholderTexture);
    }

    // Crosshair overlay — covers full widget.
    if (showCrosshairOverlay && node->crosshairOverlayTexture != nullptr
        && !crosshairOverlaySize.isEmpty()) {
        const QRectF target(0.0, 0.0,
                            static_cast<double>(crosshairOverlaySize.width()),
                            static_cast<double>(crosshairOverlaySize.height()));
        configureTextureNode(
            node->crosshairOverlay,
            node->crosshairOverlayTexture,
            target,
            QRect(0, 0, crosshairOverlaySize.width(), crosshairOverlaySize.height()),
            node->placeholderTexture);
    } else {
        configureTextureNode(
            node->crosshairOverlay, nullptr, QRectF(), QRect(), node->placeholderTexture);
    }

    if (showOverlay && node->overlayTexture != nullptr && !overlaySize.isEmpty()) {
        const QRectF target(
            static_cast<double>(w - overlaySize.width() - 8),
            4.0,
            static_cast<double>(overlaySize.width()),
            static_cast<double>(overlaySize.height()));
        configureTextureNode(
            node->overlay,
            node->overlayTexture,
            target,
            QRect(0, 0, overlaySize.width(), overlaySize.height()),
            node->placeholderTexture);
    } else {
        configureTextureNode(
            node->overlay,
            nullptr,
            QRectF(),
            QRect(),
            node->placeholderTexture);
    }

    delete oldOverlayTexture;
    delete oldFreqGridOverlayTexture;
    delete oldTimeGridOverlayTexture;
    delete oldCrosshairOverlayTexture;
    qDeleteAll(retiredTileTextures);

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileEnabled) {
        const auto paintEnd = std::chrono::steady_clock::now();
        const double paintMs = std::chrono::duration<double, std::milli>(paintEnd - paintStart).count();
        {
            QMutexLocker paintLock(&m_stateMutex);
            noteSmoothnessPaintLocked(paintMs);
        }
        m_profilePaints += 1;
        m_profilePaintMs += paintMs;
        if (paintMs >= 4.0 && shouldLogProfileSpike(&m_profileLastPaintSpike, paintEnd)) {
            FERROUS_SPECTROGRAM_LOGF(
                stderr,
                "[ui-spectrogram] paint_spike ms=%.3f pending=%zu cols=%zu bins=%d size=%dx%d\n",
                paintMs,
                profilePendingColumns,
                profileColumnCount,
                profileBinsPerColumn,
                w,
                h);
        }
        const double elapsed = std::chrono::duration<double>(paintEnd - m_profileLast).count();
        if (elapsed >= 1.0) {
            FERROUS_SPECTROGRAM_LOGF(
                stderr,
                "[ui-spectrogram] paints/s=%llu paint_ms/s=%.2f avg_ms=%.3f cols=%zu bins=%d\n",
                static_cast<unsigned long long>(m_profilePaints),
                m_profilePaintMs,
                m_profilePaints > 0 ? (m_profilePaintMs / static_cast<double>(m_profilePaints)) : 0.0,
                profileColumnCount,
                profileBinsPerColumn);
            m_profileLast = paintEnd;
            m_profilePaints = 0;
            m_profilePaintMs = 0.0;
        }
    }
#endif

    return node;
}

void SpectrogramItem::releaseResources() {
    QMutexLocker lock(&m_stateMutex);
    m_sceneGraphGeneration += 1;
    markAllTilesDirtyLocked();
    m_overlayDirty = true;
    m_freqGridDirty = true;
    m_timeGridDirty = true;
    m_crosshairDirty = true;
}

void SpectrogramItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        QMutexLocker lock(&m_stateMutex);
        invalidateMapping();
        invalidateCanvas();
        m_freqGridDirty = true;
        m_timeGridDirty = true;
        m_crosshairDirty = true;
    }
    update();
}

double SpectrogramItem::pixelToFrequencyHz(int pixelY, int viewHeight) const {
    QMutexLocker lock(&m_stateMutex);
    // Synchronize bins and sample rate from precomputed state (normally done in updatePaintNode).
    auto *self = const_cast<SpectrogramItem *>(this);
    bool needInvalidateMapping = false;
    if (m_precomputedBinsPerColumn > 0 && m_binsPerColumn != m_precomputedBinsPerColumn) {
        self->m_binsPerColumn = m_precomputedBinsPerColumn;
        needInvalidateMapping = true;
    }
    if (m_precomputedSampleRateHz > 0
        && m_sampleRateHz != m_precomputedSampleRateHz) {
        self->m_sampleRateHz = m_precomputedSampleRateHz;
        needInvalidateMapping = true;
    }
    if (needInvalidateMapping) {
        self->invalidateMapping();
    }
    if (viewHeight <= 0 || m_binsPerColumn <= 1) {
        return -1.0;
    }
    self->ensureMapping(viewHeight);
    return pixelToFrequencyHzLocked(pixelY, viewHeight);
}

void SpectrogramItem::hoverMoveEvent(QHoverEvent *event) {
    QMutexLocker lock(&m_stateMutex);
    m_hoverActive = true;
    m_hoverPosition = event->position();
    if (m_crosshairEnabled) {
        m_crosshairDirty = true;
    }
    lock.unlock();
    if (m_crosshairEnabled) {
        emit crosshairHoverChanged(event->position().x());
        update();
    }
}

void SpectrogramItem::hoverEnterEvent(QHoverEvent *event) {
    QMutexLocker lock(&m_stateMutex);
    m_hoverActive = true;
    m_hoverPosition = event->position();
    if (m_crosshairEnabled) {
        m_crosshairDirty = true;
    }
    lock.unlock();
    if (m_crosshairEnabled) {
        emit crosshairHoverChanged(event->position().x());
        update();
    }
}

void SpectrogramItem::hoverLeaveEvent(QHoverEvent *) {
    QMutexLocker lock(&m_stateMutex);
    m_hoverActive = false;
    if (m_crosshairEnabled) {
        m_crosshairDirty = true;
    }
    lock.unlock();
    if (m_crosshairEnabled) {
        emit crosshairHoverChanged(-1.0);
        update();
    }
}

void SpectrogramItem::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::MiddleButton && m_zoomEnabled) {
        event->accept();
        emit zoomResetRequested();
        return;
    }

    if (event->button() != Qt::RightButton || !m_crosshairEnabled
        || m_displayMode == 0) {
        event->ignore();
        return;
    }

    QMutexLocker lock(&m_stateMutex);

    const double columnsPerSecond =
        m_precomputedHopSize > 0
            ? static_cast<double>(m_precomputedSampleRateHz)
                  / static_cast<double>(m_precomputedHopSize)
            : 0.0;
    if (columnsPerSecond <= 0.0) {
        lock.unlock();
        event->ignore();
        return;
    }

    const double seconds = pixelToTimeSeconds(
        event->position().x(),
        m_crosshairCachedDisplayLeft,
        m_crosshairCachedRollingMode,
        m_rollingEpoch,
        columnsPerSecond,
        m_crosshairCachedDrawX,
        effectiveZoomLocked());

    lock.unlock();

    if (seconds < 0.0) {
        event->ignore();
        return;
    }

    event->accept();
    emit seekRequested(seconds);
}

void SpectrogramItem::wheelEvent(QWheelEvent *event) {
    if (!m_zoomEnabled) {
        event->ignore();
        return;
    }
    event->accept();

    const double steps = event->angleDelta().y() / 120.0;
    if (std::abs(steps) < 0.01) {
        return;
    }

    constexpr double kZoomStepFactor = 1.25;
    constexpr double kMaxZoom = 16.0;
    QMutexLocker lock(&m_stateMutex);
    const double minZoom = minimumZoomLevelLocked();
    const double currentZoom = m_zoomLevel;
    lock.unlock();
    const double newZoom = std::clamp(
        currentZoom * std::pow(kZoomStepFactor, steps),
        minZoom, kMaxZoom);
    emit zoomRequested(newZoom);
}

double SpectrogramItem::pixelToFrequencyHzLocked(int pixelY, int viewHeight) const {
    if (viewHeight <= 0 || m_binsPerColumn <= 1
        || static_cast<int>(m_iToBin.size()) != viewHeight) {
        return -1.0;
    }
    const int row = viewHeight - 1 - pixelY;
    if (row < 0 || row >= static_cast<int>(m_iToBin.size())) {
        return -1.0;
    }
    const int bin = m_iToBin[static_cast<size_t>(row)];
    const double freqRes =
        static_cast<double>(m_sampleRateHz)
        / (2.0 * static_cast<double>(std::max(1, m_binsPerColumn - 1)));
    return static_cast<double>(bin) * freqRes;
}

int SpectrogramItem::frequencyToPixelYLocked(double freqHz, int viewHeight) const {
    if (viewHeight <= 0 || m_binsPerColumn <= 1
        || static_cast<int>(m_iToBin.size()) != viewHeight) {
        return -1;
    }
    const double freqRes =
        static_cast<double>(m_sampleRateHz)
        / (2.0 * static_cast<double>(std::max(1, m_binsPerColumn - 1)));
    const int targetBin = std::clamp(
        static_cast<int>(std::round(freqHz / freqRes)), 0, m_binsPerColumn - 1);
    // m_iToBin is non-decreasing: binary search for targetBin.
    auto it = std::lower_bound(m_iToBin.begin(), m_iToBin.end(), targetBin);
    if (it == m_iToBin.end()) {
        return 0; // above range -> top pixel
    }
    int row = static_cast<int>(std::distance(m_iToBin.begin(), it));
    // If previous row's bin is closer, prefer it.
    if (row > 0
        && std::abs(m_iToBin[static_cast<size_t>(row - 1)] - targetBin)
               < std::abs(*it - targetBin)) {
        --row;
    }
    return viewHeight - 1 - row;
}

void SpectrogramItem::updateCrosshairOverlayLocked(
    int width, int height,
    qint64 displayLeft, bool rollingMode,
    double columnsPerSecond, double drawX) {
    // Determine the effective X for the vertical crosshair line.
    // Hovered pane uses its own mouse position; non-hovered panes
    // use the shared X coordinate propagated from the hovered pane.
    const bool localHover = m_hoverActive;
    const bool hasSharedX = m_crosshairSharedX >= 0.0;
    if (!m_crosshairEnabled || (!localHover && !hasSharedX)
        || width <= 0 || height <= 0) {
        m_crosshairImage = QImage();
        m_crosshairDirty = false;
        m_crosshairCachedDisplayLeft = displayLeft;
        m_crosshairCachedDrawX = drawX;
        m_crosshairCachedRollingMode = rollingMode;
        m_crosshairCachedBinsPerColumn = m_binsPerColumn;
        return;
    }

    const int effectiveX = localHover
        ? std::clamp(static_cast<int>(m_hoverPosition.x()), 0, width - 1)
        : std::clamp(static_cast<int>(m_crosshairSharedX), 0, width - 1);

    m_crosshairImage = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
    m_crosshairImage.fill(Qt::transparent);

    QPainter painter(&m_crosshairImage);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QColor lineColor(255, 255, 255, 140);
    QPen dotPen(lineColor);
    dotPen.setStyle(Qt::DotLine);
    dotPen.setWidthF(1.0);
    painter.setPen(dotPen);

    // Vertical line: full height on all panes (spans across panes visually).
    painter.drawLine(effectiveX, 0, effectiveX, height - 1);

    if (localHover) {
        const int mouseY = std::clamp(static_cast<int>(m_hoverPosition.y()), 0, height - 1);
        // Horizontal line: full width on the hovered pane.
        painter.drawLine(0, mouseY, width - 1, mouseY);

        // Frequency label at RIGHT edge, aligned to cursor Y.
        QFont font;
        font.setPixelSize(10);
        painter.setFont(font);
        const QFontMetrics fm(font);

        const QColor labelBg(0, 0, 10, 160);
        const QColor labelFg(255, 255, 255, 220);
        constexpr int pad = 3;

        const double freqHz = pixelToFrequencyHzLocked(mouseY, height);
        if (freqHz >= 0.0) {
            const QString freqText = formatFrequencyLabelPrecise(freqHz);
            const int textW = fm.horizontalAdvance(freqText);
            const int textH = fm.height();
            const int labelY = std::clamp(mouseY - textH / 2, 0, height - textH - 2 * pad);
            const int labelX = width - textW - 2 * pad - 2;
            painter.setPen(Qt::NoPen);
            painter.setBrush(labelBg);
            painter.drawRoundedRect(labelX, labelY, textW + 2 * pad, textH + 2 * pad, 3, 3);
            painter.setPen(labelFg);
            painter.drawText(labelX + pad, labelY + pad + fm.ascent(), freqText);
        }
    }

    // Time label at BOTTOM edge — only on the bottom-most pane,
    // shown whenever any pane is hovered (via shared X).
    // Subtract gapless offset to show per-track time.
    if (m_showTimeLabels) {
        const double continuousTime = pixelToTimeSeconds(
            static_cast<double>(effectiveX), displayLeft, rollingMode,
            m_rollingEpoch, columnsPerSecond, drawX, effectiveZoomLocked());
        const double trackTime = continuousTime - m_gaplessPositionOffset;
        if (trackTime >= 0.0) {
            QFont font;
            font.setPixelSize(10);
            painter.setFont(font);
            const QFontMetrics fm(font);

            const QColor labelBg(0, 0, 10, 160);
            const QColor labelFg(255, 255, 255, 220);
            constexpr int pad = 3;

            const QString timeText = formatTimeLabelPrecise(trackTime);
            const int textW = fm.horizontalAdvance(timeText);
            const int textH = fm.height();
            const int labelX = std::clamp(effectiveX - textW / 2, 0, width - textW - 2 * pad);
            painter.setPen(Qt::NoPen);
            painter.setBrush(labelBg);
            painter.drawRoundedRect(labelX, height - textH - 2 * pad - 2,
                                     textW + 2 * pad, textH + 2 * pad, 3, 3);
            painter.setPen(labelFg);
            painter.drawText(labelX + pad, height - pad - 2 - fm.descent(), timeText);
        }
    }

    painter.end();
    m_crosshairDirty = false;
    m_crosshairCachedDisplayLeft = displayLeft;
    m_crosshairCachedDrawX = drawX;
    m_crosshairCachedRollingMode = rollingMode;
    m_crosshairCachedBinsPerColumn = m_binsPerColumn;
}

void SpectrogramItem::updateFreqGridOverlayLocked(int width, int height) {
    if (!m_gridEnabled || width <= 0 || height <= 0 || m_binsPerColumn <= 1) {
        m_freqGridImage = QImage();
        m_freqGridDirty = false;
        m_freqGridCachedSampleRateHz = m_sampleRateHz;
        m_freqGridCachedBinsPerColumn = m_binsPerColumn;
        return;
    }

    m_freqGridImage = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
    m_freqGridImage.fill(Qt::transparent);

    QPainter painter(&m_freqGridImage);
    painter.setRenderHint(QPainter::Antialiasing, false);

    QFont font;
    font.setPixelSize(9);
    painter.setFont(font);
    const QFontMetrics fm(font);

    const QColor gridLineColor(255, 255, 255, 50);
    const QColor labelBg(0, 0, 10, 180);
    const QColor labelFg(220, 220, 235, 240);
    constexpr int pad = 2;
    constexpr int minPixelSpacing = 50;

    const double nyquist = 0.5 * static_cast<double>(m_sampleRateHz);

    static constexpr double kFreqCandidates[] = {
        50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000
    };
    const double hzPerPixel = nyquist / static_cast<double>(std::max(1, height));
    const double freqInterval = selectGridInterval(
        kFreqCandidates,
        static_cast<int>(std::size(kFreqCandidates)),
        hzPerPixel,
        minPixelSpacing);

    QPen gridPen(gridLineColor);
    gridPen.setWidthF(1.0);

    int prevPixelY = -1;
    for (double freq = freqInterval; freq < nyquist; freq += freqInterval) {
        const int pixelY = frequencyToPixelYLocked(freq, height);
        if (pixelY < 0 || pixelY >= height) {
            continue;
        }
        if (prevPixelY >= 0 && std::abs(pixelY - prevPixelY) < minPixelSpacing) {
            continue;
        }
        prevPixelY = pixelY;

        painter.setPen(gridPen);
        painter.drawLine(0, pixelY, width - 1, pixelY);

        const QString label = formatFrequencyLabel(freq);
        const int textW = fm.horizontalAdvance(label);
        const int textH = fm.height();
        const int labelY = std::clamp(pixelY - textH / 2, 0, height - textH - 2 * pad);
        const int labelX = width - textW - 2 * pad - 2;
        painter.setPen(Qt::NoPen);
        painter.setBrush(labelBg);
        painter.drawRoundedRect(labelX, labelY, textW + 2 * pad, textH + 2 * pad, 2, 2);
        painter.setPen(labelFg);
        painter.drawText(labelX + pad, labelY + pad + fm.ascent(), label);
    }

    painter.end();
    m_freqGridDirty = false;
    m_freqGridCachedSampleRateHz = m_sampleRateHz;
    m_freqGridCachedBinsPerColumn = m_binsPerColumn;
}

void SpectrogramItem::updateTimeGridOverlayLocked(
    int width, int height, int padding,
    qint64 displayLeft,
    bool rollingMode, double columnsPerSecond, double drawX) {
    if (!m_gridEnabled || width <= 0 || height <= 0 || columnsPerSecond <= 0.0) {
        m_timeGridImage = QImage();
        m_timeGridDirty = false;
        m_timeGridRenderDisplayLeft = displayLeft;
        m_timeGridRenderDrawX = drawX;
        m_timeGridCachedRollingMode = rollingMode;
        m_timeGridPadding = 0;
        return;
    }

    // Render wider than the widget so the texture source rect can be
    // shifted cheaply on each frame without rebuilding.
    const int renderWidth = width + padding;
    m_timeGridImage = QImage(renderWidth, height, QImage::Format_ARGB32_Premultiplied);
    m_timeGridImage.fill(Qt::transparent);

    QPainter painter(&m_timeGridImage);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QColor gridLineColor(255, 255, 255, 50);
    constexpr int minPixelSpacing = 50;

    static constexpr double kTimeCandidates[] = {
        1, 2, 5, 10, 15, 30, 60, 120, 300, 600
    };
    const double secondsPerPixel = 1.0 / (columnsPerSecond * effectiveZoomLocked());
    const double timeInterval = selectGridInterval(
        kTimeCandidates,
        static_cast<int>(std::size(kTimeCandidates)),
        secondsPerPixel,
        minPixelSpacing);

    // Compute time range for the full padded width.
    // These are in continuous (cross-track) time.
    const double ez = effectiveZoomLocked();
    const double timeLeft = pixelToTimeSeconds(
        0.0, displayLeft, rollingMode, m_rollingEpoch, columnsPerSecond, drawX, ez);
    const double timeRight = pixelToTimeSeconds(
        static_cast<double>(renderWidth - 1), displayLeft, rollingMode,
        m_rollingEpoch, columnsPerSecond, drawX, ez);

    // Convert to per-track time for grid line snapping and labels.
    // m_gaplessPositionOffset is the continuous-time value where the
    // current track starts (0 when no gapless transition has occurred).
    const double trackTimeLeft = timeLeft - m_gaplessPositionOffset;
    const double trackTimeRight = timeRight - m_gaplessPositionOffset;

    const double startTrackTime = std::max(0.0,
        std::ceil(std::max(0.0, trackTimeLeft) / timeInterval) * timeInterval);

    QPen gridPen(gridLineColor);
    gridPen.setWidthF(1.0);

    QFont font;
    QFontMetrics fm(font);
    QColor labelBg;
    QColor labelFg;
    constexpr int pad = 2;
    if (m_showTimeLabels) {
        font.setPixelSize(9);
        fm = QFontMetrics(font);
        labelBg = QColor(0, 0, 10, 180);
        labelFg = QColor(220, 220, 235, 240);
        painter.setFont(font);
    }

    for (double trackT = startTrackTime; trackT <= trackTimeRight; trackT += timeInterval) {
        // Convert per-track time back to continuous time for pixel positioning.
        const double continuousT = trackT + m_gaplessPositionOffset;
        const double pxF = timeToPixelX(
            continuousT, displayLeft, rollingMode, m_rollingEpoch, columnsPerSecond, drawX,
            ez);
        const int pixelX = static_cast<int>(std::round(pxF));
        if (pixelX < 0 || pixelX >= renderWidth) {
            continue;
        }

        painter.setPen(gridPen);
        painter.drawLine(pixelX, 0, pixelX, height - 1);

        if (m_showTimeLabels) {
            const QString label = formatTimeLabel(trackT);
            const int textW = fm.horizontalAdvance(label);
            const int textH = fm.height();
            const int labelX = std::clamp(pixelX - textW / 2, 0, renderWidth - textW - 2 * pad);
            painter.setPen(Qt::NoPen);
            painter.setBrush(labelBg);
            painter.drawRoundedRect(labelX, height - textH - 2 * pad - 2,
                                     textW + 2 * pad, textH + 2 * pad, 2, 2);
            painter.setPen(labelFg);
            painter.drawText(labelX + pad, height - pad - 2 - fm.descent(), label);
        }
    }

    painter.end();
    m_timeGridDirty = false;
    m_timeGridRenderDisplayLeft = displayLeft;
    m_timeGridRenderDrawX = drawX;
    m_timeGridCachedRollingMode = rollingMode;
    m_timeGridPadding = padding;
}

void SpectrogramItem::rebuildPalette() {
    // SoX spectrogram default palette (from spectrogram.c, perm=0).
    // Procedural sin/cos curves produce:
    //   Black → Dark Blue → Purple → Magenta → Red → Orange → Yellow → White
    // This matches Adobe Audition 3.0's spectral display gradient.
    //
    // Our palette array: index 0 = brightest (x=1), index N-1 = darkest (x=0).

    for (int i = 0; i < kGradientTableSize; ++i) {
        // x: 1.0 at index 0 (loud/bright), 0.0 at last index (quiet/dark).
        const double x = 1.0
            - static_cast<double>(i) / static_cast<double>(kGradientTableSize - 1);

        // c0 (red): sin ramp from x=0.13 to x=0.73, then saturated.
        double c0;
        if (x < 0.13)
            c0 = 0.0;
        else if (x < 0.73)
            c0 = std::sin((x - 0.13) / 0.60 * M_PI / 2.0);
        else
            c0 = 1.0;

        // c1 (green): sin ramp from x=0.60 to x=0.91, then saturated.
        double c1;
        if (x < 0.60)
            c1 = 0.0;
        else if (x < 0.91)
            c1 = std::sin((x - 0.60) / 0.31 * M_PI / 2.0);
        else
            c1 = 1.0;

        // c2 (blue): half-sine from 0 to 0.60, gap, linear ramp 0.78-1.0.
        double c2;
        if (x < 0.60)
            c2 = 0.5 * std::sin(x / 0.60 * M_PI);
        else if (x < 0.78)
            c2 = 0.0;
        else
            c2 = (x - 0.78) / 0.22;

        // perm=0: R=c[0], G=c[1], B=c[2] (no channel swapping).
        m_palette32[static_cast<size_t>(i)] = qRgb(
            std::clamp(static_cast<int>(0.5 + 255.0 * c0), 0, 255),
            std::clamp(static_cast<int>(0.5 + 255.0 * c1), 0, 255),
            std::clamp(static_cast<int>(0.5 + 255.0 * c2), 0, 255));
    }

    // Build matching grayscale palette using luminance weights.
    for (int i = 0; i < kGradientTableSize; ++i) {
        const int r = qRed(m_palette32[static_cast<size_t>(i)]);
        const int g = qGreen(m_palette32[static_cast<size_t>(i)]);
        const int b = qBlue(m_palette32[static_cast<size_t>(i)]);
        // ITU-R BT.709 luminance.
        const int lum = std::clamp(
            static_cast<int>(0.2126 * r + 0.7152 * g + 0.0722 * b + 0.5),
            0, 255);
        m_palette32Gray[static_cast<size_t>(i)] = qRgb(lum, lum, lum);
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
    m_precomputedCanvasDisplayLeft = 0;
    m_precomputedCanvasDisplayRight = -1;
    m_precomputedCanvasRolling = false;
    m_precomputedCanvasDirty = true;
    m_dirtyTiles.clear();
}

void SpectrogramItem::resizeDirtyTilesLocked() {
    if (m_canvas.isNull() || m_canvas.width() <= 0) {
        m_dirtyTiles.clear();
        return;
    }
    const int tileCount = std::max(1, (m_canvas.width() + kCanvasTileWidth - 1) / kCanvasTileWidth);
    if (static_cast<int>(m_dirtyTiles.size()) != tileCount) {
        m_dirtyTiles.assign(static_cast<size_t>(tileCount), 1);
    }
}

void SpectrogramItem::markTileDirtyLocked(int x) {
    if (m_canvas.isNull() || m_canvas.width() <= 0) {
        return;
    }
    resizeDirtyTilesLocked();
    const int tileIndex = std::clamp(x / kCanvasTileWidth, 0, static_cast<int>(m_dirtyTiles.size()) - 1);
    m_dirtyTiles[static_cast<size_t>(tileIndex)] = 1;
}

void SpectrogramItem::markAllTilesDirtyLocked() {
    resizeDirtyTilesLocked();
    std::fill(m_dirtyTiles.begin(), m_dirtyTiles.end(), 1);
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
        const double freqRes = std::max(
            1.0,
            static_cast<double>(m_sampleRateHz) / (2.0 * std::max(1, m_binsPerColumn - 1)));
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
            const int bin = static_cast<int>(std::floor(
                (static_cast<double>(i) / std::max(1, height - 1))
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
        resizeDirtyTilesLocked();
        markAllTilesDirtyLocked();
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
        markAllTilesDirtyLocked();
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
    markAllTilesDirtyLocked();
}

void SpectrogramItem::drawColumnAt(int x, const std::vector<quint8> &col) {
    if (m_canvas.isNull() || x < 0 || x >= m_canvas.width() || col.empty()) {
        return;
    }

    markTileDirtyLocked(x);

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
        const auto &palette = m_channelMuted ? m_palette32Gray : m_palette32;
        auto *line = reinterpret_cast<QRgb *>(m_canvas.scanLine(y));
        line[x] = palette[static_cast<size_t>(paletteIndex)];
    }
}

std::array<quint8, 256> SpectrogramItem::buildPrecomputedDbRemapLocked() const {
    std::array<quint8, 256> dbRemap{};
    static constexpr double kBakedDbRange = 132.0;
    const int fftSize = std::max(2, (m_precomputedBinsPerColumn - 1) * 2);
    const double peakDb = 20.0
        * std::log10(std::max(
            1.0,
            static_cast<double>(fftSize) * 0.35875 / 2.0));
    const double userDbRange = std::clamp(m_dbRange, 50.0, 150.0);
    for (int i = 0; i < 256; ++i) {
        const double db = (static_cast<double>(i) / 255.0)
            * kBakedDbRange - kBakedDbRange + peakDb;
        const double xdb = std::clamp(
            db + userDbRange - peakDb,
            0.0,
            userDbRange);
        const double normalized = xdb / userDbRange;
        static constexpr double kB = 1.3;
        const double gamma = (normalized > 0.0)
            ? std::pow(normalized, 2.0 - kB * normalized)
            : 0.0;
        const double remapped = gamma * 255.0;
        dbRemap[static_cast<size_t>(i)] = static_cast<quint8>(std::clamp(
            static_cast<int>(std::lround(remapped)),
            0,
            255));
    }
    return dbRemap;
}

void SpectrogramItem::drawPrecomputedColumnAtLocked(
    int x,
    qint64 displayIndex,
    bool rollingMode,
    const std::array<quint8, 256> &dbRemap) {
    if (m_canvas.isNull() || x < 0 || x >= m_canvas.width() || m_precomputedBinsPerColumn <= 0) {
        return;
    }

    markTileDirtyLocked(x);

    const int bins = m_precomputedBinsPerColumn;
    const auto *ringData = reinterpret_cast<const quint8 *>(m_ringBuffer.constData());
    const QRgb bgColor = kBackgroundColor.rgba();
    const double gradScale = static_cast<double>(kGradientTableSize) / 255.0;

    int slot = -1;
    bool valid = false;
    if (rollingMode) {
        slot = m_ringCapacity > 0
            ? static_cast<int>(displayIndex % m_ringCapacity)
            : -1;
        valid = displayIndex >= m_ringOldestSeq
            && displayIndex < m_ringWriteSeq
            && slot >= 0
            && slot < m_ringCapacity
            && !m_ringSequenceId.empty()
            && m_ringSequenceId[static_cast<size_t>(slot)] == displayIndex
            && !m_ringColumnId.empty()
            && m_ringColumnId[static_cast<size_t>(slot)] >= 0;
    } else {
        const auto tokenColumns = m_trackColumnToSeqByToken.constFind(m_precomputedTrackToken);
        const qint64 seq = tokenColumns != m_trackColumnToSeqByToken.cend()
            ? tokenColumns->value(static_cast<qint32>(displayIndex), -1)
            : -1;
        slot = seq >= 0 && m_ringCapacity > 0
            ? static_cast<int>(seq % m_ringCapacity)
            : -1;
        valid = displayIndex >= 0
            && seq >= m_ringOldestSeq
            && seq < m_ringWriteSeq
            && slot >= 0
            && slot < m_ringCapacity
            && !m_ringSequenceId.empty()
            && m_ringSequenceId[static_cast<size_t>(slot)] == seq
            && !m_ringColumnId.empty()
            && m_ringColumnId[static_cast<size_t>(slot)] == static_cast<qint32>(displayIndex);
    }

    if (!valid) {
        for (int y = 0; y < m_canvas.height(); ++y) {
            reinterpret_cast<QRgb *>(m_canvas.scanLine(y))[x] = bgColor;
        }
        return;
    }

    const int baseOff = slot * bins;
    const int mapSize = static_cast<int>(m_iToBin.size());
    for (int y = 0; y < m_canvas.height(); ++y) {
        const int mi = m_canvas.height() - 1 - y;
        int binLo = 0;
        int binHi = 0;
        if (mi >= 0 && mi < mapSize) {
            const int bc = m_iToBin[static_cast<size_t>(mi)];
            const int bp = (mi > 0)
                ? m_iToBin[static_cast<size_t>(mi - 1)]
                : bc;
            const int bn = (mi + 1 < mapSize)
                ? m_iToBin[static_cast<size_t>(mi + 1)]
                : bc;
            binLo = bp + (bc - bp) / 2;
            binHi = bc + (bn - bc) / 2;
            if (binLo == bp && bp != bc) {
                binLo = bc;
            }
            if (binHi == bn && bn != bc) {
                binHi = bc;
            }
            if (binLo > binHi) {
                std::swap(binLo, binHi);
            }
            binLo = std::clamp(binLo, 0, bins - 1);
            binHi = std::clamp(binHi, 0, bins - 1);
        }

        quint8 rawMax = 0;
        for (int b = binLo; b <= binHi; ++b) {
            rawMax = std::max(rawMax, ringData[baseOff + b]);
        }
        const quint8 intensity = dbRemap[static_cast<size_t>(rawMax)];
        int paletteIndex = kGradientTableSize
            - static_cast<int>(std::lround(gradScale * static_cast<double>(intensity)));
        paletteIndex = std::clamp(paletteIndex, 0, kGradientTableSize - 1);
        const auto &palette = m_channelMuted ? m_palette32Gray : m_palette32;
        reinterpret_cast<QRgb *>(m_canvas.scanLine(y))[x] =
            palette[static_cast<size_t>(paletteIndex)];
    }
}

int SpectrogramItem::ringSlotForDisplayIndexLocked(
    qint64 displayIndex, bool rollingMode) const {
    if (m_ringCapacity <= 0 || m_ringSequenceId.empty() || m_ringColumnId.empty()) {
        return -1;
    }
    if (rollingMode) {
        const int slot = static_cast<int>(displayIndex % m_ringCapacity);
        if (displayIndex >= m_ringOldestSeq
            && displayIndex < m_ringWriteSeq
            && slot >= 0 && slot < m_ringCapacity
            && m_ringSequenceId[static_cast<size_t>(slot)] == displayIndex
            && m_ringColumnId[static_cast<size_t>(slot)] >= 0) {
            return slot;
        }
        return -1;
    }
    const auto tokenColumns =
        m_trackColumnToSeqByToken.constFind(m_precomputedTrackToken);
    const qint64 seq = tokenColumns != m_trackColumnToSeqByToken.cend()
        ? tokenColumns->value(static_cast<qint32>(displayIndex), -1)
        : -1;
    if (seq < 0) {
        return -1;
    }
    const int slot = static_cast<int>(seq % m_ringCapacity);
    if (displayIndex >= 0
        && seq >= m_ringOldestSeq && seq < m_ringWriteSeq
        && slot >= 0 && slot < m_ringCapacity
        && m_ringSequenceId[static_cast<size_t>(slot)] == seq
        && m_ringColumnId[static_cast<size_t>(slot)]
               == static_cast<qint32>(displayIndex)) {
        return slot;
    }
    return -1;
}

void SpectrogramItem::drawInterpolatedColumnAtLocked(
    int x,
    qint64 displayIndexL,
    qint64 displayIndexR,
    double t,
    bool rollingMode,
    const std::array<quint8, 256> &dbRemap) {
    if (m_canvas.isNull() || x < 0 || x >= m_canvas.width()
        || m_precomputedBinsPerColumn <= 0) {
        return;
    }

    markTileDirtyLocked(x);

    const int bins = m_precomputedBinsPerColumn;
    const auto *ringData =
        reinterpret_cast<const quint8 *>(m_ringBuffer.constData());
    const QRgb bgColor = kBackgroundColor.rgba();
    const double gradScale =
        static_cast<double>(kGradientTableSize) / 255.0;

    const int slotL = ringSlotForDisplayIndexLocked(displayIndexL, rollingMode);
    const int slotR = ringSlotForDisplayIndexLocked(displayIndexR, rollingMode);

    if (slotL < 0 && slotR < 0) {
        for (int y = 0; y < m_canvas.height(); ++y) {
            reinterpret_cast<QRgb *>(m_canvas.scanLine(y))[x] = bgColor;
        }
        return;
    }
    if (slotL < 0) {
        drawPrecomputedColumnAtLocked(
            x, displayIndexR, rollingMode, dbRemap);
        return;
    }
    if (slotR < 0) {
        drawPrecomputedColumnAtLocked(
            x, displayIndexL, rollingMode, dbRemap);
        return;
    }

    const int baseL = slotL * bins;
    const int baseR = slotR * bins;
    const double oneMinusT = 1.0 - t;
    const int mapSize = static_cast<int>(m_iToBin.size());
    const auto &palette = m_channelMuted ? m_palette32Gray : m_palette32;

    for (int y = 0; y < m_canvas.height(); ++y) {
        const int mi = m_canvas.height() - 1 - y;
        int binLo = 0;
        int binHi = 0;
        if (mi >= 0 && mi < mapSize) {
            const int bc = m_iToBin[static_cast<size_t>(mi)];
            const int bp = (mi > 0)
                ? m_iToBin[static_cast<size_t>(mi - 1)]
                : bc;
            const int bn = (mi + 1 < mapSize)
                ? m_iToBin[static_cast<size_t>(mi + 1)]
                : bc;
            binLo = bp + (bc - bp) / 2;
            binHi = bc + (bn - bc) / 2;
            if (binLo == bp && bp != bc) {
                binLo = bc;
            }
            if (binHi == bn && bn != bc) {
                binHi = bc;
            }
            if (binLo > binHi) {
                std::swap(binLo, binHi);
            }
            binLo = std::clamp(binLo, 0, bins - 1);
            binHi = std::clamp(binHi, 0, bins - 1);
        }

        quint8 rawMaxL = 0;
        quint8 rawMaxR = 0;
        for (int b = binLo; b <= binHi; ++b) {
            rawMaxL = std::max(rawMaxL, ringData[baseL + b]);
            rawMaxR = std::max(rawMaxR, ringData[baseR + b]);
        }

        const auto rawBlended = static_cast<quint8>(std::lround(
            static_cast<double>(rawMaxL) * oneMinusT
            + static_cast<double>(rawMaxR) * t));
        const quint8 intensity = dbRemap[static_cast<size_t>(rawBlended)];
        int paletteIndex = kGradientTableSize
            - static_cast<int>(
                  std::lround(gradScale * static_cast<double>(intensity)));
        paletteIndex = std::clamp(paletteIndex, 0, kGradientTableSize - 1);
        reinterpret_cast<QRgb *>(m_canvas.scanLine(y))[x] =
            palette[static_cast<size_t>(paletteIndex)];
    }
}

void SpectrogramItem::rebuildPrecomputedCanvasLocked(
    int width,
    int height,
    qint64 displayLeft,
    qint64 displayRight,
    bool rollingMode) {
    if (width <= 0 || height <= 0 || displayRight < displayLeft) {
        invalidateCanvas();
        return;
    }

    if (m_canvas.isNull()
        || m_canvas.width() != width
        || m_canvas.height() != height
        || m_canvas.format() != QImage::Format_RGB32) {
        m_canvas = QImage(width, height, QImage::Format_RGB32);
    }
    m_canvas.fill(Qt::black);
    resizeDirtyTilesLocked();
    markAllTilesDirtyLocked();

    const qint64 sourceColumns = displayRight - displayLeft + 1;
    const double rebuildEffectiveZoom = effectiveZoomLocked();
    const int drawPixels = std::min(width,
        static_cast<int>(std::ceil(
            static_cast<double>(sourceColumns) * rebuildEffectiveZoom)));
    const double columnsPerPixel = 1.0 / rebuildEffectiveZoom;
    const auto dbRemap = buildPrecomputedDbRemapLocked();

    const bool interpolate = rebuildEffectiveZoom > 1.001;
    for (int px = 0; px < drawPixels; ++px) {
        const double srcColF =
            static_cast<double>(px) * columnsPerPixel;
        const qint64 colL = std::min(
            displayLeft + static_cast<qint64>(std::floor(srcColF)),
            displayRight);

        if (interpolate) {
            const double t = srcColF - std::floor(srcColF);
            if (t > 0.001) {
                const qint64 colR = std::min(colL + 1, displayRight);
                if (colR != colL) {
                    drawInterpolatedColumnAtLocked(
                        px, colL, colR, t, rollingMode, dbRemap);
                    continue;
                }
            }
        }
        drawPrecomputedColumnAtLocked(px, colL, rollingMode, dbRemap);
    }

    m_canvasWriteX = width > 0 ? (drawPixels % width) : 0;
    m_canvasFilledCols = drawPixels;
    m_precomputedCanvasDisplayLeft = displayLeft;
    m_precomputedCanvasDisplayRight =
        drawPixels > 0
            ? (displayLeft + static_cast<qint64>(
                   static_cast<double>(drawPixels) * columnsPerPixel) - 1)
            : (displayLeft - 1);
    m_precomputedCanvasRolling = rollingMode;
    m_precomputedCanvasZoomLevel = rebuildEffectiveZoom;
    m_precomputedCanvasDirty = false;
}

bool SpectrogramItem::advancePrecomputedCanvasLocked(
    qint64 displayLeft,
    qint64 displayRight,
    bool rollingMode) {
    // Incremental advance only works at 1:1 column-to-pixel mapping.
    // TODO: Implement incremental advance for non-1.0 zoom levels if
    // full rebuild shows measurable frame drops on target hardware.
    if (std::abs(effectiveZoomLocked() - 1.0) > 0.001) {
        return false;
    }
    if (m_canvas.isNull()
        || m_canvas.width() <= 0
        || displayRight < displayLeft
        || rollingMode != m_precomputedCanvasRolling
        || m_precomputedCanvasDisplayRight < m_precomputedCanvasDisplayLeft) {
        return false;
    }

    if (displayLeft < m_precomputedCanvasDisplayLeft
        || displayRight < m_precomputedCanvasDisplayRight) {
        return false;
    }

    const int width = m_canvas.width();
    const int nextVisibleCols = std::min(
        width,
        static_cast<int>(std::max<qint64>(0, displayRight - displayLeft + 1)));
    if (nextVisibleCols <= 0) {
        m_canvasFilledCols = 0;
        m_precomputedCanvasDisplayLeft = displayLeft;
        m_precomputedCanvasDisplayRight = displayLeft - 1;
        m_precomputedCanvasDirty = false;
        return true;
    }

    const qint64 appendStart = std::max(m_precomputedCanvasDisplayRight + 1, displayLeft);
    const qint64 appendCount = std::max<qint64>(0, displayRight - appendStart + 1);
    if (appendCount > width) {
        return false;
    }

    if (appendCount > 0) {
        const auto dbRemap = buildPrecomputedDbRemapLocked();
        for (qint64 displayIndex = appendStart; displayIndex <= displayRight; ++displayIndex) {
            drawPrecomputedColumnAtLocked(
                m_canvasWriteX,
                displayIndex,
                rollingMode,
                dbRemap);
            m_canvasWriteX = (m_canvasWriteX + 1) % width;
            m_canvasFilledCols = std::min(width, m_canvasFilledCols + 1);
        }
    }

    m_canvasFilledCols = nextVisibleCols;
    m_precomputedCanvasDisplayLeft = displayLeft;
    m_precomputedCanvasDisplayRight =
        displayLeft + static_cast<qint64>(nextVisibleCols) - 1;
    m_precomputedCanvasDirty = false;
    return true;
}

double SpectrogramItem::targetRowsPerSecondLocked() const {
    if (m_sampleRateHz > 0) {
        const double stableRate = static_cast<double>(m_sampleRateHz) / kReferenceHopSamples;
        return std::clamp(stableRate, 1.0, 400.0);
    }
    return 0.0;
}

void SpectrogramItem::updateOverlayImageLocked() {
    if (!m_showFpsOverlay || m_fpsValue <= 0) {
        m_overlayImage = QImage();
        m_overlayDirty = false;
        return;
    }

    QFont font;
    font.setPixelSize(10);
    const QString text = QStringLiteral("%1 fps").arg(m_fpsValue);
    const QFontMetrics metrics(font);
    const int imageWidth = std::max(1, metrics.horizontalAdvance(text));
    const int imageHeight = std::max(1, metrics.height());

    m_overlayImage = QImage(imageWidth, imageHeight, QImage::Format_ARGB32_Premultiplied);
    m_overlayImage.fill(Qt::transparent);

    QPainter painter(&m_overlayImage);
    painter.setFont(font);
    painter.setPen(kOverlayColor);
    painter.drawText(QPointF(0.0, static_cast<double>(metrics.ascent())), text);
    painter.end();

    m_overlayDirty = false;
}

std::vector<quint8> SpectrogramItem::rowToIntensity(const QVariantList &row) const {
    std::vector<quint8> out;
    out.reserve(static_cast<size_t>(row.size()));

    const double dbRange = std::clamp(m_dbRange, 50.0, 150.0);
    const double dbScale = 10.0 / std::log(10.0);
    // BH4 peak power normalisation: 20·log₁₀(N·a₀/2).
    const int fftSize = (m_binsPerColumn > 1) ? (m_binsPerColumn - 1) * 2 : 2048;
    const double peakDb = 20.0
        * std::log10(std::max(1.0,
            static_cast<double>(fftSize) * 0.35875 / 2.0));

    for (const QVariant &value : row) {
        const double v = value.toDouble();
        int idx = 0;
        if (v >= 0.0 && v <= 255.0 && std::floor(v) == v) {
            idx = static_cast<int>(v);
        } else {
            const double db = v > 0.0 ? (dbScale * std::log(v)) : -200.0;
            const double xdb = std::clamp(db + dbRange - peakDb, 0.0, dbRange);
            idx = static_cast<int>(std::lround((xdb / dbRange) * 255.0));
        }
        out.push_back(static_cast<quint8>(std::clamp(idx, 0, 255)));
    }

    return out;
}

void SpectrogramItem::bindWindowFpsTracking(QQuickWindow *window) {
    if (m_animationTickConnection) {
        disconnect(m_animationTickConnection);
        m_animationTickConnection = QMetaObject::Connection{};
    }
    if (m_windowVisibilityConnection) {
        disconnect(m_windowVisibilityConnection);
        m_windowVisibilityConnection = QMetaObject::Connection{};
    }
    {
        QMutexLocker lock(&m_stateMutex);
        m_sceneGraphGeneration += 1;
        markAllTilesDirtyLocked();
        m_overlayDirty = true;
        m_fpsInitialized = false;
        m_fpsValue = 0;
        m_fpsAccumFrames = 0;
        m_fpsAccumSeconds = 0.0;
        m_animationTickInitialized = false;
    }

    if (window == nullptr) {
        update();
        return;
    }
    m_animationTickConnection = connect(
        window,
        &QQuickWindow::frameSwapped,
        this,
        &SpectrogramItem::handleWindowAfterAnimating,
        Qt::QueuedConnection);
    // When the item is reparented into a window that is currently hidden
    // (e.g. the fullscreen viewer window before it is made visible),
    // frameSwapped is never emitted for the hidden window, so the
    // self-sustaining render loop is never started. Connect to
    // visibleChanged so we can re-kick the loop the moment the window
    // is shown.
    m_windowVisibilityConnection = connect(
        window,
        &QWindow::visibleChanged,
        this,
        [this](bool visible) {
            if (!visible) {
                return;
            }
            {
                QMutexLocker lock(&m_stateMutex);
                markAllTilesDirtyLocked();
                m_overlayDirty = true;
                m_animationTickInitialized = false;
            }
            update();
        },
        Qt::QueuedConnection);
    update();
}

void SpectrogramItem::handleWindowAfterAnimating() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
#endif

    QMutexLocker lock(&m_stateMutex);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    double elapsed = 0.0;
    if (m_animationTickInitialized) {
        elapsed = std::chrono::duration<double>(now - m_lastAnimationTick).count();
    }
#endif
    m_lastAnimationTick = now;
    m_animationTickInitialized = true;
    if (m_positionJumpHoldActive) {
        const double holdElapsedSeconds =
            std::chrono::duration<double>(now - m_positionJumpHoldStartedAt).count();
        if (holdElapsedSeconds >= kPositionJumpHoldTimeoutSeconds) {
            setPositionAnchorLocked(m_positionJumpHoldSeconds, now);
            m_positionJumpHoldActive = false;
        }
    }

    const int prev = m_fpsValue;
    bool changed = false;
    if (m_showFpsOverlay) {
        updateFpsEstimateLocked();
        changed = m_fpsValue != prev;
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    maybeStartSmoothnessProfileLocked(nowMs);
    maybeStartSeekProfileLocked(nowMs);
#endif
    const bool precomputedActive = m_precomputedReady;
    const bool playing = m_playing;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    noteSmoothnessProfileFrameLocked(nowMs, elapsed, false, false);
    noteSeekProfileFrameLocked(nowMs, elapsed, false, false);
    if (m_profileEnabled
        && elapsed >= 0.025
        && shouldLogProfileSpike(&m_profileLastFrameGapSpike, now)) {
        FERROUS_SPECTROGRAM_LOGF(
            stderr,
            "[ui-spectrogram] frame_gap ms=%.3f fps=%d\n",
            elapsed * 1000.0,
            m_fpsValue);
    }
#endif
    lock.unlock();
    // Only sustain the render loop when the spectrogram is actively
    // scrolling (playing).  When stopped/paused the display is static
    // and re-renders are already triggered on demand by data feeds
    // (feedPrecomputedChunk) or property changes (setPositionSeconds).
    if (changed || (precomputedActive && playing)) {
        update();
    }
}

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
void SpectrogramItem::resetSmoothnessProfileLocked() {
    m_smoothnessProfile = SmoothnessProfileState{};
}

void SpectrogramItem::maybeStartSmoothnessProfileLocked(qint64 nowMs) {
    if (!m_profileEnabled) {
        return;
    }

    const bool streamActive =
        (m_lastIncomingRowsAtMs > 0 && (nowMs - m_lastIncomingRowsAtMs) <= kSmoothnessIdleMs);

    if (m_smoothnessProfile.active) {
        if ((nowMs - m_smoothnessProfile.startedAtMs) >= kSmoothnessWindowMs) {
            finalizeSmoothnessProfileLocked(nowMs, "rolling");
        } else if (!streamActive && (nowMs - m_smoothnessProfile.lastFrameAtMs) >= kSmoothnessIdleMs) {
            finalizeSmoothnessProfileLocked(nowMs, "idle");
        }
    }

    if (!m_smoothnessProfile.active && streamActive) {
        resetSmoothnessProfileLocked();
        m_smoothnessProfile.active = true;
        m_smoothnessProfile.startedAtMs = nowMs;
        m_smoothnessProfile.lastFrameAtMs = nowMs;
    }
}

void SpectrogramItem::noteSmoothnessProfileFrameLocked(
    qint64 nowMs,
    double elapsedSeconds,
    bool pending,
    bool advanced) {
    if (!m_smoothnessProfile.active) {
        return;
    }

    m_smoothnessProfile.framesObserved += 1;
    m_smoothnessProfile.lastFrameAtMs = nowMs;
    if (pending) {
        m_smoothnessProfile.pendingFrames += 1;
    } else {
        m_smoothnessProfile.inStallCluster = false;
    }

    const int canvasWidth = m_canvas.width() > 0 ? m_canvas.width()
        : std::max(1, static_cast<int>(std::floor(width())));
    const double headUnits = static_cast<double>(m_canvasWriteX);
    double unwrappedHeadUnits = headUnits;
    if (m_smoothnessProfile.lastHeadValid && canvasWidth > 1) {
        if ((m_smoothnessProfile.lastHeadUnits - unwrappedHeadUnits)
            > (static_cast<double>(canvasWidth) * 0.5)) {
            unwrappedHeadUnits += static_cast<double>(canvasWidth);
        }
        const double delta = unwrappedHeadUnits - m_smoothnessProfile.lastHeadUnits;
        if (pending) {
            if (delta > 0.05) {
                m_smoothnessProfile.sawForwardMotion = true;
                m_smoothnessProfile.inStallCluster = false;
            } else if (m_smoothnessProfile.sawForwardMotion) {
                m_smoothnessProfile.stallFrames += 1;
                if (!m_smoothnessProfile.inStallCluster) {
                    m_smoothnessProfile.stallClusters += 1;
                    m_smoothnessProfile.inStallCluster = true;
                }
            }
        }
        if (delta < -0.05) {
            m_smoothnessProfile.regressionCount += 1;
        }
    }
    m_smoothnessProfile.lastHeadUnits = unwrappedHeadUnits;
    m_smoothnessProfile.lastHeadValid = true;

    const double elapsedMs = elapsedSeconds * 1000.0;
    if (elapsedMs >= 25.0) {
        m_smoothnessProfile.gapFrames += 1;
        m_smoothnessProfile.maxGapMs = std::max(m_smoothnessProfile.maxGapMs, elapsedMs);
        if (pending) {
            m_smoothnessProfile.pendingGapFrames += 1;
        }
    }
    if (elapsedMs >= 40.0) {
        m_smoothnessProfile.severeGapFrames += 1;
    }

    if (!m_smoothnessProfile.incidentReported
        && smoothnessLooksIncident(
            m_smoothnessProfile.gapFrames,
            m_smoothnessProfile.severeGapFrames,
            m_smoothnessProfile.stallClusters,
            m_smoothnessProfile.regressionCount,
            m_smoothnessProfile.paintSpikeCount)) {
        const double rowsPerSecond = targetRowsPerSecondLocked();
        m_smoothnessProfile.incidentDetected = true;
        m_smoothnessProfile.incidentReported = true;
        FERROUS_SPECTROGRAM_LOGF(
            stderr,
            "[ui-spectrogram] smoothness_hitch_detected sample_rate_hz=%d rows_per_second=%.3f frames=%d pending_frames=%d gap_frames=%d severe_gap_frames=%d pending_gap_frames=%d stall_clusters=%d regressions=%d pending_max=%d drain_passes=%d drained=%d paint_spikes=%d max_gap_ms=%.3f advanced=%d\n",
            m_sampleRateHz,
            rowsPerSecond,
            m_smoothnessProfile.framesObserved,
            m_smoothnessProfile.pendingFrames,
            m_smoothnessProfile.gapFrames,
            m_smoothnessProfile.severeGapFrames,
            m_smoothnessProfile.pendingGapFrames,
            m_smoothnessProfile.stallClusters,
            m_smoothnessProfile.regressionCount,
            m_smoothnessProfile.maxPendingRows,
            m_smoothnessProfile.drainPasses,
            m_smoothnessProfile.drainedColumns,
            m_smoothnessProfile.paintSpikeCount,
            m_smoothnessProfile.maxGapMs,
            advanced ? 1 : 0);
    }
}

void SpectrogramItem::noteSmoothnessPaintLocked(double paintMs) {
    if (!m_smoothnessProfile.active) {
        return;
    }
    m_smoothnessProfile.paintSamples += 1;
    m_smoothnessProfile.paintMsTotal += paintMs;
    m_smoothnessProfile.maxPaintMs = std::max(m_smoothnessProfile.maxPaintMs, paintMs);
    if (paintMs >= 4.0) {
        m_smoothnessProfile.paintSpikeCount += 1;
    }
}

void SpectrogramItem::finalizeSmoothnessProfileLocked(qint64 nowMs, const char *reason) {
    if (!m_smoothnessProfile.active) {
        return;
    }
    const double rowsPerSecond = targetRowsPerSecondLocked();

    m_smoothnessProfile.incidentDetected = m_smoothnessProfile.incidentDetected
        || smoothnessLooksIncident(
            m_smoothnessProfile.gapFrames,
            m_smoothnessProfile.severeGapFrames,
            m_smoothnessProfile.stallClusters,
            m_smoothnessProfile.regressionCount,
            m_smoothnessProfile.paintSpikeCount);

    QVariantMap summary;
    summary.insert(QStringLiteral("active"), false);
    summary.insert(QStringLiteral("startedAtMs"), m_smoothnessProfile.startedAtMs);
    summary.insert(QStringLiteral("finishedAtMs"), nowMs);
    summary.insert(QStringLiteral("reason"), QString::fromUtf8(reason));
    summary.insert(QStringLiteral("sampleRateHz"), m_sampleRateHz);
    summary.insert(QStringLiteral("rowsPerSecond"), rowsPerSecond);
    summary.insert(QStringLiteral("framesObserved"), m_smoothnessProfile.framesObserved);
    summary.insert(QStringLiteral("pendingFrames"), m_smoothnessProfile.pendingFrames);
    summary.insert(QStringLiteral("stallFrames"), m_smoothnessProfile.stallFrames);
    summary.insert(QStringLiteral("stallClusters"), m_smoothnessProfile.stallClusters);
    summary.insert(QStringLiteral("gapFrames"), m_smoothnessProfile.gapFrames);
    summary.insert(QStringLiteral("severeGapFrames"), m_smoothnessProfile.severeGapFrames);
    summary.insert(QStringLiteral("pendingGapFrames"), m_smoothnessProfile.pendingGapFrames);
    summary.insert(QStringLiteral("maxGapMs"), m_smoothnessProfile.maxGapMs);
    summary.insert(QStringLiteral("regressionCount"), m_smoothnessProfile.regressionCount);
    summary.insert(QStringLiteral("drainPasses"), m_smoothnessProfile.drainPasses);
    summary.insert(QStringLiteral("drainedColumns"), m_smoothnessProfile.drainedColumns);
    summary.insert(QStringLiteral("maxPendingRows"), m_smoothnessProfile.maxPendingRows);
    summary.insert(QStringLiteral("paintSpikeCount"), m_smoothnessProfile.paintSpikeCount);
    summary.insert(QStringLiteral("maxPaintMs"), m_smoothnessProfile.maxPaintMs);
    summary.insert(
        QStringLiteral("avgPaintMs"),
        m_smoothnessProfile.paintSamples > 0
            ? (m_smoothnessProfile.paintMsTotal / static_cast<double>(m_smoothnessProfile.paintSamples))
            : 0.0);
    summary.insert(QStringLiteral("incidentDetected"), m_smoothnessProfile.incidentDetected);
    m_smoothnessProfile.lastSummary = summary;

    FERROUS_SPECTROGRAM_LOGF(
        stderr,
        "[ui-spectrogram] smoothness_window reason=%s sample_rate_hz=%d rows_per_second=%.3f frames=%d pending_frames=%d gap_frames=%d severe_gap_frames=%d pending_gap_frames=%d stall_clusters=%d regressions=%d drain_passes=%d drained=%d pending_max=%d paint_spikes=%d max_gap_ms=%.3f max_paint_ms=%.3f avg_paint_ms=%.3f incident=%d\n",
        reason,
        m_sampleRateHz,
        rowsPerSecond,
        m_smoothnessProfile.framesObserved,
        m_smoothnessProfile.pendingFrames,
        m_smoothnessProfile.gapFrames,
        m_smoothnessProfile.severeGapFrames,
        m_smoothnessProfile.pendingGapFrames,
        m_smoothnessProfile.stallClusters,
        m_smoothnessProfile.regressionCount,
        m_smoothnessProfile.drainPasses,
        m_smoothnessProfile.drainedColumns,
        m_smoothnessProfile.maxPendingRows,
        m_smoothnessProfile.paintSpikeCount,
        m_smoothnessProfile.maxGapMs,
        m_smoothnessProfile.maxPaintMs,
        m_smoothnessProfile.paintSamples > 0
            ? (m_smoothnessProfile.paintMsTotal / static_cast<double>(m_smoothnessProfile.paintSamples))
            : 0.0,
        m_smoothnessProfile.incidentDetected ? 1 : 0);

    const QVariantMap lastSummary = m_smoothnessProfile.lastSummary;
    resetSmoothnessProfileLocked();
    m_smoothnessProfile.lastSummary = lastSummary;
}

QVariantMap SpectrogramItem::debugSmoothnessProfileStateLocked() const {
    if (m_smoothnessProfile.active) {
        QVariantMap state;
        state.insert(QStringLiteral("active"), true);
        state.insert(QStringLiteral("startedAtMs"), m_smoothnessProfile.startedAtMs);
        state.insert(QStringLiteral("sampleRateHz"), m_sampleRateHz);
        state.insert(QStringLiteral("rowsPerSecond"), targetRowsPerSecondLocked());
        state.insert(QStringLiteral("framesObserved"), m_smoothnessProfile.framesObserved);
        state.insert(QStringLiteral("pendingFrames"), m_smoothnessProfile.pendingFrames);
        state.insert(QStringLiteral("stallFrames"), m_smoothnessProfile.stallFrames);
        state.insert(QStringLiteral("stallClusters"), m_smoothnessProfile.stallClusters);
        state.insert(QStringLiteral("gapFrames"), m_smoothnessProfile.gapFrames);
        state.insert(QStringLiteral("severeGapFrames"), m_smoothnessProfile.severeGapFrames);
        state.insert(QStringLiteral("pendingGapFrames"), m_smoothnessProfile.pendingGapFrames);
        state.insert(QStringLiteral("maxGapMs"), m_smoothnessProfile.maxGapMs);
        state.insert(QStringLiteral("regressionCount"), m_smoothnessProfile.regressionCount);
        state.insert(QStringLiteral("drainPasses"), m_smoothnessProfile.drainPasses);
        state.insert(QStringLiteral("drainedColumns"), m_smoothnessProfile.drainedColumns);
        state.insert(QStringLiteral("maxPendingRows"), m_smoothnessProfile.maxPendingRows);
        state.insert(QStringLiteral("paintSpikeCount"), m_smoothnessProfile.paintSpikeCount);
        state.insert(QStringLiteral("maxPaintMs"), m_smoothnessProfile.maxPaintMs);
        state.insert(
            QStringLiteral("avgPaintMs"),
            m_smoothnessProfile.paintSamples > 0
                ? (m_smoothnessProfile.paintMsTotal / static_cast<double>(m_smoothnessProfile.paintSamples))
                : 0.0);
        state.insert(QStringLiteral("incidentDetected"), m_smoothnessProfile.incidentDetected);
        return state;
    }
    return m_smoothnessProfile.lastSummary;
}

void SpectrogramItem::resetSeekProfileLocked() {
    m_seekProfile = SeekProfileState{};
}

void SpectrogramItem::maybeStartSeekProfileLocked(qint64 nowMs) {
    if (!m_profileEnabled) {
        return;
    }

    if (m_seekProfile.active && !SpectrogramSeekTrace::isActive(nowMs)) {
        finalizeSeekProfileLocked(nowMs, "expired");
    }

    const quint64 generation = SpectrogramSeekTrace::currentGeneration();
    if (generation == 0 || generation == m_seekProfile.generation || !SpectrogramSeekTrace::isActive(nowMs)) {
        return;
    }

    if (m_seekProfile.active) {
        finalizeSeekProfileLocked(nowMs, "superseded");
    }

    resetSeekProfileLocked();
    m_seekProfile.active = true;
    m_seekProfile.generation = generation;
    m_seekProfile.startedAtMs = SpectrogramSeekTrace::startedAtMs();
    m_seekProfile.targetSeconds = SpectrogramSeekTrace::targetSeconds();
}

void SpectrogramItem::noteSeekProfileFrameLocked(
    qint64 nowMs,
    double elapsedSeconds,
    bool pending,
    bool advanced) {
    if (!m_seekProfile.active) {
        return;
    }

    m_seekProfile.framesObserved += 1;
    m_seekProfile.lastFrameAtMs = nowMs;
    if (pending) {
        m_seekProfile.pendingFrames += 1;
    } else {
        m_seekProfile.inStallCluster = false;
    }

    const int canvasWidth = m_canvas.width() > 0 ? m_canvas.width() : std::max(1, static_cast<int>(std::floor(width())));
    const double headUnits = static_cast<double>(m_canvasWriteX);
    double unwrappedHeadUnits = headUnits;
    if (m_seekProfile.lastHeadValid && canvasWidth > 1) {
        if ((m_seekProfile.lastHeadUnits - unwrappedHeadUnits) > (static_cast<double>(canvasWidth) * 0.5)) {
            unwrappedHeadUnits += static_cast<double>(canvasWidth);
        }
        const double delta = unwrappedHeadUnits - m_seekProfile.lastHeadUnits;
        if (pending) {
            if (delta > 0.05) {
                m_seekProfile.sawForwardMotion = true;
                m_seekProfile.inStallCluster = false;
            } else if (m_seekProfile.sawForwardMotion) {
                m_seekProfile.stallFrames += 1;
                if (!m_seekProfile.inStallCluster) {
                    m_seekProfile.stallClusters += 1;
                    m_seekProfile.inStallCluster = true;
                }
            }
        }
        if (delta < -0.05) {
            m_seekProfile.regressionCount += 1;
        }
    }
    m_seekProfile.lastHeadUnits = unwrappedHeadUnits;
    m_seekProfile.lastHeadValid = true;

    const double elapsedMs = elapsedSeconds * 1000.0;
    if (pending && elapsedMs >= 25.0) {
        m_seekProfile.gapFrames += 1;
        m_seekProfile.maxGapMs = std::max(m_seekProfile.maxGapMs, elapsedMs);
    }

    if (!m_seekProfile.incidentReported
        && seekTraceLooksIncident(
            m_seekProfile.gapFrames,
            m_seekProfile.stallClusters,
            m_seekProfile.regressionCount)) {
        const double rowsPerSecond = targetRowsPerSecondLocked();
        m_seekProfile.incidentDetected = true;
        m_seekProfile.incidentReported = true;
        FERROUS_SPECTROGRAM_LOGF(
            stderr,
            "[ui-spectrogram] seek_hitch_detected gen=%llu target_s=%.3f sample_rate_hz=%d rows_per_second=%.3f gap_frames=%d stall_clusters=%d regressions=%d pending_max=%d drain_passes=%d drained=%d advanced=%d\n",
            static_cast<unsigned long long>(m_seekProfile.generation),
            m_seekProfile.targetSeconds,
            m_sampleRateHz,
            rowsPerSecond,
            m_seekProfile.gapFrames,
            m_seekProfile.stallClusters,
            m_seekProfile.regressionCount,
            m_seekProfile.maxPendingRows,
            m_seekProfile.drainPasses,
            m_seekProfile.drainedColumns,
            advanced ? 1 : 0);
    }

    const qint64 seekAgeMs = nowMs - m_seekProfile.startedAtMs;
    if (seekAgeMs >= kSeekTraceWindowMs || (!pending && seekAgeMs >= 150 && m_seekProfile.framesObserved >= 4)) {
        finalizeSeekProfileLocked(nowMs, pending ? "expired" : "settled");
    }
}

void SpectrogramItem::finalizeSeekProfileLocked(qint64 nowMs, const char *reason) {
    if (!m_seekProfile.active) {
        return;
    }
    const double rowsPerSecond = targetRowsPerSecondLocked();

    m_seekProfile.incidentDetected = m_seekProfile.incidentDetected
        || seekTraceLooksIncident(
            m_seekProfile.gapFrames,
            m_seekProfile.stallClusters,
            m_seekProfile.regressionCount);

    QVariantMap summary;
    summary.insert(QStringLiteral("active"), false);
    summary.insert(QStringLiteral("generation"), QVariant::fromValue(m_seekProfile.generation));
    summary.insert(QStringLiteral("targetSeconds"), m_seekProfile.targetSeconds);
    summary.insert(QStringLiteral("startedAtMs"), m_seekProfile.startedAtMs);
    summary.insert(QStringLiteral("finishedAtMs"), nowMs);
    summary.insert(QStringLiteral("reason"), QString::fromUtf8(reason));
    summary.insert(QStringLiteral("sampleRateHz"), m_sampleRateHz);
    summary.insert(QStringLiteral("rowsPerSecond"), rowsPerSecond);
    summary.insert(QStringLiteral("framesObserved"), m_seekProfile.framesObserved);
    summary.insert(QStringLiteral("pendingFrames"), m_seekProfile.pendingFrames);
    summary.insert(QStringLiteral("stallFrames"), m_seekProfile.stallFrames);
    summary.insert(QStringLiteral("stallClusters"), m_seekProfile.stallClusters);
    summary.insert(QStringLiteral("gapFrames"), m_seekProfile.gapFrames);
    summary.insert(QStringLiteral("maxGapMs"), m_seekProfile.maxGapMs);
    summary.insert(QStringLiteral("regressionCount"), m_seekProfile.regressionCount);
    summary.insert(QStringLiteral("drainPasses"), m_seekProfile.drainPasses);
    summary.insert(QStringLiteral("drainedColumns"), m_seekProfile.drainedColumns);
    summary.insert(QStringLiteral("maxPendingRows"), m_seekProfile.maxPendingRows);
    summary.insert(QStringLiteral("incidentDetected"), m_seekProfile.incidentDetected);
    m_seekProfile.lastSummary = summary;

    FERROUS_SPECTROGRAM_LOGF(
        stderr,
        "[ui-spectrogram] seek_hitch_window gen=%llu target_s=%.3f reason=%s sample_rate_hz=%d rows_per_second=%.3f frames=%d pending_frames=%d gap_frames=%d stall_clusters=%d regressions=%d drain_passes=%d drained=%d pending_max=%d incident=%d\n",
        static_cast<unsigned long long>(m_seekProfile.generation),
        m_seekProfile.targetSeconds,
        reason,
        m_sampleRateHz,
        rowsPerSecond,
        m_seekProfile.framesObserved,
        m_seekProfile.pendingFrames,
        m_seekProfile.gapFrames,
        m_seekProfile.stallClusters,
        m_seekProfile.regressionCount,
        m_seekProfile.drainPasses,
        m_seekProfile.drainedColumns,
        m_seekProfile.maxPendingRows,
        m_seekProfile.incidentDetected ? 1 : 0);

    const QVariantMap lastSummary = m_seekProfile.lastSummary;
    resetSeekProfileLocked();
    m_seekProfile.lastSummary = lastSummary;
}

QVariantMap SpectrogramItem::debugSeekProfileStateLocked() const {
    if (m_seekProfile.active) {
        QVariantMap state;
        state.insert(QStringLiteral("active"), true);
        state.insert(QStringLiteral("generation"), QVariant::fromValue(m_seekProfile.generation));
        state.insert(QStringLiteral("targetSeconds"), m_seekProfile.targetSeconds);
        state.insert(QStringLiteral("startedAtMs"), m_seekProfile.startedAtMs);
        state.insert(QStringLiteral("sampleRateHz"), m_sampleRateHz);
        state.insert(QStringLiteral("rowsPerSecond"), targetRowsPerSecondLocked());
        state.insert(QStringLiteral("framesObserved"), m_seekProfile.framesObserved);
        state.insert(QStringLiteral("pendingFrames"), m_seekProfile.pendingFrames);
        state.insert(QStringLiteral("stallFrames"), m_seekProfile.stallFrames);
        state.insert(QStringLiteral("stallClusters"), m_seekProfile.stallClusters);
        state.insert(QStringLiteral("gapFrames"), m_seekProfile.gapFrames);
        state.insert(QStringLiteral("maxGapMs"), m_seekProfile.maxGapMs);
        state.insert(QStringLiteral("regressionCount"), m_seekProfile.regressionCount);
        state.insert(QStringLiteral("drainPasses"), m_seekProfile.drainPasses);
        state.insert(QStringLiteral("drainedColumns"), m_seekProfile.drainedColumns);
        state.insert(QStringLiteral("maxPendingRows"), m_seekProfile.maxPendingRows);
        state.insert(QStringLiteral("incidentDetected"), m_seekProfile.incidentDetected);
        return state;
    }
    return m_seekProfile.lastSummary;
}
#endif

void SpectrogramItem::updateFpsEstimateLocked() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    if (!m_fpsInitialized) {
        m_fpsInitialized = true;
        m_lastFrameTime = now;
        m_fpsValue = 0;
        m_fpsAccumFrames = 0;
        m_fpsAccumSeconds = 0.0;
        m_overlayDirty = true;
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
    const int nextFps = std::clamp(static_cast<int>(std::lround(fps)), 0, 999);
    if (nextFps != m_fpsValue) {
        m_fpsValue = nextFps;
        m_overlayDirty = true;
    }
    m_fpsAccumFrames = 0;
    m_fpsAccumSeconds = 0.0;
}
