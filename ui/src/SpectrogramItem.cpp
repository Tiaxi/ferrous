// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramItem.h"
#include "FerrousBridgeFfi.h"

#include "SpectrogramSeekTrace.h"
#include "SpectrogramTraceLogging.h"

#include <QFont>
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
#include <QSet>
#include <QString>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>
#include <utility>

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
#define FERROUS_SPECTROGRAM_LOGF(...)               \
    do {                                            \
        if (SpectrogramTraceLogging::detailedEnabled()) { \
            std::fprintf(__VA_ARGS__);              \
        }                                           \
    } while (false)
#else
#define FERROUS_SPECTROGRAM_LOGF(...) \
    do {                              \
    } while (false)
#endif

// Immutable frequency and color projection shared by equivalent channel panes.
// A draw pass holds its own reference, so later setting changes cannot mutate
// the inputs used by that pass. The global cache retains only weak references.
struct SpectrogramProjection {
    using Key = std::tuple<int, int, int, int, int, bool, double, bool>;
    Key key;
    std::vector<std::pair<int, int>> binRanges;
    std::array<QRgb, 256> colors;
};

// Maps canvas x positions to where a zoom preview shows them: scaled by
// `scale` about `fromX`, which lands at `toX`.
struct SpectrogramZoomPreview {
    bool active{false};
    double scale{1.0};
    double fromX{0.0};
    double toX{0.0};

    double map(double x) const {
        return active ? toX + (x - fromX) * scale : x;
    }
};

// How rasterizations reach their item, which may be gone by then. The item
// clears `owner` when it is destroyed; `latestSerial` is the one
// rasterization whose result it still wants.
struct SpectrogramRasterMailbox {
    QMutex mutex;
    SpectrogramItem *owner{nullptr};
    std::atomic<quint64> latestSerial{0};
};

namespace {
constexpr double kMinFreqHz = 25.0;
constexpr double kReferenceHopSamples = 1024.0;
constexpr double kPositionJumpHoldThresholdSeconds = 0.75;
// Last-resort fallback only: the hold is normally released by
// applyPrecomputedResetLocked() when the reset data arrives (~200–400 ms
// after a seek or track change).  The timeout must be long enough that the
// data-driven release always wins, while still being short enough to
// recover if the backend never sends a reset (e.g. stream interruption).
constexpr double kPositionJumpHoldTimeoutSeconds = 2.0;
// Canvas spans the scene node starts with; a wider canvas adds more.
constexpr int kInitialTileFragments = 96;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
constexpr qint64 kSeekTraceWindowMs = 1800;
constexpr qint64 kSmoothnessWindowMs = 2000;
constexpr qint64 kSmoothnessIdleMs = 450;
#endif
const QColor kBackgroundColor(0, 0, 0);
const QColor kOverlayColor(190, 190, 200, 150);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
std::atomic<quint64> g_lastLoggedSeekProfileIncidentGeneration{0};
std::atomic<quint64> g_lastLoggedSeekProfileSummaryGeneration{0};

bool claimMonotonicGeneration(std::atomic<quint64> *lastClaimed, quint64 generation) {
    quint64 previous = lastClaimed->load(std::memory_order_relaxed);
    while (generation > previous) {
        if (lastClaimed->compare_exchange_weak(
                previous,
                generation,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}
#endif

bool centeredDecodedTailLooksFinal(
    qint64 decodedColumnCount,
    qint64 estimatedColumnCount,
    double columnsPerSecond) {
    if (decodedColumnCount <= 0 || estimatedColumnCount <= 0) {
        return false;
    }
    // The decoded tail only "looks final" when the gap to the estimate is
    // small in TIME: the estimate overshoots real audio by trailing
    // container padding (~1 s) and the finalize chunk shrinks the
    // estimate to the decoded extent at EOF anyway.  The tolerance must
    // therefore scale with the column RATE, not with the visible window:
    // a window-scaled slack was tens of seconds wide at deep zoom-out, so
    // this clamp fired during zoom-out refills and dragged the viewport
    // along with the decode head (the decoded tail lagging the playhead
    // makes the second condition below trivially true mid-fill).
    const qint64 eofSlackColumns = columnsPerSecond > 0.0
        ? std::max<qint64>(
              1, static_cast<qint64>(std::ceil(columnsPerSecond * 2.0)))
        : 1;
    return decodedColumnCount + eofSlackColumns >= estimatedColumnCount;
}

// Centered mode shows the whole track once its window covers this many
// percent of it: see SpectrogramItem::centeredDisplayRange.
constexpr qint64 kCenteredWholeTrackPercent = 90;

// Whether a centered range scrolls smoothly under the playhead.  Near the
// whole track, at its ends, and once clamped to the decoded end it stays
// still and the playhead moves across it instead; sub-pixel scrolling
// would make it "swim".
bool centeredRangeScrolls(
    qint64 left,
    qint64 right,
    qint64 maxColCount,
    qint64 estimateCount,
    bool decodedTailLooksFinal) {
    const qint64 totalCols = std::max(maxColCount, std::max<qint64>(estimateCount, 1));
    const qint64 span = right - left + 1;
    const bool nearFullTrack =
        totalCols > 0 && span * 100 / totalCols >= kCenteredWholeTrackPercent;
    const bool eofClampedToDecodedContent =
        decodedTailLooksFinal && maxColCount > 0 && right >= maxColCount - 1;
    return !nearFullTrack
        && left > 0
        && !eofClampedToDecodedContent
        && right < totalCols - 1;
}

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

bool configureTextureNode(
    QSGSimpleTextureNode *node,
    QSGTexture *texture,
    const QRectF &target,
    const QRectF &source,
    QSGTexture *placeholderTexture);

// An overlay image placed for a frame, with the image itself when the scene
// node has no texture of it yet.
struct SpectrogramPlacedOverlayImage {
    QString key;
    QRectF rect;
    QImage image;
};

// What an overlay draws in a frame, in logical pixels.
struct SpectrogramOverlayFrame {
    QVector<QRectF> lines;
    QVector<SpectrogramPlacedOverlayImage> images;
};

// An overlay's nodes: rectangles for its lines, then its images above them.
struct SpectrogramOverlayLayer {
    QSGNode *root{nullptr};
    QSGNode *linesRoot{nullptr};
    QSGNode *imagesRoot{nullptr};
    QVector<QSGSimpleRectNode *> lines;
    QVector<QSGSimpleTextureNode *> images;

    SpectrogramOverlayLayer() {
        root = new QSGNode();
        linesRoot = new QSGNode();
        imagesRoot = new QSGNode();
        root->appendChildNode(linesRoot);
        root->appendChildNode(imagesRoot);
    }
};

struct SpectrogramSceneNode final : public QSGNode {
    SpectrogramSceneNode() {
        background = new QSGSimpleRectNode();
        tilesRoot = new QSGNode();
        latest = new QSGSimpleTextureNode();
        playhead = new QSGSimpleRectNode();
        overlay = new QSGSimpleTextureNode();
        appendChildNode(background);
        appendChildNode(tilesRoot);
        appendChildNode(latest);
        appendChildNode(playhead);
        appendChildNode(freqGridLayer.root);
        appendChildNode(timeGridLayer.root);
        appendChildNode(crosshairLayer.root);
        appendChildNode(overlay);
        ensureTileFragments(kInitialTileFragments);
    }

    // Visible segments can outnumber source tiles by one, when the ring
    // buffer wraps inside a tile.
    void ensureTileFragments(int count) {
        tileFragments.reserve(count);
        while (tileFragments.size() < count) {
            auto *tileNode = new QSGSimpleTextureNode();
            // A renderer reads a node as soon as it joins its scene.
            if (placeholderTexture != nullptr) {
                tileNode->setTexture(placeholderTexture);
            }
            tilesRoot->appendChildNode(tileNode);
            tileFragments.push_back(tileNode);
        }
    }

    // Shows an overlay's frame, with textures of its images made from the
    // images given for them, or kept from earlier frames.
    void showOverlay(
        SpectrogramOverlayLayer &layer,
        const SpectrogramOverlayFrame &frame,
        const QColor &lineColor,
        QQuickWindow *window) {
        while (layer.lines.size() < frame.lines.size()) {
            auto *line = new QSGSimpleRectNode();
            layer.linesRoot->appendChildNode(line);
            layer.lines.push_back(line);
        }
        for (int i = 0; i < layer.lines.size(); ++i) {
            QSGSimpleRectNode *line = layer.lines[i];
            const QRectF rect = i < frame.lines.size() ? frame.lines[i] : QRectF();
            if (line->color() != lineColor) {
                line->setColor(lineColor);
            }
            if (line->rect() != rect) {
                line->setRect(rect);
            }
        }
        int shown = 0;
        for (const SpectrogramPlacedOverlayImage &placed : frame.images) {
            QSGTexture *texture = overlayTextures.value(placed.key);
            if (texture == nullptr && !placed.image.isNull() && window != nullptr) {
                texture = window->createTextureFromImage(placed.image);
                if (texture != nullptr) {
                    texture->setFiltering(QSGTexture::Nearest);
                    statistics.overlayTextures += 1;
                    statistics.overlayTextureBytes += placed.image.sizeInBytes();
                    overlayTextures.insert(placed.key, texture);
                }
            }
            if (texture == nullptr) {
                continue;
            }
            usedOverlayTextures.insert(placed.key);
            if (shown == layer.images.size()) {
                auto *imageNode = new QSGSimpleTextureNode();
                // A renderer reads a node as soon as it joins its scene.
                imageNode->setTexture(texture);
                layer.imagesRoot->appendChildNode(imageNode);
                layer.images.push_back(imageNode);
            }
            noteTextureNode(configureTextureNode(layer.images[shown++], texture, placed.rect,
                QRectF(QPointF(0.0, 0.0), texture->textureSize()), placeholderTexture));
        }
        for (int i = shown; i < layer.images.size(); ++i) {
            noteTextureNode(configureTextureNode(
                layer.images[i], nullptr, QRectF(), QRectF(), placeholderTexture));
        }
    }

    void noteTextureNode(bool changed) {
        statistics.textureNodeChanges += changed ? 1 : 0;
    }

    // Makes a texture of a canvas tile, or part of one.
    QSGTexture *createCanvasTexture(QQuickWindow *window, const QImage &image) {
        if (image.isNull() || window == nullptr) {
            return nullptr;
        }
        QSGTexture *texture = window->createTextureFromImage(image);
        if (texture != nullptr) {
            texture->setFiltering(QSGTexture::Nearest);
            statistics.canvasTextures += 1;
            statistics.canvasTextureBytes += image.sizeInBytes();
        }
        return texture;
    }

    // Deletes the overlay textures no overlay showed this frame.
    QVector<QSGTexture *> takeUnusedOverlayTextures() {
        QVector<QSGTexture *> unused;
        for (auto it = overlayTextures.begin(); it != overlayTextures.end();) {
            if (!usedOverlayTextures.contains(it.key())) {
                unused.push_back(it.value());
                it = overlayTextures.erase(it);
            } else {
                ++it;
            }
        }
        usedOverlayTextures.clear();
        return unused;
    }

    ~SpectrogramSceneNode() override {
        qDeleteAll(tileTextures);
        qDeleteAll(overlayTextures);
        delete overlayTexture;
        delete placeholderTexture;
    }

    QSGSimpleRectNode *background{nullptr};
    QSGNode *tilesRoot{nullptr};
    QSGSimpleTextureNode *latest{nullptr};
    QSGSimpleRectNode *playhead{nullptr};
    SpectrogramOverlayLayer freqGridLayer;
    SpectrogramOverlayLayer timeGridLayer;
    SpectrogramOverlayLayer crosshairLayer;
    QSGSimpleTextureNode *overlay{nullptr};
    QVector<QSGSimpleTextureNode *> tileFragments;
    QVector<QSGTexture *> tileTextures;
    QSGTexture *overlayTexture{nullptr};
    // The overlays' small images' textures, by their keys, and the ones
    // shown this frame.
    QHash<QString, QSGTexture *> overlayTextures;
    QSet<QString> usedOverlayTextures;
    QSGTexture *placeholderTexture{nullptr};
    quintptr ownerWindowId{0};
    quint64 generation{0};
    // What the last frame did.
    SpectrogramSceneStatistics statistics;
};

QImage placeholderImage() {
    QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    return image;
}

// Shows `source` of `texture` at `target`, or nothing. Only what differs
// from what the node shows is set: each change makes the renderer redo the
// node's geometry or material. Returns whether anything changed.
bool configureTextureNode(
    QSGSimpleTextureNode *node,
    QSGTexture *texture,
    const QRectF &target,
    const QRectF &source,
    QSGTexture *placeholderTexture) {
    if (node == nullptr) {
        return false;
    }
    QSGTexture *shown = texture;
    QRectF rect = target;
    QRectF sourceRect = source;
    if (texture == nullptr || target.isEmpty() || source.isEmpty()) {
        shown = placeholderTexture;
        rect = QRectF();
        sourceRect = QRectF(0.0, 0.0, 1.0, 1.0);
    }
    bool changed = false;
    if (shown != nullptr && node->texture() != shown) {
        node->setTexture(shown);
        changed = true;
    }
    if (node->filtering() != QSGTexture::Nearest) {
        node->setFiltering(QSGTexture::Nearest);
        changed = true;
    }
    if (node->rect() != rect) {
        node->setRect(rect);
        changed = true;
    }
    if (node->sourceRect() != sourceRect) {
        node->setSourceRect(sourceRect);
        changed = true;
    }
    return changed;
}

// Configures a texture node for a canvas span drawn through a zoom
// preview, cut to the item's width so that a zoomed-in preview does not
// draw over its neighbours. The target, the preview and the width are in
// canvas pixels, `pixelScale` to a logical pixel.
bool configurePreviewTextureNode(
    QSGSimpleTextureNode *node,
    QSGTexture *texture,
    const QRectF &target,
    const QRectF &source,
    const SpectrogramZoomPreview &preview,
    double itemWidth,
    double pixelScale,
    QSGTexture *placeholderTexture) {
    const auto logical = [pixelScale](const QRectF &rect) {
        return QRectF(rect.topLeft() / pixelScale, rect.size() / pixelScale);
    };
    if (!preview.active || target.isEmpty() || source.isEmpty()) {
        return configureTextureNode(node, texture, logical(target), source, placeholderTexture);
    }
    const double left = preview.map(target.left());
    const double right = preview.map(target.right());
    const double clippedLeft = std::max(0.0, left);
    const double clippedRight = std::min(itemWidth, right);
    if (clippedRight <= clippedLeft) {
        return configureTextureNode(node, nullptr, QRectF(), QRectF(), placeholderTexture);
    }
    const double sourcePerPixel = source.width() / (right - left);
    const QRectF clippedSource(
        source.left() + (clippedLeft - left) * sourcePerPixel,
        source.top(),
        (clippedRight - clippedLeft) * sourcePerPixel,
        source.height());
    return configureTextureNode(
        node,
        texture,
        logical(QRectF(clippedLeft, target.top(), clippedRight - clippedLeft, target.height())),
        clippedSource,
        placeholderTexture);
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

// Rasterizations run here, off the UI and render threads. Each item has at
// most one that it still wants; the others stop at their next check.
class SpectrogramRasterPool final : public QThreadPool {
public:
    SpectrogramRasterPool() {
        setObjectName(QStringLiteral("SpectrogramRaster"));
        setMaxThreadCount(std::clamp(QThread::idealThreadCount() / 2, 1, 4));
    }
};

QThreadPool &spectrogramRasterPool() {
    static SpectrogramRasterPool pool;
    return pool;
}

// The display columns a canvas draws from, by display index: nullptr where
// a column is missing.
template <typename Lookup>
struct SpectrogramColumns {
    Lookup lookup;
    const quint8 *column(qint64 displayIndex) const { return lookup(displayIndex); }
};

template <typename Lookup>
SpectrogramColumns<Lookup> spectrogramColumns(Lookup lookup) {
    return {std::move(lookup)};
}

struct CanvasColumn {
    QRgb *destination;
    qsizetype rowStride;
    int height;

    void set(int y, QRgb color) const {
        destination[static_cast<qsizetype>(y) * rowStride] = color;
    }
};

CanvasColumn canvasColumn(QImage &canvas, int x) {
    // Detach once per column, not once for every pixel written.
    return {
        reinterpret_cast<QRgb *>(canvas.bits()) + x,
        canvas.bytesPerLine() / static_cast<qsizetype>(sizeof(QRgb)),
        canvas.height()};
}

bool canvasColumnDrawable(
    const QImage &canvas, int x, const SpectrogramProjection &projection) {
    return !canvas.isNull()
        && x >= 0
        && x < canvas.width()
        && static_cast<int>(projection.binRanges.size()) == canvas.height();
}

void fillCanvasColumn(const CanvasColumn &column) {
    const QRgb background = kBackgroundColor.rgba();
    for (int y = 0; y < column.height; ++y) {
        column.set(y, background);
    }
}

// std::lround for a value that is not negative, without its library call:
// the same halves away from zero, as the blends below need many a frame.
int roundNonNegative(double value) {
    const int whole = static_cast<int>(value);
    return whole + (value - static_cast<double>(whole) >= 0.5 ? 1 : 0);
}

// The loudest bin of each canvas row of a display column, for the columns
// asked for last. At device pixels several pixels show one column, or blend
// it with its neighbour, and reduce its bins only once.
template <typename Source>
class SpectrogramRowMaxima {
public:
    // Only pixels that blend columns share them: `cache` is whether any do.
    SpectrogramRowMaxima(
        const Source &source, const SpectrogramProjection &projection, bool cache)
        : m_source(source)
        , m_projection(projection)
        , m_cache(cache) {}

    // nullptr where the column is missing.
    const quint8 *rows(qint64 displayIndex) {
        const Entry &entry = reduce(displayIndex, nullptr);
        return entry.present ? entry.rows.data() : nullptr;
    }

    // Draws the column into a canvas column, reducing it on the way if it
    // has not been yet. False where it is missing.
    bool draw(qint64 displayIndex, const CanvasColumn &out) {
        if (!m_cache) {
            return drawUncached(displayIndex, out);
        }
        const Entry &entry = reduce(displayIndex, &out);
        return entry.present;
    }

    const Source &source() const { return m_source; }
    const SpectrogramProjection &projection() const { return m_projection; }

private:
    struct Entry {
        qint64 index{0};
        bool filled{false};
        bool present{false};
        std::vector<quint8> rows;
    };

    bool drawUncached(qint64 displayIndex, const CanvasColumn &out) const {
        const quint8 *bins = m_source.column(displayIndex);
        if (bins == nullptr) {
            return false;
        }
        const auto &colors = m_projection.colors;
        for (int y = 0; y < out.height; ++y) {
            const auto [binLo, binHi] = m_projection.binRanges[static_cast<size_t>(y)];
            quint8 rawMax = 0;
            for (int b = binLo; b <= binHi; ++b) {
                rawMax = std::max(rawMax, bins[b]);
            }
            out.set(y, colors[static_cast<size_t>(rawMax)]);
        }
        return true;
    }

    const Entry &reduce(qint64 displayIndex, const CanvasColumn *out) {
        // Locals, which the byte stores below cannot alias.
        const QRgb *colors = m_projection.colors.data();
        QRgb *destination = out != nullptr ? out->destination : nullptr;
        const qsizetype rowStride = out != nullptr ? out->rowStride : 0;
        for (const Entry &entry : m_entries) {
            if (entry.index == displayIndex && entry.filled) {
                if (destination != nullptr && entry.present) {
                    const quint8 *rows = entry.rows.data();
                    const int height = out->height;
                    for (int y = 0; y < height; ++y) {
                        destination[static_cast<qsizetype>(y) * rowStride] = colors[rows[y]];
                    }
                }
                return entry;
            }
        }
        Entry &entry = m_entries[m_nextEntry];
        m_nextEntry = (m_nextEntry + 1) % m_entries.size();
        entry.index = displayIndex;
        entry.filled = true;
        const quint8 *bins = m_source.column(displayIndex);
        entry.present = bins != nullptr;
        if (!entry.present) {
            return entry;
        }
        const int height = static_cast<int>(m_projection.binRanges.size());
        entry.rows.resize(static_cast<size_t>(height));
        quint8 *rows = entry.rows.data();
        const std::pair<int, int> *ranges = m_projection.binRanges.data();
        for (int y = 0; y < height; ++y) {
            const auto [binLo, binHi] = ranges[y];
            quint8 rawMax = 0;
            for (int b = binLo; b <= binHi; ++b) {
                rawMax = std::max(rawMax, bins[b]);
            }
            rows[y] = rawMax;
            if (destination != nullptr) {
                destination[static_cast<qsizetype>(y) * rowStride] = colors[rawMax];
            }
        }
        return entry;
    }

    const Source &m_source;
    const SpectrogramProjection &m_projection;
    bool m_cache;
    // A pixel blends two neighbouring columns; the next pixel shares one.
    std::array<Entry, 2> m_entries{};
    size_t m_nextEntry{0};
};

template <typename Source>
void rasterColumn(
    QImage &canvas,
    int x,
    qint64 displayIndex,
    SpectrogramRowMaxima<Source> &maxima) {
    const CanvasColumn out = canvasColumn(canvas, x);
    if (!maxima.draw(displayIndex, out)) {
        fillCanvasColumn(out);
    }
}

// The loudest of up to 64 display columns that share one pixel.
template <typename Source>
void rasterPeakHold(
    QImage &canvas,
    int x,
    qint64 colFirst,
    qint64 colLast,
    SpectrogramRowMaxima<Source> &maxima) {
    if (colFirst == colLast) {
        rasterColumn(canvas, x, colFirst, maxima);
        return;
    }
    const Source &source = maxima.source();
    const SpectrogramProjection &projection = maxima.projection();
    // Cap to a reasonable max to avoid pathological cases.
    const int rangeCols = static_cast<int>(
        std::min(colLast - colFirst + 1, static_cast<qint64>(64)));
    std::array<const quint8 *, 64> columns{};
    int validCount = 0;
    for (int i = 0; i < rangeCols; ++i) {
        if (const quint8 *bins = source.column(colFirst + i)) {
            columns[static_cast<size_t>(validCount++)] = bins;
        }
    }
    const CanvasColumn out = canvasColumn(canvas, x);
    if (validCount == 0) {
        fillCanvasColumn(out);
        return;
    }
    for (int y = 0; y < out.height; ++y) {
        const auto [binLo, binHi] = projection.binRanges[static_cast<size_t>(y)];
        quint8 rawMax = 0;
        for (int c = 0; c < validCount; ++c) {
            const quint8 *bins = columns[static_cast<size_t>(c)];
            for (int b = binLo; b <= binHi; ++b) {
                rawMax = std::max(rawMax, bins[b]);
            }
        }
        out.set(y, projection.colors[static_cast<size_t>(rawMax)]);
    }
}

// A pixel between two display columns, when zoomed in.
template <typename Source>
void rasterInterpolated(
    QImage &canvas,
    int x,
    qint64 displayIndexL,
    qint64 displayIndexR,
    double t,
    SpectrogramRowMaxima<Source> &maxima) {
    const quint8 *left = maxima.rows(displayIndexL);
    const quint8 *right = maxima.rows(displayIndexR);
    if (left == nullptr && right == nullptr) {
        fillCanvasColumn(canvasColumn(canvas, x));
        return;
    }
    if (left == nullptr || right == nullptr) {
        rasterColumn(canvas, x, left == nullptr ? displayIndexR : displayIndexL, maxima);
        return;
    }
    const CanvasColumn out = canvasColumn(canvas, x);
    const QRgb *colors = maxima.projection().colors.data();
    QRgb *destination = out.destination;
    const qsizetype rowStride = out.rowStride;
    if (t == 0.5) {
        // Halfway, as every other pixel is at a scale of 2: the rounded
        // mean, the same as below.
        for (int y = 0; y < out.height; ++y) {
            const int rawBlended = (left[y] + right[y] + 1) >> 1;
            destination[static_cast<qsizetype>(y) * rowStride] = colors[rawBlended];
        }
        return;
    }
    const double oneMinusT = 1.0 - t;
    for (int y = 0; y < out.height; ++y) {
        const int rawBlended = roundNonNegative(
            static_cast<double>(left[y]) * oneMinusT
            + static_cast<double>(right[y]) * t);
        destination[static_cast<qsizetype>(y) * rowStride] = colors[rawBlended];
    }
}

// The canvas pixel that starts `pixelStart` pixels after display column
// displayLeft, drawn into canvas column canvasX. A canvas that has scrolled
// by part of a pixel has pixels that start between whole pixels.
template <typename Source>
void rasterPixel(
    QImage &canvas,
    int canvasX,
    double pixelStart,
    qint64 displayLeft,
    qint64 displayRight,
    double canvasZoom,
    SpectrogramRowMaxima<Source> &maxima) {
    if (canvasZoom <= 0.0 || displayRight < displayLeft) {
        return;
    }
    const double columnsPerPixel = 1.0 / canvasZoom;
    const double rangeStart = pixelStart * columnsPerPixel;
    const double rangeEnd = (pixelStart + 1.0) * columnsPerPixel;
    const qint64 colFirst = std::min(
        displayLeft + static_cast<qint64>(std::floor(rangeStart)),
        displayRight);
    const qint64 colLast = std::min(
        displayLeft + static_cast<qint64>(std::ceil(rangeEnd - 1.0)),
        displayRight);

    const bool interpolate = canvasZoom > 1.001;
    if (colFirst < colLast && !interpolate) {
        rasterPeakHold(canvas, canvasX, colFirst, colLast, maxima);
    } else if (interpolate) {
        const double t = rangeStart - std::floor(rangeStart);
        if (t > 0.001) {
            const qint64 colR = std::min(colFirst + 1, displayRight);
            if (colR != colFirst) {
                rasterInterpolated(canvas, canvasX, colFirst, colR, t, maxima);
                return;
            }
        }
        rasterColumn(canvas, canvasX, colFirst, maxima);
    } else {
        rasterColumn(canvas, canvasX, colFirst, maxima);
    }
}

// The first and last display columns rasterPixel draws a pixel from, as
// rasterPixel works them out.
std::pair<qint64, qint64> rasterPixelColumns(
    double pixelStart, qint64 displayLeft, qint64 displayRight, double canvasZoom) {
    const double columnsPerPixel = 1.0 / canvasZoom;
    const double rangeStart = pixelStart * columnsPerPixel;
    const double rangeEnd = (pixelStart + 1.0) * columnsPerPixel;
    const qint64 colFirst = std::min(
        displayLeft + static_cast<qint64>(std::floor(rangeStart)), displayRight);
    if (canvasZoom > 1.001) {
        const double t = rangeStart - std::floor(rangeStart);
        return {colFirst, t > 0.001 ? std::min(colFirst + 1, displayRight) : colFirst};
    }
    return {colFirst, std::max(colFirst, std::min(
        displayLeft + static_cast<qint64>(std::ceil(rangeEnd - 1.0)), displayRight))};
}

// How many pixels a full rasterization of display columns
// [displayLeft, displayRight] fills, `scale` of them to a logical pixel.
int rasterDrawPixels(
    int width, qint64 displayLeft, qint64 displayRight, double canvasZoom, double scale) {
    if (width <= 0 || displayRight < displayLeft || canvasZoom <= 0.0) {
        return 0;
    }
    const qint64 sourceColumns = displayRight - displayLeft + 1;
    int drawPixels = std::min(width,
        static_cast<int>(std::ceil(
            static_cast<double>(sourceColumns) * canvasZoom)));
    // At max zoom-out, hop rounding can produce a few fewer columns
    // than the canvas width.  Pad to width when the display covers
    // nearly all the content — the colFirst clamping in the loop
    // repeats the last column for the extra pixels.
    if (drawPixels < width
        && displayLeft == 0
        && static_cast<double>(sourceColumns + 5) * scale >= static_cast<double>(width)) {
        drawPixels = width;
    }
    return drawPixels;
}

// The last display column a rasterization of `drawPixels` pixels shows.
qint64 rasterDisplayRight(qint64 displayLeft, int drawPixels, double canvasZoom) {
    const double columnsPerPixel = 1.0 / canvasZoom;
    return drawPixels > 0
        ? (displayLeft + static_cast<qint64>(
               static_cast<double>(drawPixels) * columnsPerPixel) - 1)
        : (displayLeft - 1);
}

// Device pixels for `logical` pixels at `scale`, whole ones only: a canvas
// one texel to a device pixel needs no resampling, which would shimmer as it
// scrolls at fractional scales.
int devicePixels(double logical, double scale) {
    return std::max(1, static_cast<int>(std::floor(logical * scale + 1e-6)));
}

// A line one logical pixel thick, in whole device pixels.
int deviceLineWidth(double scale) {
    return std::max(1, static_cast<int>(std::lround(scale)));
}

constexpr int kGridLabelPadding = 2;
constexpr int kCrosshairLabelPadding = 3;

QFont overlayLabelFont(SpectrogramOverlayImage::Kind kind) {
    QFont font;
    font.setPixelSize(kind == SpectrogramOverlayImage::CrosshairLabel ? 10 : 9);
    return font;
}

// The name of an overlay image's texture.
QString overlayImageKey(const SpectrogramOverlayImage &image, double scale) {
    return QStringLiteral("%1|%2|%3|%4")
        .arg(static_cast<int>(image.kind))
        .arg(scale, 0, 'g', 17)
        .arg(image.length)
        .arg(image.text);
}

// The device pixels of an overlay image at `scale`.
QSize overlayImagePixels(const SpectrogramOverlayImage &image, double scale) {
    const int lineWidth = deviceLineWidth(scale);
    const int length = std::max(1, image.length);
    switch (image.kind) {
    case SpectrogramOverlayImage::DottedColumn:
        return {lineWidth, length};
    case SpectrogramOverlayImage::DottedRow:
        return {length, lineWidth};
    default:
        return {
            std::max(1, static_cast<int>(std::ceil(image.rect.width() * scale - 1e-6))),
            std::max(1, static_cast<int>(std::ceil(image.rect.height() * scale - 1e-6)))};
    }
}

// An overlay image at `scale` device pixels to a logical one.
QImage renderOverlayImage(const SpectrogramOverlayImage &image, double scale) {
    const bool dottedColumn = image.kind == SpectrogramOverlayImage::DottedColumn;
    if (dottedColumn || image.kind == SpectrogramOverlayImage::DottedRow) {
        // In device pixels, as the crosshair has always been drawn.
        const int lineWidth = deviceLineWidth(scale);
        const int length = std::max(1, image.length);
        QImage line(overlayImagePixels(image, scale), QImage::Format_ARGB32_Premultiplied);
        line.fill(Qt::transparent);
        QPainter painter(&line);
        painter.setRenderHint(QPainter::Antialiasing, false);
        QPen dotPen(QColor(255, 255, 255, 140));
        dotPen.setStyle(Qt::DotLine);
        dotPen.setWidth(lineWidth);
        painter.setPen(dotPen);
        // A one-pixel pen draws the pixels at its coordinates, a wider one
        // the pixels around them.
        const double offset = lineWidth > 1 ? lineWidth * 0.5 : 0.0;
        if (dottedColumn) {
            painter.drawLine(QPointF(offset, 0.0), QPointF(offset, length - 1.0));
        } else {
            painter.drawLine(QPointF(0.0, offset), QPointF(length - 1.0, offset));
        }
        return line;
    }
    // A label: its rounded background and text, at device resolution.
    const bool crosshair = image.kind == SpectrogramOverlayImage::CrosshairLabel;
    const int pad = crosshair ? kCrosshairLabelPadding : kGridLabelPadding;
    const QSizeF size = image.rect.size();
    QImage label(overlayImagePixels(image, scale), QImage::Format_ARGB32_Premultiplied);
    label.setDevicePixelRatio(scale);
    label.fill(Qt::transparent);
    QPainter painter(&label);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QFont font = overlayLabelFont(image.kind);
    painter.setFont(font);
    painter.setPen(Qt::NoPen);
    painter.setBrush(crosshair ? QColor(0, 0, 10, 160) : QColor(0, 0, 10, 180));
    const int radius = crosshair ? 3 : 2;
    painter.drawRoundedRect(QRectF(QPointF(0.0, 0.0), size), radius, radius);
    painter.setPen(crosshair ? QColor(255, 255, 255, 220) : QColor(220, 220, 235, 240));
    painter.drawText(QPointF(pad, pad + QFontMetrics(font).ascent()), image.text);
    painter.end();
    return label;
}

} // namespace

// Shared across all SpectrogramItem instances so the ring-cap floor
// survives instance tear-downs (channel-count changes destroy and
// recreate the widgets).  Matches the singleton tracker on the Rust
// side in AnalysisRuntimeState.
SpectrogramItem::CenteredDisplayRange SpectrogramItem::centeredDisplayRange(
    qint64 nowCol,
    int visibleWindowCols,
    qint64 maxColCount,
    qint64 estimateCount,
    double columnsPerSecond) {
    CenteredDisplayRange range;
    const qint64 windowCols = static_cast<qint64>(visibleWindowCols);
    const qint64 halfWindowCols = windowCols / 2;
    // Use the estimate for the range so it stays stable while the data
    // fills (no race-in, no sliding).
    const qint64 estTotalCols = std::max(maxColCount, estimateCount);
    // When the visible window covers most of the track, lock the display
    // to the full extent.  The 90% threshold accounts for decimation
    // rounding that can leave a few extra columns beyond the widget width
    // at max zoom-out.  Without the lock, centering on the playhead causes
    // the spectrogram to shift on seek and jitter during playback.
    if (estTotalCols <= 0 || windowCols * 100 / estTotalCols >= kCenteredWholeTrackPercent) {
        range.left = 0;
        range.right = estTotalCols - 1;
    } else {
        range.left = std::max<qint64>(0, nowCol - halfWindowCols);
        range.right = std::min(estTotalCols - 1, range.left + windowCols - 1);
        range.left = std::max<qint64>(0, range.right - windowCols + 1);
    }

    // The file-metadata estimate can overshoot the actual audible/decoded
    // extent by seconds (e.g. trailing silence counted by container
    // duration but trimmed during playback).  A fixed 128-column tolerance
    // fails at high zoom where 128 cols is ~0.2 s and the gap is typically
    // ~1 s. Use maxCol as the hard right edge only when the decoded tail is
    // plausibly at EOF. During rapid far seeks the decoded tail can lag far
    // behind the optimistic playhead; clamping in that state makes the
    // viewport race forward as decode catches up and can map right-edge
    // clicks backward. The EOF clamp still drives playhead detachment and
    // keeps the time axis bounded by audible content.
    range.decodedTailLooksFinal =
        centeredDecodedTailLooksFinal(maxColCount, estimateCount, columnsPerSecond);
    if (range.decodedTailLooksFinal && maxColCount - 1 < nowCol + halfWindowCols) {
        range.right = std::min(range.right, maxColCount - 1);
        range.left = std::max<qint64>(0, range.right - windowCols + 1);
    }
    return range;
}

SpectrogramItem::CenteredView SpectrogramItem::centeredViewLocked(
    double positionSeconds,
    int canvasWidth,
    int canvasHeight,
    double canvasZoom,
    double columnsPerSecond) const {
    const double columnF = std::max(0.0, positionSeconds) * columnsPerSecond;
    const qint64 nowCol = static_cast<qint64>(std::floor(columnF));
    const double columnPhase = std::clamp(columnF - std::floor(columnF), 0.0, 0.999);
    const int visibleWindowCols = static_cast<int>(
        std::ceil(static_cast<double>(canvasWidth) / canvasZoom));
    // Use maxColumnIndex (actual decoded extent) only as a right-edge clamp
    // at EOF: this makes the playhead detach from center at track end
    // without affecting the display during fill.
    const qint64 maxColCount = m_precomputedMaxColumnIndex >= 0
        ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
        : static_cast<qint64>(0);
    const qint64 estimateCount = std::max(
        static_cast<qint64>(m_precomputedTotalColumnsEstimate),
        static_cast<qint64>(1));
    const CenteredDisplayRange range = centeredDisplayRange(
        nowCol, visibleWindowCols, maxColCount, estimateCount, columnsPerSecond);
    CenteredView view;
    view.left = range.left;
    view.right = range.right;
    view.decodedTailLooksFinal = range.decodedTailLooksFinal;

    // Jitter prevention: only apply when zoom hasn't changed, against the
    // canvas being rasterized if there is one.
    qint64 referenceLeft = m_precomputedCanvasDisplayLeft;
    qint64 referenceRight = m_precomputedCanvasDisplayRight;
    double referenceZoom = m_precomputedCanvasZoomLevel;
    bool referenceComparable = !m_precomputedCanvasRolling
        && m_canvas.width() == canvasWidth
        && m_canvas.height() == canvasHeight;
    if (m_rasterPending) {
        referenceLeft = m_rasterTarget.displayLeft;
        referenceZoom = m_rasterTarget.effectiveZoom;
        referenceRight = rasterDisplayRight(
            referenceLeft,
            rasterDrawPixels(m_rasterTarget.width, referenceLeft,
                m_rasterTarget.displayRight, referenceZoom, m_rasterTarget.scale),
            referenceZoom);
        referenceComparable = !m_rasterTarget.rollingMode
            && m_rasterTarget.width == canvasWidth
            && m_rasterTarget.height == canvasHeight;
    }
    const bool isSeekJump =
        referenceRight >= referenceLeft
        && view.left < referenceLeft - 2;
    if (!isSeekJump
        && referenceComparable
        && referenceRight >= referenceLeft
        && std::abs(canvasZoom - referenceZoom) < 0.001) {
        view.left = std::max(view.left, referenceLeft);
        view.right = std::max(view.right, referenceRight);
    }

    view.scrolls = centeredRangeScrolls(
        view.left, view.right, maxColCount, estimateCount, view.decodedTailLooksFinal);
    view.drawX = view.scrolls ? -columnPhase * canvasZoom : 0.0;
    return view;
}

int SpectrogramItem::s_maxWidgetWidthSeen = 0;

void SpectrogramRingStorage::clear() {
    m_blocks.clear();
    m_bins = 0;
}

void SpectrogramRingStorage::allocate(int capacity, int bins) {
    m_blocks.clear();
    m_bins = std::max(0, bins);
    if (capacity <= 0 || m_bins <= 0) {
        return;
    }
    const int blockCount = (capacity + kColumnsPerBlock - 1) / kColumnsPerBlock;
    m_blocks.reserve(static_cast<size_t>(blockCount));
    for (int block = 0; block < blockCount; ++block) {
        const int columns = std::min(kColumnsPerBlock, capacity - block * kColumnsPerBlock);
        m_blocks.emplace_back(static_cast<qsizetype>(columns) * m_bins, '\0');
    }
}

qint64 SpectrogramRingStorage::byteSize() const {
    qint64 bytes = 0;
    for (const QByteArray &block : m_blocks) {
        bytes += block.size();
    }
    return bytes;
}

const quint8 *SpectrogramRingStorage::column(int slot) const {
    const size_t block = static_cast<size_t>(slot / kColumnsPerBlock);
    if (slot < 0 || block >= m_blocks.size()) {
        return nullptr;
    }
    const qsizetype offset = static_cast<qsizetype>(slot % kColumnsPerBlock) * m_bins;
    if (offset + m_bins > m_blocks[block].size()) {
        return nullptr;
    }
    return reinterpret_cast<const quint8 *>(m_blocks[block].constData()) + offset;
}

quint8 *SpectrogramRingStorage::writableColumn(int slot) {
    const size_t block = static_cast<size_t>(slot / kColumnsPerBlock);
    if (slot < 0 || block >= m_blocks.size()) {
        return nullptr;
    }
    const qsizetype offset = static_cast<qsizetype>(slot % kColumnsPerBlock) * m_bins;
    if (offset + m_bins > m_blocks[block].size()) {
        return nullptr;
    }
    // data() copies the block first while a rasterization still holds it.
    return reinterpret_cast<quint8 *>(m_blocks[block].data()) + offset;
}

SpectrogramItem::SpectrogramItem(QQuickItem *parent)
    : QQuickItem(parent)
    , m_rasterMailbox(std::make_shared<SpectrogramRasterMailbox>()) {
    m_rasterMailbox->owner = this;
    // Created on the UI thread, which outlives the render threads using it.
    spectrogramRasterPool();
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
    connect(this, &QQuickItem::visibleChanged, this, [this]() {
        if (!isVisible()) return;
        {
            QMutexLocker lock(&m_stateMutex);
            m_animationTickInitialized = false;
            m_precomputedCanvasDirty = true;
        }
        update();
    });
    connect(this, &QQuickItem::windowChanged, this, &SpectrogramItem::bindWindowFpsTracking);
    bindWindowFpsTracking(window());
    m_playbackFrameTimer = new QTimer(this);
    m_playbackFrameTimer->setSingleShot(true);
    m_playbackFrameTimer->setTimerType(Qt::PreciseTimer);
    connect(m_playbackFrameTimer, &QTimer::timeout, this, &SpectrogramItem::requestPlaybackFrame);
    m_zoomDebounceTimer = new QTimer(this);
    m_zoomDebounceTimer->setSingleShot(true);
    m_zoomDebounceTimer->setInterval(150);
    connect(m_zoomDebounceTimer, &QTimer::timeout, this, [this]() {
        float zoom = 1.0f;
        {
            QMutexLocker lock(&m_stateMutex);
            zoom = m_pendingBackendZoom;
            // The backend ignores a request too close to its zoom; the data
            // to wait for is then still its current zoom's.
            m_lastBackendZoom =
                ferrous_ffi_spectrogram_zoom_after_request(m_lastBackendZoom, zoom);
        }
        emit backendZoomRequested(zoom);
    });
}

SpectrogramItem::~SpectrogramItem() {
    // ~QQuickItem takes the item out of its window, which emits
    // windowChanged and visibleChanged once this item's members are gone:
    // its handlers for them must not run then.
    disconnect(this, nullptr, this, nullptr);
    disconnect(m_animationTickConnection);
    disconnect(m_windowVisibilityConnection);
    // A rasterization still running stops at its next check, and none
    // delivers to this item from now on.
    QMutexLocker lock(&m_rasterMailbox->mutex);
    m_rasterMailbox->owner = nullptr;
    m_rasterMailbox->latestSerial.store(0, std::memory_order_relaxed);
}

double SpectrogramItem::dbRange() const {
    return m_dbRange;
}

void SpectrogramItem::setDbRange(double value) {
    {
        QMutexLocker lock(&m_stateMutex);
        const double clamped = std::clamp(value, 50.0, 150.0);
        if (std::abs(m_dbRange - clamped) < 0.001) {
            return;
        }
        m_dbRange = clamped;
        markCanvasStaleLocked();
    }
    emit dbRangeChanged();
    update();
}

bool SpectrogramItem::logScale() const {
    return m_logScale;
}

void SpectrogramItem::setLogScale(bool value) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_logScale == value) {
            return;
        }
        m_logScale = value;
        invalidateMapping();
        markCanvasStaleLocked();
        m_freqGridDirty = true;
        m_timeGridDirty = true;
        m_crosshairDirty = true;
    }
    emit logScaleChanged();
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
        // The estimate starts over; the scene node and its textures stay.
        resetFpsEstimateLocked();
    }
    emit showFpsOverlayChanged();
    update();
}

int SpectrogramItem::sampleRateHz() const {
    return m_sampleRateHz;
}

void SpectrogramItem::setSampleRateHz(int value) {
    {
        QMutexLocker lock(&m_stateMutex);
        const int clamped = std::max(1000, value);
        if (m_sampleRateHz == clamped) {
            return;
        }
        m_sampleRateHz = clamped;
        invalidateMapping();
        markCanvasStaleLocked();
        m_freqGridDirty = true;
        m_timeGridDirty = true;
        m_crosshairDirty = true;
    }
    emit sampleRateHzChanged();
    update();
}

int SpectrogramItem::maxColumns() const {
    return m_maxColumns;
}

void SpectrogramItem::setMaxColumns(int value) {
    {
        QMutexLocker lock(&m_stateMutex);
        const int clamped = std::clamp(value, 128, 8192);
        if (m_maxColumns == clamped) {
            return;
        }
        m_maxColumns = clamped;
        while (static_cast<int>(m_columns.size()) > m_maxColumns) {
            m_columns.pop_front();
        }
        markCanvasStaleLocked();
    }
    emit maxColumnsChanged();
    update();
}

double SpectrogramItem::positionSeconds() const {
    QMutexLocker lock(&m_stateMutex);
    return m_positionSeconds;
}

PlaybackClock *SpectrogramItem::clock() const {
    return m_clock;
}

void SpectrogramItem::setClock(PlaybackClock *value) {
    if (m_clock == value) {
        return;
    }
    disconnect(m_clockConnection);
    m_clock = value;
    if (m_clock) {
        m_clockConnection = connect(
            m_clock, &PlaybackClock::stateChanged, this, &SpectrogramItem::clockStateChanged);
    }
    emit clockChanged();
    clockStateChanged();
}

void SpectrogramItem::clockStateChanged() {
    using Clock = std::chrono::steady_clock;
    const PlaybackClockState next = m_clock ? m_clock->state() : PlaybackClockState{};
    bool courseCorrection = false;
    {
        QMutexLocker lock(&m_stateMutex);
        const auto now = Clock::now();
        const qint64 nowNs = monotonicNs(now);
        const bool held = positionHeldLocked(nowNs);
        const PlaybackClockState previous = m_clockState;
        const double previousPosition = currentRenderPositionSecondsLocked(now);
        if (next.trackToken != previous.trackToken) {
            // Where the previous track goes on, in case its successor
            // follows it gaplessly.
            m_previousTrackClockState = held ? m_positionHoldTrajectory : previous;
            m_previousTrackPositionOffset = held ? m_positionHoldOffset : m_gaplessPositionOffset;
        }
        m_clockState = next;
        if (m_displayMode != 0) {
            // Centered views are random access: a seek or a new track moves
            // them at once, even before its data arrives.
            m_positionJumpHoldActive = false;
            m_positionJumpHoldUntilNs = 0;
        } else if (m_gaplessContinuationPending
            && next.trackToken != 0
            && next.trackToken == m_precomputedTrackToken) {
            m_gaplessContinuationPending = false;
            continueGaplessLocked(now);
        } else {
            const double position = next.positionAt(nowNs) + m_gaplessPositionOffset;
            // The rolling history and the clock describe different tracks
            // until the worker's data for the clock's track arrives.
            const bool tracksDisagree = next.trackToken != 0
                && m_precomputedTrackToken != 0
                && next.trackToken != m_precomputedTrackToken;
            const bool sameTrackJump = next.trackToken == previous.trackToken
                && std::abs(position - previousPosition) >= kPositionJumpHoldThresholdSeconds;
            if (m_precomputedReady && m_playing && (tracksDisagree || sameTrackJump)) {
                if (!held || m_positionJumpHoldUntilNs != 0) {
                    holdPositionLocked(
                        held ? m_positionHoldTrajectory : previous,
                        held ? m_positionHoldOffset : m_gaplessPositionOffset,
                        now);
                }
            } else if (!tracksDisagree && m_positionJumpHoldUntilNs == 0) {
                m_positionJumpHoldActive = false;
            }
        }
        m_positionSeconds = currentRenderPositionSecondsLocked(now);
        // A sample that only corrects the course of playback moves nothing
        // at once: the playback frames follow it. Anything else, such as a
        // seek, a track change or a pause, shows at once.
        courseCorrection = m_playing
            && m_precomputedReady
            && next.advancing()
            && previous.advancing()
            && next.trackToken == previous.trackToken
            && next.epoch == previous.epoch
            && next.durationSeconds == previous.durationSeconds
            && held == positionHeldLocked(nowNs);
    }
    emit positionSecondsChanged();
    if (courseCorrection) {
        schedulePlaybackFrame();
    } else {
        requestPlaybackFrame();
    }
}

bool SpectrogramItem::positionHeldLocked(qint64 nowNs) const {
    return m_positionJumpHoldActive
        && (m_positionJumpHoldUntilNs == 0 || nowNs < m_positionJumpHoldUntilNs);
}

void SpectrogramItem::holdPositionLocked(
    const PlaybackClockState &trajectory,
    double offset,
    std::chrono::steady_clock::time_point now) {
    m_positionJumpHoldActive = true;
    m_positionHoldTrajectory = trajectory;
    m_positionHoldOffset = offset;
    m_positionJumpHoldUntilNs = 0;
    m_positionJumpHoldStartedAt = now;
}

void SpectrogramItem::releasePositionHoldLocked(std::chrono::steady_clock::time_point now) {
    m_positionJumpHoldActive = false;
    m_positionJumpHoldUntilNs = 0;
    m_positionSeconds = currentRenderPositionSecondsLocked(now);
}

void SpectrogramItem::continueGaplessLocked(std::chrono::steady_clock::time_point now) {
    // The rolling history goes on with the successor's columns after all of
    // the previous track's, so the successor starts at the end of that
    // track. The engine stamps the successor's start with the time the
    // previous track ends, which can still be ahead; until then the
    // previous track plays on.
    const qint64 nowNs = monotonicNs(now);
    qint64 startNs = nowNs;
    double startPosition = m_clockState.positionAt(nowNs);
    if (m_clockState.advancing()) {
        startNs = m_clockState.anchorNs
            - static_cast<qint64>(std::llround(
                m_clockState.anchorSeconds / m_clockState.rate * 1'000'000'000.0));
        startPosition = 0.0;
    }
    const double previousDuration = m_previousTrackClockState.durationSeconds;
    const double previousEnd = m_previousTrackPositionOffset
        + (previousDuration > 0.0
            ? previousDuration
            : m_previousTrackClockState.positionAt(startNs));
    m_gaplessPositionOffset = previousEnd - startPosition;
    if (startNs > nowNs) {
        holdPositionLocked(m_previousTrackClockState, m_previousTrackPositionOffset, now);
        m_positionJumpHoldUntilNs = startNs;
    } else {
        m_positionJumpHoldActive = false;
        m_positionJumpHoldUntilNs = 0;
    }
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
        m_playing = value;
        if (!m_playing && m_positionJumpHoldActive) {
            // Playback stopped while the playhead waited for data: it stays
            // where it is, and the rolling history stays parked there until
            // the worker restarts.
            const auto now = Clock::now();
            const double parked = currentRenderPositionSecondsLocked(now);
            PlaybackClockState frozen;
            frozen.anchorSeconds = parked;
            frozen.anchorNs = monotonicNs(now);
            holdPositionLocked(frozen, 0.0, now);
            m_positionSeconds = parked;
        }
        changed = true;
    }
    if (changed) {
        if (!value) {
            m_playbackFrameTimer->stop();
        }
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

bool SpectrogramItem::reanchorRollingEpochForCurrentDataLocked(
    std::chrono::steady_clock::time_point now) {
    if (m_ringWriteSeq <= 0
        || m_ringCapacity <= 0
        || m_precomputedSampleRateHz <= 0
        || m_precomputedHopSize <= 0) {
        return false;
    }

    const double columnsPerSecond =
        static_cast<double>(m_precomputedSampleRateHz)
        / static_cast<double>(m_precomputedHopSize);
    const qint64 nowCol = static_cast<qint64>(std::floor(
        std::max(0.0, currentRenderPositionSecondsLocked(now)) * columnsPerSecond));

    qint64 bestSeq = -1;
    qint64 bestTrackCol = -1;
    qint64 bestDistance = std::numeric_limits<qint64>::max();

    for (qint64 seq = m_ringOldestSeq; seq < m_ringWriteSeq; ++seq) {
        const int slot = static_cast<int>(seq % m_ringCapacity);
        if (slot < 0 || slot >= m_ringCapacity) {
            continue;
        }
        if (m_ringSequenceId.empty()
            || m_ringColumnId.empty()
            || m_ringTrackToken.empty()
            || m_ringSequenceId[static_cast<size_t>(slot)] != seq
            || m_ringTrackToken[static_cast<size_t>(slot)] != m_precomputedTrackToken) {
            continue;
        }

        const qint64 trackCol = m_ringColumnId[static_cast<size_t>(slot)];
        if (trackCol < 0) {
            continue;
        }

        const qint64 distance = std::llabs(trackCol - nowCol);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestSeq = seq;
            bestTrackCol = trackCol;
            if (distance == 0) {
                break;
            }
        }
    }

    if (bestSeq < 0 || bestTrackCol < 0) {
        return false;
    }

    m_rollingEpoch = bestSeq - bestTrackCol;
    return true;
}

void SpectrogramItem::setDisplayMode(int value) {
    const int clamped = std::clamp(value, 0, 1);
    if (m_displayMode == clamped) {
        return;
    }
    bool redraw = false;
    {
        QMutexLocker lock(&m_stateMutex);
        const int previousMode = m_displayMode;
        m_displayMode = clamped;
        if (previousMode == 1 && clamped == 0) {
            reanchorRollingEpochForCurrentDataLocked(std::chrono::steady_clock::now());
        }
        if (m_precomputedReady) {
            m_precomputedLastRightCol = -1;
            markCanvasStaleLocked();
            redraw = true;
        }
    }
    emit displayModeChanged();
    if (redraw) {
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
    {
        QMutexLocker lock(&m_stateMutex);
        m_channelMuted = muted;
        markCanvasStaleLocked();
    }
    emit channelMutedChanged();
    update();
}

double SpectrogramItem::zoomLevel() const {
    return m_zoomLevel;
}

double SpectrogramItem::effectiveZoomLocked() const {
    if (m_precomputedHopSize <= 0) {
        return m_renderZoomLevel;
    }
    return m_renderZoomLevel * static_cast<double>(m_precomputedHopSize)
           / kReferenceHopSamples;
}

double SpectrogramItem::canvasZoomLocked() const {
    return effectiveZoomLocked() * m_canvasScale;
}

int SpectrogramItem::canvasWidthLocked() const {
    return devicePixels(width(), m_canvasScale);
}

int SpectrogramItem::canvasHeightLocked() const {
    return devicePixels(height(), m_canvasScale);
}

double SpectrogramItem::windowDevicePixelRatio() const {
    const QQuickWindow *currentWindow = window();
    return currentWindow != nullptr ? currentWindow->effectiveDevicePixelRatio() : m_canvasScale;
}

void SpectrogramItem::applyCanvasScaleLocked(double scale) {
    if (!std::isfinite(scale) || scale <= 0.0 || std::abs(scale - m_canvasScale) <= 1e-9) {
        return;
    }
    // The canvas on screen stays there, scaled to the new pixels, until its
    // replacement at this scale arrives.
    m_canvasScale = scale;
    invalidateMapping();
    markCanvasStaleLocked();
    if (m_rasterPending) {
        // A rasterization at the old scale is dropped when it arrives, and
        // the next frame asks for one at this scale.
        ++m_rasterSerial;
        m_rasterMailbox->latestSerial.store(m_rasterSerial, std::memory_order_relaxed);
        m_rasterTarget.width = 0;
    }
    m_freqGridDirty = true;
    m_timeGridDirty = true;
    m_crosshairDirty = true;
    m_overlayDirty = true;
}

double SpectrogramItem::minimumZoomLevelLocked() const {
    const int w = static_cast<int>(width());
    if (w <= 0) {
        return 1.0;
    }
    // Use the full track estimate so the zoom-out limit is stable from
    // the first chunk (set from file metadata) and doesn't shift as
    // decoding progresses.  Fall back to maxColumnIndex for streams
    // where the estimate may lag behind actual content.
    const qint64 totalCols = std::max(
        static_cast<qint64>(m_precomputedTotalColumnsEstimate),
        m_precomputedMaxColumnIndex >= 0
            ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
            : static_cast<qint64>(0));
    if (totalCols <= 0) {
        return 1.0;
    }
    // Normalize to reference-hop columns.  With zoom-adapted decimation
    // the backend reports fewer columns at sub-1.0 zoom (larger hop).
    // Without normalization, the minimum zoom tightens as you zoom out,
    // preventing full-song view.  The product totalCols × hop is the
    // track's sample count and stays constant across zoom levels.
    const qint64 referenceCols = m_precomputedHopSize > 0
        ? totalCols * static_cast<qint64>(m_precomputedHopSize)
              / static_cast<qint64>(kReferenceHopSamples)
        : totalCols;
    if (referenceCols <= static_cast<qint64>(w)) {
        return 1.0;
    }
    // Mirror the Rust-side clamp in AnalysisCommand::SetSpectrogramZoomLevel
    // (`.clamp(0.05, 16.0)`).  If Qt lets the user request a zoom below
    // Rust's floor, setSpectrogramZoomLevel(0.04) gets clamped to 0.05
    // on the backend, the session restarts at a slightly different
    // effective hop than Qt expected, and the next widthSettle-driven
    // re-send gets clamped the same way — the user sees their zoom-out
    // attempts "restart the same zoom level over and over".
    constexpr double kBackendMinZoom = 0.05;
    const double trackFit =
        static_cast<double>(w) / static_cast<double>(referenceCols);
    return std::max(trackFit, kBackendMinZoom);
}

double SpectrogramItem::minimumZoomLevel() const {
    QMutexLocker lock(&m_stateMutex);
    return minimumZoomLevelLocked();
}

double SpectrogramItem::backendZoomForRequestLocked() const {
    // The backend keeps its zoom for a request too close to it; see
    // accepted_spectrogram_zoom in src/analysis.
    return static_cast<double>(ferrous_ffi_spectrogram_zoom_after_request(
        m_lastBackendZoom, static_cast<float>(m_zoomLevel)));
}

int SpectrogramItem::columnHopForZoomLocked(double zoom) const {
    // bins = fft / 2 + 1.
    const int fftSize = m_precomputedBinsPerColumn > 1
        ? (m_precomputedBinsPerColumn - 1) * 2
        : 8192;
    return static_cast<int>(ferrous_ffi_spectrogram_column_hop(
        static_cast<std::uint32_t>(fftSize), static_cast<float>(zoom)));
}

double SpectrogramItem::renderZoomForZoomLocked(double zoom) const {
    // Data at a hop other than the reference one is drawn one column per
    // pixel; see the snap in feedPrecomputedChunk.
    const int hop = columnHopForZoomLocked(zoom);
    if (hop > 0 && hop != static_cast<int>(kReferenceHopSamples)) {
        return kReferenceHopSamples / static_cast<double>(hop);
    }
    return zoom;
}

SpectrogramZoomPreview SpectrogramItem::zoomPreviewLocked(
    int width,
    double positionSeconds,
    double drawX,
    bool rollingMode,
    double columnsPerSecond) const {
    // In canvas pixels: width, drawX (where the canvas is drawn) and the
    // preview. The canvas may be at another scale than this frame's.
    SpectrogramZoomPreview preview;
    const double targetZoom = renderZoomForZoomLocked(backendZoomForRequestLocked());
    const double targetPixelsPerSecond = targetZoom * m_canvasScale
        * static_cast<double>(m_precomputedSampleRateHz) / kReferenceHopSamples;
    const double scale = targetPixelsPerSecond
        / (m_precomputedCanvasPixelsPerSecond * m_precomputedCanvasScale);
    if (!std::isfinite(scale) || scale <= 0.0 || std::abs(scale - 1.0) <= 1e-3) {
        return preview;
    }
    // Where the playhead is on the canvas as drawn.
    const double canvasColumn = static_cast<double>(m_precomputedCanvasEpoch)
        + positionSeconds * m_precomputedCanvasColumnsPerSecond;
    preview.fromX = drawX + m_precomputedCanvasSubpixelOffset
        + (canvasColumn - static_cast<double>(m_precomputedCanvasDisplayLeft))
            * m_precomputedCanvasZoomLevel;
    // Where the playhead will be once the data for the zoom arrives: the
    // view follows the playhead, at the right edge in rolling mode, in the
    // middle of a centered view unless near the track's ends.  Scaling
    // about it keeps the preview where that data will be.
    preview.toX = static_cast<double>(width);
    if (!rollingMode) {
        const int targetHop = columnHopForZoomLocked(backendZoomForRequestLocked());
        if (targetHop <= 0 || columnsPerSecond <= 0.0) {
            return preview;
        }
        const double targetColumnsPerSecond =
            static_cast<double>(m_precomputedSampleRateHz) / static_cast<double>(targetHop);
        const double targetEffectiveZoom = m_canvasScale
            * targetZoom * static_cast<double>(targetHop) / kReferenceHopSamples;
        const double toTargetColumns = targetColumnsPerSecond / columnsPerSecond;
        const qint64 maxColCount = m_precomputedMaxColumnIndex >= 0
            ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
            : static_cast<qint64>(0);
        const qint64 targetMaxColCount = static_cast<qint64>(
            std::llround(static_cast<double>(maxColCount) * toTargetColumns));
        const qint64 targetEstimateCount = std::max<qint64>(1, static_cast<qint64>(
            std::llround(static_cast<double>(m_precomputedTotalColumnsEstimate)
                * toTargetColumns)));
        const double targetColumnF = positionSeconds * targetColumnsPerSecond;
        const qint64 targetNowCol = static_cast<qint64>(std::floor(targetColumnF));
        const int targetWindowCols = static_cast<int>(
            std::ceil(static_cast<double>(width) / targetEffectiveZoom));
        const CenteredDisplayRange range = centeredDisplayRange(
            targetNowCol, targetWindowCols, targetMaxColCount, targetEstimateCount,
            targetColumnsPerSecond);
        const bool scrolls = centeredRangeScrolls(
            range.left, range.right, targetMaxColCount, targetEstimateCount,
            range.decodedTailLooksFinal);
        const double playheadColumn = scrolls
            ? static_cast<double>(targetNowCol)
            : targetColumnF;
        preview.toX = (playheadColumn - static_cast<double>(range.left)) * targetEffectiveZoom;
    }
    preview.scale = scale;
    preview.active = true;
    return preview;
}

bool SpectrogramItem::zoomPreviewWantedLocked() const {
    // Also while a zoom fill keeps an older canvas on screen.
    return (m_awaitingZoomData || m_zoomFillActive)
        && m_precomputedCanvasPixelsPerSecond > 0.0
        && m_precomputedSampleRateHz > 0;
}

void SpectrogramItem::setZoomLevel(double value) {
    QMutexLocker lock(&m_stateMutex);
    const double minZoom = minimumZoomLevelLocked();
    value = std::clamp(value, minZoom, 16.0);
    if (std::abs(m_zoomLevel - value) < 0.0001) {
        return;
    }
    m_zoomLevel = value;

    // All zoom changes go through the backend: zoom-in needs a finer hop,
    // zoom-out needs coarser decimation.  Until data for this zoom
    // arrives, the canvas on screen is shown scaled to it.
    m_pendingBackendZoom = static_cast<float>(m_zoomLevel);
    m_zoomDebounceTimer->start(); // restarts the 150ms timer
    const int hop = columnHopForZoomLocked(backendZoomForRequestLocked());
    const bool dataIsForZoom = hop > 0
        && hop == m_precomputedHopSize
        && hop == columnHopForZoomLocked(m_lastBackendZoom);
    if (dataIsForZoom) {
        // Back at the zoom of the data on screen, with no other zoom asked
        // of the backend: nothing to wait for.
        m_awaitingZoomData = false;
        m_renderZoomLevel = renderZoomForZoomLocked(backendZoomForRequestLocked());
        m_crosshairDirty = true;
        updateCanvasFillReadinessLocked();
    } else {
        m_awaitingZoomData = true;
    }

    lock.unlock();
    emit zoomLevelChanged();
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
}

void SpectrogramItem::updateCanvasFillReadinessLocked() {
    if (m_zoomFillActive && m_awaitingZoomData) {
        // The data is not for the requested zoom yet.
        return;
    }
    if (m_zoomFillActive) {
        // Only a centered zoom-in, or a centered seek, waits for the ring
        // to cover the display.  Elsewhere the new data fills in as it
        // arrives, which beats a preview held until almost the whole
        // zoomed-out track has decoded.
        const double currentPixelsPerSecond = m_precomputedSampleRateHz > 0
            ? m_renderZoomLevel * static_cast<double>(m_precomputedSampleRateHz)
                / kReferenceHopSamples
            : 0.0;
        const bool zoomChanged = m_precomputedCanvasPixelsPerSecond > 0.0
            && std::abs(currentPixelsPerSecond / m_precomputedCanvasPixelsPerSecond - 1.0)
                > 1e-3;
        const bool waitForCoverage = m_displayMode == 1
            && (!zoomChanged || currentPixelsPerSecond > m_precomputedCanvasPixelsPerSecond);
        if (!waitForCoverage) {
            m_zoomFillActive = false;
            m_precomputedCanvasDirty = true;
            return;
        }
    }
    // Canvas freeze readiness: clear m_zoomFillActive when the ring
    // has enough data to cover the visible display.
    if (m_zoomFillActive) {
        const int screenWidth = std::max(static_cast<int>(width()), 1);
        // Two conditions must be met:
        // 1. The ring has at least screenWidth columns (enough data
        //    to fill the canvas).
        // 2. The newest column in the ring covers the display's
        //    right edge (playheadCol + screenWidth/2), so the
        //    display won't have a black region on the right.
        // Both are capped at totalEstimate for max zoom-out.
        const qint64 ringFill = m_ringWriteSeq - m_ringOldestSeq;
        const int fillWidth = m_precomputedTotalColumnsEstimate > 0
            ? std::min(screenWidth, m_precomputedTotalColumnsEstimate)
            : screenWidth;
        const bool ringHasEnoughColumns =
            ringFill >= static_cast<qint64>(fillWidth) - 16;

        const double cps = m_precomputedHopSize > 0
            ? static_cast<double>(m_precomputedSampleRateHz)
                / static_cast<double>(m_precomputedHopSize)
            : 1.0;
        const double playheadSeconds = std::max(
            0.0, currentRenderPositionSecondsLocked(std::chrono::steady_clock::now()));
        const qint64 displayRightCol = static_cast<qint64>(
            playheadSeconds * cps) + screenWidth / 2;
        const qint64 displayRightCapped =
            m_precomputedTotalColumnsEstimate > 0
                ? std::min(displayRightCol,
                    static_cast<qint64>(m_precomputedTotalColumnsEstimate))
                : displayRightCol;
        const bool ringCoversDisplayRight =
            m_precomputedMaxColumnIndex + 1 >= displayRightCapped - 16;

        // A large absolute max column does not prove that the ring contains
        // the LEFT edge of this viewport.  In particular, a stale backend
        // target can refill thousands of columns near EOF while the stopped
        // Qt viewport is at column 0.  Releasing the freeze in that state
        // rebuilds every requested pixel from missing slots and turns the
        // widget fully black.  Require the active token to retain the
        // viewport's first source column before handing off to the new zoom.
        const int visibleWindowCols = static_cast<int>(std::ceil(
            static_cast<double>(screenWidth)
            / std::max(0.001, effectiveZoomLocked())));
        const qint64 estimateCount = std::max<qint64>(
            static_cast<qint64>(m_precomputedTotalColumnsEstimate), 1);
        const qint64 nowCol = static_cast<qint64>(playheadSeconds * cps);
        qint64 displayLeftCol;
        if (static_cast<qint64>(visibleWindowCols) * 100 / estimateCount >= 90) {
            displayLeftCol = 0;
        } else {
            const qint64 halfWindowCols = visibleWindowCols / 2;
            displayLeftCol = std::max<qint64>(0, nowCol - halfWindowCols);
            const qint64 displayRightColForEstimate = std::min(
                estimateCount - 1,
                displayLeftCol + static_cast<qint64>(visibleWindowCols) - 1);
            displayLeftCol = std::max<qint64>(
                0,
                displayRightColForEstimate
                    - static_cast<qint64>(visibleWindowCols) + 1);
        }
        const bool ringCoversDisplayLeft =
            ringSlotForDisplayIndexLocked(displayLeftCol, false) >= 0;

        // Decode tail-end: the STFT window truncates the last few
        // columns so maxCol+1 stops short of the estimate by ~20–30
        // columns.  At max zoom-out in wide fullscreen views,
        // fillWidth ≈ totalEstimate and ringHasEnoughColumns would
        // require ringFill to exceed that short-by-tail value —
        // which it never does.  Without this bypass, the freeze
        // persists forever and the old canvas leaves a slice of
        // stale content at the right edge.  Once maxCol is within
        // a reasonable slack of the estimate, no more data is
        // coming; clear the freeze so the rebuild can run.
        const bool decodeReachedEnd =
            m_precomputedTotalColumnsEstimate > 0
            && m_precomputedMaxColumnIndex >= 0
            && static_cast<qint64>(m_precomputedMaxColumnIndex) + 1 + 64
                >= static_cast<qint64>(m_precomputedTotalColumnsEstimate);

        if (ringCoversDisplayLeft
            && ((ringHasEnoughColumns && ringCoversDisplayRight)
                || decodeReachedEnd)) {
            m_zoomFillActive = false;
            // Don't invalidateCanvas() here — the canvas must stay
            // non-null so the NEXT zoom transition can activate its
            // freeze.  Just mark dirty; the rebuild replaces all
            // canvas content.
            m_precomputedCanvasDirty = true;
        }
    }

}

void SpectrogramItem::feedPrecomputedChunk(
    const QByteArray &data, int bins, int channelIndex,
    int columns, int startIndex, int totalEstimate,
    int sampleRate, int hopSize, bool complete,
    bool bufferReset, quint64 trackToken, bool clearHistoryOnReset,
    quint64 generation) {
    using Clock = std::chrono::steady_clock;
    QMutexLocker lock(&m_stateMutex);

    FERROUS_SPECTROGRAM_LOGF(stderr,
        "[Qt-feed@%p] chIdx=%d cols=%d start=%d total=%d bins=%d sr=%d hop=%d tok=%llu gen=%llu ready=%d reset=%d clear=%d await=%d complete=%d\n",
        static_cast<const void *>(this),
        channelIndex, columns, startIndex, totalEstimate, bins,
        sampleRate, hopSize, static_cast<unsigned long long>(trackToken),
        static_cast<unsigned long long>(generation),
        m_precomputedReady ? 1 : 0, bufferReset ? 1 : 0,
        clearHistoryOnReset ? 1 : 0, m_awaitingWorkerReset ? 1 : 0,
        complete ? 1 : 0);

    const bool hasGeneration = generation != 0;
    if (hasGeneration
        && m_precomputedCommittedGeneration != 0
        && generation < m_precomputedCommittedGeneration) {
        return;
    }
    if (hasGeneration
        && m_precomputedSessionGeneration != 0
        && generation < m_precomputedSessionGeneration) {
        return;
    }

    // Finalize chunk: carries the true decoded-column count for the
    // matching track so the centered-mode EOF clamp fires at the real
    // audio end instead of the file-metadata estimate (which may
    // overshoot by seconds, especially at high zoom).  Apply only when
    // the token matches the current track — a stale finalize for a
    // superseded track would otherwise corrupt the new-track estimate.
    // Skip all other reset / ring / canvas logic; only the estimate
    // needs to change.
    if (complete && columns == 0 && !bufferReset && !clearHistoryOnReset) {
        if (totalEstimate > 0
            && (m_precomputedCommittedToken == 0
                || trackToken == m_precomputedCommittedToken)
            && (!hasGeneration
                || m_precomputedSessionGeneration == 0
                || generation == m_precomputedSessionGeneration)
            && m_precomputedTotalColumnsEstimate > 0
            && static_cast<int>(totalEstimate) < m_precomputedTotalColumnsEstimate) {
            m_precomputedTotalColumnsEstimate = totalEstimate;
            m_precomputedCanvasDirty = true;
            // EOF can make the viewport attainable when the duration estimate
            // extends past decoded audio. No further data may arrive to end
            // the preserved-canvas handoff, so re-evaluate it here too.
            updateCanvasFillReadinessLocked();
            update();
        }
        return;
    }

    // Allow buffer_reset chunks with no data (bins=0) through so the ring
    // can be cleared immediately. These are emitted by the analysis thread
    // on far seeks to prevent stale-data flashes.
    if ((totalEstimate <= 0 || bins <= 0) && !bufferReset) {
        return;
    }

    // Record the committed token on buffer_reset so we can drop stale
    // chunks from a superseded session that arrive out of order.
    if (bufferReset && trackToken != 0) {
        m_precomputedCommittedToken = trackToken;
    }
    if (bufferReset && hasGeneration) {
        m_precomputedCommittedGeneration = generation;
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

    // When m_awaitingWorkerReset is true, reject all data until the
    // worker's proper reset arrives (bufferReset with valid bins).
    // This blocks stale queued chunks from refilling the ring after
    // a synthetic clear.  Must be checked BEFORE the implicit reset
    // below, which would otherwise recreate the ring from stale data.
    if (m_awaitingWorkerReset) {
        if (bufferReset && bins > 0) {
            FERROUS_SPECTROGRAM_LOGF(stderr,
                "[Qt-gate-open@%p] worker reset arrived, bins=%d tok=%llu\n",
                static_cast<const void *>(this), bins,
                static_cast<unsigned long long>(trackToken));
            m_awaitingWorkerReset = false;
            // Fall through to normal reset handling below.
        } else {
            FERROUS_SPECTROGRAM_LOGF(stderr,
                "[Qt-gate-reject@%p] stale chunk rejected, cols=%d bins=%d reset=%d\n",
                static_cast<const void *>(this), columns, bins,
                bufferReset ? 1 : 0);
            return;
        }
    }

    // Early hop change detection: activate canvas freeze BEFORE any
    // reset processing that might call invalidateCanvas().  The freeze
    // is only needed for finer-hop zoom-in restarts; on coarser-hop
    // zoom-outs, preserving the old centered image makes the view look
    // stuck at 1.0 until almost the full zoomed-out track has decoded.
    if (hopSize > 0
        && hopSize != m_precomputedHopSize
        && (m_precomputedHopSize <= 0 || hopSize < m_precomputedHopSize)
        && m_displayMode == 1
        && !m_canvas.isNull()) {
        m_zoomFillActive = true;
    }
    // A restart that is not for the requested zoom yet, such as a session
    // for an earlier zoom: keep the previewed canvas on screen rather than
    // flash that data.  The freeze ends with data for the requested zoom.
    if (m_awaitingZoomData
        && columns > 0
        && hopSize > 0
        && (bufferReset || m_precomputedResetPending)
        && hopSize != columnHopForZoomLocked(backendZoomForRequestLocked())
        && !m_canvas.isNull()
        && m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft) {
        m_zoomFillActive = true;
    }

    // If this widget has no ring and receives data (not a reset), it was
    // created/recycled after the reset went to a different widget instance.
    // Apply an implicit reset so the widget can accept the arriving data.
    bool appliedImplicitReset = false;
    if (!bufferReset && columns > 0 && m_ringCapacity == 0 && !m_precomputedResetPending) {
        applyPrecomputedResetLocked(startIndex, bins, trackToken, generation, true);
        appliedImplicitReset = true;
    }

    bool appliedReset = false;

    if (bufferReset && columns <= 0 && clearHistoryOnReset && bins <= 0) {
        // Immediate ring clear from a synthetic reset chunk (emitted by
        // the analysis thread on far seeks).  Clear the ring and block
        // all subsequent data until the worker's proper reset arrives.
        //
        // Preserve the old canvas across the seek in centered mode when
        // there's a valid image to show.  Without this, the display
        // goes fully black for ~100 ms while the decoder catches up
        // from (pos - margin) to the new playhead — very visible at
        // max zoom-in where the visible window is only a couple of
        // seconds wide.  The existing canvasFreeze machinery handles
        // the brief interval; readiness clears once the new session
        // has produced enough columns to cover the new display range.
        const bool preserveCanvasForSeek =
            !m_canvas.isNull()
            && m_displayMode == 1
            && m_precomputedCanvasDisplayRight
                >= m_precomputedCanvasDisplayLeft
            && m_precomputedTotalColumnsEstimate > 0
            && m_precomputedBinsPerColumn > 0;

        m_ringWriteSeq = 0;
        m_ringOldestSeq = 0;
        m_ringCapacity = 0;
        m_ring.clear();
        m_ringColumnId.clear();
        m_ringSequenceId.clear();
        m_ringTrackToken.clear();
        m_trackColumnToSeqByToken.clear();
        ++m_ringGeneration;
        cancelPendingRasterLocked();
        m_precomputedMaxColumnIndex = -1;
        m_precomputedCanvasDirty = true;
        m_awaitingWorkerReset = true;

        if (preserveCanvasForSeek) {
            // Canvas, m_precomputedReady, and the stored display range
            // all stay as-is so the paint keeps rendering the old
            // image through canvasFreeze.
            m_zoomFillActive = true;
        } else {
            m_precomputedReady = false;
            m_precomputedCanvasDisplayLeft = 0;
            m_precomputedCanvasDisplayRight = -1;
            invalidateCanvas();
        }
        FERROUS_SPECTROGRAM_LOGF(stderr,
            "[Qt-synthetic-clear@%p] ring wiped, gate armed, preserved=%d\n",
            static_cast<const void *>(this),
            preserveCanvasForSeek ? 1 : 0);
        update();
        return;
    } else if (bufferReset
        && columns <= 0
        && clearHistoryOnReset
        && bins > 0
        && startIndex == 0
        && m_displayMode == 1
        && trackToken != 0
        && m_precomputedTrackToken != 0
        && trackToken != m_precomputedTrackToken) {
        // Non-gapless track changes arrive first as metadata-only reset
        // chunks.  Deferring these like same-track seeks keeps the previous
        // track's per-pane render caches alive until the first data chunk,
        // so one stereo pane can visibly lag behind on track changes.
        const bool wasReady = m_precomputedReady;
        const bool qtRenderNotAtDefault =
            std::abs(m_renderZoomLevel - 1.0) > 0.001;
        const bool backendNotAtReferenceHop =
            hopSize > 0 && hopSize != static_cast<int>(kReferenceHopSamples);
        const bool needsZoomReset =
            qtRenderNotAtDefault || backendNotAtReferenceHop;

        m_zoomFillActive = false;
        applyPrecomputedResetLocked(
            startIndex, bins, trackToken, generation, clearHistoryOnReset);
        invalidateCanvas();
        m_precomputedResetPending = false;
        m_precomputedPendingResetStartIndex = 0;
        m_precomputedPendingResetBins = 0;
        m_precomputedPendingResetTrackToken = 0;
        m_precomputedPendingResetGeneration = 0;
        m_precomputedPendingResetClearHistory = false;
        m_precomputedReady = false;
        m_precomputedBinsPerColumn = bins;
        if (totalEstimate > 0) {
            m_precomputedTotalColumnsEstimate = totalEstimate;
        }
        if (sampleRate > 0) {
            m_precomputedSampleRateHz = sampleRate;
        }
        if (hopSize > 0) {
            m_precomputedHopSize = hopSize;
        }
        // The playhead follows the clock, which shows the new track.
        releasePositionHoldLocked(Clock::now());
        m_precomputedTrackToken = trackToken;
        m_precomputedLastRightCol = -1;
        m_precomputedLastDisplaySeq = -1;
        m_timeGridDirty = true;

        if (needsZoomReset) {
            m_zoomLevel = 1.0;
            m_renderZoomLevel = 1.0;
            m_awaitingZoomData = false;
            m_pendingBackendZoom = 1.0f;
            m_lastBackendZoom =
                ferrous_ffi_spectrogram_zoom_after_request(m_lastBackendZoom, 1.0f);
            if (m_zoomDebounceTimer) {
                m_zoomDebounceTimer->stop();
            }
            if (qtRenderNotAtDefault) {
                m_precomputedTotalColumnsEstimate = 0;
            }
            m_precomputedCanvasDirty = true;
            m_crosshairDirty = true;
            m_timeGridDirty = true;
        }

        lock.unlock();
        if (needsZoomReset) {
            emit zoomLevelChanged();
            emit zoomResetRequested();
            emit backendZoomRequested(1.0f);
        }
        if (wasReady) {
            emit precomputedReadyChanged();
        }
        update();
        return;
    } else if (bufferReset && columns <= 0) {
        // Delay the reset handoff until the first data-bearing post-seek
        // chunk arrives. Resetting on the metadata-only frame makes the
        // item repaint against an empty/new timeline before there is any
        // matching data, which shows up as a blank flash on backward seeks.
        m_precomputedResetPending = true;
        m_precomputedPendingResetStartIndex = startIndex;
        m_precomputedPendingResetBins = bins;
        m_precomputedPendingResetTrackToken = trackToken;
        m_precomputedPendingResetGeneration = generation;
        m_precomputedPendingResetClearHistory = clearHistoryOnReset;
    } else if (bufferReset) {
        applyPrecomputedResetLocked(
            startIndex, bins, trackToken, generation, clearHistoryOnReset);
        m_precomputedResetPending = false;
        m_precomputedPendingResetStartIndex = 0;
        m_precomputedPendingResetBins = 0;
        m_precomputedPendingResetTrackToken = 0;
        m_precomputedPendingResetGeneration = 0;
        m_precomputedPendingResetClearHistory = false;
        appliedReset = true;
    } else if (m_precomputedResetPending
        && columns > 0
        && bins == m_precomputedPendingResetBins
        && (m_precomputedPendingResetTrackToken == 0
            || trackToken == 0
            || trackToken == m_precomputedPendingResetTrackToken)
        && (m_precomputedPendingResetGeneration == 0
            || generation == 0
            || generation == m_precomputedPendingResetGeneration)) {
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
            m_precomputedPendingResetGeneration != 0
                ? m_precomputedPendingResetGeneration
                : generation,
            m_precomputedPendingResetClearHistory);
        m_precomputedResetPending = false;
        m_precomputedPendingResetStartIndex = 0;
        m_precomputedPendingResetBins = 0;
        m_precomputedPendingResetTrackToken = 0;
        m_precomputedPendingResetGeneration = 0;
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

        // Gapless rolling transition: the successor's columns follow the
        // previous track's in the ring, so its position is shown with an
        // offset, in the ring's continuous coordinate space. The clock and
        // this data may report the new track in either order.
        if (m_displayMode == 0) {
            if (m_clockState.trackToken == trackToken) {
                continueGaplessLocked(Clock::now());
            } else {
                m_gaplessContinuationPending = true;
            }
        } else {
            // Centered mode: the new track uses 0-based coordinates, and
            // the playhead already follows the clock.
            releasePositionHoldLocked(Clock::now());
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
                m_positionSeconds,
                m_gaplessPositionOffset,
                static_cast<long long>(validCount), m_ringCapacity,
                m_precomputedTotalColumnsEstimate);
        }
#endif
    }

    // Detect track change before updating the stored token.
    // Gapless transitions set isGaplessTrackChange; non-gapless set isTrackChange.
    const bool isGaplessTrackChange =
        trackToken != 0
        && m_precomputedTrackToken != 0
        && trackToken != m_precomputedTrackToken
        && !bufferReset && !appliedReset && !appliedImplicitReset;
    // Centered same-track seeks outside the current window restart the
    // worker with the SAME track token but a non-zero start column.
    // Fresh SpectrogramItem instances can miss the prior synthetic clear,
    // so m_precomputedTrackToken may still be 0 when that first data-bearing
    // reset chunk lands. Treat only 0-based resets as real track changes;
    // otherwise a same-track seek at max zoom-out gets misclassified as a
    // fresh-track backend/hop desync and forces zoom back to 1.0.
    const bool isTrackChange = appliedReset
        && startIndex == 0
        && trackToken != 0
        && (m_precomputedTrackToken == 0 || trackToken != m_precomputedTrackToken);

    if (trackToken != 0 && !(bufferReset && columns <= 0)) {
        m_precomputedTrackToken = trackToken;
    }

    m_precomputedBinsPerColumn = bins;
    // Accept the latest estimate for the active session.  Same-hop
    // increases are legitimate for raw-surround files: the worker can
    // start with a fallback/header estimate, then raise it once a
    // duration re-query succeeds or once decoding proves the fallback
    // too small.  Rejecting those updates leaves Qt stuck on the stale
    // shorter length, which tightens minimumZoomLevel and clamps
    // centered-mode displayRight/seek math before the actual EOF.
    //
    // Stale-session chunks are filtered above by token/reset gating, and
    // finalize chunks still take the dedicated shrink-only path.
    if (totalEstimate > 0) {
        m_precomputedTotalColumnsEstimate = totalEstimate;
    }

    // Only update rate/hop from chunks that carry actual column data.
    if (columns > 0) {
        if (sampleRate > 0) {
            m_precomputedSampleRateHz = sampleRate;
        }
        if (hopSize > 0) {
            m_precomputedHopSize = hopSize;
        }
        // Apply deferred visual zoom when backend recalculation data arrives.
        // The render zoom is deferred in setZoomLevel() to avoid showing an
        // interpolated intermediate.  Instead of applying immediately, we
        // enter a "zoom fill" state: the old frame stays on screen while
        // the backend streams enough data to cover the visible window.
        // When enough columns have arrived, the render zoom is applied and
        // the canvas is rebuilt with complete data — no left-to-right fill.
        if (m_awaitingZoomData && (appliedReset || appliedImplicitReset)) {
            // Only data at the requested zoom's hop ends the wait.  Other
            // data is from before the backend restarted for it: a session
            // for an earlier zoom, or a restart for another reason (e.g. a
            // width change) while the request waited for the debounce.
            const bool dataIsForZoom =
                m_precomputedHopSize == columnHopForZoomLocked(backendZoomForRequestLocked());
            if (dataIsForZoom) {
                m_awaitingZoomData = false;
            }
            // Snap renderZoomLevel so effectiveZoom = renderZoom × hop / ref
            // equals exactly 1.0.  Without this snap, hop rounding at
            // non-power-of-2 zoom levels (e.g. zoom=3.815 → hop=268 →
            // effectiveZoom=0.998) causes the advance path to fail,
            // forcing 42ms full rebuilds every frame.  Data for another
            // zoom is drawn at its own zoom, so the preview can scale it.
            if (m_precomputedHopSize > 0
                && m_precomputedHopSize
                    != static_cast<int>(kReferenceHopSamples)) {
                m_renderZoomLevel = kReferenceHopSamples
                    / static_cast<double>(m_precomputedHopSize);
            } else {
                m_renderZoomLevel = dataIsForZoom ? m_zoomLevel : 1.0;
            }
            m_crosshairDirty = true;
        }
        if (appliedReset) {
            // The worker's data for the new position has arrived, so the
            // playhead follows the clock again.
            const auto now = Clock::now();
            releasePositionHoldLocked(now);
            if (m_displayMode == 0
                && m_playing
                && m_precomputedSampleRateHz > 0
                && m_precomputedHopSize > 0) {
                // Unless the data is for a track or position the clock does
                // not show yet: then the playhead starts from the data until
                // the clock catches up.
                const double dataStartSeconds =
                    static_cast<double>(startIndex * m_precomputedHopSize)
                    / static_cast<double>(m_precomputedSampleRateHz);
                const bool otherTrack = m_clockState.trackToken != 0
                    && trackToken != 0
                    && m_clockState.trackToken != trackToken;
                const bool otherPosition =
                    std::abs(m_clockState.positionAt(monotonicNs(now)) - dataStartSeconds)
                    >= kPositionJumpHoldThresholdSeconds;
                if (otherTrack || otherPosition) {
                    PlaybackClockState dataStart;
                    dataStart.anchorSeconds = dataStartSeconds;
                    dataStart.anchorNs = monotonicNs(now);
                    dataStart.rate = m_clockState.advancing() ? 1.0 : 0.0;
                    holdPositionLocked(dataStart, 0.0, now);
                    m_positionSeconds = dataStartSeconds;
                }
            }
        }
    }

    // Dynamically size the ring buffer based on widget width and display mode.
    if (columns > 0 && bins > 0) {
        // Track the max widget width so the ring can hold enough cols
        // to cover the Rust decoder's lookahead even after a session
        // reset that shrinks the current widget.  Static so the floor
        // survives instance tear-downs (channel-count changes destroy
        // and recreate the widgets) — matches the singleton tracker on
        // the Rust side in AnalysisRuntimeState.
        const int currentWidth = static_cast<int>(width());
        s_maxWidgetWidthSeen = std::max(s_maxWidgetWidthSeen, currentWidth);
        const int screenWidth = std::max(
            {currentWidth, 1920, s_maxWidgetWidthSeen});
        const int neededCapacity = static_cast<int>(ferrous_ffi_spectrogram_ring_capacity(
            static_cast<std::uint32_t>(screenWidth),
            static_cast<std::uint32_t>(m_precomputedSampleRateHz),
            static_cast<std::uint32_t>(m_precomputedHopSize),
            static_cast<std::uint32_t>(bins),
            static_cast<std::uint32_t>(channelCount),
            static_cast<double>(static_cast<float>(m_zoomLevel)), m_displayMode == 1));

        // Check if we need to (re)allocate: either more columns, or
        // more bins_per_column (e.g. FFT size switch).
        const bool mustRealloc =
            bins > m_ring.bins()
            || neededCapacity > m_ringCapacity;

        if (mustRealloc) {
            SpectrogramRingStorage newRing;
            newRing.allocate(neededCapacity, bins);
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
                && m_ring.bins() == bins;
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
                    const quint8 *source = m_ring.column(oldSlot);
                    quint8 *destination = newRing.writableColumn(newSlot);
                    if (source == nullptr || destination == nullptr) {
                        continue;
                    }
                    memcpy(destination, source, static_cast<size_t>(bins));
                    newColId[static_cast<size_t>(newSlot)] = trackCol;
                    newSeqId[static_cast<size_t>(newSlot)] = seq;
                    newTrackToken[static_cast<size_t>(newSlot)] = token;
                    newTrackColumnToSeqByToken[token].insert(trackCol, seq);
                }
            }
            m_ring = std::move(newRing);
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
        const quint64 effectiveTrackToken =
            trackToken != 0 ? trackToken : m_precomputedTrackToken;
        auto activeTokenIt = m_trackColumnToSeqByToken.find(effectiveTrackToken);
        if (activeTokenIt == m_trackColumnToSeqByToken.end()) {
            activeTokenIt = m_trackColumnToSeqByToken.insert(
                effectiveTrackToken,
                QHash<qint32, qint64>{});
        }
        activeTokenIt->reserve(activeTokenIt->size() + columns);

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
                            if (tokenIt != activeTokenIt && tokenIt->isEmpty()) {
                                m_trackColumnToSeqByToken.erase(tokenIt);
                                // QHash::erase() can relocate buckets while
                                // repairing the probe chain, invalidating
                                // iterators to entries that were not erased.
                                activeTokenIt =
                                    m_trackColumnToSeqByToken.find(effectiveTrackToken);
                            }
                        }
                    }
                }
            }
            if (quint8 *destination = m_ring.writableColumn(slot)) {
                memcpy(destination, srcData + srcOff, static_cast<size_t>(bins));
            }
            m_ringColumnId[static_cast<size_t>(slot)] = trackCol;
            if (!m_ringSequenceId.empty()) {
                m_ringSequenceId[static_cast<size_t>(slot)] = m_ringWriteSeq;
            }
            if (!m_ringTrackToken.empty()) {
                m_ringTrackToken[static_cast<size_t>(slot)] = effectiveTrackToken;
            }
            activeTokenIt->insert(trackCol, m_ringWriteSeq);
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

    // A canvas drawn at another hop or zoom, such as the frozen preview of
    // an earlier zoom, has no place for this data's columns: writing them
    // in would mix time scales.  It is rebuilt once the freeze ends.
    const double chunkColumnsPerSecond = m_precomputedHopSize > 0
        ? static_cast<double>(m_precomputedSampleRateHz)
            / static_cast<double>(m_precomputedHopSize)
        : 0.0;
    const bool canvasDrawnFromThisData =
        std::abs(m_precomputedCanvasZoomLevel - canvasZoomLocked()) < 0.001
        && (m_precomputedCanvasColumnsPerSecond <= 0.0
            || std::abs(m_precomputedCanvasColumnsPerSecond - chunkColumnsPerSecond)
                <= 1e-9 * chunkColumnsPerSecond);
    if (!isVisible() || (window() != nullptr && !window()->isVisible())) {
        m_precomputedCanvasDirty = true;
    } else if (m_displayMode == 1
        && m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft
        && !m_canvas.isNull()
        && m_precomputedBinsPerColumn > 0
        && !canvasDrawnFromThisData) {
        if (!m_zoomFillActive) {
            m_precomputedCanvasDirty = true;
        }
    } else if (m_displayMode == 1
        && m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft
        && !m_canvas.isNull()
        && m_precomputedBinsPerColumn > 0) {
        // Centered mode with existing canvas: draw new columns in-place
        // using the same column-to-pixel mapping as the rasterization.
        // This avoids full rasterizations while the growing edge fills
        // in during initial decode. A rasterization in progress draws
        // these columns in once it arrives.
        if (columns > 0) {
            drawCenteredColumnsInPlaceLocked(
                startIndex, static_cast<qint64>(startIndex) + columns - 1,
                canvasZoomLocked());
        }
    } else if (m_displayMode == 0
        && m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft) {
        // Rolling mode with existing canvas: the advance path handles
        // new data incrementally.  Avoid forcing a full rebuild on every
        // chunk, which is the primary cause of FPS drops during playback.
    } else {
        // No canvas range yet (first data after reset) — force build.
        m_precomputedCanvasDirty = true;
    }

    updateCanvasFillReadinessLocked();

    // Reset zoom to 1.0 on any track change (gapless or user-initiated).
    // Check m_renderZoomLevel (not m_zoomLevel) because in multi-channel
    // layouts the first pane's signal may have already set m_zoomLevel=1.0
    // on this pane via QML, but m_renderZoomLevel is still at the old
    // value and m_awaitingZoomData still needs clearing.
    //
    // Also reset when the arriving chunk's hop size is not the default
    // reference hop: this catches the case where the SpectrogramItem
    // instance is fresh (channel-count change tore down the previous
    // widgets) so m_renderZoomLevel defaults to 1.0 even though the
    // backend persisted a non-default zoom from the previous track.
    // Without this, Qt renders at effectiveZoom = 1.0 × hop / refHop
    // (= 0.0625 at hop=64), the visible window inflates to thousands of
    // cols, and the right side of the widget stays un-backed while the
    // previous canvas smears through the gap.
    const bool qtRenderNotAtDefault =
        std::abs(m_renderZoomLevel - 1.0) > 0.001;
    const bool backendNotAtReferenceHop =
        hopSize > 0 && hopSize != static_cast<int>(kReferenceHopSamples);
    const bool preserveRollingGaplessZoom =
        m_displayMode == 0 && isGaplessTrackChange;
    const bool resetForTrackTransition =
        isTrackChange || (isGaplessTrackChange && !preserveRollingGaplessZoom);
    const bool needsZoomReset =
        resetForTrackTransition
        && (qtRenderNotAtDefault || backendNotAtReferenceHop);
    if (needsZoomReset) {
        m_zoomLevel = 1.0;
        m_renderZoomLevel = 1.0;
        m_awaitingZoomData = false;
        m_zoomFillActive = false;
        m_lastBackendZoom =
            ferrous_ffi_spectrogram_zoom_after_request(m_lastBackendZoom, 1.0f);
        // Cancel any pending debounced setZoomLevel that would
        // otherwise overwrite this reset 150 ms later.  Scenario: the
        // QML zoomLevel binding pushes the SpectrogramSurface's
        // _widgetZoomLevel into freshly-instantiated SpectrogramItems
        // (channel-count change recreated the Repeater children).  If
        // _widgetZoomLevel was still at the prior track's max value
        // (e.g. 16), setZoomLevel(16) on the new instance arms the
        // debounce timer with m_pendingBackendZoom = 16 — which we
        // must null out here so the timer doesn't fire after our
        // reset and restart the backend at the old zoom.
        m_pendingBackendZoom = 1.0f;
        if (m_zoomDebounceTimer) {
            m_zoomDebounceTimer->stop();
        }
        // Clear the estimate only when Qt's render zoom was actually
        // changing.  The pre-reset estimate came from a different zoom's
        // decimation and would otherwise be stale.  If we're only
        // syncing the backend (Qt already at default zoom on a fresh
        // instance), the estimate that just arrived is the correct one
        // for the new track — don't discard it.
        if (qtRenderNotAtDefault) {
            m_precomputedTotalColumnsEstimate = 0;
        }
        m_precomputedCanvasDirty = true;
        m_crosshairDirty = true;
        m_timeGridDirty = true;
    }

    const bool readyChanged = m_precomputedReady && !wasReady;

    // Unlock before emitting signals — the QML handlers may call
    // setZoomLevel which re-acquires the mutex.
    lock.unlock();

    if (needsZoomReset) {
        emit zoomLevelChanged();
        emit zoomResetRequested();
        // Tell the backend to restart at zoom=1.0.  The QML handler
        // for zoomResetRequested sets the zoom property to 1.0, but
        // setZoomLevel(1.0) is a no-op because the reset above already
        // set m_zoomLevel=1.0.  Without this explicit signal the
        // backend continues at the old zoom level.
        emit backendZoomRequested(1.0f);
    }
    if (readyChanged) {
        emit precomputedReadyChanged();
    }
    update();
}

void SpectrogramItem::clearPrecomputed() {
    FERROUS_SPECTROGRAM_LOGF(stderr, "[Qt-clearPrecomputed] ringCap=%d ready=%d\n",
        m_ringCapacity, m_precomputedReady ? 1 : 0);
    QMutexLocker lock(&m_stateMutex);
    m_ring.clear();
    ++m_ringGeneration;
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
    // Don't zero m_precomputedTotalColumnsEstimate here — it represents
    // the track length and should persist across resets. Zeroing it
    // creates a window where the worker's doubled estimate bypasses
    // the inflation filter in feedPrecomputedChunk.
    m_precomputedResetPending = false;
    m_precomputedPendingResetStartIndex = 0;
    m_precomputedPendingResetBins = 0;
    m_precomputedPendingResetTrackToken = 0;
    m_precomputedPendingResetGeneration = 0;
    m_precomputedPendingResetClearHistory = false;
    m_precomputedSessionGeneration = 0;
    m_precomputedCommittedGeneration = 0;
    m_precomputedLastRightCol = -1;
    m_precomputedLastDisplaySeq = -1;
    m_precomputedTrackToken = 0;
    m_awaitingZoomData = false;
    m_zoomFillActive = false;
    m_renderZoomLevel = m_zoomLevel;
    m_gaplessPositionOffset = 0.0;
    m_gaplessContinuationPending = false;
    m_positionJumpHoldActive = false;
    m_positionJumpHoldUntilNs = 0;
    m_timeGridDirty = true;
    const bool wasReady = m_precomputedReady;
    m_precomputedReady = false;
    invalidateCanvas();
    lock.unlock();
    if (wasReady) {
        emit precomputedReadyChanged();
    }
    update();
}

void SpectrogramItem::applyPrecomputedResetLocked(
    int startIndex,
    int bins,
    quint64 trackToken,
    quint64 generation,
    bool clearHistoryOnReset) {
    Q_UNUSED(trackToken);
    if (generation != 0) {
        m_precomputedSessionGeneration = generation;
        m_precomputedCommittedGeneration = std::max(m_precomputedCommittedGeneration, generation);
    }
    // A rasterization of the columns before the reset is obsolete.
    cancelPendingRasterLocked();
    const bool preserveRollingHistory =
        !clearHistoryOnReset &&
        m_displayMode == 0
        && m_ringCapacity > 0
        && m_precomputedBinsPerColumn > 0
        && bins == m_precomputedBinsPerColumn;
    if (!preserveRollingHistory) {
        ++m_ringGeneration;
    }
    // The canvas stays on screen until the rasterization replacing it
    // arrives when it shows rolling history that keeps its write sequences,
    // or when the zoom preview shows it while waiting for this data.
    const bool hasCanvas = !m_canvas.isNull()
        && m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft;
    const bool keepCanvas = hasCanvas
        && ((preserveRollingHistory && m_precomputedCanvasRolling) || m_awaitingZoomData);
    // A reset (seek or manual track change) breaks the continuous
    // gapless coordinate space — clear the accumulated offset,
    // per-track column tracking, and canvas display range so the
    // centered-mode monotonic clamp doesn't stick at old positions.
    m_gaplessPositionOffset = 0.0;
    m_gaplessContinuationPending = false;
    m_precomputedMaxColumnIndex = -1;
    if (!m_zoomFillActive && !keepCanvas) {
        m_precomputedCanvasDisplayLeft = 0;
        m_precomputedCanvasDisplayRight = -1;
    }
    if (!preserveRollingHistory) {
        m_ringOldestSeq = 0;
        m_ringWriteSeq = 0;
        m_trackEpochSeq = 0;
        // Even when a rolling reset clears history (manual track
        // change or same-track zoom restart), the new ring's sequence
        // space starts at startIndex.  Re-anchor the epoch so the
        // current playback column maps to sequence 0 instead of its
        // absolute track column; otherwise updatePaintNode clamps the
        // display against a huge imaginary headroom gap and scrolling
        // appears frozen until the worker decodes thousands of cols.
        m_rollingEpoch = m_displayMode == 0
            ? -static_cast<qint64>(startIndex)
            : 0;
        m_ring.clear();
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
    // During a zoom fill, preserve the old canvas so updatePaintNode
    // can keep showing it while the ring refills at the new hop.
    if (m_zoomFillActive && !m_canvas.isNull()) {
        // Keep old canvas — canvasFreeze in updatePaintNode will
        // prevent rebuild/advance until the ring covers the display.
    } else if (keepCanvas) {
        m_precomputedCanvasDirty = true;
    } else {
        invalidateCanvas();
    }
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
    const qint64 nowNs = monotonicNs(now);
    if (positionHeldLocked(nowNs)) {
        return std::max(0.0, m_positionHoldTrajectory.positionAt(nowNs) + m_positionHoldOffset);
    }
    return std::max(0.0, m_clockState.positionAt(nowNs) + m_gaplessPositionOffset);
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
    node->statistics = {};
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

    // The canvas and the overlays are in device pixels, `scale` to a
    // logical one: cw x ch for the item.
    double scale = 1.0;
    int cw = w;
    int ch = h;
    // Canvas pixels to a logical pixel: the scale, or one for the canvas
    // drawn from appended rows.
    double canvasScale = 1.0;
    bool hasCanvas = false;
    QSize canvasSize;
    int drawCols = 0;
    int srcStart = 0;
    int latestX = 0;
    double scrollOffset = 0.0;
    // The part of a pixel the canvas has scrolled past its first column,
    // which leaves its end that much short of the view's.
    double canvasEndGap = 0.0;
    // Where the view starts, and where the canvas on screen goes as drawn.
    double drawX = 0.0;
    double canvasDrawX = 0.0;
    bool showPlayhead = false;
    QRectF playheadRect;
    SpectrogramZoomPreview preview;
    QVector<QImage> tileImages;
    QVector<int> tileDirtyIndexes;
    int tileCount = 0;
    bool showOverlay = false;
    bool overlayChanged = false;
    QImage overlayImage;
    QSize overlaySize;

    SpectrogramOverlayFrame freqGridFrame;
    SpectrogramOverlayFrame timeGridFrame;
    SpectrogramOverlayFrame crosshairFrame;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    size_t profilePendingColumns = 0;
    size_t profileColumnCount = 0;
    int profileBinsPerColumn = 0;
#endif

    {
        QMutexLocker lock(&m_stateMutex);
        // The window's scale changes reach the item first; this catches up
        // with a window it has not heard from.
        applyCanvasScaleLocked(windowDevicePixelRatio());
        scale = m_canvasScale;
        cw = canvasWidthLocked();
        ch = canvasHeightLocked();

        const double renderPositionSeconds = currentRenderPositionSecondsLocked(renderNow);
        m_zoomPreviewShown = false;
        m_idleView = {};
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
                    m_positionSeconds,
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
            ensureMapping(ch);
            canvasScale = scale;
            const double columnsPerSecond =
                static_cast<double>(m_precomputedSampleRateHz) / static_cast<double>(m_precomputedHopSize);
            const double canvasZoom = canvasZoomLocked();
            const double clampedPositionSeconds =
                std::max(0.0, renderPositionSeconds);
            double columnF = clampedPositionSeconds * columnsPerSecond;
            const int nowCol = static_cast<int>(std::floor(columnF));
            double columnPhase = std::clamp(columnF - std::floor(columnF), 0.0, 0.999);

            qint64 displayLeft, displayRight;
            int playheadPixel;
            bool rollingMode;
            qint64 writeHeadSeq = -1;
            CenteredView centeredView;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            qint64 unclampedDisplaySeq = 0;
            qint64 writeHeadHeadroom = 0;
            bool writeHeadClamped = false;
#endif

            if (m_displayMode == 1) {
                rollingMode = false;
                centeredView = centeredViewLocked(
                    clampedPositionSeconds, cw, ch, canvasZoom, columnsPerSecond);
                displayLeft = centeredView.left;
                displayRight = centeredView.right;

                playheadPixel = static_cast<int>(std::round(
                    static_cast<double>(nowCol - displayLeft) * canvasZoom));

            } else {
                rollingMode = true;
                const int visibleWindowCols = static_cast<int>(
                    std::ceil(static_cast<double>(cw) / canvasZoom));
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
                cw,
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
                    || m_canvas.width() != cw
                    || m_canvas.height() != ch
                    || rollingMode != m_precomputedCanvasRolling
                    || !hasCanvasRange
                    || displayLeft < m_precomputedCanvasDisplayLeft
                    || displayRight < m_precomputedCanvasDisplayRight
                    || std::abs(canvasZoom - m_precomputedCanvasZoomLevel) > 0.001);

            // During a zoom fill, keep the old canvas visible.
            // m_zoomFillActive is independent of m_awaitingZoomData.
            const bool canvasFreeze =
                m_zoomFillActive && !m_canvas.isNull()
                && m_precomputedCanvasDisplayRight
                    >= m_precomputedCanvasDisplayLeft;

            // Full rasterizations happen on a worker; until one arrives,
            // the canvas on screen stays there, aligned to this view.
            if (visibleCols > 0 && !canvasFreeze) {
                if (m_rasterPending) {
                    if (pendingRasterOutdatedLocked(cw, ch, displayLeft, displayRight, rollingMode)) {
                        requestPrecomputedRasterLocked(cw, ch, displayLeft, displayRight, rollingMode);
                    }
                } else if (needsFullRebuild || m_precomputedCanvasDirty) {
                    requestPrecomputedRasterLocked(cw, ch, displayLeft, displayRight, rollingMode);
                } else if (rangeChanged) {
                    // At sub-1.0 zoom, multiple source columns map to each
                    // pixel.  Small column shifts don't produce visible
                    // changes — skip the expensive full rebuild until the
                    // accumulated shift reaches at least one pixel.
                    const double pixelShift = std::abs(
                        static_cast<double>(displayLeft - m_precomputedCanvasDisplayLeft))
                        * canvasZoom;
                    if (canvasZoom < 0.999 && pixelShift < 1.0) {
                        // Old canvas is still close enough — skip.
                    } else if (!advancePrecomputedCanvasLocked(displayLeft, displayRight, rollingMode)) {
                        requestPrecomputedRasterLocked(cw, ch, displayLeft, displayRight, rollingMode);
                    }
                }
                m_precomputedLastRightCol = nowCol;
                m_precomputedLastDisplaySeq = rollingMode
                    ? displayRight
                    : static_cast<qint64>(nowCol);
            }

            // Where this view starts: the canvas's position when it is the
            // view's, and where a rasterization of the view will put it.
            const bool centeredScrolling = !rollingMode && centeredView.scrolls;
            const bool canvasIsView = !m_rasterPending
                && !m_canvas.isNull()
                && m_canvas.width() == cw
                && m_canvas.height() == ch
                && rollingMode == m_precomputedCanvasRolling
                && std::abs(canvasZoom - m_precomputedCanvasZoomLevel) <= 0.001
                && (m_precomputedCanvasColumnsPerSecond <= 0.0
                    || std::abs(m_precomputedCanvasColumnsPerSecond - columnsPerSecond)
                        <= 1e-9 * columnsPerSecond);
            // drawX is where display column displayLeft starts, in canvas
            // pixels. A canvas scrolled by part of a pixel starts that much
            // further left.
            const double canvasSubpixelOffset =
                canvasIsView ? m_precomputedCanvasSubpixelOffset : 0.0;
            if (rollingMode) {
                const int viewPixels = canvasIsView
                    ? std::min(m_canvasFilledCols, m_canvas.width())
                    : rasterDrawPixels(cw, displayLeft, displayRight, canvasZoom, scale);
                drawX = static_cast<double>(cw - viewPixels)
                    - columnPhase * canvasZoom;
            } else {
                drawX = centeredView.drawX;
            }

            const bool zoomPreviewWanted = zoomPreviewWantedLocked()
                || (m_rasterPending && m_rasterHoldsZoomPreview);
            m_zoomPreviewWantedLastFrame = zoomPreviewWanted;
            bool zoomPreviewShown = false;
            if (!m_canvas.isNull() && m_canvasFilledCols > 0) {
                hasCanvas = true;
                canvasSize = m_canvas.size();
                drawCols = std::min(m_canvasFilledCols, canvasSize.width());
                srcStart = (m_canvasWriteX - drawCols + canvasSize.width()) % canvasSize.width();
                // A view that scrolls has moved part of a column past its
                // last one; a centered one that stays still has not.
                scrollOffset = rollingMode || centeredScrolling ? columnPhase * canvasZoom : 0.0;
                canvasEndGap = canvasIsView || rollingMode ? m_precomputedCanvasSubpixelOffset : 0.0;
                // Where the canvas goes as drawn: at the view's start when
                // it is the view's, and else where it went last.
                canvasDrawX = drawX - canvasSubpixelOffset;
                if (rollingMode && !canvasIsView) {
                    canvasDrawX = static_cast<double>(cw - drawCols)
                        - columnPhase * canvasZoom
                        - m_precomputedCanvasSubpixelOffset;
                }
                if (zoomPreviewWanted) {
                    preview = zoomPreviewLocked(
                        cw, clampedPositionSeconds, canvasDrawX, rollingMode, columnsPerSecond);
                    zoomPreviewShown = preview.active;
                } else if (!canvasIsView) {
                    preview = staleCanvasAlignmentLocked(
                        canvasDrawX, drawCols, static_cast<double>(cw), drawX, displayLeft, rollingMode,
                        canvasZoom, columnsPerSecond);
                }
                if (!preview.active && std::abs(m_precomputedCanvasScale - scale) > 1e-9) {
                    // A canvas not aligned to this view, such as across a
                    // jump or for another track's columns, stays where it
                    // is, but at this frame's scale: about its right end in
                    // rolling mode, where it ends, and else its left end.
                    preview.active = true;
                    preview.scale = scale / m_precomputedCanvasScale;
                    preview.fromX = rollingMode
                        ? canvasDrawX + static_cast<double>(drawCols)
                        : canvasDrawX;
                    preview.toX = preview.fromX;
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
                    // The rasterization's copy, if no column changed since.
                    QImage rasterTile;
                    if (tileIndex < m_rasterTiles.size()) {
                        rasterTile = std::exchange(m_rasterTiles[tileIndex], QImage());
                    }
                    tileImages.push_back(
                        rasterTile.size() == QSize(tileWidth, canvasSize.height())
                            ? std::move(rasterTile)
                            : m_canvas.copy(tileStart, 0, tileWidth, canvasSize.height()));
                    m_dirtyTiles[static_cast<size_t>(tileIndex)] = 0;
                }
            }

            if (zoomPreviewShown && !rollingMode) {
                playheadPixel = static_cast<int>(std::round(preview.toX));
            }
            m_zoomPreviewShown = zoomPreviewShown;
            if (zoomPreviewShown) {
                // In logical pixels, where clicks are.
                m_zoomPreviewAnchorSeconds = clampedPositionSeconds;
                m_zoomPreviewAnchorX = preview.toX / scale;
                m_zoomPreviewPixelsPerSecond = preview.scale
                    * m_precomputedCanvasPixelsPerSecond * m_precomputedCanvasScale / scale;
            }

            if (!rollingMode && playheadPixel >= 0) {
                showPlayhead = true;
                playheadRect = QRectF(
                    static_cast<double>(std::clamp(playheadPixel, 0, cw - 1)) / scale,
                    0.0,
                    static_cast<double>(deviceLineWidth(scale)) / scale,
                    static_cast<double>(ch) / scale);
            }

            // A centered view that stays still, with its canvas in place,
            // changes only when the playhead moves to another pixel or the
            // view starts to scroll. Until playback reaches that column the
            // frames would all be this one.
            if (!rollingMode
                && !centeredScrolling
                && canvasIsView
                && !canvasFreeze
                && !zoomPreviewWanted
                && !preview.active
                && visibleCols > 0
                && !positionHeldLocked(monotonicNs(renderNow))) {
                const qint64 maxColCount = m_precomputedMaxColumnIndex >= 0
                    ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
                    : static_cast<qint64>(0);
                const qint64 estimateCount = std::max<qint64>(m_precomputedTotalColumnsEstimate, 1);
                const int visibleWindowCols = static_cast<int>(
                    std::ceil(static_cast<double>(cw) / canvasZoom));
                const auto drawnPlayhead = [cw](int pixel) {
                    return pixel < 0 ? -1 : std::clamp(pixel, 0, cw - 1);
                };
                const int shownPlayhead = drawnPlayhead(playheadPixel);
                // The playhead moves a pixel within 1 / canvasZoom columns;
                // a view zoomed out further than this many columns to a
                // pixel looks again after that many.
                constexpr qint64 kMaxIdleColumns = 4096;
                qint64 column = nowCol + 1;
                for (; column <= nowCol + kMaxIdleColumns; ++column) {
                    const CenteredDisplayRange next = centeredDisplayRange(
                        column, visibleWindowCols, maxColCount, estimateCount, columnsPerSecond);
                    // The view keeps the canvas's range, as above.
                    if (std::max(next.left, displayLeft) != displayLeft
                        || std::max(next.right, displayRight) != displayRight
                        || drawnPlayhead(static_cast<int>(std::round(
                               static_cast<double>(column - displayLeft) * canvasZoom)))
                            != shownPlayhead) {
                        break;
                    }
                }
                m_idleView.valid = true;
                m_idleView.changeSeconds = static_cast<double>(column) / columnsPerSecond;
            }

            // The overlays draw lines and small images in place, and lay
            // them out again only when what they show changes.
            const QRectF pane(0.0, 0.0, cw / scale, ch / scale);
            const auto placeOverlay = [&](const SpectrogramOverlayLayout &layout,
                                          int shift,
                                          SpectrogramOverlayFrame &frame) {
                // `shift` moves it that many device pixels left.
                for (const QRect &line : layout.lines) {
                    const QRectF rect((line.x() - shift) / scale, line.y() / scale,
                        line.width() / scale, line.height() / scale);
                    if (rect.intersects(pane)) {
                        frame.lines.push_back(rect);
                    }
                }
                for (const SpectrogramOverlayImage &image : layout.images) {
                    // Whole device pixels, one texel to each.
                    const QSize pixels = overlayImagePixels(image, scale);
                    const QRectF rect(
                        std::round((image.rect.x() * scale) - shift) / scale,
                        std::round(image.rect.y() * scale) / scale,
                        pixels.width() / scale,
                        pixels.height() / scale);
                    if (!rect.intersects(pane)) {
                        continue;
                    }
                    SpectrogramPlacedOverlayImage placed{image.key, rect, QImage()};
                    if (!node->overlayTextures.contains(placed.key)) {
                        placed.image = renderOverlayImage(image, scale);
                    }
                    frame.images.push_back(std::move(placed));
                }
            };

            // Frequency grid — only depends on static parameters.
            // Never laid out again during normal playback.
            if (m_gridEnabled && m_binsPerColumn > 1) {
                const bool freqStale = m_freqGridDirty
                    || !m_freqGrid.valid
                    || m_freqGrid.size != QSize(cw, ch)
                    || m_sampleRateHz != m_freqGridCachedSampleRateHz
                    || m_binsPerColumn != m_freqGridCachedBinsPerColumn;
                if (freqStale) {
                    updateFreqGridOverlayLocked(cw, ch);
                }
                placeOverlay(m_freqGrid, 0, freqGridFrame);
            }

            // Time grid — vertical lines + labels (bottom only), laid out
            // wider than the widget with right-side padding. Each frame
            // shifts it by the combined displayLeft + drawX delta, and it is
            // laid out again only when the shift exhausts the padding.
            // The time grid and the crosshair's readout follow the data,
            // not the scaled preview, so they wait for the data.
            if (m_gridEnabled && m_binsPerColumn > 1 && !zoomPreviewShown) {
                const int timeGridPadding = devicePixels(400.0, scale);
                // The pixel shift accounts for both displayLeft advancement
                // and drawX changes (critical in rolling mode where drawX
                // varies during the initial canvas fill phase).
                const double rawShift =
                    static_cast<double>(displayLeft - m_timeGridRenderDisplayLeft) * canvasZoom
                    - (drawX - m_timeGridRenderDrawX);
                int shift = static_cast<int>(std::round(rawShift));
                const bool timeStale = m_timeGridDirty
                    || !m_timeGrid.valid
                    || m_timeGrid.size != QSize(cw + timeGridPadding, ch)
                    || shift < 0 || shift > m_timeGridPadding
                    || rollingMode != m_timeGridCachedRollingMode;
                if (timeStale) {
                    const double cps =
                        static_cast<double>(m_precomputedSampleRateHz)
                        / static_cast<double>(m_precomputedHopSize);
                    updateTimeGridOverlayLocked(cw, ch, timeGridPadding, displayLeft, rollingMode, cps, drawX);
                    shift = 0;
                }
                placeOverlay(m_timeGrid, shift, timeGridFrame);
            }

            // Crosshair overlay — laid out again on mouse move or display
            // parameter change. Shown on the hovered pane (full crosshair)
            // and on non-hovered panes when crosshairSharedX >= 0 (vertical
            // line + bottom time label).
            if (m_crosshairEnabled
                && (m_hoverActive || m_crosshairSharedX >= 0.0)
                && !zoomPreviewShown) {
                bool crosshairTimeStale = false;
                if (m_showTimeLabels) {
                    crosshairTimeStale =
                        displayLeft != m_crosshairCachedDisplayLeft
                        || !qFuzzyCompare(drawX / scale + 1.0, m_crosshairCachedDrawX + 1.0)
                        || rollingMode != m_crosshairCachedRollingMode;
                }
                const bool crosshairStale = m_crosshairDirty
                    || !m_crosshair.valid
                    || m_crosshair.size != QSize(cw, ch)
                    || crosshairTimeStale
                    || m_binsPerColumn != m_crosshairCachedBinsPerColumn;
                if (crosshairStale) {
                    const double cps =
                        static_cast<double>(m_precomputedSampleRateHz)
                        / static_cast<double>(m_precomputedHopSize);
                    updateCrosshairOverlayLocked(cw, ch, displayLeft, rollingMode, cps, drawX);
                }
                placeOverlay(m_crosshair, 0, crosshairFrame);
            }
        } else if (!m_columns.empty() && m_binsPerColumn > 0) {
            // Appended rows are drawn one to a logical pixel.
            cw = w;
            ch = h;
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
                    canvasDrawX = drawX;
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
        QSGTexture *newTexture = node->createCanvasTexture(currentWindow, tileImages[i]);
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
                node->overlayTexture->setFiltering(QSGTexture::Nearest);
            }
        }
    }

    // Each fragment is set once, to what it shows this frame, so that the
    // ones that show the same as before stay untouched.
    node->ensureTileFragments(tileCount + 1);
    int fragmentCursor = 0;
    if (hasCanvas) {
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
                // A canvas awaiting its rasterization for another height
                // is stretched to this one.
                node->noteTextureNode(configurePreviewTextureNode(
                    node->tileFragments[fragmentCursor++],
                    tileTexture,
                    QRectF(
                        segmentTargetX,
                        0.0,
                        static_cast<double>(span),
                        static_cast<double>(ch)),
                    QRectF(withinTile, 0, span, canvasSize.height()),
                    preview,
                    static_cast<double>(cw),
                    canvasScale,
                    node->placeholderTexture));
                segmentOffset += span;
                segmentTargetX += static_cast<double>(span);
                remainingLength -= span;
            }
        };

        const int firstLength = std::min(canvasSize.width() - srcStart, drawCols);
        const int secondLength = drawCols - firstLength;
        configureSegment(srcStart, firstLength, canvasDrawX);
        if (secondLength > 0) {
            configureSegment(0, secondLength, canvasDrawX + static_cast<double>(firstLength));
        }

        // The newest column fills the rest of the view to its edge.
        const double latestWidth = scrollOffset + canvasEndGap;
        int latestTileIndex = latestX / kCanvasTileWidth;
        if (latestWidth > 0.0
            && latestTileIndex >= 0
            && latestTileIndex < node->tileTextures.size()) {
            QSGTexture *latestTexture = node->tileTextures[latestTileIndex];
            const int latestWithinTile = latestX - (latestTileIndex * kCanvasTileWidth);
            node->noteTextureNode(configurePreviewTextureNode(
                node->latest,
                latestTexture,
                QRectF(
                    canvasDrawX + static_cast<double>(drawCols),
                    0.0,
                    latestWidth,
                    static_cast<double>(ch)),
                QRectF(latestWithinTile, 0, 1, canvasSize.height()),
                preview,
                static_cast<double>(cw),
                canvasScale,
                node->placeholderTexture));
        } else {
            node->noteTextureNode(configureTextureNode(
                node->latest, nullptr, QRectF(), QRect(), node->placeholderTexture));
        }
    } else {
        node->noteTextureNode(configureTextureNode(
            node->latest, nullptr, QRectF(), QRect(), node->placeholderTexture));
    }
    for (int i = fragmentCursor; i < node->tileFragments.size(); ++i) {
        node->noteTextureNode(configureTextureNode(
            node->tileFragments[i], nullptr, QRectF(), QRect(), node->placeholderTexture));
    }

    if (showPlayhead) {
        node->playhead->setRect(playheadRect);
        node->playhead->setColor(QColor(255, 255, 255, 128));
    } else {
        node->playhead->setRect(QRectF());
    }

    // The grids and the crosshair: lines, and small images one texel to a
    // device pixel.
    const QColor gridLineColor(255, 255, 255, 50);
    node->showOverlay(node->freqGridLayer, freqGridFrame, gridLineColor, currentWindow);
    node->showOverlay(node->timeGridLayer, timeGridFrame, gridLineColor, currentWindow);
    node->showOverlay(node->crosshairLayer, crosshairFrame, QColor(), currentWindow);
    const QVector<QSGTexture *> unusedOverlayTextures = node->takeUnusedOverlayTextures();

    if (showOverlay && node->overlayTexture != nullptr && !overlaySize.isEmpty()) {
        const QRectF target(
            static_cast<double>(w) - static_cast<double>(overlaySize.width()) / scale - 8.0,
            4.0,
            static_cast<double>(overlaySize.width()) / scale,
            static_cast<double>(overlaySize.height()) / scale);
        node->noteTextureNode(configureTextureNode(
            node->overlay,
            node->overlayTexture,
            target,
            QRect(0, 0, overlaySize.width(), overlaySize.height()),
            node->placeholderTexture));
    } else {
        node->noteTextureNode(configureTextureNode(
            node->overlay, nullptr, QRectF(), QRect(), node->placeholderTexture));
    }

    delete oldOverlayTexture;
    qDeleteAll(unusedOverlayTextures);
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

SpectrogramSceneStatistics SpectrogramItem::sceneStatistics(const QSGNode *node) {
    return node != nullptr
        ? static_cast<const SpectrogramSceneNode *>(node)->statistics
        : SpectrogramSceneStatistics{};
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

void SpectrogramItem::itemChange(ItemChange change, const ItemChangeData &value) {
    QQuickItem::itemChange(change, value);
    if (change != ItemDevicePixelRatioHasChanged && change != ItemSceneChange) {
        return;
    }
    // Moving to a screen with another scale, or the scale changing.
    {
        QMutexLocker lock(&m_stateMutex);
        applyCanvasScaleLocked(windowDevicePixelRatio());
    }
    update();
}

void SpectrogramItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        QMutexLocker lock(&m_stateMutex);
        invalidateMapping();
        markCanvasStaleLocked();
        m_freqGridDirty = true;
        m_timeGridDirty = true;
        m_crosshairDirty = true;
    }
    const int newWidth = static_cast<int>(newGeometry.width());
    const int oldWidth = static_cast<int>(oldGeometry.width());
    if (newWidth != oldWidth && newWidth > 0) {
        // Keep the ring's width floor in sync with the Rust worker as soon
        // as geometry changes. The centered decoder can be parked while the
        // item is fullscreen, so waiting for the next data chunk may miss
        // the wide geometry entirely if the viewer is closed first.
        s_maxWidgetWidthSeen = std::max(s_maxWidgetWidthSeen, newWidth);
        emit spectrogramWidthChanged(newWidth);
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

    if (m_zoomPreviewShown && m_zoomPreviewPixelsPerSecond > 0.0) {
        // The scaled preview is on screen: seek where it shows.
        const double seconds = m_zoomPreviewAnchorSeconds
            + (event->position().x() - m_zoomPreviewAnchorX) / m_zoomPreviewPixelsPerSecond;
        lock.unlock();
        if (seconds < 0.0) {
            event->ignore();
            return;
        }
        event->accept();
        emit seekRequested(seconds);
        return;
    }

    qint64 displayLeft = m_crosshairCachedDisplayLeft;
    bool rollingMode = m_crosshairCachedRollingMode;
    double drawX = m_crosshairCachedDrawX;
    if (m_displayMode == 1
        && width() > 0.0
        && m_precomputedTotalColumnsEstimate > 0) {
        // The view the paint draws, held as it holds it.
        const CenteredView view = centeredViewLocked(
            currentRenderPositionSecondsLocked(std::chrono::steady_clock::now()),
            canvasWidthLocked(),
            canvasHeightLocked(),
            canvasZoomLocked(),
            columnsPerSecond);
        displayLeft = view.left;
        drawX = view.drawX / m_canvasScale;
        rollingMode = false;
    }

    // Where the crosshair is drawn: on the device pixel under the pointer.
    const double seconds = pixelToTimeSeconds(
        static_cast<double>(crosshairDeviceColumnLocked(
            event->position().x(), std::max(1, canvasWidthLocked()))) / m_canvasScale,
        displayLeft,
        rollingMode,
        m_rollingEpoch,
        columnsPerSecond,
        drawX,
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
    // Touchpads and high-resolution wheels zoom by part of a notch.
    // Pixel-only events count kWheelPixelsPerNotch pixels per notch.
    constexpr double kWheelAngleUnitsPerNotch = 120.0;
    constexpr double kWheelPixelsPerNotch = 120.0;
    double steps = event->angleDelta().y() / kWheelAngleUnitsPerNotch;
    if (event->angleDelta().y() == 0) {
        steps = event->pixelDelta().y() / kWheelPixelsPerNotch;
    }
    if (steps == 0.0) {
        // Horizontal scrolling is for someone else.
        event->ignore();
        return;
    }
    event->accept();

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
    // width, height and drawX are in device pixels; the labels are laid out
    // in logical ones.
    const double scale = m_canvasScale;
    const double logicalDrawX = drawX / scale;
    m_crosshair = SpectrogramOverlayLayout{};
    m_crosshair.size = QSize(width, height);
    m_crosshairVerticalLineRect = QRect();
    m_crosshairHorizontalLineRect = QRect();
    m_crosshairFreqLabelRect = QRect();
    m_crosshairTimeLabelRect = QRect();
    m_crosshairDirty = false;
    m_crosshairCachedDisplayLeft = displayLeft;
    m_crosshairCachedDrawX = logicalDrawX;
    m_crosshairCachedRollingMode = rollingMode;
    m_crosshairCachedBinsPerColumn = m_binsPerColumn;
    // Determine the effective X for the vertical crosshair line.
    // Hovered pane uses its own mouse position; non-hovered panes
    // use the shared X coordinate propagated from the hovered pane.
    const bool localHover = m_hoverActive;
    const bool hasSharedX = m_crosshairSharedX >= 0.0;
    if (!m_crosshairEnabled || (!localHover && !hasSharedX)
        || width <= 0 || height <= 0) {
        return;
    }

    // The crosshair is on the device column and row under the pointer.
    const int deviceX = crosshairDeviceColumnLocked(
        localHover ? m_hoverPosition.x() : m_crosshairSharedX, width);
    const int deviceY = crosshairDeviceColumnLocked(m_hoverPosition.y(), height);
    const int logicalWidth = std::max(1, static_cast<int>(std::floor(width / scale + 1e-6)));
    const int logicalHeight = std::max(1, static_cast<int>(std::floor(height / scale + 1e-6)));
    const int lineWidth = deviceLineWidth(scale);
    const QFontMetrics fm(overlayLabelFont(SpectrogramOverlayImage::CrosshairLabel));
    constexpr int pad = kCrosshairLabelPadding;
    m_crosshair.valid = true;

    // Dotted lines: full height on all panes (spanning them visually), and
    // full width on the hovered pane.
    m_crosshairVerticalLineRect = QRect(deviceX, 0, lineWidth, height);
    m_crosshair.images.push_back({SpectrogramOverlayImage::DottedColumn, QString(), height,
        QRectF(deviceX / scale, 0.0, lineWidth / scale, height / scale), QString()});
    if (localHover) {
        m_crosshairHorizontalLineRect = QRect(0, deviceY, width, lineWidth);
        m_crosshair.images.push_back({SpectrogramOverlayImage::DottedRow, QString(), width,
            QRectF(0.0, deviceY / scale, width / scale, lineWidth / scale), QString()});
    }

    // Frequency label at the right edge, aligned to the pointer.
    if (localHover) {
        const int mouseY = std::clamp(static_cast<int>(m_hoverPosition.y()), 0, logicalHeight - 1);
        const double freqHz = pixelToFrequencyHzLocked(deviceY, height);
        if (freqHz >= 0.0) {
            const QString text = formatFrequencyLabelPrecise(freqHz);
            const int textW = fm.horizontalAdvance(text);
            const int textH = fm.height();
            const int labelY = std::clamp(mouseY - textH / 2, 0, logicalHeight - textH - 2 * pad);
            const int labelX = logicalWidth - textW - 2 * pad - 2;
            m_crosshairFreqLabelRect = QRect(labelX, labelY, textW + 2 * pad, textH + 2 * pad);
            m_crosshair.images.push_back({SpectrogramOverlayImage::CrosshairLabel, text, 0,
                QRectF(m_crosshairFreqLabelRect), QString()});
        }
    }

    // Time label at the bottom edge — only on the bottom-most pane, shown
    // whenever any pane is hovered (via the shared X). Without the gapless
    // offset, it is the track's own time.
    if (m_showTimeLabels) {
        const double lineX = static_cast<double>(deviceX) / scale;
        const double continuousTime = pixelToTimeSeconds(
            lineX, displayLeft, rollingMode,
            m_rollingEpoch, columnsPerSecond, logicalDrawX, effectiveZoomLocked());
        const double trackTime = continuousTime - m_gaplessPositionOffset;
        if (trackTime >= 0.0) {
            const QString text = formatTimeLabelPrecise(trackTime);
            const int textW = fm.horizontalAdvance(text);
            const int textH = fm.height();
            const int labelX = std::clamp(
                static_cast<int>(lineX) - textW / 2, 0, logicalWidth - textW - 2 * pad);
            m_crosshairTimeLabelRect = QRect(
                labelX, logicalHeight - textH - 2 * pad - 2, textW + 2 * pad, textH + 2 * pad);
            m_crosshair.images.push_back({SpectrogramOverlayImage::CrosshairLabel, text, 0,
                QRectF(m_crosshairTimeLabelRect), QString()});
        }
    }
    nameOverlayImagesLocked(m_crosshair);
}

void SpectrogramItem::nameOverlayImagesLocked(SpectrogramOverlayLayout &layout) const {
    for (SpectrogramOverlayImage &image : layout.images) {
        image.key = overlayImageKey(image, m_canvasScale);
    }
}

int SpectrogramItem::crosshairDeviceColumnLocked(double logical, int devicePixelCount) const {
    return std::clamp(
        static_cast<int>(std::floor(logical * m_canvasScale + 1e-9)), 0, devicePixelCount - 1);
}

void SpectrogramItem::updateFreqGridOverlayLocked(int width, int height) {
    m_freqGrid = SpectrogramOverlayLayout{};
    m_freqGrid.size = QSize(width, height);
    m_freqGridDirty = false;
    m_freqGridCachedSampleRateHz = m_sampleRateHz;
    m_freqGridCachedBinsPerColumn = m_binsPerColumn;
    if (!m_gridEnabled || width <= 0 || height <= 0 || m_binsPerColumn <= 1) {
        return;
    }
    m_freqGrid.valid = true;

    // Lines on the canvas's device rows; labels laid out in logical pixels.
    const double scale = m_canvasScale;
    const int logicalWidth = std::max(1, static_cast<int>(std::floor(width / scale + 1e-6)));
    const int logicalHeight = std::max(1, static_cast<int>(std::floor(height / scale + 1e-6)));
    const int lineWidth = deviceLineWidth(scale);
    const QFontMetrics fm(overlayLabelFont(SpectrogramOverlayImage::GridLabel));
    constexpr int pad = kGridLabelPadding;
    constexpr int minPixelSpacing = 50;

    const double nyquist = 0.5 * static_cast<double>(m_sampleRateHz);

    static constexpr double kFreqCandidates[] = {
        50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000
    };
    const double hzPerPixel = nyquist / static_cast<double>(logicalHeight);
    const double freqInterval = selectGridInterval(
        kFreqCandidates,
        static_cast<int>(std::size(kFreqCandidates)),
        hzPerPixel,
        minPixelSpacing);

    int prevPixelY = -1;
    for (double freq = freqInterval; freq < nyquist; freq += freqInterval) {
        const int pixelY = frequencyToPixelYLocked(freq, height);
        if (pixelY < 0 || pixelY >= height) {
            continue;
        }
        if (prevPixelY >= 0
            && std::abs(pixelY - prevPixelY) < minPixelSpacing * scale) {
            continue;
        }
        prevPixelY = pixelY;
        m_freqGrid.lines.push_back(QRect(0, pixelY, width, lineWidth));

        const QString label = formatFrequencyLabel(freq);
        const int textW = fm.horizontalAdvance(label);
        const int textH = fm.height();
        const int labelY = std::clamp(
            static_cast<int>(pixelY / scale) - textH / 2, 0, logicalHeight - textH - 2 * pad);
        const int labelX = logicalWidth - textW - 2 * pad - 2;
        m_freqGrid.images.push_back({SpectrogramOverlayImage::GridLabel, label, 0,
            QRectF(labelX, labelY, textW + 2 * pad, textH + 2 * pad), QString()});
    }
    nameOverlayImagesLocked(m_freqGrid);
}

void SpectrogramItem::updateTimeGridOverlayLocked(
    int width, int height, int padding,
    qint64 displayLeft,
    bool rollingMode, double columnsPerSecond, double drawX) {
    // width, height, padding and drawX are in device pixels.
    m_timeGrid = SpectrogramOverlayLayout{};
    m_timeGrid.size = QSize(width + padding, height);
    m_timeGridDirty = false;
    m_timeGridRenderDisplayLeft = displayLeft;
    m_timeGridRenderDrawX = drawX;
    m_timeGridCachedRollingMode = rollingMode;
    m_timeGridPadding = padding;
    if (!m_gridEnabled || width <= 0 || height <= 0 || columnsPerSecond <= 0.0) {
        m_timeGridPadding = 0;
        return;
    }
    m_timeGrid.valid = true;

    const double scale = m_canvasScale;
    const int logicalHeight = std::max(1, static_cast<int>(std::floor(height / scale + 1e-6)));
    const int lineWidth = deviceLineWidth(scale);
    // Laid out wider than the widget so that each frame can shift it
    // instead of laying it out again.
    const int renderWidth = width + padding;
    const double logicalRenderWidth = renderWidth / scale;
    constexpr int minPixelSpacing = 50;

    static constexpr double kTimeCandidates[] = {
        1, 2, 5, 10, 15, 30, 60, 120, 300, 600
    };
    // Spacing in logical pixels; positions in device pixels.
    const double secondsPerPixel = 1.0 / (columnsPerSecond * effectiveZoomLocked());
    const double timeInterval = selectGridInterval(
        kTimeCandidates,
        static_cast<int>(std::size(kTimeCandidates)),
        secondsPerPixel,
        minPixelSpacing);

    // Compute time range for the full padded width.
    // These are in continuous (cross-track) time.
    const double cz = canvasZoomLocked();
    const double timeLeft = pixelToTimeSeconds(
        0.0, displayLeft, rollingMode, m_rollingEpoch, columnsPerSecond, drawX, cz);
    const double timeRight = pixelToTimeSeconds(
        static_cast<double>(renderWidth - 1), displayLeft, rollingMode,
        m_rollingEpoch, columnsPerSecond, drawX, cz);

    // Convert to per-track time for grid line snapping and labels.
    // m_gaplessPositionOffset is the continuous-time value where the
    // current track starts (0 when no gapless transition has occurred).
    const double trackTimeLeft = timeLeft - m_gaplessPositionOffset;
    const double trackTimeRight = timeRight - m_gaplessPositionOffset;

    const double startTrackTime = std::max(0.0,
        std::ceil(std::max(0.0, trackTimeLeft) / timeInterval) * timeInterval);

    const QFontMetrics fm(overlayLabelFont(SpectrogramOverlayImage::GridLabel));
    constexpr int pad = kGridLabelPadding;

    for (double trackT = startTrackTime; trackT <= trackTimeRight; trackT += timeInterval) {
        // Convert per-track time back to continuous time for pixel positioning.
        const double continuousT = trackT + m_gaplessPositionOffset;
        const double pxF = timeToPixelX(
            continuousT, displayLeft, rollingMode, m_rollingEpoch, columnsPerSecond, drawX,
            cz);
        const int pixelX = static_cast<int>(std::round(pxF));
        if (pixelX < 0 || pixelX >= renderWidth) {
            continue;
        }
        m_timeGrid.lines.push_back(QRect(pixelX, 0, lineWidth, height));

        if (m_showTimeLabels) {
            const QString label = formatTimeLabel(trackT);
            const int textW = fm.horizontalAdvance(label);
            const int textH = fm.height();
            const int labelX = std::clamp(
                static_cast<int>(pixelX / scale) - textW / 2,
                0,
                static_cast<int>(logicalRenderWidth) - textW - 2 * pad);
            m_timeGrid.images.push_back({SpectrogramOverlayImage::GridLabel, label, 0,
                QRectF(labelX, logicalHeight - textH - 2 * pad - 2,
                    textW + 2 * pad, textH + 2 * pad), QString()});
        }
    }
    nameOverlayImagesLocked(m_timeGrid);
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
    cancelPendingRasterLocked();
    m_canvas = QImage();
    m_rasterTiles.clear();
    m_canvasDirty = true;
    m_canvasWriteX = 0;
    m_canvasFilledCols = 0;
    m_precomputedCanvasDisplayLeft = 0;
    m_precomputedCanvasDisplayRight = -1;
    m_precomputedCanvasViewRight = -1;
    m_precomputedCanvasRolling = false;
    m_precomputedCanvasDirty = true;
    m_precomputedCanvasSubpixelOffset = 0.0;
    m_dirtyTiles.clear();
}

void SpectrogramItem::markCanvasStaleLocked() {
    // The canvas stays on screen until its replacement is rasterized.
    m_canvasDirty = true;
    m_precomputedCanvasDirty = true;
}

SpectrogramZoomPreview SpectrogramItem::staleCanvasAlignmentLocked(
    double canvasDrawX,
    int canvasPixels,
    double viewWidth,
    double viewDrawX,
    qint64 displayLeft,
    bool rollingMode,
    double canvasViewZoom,
    double columnsPerSecond) const {
    // In canvas pixels at this frame's scale; the canvas's own pixels may
    // be at another.
    SpectrogramZoomPreview alignment;
    const double canvasZoom = m_precomputedCanvasZoomLevel;
    // Centered columns are another track's after a track change: that
    // canvas stays as it is.
    if (canvasZoom <= 0.0
        || canvasViewZoom <= 0.0
        || m_precomputedCanvasDisplayRight < m_precomputedCanvasDisplayLeft
        || (!m_precomputedCanvasRolling
            && m_precomputedCanvasTrackToken != m_precomputedTrackToken)) {
        return alignment;
    }
    const double canvasColumnsPerSecond = m_precomputedCanvasColumnsPerSecond > 0.0
        ? m_precomputedCanvasColumnsPerSecond
        : columnsPerSecond;
    // Where the canvas's first pixel starts: before its first display
    // column, by as much as it has scrolled past it.
    const double canvasLeft = static_cast<double>(m_precomputedCanvasDisplayLeft)
        - m_precomputedCanvasSubpixelOffset / canvasZoom;
    // The canvas's first column, in this view's columns: the same ones in
    // the same mode and time scale (for rolling history, while the ring
    // keeps its write sequences), and else the column at its time.
    double viewColumn = canvasLeft;
    double scale = canvasViewZoom / canvasZoom;
    const bool sameColumns = rollingMode == m_precomputedCanvasRolling
        && std::abs(canvasColumnsPerSecond - columnsPerSecond) <= 1e-9 * columnsPerSecond
        && (!rollingMode || m_precomputedCanvasRingGeneration == m_ringGeneration);
    if (!sameColumns) {
        if (canvasColumnsPerSecond <= 0.0 || columnsPerSecond <= 0.0) {
            return alignment;
        }
        const double seconds =
            (canvasLeft - static_cast<double>(m_precomputedCanvasEpoch)) / canvasColumnsPerSecond;
        viewColumn = seconds * columnsPerSecond
            + (rollingMode ? static_cast<double>(m_rollingEpoch) : 0.0);
        scale = (canvasViewZoom * columnsPerSecond) / (canvasZoom * canvasColumnsPerSecond);
    }
    alignment.fromX = canvasDrawX;
    alignment.toX = viewDrawX + (viewColumn - static_cast<double>(displayLeft)) * canvasViewZoom;
    alignment.scale = scale;
    alignment.active = std::isfinite(scale)
        && scale > 0.0
        && std::isfinite(alignment.toX)
        && (std::abs(scale - 1.0) > 1e-9 || std::abs(alignment.toX - alignment.fromX) > 1e-6);
    if (!alignment.active) {
        return alignment;
    }
    // A jump, such as a seek, would move the canvas mostly or wholly off
    // screen and leave the view blank until its replacement arrives. It
    // stays where it is instead, as a zoom fill keeps it.
    const auto visibleWidth = [viewWidth](double left, double right) {
        return std::max(0.0, std::min(right, viewWidth) - std::max(left, 0.0));
    };
    const double shown = visibleWidth(canvasDrawX, canvasDrawX + canvasPixels);
    const double aligned = visibleWidth(
        alignment.map(canvasDrawX), alignment.map(canvasDrawX + canvasPixels));
    if (aligned < 0.5 * std::min(shown * scale, viewWidth)) {
        alignment.active = false;
    }
    return alignment;
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
    // The rasterization's copy of this tile misses what is drawn now.
    if (tileIndex < m_rasterTiles.size()) {
        m_rasterTiles[tileIndex] = QImage();
    }
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
        m_rasterTiles.clear();
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
    m_rasterTiles.clear();
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
    auto mappedBinAtRow = [this, height, maxBin](int row) -> int {
        const int clampedRow = std::clamp(row, 0, std::max(0, height - 1));
        if (static_cast<int>(m_iToBin.size()) == height) {
            return std::clamp(m_iToBin[static_cast<size_t>(clampedRow)], 0, maxBin);
        }
        if (height <= 1) {
            return maxBin;
        }
        const double fraction =
            static_cast<double>(clampedRow)
            / static_cast<double>(height - 1);
        return std::clamp(
            static_cast<int>(std::floor(fraction * static_cast<double>(maxBin))),
            0,
            maxBin);
    };

    for (int y = 0; y < height; ++y) {
        const int i = height - 1 - y;

        const int bin0 = mappedBinAtRow(i - 1);
        const int bin1 = mappedBinAtRow(i);
        const int bin2 = mappedBinAtRow(i + 1);

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
    // Rust reserves zero for silence/below-floor; 1..255 spans -150..0 dBFS.
    static constexpr double kBakedDbRange = 150.0;
    const double userDbRange = std::clamp(m_dbRange, 50.0, 150.0);
    for (int i = 1; i < 256; ++i) {
        const double db = (static_cast<double>(i - 1) / 254.0)
            * kBakedDbRange - kBakedDbRange;
        const double xdb = std::clamp(db + userDbRange, 0.0, userDbRange);
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

std::shared_ptr<const SpectrogramProjection> SpectrogramItem::precomputedProjectionLocked(
    int height) const {
    if (height < 0) {
        height = m_canvas.height();
    }
    const SpectrogramProjection::Key key{
        height, m_mappingHeight, m_binsPerColumn,
        m_precomputedBinsPerColumn, m_sampleRateHz, m_logScale,
        m_dbRange, m_channelMuted};
    if (m_precomputedProjection && m_precomputedProjection->key == key) {
        return m_precomputedProjection;
    }
    static QMutex cacheMutex;
    static std::map<SpectrogramProjection::Key, std::weak_ptr<const SpectrogramProjection>> cache;
    QMutexLocker lock(&cacheMutex);
    if (const auto found = cache.find(key); found != cache.end()) {
        if (auto shared = found->second.lock()) {
            m_precomputedProjection = std::move(shared);
            return m_precomputedProjection;
        }
    }
    std::erase_if(cache, [](const auto &entry) { return entry.second.expired(); });
    auto projection = std::make_shared<SpectrogramProjection>();
    projection->key = key;
    projection->binRanges.reserve(static_cast<size_t>(std::max(0, height)));
    const int mapSize = static_cast<int>(m_iToBin.size());
    const int bins = std::max(1, m_precomputedBinsPerColumn);
    for (int y = 0; y < height; ++y) {
        const int mi = height - 1 - y;
        int lo = 0;
        int hi = 0;
        if (mi >= 0 && mi < mapSize) {
            const int center = m_iToBin[static_cast<size_t>(mi)];
            const int previous = mi > 0 ? m_iToBin[static_cast<size_t>(mi - 1)] : center;
            const int next = mi + 1 < mapSize ? m_iToBin[static_cast<size_t>(mi + 1)] : center;
            lo = previous + (center - previous) / 2;
            hi = center + (next - center) / 2;
            if (lo == previous && previous != center) lo = center;
            if (hi == next && next != center) hi = center;
            if (lo > hi) std::swap(lo, hi);
            lo = std::clamp(lo, 0, bins - 1);
            hi = std::clamp(hi, 0, bins - 1);
        }
        projection->binRanges.emplace_back(lo, hi);
    }
    const auto remap = buildPrecomputedDbRemapLocked();
    const auto &palette = m_channelMuted ? m_palette32Gray : m_palette32;
    const double scale = static_cast<double>(kGradientTableSize) / 255.0;
    for (size_t raw = 0; raw < projection->colors.size(); ++raw) {
        const int paletteIndex = std::clamp(kGradientTableSize
            - static_cast<int>(std::lround(scale * remap[raw])), 0, kGradientTableSize - 1);
        projection->colors[raw] = palette[static_cast<size_t>(paletteIndex)];
    }
    m_precomputedProjection = projection;
    cache[key] = projection;
    return m_precomputedProjection;
}

const quint8 *SpectrogramItem::ringColumnLocked(qint64 displayIndex, bool rollingMode) const {
    const int slot = ringSlotForDisplayIndexLocked(displayIndex, rollingMode);
    return slot >= 0 ? m_ring.column(slot) : nullptr;
}

void SpectrogramItem::drawPrecomputedColumnAtLocked(
    int x,
    qint64 displayIndex,
    bool rollingMode,
    const SpectrogramProjection &projection) {
    if (!canvasColumnDrawable(m_canvas, x, projection) || m_precomputedBinsPerColumn <= 0) {
        return;
    }
    markTileDirtyLocked(x);
    const auto source = spectrogramColumns([this, rollingMode](qint64 index) {
        return ringColumnLocked(index, rollingMode);
    });
    SpectrogramRowMaxima maxima(source, projection, false);
    rasterColumn(m_canvas, x, displayIndex, maxima);
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

void SpectrogramItem::drawPrecomputedPixelsLocked(
    int firstCanvasX,
    int firstPixel,
    int endPixel,
    double subpixelOffset,
    qint64 displayLeft,
    qint64 displayRight,
    bool rollingMode,
    double canvasZoom,
    const SpectrogramProjection &projection) {
    if (!canvasColumnDrawable(m_canvas, 0, projection)
        || m_precomputedBinsPerColumn <= 0
        || canvasZoom <= 0.0
        || displayRight < displayLeft
        || endPixel <= firstPixel) {
        return;
    }
    const int width = m_canvas.width();
    const auto source = spectrogramColumns([this, rollingMode](qint64 index) {
        return ringColumnLocked(index, rollingMode);
    });
    SpectrogramRowMaxima maxima(source, projection, canvasZoom > 1.001);
    for (int pixel = firstPixel; pixel < endPixel; ++pixel) {
        const int canvasX = ((firstCanvasX + pixel - firstPixel) % width + width) % width;
        markTileDirtyLocked(canvasX);
        rasterPixel(m_canvas, canvasX, static_cast<double>(pixel) - subpixelOffset,
            displayLeft, displayRight, canvasZoom, maxima);
    }
}

std::shared_ptr<SpectrogramRasterJob> SpectrogramItem::makeRasterJobLocked(
    int width,
    int height,
    qint64 displayLeft,
    qint64 displayRight,
    bool rollingMode) {
    if (width <= 0 || height <= 0 || displayRight < displayLeft
        || m_precomputedBinsPerColumn <= 0) {
        return {};
    }
    ensureMapping(height);
    auto job = std::make_shared<SpectrogramRasterJob>();
    SpectrogramRasterTarget &target = job->target;
    target.width = width;
    target.height = height;
    target.scale = m_canvasScale;
    target.displayLeft = displayLeft;
    target.displayRight = displayRight;
    target.rollingMode = rollingMode;
    target.effectiveZoom = canvasZoomLocked();
    target.columnsPerSecond = m_precomputedHopSize > 0
        ? static_cast<double>(m_precomputedSampleRateHz)
            / static_cast<double>(m_precomputedHopSize)
        : 0.0;
    target.epoch = rollingMode ? m_rollingEpoch : 0;
    target.trackToken = m_precomputedTrackToken;
    target.ringGeneration = m_ringGeneration;
    target.capturedAt = std::chrono::steady_clock::now();
    target.projection = precomputedProjectionLocked(height);
    // Copying the ring shares its blocks; only the slot lookup, one per
    // display column, costs anything here.
    job->ring = m_ring;
    const qint64 columnCount = displayRight - displayLeft + 1;
    job->columnSlots.resize(static_cast<size_t>(columnCount));
    for (qint64 i = 0; i < columnCount; ++i) {
        job->columnSlots[static_cast<size_t>(i)] =
            ringSlotForDisplayIndexLocked(displayLeft + i, rollingMode);
    }
    return job;
}

std::shared_ptr<SpectrogramRasterResult> SpectrogramItem::rasterize(
    const SpectrogramRasterJob &job,
    const std::atomic<quint64> *latestSerial) {
    const SpectrogramRasterTarget &target = job.target;
    const auto wanted = [&job, latestSerial]() {
        return latestSerial == nullptr
            || latestSerial->load(std::memory_order_relaxed) == job.serial;
    };
    if (!wanted() || target.projection == nullptr) {
        return {};
    }
    QImage canvas(target.width, target.height, QImage::Format_RGB32);
    if (canvas.isNull()
        || static_cast<int>(target.projection->binRanges.size()) != target.height) {
        return {};
    }
    const auto source = spectrogramColumns([&job](qint64 index) -> const quint8 * {
        const qint64 offset = index - job.target.displayLeft;
        if (offset < 0 || offset >= static_cast<qint64>(job.columnSlots.size())) {
            return nullptr;
        }
        const int slot = job.columnSlots[static_cast<size_t>(offset)];
        return slot >= 0 ? job.ring.column(slot) : nullptr;
    });
    const int drawPixels = rasterDrawPixels(
        target.width, target.displayLeft, target.displayRight, target.effectiveZoom, target.scale);
    // Every pixel drawn is written; the rest is background.
    if (drawPixels < target.width) {
        const QRgb background = kBackgroundColor.rgba();
        for (int y = 0; y < target.height; ++y) {
            auto *line = reinterpret_cast<QRgb *>(canvas.scanLine(y));
            std::fill(line + drawPixels, line + target.width, background);
        }
    }
    SpectrogramRowMaxima maxima(source, *target.projection, target.effectiveZoom > 1.001);
    for (int px = 0; px < drawPixels; ++px) {
        // A newer rasterization replaced this one: stop.
        if (px % kCanvasTileWidth == 0 && !wanted()) {
            return {};
        }
        rasterPixel(canvas, px, static_cast<double>(px), target.displayLeft,
            target.displayRight, target.effectiveZoom, maxima);
    }
    auto result = std::make_shared<SpectrogramRasterResult>();
    result->serial = job.serial;
    result->target = target;
    result->drawPixels = drawPixels;
    result->columnSlots = job.columnSlots;
    const int tileCount = std::max(1, (target.width + kCanvasTileWidth - 1) / kCanvasTileWidth);
    result->tiles.reserve(tileCount);
    for (int tile = 0; tile < tileCount; ++tile) {
        const int tileStart = tile * kCanvasTileWidth;
        result->tiles.push_back(canvas.copy(
            tileStart, 0, std::min(kCanvasTileWidth, target.width - tileStart), target.height));
    }
    result->canvas = std::move(canvas);
    return result;
}

void SpectrogramItem::requestPrecomputedRasterLocked(
    int width,
    int height,
    qint64 displayLeft,
    qint64 displayRight,
    bool rollingMode) {
    std::shared_ptr<SpectrogramRasterJob> job =
        makeRasterJobLocked(width, height, displayLeft, displayRight, rollingMode);
    if (!job) {
        return;
    }
    // A canvas shown through the zoom preview stays so until this result
    // replaces it, which beats showing it as it is for a few frames.
    m_rasterHoldsZoomPreview = m_zoomPreviewWantedLastFrame
        || (m_rasterPending && m_rasterHoldsZoomPreview);
    job->serial = ++m_rasterSerial;
    // Any older rasterization stops at its next check.
    m_rasterMailbox->latestSerial.store(job->serial, std::memory_order_relaxed);
    m_rasterPending = true;
    m_rasterTarget = job->target;
    m_precomputedCanvasDirty = false;
    spectrogramRasterPool().start([job, mailbox = m_rasterMailbox]() {
        std::shared_ptr<SpectrogramRasterResult> result =
            rasterize(*job, &mailbox->latestSerial);
        if (!result) {
            return;
        }
        QMutexLocker lock(&mailbox->mutex);
        SpectrogramItem *owner = mailbox->owner;
        if (owner == nullptr
            || mailbox->latestSerial.load(std::memory_order_relaxed) != result->serial) {
            return;
        }
        // Holding the mutex keeps the owner alive while this posts; its
        // destruction discards the event if it comes first.
        QMetaObject::invokeMethod(
            owner, [owner, result]() { owner->installRasterResult(result); },
            Qt::QueuedConnection);
    });
}

bool SpectrogramItem::pendingRasterOutdatedLocked(
    int width,
    int height,
    qint64 displayLeft,
    qint64 displayRight,
    bool rollingMode) const {
    const SpectrogramRasterTarget &target = m_rasterTarget;
    const double canvasZoom = canvasZoomLocked();
    const double columnsPerSecond = m_precomputedHopSize > 0
        ? static_cast<double>(m_precomputedSampleRateHz)
            / static_cast<double>(m_precomputedHopSize)
        : 0.0;
    if (target.width != width
        || target.height != height
        || std::abs(target.scale - m_canvasScale) > 1e-9
        || target.rollingMode != rollingMode
        || std::abs(target.effectiveZoom - canvasZoom) > 0.001
        || std::abs(target.columnsPerSecond - columnsPerSecond)
            > 1e-9 * std::max(1.0, columnsPerSecond)
        || (rollingMode && target.epoch != m_rollingEpoch)
        || (!rollingMode && target.trackToken != m_precomputedTrackToken)
        || target.ringGeneration != m_ringGeneration
        || target.projection != precomputedProjectionLocked(height)) {
        return true;
    }
    // Columns arriving while it rasterizes are drawn in once it arrives, as
    // the view moves on. A view behind it, or a canvas width ahead, needs
    // another one.
    const qint64 targetRight = rasterDisplayRight(
        target.displayLeft,
        rasterDrawPixels(
            width, target.displayLeft, target.displayRight, target.effectiveZoom, target.scale),
        target.effectiveZoom);
    return displayLeft < target.displayLeft
        || displayRight < targetRight
        || static_cast<double>(displayLeft - target.displayLeft) * canvasZoom
            >= static_cast<double>(width);
}

void SpectrogramItem::cancelPendingRasterLocked() {
    if (!m_rasterPending) {
        return;
    }
    m_rasterPending = false;
    m_rasterHoldsZoomPreview = false;
    ++m_rasterSerial;
    m_rasterMailbox->latestSerial.store(m_rasterSerial, std::memory_order_relaxed);
    m_precomputedCanvasDirty = true;
}

void SpectrogramItem::installRasterResult(
    const std::shared_ptr<SpectrogramRasterResult> &result) {
    {
        QMutexLocker lock(&m_stateMutex);
        // Only the newest rasterization may replace the canvas.
        if (!m_rasterPending || result->serial != m_rasterSerial) {
            return;
        }
        m_rasterPending = false;
        m_rasterHoldsZoomPreview = false;
        if (result->target.ringGeneration != m_ringGeneration) {
            m_precomputedCanvasDirty = true;
        } else {
            installRasterResultLocked(*result);
        }
    }
    update();
}

bool SpectrogramItem::installRasterResultLocked(const SpectrogramRasterResult &result) {
    const SpectrogramRasterTarget &target = result.target;
    if (result.canvas.isNull() || target.width <= 0) {
        return false;
    }
    // Freeing the canvas it replaces takes a millisecond: a worker does it.
    QImage replacedCanvas = std::exchange(m_canvas, result.canvas);
    QVector<QImage> replacedTiles = std::exchange(m_rasterTiles, result.tiles);
    if (!replacedCanvas.isNull() || !replacedTiles.isEmpty()) {
        spectrogramRasterPool().start(
            [replacedCanvas = std::move(replacedCanvas),
             replacedTiles = std::move(replacedTiles)]() {});
    }
    resizeDirtyTilesLocked();
    markAllTilesDirtyLocked();
    m_canvasWriteX = result.drawPixels % target.width;
    m_canvasFilledCols = result.drawPixels;
    m_precomputedCanvasDisplayLeft = target.displayLeft;
    m_precomputedCanvasDisplayRight =
        rasterDisplayRight(target.displayLeft, result.drawPixels, target.effectiveZoom);
    m_precomputedCanvasViewRight = target.displayRight;
    m_precomputedCanvasRolling = target.rollingMode;
    m_precomputedCanvasZoomLevel = target.effectiveZoom;
    m_precomputedCanvasScale = target.scale;
    m_precomputedCanvasSubpixelOffset = 0.0;
    m_precomputedCanvasColumnsPerSecond = target.columnsPerSecond;
    m_precomputedCanvasEpoch = target.epoch;
    m_precomputedCanvasTrackToken = target.trackToken;
    m_precomputedCanvasRingGeneration = target.ringGeneration;
    m_precomputedCanvasDrawnAt = target.capturedAt;
    m_precomputedCanvasPixelsPerSecond =
        target.effectiveZoom / target.scale * target.columnsPerSecond;
    if (!target.rollingMode) {
        // Draw in the centered columns that arrived during the rasterization.
        qint64 runStart = -1;
        for (size_t i = 0; i <= result.columnSlots.size(); ++i) {
            const qint64 column = target.displayLeft + static_cast<qint64>(i);
            const bool arrived = i < result.columnSlots.size()
                && result.columnSlots[i] < 0
                && ringSlotForDisplayIndexLocked(column, false) >= 0;
            if (arrived && runStart < 0) {
                runStart = column;
            } else if (!arrived && runStart >= 0) {
                drawCenteredColumnsInPlaceLocked(runStart, column - 1, target.effectiveZoom);
                runStart = -1;
            }
        }
    }
    return true;
}

void SpectrogramItem::drawCenteredColumnsInPlaceLocked(
    qint64 firstColumn, qint64 lastColumn, double canvasZoom) {
    const qint64 dispL = m_precomputedCanvasDisplayLeft;
    const qint64 dispR = m_precomputedCanvasDisplayRight;
    const qint64 viewRight = std::max(dispR, m_precomputedCanvasViewRight);
    // A canvas from rolling mode, or one awaiting its rasterization for
    // another height, has no place for them.
    if (m_canvas.isNull()
        || m_canvasFilledCols <= 0
        || m_precomputedCanvasRolling
        || dispR < dispL
        || canvasZoom <= 0.0
        || m_canvas.height() != m_mappingHeight
        || firstColumn > viewRight
        || lastColumn < dispL) {
        return;
    }
    const auto projectionOwner = precomputedProjectionLocked();
    const auto &projection = *projectionOwner;
    const int canvasWidth = m_canvas.width();
    const int drawCols = std::min(m_canvasFilledCols, canvasWidth);
    const int canvasStart =
        (m_canvasWriteX - drawCols + canvasWidth) % canvasWidth;
    const double subpixelOffset = m_precomputedCanvasSubpixelOffset;
    // The pixels that show these columns, or blend them with a neighbour,
    // drawn as a rasterization of the canvas would. Once centered playback
    // starts scrolling, pixel 0 no longer lives at canvas x=0: they go
    // through the circular canvas offset. Writing late right-edge data to
    // the pixel's own x leaves the visible circular slot permanently black.
    int firstPixel = std::max(0, static_cast<int>(std::floor(
        static_cast<double>(firstColumn - 1 - dispL) * canvasZoom + subpixelOffset)) - 1);
    while (firstPixel < drawCols
        && rasterPixelColumns(static_cast<double>(firstPixel) - subpixelOffset,
               dispL, viewRight, canvasZoom).second < firstColumn) {
        ++firstPixel;
    }
    int endPixel = firstPixel;
    while (endPixel < drawCols
        && rasterPixelColumns(static_cast<double>(endPixel) - subpixelOffset,
               dispL, viewRight, canvasZoom).first <= lastColumn) {
        ++endPixel;
    }
    drawPrecomputedPixelsLocked(
        (canvasStart + firstPixel) % canvasWidth, firstPixel, endPixel, subpixelOffset,
        dispL, viewRight, false, canvasZoom, projection);
}

int SpectrogramItem::maxSyncAppendPixelsLocked(int canvasWidth, double canvasZoom) const {
    // The render sync moves the canvas on as far as playback can have moved
    // it since it was drawn, with half again for uneven frames, and at least
    // 128 canvas pixels (about a millisecond at 719 rows and 4097 bins, and
    // less at device pixels, whose columns share their bins). That is
    // 6000 columns a second at 384 kHz and 16x zoom: 200 pixels a frame at
    // 30 fps, and more after a slow frame or a slow rasterization, whose
    // columns are caught up here. A jump beyond that, such as a seek, goes
    // to a worker like a full rebuild, and so does a whole canvas.
    constexpr int kMinSyncAppendPixels = 128;
    constexpr double kPlaybackSlack = 1.5;
    const double rate = std::max(
        m_clockState.rate, m_positionJumpHoldActive ? m_positionHoldTrajectory.rate : 0.0);
    double playbackPixels = 0.0;
    if (rate > 0.0
        && m_precomputedHopSize > 0
        && m_precomputedCanvasDrawnAt != std::chrono::steady_clock::time_point{}) {
        const double elapsedSeconds = std::max(0.0, std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_precomputedCanvasDrawnAt).count());
        const double columnsPerSecond = static_cast<double>(m_precomputedSampleRateHz)
            / static_cast<double>(m_precomputedHopSize);
        playbackPixels =
            elapsedSeconds * rate * columnsPerSecond * canvasZoom * kPlaybackSlack;
    }
    const double budget = std::max(static_cast<double>(kMinSyncAppendPixels), std::ceil(playbackPixels));
    return static_cast<int>(std::min(static_cast<double>(canvasWidth - 1), budget));
}

bool SpectrogramItem::advancePrecomputedCanvasLocked(
    qint64 displayLeft,
    qint64 displayRight,
    bool rollingMode) {
    const double canvasZoom = canvasZoomLocked();
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
    const double oldCanvasZoom = m_precomputedCanvasZoomLevel;
    if (std::abs(canvasZoom - oldCanvasZoom) > 0.001) {
        return false;
    }

    const int nextVisibleCols = std::min(
        width,
        static_cast<int>(std::max<qint64>(0, displayRight - displayLeft + 1)));
    if (nextVisibleCols <= 0) {
        m_canvasFilledCols = 0;
        m_precomputedCanvasDisplayLeft = displayLeft;
        m_precomputedCanvasDisplayRight = displayLeft - 1;
        m_precomputedCanvasViewRight = displayRight;
        m_precomputedCanvasDirty = false;
        m_precomputedCanvasSubpixelOffset = 0.0;
        m_precomputedCanvasDrawnAt = std::chrono::steady_clock::now();
        return true;
    }

    if (std::abs(canvasZoom - 1.0) <= 0.001) {
        const qint64 appendStart =
            std::max(m_precomputedCanvasDisplayRight + 1, displayLeft);
        const qint64 appendCount =
            std::max<qint64>(0, displayRight - appendStart + 1);
        if (appendCount > maxSyncAppendPixelsLocked(width, canvasZoom)) {
            return false;
        }

        if (appendCount > 0) {
            // A pixel to a column: pixel i of the view is column
            // displayLeft + i.
            const auto projectionOwner = precomputedProjectionLocked();
            drawPrecomputedPixelsLocked(
                m_canvasWriteX,
                static_cast<int>(appendStart - displayLeft),
                static_cast<int>(displayRight - displayLeft + 1),
                0.0, displayLeft, displayRight, rollingMode, 1.0, *projectionOwner);
            m_canvasWriteX = static_cast<int>((m_canvasWriteX + appendCount) % width);
        }

        m_canvasFilledCols = nextVisibleCols;
        m_precomputedCanvasDisplayLeft = displayLeft;
        m_precomputedCanvasDisplayRight =
            displayLeft + static_cast<qint64>(nextVisibleCols) - 1;
        m_precomputedCanvasViewRight = displayRight;
        m_precomputedCanvasDirty = false;
        m_precomputedCanvasSubpixelOffset = 0.0;
        m_precomputedCanvasDrawnAt = std::chrono::steady_clock::now();
        return true;
    }

    // Any other number of pixels to a column, such as a column's two device
    // pixels at a scale of 2: the canvas scrolls by whole pixels and keeps
    // the part of a pixel it has scrolled past. Its pixels then start that
    // much before where a rasterization from displayLeft would put them, and
    // the new ones are drawn so, which makes the canvas what a rasterization
    // of the same pixels would be.
    const auto fallBack = [&]() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        if (m_profileEnabled) {
            noteSmoothnessAdvanceFallbackLocked(canvasZoom);
        }
#endif
        return false;
    };
    const int oldFilledPixels = std::clamp(m_canvasFilledCols, 0, width);
    const qint64 advanceColumns = displayLeft - m_precomputedCanvasDisplayLeft;
    const double requestedShiftPixels =
        m_precomputedCanvasSubpixelOffset
        + static_cast<double>(advanceColumns) * canvasZoom;
    const int shiftPixels = std::max(
        0,
        static_cast<int>(std::floor(requestedShiftPixels + 1e-9)));
    double nextSubpixelOffset = std::clamp(
        requestedShiftPixels - static_cast<double>(shiftPixels),
        0.0,
        0.999999);
    if (nextSubpixelOffset < 1e-9) {
        nextSubpixelOffset = 0.0;
    }

    if (shiftPixels >= width) {
        return fallBack();
    }

    const int newFilledPixels = std::min(
        width,
        static_cast<int>(std::ceil(
            static_cast<double>(displayRight - displayLeft + 1) * canvasZoom
            + nextSubpixelOffset - 1e-9)));
    const int preservedPixels = std::max(0, oldFilledPixels - shiftPixels);
    if (newFilledPixels < preservedPixels) {
        return fallBack();
    }
    // Pixels at the old right end were drawn without the columns after it:
    // draw them again with those.
    const qint64 oldViewRight = std::max(
        m_precomputedCanvasDisplayRight, m_precomputedCanvasViewRight);
    int firstPixel = preservedPixels;
    if (displayRight > oldViewRight) {
        while (firstPixel > 0
            && rasterPixelColumns(
                   static_cast<double>(firstPixel - 1) - nextSubpixelOffset,
                   displayLeft, std::numeric_limits<qint64>::max(), canvasZoom).second
                > oldViewRight) {
            --firstPixel;
        }
    }
    if (newFilledPixels - firstPixel > maxSyncAppendPixelsLocked(width, canvasZoom)) {
        return fallBack();
    }

    if (newFilledPixels > firstPixel) {
        const auto projectionOwner = precomputedProjectionLocked();
        const int firstCanvasX =
            ((m_canvasWriteX - (preservedPixels - firstPixel)) % width + width) % width;
        drawPrecomputedPixelsLocked(
            firstCanvasX, firstPixel, newFilledPixels, nextSubpixelOffset,
            displayLeft, displayRight, rollingMode, canvasZoom, *projectionOwner);
        m_canvasWriteX = (firstCanvasX + newFilledPixels - firstPixel) % width;
    }

    m_canvasFilledCols = newFilledPixels;
    m_precomputedCanvasDisplayLeft = displayLeft;
    m_precomputedCanvasDisplayRight =
        newFilledPixels > 0
            ? (displayLeft + static_cast<qint64>(std::floor(
                   (static_cast<double>(newFilledPixels) - nextSubpixelOffset) / canvasZoom
                   + 1e-9)) - 1)
            : (displayLeft - 1);
    m_precomputedCanvasViewRight = displayRight;
    m_precomputedCanvasDirty = false;
    m_precomputedCanvasSubpixelOffset = nextSubpixelOffset;
    m_precomputedCanvasDrawnAt = std::chrono::steady_clock::now();
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

    m_overlayImage = QImage(
        devicePixels(imageWidth, m_canvasScale), devicePixels(imageHeight, m_canvasScale),
        QImage::Format_ARGB32_Premultiplied);
    m_overlayImage.setDevicePixelRatio(m_canvasScale);
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
        resetFpsEstimateLocked();
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
    if (!isVisible()) return;
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
        // A hold for the start of a gapless successor ends then; one for
        // data that does not arrive gives up after a timeout.
        // A stopped or paused playhead may wait for as long as it stays.
        const bool expired = m_positionJumpHoldUntilNs != 0
            ? monotonicNs(now) >= m_positionJumpHoldUntilNs
            : m_clockState.advancing()
                && std::chrono::duration<double>(now - m_positionJumpHoldStartedAt).count()
                    >= kPositionJumpHoldTimeoutSeconds;
        if (expired) {
            releasePositionHoldLocked(now);
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
    // Only sustain the render loop while playing, and only with frames
    // that differ. When stopped or paused the display is static and
    // re-renders are triggered on demand by data feeds
    // (feedPrecomputedChunk) or property changes.
    if (changed) {
        update();
    } else if (precomputedActive && playing) {
        schedulePlaybackFrame();
    }
}

qint64 SpectrogramItem::playbackFrameDelayNsLocked(qint64 nowNs) const {
    if (!m_idleView.valid || positionHeldLocked(nowNs)) {
        return 0;
    }
    // Paused, or playback ends first: until the clock changes course.
    const std::optional<qint64> changesAt =
        m_clockState.reachesAt(m_idleView.changeSeconds - m_gaplessPositionOffset);
    if (!changesAt) {
        return -1;
    }
    return *changesAt > nowNs ? *changesAt - nowNs : 0;
}

void SpectrogramItem::schedulePlaybackFrame() {
    qint64 delayNs = 0;
    {
        QMutexLocker lock(&m_stateMutex);
        if (!m_precomputedReady || !m_playing) {
            m_playbackFrameTimer->stop();
            return;
        }
        delayNs = playbackFrameDelayNsLocked(monotonicNowNs());
    }
    if (delayNs == 0) {
        m_playbackFrameTimer->stop();
        requestPlaybackFrame();
    } else if (delayNs < 0) {
        // Nothing moves until the clock changes course, which reschedules.
        m_playbackFrameTimer->stop();
    } else {
        // Whole milliseconds, rounded up: the frame is not due before.
        const qint64 delayMs = (delayNs + 999'999) / 1'000'000;
        m_playbackFrameTimer->start(static_cast<int>(std::min<qint64>(delayMs, 3'600'000)));
    }
}

void SpectrogramItem::requestPlaybackFrame() {
    ++m_playbackFrameRequests;
    update();
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
            "[ui-spectrogram] smoothness_hitch_detected sample_rate_hz=%d rows_per_second=%.3f frames=%d pending_frames=%d gap_frames=%d severe_gap_frames=%d pending_gap_frames=%d stall_clusters=%d regressions=%d pending_max=%d drain_passes=%d drained=%d advance_fallback_frames=%d advance_fallback_non_unity=%d advance_fallback_max_zoom_delta=%.6f paint_spikes=%d max_gap_ms=%.3f advanced=%d\n",
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
            m_smoothnessProfile.advanceFallbackFrames,
            m_smoothnessProfile.nonUnityAdvanceFallbackFrames,
            m_smoothnessProfile.maxAdvanceFallbackZoomDelta,
            m_smoothnessProfile.paintSpikeCount,
            m_smoothnessProfile.maxGapMs,
            advanced ? 1 : 0);
    }
}

void SpectrogramItem::noteSmoothnessAdvanceFallbackLocked(double effectiveZoom) {
    if (!m_smoothnessProfile.active) {
        return;
    }

    m_smoothnessProfile.advanceFallbackFrames += 1;
    const double zoomDelta = std::abs(effectiveZoom - 1.0);
    if (zoomDelta > 0.001) {
        m_smoothnessProfile.nonUnityAdvanceFallbackFrames += 1;
        m_smoothnessProfile.maxAdvanceFallbackZoomDelta =
            std::max(m_smoothnessProfile.maxAdvanceFallbackZoomDelta, zoomDelta);
    }

    if (m_profileEnabled
        && zoomDelta > 0.001
        && shouldLogProfileSpike(
            &m_profileLastAdvanceFallbackSpike,
            std::chrono::steady_clock::now(),
            0.10)) {
        FERROUS_SPECTROGRAM_LOGF(
            stderr,
            "[ui-spectrogram] advance_fallback effective_zoom=%.6f delta=%.6f frames=%d non_unity=%d\n",
            effectiveZoom,
            zoomDelta,
            m_smoothnessProfile.advanceFallbackFrames,
            m_smoothnessProfile.nonUnityAdvanceFallbackFrames);
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
    summary.insert(
        QStringLiteral("advanceFallbackFrames"),
        m_smoothnessProfile.advanceFallbackFrames);
    summary.insert(
        QStringLiteral("nonUnityAdvanceFallbackFrames"),
        m_smoothnessProfile.nonUnityAdvanceFallbackFrames);
    summary.insert(
        QStringLiteral("maxAdvanceFallbackZoomDelta"),
        m_smoothnessProfile.maxAdvanceFallbackZoomDelta);
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
        "[ui-spectrogram] smoothness_window reason=%s sample_rate_hz=%d rows_per_second=%.3f frames=%d pending_frames=%d gap_frames=%d severe_gap_frames=%d pending_gap_frames=%d stall_clusters=%d regressions=%d drain_passes=%d drained=%d pending_max=%d advance_fallback_frames=%d advance_fallback_non_unity=%d advance_fallback_max_zoom_delta=%.6f paint_spikes=%d max_gap_ms=%.3f max_paint_ms=%.3f avg_paint_ms=%.3f incident=%d\n",
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
        m_smoothnessProfile.advanceFallbackFrames,
        m_smoothnessProfile.nonUnityAdvanceFallbackFrames,
        m_smoothnessProfile.maxAdvanceFallbackZoomDelta,
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
        state.insert(
            QStringLiteral("advanceFallbackFrames"),
            m_smoothnessProfile.advanceFallbackFrames);
        state.insert(
            QStringLiteral("nonUnityAdvanceFallbackFrames"),
            m_smoothnessProfile.nonUnityAdvanceFallbackFrames);
        state.insert(
            QStringLiteral("maxAdvanceFallbackZoomDelta"),
            m_smoothnessProfile.maxAdvanceFallbackZoomDelta);
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
    const quint64 lastLoggedSummary =
        g_lastLoggedSeekProfileSummaryGeneration.load(std::memory_order_relaxed);
    if (generation == 0
        || generation == m_seekProfile.generation
        || generation == m_lastFinalizedSeekProfileGeneration
        || generation <= lastLoggedSummary
        || !SpectrogramSeekTrace::isActive(nowMs)) {
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
        if (claimMonotonicGeneration(
                &g_lastLoggedSeekProfileIncidentGeneration,
                m_seekProfile.generation)) {
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

    if (claimMonotonicGeneration(
            &g_lastLoggedSeekProfileSummaryGeneration,
            m_seekProfile.generation)) {
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
    }

    const QVariantMap lastSummary = m_seekProfile.lastSummary;
    m_lastFinalizedSeekProfileGeneration = m_seekProfile.generation;
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

void SpectrogramItem::resetFpsEstimateLocked() {
    m_overlayDirty = true;
    m_fpsInitialized = false;
    m_fpsValue = 0;
    m_fpsAccumFrames = 0;
    m_fpsAccumSeconds = 0.0;
}

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
