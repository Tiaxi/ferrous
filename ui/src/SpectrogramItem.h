// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QLineF>
#include <QMetaObject>
#include <QMutex>
#include <QQuickItem>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QTimer>

#include "SpectrogramSourceTexture.h"

#include <array>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

class QQuickWindow;
class QSGNode;
class QWheelEvent;

class SpectrogramItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(double dbRange READ dbRange WRITE setDbRange NOTIFY dbRangeChanged)
    Q_PROPERTY(bool logScale READ logScale WRITE setLogScale NOTIFY logScaleChanged)
    Q_PROPERTY(bool showFpsOverlay READ showFpsOverlay WRITE setShowFpsOverlay NOTIFY showFpsOverlayChanged)
    Q_PROPERTY(bool forceFpsOverlay READ forceFpsOverlay CONSTANT)
    Q_PROPERTY(int sampleRateHz READ sampleRateHz WRITE setSampleRateHz NOTIFY sampleRateHzChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds WRITE setPositionSeconds NOTIFY positionSecondsChanged)
    Q_PROPERTY(bool playing READ isPlaying WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(bool precomputedReady READ precomputedReady NOTIFY precomputedReadyChanged)
    Q_PROPERTY(int displayMode READ displayMode WRITE setDisplayMode NOTIFY displayModeChanged)
    Q_PROPERTY(bool crosshairEnabled READ crosshairEnabled WRITE setCrosshairEnabled NOTIFY crosshairEnabledChanged)
    Q_PROPERTY(bool gridEnabled READ gridEnabled WRITE setGridEnabled NOTIFY gridEnabledChanged)
    Q_PROPERTY(bool showTimeLabels READ showTimeLabels WRITE setShowTimeLabels NOTIFY showTimeLabelsChanged)
    Q_PROPERTY(double crosshairSharedX READ crosshairSharedX WRITE setCrosshairSharedX NOTIFY crosshairSharedXChanged)
    Q_PROPERTY(bool channelMuted READ channelMuted WRITE setChannelMuted NOTIFY channelMutedChanged)
    Q_PROPERTY(double zoomLevel READ zoomLevel WRITE setZoomLevel NOTIFY zoomLevelChanged)
    Q_PROPERTY(bool zoomEnabled READ zoomEnabled WRITE setZoomEnabled NOTIFY zoomEnabledChanged)

public:
    explicit SpectrogramItem(QQuickItem *parent = nullptr);

    double dbRange() const;
    void setDbRange(double value);

    bool logScale() const;
    void setLogScale(bool value);

    bool showFpsOverlay() const;
    void setShowFpsOverlay(bool value);
    bool forceFpsOverlay() const;

    int sampleRateHz() const;
    void setSampleRateHz(int value);

    double positionSeconds() const;
    void setPositionSeconds(double value);

    bool isPlaying() const;
    void setPlaying(bool value);

    bool precomputedReady() const;

    int displayMode() const;
    void setDisplayMode(int value);

    bool crosshairEnabled() const;
    void setCrosshairEnabled(bool value);

    bool gridEnabled() const;
    void setGridEnabled(bool value);

    bool showTimeLabels() const;
    void setShowTimeLabels(bool value);

    double crosshairSharedX() const;
    void setCrosshairSharedX(double value);

    bool channelMuted() const;
    void setChannelMuted(bool muted);

    double zoomLevel() const;
    void setZoomLevel(double value);

    bool zoomEnabled() const;
    void setZoomEnabled(bool value);

    double pixelToFrequencyHz(int pixelY, int viewHeight) const;

    Q_INVOKABLE void feedPrecomputedChunk(
        const QByteArray &data, int bins, int channelIndex,
        int columns, int startIndex, int totalEstimate,
        int sampleRate, int hopSize, bool complete,
        bool bufferReset, quint64 trackToken = 0,
        bool clearHistoryOnReset = false,
        quint64 generation = 0);
    Q_INVOKABLE void clearPrecomputed();

    Q_INVOKABLE void reset();
    Q_INVOKABLE void halt();

    Q_INVOKABLE double minimumZoomLevel() const;

signals:
    void dbRangeChanged();
    void logScaleChanged();
    void showFpsOverlayChanged();
    void sampleRateHzChanged();
    void positionSecondsChanged();
    void playingChanged();
    void precomputedReadyChanged();
    void displayModeChanged();
    void crosshairEnabledChanged();
    void gridEnabledChanged();
    void showTimeLabelsChanged();
    void crosshairSharedXChanged();
    void crosshairHoverChanged(double x);
    void channelMutedChanged();
    void seekRequested(double seconds);
    void zoomLevelChanged();
    void zoomEnabledChanged();
    void zoomRequested(double newZoomLevel);
    void zoomResetRequested();
    void backendZoomRequested(float zoomLevel);
    void spectrogramWidthChanged(int width);

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void releaseResources() override;

private:
    static constexpr int kGradientTableSize = 2048;
    static constexpr int kRetainedSourcePageWidth = 2048;

    struct RetainedRenderSegment {
        QRect sourceRect;
        QRectF targetRect;
        bool valid{false};
    };

    struct RetainedRenderPlan {
        QVector<RetainedRenderSegment> segments;
        bool haveSegments{false};
        bool overflowed{false};
        bool coversRange{false};
    };

    struct RetainedPageSpan {
        qint64 pageIndex{0};
        int pageXBegin{0};
        int pageXEnd{-1};
        qint64 displayLeft{0};
        qint64 displayRight{-1};
    };

    struct RetainedPageUploadSpan {
        qint64 pageIndex{0};
        int pageXBegin{0};
        int pageXEnd{-1};
        qint64 displayLeft{0};
        qint64 displayRight{-1};
        bool resetPage{false};
    };

    struct RetainedSourcePageKey {
        quint64 trackToken{0};
        bool rollingMode{false};
        qint64 pageIndex{0};
    };

    struct RetainedSourcePageState {
        RetainedSourcePageKey key;
        SpectrogramSourceTexture texture;
        int width{0};
        int binsPerColumn{0};
        qint64 uploadedDisplayLeft{0};
        qint64 uploadedDisplayRight{-1};
        QVector<SpectrogramSourceUploadSpan> dirtySpans;
    };

    void rebuildPalette();
    void invalidateMapping();
    void ensureMapping(int height);
    bool ensureZoomFillSnapshotImageLocked();
    void captureZoomFillFrozenCacheLocked();
    void clearZoomFillFrozenCacheLocked();
    void invalidateRetainedSourceTextureStateLocked();
    void invalidateRenderCachesLocked();
    bool reanchorRollingEpochForCurrentDataLocked(
        std::chrono::steady_clock::time_point now);
    void applyPrecomputedResetLocked(
        int startIndex,
        int bins,
        quint64 trackToken,
        quint64 generation,
        bool clearHistoryOnReset);
    qint64 currentRollingDisplayRightLocked(std::chrono::steady_clock::time_point now) const;
    void truncateRollingTailLocked(qint64 newWriteSeq);
    double currentRenderPositionSecondsLocked(std::chrono::steady_clock::time_point now) const;
    void setPositionAnchorLocked(double value, std::chrono::steady_clock::time_point now);
    void syncPositionAnchorLocked(std::chrono::steady_clock::time_point now);
    qint64 firstMissingCenteredDisplayColumnLocked(qint64 displayLeft, qint64 displayRight) const;
    int ringSlotForDisplayIndexLocked(
        qint64 displayIndex,
        bool rollingMode) const;
    std::array<quint8, 256> buildPrecomputedDbRemapLocked() const;
    QImage buildPrecomputedDbRemapImageLocked() const;
    QImage buildPrecomputedPaletteImageLocked() const;
    QImage buildRowLutImageLocked(int height) const;
    bool retainedSourceTextureStateChangedLocked(
        QQuickWindow *window,
        const QSize &retainedTextureSize,
        bool rollingMode) const;
    QVector<SpectrogramSourceUploadSpan> collectRetainedSourceUploadSpansLocked(
        qint64 displayLeft,
        qint64 displayRight,
        bool rollingMode,
        bool forceFullUpload = false) const;
    SpectrogramRetainedSourceLayout buildRetainedSourceLayoutLocked(
        qint64 displayLeft,
        qint64 displayRight,
        bool rollingMode) const;
    QVector<RetainedPageSpan> buildRetainedPageSpansLocked(
        qint64 displayLeft,
        qint64 displayRight,
        qint64 renderableRight,
        bool rollingMode,
        int pageWidth = kRetainedSourcePageWidth) const;
    QVector<RetainedPageUploadSpan> collectRetainedPageUploadSpansLocked(
        qint64 displayLeft,
        qint64 displayRight,
        qint64 renderableRight,
        bool rollingMode,
        int pageWidth = kRetainedSourcePageWidth) const;
    RetainedSourcePageState *findRetainedSourcePageLocked(
        quint64 trackToken,
        bool rollingMode,
        qint64 pageIndex) const;
    RetainedRenderPlan buildRetainedRenderPlanLocked(
        qint64 displayLeft,
        qint64 displayRight,
        qint64 renderableRight,
        double drawX,
        int drawPixels,
        int height,
        bool rollingMode) const;
    void syncRetainedSourceTextureLocked(
        QQuickWindow *window,
        qint64 displayLeft,
        qint64 displayRight,
        bool rollingMode);
    void syncRetainedSourcePagesLocked(
        QQuickWindow *window,
        qint64 displayLeft,
        qint64 displayRight,
        qint64 renderableRight,
        bool rollingMode);
    void syncPrecomputedGpuLookupImagesLocked();
    void syncGpuRowLutImageLocked(int height);
    void syncPrecomputedDisplayImageLocked(
        int drawPixels,
        int height,
        qint64 displayLeft,
        qint64 displayRight,
        bool rollingMode,
        double effectiveZoom,
        bool maintainImagePixels = true);
    void paintPrecomputedDisplayColumnsLocked(
        QImage &image,
        int xBegin,
        int xEnd,
        int drawPixels,
        int filledPixels,
        int height,
        qint64 displayLeft,
        qint64 displayRight,
        bool rollingMode,
        const std::array<quint8, 256> &dbRemap,
        const QRgb *palette) const;
    int gpuMaxTextureColumns(QQuickWindow *window) const;
    int retainedSourcePageWidthForGpuLimit(QQuickWindow *window) const;
    bool retainedSourceTextureFitsGpuLimit(
        QQuickWindow *window,
        int capacityColumns,
        int binsPerColumn) const;
    int gpuDrawWidthForSourceColumnsLocked(
        int width,
        qint64 displayLeft,
        qint64 displayRight,
        double effectiveZoom) const;
    void updateOverlayImageLocked();
    void updateCrosshairOverlayLocked(
        int width, int height,
        qint64 displayLeft, bool rollingMode,
        double columnsPerSecond, double drawX, bool drawLinePrimitives);
    void updateFreqGridOverlayLocked(int width, int height, bool drawLinePrimitives);
    void updateTimeGridOverlayLocked(
        int width, int height, int padding,
        qint64 displayLeft,
        bool rollingMode, double columnsPerSecond, double drawX, bool drawLinePrimitives);
    QVector<QLineF> buildFreqGridLinesLocked(int width, int height) const;
    QVector<QLineF> buildTimeGridLinesLocked(
        int width,
        int height,
        qint64 displayLeft,
        bool rollingMode,
        double columnsPerSecond,
        double drawX) const;
    std::optional<QLineF> buildPlayheadVerticalLineLocked(int pixelX, int height) const;
    std::optional<QLineF> buildCrosshairVerticalLineLocked(int width, int height) const;
    std::optional<QLineF> buildCrosshairHorizontalLineLocked(int width, int height) const;
    double effectiveZoomLocked() const;
    double minimumZoomLevelLocked() const;
    double pixelToFrequencyHzLocked(int pixelY, int viewHeight) const;
    int frequencyToPixelYLocked(double freqHz, int viewHeight) const;
    void bindWindowFpsTracking(QQuickWindow *window);
    void handleWindowAfterAnimating();
    void maybeDumpGpuDebugFrame();
    void updateFpsEstimateLocked();
    double targetRowsPerSecondLocked() const;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    void resetSeekProfileLocked();
    void maybeStartSeekProfileLocked(qint64 nowMs);
    void noteSeekProfileFrameLocked(qint64 nowMs, double elapsedSeconds, bool pending, bool advanced);
    void finalizeSeekProfileLocked(qint64 nowMs, const char *reason);
    QVariantMap debugSeekProfileStateLocked() const;
    void resetSmoothnessProfileLocked();
    void maybeStartSmoothnessProfileLocked(qint64 nowMs);
    void noteSmoothnessServoLocked(double errorSeconds, bool regressedDuringPlayback);
    void noteSmoothnessProfileFrameLocked(qint64 nowMs, double elapsedSeconds, bool pending, bool advanced);
    void noteSmoothnessAdvanceFallbackLocked(double effectiveZoom);
    void noteSmoothnessPaintLocked(double paintMs);
    void finalizeSmoothnessProfileLocked(qint64 nowMs, const char *reason);
    QVariantMap debugSmoothnessProfileStateLocked() const;
#endif

    double m_dbRange{132.0};
    bool m_logScale{false};
    int m_sampleRateHz{48000};
    int m_binsPerColumn{0};

    std::array<QRgb, kGradientTableSize> m_palette32{};
    std::array<QRgb, kGradientTableSize> m_palette32Gray{};
    bool m_channelMuted{false};
    std::vector<int> m_iToBin;
    int m_mappingHeight{-1};
    int m_lowResEnd{0};

    QImage m_overlayImage;
    bool m_overlayDirty{true};
    bool m_animationTickInitialized{false};
    std::chrono::steady_clock::time_point m_lastAnimationTick{};
    bool m_forceFpsOverlay{false};
    bool m_showFpsOverlay{false};
    bool m_fpsInitialized{false};
    int m_fpsValue{0};
    int m_fpsAccumFrames{0};
    double m_fpsAccumSeconds{0.0};
    int m_gpuDumpFramesRemaining{0};
    int m_gpuDumpFrameSerial{0};
    QString m_gpuDumpDirectory;
    std::chrono::steady_clock::time_point m_lastFrameTime{};
    bool m_profileEnabled{false};
    std::chrono::steady_clock::time_point m_profileLast{};
    std::chrono::steady_clock::time_point m_profileLastAppendSpike{};
    std::chrono::steady_clock::time_point m_profileLastFrameGapSpike{};
    std::chrono::steady_clock::time_point m_profileLastPaintSpike{};
    std::chrono::steady_clock::time_point m_profileLastWriteHeadClampSpike{};
    std::chrono::steady_clock::time_point m_profileLastServoSpike{};
    std::chrono::steady_clock::time_point m_profileLastAdvanceFallbackSpike{};
    quint64 m_profilePaints{0};
    double m_profilePaintMs{0.0};
    quint64 m_sceneGraphGeneration{0};
    QMetaObject::Connection m_animationTickConnection;
    QMetaObject::Connection m_windowVisibilityConnection;

    // Ring-buffer spectrogram storage.
    QByteArray m_ringBuffer;          // capacity * bins_per_column bytes
    std::vector<qint32> m_ringColumnId; // per write-order slot: track column index, or -1
    std::vector<qint64> m_ringSequenceId; // per slot: write sequence stored there, or -1
    std::vector<quint64> m_ringTrackToken; // per slot: track token stored there, or 0
    QHash<quint64, QHash<qint32, qint64>> m_trackColumnToSeqByToken;
    int m_ringCapacity{0};            // number of column slots
    // Max widget width ever observed ACROSS ALL SpectrogramItem
    // instances (static).  The Rust-side max-widget-width tracker that
    // sizes the decoder lookahead is a singleton in AnalysisRuntimeState
    // and persists across instance tear-downs (e.g. when the channel
    // count changes — 6ch PerChannel → 2ch PerChannel destroys the
    // prior widgets and creates fresh ones).  A per-instance tracker
    // would reset to 0 on each new widget, under-sizing the ring
    // relative to the still-large Rust lookahead and letting the
    // decoder lap the ring, evicting left-margin cols and smearing the
    // previous canvas through the blanked region.
    static int s_maxWidgetWidthSeen;
    qint64 m_ringWriteSeq{0};         // next write-order sequence number
    qint64 m_ringOldestSeq{0};        // oldest write-order sequence still retained
    qint64 m_trackEpochSeq{0};        // legacy reset bookkeeping
    qint64 m_rollingEpoch{0};         // maps track columns to rolling write-order history
    qint32 m_precomputedMaxColumnIndex{-1}; // highest column for current token
    bool m_awaitingWorkerReset{false};    // block data until worker's proper reset
    int m_precomputedBinsPerColumn{0};
    int m_precomputedTotalColumnsEstimate{0};
    int m_precomputedSampleRateHz{44100};
    int m_precomputedHopSize{1024};
    quint64 m_precomputedTrackToken{0};
    quint64 m_precomputedCommittedToken{0}; // token from the most recent buffer_reset
    quint64 m_precomputedSessionGeneration{0};
    quint64 m_precomputedCommittedGeneration{0};
    bool m_precomputedReady{false};
    double m_positionSeconds{0.0};
    double m_positionAnchorSeconds{0.0};
    std::chrono::steady_clock::time_point m_positionAnchorUpdatedAt{};
    bool m_positionAnchorInitialized{false};
    bool m_playing{false};
    bool m_positionJumpHoldActive{false};
    double m_positionJumpHoldSeconds{0.0};
    std::chrono::steady_clock::time_point m_positionJumpHoldStartedAt{};
    bool m_precomputedResetPending{false};
    int m_precomputedPendingResetStartIndex{0};
    int m_precomputedPendingResetBins{0};
    quint64 m_precomputedPendingResetTrackToken{0};
    quint64 m_precomputedPendingResetGeneration{0};
    bool m_precomputedPendingResetClearHistory{false};
    qint64 m_precomputedLastDisplaySeq{-1};
    int m_precomputedLastRightCol{-1};
    qint64 m_precomputedCanvasDisplayLeft{0};
    qint64 m_precomputedCanvasDisplayRight{-1};
    bool m_precomputedCanvasRolling{false};
    bool m_precomputedCanvasDirty{true};
    int m_displayMode{0}; // 0=Rolling, 1=Centered
    SpectrogramSourceTexture m_gpuSourceTexture;
    QByteArray m_gpuSourceUploadRgba;
    QVector<SpectrogramSourceUploadSpan> m_gpuSourceUploadSpans;
    int m_gpuSourcePageUploadSpanCount{0};
    int m_gpuSourcePageUploadColumns{0};
    bool m_gpuSourcePageTextureReset{false};
    qint64 m_gpuSourceTextureDisplayLeft{0};
    qint64 m_gpuSourceTextureDisplayRight{-1};
    bool m_gpuSourceTextureRolling{false};
    quint64 m_gpuSourceTextureTrackToken{0};
    qint64 m_gpuSourceTextureRingWriteSeq{-1};
    qint64 m_gpuSourceTextureRingOldestSeq{-1};
    qint64 m_gpuSourceTextureMinColumnIndex{0};
    qint64 m_gpuSourceTextureMaxColumnIndex{-1};
    std::vector<std::unique_ptr<RetainedSourcePageState>> m_gpuSourcePages;
    QImage m_gpuDisplayImage;
    QImage m_zoomFillFrozenDisplayImage;
    QVector<RetainedRenderSegment> m_zoomFillFrozenRetainedSegments;
    QImage m_gpuDbRemapImage;
    QImage m_gpuPaletteImage;
    QImage m_gpuRowLutImage;
    quint64 m_gpuDisplayGeneration{0};
    quint64 m_zoomFillFrozenDisplayGeneration{0};
    quint64 m_gpuDbRemapGeneration{0};
    quint64 m_gpuPaletteGeneration{0};
    quint64 m_gpuRowLutGeneration{0};
    quint64 m_gpuDisplaySourceGeneration{0};
    int m_gpuDisplayWidth{0};
    int m_gpuDisplayFilledWidth{0};
    int m_gpuDisplayHeight{0};
    int m_gpuDisplaySampleRateHz{0};
    int m_gpuDisplayBins{0};
    double m_gpuDisplayDbRange{132.0};
    bool m_gpuDisplayMuted{false};
    bool m_gpuDisplayLogScale{false};
    qint64 m_gpuDisplayDisplayLeft{0};
    qint64 m_gpuDisplayDisplayRight{-1};
    bool m_gpuDisplayRolling{false};
    qint64 m_gpuDisplayRingWriteSeq{-1};
    qint64 m_gpuDisplayRingOldestSeq{-1};
    qint32 m_gpuDisplayMaxColumnIndex{-1};
    quint64 m_gpuDisplayTrackToken{0};
    double m_gpuDisplayEffectiveZoom{1.0};
    bool m_gpuDisplayLastUpdateWasIncremental{false};
    int m_gpuDisplayLastShiftPixels{0};
    int m_gpuDisplayLastUpdatedColumns{0};
    double m_gpuDisplaySubpixelOffset{0.0};
    int m_gpuDbRemapBins{0};
    double m_gpuDbRemapDbRange{132.0};
    bool m_gpuPaletteMuted{false};
    int m_gpuRowLutHeight{0};
    int m_gpuRowLutBins{0};
    int m_gpuMaxTextureColumns{std::numeric_limits<int>::max()};
    double m_zoomLevel{1.0};
    double m_renderZoomLevel{1.0};
    bool m_zoomEnabled{false};
    // Canvas freeze during zoom transition.  Independent of
    // m_awaitingZoomData (which controls the zoom snap).  Set when a
    // session restart changes the hop; cleared when the ring covers
    // the display.  updatePaintNode skips canvas updates while active.
    bool m_zoomFillActive{false};
    bool m_zoomFillDataReady{false};
    bool m_zoomFillFrozenRetainedAvailable{false};
    bool m_zoomFillFrozenRetainedActive{false};
    double m_precomputedCanvasZoomLevel{1.0};
    int m_precomputedCanvasHopSize{0};
    int m_precomputedCanvasTotalColumnsEstimate{0};
    QTimer *m_zoomDebounceTimer{nullptr};
    float m_pendingBackendZoom{1.0f};
    bool m_awaitingZoomData{false};

    // Crosshair overlay state.
    bool m_crosshairEnabled{false};
    bool m_hoverActive{false};
    QPointF m_hoverPosition;
    double m_crosshairSharedX{-1.0};
    QImage m_crosshairImage;
    bool m_crosshairDirty{true};
    qint64 m_crosshairCachedDisplayLeft{0};
    double m_crosshairCachedDrawX{0.0};
    bool m_crosshairCachedRollingMode{false};
    int m_crosshairCachedBinsPerColumn{0};
    QRect m_crosshairFreqLabelRect;
    QRect m_crosshairTimeLabelRect;

    // Grid overlay state — split into static frequency grid and dynamic time grid.
    bool m_gridEnabled{false};
    bool m_showTimeLabels{false};

    // Frequency grid: horizontal lines + labels. Only depends on
    // height/sampleRate/binsPerColumn/logScale. Never rebuilt during playback.
    QImage m_freqGridImage;
    bool m_freqGridDirty{true};
    int m_freqGridCachedSampleRateHz{0};
    int m_freqGridCachedBinsPerColumn{0};

    // Time grid: vertical lines + labels (bottom pane only). Rendered
    // wider than the widget with right-side padding so the texture
    // source rect can be shifted cheaply on each frame. Only rebuilt
    // when the shift exhausts the padding (~every 8-10 seconds).
    QImage m_timeGridImage;
    bool m_timeGridDirty{true};
    qint64 m_timeGridRenderDisplayLeft{0};
    double m_timeGridRenderDrawX{0.0};
    bool m_timeGridCachedRollingMode{false};
    int m_timeGridPadding{0};

    double m_gaplessPositionOffset{0.0};
    std::chrono::steady_clock::time_point m_centeredGaplessTransitionAt{};
    int m_debugPaintCounter{0};

    mutable QMutex m_stateMutex;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    double m_debugPrevRenderPos{0.0};
    quint64 m_debugPrevTrackToken{0};
    std::chrono::steady_clock::time_point m_debugLastTransitionFeedAt{};

    struct SmoothnessProfileState {
        bool active{false};
        bool incidentDetected{false};
        bool incidentReported{false};
        bool sawForwardMotion{false};
        bool inStallCluster{false};
        qint64 startedAtMs{0};
        qint64 lastFrameAtMs{0};
        double lastHeadUnits{0.0};
        bool lastHeadValid{false};
        int framesObserved{0};
        int pendingFrames{0};
        int stallFrames{0};
        int stallClusters{0};
        int gapFrames{0};
        int severeGapFrames{0};
        int pendingGapFrames{0};
        double maxGapMs{0.0};
        int regressionCount{0};
        int servoFrames{0};
        int servoRegressionDrops{0};
        double servoErrorMsTotal{0.0};
        double maxServoErrorMs{0.0};
        int drainPasses{0};
        int drainedColumns{0};
        int maxPendingRows{0};
        int advanceFallbackFrames{0};
        int nonUnityAdvanceFallbackFrames{0};
        double maxAdvanceFallbackZoomDelta{0.0};
        int paintSpikeCount{0};
        double maxPaintMs{0.0};
        double paintMsTotal{0.0};
        int paintSamples{0};
        QVariantMap lastSummary;
    };

    struct SeekProfileState {
        bool active{false};
        bool incidentDetected{false};
        bool incidentReported{false};
        bool sawForwardMotion{false};
        bool inStallCluster{false};
        quint64 generation{0};
        qint64 startedAtMs{0};
        qint64 lastFrameAtMs{0};
        double targetSeconds{0.0};
        double lastHeadUnits{0.0};
        bool lastHeadValid{false};
        int framesObserved{0};
        int pendingFrames{0};
        int stallFrames{0};
        int stallClusters{0};
        int gapFrames{0};
        double maxGapMs{0.0};
        int regressionCount{0};
        int drainPasses{0};
        int drainedColumns{0};
        int maxPendingRows{0};
        QVariantMap lastSummary;
    };

    qint64 m_lastIncomingRowsAtMs{0};
    SmoothnessProfileState m_smoothnessProfile;
    SeekProfileState m_seekProfile;
#endif
};
