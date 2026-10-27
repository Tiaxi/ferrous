// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <functional>

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QLine>
#include <QMutex>
#include <QPainterPath>
#include <QPointF>
#include <QPointer>
#include <QPolygonF>
#include <QQuickItem>
#include <QRectF>
#include <QString>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <vector>

#include "PlaybackClock.h"

class QHoverEvent;
class QMouseEvent;
class QPainter;
class QQuickWindow;
class QWheelEvent;

class WaveformEditorItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString sourcePath READ sourcePath WRITE setSourcePath NOTIFY sourcePathChanged)
    Q_PROPERTY(QByteArray overviewData READ overviewData WRITE setOverviewData NOTIFY overviewDataChanged)
    Q_PROPERTY(double overviewCoverageSeconds READ overviewCoverageSeconds WRITE setOverviewCoverageSeconds NOTIFY overviewCoverageChanged)
    Q_PROPERTY(bool overviewComplete READ overviewComplete WRITE setOverviewComplete NOTIFY overviewCoverageChanged)
    // Where playback is; the view follows its position.
    Q_PROPERTY(PlaybackClock *clock READ clock WRITE setClock NOTIFY clockChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY positionSecondsChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds WRITE setDurationSeconds NOTIFY durationSecondsChanged)
    Q_PROPERTY(double zoomLevel READ zoomLevel WRITE setZoomLevel NOTIFY zoomLevelChanged)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(bool zoomEnabled READ zoomEnabled WRITE setZoomEnabled NOTIFY zoomEnabledChanged)
    Q_PROPERTY(bool gridEnabled READ gridEnabled WRITE setGridEnabled NOTIFY gridEnabledChanged)
    Q_PROPERTY(bool crosshairEnabled READ crosshairEnabled WRITE setCrosshairEnabled NOTIFY crosshairEnabledChanged)
    Q_PROPERTY(bool showFpsOverlay READ showFpsOverlay WRITE setShowFpsOverlay NOTIFY showFpsOverlayChanged)
    Q_PROPERTY(int viewMode READ viewMode WRITE setViewMode NOTIFY viewModeChanged)
    // Whether each channel, 0 to 63, is muted: a 64-bit mask would lose its
    // low bits as a QML number.
    Q_PROPERTY(QList<bool> mutedChannels READ mutedChannels WRITE setMutedChannels NOTIFY mutedChannelsChanged)
    Q_PROPERTY(int soloedChannel READ soloedChannel WRITE setSoloedChannel NOTIFY soloedChannelChanged)
    Q_PROPERTY(int channelCountHint READ channelCountHint WRITE setChannelCountHint NOTIFY channelCountHintChanged)
    Q_PROPERTY(int channelCount READ channelCount NOTIFY channelCountChanged)
    Q_PROPERTY(int sampleRateHz READ sampleRateHz NOTIFY sampleRateHzChanged)
    Q_PROPERTY(bool samplePointsVisible READ samplePointsVisible NOTIFY samplePointsVisibleChanged)

public:
    explicit WaveformEditorItem(QQuickItem *parent = nullptr);
    ~WaveformEditorItem() override;

    QString sourcePath() const;
    void setSourcePath(const QString &value);
    QByteArray overviewData() const;
    void setOverviewData(const QByteArray &value);
    double overviewCoverageSeconds() const;
    void setOverviewCoverageSeconds(double value);
    bool overviewComplete() const;
    void setOverviewComplete(bool value);
    PlaybackClock *clock() const;
    void setClock(PlaybackClock *value);
    double positionSeconds() const;
    double durationSeconds() const;
    void setDurationSeconds(double value);
    double zoomLevel() const;
    void setZoomLevel(double value);
    bool playing() const;
    void setPlaying(bool value);
    bool zoomEnabled() const;
    void setZoomEnabled(bool value);
    bool gridEnabled() const;
    void setGridEnabled(bool value);
    bool crosshairEnabled() const;
    void setCrosshairEnabled(bool value);
    bool showFpsOverlay() const;
    void setShowFpsOverlay(bool value);
    int viewMode() const;
    void setViewMode(int value);
    qulonglong mutedChannelsMask() const;
    void setMutedChannelsMask(qulonglong value);
    QList<bool> mutedChannels() const;
    void setMutedChannels(const QList<bool> &value);
    int soloedChannel() const;
    void setSoloedChannel(int value);
    int channelCountHint() const;
    void setChannelCountHint(int value);
    int channelCount() const;
    int sampleRateHz() const;
    bool samplePointsVisible() const;

    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE double maximumZoomLevel() const;
    Q_INVOKABLE QString formatViewportDuration(double seconds) const;
    Q_INVOKABLE void setHoverPosition(double x, double y, bool active);

    // Shared drawing reference used by the scene graph adapter and raster tests.
    void paint(QPainter *painter);

signals:
    void sourcePathChanged();
    void overviewDataChanged();
    void overviewCoverageChanged();
    void clockChanged();
    void positionSecondsChanged();
    void durationSecondsChanged();
    void zoomLevelChanged();
    void playingChanged();
    void zoomEnabledChanged();
    void gridEnabledChanged();
    void crosshairEnabledChanged();
    void showFpsOverlayChanged();
    void viewModeChanged();
    void mutedChannelsChanged();
    void soloedChannelChanged();
    void channelCountHintChanged();
    void channelCountChanged();
    void sampleRateHzChanged();
    void samplePointsVisibleChanged();
    void seekRequested(double seconds);

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void clockStateChanged();
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    struct ProfileState {
        bool enabled{false};
        bool frameInitialized{false};
        std::chrono::steady_clock::time_point lastSummary{};
        std::chrono::steady_clock::time_point lastFrame{};
        std::chrono::steady_clock::time_point lastFrameGapSpike{};
        std::chrono::steady_clock::time_point lastPaintSpike{};
        quint64 paints{0};
        quint64 directPaints{0};
        quint64 cachedPaints{0};
        quint64 cacheRebuilds{0};
        quint64 stagedCacheStarts{0};
        quint64 stagedCacheSwaps{0};
        quint64 detailRequests{0};
        quint64 detailHandoffs{0};
        double paintMs{0.0};
        double maximumPaintMs{0.0};
        double cacheRebuildMs{0.0};
        double maximumCacheRebuildMs{0.0};
        double lastCacheRebuildMs{0.0};
        double stagedCacheMs{0.0};
        double maximumStagedCacheMs{0.0};
        double lastStagedCacheMs{0.0};
        int lastStagedCacheColumns{0};
        double maximumFrameGapMs{0.0};
        double decodeMs{0.0};
        double maximumDecodeMs{0.0};
        quint64 tileRenders{0};
        double tileRenderMs{0.0};
        double maximumTileRenderMs{0.0};
    };
#endif

    struct DetailWindow {
        int sampleRateHz{0};
        int channelCount{0};
        double startSeconds{0.0};
        double endSeconds{0.0};
        quint32 framesPerPoint{0};
        int pointCount{0};
        std::vector<float> extrema;
        std::vector<float> downmixExtrema;
    };

    struct CacheGrid {
        double startSeconds{0.0};
        double endSeconds{0.0};
        double secondsPerPixel{0.0};
        int width{0};
    };

    struct PlaybackTile {
        QImage image;
    };

    struct PlaybackTilePaint {
        QImage image;
        QRectF target;
    };

    static QByteArray decodeWindow(
        const QString &path, double startSeconds, double endSeconds, int maxPoints, const std::shared_ptr<std::atomic_bool> &cancelled);
    static bool parseWindow(const QByteArray &bytes, DetailWindow *window);
    void scheduleDetailRequest(bool immediate = false);
    void requestDetailWindow();
    void clearDetailLocked();
    void clearPendingRequestLocked();
    // The playback position and the view at a time on the clock. A frame
    // reads the clock once, so that its view and playhead agree.
    double displayedPositionSecondsLocked(qint64 nowNs) const;
    double displayedPositionSecondsLocked() const;
    bool detailOrPendingRequestCoversLocked(double startSeconds, double endSeconds) const;
    bool detailCoversRangeLocked(double startSeconds, double endSeconds) const;
    bool detailResolutionCoversLocked(double startSeconds, double endSeconds) const;
    bool detailResolutionCoversPixelSpanLocked(
        double startSeconds, double endSeconds, int pixelWidth) const;
    std::pair<double, double> visibleRangeForZoomLocked(
        double zoomLevel, qint64 nowNs) const;
    std::pair<double, double> visibleRangeForZoomLocked(double zoomLevel) const;
    std::pair<double, double> visibleRangeLocked(qint64 nowNs) const;
    std::pair<double, double> visibleRangeLocked() const;
    std::pair<double, double> detailRequestVisibleRangeLocked() const;
    std::pair<double, double> requestRangeLocked(
        double visibleStart, double visibleEnd) const;
    double requiredVisibleDetailPointsLocked(double visibleSpan) const;
    double detailRequestMarginLocked(double visibleSpan) const;
    int detailRequestPointCountLocked(double requestStart, double requestEnd) const;
    // Device pixels to a logical pixel, and the caches' size in them.
    double renderScaleLocked() const;
    int renderPixelWidthLocked() const;
    int renderPixelHeightLocked() const;
    CacheGrid cacheGridForRangeLocked(
        double visibleStart, double visibleEnd,
        double sourceStart, double sourceEnd) const;
    double maximumZoomLevelLocked() const;
    bool clampZoomToMaximumLocked();
    double sampleSpacingPixelsLocked(
        int pixelWidth, double visibleStart, double visibleEnd) const;
    double sampleSpacingPixelsLocked() const;
    bool sampleCurveRequestedForPixelSpanLocked(
        int pixelWidth, double visibleStart, double visibleEnd) const;
    bool sampleCurveVisibleLocked() const;
    bool samplePointsVisibleLocked() const;
    bool renderDetailDirectlyLocked(double visibleStart, double visibleEnd) const;
    bool detailResultRequiresImmediateCacheRefreshLocked(
        bool firstDetail, bool channelsChanged, bool zoomChanged,
        bool replaceOverviewFallback, quint32 previousFramesPerPoint,
        bool detailReady) const;
    double detailPointTimeLocked(int point) const;
    std::pair<int, int> detailPointRangeLocked(
        double visibleStart, double visibleEnd) const;
    void updateFpsEstimateLocked();
    void bindWindowFrameLoop(QQuickWindow *window);
    void handleWindowFrameSwapped();
    // How long until a frame of playback would differ from the last, in
    // nanoseconds from nowNs: zero for now, or -1 for not while the clock
    // keeps its course.
    qint64 playbackFrameDelayNsLocked(qint64 nowNs) const;
    // Where playback makes a frame of this view, with the playhead at
    // `position`, out of date: the playhead's next pixel, or the view
    // starting to follow it. NaN while the view moves with playback, and
    // infinity if it never does.
    double frameChangeSecondsLocked(
        double visibleStart, double visibleEnd, double position, int width) const;
    // Asks for the next frame of playback, now or once it would differ.
    void schedulePlaybackFrame(qint64 delayNs);
    void requestPlaybackFrame();
    bool cacheCoversRangeLocked(double start, double end) const;
    void invalidateCacheLocked();
    void clearStagedCacheLocked();
    void beginStagedCacheLocked();
    void beginStagedCacheForRangeLocked(
        double visibleStart, double visibleEnd, bool commitsDeferredZoom);
    bool advanceStagedCacheLocked(qint64 nowNs);
    bool advanceStagedCacheLocked();
    void queueGuiContinuationFromPaint(
        bool notifySamplePointsChanged, bool requestRepaint);
    void rebuildCacheLocked(
        int width, int height, double visibleStart, double visibleEnd);
    void rebuildCacheLocked(int width, int height);
    bool playbackTilesEligibleLocked(
        double visibleStart, double visibleEnd) const;
    void clearPlaybackTilesLocked();
    void preparePlaybackTilesLocked(
        double visibleStart, double visibleEnd, int height);
    int renderMissingPlaybackTilesLocked(
        double visibleStart, double visibleEnd, int height, int tileBudget);
    int playbackTileRenderBudgetLocked(
        double visibleStart, double visibleEnd) const;
    bool playbackTilesCoverLocked(
        double visibleStart, double visibleEnd) const;
    std::vector<PlaybackTilePaint> playbackTilePaintsLocked(
        double visibleStart, double visibleEnd, int canvasWidth, int canvasHeight) const;
    void drawGridLocked(QPainter &painter, int width, int height,
                        double visibleStart, double visibleEnd, int channels) const;
    static void drawChannelSeparators(
        QPainter &painter, int width, int height, int channels);
    static void drawContrastingLine(
        QPainter &painter, const QLineF &line,
        const QColor &foreground, const QColor &contrast);
    static QPainterPath buildSamplePath(const QPolygonF &samples);
    static QString formatCrosshairTime(double seconds);
    static std::pair<QRect, QRect> crosshairLabelRects(
        int width, int height, int x, int y,
        int valueTextWidth, int timeTextWidth, int textHeight);
    // These draw in the painter's pixels, pixelScale to a logical pixel.
    void drawOverviewLocked(QPainter &painter, int width, int height,
                            double visibleStart, double visibleEnd, int channels,
                            double pixelScale = 1.0) const;
    void drawDetailLocked(QPainter &painter, int width, int height,
                          double visibleStart, double visibleEnd, int channels,
                          double pixelScale = 1.0) const;
    void drawDetailSliceLocked(QPainter &painter, int width, int height,
                               double visibleStart, double visibleEnd, int channels,
                               int firstX, int lastX, double pixelScale = 1.0) const;
    void drawCrosshair(QPainter &painter, int width, int height,
                       double visibleStart, double visibleEnd) const;
    static void drawFpsOverlay(QPainter &painter, int width, int fpsValue);
    int displayedChannelCountLocked() const;
    bool channelIsMutedLocked(int channel) const;

    mutable QMutex m_stateMutex;
    QString m_sourcePath;
    QByteArray m_overviewData;
    double m_overviewCoverageSeconds{0.0};
    bool m_overviewComplete{false};
    QPointer<PlaybackClock> m_clock;
    // Now on the clock playback is stamped with; tests set their own.
    std::function<qint64()> m_nowNs{monotonicNowNs};
    QMetaObject::Connection m_clockConnection;
    // A copy of the clock's state, which the render thread evaluates.
    PlaybackClockState m_clockState;
    double m_durationSeconds{0.0};
    double m_zoomLevel{1.0};
    double m_presentedZoomLevel{1.0};
    bool m_zoomOutHandoffPending{false};
    bool m_playing{false};
    bool m_zoomEnabled{true};
    bool m_gridEnabled{false};
    bool m_crosshairEnabled{false};
    bool m_showFpsOverlay{false};
    int m_viewMode{0};
    qulonglong m_mutedChannelsMask{0};
    int m_soloedChannel{-1};
    int m_channelCountHint{1};
    int m_channelCount{0};
    int m_sampleRateHz{0};
    bool m_hoverActive{false};
    QPointF m_hoverPosition;
    DetailWindow m_detail;
    bool m_zoomFallbackToOverview{false};
    // The cache, the staged cache and the playback tiles are in device
    // pixels.
    QImage m_cache;
    bool m_cacheDirty{true};
    int m_cachedViewportWidth{0};
    int m_cachedViewportHeight{0};
    double m_cacheStartSeconds{0.0};
    double m_cacheEndSeconds{0.0};
    double m_cacheSecondsPerPixel{0.0};
    quint32 m_cacheFramesPerPoint{0};
    int m_cacheDisplayedChannels{0};
    QImage m_stagedCache;
    double m_stagedCacheStartSeconds{0.0};
    double m_stagedCacheEndSeconds{0.0};
    double m_stagedCacheSecondsPerPixel{0.0};
    quint32 m_stagedCacheFramesPerPoint{0};
    int m_stagedCacheDisplayedChannels{0};
    int m_stagedCacheNextX{0};
    // The viewport the staged cache is for, in device pixels.
    int m_stagedCacheViewportWidth{0};
    int m_stagedCacheViewportHeight{0};
    bool m_stagedCacheCommitsDeferredZoom{false};
    std::map<qint64, PlaybackTile> m_playbackTiles;
    double m_playbackTileSecondsPerPixel{0.0};
    int m_playbackTileHeight{0};
    quint32 m_playbackTileFramesPerPoint{0};
    int m_playbackTileDisplayedChannels{0};
    void invalidateDetailRequestLocked();
    std::shared_ptr<std::atomic_bool> m_decodeCancelled;
    bool m_decodeActive{false};
    std::function<QByteArray(const QString &, double, double, int,
                            const std::shared_ptr<std::atomic_bool> &)> m_decodeWindow{decodeWindow};
    quint64 m_requestGeneration{0};
    bool m_requestInFlight{false};
    double m_requestedStartSeconds{0.0};
    double m_requestedEndSeconds{0.0};
    int m_requestedMaxPoints{0};
    int m_requestedRenderWidth{0};
    bool m_fpsInitialized{false};
    int m_fpsValue{0};
    int m_fpsAccumFrames{0};
    double m_fpsAccumSeconds{0.0};
    std::chrono::steady_clock::time_point m_lastFrameTime;
    QMetaObject::Connection m_frameSwappedConnection;
    QMetaObject::Connection m_windowVisibilityConnection;
    QTimer m_requestTimer;
    // Wakes the next playback frame of a view that stays still.
    QTimer m_playbackFrameTimer;
    // How many playback frames the item has asked for, for tests.
    quint64 m_playbackFrameRequests{0};
    // frameChangeSecondsLocked of the frame painted last; NaN, moving,
    // until the first.
    double m_paintedFrameChangeSeconds{std::numeric_limits<double>::quiet_NaN()};
    std::atomic_bool m_guiContinuationQueued{false};
    std::atomic_bool m_guiRepaintPending{false};
    std::atomic_bool m_samplePointsNotificationPending{false};
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    ProfileState m_profile;
#endif
};
