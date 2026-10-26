// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QMetaObject>
#include <QMutex>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>
#include <QVector>
#include <QVariantList>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <vector>

#include "PlaybackClock.h"

class QQuickWindow;
class QSGNode;
class QWheelEvent;
struct SpectrogramProjection;
struct SpectrogramRasterMailbox;
struct SpectrogramZoomPreview;

// Ring columns in fixed-size, implicitly shared blocks. A rasterization
// keeps copies of the blocks it reads, so writing a column copies only its
// block, and only while a rasterization still holds that block.
class SpectrogramRingStorage {
public:
    static constexpr int kColumnsPerBlock = 64;

    void clear();
    // Zero-filled room for `capacity` columns of `bins` bytes.
    void allocate(int capacity, int bins);
    int bins() const { return m_bins; }
    qint64 byteSize() const;
    const quint8 *column(int slot) const;
    quint8 *writableColumn(int slot);

private:
    std::vector<QByteArray> m_blocks;
    int m_bins{0};
};

// What a rasterization draws: the canvas's size in device pixels and the
// display columns it shows, and what they are drawn from.
struct SpectrogramRasterTarget {
    int width{0};
    int height{0};
    // Device pixels per logical pixel.
    double scale{1.0};
    qint64 displayLeft{0};
    qint64 displayRight{-1};
    bool rollingMode{false};
    // Canvas pixels per display column: the zoom's logical pixels, times
    // the scale.
    double effectiveZoom{1.0};
    double columnsPerSecond{0.0};
    qint64 epoch{0};
    quint64 trackToken{0};
    quint64 ringGeneration{0};
    // When the inputs were taken, which the canvas then shows.
    std::chrono::steady_clock::time_point capturedAt{};
    std::shared_ptr<const SpectrogramProjection> projection;
};

// A canvas to draw on a worker, with what it reads: copies of the ring's
// blocks, and where in them each display column is.
struct SpectrogramRasterJob {
    quint64 serial{0};
    SpectrogramRasterTarget target;
    SpectrogramRingStorage ring;
    // The slot of display column target.displayLeft + i, or -1 without it.
    std::vector<int> columnSlots;
};

struct SpectrogramRasterResult {
    quint64 serial{0};
    SpectrogramRasterTarget target;
    QImage canvas;
    // Copies of the canvas's tiles, ready to upload.
    QVector<QImage> tiles;
    int drawPixels{0};
    // The job's slots, which tell the columns it had to draw without.
    std::vector<int> columnSlots;
};

class SpectrogramItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(double dbRange READ dbRange WRITE setDbRange NOTIFY dbRangeChanged)
    Q_PROPERTY(bool logScale READ logScale WRITE setLogScale NOTIFY logScaleChanged)
    Q_PROPERTY(bool showFpsOverlay READ showFpsOverlay WRITE setShowFpsOverlay NOTIFY showFpsOverlayChanged)
    Q_PROPERTY(bool forceFpsOverlay READ forceFpsOverlay CONSTANT)
    Q_PROPERTY(int sampleRateHz READ sampleRateHz WRITE setSampleRateHz NOTIFY sampleRateHzChanged)
    Q_PROPERTY(int maxColumns READ maxColumns WRITE setMaxColumns NOTIFY maxColumnsChanged)
    // Where playback is; the playhead follows its position.
    Q_PROPERTY(PlaybackClock *clock READ clock WRITE setClock NOTIFY clockChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY positionSecondsChanged)
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
    ~SpectrogramItem() override;

    double dbRange() const;
    void setDbRange(double value);

    bool logScale() const;
    void setLogScale(bool value);

    bool showFpsOverlay() const;
    void setShowFpsOverlay(bool value);
    bool forceFpsOverlay() const;

    int sampleRateHz() const;
    void setSampleRateHz(int value);

    int maxColumns() const;
    void setMaxColumns(int value);

    PlaybackClock *clock() const;
    void setClock(PlaybackClock *value);
    double positionSeconds() const;

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
    Q_INVOKABLE void appendRows(const QVariantList &rows);

    Q_INVOKABLE double minimumZoomLevel() const;

signals:
    void dbRangeChanged();
    void logScaleChanged();
    void showFpsOverlayChanged();
    void sampleRateHzChanged();
    void maxColumnsChanged();
    void clockChanged();
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
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void releaseResources() override;

private:
    static constexpr int kGradientTableSize = 2048;
    static constexpr int kCanvasTileWidth = 64;

    void rebuildPalette();
    void invalidateMapping();
    void ensureMapping(int height);
    void invalidateCanvas();
    void ensureCanvas(int width, int height);
    bool reanchorRollingEpochForCurrentDataLocked(
        std::chrono::steady_clock::time_point now);
    void updateCanvasFillReadinessLocked();
    void applyPrecomputedResetLocked(
        int startIndex,
        int bins,
        quint64 trackToken,
        quint64 generation,
        bool clearHistoryOnReset);
    qint64 currentRollingDisplayRightLocked(std::chrono::steady_clock::time_point now) const;
    void truncateRollingTailLocked(qint64 newWriteSeq);
    double currentRenderPositionSecondsLocked(std::chrono::steady_clock::time_point now) const;
    bool positionHeldLocked(qint64 nowNs) const;
    void clockStateChanged();
    void holdPositionLocked(
        const PlaybackClockState &trajectory,
        double offset,
        std::chrono::steady_clock::time_point now);
    void releasePositionHoldLocked(std::chrono::steady_clock::time_point now);
    void continueGaplessLocked(std::chrono::steady_clock::time_point now);
    std::shared_ptr<SpectrogramRasterJob> makeRasterJobLocked(
        int width,
        int height,
        qint64 displayLeft,
        qint64 displayRight,
        bool rollingMode);
    static std::shared_ptr<SpectrogramRasterResult> rasterize(
        const SpectrogramRasterJob &job,
        const std::atomic<quint64> *latestSerial = nullptr);
    void requestPrecomputedRasterLocked(
        int width,
        int height,
        qint64 displayLeft,
        qint64 displayRight,
        bool rollingMode);
    bool pendingRasterOutdatedLocked(
        int width,
        int height,
        qint64 displayLeft,
        qint64 displayRight,
        bool rollingMode) const;
    void cancelPendingRasterLocked();
    void installRasterResult(const std::shared_ptr<SpectrogramRasterResult> &result);
    bool installRasterResultLocked(const SpectrogramRasterResult &result);
    void drawCenteredColumnsInPlaceLocked(
        qint64 firstColumn, qint64 lastColumn, double effectiveZoom);
    const quint8 *ringColumnLocked(qint64 displayIndex, bool rollingMode) const;
    void markCanvasStaleLocked();
    SpectrogramZoomPreview staleCanvasAlignmentLocked(
        double canvasDrawX,
        int canvasPixels,
        double viewWidth,
        double viewDrawX,
        qint64 displayLeft,
        bool rollingMode,
        double canvasViewZoom,
        double columnsPerSecond) const;
    int maxSyncAppendPixelsLocked(int canvasWidth, double canvasZoom) const;
    bool advancePrecomputedCanvasLocked(
        qint64 displayLeft,
        qint64 displayRight,
        bool rollingMode);
    void drawPrecomputedColumnAtLocked(
        int x,
        qint64 displayIndex,
        bool rollingMode,
        const SpectrogramProjection &projection);
    // Canvas pixels firstPixel to endPixel - 1 of the view from display
    // column displayLeft, into canvas columns from firstCanvasX on, wrapping.
    // The canvas has scrolled subpixelOffset pixels past that column.
    void drawPrecomputedPixelsLocked(
        int firstCanvasX,
        int firstPixel,
        int endPixel,
        double subpixelOffset,
        qint64 displayLeft,
        qint64 displayRight,
        bool rollingMode,
        double canvasZoom,
        const SpectrogramProjection &projection);
    int ringSlotForDisplayIndexLocked(
        qint64 displayIndex,
        bool rollingMode) const;
    std::array<quint8, 256> buildPrecomputedDbRemapLocked() const;
    // For `height` rows, or the canvas's height by default.
    std::shared_ptr<const SpectrogramProjection> precomputedProjectionLocked(int height = -1) const;
    mutable std::shared_ptr<const SpectrogramProjection> m_precomputedProjection;
    void rebuildCanvasFromColumns();
    void drawColumnAt(int x, const std::vector<quint8> &col);
    void resizeDirtyTilesLocked();
    void markTileDirtyLocked(int x);
    void markAllTilesDirtyLocked();
    void updateOverlayImageLocked();
    void updateCrosshairOverlayLocked(
        int width, int height,
        qint64 displayLeft, bool rollingMode,
        double columnsPerSecond, double drawX);
    // The device column (or row) of `logical`, clamped to the overlay.
    int crosshairDeviceColumnLocked(double logical, int devicePixelCount) const;
    void updateFreqGridOverlayLocked(int width, int height);
    void updateTimeGridOverlayLocked(
        int width, int height, int padding,
        qint64 displayLeft,
        bool rollingMode, double columnsPerSecond, double drawX);
    double effectiveZoomLocked() const;
    // Canvas pixels per display column.
    double canvasZoomLocked() const;
    int canvasWidthLocked() const;
    int canvasHeightLocked() const;
    // The window's device pixel ratio, or the current one without a window.
    double windowDevicePixelRatio() const;
    void applyCanvasScaleLocked(double scale);
    double minimumZoomLevelLocked() const;
    double backendZoomForRequestLocked() const;
    int columnHopForZoomLocked(double zoom) const;
    double renderZoomForZoomLocked(double zoom) const;
    bool zoomPreviewWantedLocked() const;
    SpectrogramZoomPreview zoomPreviewLocked(
        int width,
        double positionSeconds,
        double drawX,
        bool rollingMode,
        double columnsPerSecond) const;
    double pixelToFrequencyHzLocked(int pixelY, int viewHeight) const;
    int frequencyToPixelYLocked(double freqHz, int viewHeight) const;
    std::vector<quint8> rowToIntensity(const QVariantList &row) const;
    void bindWindowFpsTracking(QQuickWindow *window);
    void handleWindowAfterAnimating();
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
    void noteSmoothnessProfileFrameLocked(qint64 nowMs, double elapsedSeconds, bool pending, bool advanced);
    void noteSmoothnessAdvanceFallbackLocked(double effectiveZoom);
    void noteSmoothnessPaintLocked(double paintMs);
    void finalizeSmoothnessProfileLocked(qint64 nowMs, const char *reason);
    QVariantMap debugSmoothnessProfileStateLocked() const;
#endif

    double m_dbRange{132.0};
    bool m_logScale{false};
    int m_sampleRateHz{48000};
    int m_maxColumns{640};
    int m_binsPerColumn{0};

    std::array<QRgb, kGradientTableSize> m_palette32{};
    std::array<QRgb, kGradientTableSize> m_palette32Gray{};
    bool m_channelMuted{false};
    std::vector<int> m_iToBin;
    int m_mappingHeight{-1};
    int m_lowResEnd{0};

    // The canvas and the overlays are in device pixels: this many to a
    // logical pixel. The time axis stays in logical pixels, so a display
    // column at a zoom is this many times as many canvas pixels.
    double m_canvasScale{1.0};
    QImage m_canvas;
    bool m_canvasDirty{true};
    QImage m_overlayImage;
    bool m_overlayDirty{true};
    int m_canvasWriteX{0};
    int m_canvasFilledCols{0};
    std::deque<std::vector<quint8>> m_columns;
    std::vector<unsigned char> m_dirtyTiles;
    // Tile copies a rasterization made of the canvas it delivered, for the
    // render thread to upload instead of copying them itself. A tile drawn
    // into since then is copied again.
    QVector<QImage> m_rasterTiles;
    // The rasterization that will replace the canvas: its target, and its
    // serial, which only its result carries. The canvas stays on screen,
    // aligned to the view, until the result arrives.
    std::shared_ptr<SpectrogramRasterMailbox> m_rasterMailbox;
    quint64 m_rasterSerial{0};
    bool m_rasterPending{false};
    SpectrogramRasterTarget m_rasterTarget;
    // The canvas being replaced was on screen through the zoom preview,
    // which goes on until its replacement arrives.
    bool m_rasterHoldsZoomPreview{false};
    bool m_zoomPreviewWantedLastFrame{false};
    // Changes whenever the ring's columns are cleared or renumbered, which
    // makes a rasterization of them obsolete.
    quint64 m_ringGeneration{0};
    bool m_seedHistoryOnNextAppend{true};
    bool m_animationTickInitialized{false};
    std::chrono::steady_clock::time_point m_lastAnimationTick{};
    bool m_forceFpsOverlay{false};
    bool m_showFpsOverlay{false};
    bool m_fpsInitialized{false};
    int m_fpsValue{0};
    int m_fpsAccumFrames{0};
    double m_fpsAccumSeconds{0.0};
    std::chrono::steady_clock::time_point m_lastFrameTime{};
    bool m_profileEnabled{false};
    std::chrono::steady_clock::time_point m_profileLast{};
    std::chrono::steady_clock::time_point m_profileLastAppendSpike{};
    std::chrono::steady_clock::time_point m_profileLastFrameGapSpike{};
    std::chrono::steady_clock::time_point m_profileLastPaintSpike{};
    std::chrono::steady_clock::time_point m_profileLastWriteHeadClampSpike{};
    std::chrono::steady_clock::time_point m_profileLastAdvanceFallbackSpike{};
    quint64 m_profilePaints{0};
    double m_profilePaintMs{0.0};
    quint64 m_sceneGraphGeneration{0};
    QMetaObject::Connection m_animationTickConnection;
    QMetaObject::Connection m_windowVisibilityConnection;

    // Ring-buffer spectrogram storage.
    SpectrogramRingStorage m_ring;    // capacity columns of bins_per_column bytes
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
    // The playhead's position when the clock or the data last changed it.
    double m_positionSeconds{0.0};
    QPointer<PlaybackClock> m_clock;
    QMetaObject::Connection m_clockConnection;
    // A copy of the clock's state, which the render thread evaluates.
    PlaybackClockState m_clockState;
    bool m_playing{false};
    // Rolling history is in write order, so after a jump the playhead
    // goes on along m_positionHoldTrajectory (plus its offset) until the
    // worker's data for the new position arrives, or, with a deadline in
    // m_positionJumpHoldUntilNs, until then.
    bool m_positionJumpHoldActive{false};
    PlaybackClockState m_positionHoldTrajectory;
    double m_positionHoldOffset{0.0};
    qint64 m_positionJumpHoldUntilNs{0};
    std::chrono::steady_clock::time_point m_positionJumpHoldStartedAt{};
    // Where the previous track was, for a gapless continuation: the clock's
    // state before it changed tracks, and the offset it was shown with.
    PlaybackClockState m_previousTrackClockState;
    double m_previousTrackPositionOffset{0.0};
    // Data for a gapless successor arrived before the clock changed tracks.
    bool m_gaplessContinuationPending{false};
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
    // How far the canvas has scrolled past its first display column, in
    // part of a canvas pixel: its pixel x starts at x minus this.
    double m_precomputedCanvasSubpixelOffset{0.0};
    // The display range's right end the canvas was drawn for, which its
    // last pixels may not reach.
    qint64 m_precomputedCanvasViewRight{-1};
    // The scale the canvas was drawn at.
    double m_precomputedCanvasScale{1.0};
    int m_displayMode{0}; // 0=Rolling, 1=Centered
    double m_zoomLevel{1.0};
    double m_renderZoomLevel{1.0};
    bool m_zoomEnabled{false};
    // Canvas freeze during zoom transition.  Independent of
    // m_awaitingZoomData (which controls the zoom snap).  Set when a
    // session restart changes the hop; cleared when the ring covers
    // the display.  updatePaintNode skips canvas updates while active.
    bool m_zoomFillActive{false};
    // The canvas's pixels per display column.
    double m_precomputedCanvasZoomLevel{1.0};
    // Restarting the backend for every wheel step would decode the same
    // window again and again, so zoom requests reach it after a pause.
    QTimer *m_zoomDebounceTimer{nullptr};
    float m_pendingBackendZoom{1.0f};
    // The zoom the backend is at after the requests sent to it, which
    // leave it alone when too close to its zoom.
    float m_lastBackendZoom{1.0f};
    // The data on screen is not for m_zoomLevel yet. Until it is, the
    // canvas is shown scaled to m_zoomLevel.
    bool m_awaitingZoomData{false};
    // Where the canvas content is in time: its columns per second, the
    // rolling epoch it was drawn with, and its logical pixels per second.
    double m_precomputedCanvasColumnsPerSecond{0.0};
    qint64 m_precomputedCanvasEpoch{0};
    double m_precomputedCanvasPixelsPerSecond{0.0};
    // The track of a centered canvas's columns, and the ring numbering of
    // a rolling one's.
    quint64 m_precomputedCanvasTrackToken{0};
    quint64 m_precomputedCanvasRingGeneration{0};
    // When the canvas's columns were last brought up to the view.
    std::chrono::steady_clock::time_point m_precomputedCanvasDrawnAt{};
    // The scaled canvas drawn last, so that clicks seek where it shows:
    // the track time at anchor X, and pixels per second.
    bool m_zoomPreviewShown{false};
    double m_zoomPreviewAnchorSeconds{0.0};
    double m_zoomPreviewAnchorX{0.0};
    double m_zoomPreviewPixelsPerSecond{0.0};

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
    QRect m_crosshairVerticalLineRect;
    QRect m_crosshairHorizontalLineRect;
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

    // Rolling mode shows a gapless successor after the tracks before it:
    // its position plus this offset.
    double m_gaplessPositionOffset{0.0};
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
    quint64 m_lastFinalizedSeekProfileGeneration{0};
    SmoothnessProfileState m_smoothnessProfile;
    SeekProfileState m_seekProfile;
#endif
};
