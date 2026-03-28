// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QMetaObject>
#include <QMutex>
#include <QQuickItem>
#include <QVariantList>

#include <array>
#include <chrono>
#include <deque>
#include <vector>

class QQuickWindow;
class QSGNode;

class SpectrogramItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(double dbRange READ dbRange WRITE setDbRange NOTIFY dbRangeChanged)
    Q_PROPERTY(bool logScale READ logScale WRITE setLogScale NOTIFY logScaleChanged)
    Q_PROPERTY(bool showFpsOverlay READ showFpsOverlay WRITE setShowFpsOverlay NOTIFY showFpsOverlayChanged)
    Q_PROPERTY(bool forceFpsOverlay READ forceFpsOverlay CONSTANT)
    Q_PROPERTY(int sampleRateHz READ sampleRateHz WRITE setSampleRateHz NOTIFY sampleRateHzChanged)
    Q_PROPERTY(int maxColumns READ maxColumns WRITE setMaxColumns NOTIFY maxColumnsChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds WRITE setPositionSeconds NOTIFY positionSecondsChanged)
    Q_PROPERTY(bool playing READ isPlaying WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(bool precomputedReady READ precomputedReady NOTIFY precomputedReadyChanged)
    Q_PROPERTY(int displayMode READ displayMode WRITE setDisplayMode NOTIFY displayModeChanged)
    Q_PROPERTY(bool crosshairEnabled READ crosshairEnabled WRITE setCrosshairEnabled NOTIFY crosshairEnabledChanged)
    Q_PROPERTY(bool gridEnabled READ gridEnabled WRITE setGridEnabled NOTIFY gridEnabledChanged)
    Q_PROPERTY(bool showTimeLabels READ showTimeLabels WRITE setShowTimeLabels NOTIFY showTimeLabelsChanged)
    Q_PROPERTY(double crosshairSharedX READ crosshairSharedX WRITE setCrosshairSharedX NOTIFY crosshairSharedXChanged)

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

    int maxColumns() const;
    void setMaxColumns(int value);

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

    double pixelToFrequencyHz(int pixelY, int viewHeight) const;

    Q_INVOKABLE void feedPrecomputedChunk(
        const QByteArray &data, int bins, int channelIndex,
        int columns, int startIndex, int totalEstimate,
        int sampleRate, int hopSize, bool complete,
        bool bufferReset, quint64 trackToken = 0,
        bool clearHistoryOnReset = false);
    Q_INVOKABLE void clearPrecomputed();

    Q_INVOKABLE void reset();
    Q_INVOKABLE void halt();
    Q_INVOKABLE void appendRows(const QVariantList &rows);

signals:
    void dbRangeChanged();
    void logScaleChanged();
    void showFpsOverlayChanged();
    void sampleRateHzChanged();
    void maxColumnsChanged();
    void positionSecondsChanged();
    void playingChanged();
    void precomputedReadyChanged();
    void displayModeChanged();
    void crosshairEnabledChanged();
    void gridEnabledChanged();
    void showTimeLabelsChanged();
    void crosshairSharedXChanged();
    void crosshairHoverChanged(double x);

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
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
    void applyPrecomputedResetLocked(
        int startIndex,
        int bins,
        quint64 trackToken,
        bool clearHistoryOnReset);
    qint64 currentRollingDisplayRightLocked(std::chrono::steady_clock::time_point now) const;
    void truncateRollingTailLocked(qint64 newWriteSeq);
    double currentRenderPositionSecondsLocked(std::chrono::steady_clock::time_point now) const;
    void setPositionAnchorLocked(double value, std::chrono::steady_clock::time_point now);
    void syncPositionAnchorLocked(std::chrono::steady_clock::time_point now);
    void rebuildPrecomputedCanvasLocked(
        int width,
        int height,
        qint64 displayLeft,
        qint64 displayRight,
        bool rollingMode);
    bool advancePrecomputedCanvasLocked(
        qint64 displayLeft,
        qint64 displayRight,
        bool rollingMode);
    void drawPrecomputedColumnAtLocked(
        int x,
        qint64 displayIndex,
        bool rollingMode,
        const std::array<quint8, 256> &dbRemap);
    std::array<quint8, 256> buildPrecomputedDbRemapLocked() const;
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
    void updateFreqGridOverlayLocked(int width, int height);
    void updateTimeGridOverlayLocked(
        int width, int height, int padding,
        qint64 displayLeft,
        bool rollingMode, double columnsPerSecond, double drawX);
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
    std::vector<int> m_iToBin;
    int m_mappingHeight{-1};
    int m_lowResEnd{0};

    QImage m_canvas;
    bool m_canvasDirty{true};
    QImage m_overlayImage;
    bool m_overlayDirty{true};
    int m_canvasWriteX{0};
    int m_canvasFilledCols{0};
    std::deque<std::vector<quint8>> m_columns;
    std::vector<unsigned char> m_dirtyTiles;
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
    qint64 m_ringWriteSeq{0};         // next write-order sequence number
    qint64 m_ringOldestSeq{0};        // oldest write-order sequence still retained
    qint64 m_trackEpochSeq{0};        // legacy reset bookkeeping
    qint64 m_rollingEpoch{0};         // maps track columns to rolling write-order history
    qint32 m_precomputedMaxColumnIndex{-1}; // highest column for current token
    int m_precomputedBinsPerColumn{0};
    int m_precomputedTotalColumnsEstimate{0};
    int m_precomputedSampleRateHz{44100};
    int m_precomputedHopSize{1024};
    quint64 m_precomputedTrackToken{0};
    quint64 m_precomputedCommittedToken{0}; // token from the most recent buffer_reset
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
    bool m_precomputedPendingResetClearHistory{false};
    qint64 m_precomputedLastDisplaySeq{-1};
    int m_precomputedLastRightCol{-1};
    qint64 m_precomputedCanvasDisplayLeft{0};
    qint64 m_precomputedCanvasDisplayRight{-1};
    bool m_precomputedCanvasRolling{false};
    bool m_precomputedCanvasDirty{true};
    int m_displayMode{0}; // 0=Rolling, 1=Centered

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
        int drainPasses{0};
        int drainedColumns{0};
        int maxPendingRows{0};
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
