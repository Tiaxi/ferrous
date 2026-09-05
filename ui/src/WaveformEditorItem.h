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
#include <QPolygonF>
#include <QQuickPaintedItem>
#include <QRectF>
#include <QString>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <map>
#include <vector>

class QHoverEvent;
class QMouseEvent;
class QPainter;
class QQuickWindow;
class QWheelEvent;

class WaveformEditorItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString sourcePath READ sourcePath WRITE setSourcePath NOTIFY sourcePathChanged)
    Q_PROPERTY(QByteArray overviewData READ overviewData WRITE setOverviewData NOTIFY overviewDataChanged)
    Q_PROPERTY(double overviewCoverageSeconds READ overviewCoverageSeconds WRITE setOverviewCoverageSeconds NOTIFY overviewCoverageChanged)
    Q_PROPERTY(bool overviewComplete READ overviewComplete WRITE setOverviewComplete NOTIFY overviewCoverageChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds WRITE setPositionSeconds NOTIFY positionSecondsChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds WRITE setDurationSeconds NOTIFY durationSecondsChanged)
    Q_PROPERTY(double zoomLevel READ zoomLevel WRITE setZoomLevel NOTIFY zoomLevelChanged)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(bool zoomEnabled READ zoomEnabled WRITE setZoomEnabled NOTIFY zoomEnabledChanged)
    Q_PROPERTY(bool gridEnabled READ gridEnabled WRITE setGridEnabled NOTIFY gridEnabledChanged)
    Q_PROPERTY(bool crosshairEnabled READ crosshairEnabled WRITE setCrosshairEnabled NOTIFY crosshairEnabledChanged)
    Q_PROPERTY(bool showFpsOverlay READ showFpsOverlay WRITE setShowFpsOverlay NOTIFY showFpsOverlayChanged)
    Q_PROPERTY(int viewMode READ viewMode WRITE setViewMode NOTIFY viewModeChanged)
    Q_PROPERTY(qulonglong mutedChannelsMask READ mutedChannelsMask WRITE setMutedChannelsMask NOTIFY mutedChannelsMaskChanged)
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
    double positionSeconds() const;
    void setPositionSeconds(double value);
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
    int soloedChannel() const;
    void setSoloedChannel(int value);
    int channelCountHint() const;
    void setChannelCountHint(int value);
    int channelCount() const;
    int sampleRateHz() const;
    bool samplePointsVisible() const;

    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE void applyExplicitSeekPosition(double seconds);
    Q_INVOKABLE double maximumZoomLevel() const;
    Q_INVOKABLE QString formatViewportDuration(double seconds) const;
    Q_INVOKABLE void setHoverPosition(double x, double y, bool active);

    void paint(QPainter *painter) override;

signals:
    void sourcePathChanged();
    void overviewDataChanged();
    void overviewCoverageChanged();
    void positionSecondsChanged();
    void durationSecondsChanged();
    void zoomLevelChanged();
    void playingChanged();
    void zoomEnabledChanged();
    void gridEnabledChanged();
    void crosshairEnabledChanged();
    void showFpsOverlayChanged();
    void viewModeChanged();
    void mutedChannelsMaskChanged();
    void soloedChannelChanged();
    void channelCountHintChanged();
    void channelCountChanged();
    void sampleRateHzChanged();
    void samplePointsVisibleChanged();
    void seekRequested(double seconds);

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void updatePositionSeconds(double seconds, bool explicitSeek);
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
    void scheduleDetailRequest();
    void requestDetailWindow();
    void clearDetailLocked();
    void clearPendingRequestLocked();
    double displayedPositionSecondsLocked() const;
    bool detailOrPendingRequestCoversLocked(double startSeconds, double endSeconds) const;
    bool detailCoversRangeLocked(double startSeconds, double endSeconds) const;
    bool detailResolutionCoversLocked(double startSeconds, double endSeconds) const;
    bool detailResolutionCoversPixelSpanLocked(
        double startSeconds, double endSeconds, int pixelWidth) const;
    std::pair<double, double> visibleRangeForZoomLocked(double zoomLevel) const;
    std::pair<double, double> visibleRangeLocked() const;
    std::pair<double, double> detailRequestVisibleRangeLocked() const;
    std::pair<double, double> requestRangeLocked(
        double visibleStart, double visibleEnd) const;
    double requiredVisibleDetailPointsLocked(double visibleSpan) const;
    double detailRequestMarginLocked(double visibleSpan) const;
    int detailRequestPointCountLocked(double requestStart, double requestEnd) const;
    int renderPixelWidthLocked() const;
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
    void invalidateCacheLocked();
    void clearStagedCacheLocked();
    void beginStagedCacheLocked();
    void beginStagedCacheForRangeLocked(
        double visibleStart, double visibleEnd, bool commitsDeferredZoom);
    bool advanceStagedCacheLocked();
    void queueGuiContinuationFromPaint(
        bool notifySamplePointsChanged, bool requestRepaint);
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
        double visibleStart, double visibleEnd, int canvasWidth) const;
    void drawGridLocked(QPainter &painter, int width, int height,
                        double visibleStart, double visibleEnd, int channels) const;
    static void drawChannelSeparators(
        QPainter &painter, int width, int height, int channels);
    static void drawContrastingLine(
        QPainter &painter, const QLine &line,
        const QColor &foreground, const QColor &contrast);
    static QPainterPath buildSamplePath(const QPolygonF &samples);
    static QString formatCrosshairTime(double seconds);
    static std::pair<QRect, QRect> crosshairLabelRects(
        int width, int height, int x, int y,
        int valueTextWidth, int timeTextWidth, int textHeight);
    void drawOverviewLocked(QPainter &painter, int width, int height,
                            double visibleStart, double visibleEnd, int channels) const;
    void drawDetailLocked(QPainter &painter, int width, int height,
                          double visibleStart, double visibleEnd, int channels) const;
    void drawDetailSliceLocked(QPainter &painter, int width, int height,
                               double visibleStart, double visibleEnd, int channels,
                               int firstX, int lastX) const;
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
    double m_positionSeconds{0.0};
    std::chrono::steady_clock::time_point m_positionUpdatedAt;
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
    std::atomic_bool m_guiContinuationQueued{false};
    std::atomic_bool m_guiRepaintPending{false};
    std::atomic_bool m_samplePointsNotificationPending{false};
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    ProfileState m_profile;
#endif
};
