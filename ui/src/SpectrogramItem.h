// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QMetaObject>
#include <QMutex>
#include <QPointer>
#include <QQuickItem>
#include <limits>
#include <QRectF>
#include <QSizeF>
#include <QTimer>
#include <QVector>
#include <QVariantList>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include "CrosshairReadout.h"
#include "PlaybackClock.h"
#include "PlaybackFrameScheduler.h"
#include "WindowPresence.h"

class QQuickWindow;
class QSGNode;
class QSGTexture;
class QWheelEvent;
struct SpectrogramProjection;
struct SpectrogramRasterMailbox;
struct SpectrogramZoomPreview;

// Ring columns in fixed-size, implicitly shared blocks. A rasterization
// keeps copies of the blocks it reads, so writing a column copies only its
// block, and only while a rasterization still holds that block.
//
// A block takes memory when one of its columns is first written, so making
// room for a ring costs nothing however large it is. Columns of a block
// never written read as missing.
class SpectrogramRingStorage {
public:
    static constexpr int kColumnsPerBlock = 64;

    void clear();
    // Room for `capacity` columns of `bins` bytes.
    void allocate(int capacity, int bins);
    int bins() const { return m_bins; }
    int capacity() const { return m_capacity; }
    // The bytes of the whole capacity.
    qint64 byteSize() const;
    // The bytes of the blocks written so far.
    qint64 committedBytes() const;
    // Null for a column of a block not written yet.
    const quint8 *column(int slot) const;
    // Zero-fills the column's block when it is the block's first write.
    quint8 *writableColumn(int slot);

private:
    int blockColumns(size_t block) const;

    // Null until written.
    std::vector<QByteArray> m_blocks;
    int m_capacity{0};
    int m_bins{0};
};

// Which columns of a canvas have changed since the scene graph last took
// them, or that all of it has.
class SpectrogramCanvasDamage {
public:
    // A canvas `width` columns wide, all of it changed.
    void reset(int width);
    int width() const { return static_cast<int>(m_columns.size()); }
    void markColumn(int x);
    void markAll() { m_all = true; }
    bool all() const { return m_all; }
    int columnCount() const { return m_count; }
    bool anyIn(int first, int end) const;
    // Calls `visit(first, end)` for each run of changed columns, in order.
    template <typename Visit>
    void forEachRun(Visit &&visit) const {
        const int columns = width();
        for (int x = 0; x < columns;) {
            if (m_columns[static_cast<size_t>(x)] == 0) {
                ++x;
                continue;
            }
            const int first = x;
            while (x < columns && m_columns[static_cast<size_t>(x)] != 0) {
                ++x;
            }
            visit(first, x);
        }
    }
    // Nothing has changed.
    void clear();

private:
    std::vector<unsigned char> m_columns;
    int m_count{0};
    bool m_all{true};
};

// Canvas pixels for the scene graph to upload, in order: images of the
// canvas columns from x on.
struct SpectrogramCanvasUpload {
    struct Part {
        int x{0};
        QImage image;
    };
    QVector<Part> parts;
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
    // How far playback had travelled when the inputs were taken, which
    // the canvas then shows.
    double capturedTravelSeconds{0.0};
    std::shared_ptr<const SpectrogramProjection> projection;
    // Pixels of the view it leaves without a column: columns not decoded
    // yet, or a view wider than the columns.
    bool missingColumns{false};
    // The stream origin and the layers' validity when it was asked for.
    double streamOrigin{0.0};
    quint64 validity{0};
};

// Older data drawn under the canvas, whose missing columns are transparent,
// at the times its own columns are of, so that it stands in for columns
// still to come rather than the background: the canvas that a canvas with
// columns missing replaced, as the view before a zoom was (the backdrop),
// and the ring's columns around the view (the reserve), which a zoom out
// shows beside the canvas before the zoom's data arrives.
struct SpectrogramBackdrop {
    QImage image;
    // The pixels drawn: `pixels` of them from `start`, wrapping.
    int start{0};
    int pixels{0};
    // The time pixel `start` starts at, as playback reckons it (see
    // staleCanvasAlignmentLocked) plus the stream origin then (see
    // m_streamOrigin), and the image's pixels a second.
    double leftSeconds{0.0};
    double pixelsPerSecond{0.0};
    bool rolling{false};
    // A centered layer's track, which another track's view has nothing
    // of. A rolling layer goes on with the history across a gapless or
    // Repeat-One continuation, and a reset that is not a zoom's restart,
    // which breaks the history, drops it.
    quint64 trackToken{0};
    // The validity it was made with (m_backdropValidity).
    quint64 validity{0};
    // What its rows and colours are of: another projection's are not this
    // view's.
    std::shared_ptr<const SpectrogramProjection> projection;
    // Which it is, for the scene node's texture of it.
    quint64 serial{0};

    bool isNull() const { return image.isNull(); }
    double rightSeconds() const {
        return pixelsPerSecond > 0.0 ? leftSeconds + pixels / pixelsPerSecond : leftSeconds;
    }
};

// A canvas to draw on a worker, with what it reads: copies of the ring's
// blocks, and where in them each display column is.
struct SpectrogramRasterJob {
    quint64 serial{0};
    SpectrogramRasterTarget target;
    SpectrogramRingStorage ring;
    // The slot of display column target.displayLeft + i, or -1 without it.
    std::vector<int> columnSlots;
    // The columns of each copy of the canvas to make for upload: the scene
    // graph's texture tiles, or 0 for one copy of all of it.
    int tileWidth{0};
    // Whether to make those copies: a reserve is uploaded whole.
    bool makeTiles{true};
    // Older data drawn under the pixels that have no column: the image of
    // the reserve it replaces, the `underlayPixels` of it from its left,
    // and their seconds (see SpectrogramBackdrop).
    QImage underlay;
    int underlayPixels{0};
    double underlayLeftSeconds{0.0};
    double underlayPixelsPerSecond{0.0};
};

// A small image an overlay places on the pane: a label, or a crosshair's
// dotted line. The scene node keeps its texture while the overlay shows it.
struct SpectrogramOverlayImage {
    // A scale label: outlined text on the plot. The crosshair's dashed lines,
    // and its readout's plates (see CrosshairReadout). A scale label of the
    // lane at rest, before it has columns, in the secondary text colour.
    enum Kind { ScaleLabel, DashedColumn, DashedRow, ReadoutPlate, RestScaleLabel };
    Kind kind{ScaleLabel};
    // A label's text, or a dotted line's length in device pixels.
    QString text;
    int length{0};
    // Where it goes, in logical pixels: one texel to a device pixel.
    QRectF rect;
    // The name of its texture, at the scale it was laid out for.
    QString key;
};

// What an overlay draws: solid lines, in device pixels, and small images.
// Nothing as large as the pane is drawn, so laying one out costs little at
// any device pixel ratio.
struct SpectrogramOverlayLayout {
    bool valid{false};
    // The device pixels it was laid out for.
    QSize size;
    QVector<QRect> lines;
    QVector<SpectrogramOverlayImage> images;
};

struct SpectrogramRasterResult {
    quint64 serial{0};
    SpectrogramRasterTarget target;
    QImage canvas;
    // Copies of the canvas, ready to upload: of its tiles, tileWidth
    // columns each, or one of all of it for a tileWidth of 0.
    QVector<QImage> tiles;
    int tileWidth{0};
    int drawPixels{0};
    // The job's slots, which tell the columns it had to draw without.
    std::vector<int> columnSlots;
};

// What a spectrogram's scene node did in its last frame: the canvas
// textures it made, the uploads into canvas textures it kept, the canvas
// pixels' bytes it passed on for both, and how many of its texture nodes it
// changed. For tests and benchmarks.
struct SpectrogramSceneStatistics {
    // Whether its canvas textures are kept and updated in place, and how
    // many it has.
    bool canvasInPlace{false};
    int canvasTextureCount{0};
    int canvasTextures{0};
    int canvasUploads{0};
    qint64 canvasTextureBytes{0};
    int textureNodeChanges{0};
    // The grids' and the crosshair's label and line textures made.
    int overlayTextures{0};
    qint64 overlayTextureBytes{0};
};

class SpectrogramItem : public QQuickItem, public PlaybackFrameScheduler::Client {
    Q_OBJECT
    Q_PROPERTY(double dbRange READ dbRange WRITE setDbRange NOTIFY dbRangeChanged)
    Q_PROPERTY(bool logScale READ logScale WRITE setLogScale NOTIFY logScaleChanged)
    Q_PROPERTY(bool showFpsOverlay READ showFpsOverlay WRITE setShowFpsOverlay NOTIFY showFpsOverlayChanged)
    Q_PROPERTY(bool forceFpsOverlay READ forceFpsOverlay CONSTANT)
    // What the controls over the top right corner take of it; see ViewCorner.
    Q_PROPERTY(QSizeF reservedCorner READ reservedCorner WRITE setReservedCorner NOTIFY reservedCornerChanged)
    Q_PROPERTY(int sampleRateHz READ sampleRateHz WRITE setSampleRateHz NOTIFY sampleRateHzChanged)
    Q_PROPERTY(int maxColumns READ maxColumns WRITE setMaxColumns NOTIFY maxColumnsChanged)
    // Where playback is; the playhead follows its position.
    Q_PROPERTY(PlaybackClock *clock READ clock WRITE setClock NOTIFY clockChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY positionSecondsChanged)
    Q_PROPERTY(bool playing READ isPlaying WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(bool precomputedReady READ precomputedReady NOTIFY precomputedReadyChanged)
    Q_PROPERTY(int displayMode READ displayMode WRITE setDisplayMode NOTIFY displayModeChanged)
    Q_PROPERTY(bool crosshairEnabled READ crosshairEnabled WRITE setCrosshairEnabled NOTIFY crosshairEnabledChanged)
    // How much the crosshair, its lines and its readout, shows, from 0 to 1:
    // a viewer's controls fade it out and in with them.
    Q_PROPERTY(double crosshairOpacity READ crosshairOpacity WRITE setCrosshairOpacity NOTIFY crosshairOpacityChanged)
    // What the crosshair's plates on this pane read, a line each: the
    // frequency and level at the pointer ("4.4 kHz, −62 dB") on the hovered
    // pane, then the time ("1:46.0") on the pane that shows it. Empty while
    // it shows none.
    Q_PROPERTY(QString crosshairReadout READ crosshairReadout NOTIFY crosshairReadoutChanged)
    // Whether the crosshair's time shows on this pane's bottom edge: on the
    // lowest of a plot's lanes.
    Q_PROPERTY(bool showCrosshairTime READ showCrosshairTime WRITE setShowCrosshairTime NOTIFY showCrosshairTimeChanged)
    // What this lane's label and buttons take of it, from its top left; the
    // crosshair's time keeps clear of them. Empty without any.
    Q_PROPERTY(QRectF laneHeadBounds READ laneHeadBounds WRITE setLaneHeadBounds NOTIFY laneHeadBoundsChanged)
    // The readout's plates, in the plot's colours (UiPalette's PlotColors).
    Q_PROPERTY(QColor readoutPlateColor READ readoutPlateColor WRITE setReadoutPlateColor NOTIFY readoutColorsChanged)
    Q_PROPERTY(QColor readoutPlateEdgeColor READ readoutPlateEdgeColor WRITE setReadoutPlateEdgeColor NOTIFY readoutColorsChanged)
    Q_PROPERTY(QColor readoutTextColor READ readoutTextColor WRITE setReadoutTextColor NOTIFY readoutColorsChanged)
    // A time of the track to follow, such as where its file is cut off, or
    // -1; markerX is where the view draws it, NaN while it does not.
    Q_PROPERTY(double markerSeconds READ markerSeconds WRITE setMarkerSeconds NOTIFY markerSecondsChanged)
    Q_PROPERTY(double markerX READ markerX NOTIFY markerXChanged)
    Q_PROPERTY(bool gridEnabled READ gridEnabled WRITE setGridEnabled NOTIFY gridEnabledChanged)
    // The analysis's FFT size and whether it caps the band (Preferences):
    // a lane at rest, without columns, draws its scale's lines where a
    // session of them will put the scale.
    Q_PROPERTY(int analysisFftSize READ analysisFftSize WRITE setAnalysisFftSize NOTIFY analysisFftSizeChanged)
    Q_PROPERTY(bool frequencyCap READ frequencyCap WRITE setFrequencyCap NOTIFY frequencyCapChanged)
    Q_PROPERTY(bool showTimeLabels READ showTimeLabels WRITE setShowTimeLabels NOTIFY showTimeLabelsChanged)
    Q_PROPERTY(double crosshairSharedX READ crosshairSharedX WRITE setCrosshairSharedX NOTIFY crosshairSharedXChanged)
    Q_PROPERTY(bool channelMuted READ channelMuted WRITE setChannelMuted NOTIFY channelMutedChanged)
    // Whether the playhead is drawn: the plot going out under another as
    // the visualization mode switches draws none, so only one shows.
    Q_PROPERTY(bool playheadVisible READ playheadVisible WRITE setPlayheadVisible NOTIFY playheadVisibleChanged)
    Q_PROPERTY(double zoomLevel READ zoomLevel WRITE setZoomLevel NOTIFY zoomLevelChanged)
    Q_PROPERTY(bool zoomEnabled READ zoomEnabled WRITE setZoomEnabled NOTIFY zoomEnabledChanged)
    // Whether the default zoom stretches a track shorter than the view to
    // its width.
    Q_PROPERTY(bool stretchShortTracks READ stretchShortTracks WRITE setStretchShortTracks NOTIFY stretchShortTracksChanged)
    // The zoom of the initial view of a track and of Reset Zoom: 1, or with
    // stretchShortTracks the zoom at which a track shorter than the view
    // fills its width. A track change moves the view to it (unless a rolling
    // view goes on gaplessly); the view's owner follows it otherwise.
    Q_PROPERTY(double defaultZoomLevel READ defaultZoomLevel NOTIFY defaultZoomLevelChanged)
    // The deepest zoom: 16, or the default zoom where that is deeper. A zoom
    // deeper than it comes back to it when it drops.
    Q_PROPERTY(double maximumZoomLevel READ maximumZoomLevel NOTIFY maximumZoomLevelChanged)

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
    QSizeF reservedCorner() const;
    void setReservedCorner(const QSizeF &value);
    // Where the FPS counter shows, in this item; empty while it does not.
    QRectF fpsCounterRect() const;

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
    QString crosshairReadout() const { return m_crosshairReadout; }
    bool showCrosshairTime() const;
    void setShowCrosshairTime(bool value);
    QRectF laneHeadBounds() const;
    void setLaneHeadBounds(const QRectF &value);
    QColor readoutPlateColor() const;
    void setReadoutPlateColor(const QColor &value);
    QColor readoutPlateEdgeColor() const;
    void setReadoutPlateEdgeColor(const QColor &value);
    QColor readoutTextColor() const;
    void setReadoutTextColor(const QColor &value);
    double markerSeconds() const;
    void setMarkerSeconds(double value);
    double markerX() const { return m_markerX; }
    void setCrosshairEnabled(bool value);
    double crosshairOpacity() const;
    void setCrosshairOpacity(double value);

    bool gridEnabled() const;
    void setGridEnabled(bool value);

    int analysisFftSize() const;
    void setAnalysisFftSize(int value);
    bool frequencyCap() const;
    void setFrequencyCap(bool value);

    bool showTimeLabels() const;
    void setShowTimeLabels(bool value);

    double crosshairSharedX() const;
    void setCrosshairSharedX(double value);

    bool channelMuted() const;
    void setChannelMuted(bool muted);
    bool playheadVisible() const;
    void setPlayheadVisible(bool value);

    double zoomLevel() const;
    void setZoomLevel(double value);

    bool zoomEnabled() const;
    void setZoomEnabled(bool value);

    bool stretchShortTracks() const;
    void setStretchShortTracks(bool value);
    double defaultZoomLevel() const;

    double pixelToFrequencyHz(int pixelY, int viewHeight) const;

    // `sessionWidth` is the width the chunk's session decodes for (0 if
    // unknown); the ring holds at least what that width needs. `fftSize` is
    // the FFT size of the columns' spectra (0 if unknown, for bins of the
    // whole spectrum up to Nyquist): a session with the frequency cap on
    // sends fewer bins than `fftSize / 2 + 1`, each still
    // `sampleRate / fftSize` Hz wide. `trackFrames` are the frames of the
    // file the session's columns are of (0 if unknown), at `sampleRate`:
    // with `fftSize`, where a session at another zoom ends. `endColumn` is
    // the column after the session's last, which the decoder's centered
    // views end at (0 if unknown): the centered view ends there too.
    Q_INVOKABLE void feedPrecomputedChunk(
        const QByteArray &data, int bins, int channelIndex,
        int columns, int startIndex, int totalEstimate,
        int sampleRate, int hopSize, bool complete,
        bool bufferReset, quint64 trackToken = 0,
        bool clearHistoryOnReset = false,
        quint64 generation = 0,
        int sessionWidth = 0,
        int fftSize = 0,
        quint64 trackFrames = 0,
        int endColumn = 0);
    Q_INVOKABLE void clearPrecomputed();

    Q_INVOKABLE void reset();
    Q_INVOKABLE void halt();
    Q_INVOKABLE void appendRows(const QVariantList &rows);

    Q_INVOKABLE double minimumZoomLevel() const;
    // The deepest zoom the wheel goes to: 16, or the default zoom where that
    // is deeper.
    double maximumZoomLevel() const;

    // Frees the ring and the canvas, which nothing uses while analysis is
    // off because no pane is shown, and takes no columns until retained:
    // showing a view starts a fresh analysis session, which clears them
    // anyway. BridgeClient, which turns analysis off and on, calls these.
    void releaseStorage();
    // Takes columns again from the next session's reset on; nothing older
    // may reach it (see BridgeClient::staleSpectrogramFrame).
    void retainStorage();
    bool storageReleased() const;

    static SpectrogramSceneStatistics sceneStatistics(const QSGNode *node);
    // The canvas textures of a scene node, with the canvas column each
    // starts at. For tests.
    static QVector<QPair<int, QSGTexture *>> canvasTextures(const QSGNode *node);

signals:
    void dbRangeChanged();
    void logScaleChanged();
    void showFpsOverlayChanged();
    void reservedCornerChanged();
    void sampleRateHzChanged();
    void maxColumnsChanged();
    void clockChanged();
    void positionSecondsChanged();
    void playingChanged();
    void precomputedReadyChanged();
    void displayModeChanged();
    void crosshairEnabledChanged();
    void crosshairOpacityChanged();
    void crosshairReadoutChanged();
    void showCrosshairTimeChanged();
    void laneHeadBoundsChanged();
    void readoutColorsChanged();
    void markerSecondsChanged();
    void markerXChanged();
    void gridEnabledChanged();
    void analysisFftSizeChanged();
    void frequencyCapChanged();
    void showTimeLabelsChanged();
    void crosshairSharedXChanged();
    void crosshairHoverChanged(double x);
    void channelMutedChanged();
    void playheadVisibleChanged();
    void seekRequested(double seconds);
    void zoomLevelChanged();
    void zoomEnabledChanged();
    void stretchShortTracksChanged();
    void defaultZoomLevelChanged();
    void maximumZoomLevelChanged();
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
    // The columns of a canvas texture tile for the software renderer, whose
    // textures cannot be updated in place: each is made again when one of
    // its columns changes.
    static constexpr int kCanvasTileWidth = 64;

    // The track columns that centered mode shows: the playhead in the
    // middle, unless that would show past either end of the track, or the
    // whole track when it nearly fits. The decoder models this view
    // (src/analysis/centered_view.rs); both are tested against
    // src/analysis/fixtures/centered-view.txt.
    struct CenteredDisplayRange {
        qint64 left{0};
        qint64 right{-1};
        bool decodedTailLooksFinal{false};
    };
    static CenteredDisplayRange centeredDisplayRange(
        qint64 nowCol,
        int visibleWindowCols,
        qint64 maxColCount,
        qint64 estimateCount,
        double columnsPerSecond);
    // The view a centered frame shows with the playhead at `positionSeconds`:
    // centeredDisplayRange, held at the canvas's range while that is about
    // the same, so that the view does not jitter. The paint draws it, and a
    // click seeks to where it shows.
    struct CenteredView {
        qint64 left{0};
        qint64 right{-1};
        bool decodedTailLooksFinal{false};
        // Whether it scrolls under the playhead, by part of a column.
        bool scrolls{false};
        // Where display column `left` starts, in canvas pixels.
        double drawX{0.0};
    };
    // The track's columns as a centered view reckons them: the session's
    // end once it is known, where the decoder's views end too, and else
    // the column estimate (`end`, 0 if neither is known). Until the
    // columns decoded reach a known end, they say nothing of where the
    // track ends, and maxColCount is 0: the view's end-of-decode clamp is
    // for the estimate's overshoot.
    struct CenteredColumns {
        qint64 maxColCount{0};
        qint64 end{0};
        bool endKnown{false};
    };
    CenteredColumns centeredColumnsLocked() const;
    CenteredView centeredViewLocked(
        double positionSeconds,
        int canvasWidth,
        int canvasHeight,
        double canvasZoom,
        double columnsPerSecond) const;

    void rebuildPalette();
    void invalidateMapping();
    void ensureMapping(int height);
    void invalidateCanvas();
    void ensureCanvas(int width, int height);
    bool reanchorRollingEpochForCurrentDataLocked(
        std::chrono::steady_clock::time_point now);
    void updateCanvasFillReadinessLocked();
    // Whether the ring holds every column of the current track from
    // firstColumn to lastColumn.
    bool ringHoldsColumnsLocked(qint64 firstColumn, qint64 lastColumn) const;
    void applyPrecomputedResetLocked(
        int startIndex,
        int bins,
        quint64 trackToken,
        quint64 generation,
        bool clearHistoryOnReset,
        int hopSize);
    // Whether a session reset of trackToken, hopSize frames a column, is
    // the backend's restart for the zoom being waited for: at the hop of
    // the zoom asked for or of one sent to the backend since, of the track
    // the zoom was asked of or the one it goes on in gaplessly, with the
    // clock's playback unbroken since (its epoch). Only then is what the
    // view showed before it of the times the reset's columns are.
    bool resetRestartsForZoomLocked(quint64 trackToken, int hopSize) const;
    qint64 currentRollingDisplayRightLocked(std::chrono::steady_clock::time_point now) const;
    void truncateRollingTailLocked(qint64 newWriteSeq);
    double currentRenderPositionSecondsLocked(std::chrono::steady_clock::time_point now) const;
    // The track seconds the shown position has moved on continuously up to
    // `now`: a pause adds nothing, and nor does a jump, such as a seek.
    double playbackTravelSecondsLocked(std::chrono::steady_clock::time_point now) const;
    // Counts the travel up to `now`; done before the shown position changes
    // course, so that the new course does not count the time before it.
    void foldPlaybackTravelLocked(std::chrono::steady_clock::time_point now);
    bool positionHeldLocked(qint64 nowNs) const;
    void clockStateChanged();
    void holdPositionLocked(
        const PlaybackClockState &trajectory,
        double offset,
        std::chrono::steady_clock::time_point now);
    void releasePositionHoldLocked(std::chrono::steady_clock::time_point now);
    void continueGaplessLocked(std::chrono::steady_clock::time_point now);
    // At canvasZoom pixels a column, or else the canvas's zoom, of the
    // ring's columns from heldLeft to heldRight; any others are missing.
    std::shared_ptr<SpectrogramRasterJob> makeRasterJobLocked(
        int width,
        int height,
        qint64 displayLeft,
        qint64 displayRight,
        bool rollingMode,
        double canvasZoom = 0.0,
        qint64 heldLeft = std::numeric_limits<qint64>::min(),
        qint64 heldRight = std::numeric_limits<qint64>::max());
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
    // Keeps the canvas, which one with columns missing replaces, as the
    // backdrop, unless it has columns missing too and there is one.
    void keepCanvasAsBackdropLocked(const SpectrogramRasterTarget &replacement);
    void dropBackdropLocked();
    void dropReserveLocked();
    // Drops both, and the reserve being drawn: their times are not the
    // view's any more.
    void invalidateBackdropLocked();
    // Draws the ring's columns from `seconds` to `endSeconds`, at most
    // `pixelsPerSecond` canvas pixels a second and `width` wide, on a
    // worker, for the reserve.
    void requestReserveLocked(double seconds, double endSeconds, double pixelsPerSecond,
        int width, int height, bool rollingMode);
    // Before a zoom's session clears the ring: its columns around the
    // canvas's view, for the reserve.
    void reserveRingBeforeResetLocked();
    void installReserve(const std::shared_ptr<SpectrogramRasterResult> &result);
    // Whether the canvas, as it is, shows every column of the view from
    // displayLeft to displayRight, canvasZoom pixels a column, cw wide.
    bool canvasShowsWholeViewLocked(
        qint64 displayLeft, qint64 displayRight, bool rollingMode, double canvasZoom, int cw) const;
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
    int maxSyncAppendPixelsLocked(
        int canvasWidth, double canvasZoom, std::chrono::steady_clock::time_point now) const;
    bool advancePrecomputedCanvasLocked(
        qint64 displayLeft,
        qint64 displayRight,
        bool rollingMode,
        std::chrono::steady_clock::time_point now);
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
    void resizeCanvasDamageLocked();
    void markCanvasColumnDirtyLocked(int x);
    void markCanvasDirtyLocked();
    // What of the canvas the scene node is to upload: the columns changed
    // since the last frame took them, or all of it. With a tileWidth, the
    // whole tiles of that many columns they are in; with 0, those columns.
    SpectrogramCanvasUpload takeCanvasUploadLocked(int tileWidth, bool all);
    void updateOverlayImageLocked();
    void updateCrosshairOverlayLocked(
        int width, int height,
        qint64 displayLeft, bool rollingMode,
        double columnsPerSecond, double drawX);
    // Hands the readout laid out to the GUI thread, if it changed.
    void publishCrosshairReadoutLocked(const QString &readout);
    // The width of the widest frequency and level the readout shows, in its
    // font for the top frequency of the band (see CrosshairReadout::plateRects).
    int crosshairWidestValueWidthLocked();
    // The time in its own track of `continuousTime` on the view's
    // timeline, or NaN where it is in no track the view knows.
    double crosshairTrackTimeLocked(double continuousTime) const;
    void setReadoutColor(QColor CrosshairReadout::Colors::*member, const QColor &value);
    // The device column (or row) of `logical`, clamped to the overlay.
    int crosshairDeviceColumnLocked(double logical, int devicePixelCount) const;
    // Names the layout's images' textures, at the canvas scale.
    void nameOverlayImagesLocked(SpectrogramOverlayLayout &layout) const;
    void updateFreqGridOverlayLocked(int width, int height);
    // How far below the lane's top the frequency scale's labels start, in
    // logical pixels.
    int frequencyScaleTopClearanceLocked() const;
    // The lane at rest, without columns: a faint line across it at each
    // tick of the frequency scale, and with the scale shown, its labels,
    // where a session at the analysis's settings and the lane's rate will
    // put them.
    void updateRestGridOverlayLocked(int width, int height);
    void updateTimeGridOverlayLocked(
        int width, int height, int padding,
        qint64 displayLeft,
        bool rollingMode, double columnsPerSecond, double drawX);
    double effectiveZoomLocked() const;
    // The same for data `hop` samples apart drawn at `renderZoom`
    // (ferrous_ffi_spectrogram_effective_zoom); `renderZoom` until a hop
    // is known.
    double effectiveZoomForHopLocked(int hop, double renderZoom) const;
    // The session's column hop at zoom 1, in samples: what zoom levels
    // are relative to, for the rate of the session's data.
    double referenceHopLocked() const;
    // Logical pixels a second of audio is wide at the render zoom; 0 until
    // a rate is known.
    double currentPixelsPerSecondLocked() const;
    // Canvas pixels per display column.
    double canvasZoomLocked() const;
    // The same for data `hop` samples apart drawn at `renderZoom`: the
    // effective zoom, then the canvas's scale. In the other order its
    // rounding can differ, and with it the columns a view shows.
    double canvasZoomForHopLocked(int hop, double renderZoom) const;
    int canvasWidthLocked() const;
    int canvasHeightLocked() const;
    // The window's device pixel ratio, or the current one without a window.
    double windowDevicePixelRatio() const;
    void applyCanvasScaleLocked(double scale);
    double minimumZoomLevelLocked() const;
    double maximumZoomLevelLocked() const;
    // The zoom at which the track's columns fill the view
    // (ferrous_ffi_spectrogram_fit_zoom), of the frames its data tells or
    // else the clock's duration of the same track; below 0 while its length,
    // rate, FFT size or the width is unknown.
    double fitZoomLocked() const;
    // Brings the default zoom up to date; true if it changed. While the
    // track's length is unknown, it stays, unless the data is of a new track
    // (`trackChanged`), whose default is 1 until its length is known.
    bool refreshDefaultZoomLocked(bool trackChanged);
    // The column hop of the default zoom's data, and the zoom it is drawn
    // at, for audio at `sampleRate` (the data's rate if 0): the reference
    // hop and 1 at zoom 1.
    int defaultZoomHopLocked(int sampleRate) const;
    double defaultRenderZoomLocked(int sampleRate) const;
    // A track change's reset of the zoom to the default: until the data at
    // its hop arrives, the data on hand is drawn as if it were.
    void resetZoomToDefaultLocked();
    // Tells of a new default zoom, and brings a zoom deeper than the new
    // limit back within it. Without the state locked.
    void publishDefaultZoomChange();
    double backendZoomForRequestLocked() const;
    int columnHopForZoomLocked(double zoom) const;
    double renderZoomForZoomLocked(double zoom) const;
    // The zoom data hop samples apart is drawn at when zoom was asked for
    // (ferrous_ffi_spectrogram_render_zoom).
    double renderZoomForHopLocked(int hop, double zoom) const;
    bool zoomPreviewWantedLocked() const;
    // The centered view that the data for the requested zoom will show, as
    // the zoom preview places it before that data arrives, in canvas pixels
    // `width` wide (for a file that does not tell its length, once its
    // session has decoded the end):
    // canvas pixels per column, the window's columns, the range shown and
    // the column the playhead is in. Not valid until the requested zoom's
    // hop and the columns a second are known.
    struct ZoomTargetView {
        bool valid{false};
        double canvasZoom{0.0};
        int windowColumns{0};
        CenteredDisplayRange range;
        qint64 playheadColumn{0};
    };
    ZoomTargetView centeredZoomTargetLocked(
        int width,
        double positionSeconds,
        double columnsPerSecond) const;
    SpectrogramZoomPreview zoomPreviewLocked(
        int width,
        double positionSeconds,
        double drawX,
        bool rollingMode,
        double columnsPerSecond) const;
    double pixelToFrequencyHzLocked(int pixelY, int viewHeight) const;
    int frequencyToPixelYLocked(double freqHz, int viewHeight) const;
    // The width of a bin, in Hz, of the columns the mapping is for.
    double binHzLocked() const;
    // The frequency of the top bin those columns hold: Nyquist, or with the
    // frequency cap, the cap.
    double topFrequencyHzLocked() const;
    // Whether the ring's columns have the bin count the projection reads:
    // the rasterizers draw ring columns only then.
    bool ringMatchesProjectionLocked() const;
    std::vector<quint8> rowToIntensity(const QVariantList &row) const;
    void bindWindowFpsTracking(QQuickWindow *window);
    void handleWindowAfterAnimating();
    // Asks for the next frame of playback, now or once it would differ
    // from the last one.
    void schedulePlaybackFrame();
    void requestPlaybackFrame() override;
    void cancelPlaybackFrame();
    // Whether anyone would see a frame of playback: the item is visible,
    // and its window, if it has one, is on screen (see WindowPresence).
    // Playback frames are only asked for then.
    bool showsPlaybackFrames() const;
    // Its window came on screen or left it.
    void windowPresenceChanged();
    void releaseStorageLocked();
    // What a data chunk could change of a frame besides its columns.
    struct ShownDataState {
        int totalColumnsEstimate{0};
        int endColumn{0};
        bool tailLooksFinal{false};
        bool ready{false};
        bool canvasDirty{false};
        quint64 trackToken{0};
        quint64 ringGeneration{0};
        qint64 rollingEpoch{0};
        int hopSize{0};
        int sampleRateHz{0};
        int binsPerColumn{0};
        int fftSize{0};
        bool zoomFillActive{false};
        bool awaitingZoomData{false};
        bool resetPending{false};
        bool operator==(const ShownDataState &) const = default;
    };
    ShownDataState shownDataStateLocked() const;
    // Whether columns written from ring sequence firstSeq, track columns
    // from firstColumn, leave the last frame as it is, so that it need not
    // be drawn again before its own next change. maxColumnBefore is the
    // last decoded column before them.
    bool chunkLeavesLastFrameLocked(const ShownDataState &before,
        qint64 firstSeq,
        qint64 firstColumn,
        qint64 maxColumnBefore) const;
    // The window's scheduler, or the item's own outside a window.
    PlaybackFrameScheduler *frameScheduler();
    // How long until the last frame would change, in nanoseconds from
    // nowNs: zero for now, or -1 for not while the clock keeps its course.
    qint64 playbackFrameDelayNsLocked(qint64 nowNs) const;
    void updateFpsEstimateLocked();
    void resetFpsEstimateLocked();
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
    // The rate the frequency mapping, grid and crosshair use: the analysis
    // session's once it is known, else playback's.
    int m_sampleRateHz{48000};
    // Playback's latest rate, from the sampleRateHz property. A decoder can
    // fall back to another rate for single frames; the session's columns
    // keep theirs, so this only stands in until a session rate is known.
    int m_playbackSampleRateHz{48000};
    int m_maxColumns{640};
    int m_binsPerColumn{0};
    // The FFT size of the columns the mapping is for, or 0 when their bins
    // are the whole spectrum of a (m_binsPerColumn - 1) × 2 point FFT.
    int m_fftSize{0};

    std::array<QRgb, kGradientTableSize> m_palette32{};
    std::array<QRgb, kGradientTableSize> m_palette32Gray{};
    bool m_channelMuted{false};
    bool m_playheadVisible{true};
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
    SpectrogramCanvasDamage m_canvasDamage;
    // Copies a rasterization made of the canvas it delivered, for the render
    // thread to upload instead of copying the canvas itself: of tiles
    // m_rasterTileWidth columns wide, a tile drawn into since then being
    // copied again; or, for a width of 0, one copy of all of it, which the
    // columns drawn into since then are uploaded over.
    QVector<QImage> m_rasterTiles;
    int m_rasterTileWidth{0};
    // The columns of the scene graph's canvas texture tiles, for the copies
    // rasterizations make: 0 when it updates one texture in place.
    int m_sceneTileWidth{0};
    // The largest canvas texture to make, below OpenGL's limit; for tests,
    // 0 for that limit.
    int m_canvasTextureSizeLimit{0};
    // What the scene node drawn last did, for tests of windows that draw it.
    SpectrogramSceneStatistics m_lastSceneStatistics;
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
    // What the canvas's projection is: a backdrop of it stands in only for
    // a canvas of the same.
    std::shared_ptr<const SpectrogramProjection> m_canvasProjection;
    // Whether the canvas showed every column of its view when it was last
    // known; a canvas with columns missing does not replace the backdrop.
    bool m_canvasComplete{false};
    // The backdrop and the reserve (SpectrogramBackdrop), null without
    // one, and what tells their images apart.
    SpectrogramBackdrop m_backdrop;
    SpectrogramBackdrop m_reserve;
    quint64 m_backdropSerial{0};
    // The reserve being drawn: its serial, whether it is under way, the
    // canvas pixels a second of the zoom it was last drawn for (0 for
    // none), and the seconds last asked of it. And the validity of what is
    // drawn, which changes when its times stop being the view's.
    std::shared_ptr<SpectrogramRasterMailbox> m_reserveMailbox;
    quint64 m_reserveSerial{0};
    bool m_reservePending{false};
    double m_reserveZoomPixelsPerSecond{0.0};
    double m_reserveRequestedLeft{0.0};
    double m_reserveRequestedRight{-1.0};
    quint64 m_backdropValidity{0};
    // What the zoom being waited for was asked of: the clock's epoch and
    // the data's track then, carried over a gapless or Repeat-One
    // continuation, and the column hops of the zooms sent to the backend
    // since.
    quint64 m_zoomClockEpoch{0};
    quint64 m_zoomTrackToken{0};
    std::vector<int> m_zoomRequestHops;
    // A rolling view's times start over at each session reset, when the
    // offset of a gapless continuation (m_gaplessPositionOffset) goes:
    // the sum of those offsets, which the layers' times are counted from,
    // and the canvas's when it was rasterized.
    double m_streamOrigin{0.0};
    double m_precomputedCanvasStreamOrigin{0.0};
    quint64 m_precomputedCanvasValidity{0};
    // Changes whenever the ring's columns are cleared or renumbered, which
    // makes a rasterization of them obsolete.
    quint64 m_ringGeneration{0};
    bool m_seedHistoryOnNextAppend{true};
    bool m_animationTickInitialized{false};
    std::chrono::steady_clock::time_point m_lastAnimationTick{};
    bool m_forceFpsOverlay{false};
    bool m_showFpsOverlay{false};
    QSizeF m_reservedCorner;
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
    QPointer<WindowPresence> m_windowPresence;
    QMetaObject::Connection m_windowPresenceConnection;

    // Its ring and canvas are released while analysis is off, and it takes
    // no columns then.
    bool m_storageReleased{false};

    // Ring-buffer spectrogram storage.
    SpectrogramRingStorage m_ring;    // capacity columns of bins_per_column bytes
    std::vector<qint32> m_ringColumnId; // per write-order slot: track column index, or -1
    std::vector<qint64> m_ringSequenceId; // per slot: write sequence stored there, or -1
    std::vector<quint64> m_ringTrackToken; // per slot: track token stored there, or 0
    QHash<quint64, QHash<qint32, qint64>> m_trackColumnToSeqByToken;
    // A centered session's columns go to the write sequence of their
    // offset from its first column, m_centeredColumnBase, so that a session
    // that sends the playhead's columns before those left of them, as the
    // worker does after a seek, has each where it belongs, and the ring
    // drops the columns furthest left first. -1 when the ring is written
    // in order of arrival; the track of those columns, or 0 for any.
    qint64 m_centeredColumnBase{-1};
    quint64 m_centeredColumnToken{0};
    int m_ringCapacity{0};            // number of column slots
    qint64 m_ringWriteSeq{0};         // next write-order sequence number
    qint64 m_ringOldestSeq{0};        // oldest write-order sequence still retained
    qint64 m_trackEpochSeq{0};        // legacy reset bookkeeping
    qint64 m_rollingEpoch{0};         // maps track columns to rolling write-order history
    qint32 m_precomputedMaxColumnIndex{-1}; // highest column for current token
    bool m_awaitingWorkerReset{false};    // block data until worker's proper reset
    int m_precomputedBinsPerColumn{0};
    // The FFT size of the analysis columns (0 if unknown).
    int m_precomputedFftSize{0};
    int m_precomputedTotalColumnsEstimate{0};
    // The frames of the file the session's columns are of (0 if unknown):
    // where a session at another zoom ends (decodedEndColumnForZoomLocked).
    quint64 m_precomputedTrackFrames{0};
    // The column after the session's last, as the decoder reckons it (0 if
    // unknown): where the centered view ends, in place of the column
    // estimate, which runs past it by an allowance that sizes buffers.
    int m_precomputedEndColumn{0};
    int m_precomputedSampleRateHz{44100};
    // Whether an analysis chunk has set m_precomputedSampleRateHz.
    bool m_precomputedSampleRateKnown{false};
    int m_precomputedHopSize{1024};
    quint64 m_precomputedTrackToken{0};
    quint64 m_precomputedCommittedToken{0}; // token from the most recent buffer_reset
    quint64 m_precomputedSessionGeneration{0};
    quint64 m_precomputedCommittedGeneration{0};
    bool m_precomputedReady{false};
    // The playhead's position when the clock or the data last changed it.
    double m_positionSeconds{0.0};
    QPointer<PlaybackClock> m_clock;
    // Now on the clock playback is stamped with; tests set their own.
    std::function<std::chrono::steady_clock::time_point()> m_now{
        &std::chrono::steady_clock::now};
    QMetaObject::Connection m_clockConnection;
    // A copy of the clock's state, which the render thread evaluates.
    PlaybackClockState m_clockState;
    // The playback travel up to m_playbackTravelAt; the shown position's
    // course since then adds the rest.
    double m_playbackTravelSeconds{0.0};
    std::chrono::steady_clock::time_point m_playbackTravelAt{};
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
    // A zoom's restart of the successor came while it was pending: its
    // columns and the times from them start at the previous track's end
    // already, and m_gaplessPositionOffset places that track before them.
    bool m_gaplessContinuationRebased{false};
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
    bool m_stretchShortTracks{true};
    double m_defaultZoomLevel{1.0};
    // Canvas freeze during zoom transition.  Independent of
    // m_awaitingZoomData (which controls the zoom snap).  Set when a
    // session restart changes the hop; cleared when the ring covers
    // the display.  updatePaintNode skips canvas updates while active.
    bool m_zoomFillActive{false};
    // The freeze is over a seek, not a zoom: it ends once the ring holds
    // the playhead's column, and the rest of the view fills in as it comes.
    bool m_seekFillActive{false};
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
    // The playback travel when the canvas's columns were last brought up
    // to the view.
    double m_precomputedCanvasDrawnTravelSeconds{0.0};
    // The scaled canvas drawn last, so that clicks seek where it shows:
    // the track time at anchor X, and pixels per second.
    bool m_zoomPreviewShown{false};
    // The last frame stays as it is until the playhead reaches
    // changeSeconds, if valid: when a centered view that does not scroll
    // moves its playhead to the next pixel or starts to scroll, and when a
    // view that scrolls reaches its next pixel or column.
    // Columns arriving after the last ones it shows, shownRight (a ring
    // sequence when rolling, a track column when centered), do not change
    // it either.
    struct IdleView {
        bool valid{false};
        double changeSeconds{0.0};
        bool rolling{false};
        qint64 shownLeft{0};
        qint64 shownRight{-1};
    };
    IdleView m_idleView;
    // Wakes playback frames until then; see schedulePlaybackFrame.
    QPointer<PlaybackFrameScheduler> m_frameScheduler;
    // How many playback frames the item has asked for, for tests.
    quint64 m_playbackFrameRequests{0};
    // How many frames steady data chunks have asked for, for tests.
    quint64 m_dataFrameRequests{0};
    double m_zoomPreviewAnchorSeconds{0.0};
    double m_zoomPreviewAnchorX{0.0};
    double m_zoomPreviewPixelsPerSecond{0.0};

    // Crosshair overlay state.
    bool m_crosshairEnabled{false};
    double m_crosshairOpacity{1.0};
    bool m_hoverActive{false};
    QPointF m_hoverPosition;
    double m_crosshairSharedX{-1.0};
    // Its dotted lines are images; their rects below are in device pixels,
    // its labels' in logical ones.
    SpectrogramOverlayLayout m_crosshair;
    bool m_crosshairDirty{true};
    qint64 m_crosshairCachedDisplayLeft{0};
    double m_crosshairCachedDrawX{0.0};
    bool m_crosshairCachedRollingMode{false};
    int m_crosshairCachedBinsPerColumn{0};
    QRect m_crosshairVerticalLineRect;
    QRect m_crosshairHorizontalLineRect;
    bool m_showCrosshairTime{true};
    QRectF m_laneHeadBounds;
    // UiPalette's spectrogramPlot plates, until QML sets them.
    CrosshairReadout::Colors m_readoutColors{
        QColor(20, 20, 20, 224), QColor(0xec, 0xec, 0xec), QColor(255, 255, 255, 36)};
    // The readout laid out with the crosshair, and the one the GUI thread
    // has (crosshairReadout).
    QString m_crosshairLaidOutReadout;
    QString m_crosshairReadout;
    // crosshairWidestValueWidthLocked's, for the font and top frequency.
    CrosshairReadout::WidestWidth m_crosshairWidestValue;
    double m_markerSeconds{-1.0};
    // The marker's x as the render thread laid it out, and as the GUI
    // thread shows it.
    double m_markerXLaidOut{std::numeric_limits<double>::quiet_NaN()};
    double m_markerX{std::numeric_limits<double>::quiet_NaN()};
    void publishMarkerX(double x);

    // Grid overlay state — split into static frequency grid and dynamic time grid.
    bool m_gridEnabled{false};
    // How far from the right edge the frequency scale's labels reach, in
    // logical pixels; 0 without any.
    int m_freqScaleWidth{0};
    bool m_showTimeLabels{false};

    // Frequency grid: horizontal lines + labels. Only depends on
    // height/sampleRate/binsPerColumn/logScale. Never laid out again during
    // playback.
    SpectrogramOverlayLayout m_freqGrid;
    bool m_freqGridDirty{true};
    // The lane at rest (updateRestGridOverlayLocked), and the analysis's
    // settings it is laid out for: the app's default FFT size until told.
    SpectrogramOverlayLayout m_restGrid;
    int m_analysisFftSize{8192};
    bool m_frequencyCap{false};
    int m_freqGridCachedSampleRateHz{0};
    int m_freqGridCachedBinsPerColumn{0};

    // Time grid: vertical lines + labels (bottom pane only). Laid out
    // wider than the widget, with right-side padding, from where the view
    // was then; each frame shifts it by how far the view has moved since,
    // and it is laid out again once the shift exhausts the padding.
    SpectrogramOverlayLayout m_timeGrid;
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
