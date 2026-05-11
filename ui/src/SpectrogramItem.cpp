// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramItem.h"

#include "SpectrogramBodyMaterial.h"
#include "SpectrogramRenderUtils.h"
#include "SpectrogramSeekTrace.h"
#include "SpectrogramTraceLogging.h"

#include <QDir>
#include <QFontMetrics>
#include <QDateTime>
#include <QHoverEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QWheelEvent>
#include <QMutexLocker>
#include <QPainter>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGRendererInterface>
#include <QSGNode>
#include <QSGSimpleRectNode>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSGVertexColorMaterial>
#include <QString>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
#define FERROUS_SPECTROGRAM_TRACE_LOGF(...)              \
    do {                                                 \
        if (SpectrogramTraceLogging::detailedEnabled()) { \
            std::fprintf(__VA_ARGS__);                   \
        }                                                \
    } while (false)
#define FERROUS_SPECTROGRAM_PROFILE_LOGF(...)                 \
    do {                                                      \
        if (SpectrogramTraceLogging::profileSummaryEnabled()) { \
            std::fprintf(__VA_ARGS__);                        \
        }                                                     \
    } while (false)
#else
#define FERROUS_SPECTROGRAM_TRACE_LOGF(...) \
    do {                                    \
    } while (false)
#define FERROUS_SPECTROGRAM_PROFILE_LOGF(...) \
    do {                                      \
    } while (false)
#endif

namespace {
constexpr double kMinFreqHz = 25.0;
constexpr double kReferenceHopSamples = 1024.0;
constexpr double kPositionJumpHoldThresholdSeconds = 0.75;
constexpr double kPositionHeartbeatRegressionToleranceSeconds = 0.001;
constexpr int kPlayheadLineAlpha = 128;
constexpr int kCrosshairLineAlpha = 128;
// Last-resort fallback only: the hold is normally released by
// applyPrecomputedResetLocked() when the reset data arrives (~200–400 ms
// after a seek or track change).  The timeout must be long enough that the
// data-driven release always wins, while still being short enough to
// recover if the backend never sends a reset (e.g. stream interruption).
constexpr double kPositionJumpHoldTimeoutSeconds = 2.0;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
constexpr qint64 kSeekTraceWindowMs = 1800;
constexpr qint64 kSmoothnessWindowMs = 2000;
constexpr qint64 kSmoothnessIdleMs = 450;
constexpr double kPrecomputedFeedPhaseLogThresholdMs = 8.0;
#endif
const QColor kBackgroundColor(0, 0, 0);
const QColor kOverlayColor(190, 190, 200, 150);
std::atomic<int> g_gpuSpectrogramPaintLogBudget{12};
std::atomic<int> g_zoomFillRenderLogBudget{80};
std::atomic<int> g_zoomFillSourceUploadLogBudget{80};
std::atomic<int> g_gpuDebugStageLogBudget{24};
std::atomic<int> g_gpuMaterialLogBudget{24};
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
std::atomic<quint64> g_lastLoggedSeekProfileIncidentGeneration{0};
std::atomic<quint64> g_lastLoggedSeekProfileSummaryGeneration{0};
#endif

int readNonNegativeEnvInt(const char *key, int fallback) {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(key, &ok);
    if (!ok || value < 0) {
        return fallback;
    }
    return value;
}

bool centeredDecodedTailLooksFinal(
    qint64 decodedColumnCount,
    qint64 estimatedColumnCount,
    qint64 visibleWindowColumns) {
    if (decodedColumnCount <= 0 || estimatedColumnCount <= 0) {
        return false;
    }
    const qint64 eofSlackColumns =
        std::max<qint64>(64, std::max<qint64>(1, visibleWindowColumns));
    return decodedColumnCount + eofSlackColumns >= estimatedColumnCount;
}

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
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

struct DebugFrameStats {
    int nonBlackPixels{0};
    double averageLuma{0.0};
};

DebugFrameStats analyzeDebugFrame(const QImage &image) {
    DebugFrameStats stats;
    if (image.isNull()) {
        return stats;
    }

    qint64 lumaSum = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            const int luma = (2126 * color.red() + 7152 * color.green() + 722 * color.blue()) / 10000;
            lumaSum += luma;
            if (luma > 8) {
                stats.nonBlackPixels += 1;
            }
        }
    }

    const qint64 pixelCount = static_cast<qint64>(image.width()) * static_cast<qint64>(image.height());
    if (pixelCount > 0) {
        stats.averageLuma = static_cast<double>(lumaSum) / static_cast<double>(pixelCount);
    }
    return stats;
}

int expectedNativeHopForZoom(const double zoomLevel) {
    const double clampedZoom = std::clamp(zoomLevel, 0.05, 16.0);
    const double zoomOutOversample = clampedZoom < 0.999 ? 2.0 : 1.0;
    return static_cast<int>(
        std::llround(kReferenceHopSamples / (clampedZoom * zoomOutOversample)));
}

bool effectiveHopMatchesZoom(int hopSize, double zoomLevel) {
    if (hopSize <= 0) {
        return false;
    }
    const int expectedHop = expectedNativeHopForZoom(zoomLevel);
    const int tolerance = std::max(2, static_cast<int>(std::ceil(
        static_cast<double>(expectedHop) * 0.01)));
    return std::abs(hopSize - expectedHop) <= tolerance;
}

double linearInterpolate(double y1, double y2, double mu) {
    return y1 * (1.0 - mu) + y2 * mu;
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

struct PrecomputedFeedPhaseProfile {
    using Clock = std::chrono::steady_clock;

    Clock::time_point startedAt{Clock::now()};
    Clock::time_point phaseStartedAt{startedAt};
    double lockWaitMs{0.0};
    double gateMs{0.0};
    double freezeMs{0.0};
    double resetMs{0.0};
    double sessionMs{0.0};
    double metadataMs{0.0};
    double allocMs{0.0};
    double writeMs{0.0};
    double readyMs{0.0};
    double zoomResetMs{0.0};
    double emitUpdateMs{0.0};
    bool resetPendingBefore{false};
    bool resetPendingAfter{false};
    bool zoomFillActiveBefore{false};
    bool zoomFillActiveAfter{false};
    bool frozenRetainedBefore{false};
    bool frozenRetainedAfter{false};
    bool gpuImageBefore{false};
    bool gpuImageAfter{false};
    bool appliedReset{false};
    bool appliedImplicitReset{false};
    bool mustRealloc{false};
    int ringCapacityBefore{0};
    int ringCapacityAfter{0};
    qint64 ringBytesBefore{0};
    qint64 ringBytesAfter{0};
    int neededCapacity{0};
    qint64 neededBytes{0};
    qint64 ringFillAfter{0};
    int maxColumnAfter{-1};
    int displayMode{0};

    void mark(double &bucket) {
        const Clock::time_point now = Clock::now();
        bucket +=
            std::chrono::duration<double, std::milli>(now - phaseStartedAt)
                .count();
        phaseStartedAt = now;
    }

    double totalMs() const {
        return std::chrono::duration<double, std::milli>(
                   Clock::now() - startedAt)
            .count();
    }
};
#endif

void bumpImageGeneration(quint64 &generation) {
    generation += 1;
}

struct SpectrogramTailSegment {
    QSGSimpleTextureNode *node{nullptr};
    QSGTexture *texture{nullptr};
    int width{0};
};

struct RetainedBodySegmentNode {
    QSGSimpleTextureNode *textureNode{nullptr};
    QSGGeometryNode *bodySampleNode{nullptr};
    QSGGeometryNode *sourceSampleNode{nullptr};
    QSGGeometryNode *stockMaterialNode{nullptr};
};

struct RetainedBodyDrawSegment {
    QRectF targetRect;
    QRect sourceRect;
    QSGTexture *texture{nullptr};
};

struct SpectrogramSceneNode final : public QSGNode {
    SpectrogramSceneNode() {
        spectrogramTextureDebugMode = SpectrogramGpu::currentDebugMode();
        background = new QSGSimpleRectNode();
        spectrogramBodySampleNode = createSpectrogramBodySampleMaterialNode();
        spectrogramTailBodySampleNode = createSpectrogramBodySampleMaterialNode();
        spectrogramSourceSampleNode = createSpectrogramSourceSampleMaterialNode();
        spectrogramTailSourceSampleNode = createSpectrogramSourceSampleMaterialNode();
        spectrogramStockMaterialNode = createSpectrogramStockMaterialNode();
        spectrogramTailStockMaterialNode = createSpectrogramStockMaterialNode();
        spectrogramTexture = new QSGSimpleTextureNode();
        spectrogramTailTexture = new QSGSimpleTextureNode();
        overlayLines = new QSGGeometryNode();
        playhead = new QSGSimpleRectNode();
        freqGridOverlay = new QSGSimpleTextureNode();
        timeGridOverlay = new QSGSimpleTextureNode();
        crosshairOverlay = new QSGSimpleTextureNode();
        overlay = new QSGSimpleTextureNode();
        appendChildNode(background);
        if (spectrogramTextureDebugMode == SpectrogramGpu::DebugMode::TextureNode) {
            appendChildNode(spectrogramTexture);
            appendChildNode(spectrogramTailTexture);
            appendChildNode(spectrogramBodySampleNode);
            appendChildNode(spectrogramTailBodySampleNode);
            appendChildNode(spectrogramSourceSampleNode);
            appendChildNode(spectrogramTailSourceSampleNode);
            appendChildNode(spectrogramStockMaterialNode);
            appendChildNode(spectrogramTailStockMaterialNode);
        } else {
            appendChildNode(spectrogramBodySampleNode);
            appendChildNode(spectrogramTailBodySampleNode);
            appendChildNode(spectrogramSourceSampleNode);
            appendChildNode(spectrogramTailSourceSampleNode);
            appendChildNode(spectrogramStockMaterialNode);
            appendChildNode(spectrogramTailStockMaterialNode);
            appendChildNode(spectrogramTexture);
            appendChildNode(spectrogramTailTexture);
        }
        appendChildNode(overlayLines);
        appendChildNode(playhead);
        appendChildNode(freqGridOverlay);
        appendChildNode(timeGridOverlay);
        appendChildNode(crosshairOverlay);
        appendChildNode(overlay);

        auto *lineGeometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        lineGeometry->setDrawingMode(QSGGeometry::DrawLines);
        lineGeometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
        overlayLines->setGeometry(lineGeometry);
        overlayLines->setFlag(QSGNode::OwnsGeometry);

        auto *lineMaterial = new QSGVertexColorMaterial();
        lineMaterial->setFlag(QSGMaterial::Blending, true);
        overlayLines->setMaterial(lineMaterial);
        overlayLines->setFlag(QSGNode::OwnsMaterial);
    }

    ~SpectrogramSceneNode() override {
        delete spectrogramTextureResource;
        delete spectrogramTailTextureResource;
        delete spectrogramDbRemapTextureResource;
        delete spectrogramPaletteTextureResource;
        delete spectrogramRowLutTextureResource;
        for (auto &segment : spectrogramTailSegments) {
            delete segment.texture;
        }
        delete overlayTexture;
        delete freqGridOverlayTexture;
        delete timeGridOverlayTexture;
        delete crosshairOverlayTexture;
        delete placeholderTexture;
    }

    QSGSimpleRectNode *background{nullptr};
    QSGGeometryNode *spectrogramBodySampleNode{nullptr};
    QSGGeometryNode *spectrogramTailBodySampleNode{nullptr};
    QSGGeometryNode *spectrogramSourceSampleNode{nullptr};
    QSGGeometryNode *spectrogramTailSourceSampleNode{nullptr};
    QSGGeometryNode *spectrogramStockMaterialNode{nullptr};
    QSGGeometryNode *spectrogramTailStockMaterialNode{nullptr};
    QSGSimpleTextureNode *spectrogramTexture{nullptr};
    QSGSimpleTextureNode *spectrogramTailTexture{nullptr};
    QSGGeometryNode *overlayLines{nullptr};
    QSGSimpleRectNode *playhead{nullptr};
    QSGSimpleTextureNode *freqGridOverlay{nullptr};
    QSGSimpleTextureNode *timeGridOverlay{nullptr};
    QSGSimpleTextureNode *crosshairOverlay{nullptr};
    QSGSimpleTextureNode *overlay{nullptr};
    QSGTexture *spectrogramTextureResource{nullptr};
    QSGTexture *spectrogramTailTextureResource{nullptr};
    QSGTexture *spectrogramDbRemapTextureResource{nullptr};
    QSGTexture *spectrogramPaletteTextureResource{nullptr};
    QSGTexture *spectrogramRowLutTextureResource{nullptr};
    QSGTexture *overlayTexture{nullptr};
    QSGTexture *freqGridOverlayTexture{nullptr};
    QSGTexture *timeGridOverlayTexture{nullptr};
    QSGTexture *crosshairOverlayTexture{nullptr};
    QSGTexture *placeholderTexture{nullptr};
    quint64 spectrogramTextureGeneration{0};
    quint64 spectrogramDbRemapTextureGeneration{0};
    quint64 spectrogramPaletteTextureGeneration{0};
    quint64 spectrogramRowLutTextureGeneration{0};
    SpectrogramGpu::DebugMode spectrogramTextureDebugMode{
        SpectrogramGpu::DebugMode::Full};
    bool spectrogramTextureUploaded{false};
    int spectrogramTextureScrollPixels{0};
    int spectrogramTextureWidth{0};
    int spectrogramTailTextureWidth{0};
    QVector<SpectrogramTailSegment> spectrogramTailSegments;
    QVector<RetainedBodySegmentNode> retainedBodySegmentNodes;
    quintptr ownerWindowId{0};
    quint64 generation{0};
};

QImage placeholderImage() {
    QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    return image;
}

void writeRgba8888GrayPixel(uchar *scanLine, int x, quint8 value) {
    if (scanLine == nullptr || x < 0) {
        return;
    }

    uchar *pixel = scanLine + (x * 4);
    pixel[0] = value;
    pixel[1] = value;
    pixel[2] = value;
    pixel[3] = 255;
}

void configureTextureNode(
    QSGSimpleTextureNode *node,
    QSGTexture *texture,
    const QRectF &target,
    const QRect &source,
    QSGTexture *placeholderTexture,
    QSGSimpleTextureNode::TextureCoordinatesTransformMode textureTransform =
        QSGSimpleTextureNode::NoTransform) {
    if (node == nullptr) {
        return;
    }
    if (texture == nullptr || target.isEmpty() || source.isEmpty()) {
        if (placeholderTexture != nullptr) {
            node->setTexture(placeholderTexture);
        }
        node->setRect(QRectF());
        node->setSourceRect(QRectF(0.0, 0.0, 1.0, 1.0));
        node->setTextureCoordinatesTransform(QSGSimpleTextureNode::NoTransform);
        return;
    }

    node->setTexture(texture);
    node->setFiltering(QSGTexture::Nearest);
    node->setRect(target);
    node->setSourceRect(QRectF(source));
    node->setTextureCoordinatesTransform(textureTransform);
}

QSGTexture *createStandaloneWindowTexture(
    QQuickWindow *window,
    const QImage &image,
    QSGTexture::Filtering filtering) {
    if (window == nullptr || image.isNull()) {
        return nullptr;
    }

    QSGTexture *texture = window->createTextureFromImage(image);
    if (texture == nullptr) {
        return nullptr;
    }

    if (texture->isAtlasTexture()) {
        QSGTexture *standaloneTexture = texture->removedFromAtlas();
        if (standaloneTexture != texture) {
            delete texture;
            texture = standaloneTexture;
        }
    }

    if (texture != nullptr) {
        texture->setFiltering(filtering);
        texture->setMipmapFiltering(QSGTexture::None);
        texture->setHorizontalWrapMode(QSGTexture::ClampToEdge);
        texture->setVerticalWrapMode(QSGTexture::ClampToEdge);
    }
    return texture;
}

void clearTailSegments(SpectrogramSceneNode *node) {
    if (node == nullptr) {
        return;
    }
    for (auto &segment : node->spectrogramTailSegments) {
        delete segment.texture;
        if (segment.node != nullptr) {
            node->removeChildNode(segment.node);
            delete segment.node;
        }
    }
    node->spectrogramTailSegments.clear();
}

RetainedBodySegmentNode createRetainedBodySegmentNode(SpectrogramSceneNode *root) {
    RetainedBodySegmentNode segment;
    segment.textureNode = new QSGSimpleTextureNode();
    segment.bodySampleNode = createSpectrogramBodySampleMaterialNode();
    segment.sourceSampleNode = createSpectrogramSourceSampleMaterialNode();
    segment.stockMaterialNode = createSpectrogramStockMaterialNode();
    if (root != nullptr) {
        root->insertChildNodeBefore(segment.textureNode, root->overlayLines);
        root->insertChildNodeBefore(segment.bodySampleNode, root->overlayLines);
        root->insertChildNodeBefore(segment.sourceSampleNode, root->overlayLines);
        root->insertChildNodeBefore(segment.stockMaterialNode, root->overlayLines);
    }
    return segment;
}

void ensureRetainedBodySegmentNodeCount(SpectrogramSceneNode *node, qsizetype count) {
    if (node == nullptr) {
        return;
    }
    while (node->retainedBodySegmentNodes.size() < count) {
        node->retainedBodySegmentNodes.push_back(createRetainedBodySegmentNode(node));
    }
}

SpectrogramBodySampleMaterial::State retainedBodySampleStateForSegment(
    const RetainedBodyDrawSegment &segment,
    QSGTexture *dbRemapTexture,
    QSGTexture *paletteTexture,
    QSGTexture *rowLutTexture,
    QSGTexture *placeholderTexture) {
    SpectrogramBodySampleMaterial::State state;
    state.sourceTexture = segment.texture != nullptr ? segment.texture : placeholderTexture;
    state.dbRemapTexture = dbRemapTexture != nullptr ? dbRemapTexture : placeholderTexture;
    state.paletteTexture = paletteTexture != nullptr ? paletteTexture : placeholderTexture;
    state.rowLutTexture = rowLutTexture != nullptr ? rowLutTexture : placeholderTexture;
    state.sourceRect = QRectF(segment.sourceRect);
    state.drawOffset = QPointF();
    state.sourceTextureSize =
        segment.texture != nullptr ? segment.texture->textureSize() : QSize();
    state.effectiveZoom =
        segment.sourceRect.width() > 0
            ? static_cast<float>(
                  segment.targetRect.width()
                  / static_cast<double>(segment.sourceRect.width()))
            : 1.0F;
    state.sourceColumnOffset = static_cast<float>(segment.sourceRect.x());
    return state;
}

void syncRetainedBodySegmentNode(
    const RetainedBodySegmentNode &node,
    const RetainedBodyDrawSegment *segment,
    SpectrogramGpu::DebugMode debugMode,
    QSGTexture *dbRemapTexture,
    QSGTexture *paletteTexture,
    QSGTexture *rowLutTexture,
    QSGTexture *placeholderTexture) {
    const bool haveSegment =
        segment != nullptr
        && segment->texture != nullptr
        && !segment->targetRect.isEmpty()
        && !segment->sourceRect.isEmpty();
    const QRectF targetRect = haveSegment ? segment->targetRect : QRectF();
    const QRectF sourceRect = haveSegment ? QRectF(segment->sourceRect) : QRectF();
    const auto textureTransform =
        QSGSimpleTextureNode::TextureCoordinatesTransformMode(
            QSGSimpleTextureNode::MirrorVertically);

    if (debugMode == SpectrogramGpu::DebugMode::TextureNode) {
        configureTextureNode(
            node.textureNode,
            haveSegment ? segment->texture : nullptr,
            targetRect,
            haveSegment ? segment->sourceRect : QRect(),
            placeholderTexture,
            textureTransform);
    } else {
        configureTextureNode(
            node.textureNode,
            nullptr,
            QRectF(),
            QRect(),
            placeholderTexture);
    }

    const bool useFull = debugMode == SpectrogramGpu::DebugMode::Full;
    const bool useSourceOnly = debugMode == SpectrogramGpu::DebugMode::ShaderSourceOnly;
    const bool useStock = debugMode == SpectrogramGpu::DebugMode::StockMaterial;
    SpectrogramBodySampleMaterial::State sampleState =
        haveSegment
            ? retainedBodySampleStateForSegment(
                  *segment,
                  dbRemapTexture,
                  paletteTexture,
                  rowLutTexture,
                  placeholderTexture)
            : retainedBodySampleStateForSegment(
                  RetainedBodyDrawSegment{},
                  dbRemapTexture,
                  paletteTexture,
                  rowLutTexture,
                  placeholderTexture);

    syncSpectrogramBodySampleMaterialNode(
        node.bodySampleNode,
        haveSegment && useFull ? targetRect : QRectF(),
        sampleState);
    syncSpectrogramSourceSampleMaterialNode(
        node.sourceSampleNode,
        haveSegment && useSourceOnly ? targetRect : QRectF(),
        haveSegment && useSourceOnly ? segment->texture : nullptr,
        haveSegment && useSourceOnly ? sourceRect : QRectF(),
        placeholderTexture);
    syncSpectrogramStockMaterialNode(
        node.stockMaterialNode,
        haveSegment && useStock ? targetRect : QRectF(),
        haveSegment && useStock ? segment->texture : nullptr,
        haveSegment && useStock ? sourceRect : QRectF(),
        placeholderTexture);
}

void configureLineNode(
    QSGGeometryNode *node,
    const QVector<QLineF> &freqLines,
    const QVector<QLineF> &timeLines,
    const std::optional<QLineF> &playheadLine,
    const std::optional<QLineF> &crosshairVerticalLine,
    const std::optional<QLineF> &crosshairHorizontalLine) {
    if (node == nullptr || node->geometry() == nullptr) {
        return;
    }

    struct LineSample {
        QLineF line;
        QColor color;
    };

    auto appendSolidLine = [](QVector<LineSample> &samples, const QLineF &line, const QColor &color) {
        samples.push_back(LineSample{line, color});
    };

    auto appendDottedLine = [](QVector<LineSample> &samples, const QLineF &line, const QColor &color) {
        constexpr double kDotLength = 1.0;
        constexpr double kDotGap = 1.0;

        const bool vertical = qFuzzyCompare(line.x1() + 1.0, line.x2() + 1.0);
        const double fixedCoordinate = vertical ? line.x1() : line.y1();
        const double start = vertical
            ? std::min(line.y1(), line.y2())
            : std::min(line.x1(), line.x2());
        const double end = vertical
            ? std::max(line.y1(), line.y2())
            : std::max(line.x1(), line.x2());

        for (double pos = start; pos <= end; pos += kDotLength + kDotGap) {
            const double segmentEnd = std::min(pos + kDotLength, end);
            if (vertical) {
                samples.push_back(LineSample{
                    QLineF(fixedCoordinate, pos, fixedCoordinate, segmentEnd),
                    color,
                });
            } else {
                samples.push_back(LineSample{
                    QLineF(pos, fixedCoordinate, segmentEnd, fixedCoordinate),
                    color,
                });
            }
        }
    };

    QVector<LineSample> lines;
    lines.reserve(freqLines.size() + timeLines.size() + 16);

    const QColor gridColor(255, 255, 255, 50);
    const QColor playheadColor(255, 255, 255, kPlayheadLineAlpha);
    const QColor crosshairColor(255, 255, 255, kCrosshairLineAlpha);

    for (const QLineF &line : freqLines) {
        appendSolidLine(lines, line, gridColor);
    }
    for (const QLineF &line : timeLines) {
        appendSolidLine(lines, line, gridColor);
    }
    if (playheadLine.has_value()) {
        appendSolidLine(lines, *playheadLine, playheadColor);
    }
    if (crosshairVerticalLine.has_value()) {
        appendDottedLine(lines, *crosshairVerticalLine, crosshairColor);
    }
    if (crosshairHorizontalLine.has_value()) {
        appendDottedLine(lines, *crosshairHorizontalLine, crosshairColor);
    }

    auto *geometry = node->geometry();
    geometry->allocate(static_cast<int>(lines.size()) * 2);
    geometry->setDrawingMode(QSGGeometry::DrawLines);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    geometry->setLineWidth(1.0f);

    auto *vertices = geometry->vertexDataAsColoredPoint2D();
    auto premultiplied = [](int channel, int alpha) -> uchar {
        return static_cast<uchar>((channel * alpha + 127) / 255);
    };
    int index = 0;
    for (const LineSample &sample : lines) {
        const QColor &color = sample.color;
        const int alpha = color.alpha();
        const uchar red = premultiplied(color.red(), alpha);
        const uchar green = premultiplied(color.green(), alpha);
        const uchar blue = premultiplied(color.blue(), alpha);
        vertices[index++].set(
            static_cast<float>(sample.line.x1()),
            static_cast<float>(sample.line.y1()),
            red,
            green,
            blue,
            static_cast<uchar>(alpha));
        vertices[index++].set(
            static_cast<float>(sample.line.x2()),
            static_cast<float>(sample.line.y2()),
            red,
            green,
            blue,
            static_cast<uchar>(alpha));
    }

    geometry->markVertexDataDirty();
    node->markDirty(QSGNode::DirtyGeometry);
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

// Shared across all SpectrogramItem instances so the ring-cap floor
// survives instance tear-downs (channel-count changes destroy and
// recreate the widgets).  Matches the singleton tracker on the Rust
// side in AnalysisRuntimeState.
int SpectrogramItem::s_maxWidgetWidthSeen = 0;

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
    m_gpuDumpFramesRemaining = readNonNegativeEnvInt("FERROUS_GPU_DUMP_FRAMES", 0);
    m_gpuDumpDirectory = qEnvironmentVariable(
        "FERROUS_GPU_DUMP_DIR",
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("ferrous-gpu-dumps")));
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
    invalidateRenderCachesLocked();
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
    invalidateRenderCachesLocked();
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
    invalidateRenderCachesLocked();
    m_freqGridDirty = true;
    m_timeGridDirty = true;
    m_crosshairDirty = true;
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
        const bool largePositionJump = m_playing
            && std::abs(clamped - currentPosition) >= kPositionJumpHoldThresholdSeconds;
        const bool largeJump = m_precomputedReady && largePositionJump;
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
        if (largePositionJump && centeredSeek) {
            m_positionJumpHoldActive = false;
            setPositionAnchorLocked(clamped, now);
            changed = true;
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
        const bool smallCorrection =
            m_displayMode == 0 && std::abs(error) < kServoMaxErrorSeconds;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        if (m_profileEnabled
            && m_smoothnessProfile.active
            && (regressedDuringPlayback
                || (smallCorrection && std::abs(error) >= 0.001))) {
            noteSmoothnessServoLocked(error, regressedDuringPlayback);
        }
#endif
        if (!changed) {
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
    {
        QMutexLocker lock(&m_stateMutex);
        const int previousMode = m_displayMode;
        m_displayMode = clamped;
        if (previousMode == 1 && clamped == 0) {
            reanchorRollingEpochForCurrentDataLocked(std::chrono::steady_clock::now());
        }
    }
    emit displayModeChanged();
    if (m_precomputedReady) {
        m_precomputedLastRightCol = -1;
        invalidateRenderCachesLocked();
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
        invalidateRenderCachesLocked();
    }
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

qint64 SpectrogramItem::referenceTrackColumnsAtLevelZeroLocked() const {
    const qint64 totalCols = std::max(
        static_cast<qint64>(m_precomputedTotalColumnsEstimate),
        m_precomputedMaxColumnIndex >= 0
            ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
            : static_cast<qint64>(0));
    if (totalCols <= 0) {
        return 0;
    }
    return m_precomputedHopSize > 0
        ? totalCols * static_cast<qint64>(m_precomputedHopSize)
              / static_cast<qint64>(kReferenceHopSamples)
        : totalCols;
}

bool SpectrogramItem::hasFullTrackOverviewLocked() const {
    const qint64 referenceCols = referenceTrackColumnsAtLevelZeroLocked();
    if (referenceCols <= 0 || m_precomputedTrackToken == 0) {
        return false;
    }
    for (const SpectrogramLodPage &page : m_spectrogramLodPages) {
        if (page.trackToken != m_precomputedTrackToken
            || page.sourceSpan <= 1
            || page.startColumn > 0
            || page.endColumn < page.startColumn) {
            continue;
        }
        const qint64 coveredReferenceColumns =
            (page.endColumn + 1) * static_cast<qint64>(page.sourceSpan);
        if (coveredReferenceColumns >= referenceCols) {
            return true;
        }
    }
    return false;
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
    const qint64 referenceCols = referenceTrackColumnsAtLevelZeroLocked();
    if (referenceCols <= 0) {
        return 1.0;
    }
    if (referenceCols <= static_cast<qint64>(w)) {
        return 1.0;
    }
    const double trackFit =
        static_cast<double>(w) / static_cast<double>(referenceCols);
    if (hasFullTrackOverviewLocked()) {
        return std::max(trackFit, 0.005);
    }
    // Mirror the Rust-side clamp in AnalysisCommand::SetSpectrogramZoomLevel
    // (`.clamp(0.05, 16.0)`).  If Qt lets the user request a zoom below
    // Rust's floor, setSpectrogramZoomLevel(0.04) gets clamped to 0.05
    // on the backend, the session restarts at a slightly different
    // effective hop than Qt expected, and the next widthSettle-driven
    // re-send gets clamped the same way — the user sees their zoom-out
    // attempts "restart the same zoom level over and over".
    constexpr double kBackendMinZoom = 0.05;
    return std::max(trackFit, kBackendMinZoom);
}

double SpectrogramItem::minimumZoomLevel() const {
    QMutexLocker lock(&m_stateMutex);
    return minimumZoomLevelLocked();
}

SpectrogramItem::SpectrogramLodSelection SpectrogramItem::selectSpectrogramLodLevelForTests(
    const double effectiveZoom,
    const QVector<int> &availableLevels) const {
    if (availableLevels.isEmpty()) {
        return {};
    }
    const double columnsPerPixel = 1.0 / std::max(0.001, effectiveZoom);
    const double desiredFactor = std::max(1.0, columnsPerPixel);
    const int desiredLevel = static_cast<int>(std::floor(std::log2(desiredFactor)));
    int lowerLevel = std::clamp(desiredLevel, availableLevels.front(), availableLevels.back());
    int upperLevel = std::min(lowerLevel + 1, availableLevels.back());
    if (!availableLevels.contains(lowerLevel)) {
        lowerLevel = availableLevels.front();
        for (const int level : availableLevels) {
            if (level <= desiredLevel) {
                lowerLevel = level;
            }
        }
    }
    if (!availableLevels.contains(upperLevel)) {
        upperLevel = lowerLevel;
        for (const int level : availableLevels) {
            if (level > lowerLevel) {
                upperLevel = level;
                break;
            }
        }
    }
    const double lowerFactor = static_cast<double>(1 << lowerLevel);
    const double upperFactor = static_cast<double>(1 << upperLevel);
    const double blend = upperLevel == lowerLevel
        ? 0.0
        : std::clamp((desiredFactor - lowerFactor) / (upperFactor - lowerFactor), 0.0, 1.0);
    return SpectrogramLodSelection{
        .primaryLevel = lowerLevel,
        .secondaryLevel = upperLevel,
        .blend = blend,
    };
}

QVector<int> SpectrogramItem::availableSpectrogramLodLevelsForTests() const {
    QMutexLocker lock(&m_stateMutex);
    QVector<int> levels;
    if (m_precomputedReady || m_ringCapacity > 0) {
        levels.push_back(0);
    }
    for (const SpectrogramLodPage &page : m_spectrogramLodPages) {
        if (!levels.contains(page.level)) {
            levels.push_back(page.level);
        }
    }
    std::sort(levels.begin(), levels.end());
    return levels;
}

int SpectrogramItem::spectrogramLodPlaneCountForTests(const int level) const {
    QMutexLocker lock(&m_stateMutex);
    for (const SpectrogramLodPage &page : m_spectrogramLodPages) {
        if (page.level == level) {
            return page.planeCount;
        }
    }
    return level == 0 ? 1 : 0;
}

SpectrogramItem::SpectrogramRenderSourceState SpectrogramItem::spectrogramRenderSourceForTests() const {
    QMutexLocker lock(&m_stateMutex);
    if (m_precomputedHopSize > 0 && m_precomputedHopSize == m_requestedNativeHopSize) {
        return SpectrogramRenderSourceState{
            .kind = RenderSourceKind::NativeDetail,
            .hopSize = m_precomputedHopSize,
            .scaledToRequestedZoom = false,
        };
    }
    return m_renderSourceState;
}

int SpectrogramItem::centeredVisibleCoveragePercentForTests() const {
    QMutexLocker lock(&m_stateMutex);
    const qint64 referenceCols = referenceTrackColumnsAtLevelZeroLocked();
    if (referenceCols <= 0 || width() <= 0.0) {
        return 0;
    }
    const qint64 visibleColumns = static_cast<qint64>(
        std::ceil(width() / std::max(0.001, m_zoomLevel)));
    const qint64 covered = std::min(referenceCols, visibleColumns);
    return static_cast<int>((covered * 100) / referenceCols);
}

void SpectrogramItem::setZoomLevel(double value) {
    QMutexLocker lock(&m_stateMutex);
    const double minZoom = minimumZoomLevelLocked();
    value = std::clamp(value, minZoom, 16.0);
    if (std::abs(m_zoomLevel - value) < 0.0001) {
        return;
    }
    m_zoomLevel = value;
    m_requestedRenderZoomLevel = value;
    m_requestedNativeHopSize = expectedNativeHopForZoom(value);
    if (m_precomputedHopSize > 0 && m_precomputedHopSize != m_requestedNativeHopSize) {
        const bool canFreezePreview =
            m_displayMode == 1
            && m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft
            && (!m_gpuDisplayImage.isNull() || m_zoomFillFrozenRetainedAvailable);
        m_renderZoomLevel = value;
        m_precomputedCanvasDirty = true;
        invalidateMapping();
        m_renderSourceState = SpectrogramRenderSourceState{
            .kind = RenderSourceKind::Preview,
            .hopSize = m_precomputedHopSize,
            .scaledToRequestedZoom = true,
        };
        if (canFreezePreview) {
            m_zoomFillFrozenDisplayImage = QImage();
            m_zoomFillFrozenDisplayGeneration = 0;
            m_zoomFillFrozenRetainedActive = false;
            m_zoomFillActive = true;
            m_zoomFillDataReady = false;
            m_zoomFillFreezePending = true;
        }
    }

    if (m_zoomLevel < 0.05 && hasFullTrackOverviewLocked()) {
        m_renderZoomLevel = m_zoomLevel;
        m_pendingBackendZoom = 0.05F;
        m_awaitingZoomData = false;
        m_zoomFillFreezePending = false;
        m_renderSourceState = SpectrogramRenderSourceState{
            .kind = RenderSourceKind::Overview,
            .hopSize = m_precomputedHopSize,
            .scaledToRequestedZoom = true,
        };
        lock.unlock();
        emit zoomLevelChanged();
        update();
        return;
    }

    // All zoom changes go through the backend: zoom-in needs a finer hop,
    // zoom-out needs coarser decimation.  The visual update is deferred
    // until the backend restarts with matching data.
    m_pendingBackendZoom = static_cast<float>(m_zoomLevel);
    m_awaitingZoomData = true;
    if (m_zoomFillActive) {
        m_zoomFillDataReady = false;
    }
    m_zoomDebounceTimer->start(); // restarts the 150ms timer

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

void SpectrogramItem::feedPrecomputedChunk(
    const QByteArray &data, int bins, int channelIndex,
    int columns, int startIndex, int totalEstimate,
    int sampleRate, int hopSize, bool complete,
    bool bufferReset, quint64 trackToken, bool clearHistoryOnReset,
    quint64 generation) {
    using Clock = std::chrono::steady_clock;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    PrecomputedFeedPhaseProfile profile;
    const Clock::time_point lockAttemptAt = Clock::now();
#endif
    QMutexLocker lock(&m_stateMutex);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    profile.lockWaitMs =
        std::chrono::duration<double, std::milli>(Clock::now() - lockAttemptAt)
            .count();
    profile.phaseStartedAt = Clock::now();
    const bool profileFeedEnabled = m_profileEnabled;
    profile.resetPendingBefore = m_precomputedResetPending;
    profile.zoomFillActiveBefore = m_zoomFillActive;
    profile.frozenRetainedBefore = m_zoomFillFrozenRetainedAvailable;
    profile.gpuImageBefore = !m_gpuDisplayImage.isNull();
    profile.ringCapacityBefore = m_ringCapacity;
    profile.ringBytesBefore = m_ringBuffer.size();
    profile.displayMode = m_displayMode;
#endif

    FERROUS_SPECTROGRAM_TRACE_LOGF(stderr,
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

    const bool sameTrackSessionChunk =
        trackToken != 0
        && m_precomputedTrackToken != 0
        && trackToken == m_precomputedTrackToken;
    const bool sameTrackZoomHopMismatch =
        m_displayMode == 1
        && sameTrackSessionChunk
        && hopSize > 0
        && !effectiveHopMatchesZoom(hopSize, m_zoomLevel);
    const bool shouldDropStaleSameTrackZoomChunk =
        sameTrackZoomHopMismatch
        && (m_awaitingZoomData || std::abs(m_zoomLevel - 1.0) > 0.001);

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
            FERROUS_SPECTROGRAM_TRACE_LOGF(stderr,
                "[Qt-gate-open@%p] worker reset arrived, bins=%d tok=%llu\n",
                static_cast<const void *>(this), bins,
                static_cast<unsigned long long>(trackToken));
            m_awaitingWorkerReset = false;
            // Fall through to normal reset handling below.
        } else {
            FERROUS_SPECTROGRAM_TRACE_LOGF(stderr,
                "[Qt-gate-reject@%p] stale chunk rejected, cols=%d bins=%d reset=%d\n",
                static_cast<const void *>(this), columns, bins,
                bufferReset ? 1 : 0);
            return;
        }
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    profile.mark(profile.gateMs);
#endif

    // Early hop change detection: activate the frozen-image path BEFORE any
    // reset processing that might invalidate the render caches. Both finer
    // zoom-ins and coarser zoom-outs must keep the previous frame visible;
    // otherwise the centered view exposes the worker's partial refill and
    // appears to slide/speed up until the new window is fully decoded.
    if (columns > 0
        && hopSize > 0
        && hopSize != m_precomputedHopSize
        && m_displayMode == 1
        && m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft) {
        const bool shouldCaptureFreeze =
            shouldCaptureZoomFillFreezeForHopChange(
                m_zoomFillActive,
                !m_zoomFillFrozenDisplayImage.isNull(),
                m_zoomFillFrozenRetainedAvailable);
        if (shouldCaptureFreeze) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            const Clock::time_point freezeStartedAt = Clock::now();
#endif
            captureZoomFillFrozenCacheLocked(false);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            profile.freezeMs += std::chrono::duration<double, std::milli>(
                                    Clock::now() - freezeStartedAt)
                                    .count();
            profile.phaseStartedAt = Clock::now();
#endif
        }
        m_zoomFillFrozenRetainedActive =
            m_zoomFillFrozenRetainedAvailable;
        m_zoomFillActive =
            !m_zoomFillFrozenDisplayImage.isNull() || m_zoomFillFrozenRetainedActive;
        m_zoomFillDataReady = false;
    }

    if (shouldDropStaleSameTrackZoomChunk) {
        // Rapid zoom-out restarts share the same track token. Drop stale
        // chunks after freeze capture but before reset handling so older
        // sessions cannot clear/refill the ring after the final requested
        // zoom has already arrived.
        FERROUS_SPECTROGRAM_TRACE_LOGF(stderr,
            "[Qt-stale-zoom-chunk@%p] drop tok=%llu cols=%d start=%d total=%d hop=%d zoom=%.4f reset=%d\n",
            static_cast<const void *>(this),
            static_cast<unsigned long long>(trackToken),
            columns, startIndex, totalEstimate, hopSize, m_zoomLevel,
            bufferReset ? 1 : 0);
        update();
        return;
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
        const bool canPreserveCanvasForSeek =
            m_displayMode == 1
            && m_precomputedCanvasDisplayRight
                >= m_precomputedCanvasDisplayLeft
            && m_precomputedTotalColumnsEstimate > 0
            && m_precomputedBinsPerColumn > 0;
        const bool preserveCanvasForSeek =
            canPreserveCanvasForSeek
            && (!m_gpuDisplayImage.isNull() || m_zoomFillFrozenRetainedAvailable);

        m_ringWriteSeq = 0;
        m_ringOldestSeq = 0;
        m_ringCapacity = 0;
        m_ringBuffer.clear();
        m_ringColumnId.clear();
        m_ringSequenceId.clear();
        m_ringTrackToken.clear();
        m_trackColumnToSeqByToken.clear();
        m_precomputedMaxColumnIndex = -1;
        m_precomputedCanvasDirty = true;
        m_awaitingWorkerReset = true;
        invalidateRetainedSourceTextureUploadStateLocked(
            preserveCanvasForSeek && m_zoomFillFrozenRetainedAvailable);

        if (preserveCanvasForSeek) {
            // Canvas, m_precomputedReady, and the stored display range
            // all stay as-is so the paint keeps rendering the old
            // image through canvasFreeze.
            captureZoomFillFrozenCacheLocked(false);
            m_zoomFillActive = true;
            m_zoomFillDataReady = false;
            m_zoomFillFrozenRetainedActive =
                m_zoomFillFrozenRetainedAvailable;
        } else {
            m_precomputedReady = false;
            m_precomputedCanvasDisplayLeft = 0;
            m_precomputedCanvasDisplayRight = -1;
            invalidateRenderCachesLocked();
        }
        FERROUS_SPECTROGRAM_TRACE_LOGF(stderr,
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
        m_zoomFillDataReady = false;
        m_zoomFillFrozenRetainedActive = false;
        clearZoomFillFrozenCacheLocked();
        applyPrecomputedResetLocked(
            startIndex, bins, trackToken, generation, clearHistoryOnReset);
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
            if (hopSize == m_requestedNativeHopSize) {
                m_renderSourceState = SpectrogramRenderSourceState{
                    .kind = RenderSourceKind::NativeDetail,
                    .hopSize = hopSize,
                    .scaledToRequestedZoom = false,
                };
            } else if (m_renderSourceState.kind == RenderSourceKind::NativeDetail
                || m_renderSourceState.hopSize == 0) {
                m_renderSourceState = SpectrogramRenderSourceState{
                    .kind = RenderSourceKind::NativeDetail,
                    .hopSize = hopSize,
                    .scaledToRequestedZoom = false,
                };
            }
        }
        if (sampleRate > 0 && hopSize > 0) {
            const double seekPositionSeconds =
                static_cast<double>(startIndex * hopSize)
                / static_cast<double>(sampleRate);
            m_positionJumpHoldActive = false;
            setPositionAnchorLocked(seekPositionSeconds, Clock::now());
        }
        m_precomputedTrackToken = trackToken;
        m_precomputedLastRightCol = -1;
        m_precomputedLastDisplaySeq = -1;
        m_timeGridDirty = true;

        if (needsZoomReset) {
            m_zoomLevel = 1.0;
            m_renderZoomLevel = 1.0;
            m_awaitingZoomData = false;
            m_pendingBackendZoom = 1.0f;
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
        const bool canPreserveSameTrackFrame =
            clearHistoryOnReset
            && m_displayMode == 1
            && trackToken != 0
            && m_precomputedTrackToken != 0
            && trackToken == m_precomputedTrackToken
            && m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft
            && (!m_gpuDisplayImage.isNull() || m_zoomFillFrozenRetainedAvailable);
        if (canPreserveSameTrackFrame) {
            captureZoomFillFrozenCacheLocked(false);
            m_zoomFillFrozenRetainedActive =
                m_zoomFillFrozenRetainedAvailable;
            m_zoomFillActive =
                !m_zoomFillFrozenDisplayImage.isNull()
                || m_zoomFillFrozenRetainedActive;
            m_zoomFillDataReady = false;
        }
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
        FERROUS_SPECTROGRAM_TRACE_LOGF(stderr,
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
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    profile.mark(profile.resetMs);
    profile.appliedReset = appliedReset;
    profile.appliedImplicitReset = appliedImplicitReset;
#endif

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
            FERROUS_SPECTROGRAM_TRACE_LOGF(
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
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    profile.mark(profile.sessionMs);
#endif

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
            const bool resetMatchesRequestedZoom =
                effectiveHopMatchesZoom(m_precomputedHopSize, m_zoomLevel);
            // Don't consume the awaiting flag if the zoom debounce timer is
            // still running — a zoom change command hasn't been sent to the
            // backend yet.  This reset is from a different trigger (e.g.
            // widget width change) and carries data at the OLD zoom level.
            // Consuming the flag here would prevent the zoom snap from
            // firing when the actual zoom-change data arrives, leaving
            // renderZoomLevel stale and forcing full canvas rebuilds.
            //
            // Also ignore reset data whose effective hop does not match the
            // latest requested zoom.  Rapid wheel zoom-out can leave older
            // backend restarts in flight; revealing those intermediate hops
            // makes the view appear stuck before the final max-out target.
            if (!resetMatchesRequestedZoom) {
                m_precomputedCanvasDirty = true;
                update();
                return;
            }
            if (m_zoomDebounceTimer->isActive()) {
                m_zoomDebounceTimer->stop();
            }
            if (!m_zoomDebounceTimer->isActive()) {
                m_awaitingZoomData = false;
            }
            m_renderZoomLevel = m_zoomLevel;
            m_crosshairDirty = true;
        } else if ((appliedReset || appliedImplicitReset)
            && !isTrackChange
            && !isGaplessTrackChange
            && m_precomputedHopSize > 0
            && effectiveHopMatchesZoom(m_precomputedHopSize, m_zoomLevel)) {
            // Same-track resets are authoritative even if the explicit
            // awaiting flag was already consumed by a prior restart.  Rapid
            // zoom-out can produce a later coarse-hop reset after an earlier
            // intermediate reset has cleared m_awaitingZoomData; rendering
            // that coarse data with the stale 1.0 render zoom makes the view
            // look stuck at default zoom.
            if (std::abs(m_renderZoomLevel - m_zoomLevel) > 0.001) {
                m_renderZoomLevel = m_zoomLevel;
                m_precomputedCanvasDirty = true;
                m_crosshairDirty = true;
            }
        }
        if (appliedReset && m_precomputedSampleRateHz > 0 && m_precomputedHopSize > 0) {
            const double seekPositionSeconds =
                static_cast<double>(startIndex * m_precomputedHopSize)
                / static_cast<double>(m_precomputedSampleRateHz);
            if (m_displayMode == 0) {
                m_positionJumpHoldActive = false;
                setPositionAnchorLocked(seekPositionSeconds, Clock::now());
            } else if (isTrackChange) {
                // Release hold AND snap anchor to the new track's start
                // position.  Without the anchor snap, clearing the gapless
                // offset in applyPrecomputedResetLocked makes the next
                // setPositionSeconds see a 100+ second jump (old anchor
                // vs new track's ~0), which immediately re-activates
                // the hold.
                m_positionJumpHoldActive = false;
                setPositionAnchorLocked(seekPositionSeconds, Clock::now());
            }
        }
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    profile.mark(profile.metadataMs);
#endif

    // Dynamically size the ring buffer based on widget width and display mode.
    if (columns > 0 && bins > 0) {
        const double colsPerSecond =
            static_cast<double>(m_precomputedSampleRateHz)
            / static_cast<double>(m_precomputedHopSize);
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
        const int extraSeconds = 10;
        int neededCapacity;
        if (m_displayMode == 1) {
            // Centered: windowed — ~3 screen widths around playhead.
            neededCapacity = screenWidth * 3
                + static_cast<int>(extraSeconds * colsPerSecond);
        } else {
            // Rolling: need one visible viewport of history, the decoder
            // lookahead, and one extra viewport of slack so eviction never
            // intrudes into the live window when the decoder runs ahead.
            const double ez = effectiveZoomLocked();
            const int zoomAdjustedWidth = static_cast<int>(
                static_cast<double>(screenWidth) / std::max(0.05, ez));
            neededCapacity = zoomAdjustedWidth * 2
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
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        profile.neededCapacity = neededCapacity;
        profile.neededBytes = neededBytes;
        profile.mustRealloc = mustRealloc;
#endif

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
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    profile.mark(profile.allocMs);
#endif

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
                            }
                        }
                    }
                }
            }
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
            activeTokenIt->insert(trackCol, m_ringWriteSeq);
            if (trackCol > m_precomputedMaxColumnIndex) {
                m_precomputedMaxColumnIndex = trackCol;
            }
            m_ringWriteSeq++;
        }
        m_ringOldestSeq = std::max<qint64>(0, m_ringWriteSeq - m_ringCapacity);
        FERROUS_SPECTROGRAM_TRACE_LOGF(stderr,
            "[Qt-ring] tok=%llu writeSeq=%lld oldestSeq=%lld cap=%d valid=%lld maxCol=%d\n",
            static_cast<unsigned long long>(trackToken),
            static_cast<long long>(m_ringWriteSeq),
            static_cast<long long>(m_ringOldestSeq),
            m_ringCapacity,
            static_cast<long long>(m_ringWriteSeq - m_ringOldestSeq),
            static_cast<int>(m_precomputedMaxColumnIndex));
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    profile.mark(profile.writeMs);
#endif

    const bool wasReady = m_precomputedReady;
    m_precomputedReady =
        m_precomputedTotalColumnsEstimate > 0
        && m_precomputedBinsPerColumn > 0;

    m_precomputedLastRightCol = -1;

    if (m_precomputedCanvasDisplayRight < m_precomputedCanvasDisplayLeft) {
        // No canvas range yet (first data after reset) — force build.
        m_precomputedCanvasDirty = true;
    }

    // Canvas freeze readiness: mark decoded data ready when the ring
    // has enough data to cover the visible display. The freeze is
    // released later by updatePaintNode only when that frame can render
    // through the active path.
    if (m_zoomFillActive) {
        const int screenWidth = std::max(static_cast<int>(width()), 1);
        const double effectiveZoom = std::max(0.001, effectiveZoomLocked());
        const qint64 visibleWindowCols = std::max<qint64>(
            1,
            static_cast<qint64>(std::ceil(
                static_cast<double>(screenWidth) / effectiveZoom)));
        const qint64 maxDecodedCount = m_precomputedMaxColumnIndex >= 0
            ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
            : static_cast<qint64>(0);
        const qint64 estimateCount = std::max<qint64>(
            static_cast<qint64>(m_precomputedTotalColumnsEstimate),
            static_cast<qint64>(1));
        const qint64 estimatedTrackCols =
            std::max(maxDecodedCount, estimateCount);

        // Two conditions must be met:
        // 1. The ring has enough columns for the visible source span at
        //    the pending zoom. At sub-1.0 effective zoom this can be
        //    much wider than the widget in pixels.
        // 2. The newest decoded column covers the target display right
        //    edge, so the display won't reveal an unbacked region.
        // Both are capped at the track estimate for max zoom-out.
        const qint64 ringFill = m_ringWriteSeq - m_ringOldestSeq;
        const qint64 fillWidth =
            std::min(visibleWindowCols, estimatedTrackCols);
        const bool ringHasEnoughColumns = ringFill + 16 >= fillWidth;

        const double cps = m_precomputedHopSize > 0
            ? static_cast<double>(m_precomputedSampleRateHz)
                / static_cast<double>(m_precomputedHopSize)
            : 1.0;
        const qint64 nowCol = static_cast<qint64>(
            std::floor(std::max(0.0, m_positionSeconds) * cps));
        qint64 displayLeftCol = 0;
        qint64 displayRightCol;
        const qint64 halfWindowCols = visibleWindowCols / 2;
        if (m_displayMode == 1) {
            if (visibleWindowCols * 100 / estimatedTrackCols >= 90) {
                displayLeftCol = 0;
                displayRightCol = estimatedTrackCols - 1;
            } else {
                displayLeftCol = std::max<qint64>(
                    0, nowCol - halfWindowCols);
                displayRightCol = std::min(
                    estimatedTrackCols - 1,
                    displayLeftCol + visibleWindowCols - 1);
                displayLeftCol = std::max<qint64>(
                    0, displayRightCol - visibleWindowCols + 1);
            }
        } else {
            displayRightCol = nowCol + halfWindowCols;
            if (m_precomputedTotalColumnsEstimate > 0) {
                displayRightCol = std::min(
                    displayRightCol,
                    static_cast<qint64>(m_precomputedTotalColumnsEstimate) - 1);
            }
            displayLeftCol = std::max<qint64>(0, displayRightCol - visibleWindowCols + 1);
        }
        const bool ringCoversDisplayRight =
            static_cast<qint64>(m_precomputedMaxColumnIndex) + 1 + 16
            >= displayRightCol + 1;

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

        const bool fillMatchesRequestedZoom =
            !m_awaitingZoomData
            || effectiveHopMatchesZoom(m_precomputedHopSize, m_zoomLevel);
        const qint64 decodedVisibleRight = std::min(
            displayRightCol,
            static_cast<qint64>(m_precomputedMaxColumnIndex));
        const qint64 firstMissingVisibleColumn =
            m_displayMode == 1 && decodedVisibleRight >= displayLeftCol
                ? firstMissingCenteredDisplayColumnLocked(
                    displayLeftCol, decodedVisibleRight)
                : static_cast<qint64>(-1);
        const bool centeredVisibleColumnsCovered =
            m_displayMode != 1
            || (decodedVisibleRight >= displayLeftCol
                && firstMissingVisibleColumn < 0);
        const bool zoomFillReadyForData =
            fillMatchesRequestedZoom
            && centeredVisibleColumnsCovered
            && ((ringHasEnoughColumns && ringCoversDisplayRight) || decodeReachedEnd);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        const bool shouldLogZoomFillGate =
            !m_zoomFillDataReady
            && (zoomFillReadyForData
                || (!centeredVisibleColumnsCovered
                    && (complete || decodeReachedEnd || ringHasEnoughColumns)));
        if (shouldLogZoomFillGate) {
            FERROUS_SPECTROGRAM_PROFILE_LOGF(stderr,
                "[ui-spectrogram] zoom_fill_gate ready=%d mode=%d zoom=%.4f render_zoom=%.4f "
                "hop=%d match=%d display=%lld..%lld decoded_right=%lld missing=%lld "
                "ring_fill=%lld fill_width=%lld max_col=%d estimate=%d enough=%d covers_right=%d "
                "decode_end=%d await=%d complete=%d token=%llu gen=%llu\n",
                zoomFillReadyForData ? 1 : 0,
                m_displayMode,
                m_zoomLevel,
                m_renderZoomLevel,
                m_precomputedHopSize,
                fillMatchesRequestedZoom ? 1 : 0,
                static_cast<long long>(displayLeftCol),
                static_cast<long long>(displayRightCol),
                static_cast<long long>(decodedVisibleRight),
                static_cast<long long>(firstMissingVisibleColumn),
                static_cast<long long>(ringFill),
                static_cast<long long>(fillWidth),
                static_cast<int>(m_precomputedMaxColumnIndex),
                m_precomputedTotalColumnsEstimate,
                ringHasEnoughColumns ? 1 : 0,
                ringCoversDisplayRight ? 1 : 0,
                decodeReachedEnd ? 1 : 0,
                m_awaitingZoomData ? 1 : 0,
                complete ? 1 : 0,
                static_cast<unsigned long long>(m_precomputedTrackToken),
                static_cast<unsigned long long>(generation));
        }
#endif
        if (zoomFillReadyForData) {
            // The decoder has enough data, but the final render resource may
            // not be ready yet. Keep showing the frozen frame until
            // updatePaintNode can reveal the current frame through its normal
            // render path (retained source on OpenGL, display image otherwise).
            m_zoomFillDataReady = true;
            m_precomputedCanvasDirty = true;
        }
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    profile.mark(profile.readyMs);
#endif

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
        m_zoomFillDataReady = false;
        m_zoomFillFrozenRetainedActive = false;
        clearZoomFillFrozenCacheLocked();
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
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    profile.mark(profile.zoomResetMs);
    profile.resetPendingAfter = m_precomputedResetPending;
    profile.zoomFillActiveAfter = m_zoomFillActive;
    profile.frozenRetainedAfter = m_zoomFillFrozenRetainedAvailable;
    profile.gpuImageAfter = !m_gpuDisplayImage.isNull();
    profile.ringCapacityAfter = m_ringCapacity;
    profile.ringBytesAfter = m_ringBuffer.size();
    profile.ringFillAfter =
        std::max<qint64>(0, m_ringWriteSeq - m_ringOldestSeq);
    profile.maxColumnAfter = m_precomputedMaxColumnIndex;
#endif

    const bool readyChanged = m_precomputedReady && !wasReady;

    // Unlock before emitting signals — the QML handlers may call
    // setZoomLevel which re-acquires the mutex.
    lock.unlock();

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    profile.phaseStartedAt = Clock::now();
#endif
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
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    profile.mark(profile.emitUpdateMs);
    const double profileTotalMs = profile.totalMs();
    if (profileFeedEnabled
        && profileTotalMs >= kPrecomputedFeedPhaseLogThresholdMs) {
        FERROUS_SPECTROGRAM_PROFILE_LOGF(stderr,
            "[ui-spectrogram] precomputed_feed_phase total_ms=%.2f "
            "lock_wait_ms=%.2f gate_ms=%.2f freeze_ms=%.2f reset_ms=%.2f "
            "session_ms=%.2f metadata_ms=%.2f alloc_ms=%.2f write_ms=%.2f "
            "ready_ms=%.2f zoom_reset_ms=%.2f emit_update_ms=%.2f "
            "cols=%d bins=%d start=%d total=%d hop=%d reset=%d clear=%d "
            "complete=%d mode=%d pending_before=%d pending_after=%d "
            "applied_reset=%d implicit_reset=%d zoom_fill_before=%d "
            "zoom_fill_after=%d retained_before=%d retained_after=%d "
            "gpu_image_before=%d gpu_image_after=%d ring_cap_before=%d "
            "ring_cap_after=%d ring_kb_before=%.1f ring_kb_after=%.1f "
            "needed_cap=%d needed_kb=%.1f realloc=%d ring_fill=%lld "
            "max_col=%d token=%llu gen=%llu\n",
            profileTotalMs,
            profile.lockWaitMs,
            profile.gateMs,
            profile.freezeMs,
            profile.resetMs,
            profile.sessionMs,
            profile.metadataMs,
            profile.allocMs,
            profile.writeMs,
            profile.readyMs,
            profile.zoomResetMs,
            profile.emitUpdateMs,
            columns,
            bins,
            startIndex,
            totalEstimate,
            hopSize,
            bufferReset ? 1 : 0,
            clearHistoryOnReset ? 1 : 0,
            complete ? 1 : 0,
            profile.displayMode,
            profile.resetPendingBefore ? 1 : 0,
            profile.resetPendingAfter ? 1 : 0,
            profile.appliedReset ? 1 : 0,
            profile.appliedImplicitReset ? 1 : 0,
            profile.zoomFillActiveBefore ? 1 : 0,
            profile.zoomFillActiveAfter ? 1 : 0,
            profile.frozenRetainedBefore ? 1 : 0,
            profile.frozenRetainedAfter ? 1 : 0,
            profile.gpuImageBefore ? 1 : 0,
            profile.gpuImageAfter ? 1 : 0,
            profile.ringCapacityBefore,
            profile.ringCapacityAfter,
            static_cast<double>(profile.ringBytesBefore) / 1024.0,
            static_cast<double>(profile.ringBytesAfter) / 1024.0,
            profile.neededCapacity,
            static_cast<double>(profile.neededBytes) / 1024.0,
            profile.mustRealloc ? 1 : 0,
            static_cast<long long>(profile.ringFillAfter),
            profile.maxColumnAfter,
            static_cast<unsigned long long>(trackToken),
            static_cast<unsigned long long>(generation));
    }
#endif
}

void SpectrogramItem::feedPrecomputedLodChunk(
    const QByteArray &data,
    int bins,
    int channelIndex,
    int columns,
    int startIndex,
    int totalEstimate,
    int sampleRate,
    int hopSize,
    quint64 trackToken,
    quint64 generation,
    int lodLevel,
    int lodSourceSpan,
    int lodPlaneCount,
    bool complete) {
    Q_UNUSED(channelIndex);
    Q_UNUSED(totalEstimate);
    Q_UNUSED(sampleRate);
    Q_UNUSED(hopSize);
    Q_UNUSED(complete);
    QMutexLocker lock(&m_stateMutex);
    if (lodLevel <= 0 || columns <= 0 || bins <= 0 || lodPlaneCount <= 0) {
        return;
    }
    if (m_precomputedTrackToken != 0 && trackToken != m_precomputedTrackToken) {
        return;
    }
    if (m_precomputedSessionGeneration != 0 && generation < m_precomputedSessionGeneration) {
        return;
    }
    m_spectrogramLodPages.push_back(SpectrogramLodPage{
        .trackToken = trackToken,
        .generation = generation,
        .level = lodLevel,
        .binsPerColumn = bins,
        .planeCount = lodPlaneCount,
        .sourceSpan = std::max(1, lodSourceSpan),
        .startColumn = startIndex,
        .endColumn = static_cast<qint64>(startIndex) + columns - 1,
        .bytes = data,
    });
}

void SpectrogramItem::clearPrecomputed() {
    FERROUS_SPECTROGRAM_TRACE_LOGF(stderr, "[Qt-clearPrecomputed] ringCap=%d ready=%d\n",
        m_ringCapacity, m_precomputedReady ? 1 : 0);
    QMutexLocker lock(&m_stateMutex);
    m_ringBuffer.clear();
    m_ringColumnId.clear();
    m_ringSequenceId.clear();
    m_ringTrackToken.clear();
    m_trackColumnToSeqByToken.clear();
    m_spectrogramLodPages.clear();
    m_ringCapacity = 0;
    m_ringOldestSeq = 0;
    m_ringWriteSeq = 0;
    m_trackEpochSeq = 0;
    m_rollingEpoch = 0;
    m_precomputedMaxColumnIndex = -1;
    m_precomputedCommittedToken = 0;
    m_precomputedSessionGeneration = 0;
    m_precomputedCommittedGeneration = 0;
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
    m_precomputedLastRightCol = -1;
    m_precomputedLastDisplaySeq = -1;
    m_precomputedTrackToken = 0;
    m_requestedRenderZoomLevel = m_zoomLevel;
    m_requestedNativeHopSize = expectedNativeHopForZoom(m_zoomLevel);
    m_renderSourceState = SpectrogramRenderSourceState{};
    m_awaitingZoomData = false;
    m_zoomFillActive = false;
    m_zoomFillDataReady = false;
    m_zoomFillFrozenRetainedActive = false;
    clearZoomFillFrozenCacheLocked();
    m_renderZoomLevel = m_zoomLevel;
    m_gaplessPositionOffset = 0.0;
    m_positionJumpHoldActive = false;
    m_centeredGaplessTransitionAt = {};
    m_timeGridDirty = true;
    const bool wasReady = m_precomputedReady;
    m_precomputedReady = false;
    invalidateRenderCachesLocked();
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
    }
    // A reset (seek or manual track change) breaks the continuous
    // gapless coordinate space — clear the accumulated offset,
    // per-track column tracking, and canvas display range so the
    // centered-mode monotonic clamp doesn't stick at old positions.
    m_gaplessPositionOffset = 0.0;
    m_centeredGaplessTransitionAt = {};
    m_precomputedMaxColumnIndex = -1;
    if (!m_zoomFillActive) {
        m_precomputedCanvasDisplayLeft = 0;
        m_precomputedCanvasDisplayRight = -1;
    }
    const bool preserveRollingHistory =
        !clearHistoryOnReset &&
        m_displayMode == 0
        && m_ringCapacity > 0
        && m_precomputedBinsPerColumn > 0
        && bins == m_precomputedBinsPerColumn;
    if (!preserveRollingHistory) {
        const bool preserveZoomFillCache =
            m_zoomFillActive
            && (!m_gpuDisplayImage.isNull() || m_zoomFillFrozenRetainedAvailable);
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
        m_ringBuffer.clear();
        m_ringColumnId.clear();
        m_ringSequenceId.clear();
        m_ringTrackToken.clear();
        m_trackColumnToSeqByToken.clear();
        m_spectrogramLodPages.clear();
        m_ringCapacity = 0;
        invalidateRetainedSourceTextureUploadStateLocked(
            preserveZoomFillCache && m_zoomFillFrozenRetainedAvailable);
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
    if (m_zoomFillActive
        && (!m_gpuDisplayImage.isNull() || m_zoomFillFrozenRetainedAvailable)) {
        // Keep the last GPU cache alive while the ring refills. The
        // next paint freezes against this cached image until the new
        // session covers the visible display.
        m_zoomFillFrozenRetainedActive = m_zoomFillFrozenRetainedAvailable;
    } else {
        invalidateRenderCachesLocked();
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
    m_animationTickInitialized = false;

    // When precomputed mode is active, don't destroy the cached render
    // images or mapping — they will be refreshed from the ring on the
    // next frame. Destroying them causes a visible gap/flash.
    if (!m_precomputedReady) {
        m_binsPerColumn = 0;
        invalidateMapping();
        invalidateRenderCachesLocked();
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

QSGNode *SpectrogramItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const auto paintStart = std::chrono::steady_clock::now();
#endif
    const qint64 paintNowMs = QDateTime::currentMSecsSinceEpoch();
    const auto renderNow = std::chrono::steady_clock::now();
    auto *node = static_cast<SpectrogramSceneNode *>(oldNode);
    QQuickWindow *currentWindow = window();
    const quintptr windowId = reinterpret_cast<quintptr>(currentWindow);
    bool nodeRecreated = false;
    if (node != nullptr
        && (node->ownerWindowId != windowId || node->generation != m_sceneGraphGeneration)) {
        delete node;
        node = nullptr;
    }
    if (node == nullptr) {
        node = new SpectrogramSceneNode();
        node->ownerWindowId = windowId;
        node->generation = m_sceneGraphGeneration;
        nodeRecreated = true;
    }
    if (node->placeholderTexture == nullptr && currentWindow != nullptr) {
        node->placeholderTexture = currentWindow->createTextureFromImage(placeholderImage());
        if (node->placeholderTexture != nullptr) {
            node->placeholderTexture->setFiltering(QSGTexture::Nearest);
        }
    }

    const int w = std::max(1, static_cast<int>(std::floor(width())));
    const int h = std::max(1, static_cast<int>(std::floor(height())));
    const SpectrogramGpu::DebugMode gpuDebugMode = SpectrogramGpu::currentDebugMode();
    node->background->setRect(0.0, 0.0, static_cast<double>(w), static_cast<double>(h));
    node->background->setColor(kBackgroundColor);
    // Force the opaque underlay to repaint on every sync. Transparent overlay
    // textures and the moving playhead rely on a fresh background in retained
    // scene-graph backends; otherwise old pixels can survive as smear trails.
    node->background->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);

    double drawX = 0.0;
    int drawPixels = 0;
    bool showSpectrogram = false;
    bool spectrogramTextureChanged = false;
    quint64 spectrogramImageGeneration = 0;
    SpectrogramGpu::DebugMode spectrogramImageDebugMode = gpuDebugMode;
    QRectF spectrogramTargetRect;
    QRect spectrogramSourceRect;
    QImage spectrogramNodeImage;
    QSGTexture *retainedSourceTexture{nullptr};
    QVector<RetainedRenderSegment> retainedTextureSegments;
    QVector<RetainedBodyDrawSegment> retainedBodyDrawSegments;
    SpectrogramLodRenderState lodRenderState;
    bool useRetainedSourceTextureBodyForFrame = false;
    QImage spectrogramDbRemapImage;
    QImage spectrogramPaletteImage;
    QImage spectrogramRowLutImage;
    bool spectrogramRollingIncrementalScroll = false;
    int spectrogramRollingShiftPixels = 0;
    double spectrogramSubpixelOffset = 0.0;
    quint64 spectrogramDbRemapGeneration = 0;
    quint64 spectrogramPaletteGeneration = 0;
    quint64 spectrogramRowLutGeneration = 0;
    bool showPlayhead = false;
    QRectF playheadRect;
    QVector<QLineF> freqGridLines;
    QVector<QLineF> timeGridLines;
    std::optional<QLineF> playheadLine;
    std::optional<QLineF> crosshairVerticalLine;
    std::optional<QLineF> crosshairHorizontalLine;
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
    double profileCacheSyncMs = 0.0;
    double profileDisplayImageBuildMs = 0.0;
    double profileRetainedUploadMs = 0.0;
    double profileOverlayBuildMs = 0.0;
    double profileSpectrogramTextureUploadMs = 0.0;
    double profileLodTextureUploadMs = 0.0;
    double profileOverlayTextureUploadMs = 0.0;
    const char *profilePaintPath = "empty";
    const char *profileFallbackReason = "none";
    const char *profileLodMissingReason = "native";
    const char *profileRetainedResetReason = "none";
    bool profileCpuDisplayImageRebuilt = false;
    bool profileRetainedTextureReset = false;
    int profileRetainedUploadSpanCount = 0;
    int profileRetainedUploadColumns = 0;
    bool profileUsedFullSpectrogramUpload = false;
    bool profileUsedTailSpectrogramUpload = false;
    bool profileLodAvailable = false;
    int profileLodPrimaryLevel = 0;
    int profileLodSecondaryLevel = 0;
    float profileLodBlend = 0.0F;
    int profileTailUploadWidth = 0;
    bool profileOverlayTextureUploaded = false;
    bool profileFreqGridTextureUploaded = false;
    bool profileTimeGridTextureUploaded = false;
    bool profileCrosshairTextureUploaded = false;
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
                FERROUS_SPECTROGRAM_TRACE_LOGF(
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
                FERROUS_SPECTROGRAM_TRACE_LOGF(
                    stderr,
                    "[Qt-paint@%p] usePre=%d ready=%d bins=%d totalEst=%d pos=%.2f sr=%d hop=%d ringValid=%lld/%d\n",
                    static_cast<const void *>(this),
                    usePrecomputed ? 1 : 0,
                    m_precomputedReady ? 1 : 0,
                    m_precomputedBinsPerColumn,
                    m_precomputedTotalColumnsEstimate,
                    renderPositionSeconds,
                    m_precomputedSampleRateHz,
                    m_precomputedHopSize,
                    static_cast<long long>(validCount),
                    m_ringCapacity);
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
            bool centeredViewportScrollsWithPlayback = false;
            qint64 writeHeadSeq = -1;
            bool centeredDecodedTailLooksFinalForFrame = false;
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
                // Use maxColumnIndex (actual decoded extent) when it covers
                // the playhead area — this gives accurate EOF clamping so the
                // playhead detaches from center at track end.  Fall back to
                // totalColumnsEstimate only when the playhead is ahead of
                // decoded data (during the initial fill after a far seek).
                // Compute display range centered on the playhead, then
                // clamp to actual content at the right edge.
                //
                // Use the estimate for the initial range so it stays
                // stable during fill (no race-in, no sliding).  Then
                // apply maxColumnIndex as a RIGHT-EDGE-ONLY clamp for
                // EOF: this makes the playhead detach from center at
                // track end without affecting the display during fill.
                const qint64 maxColCount = m_precomputedMaxColumnIndex >= 0
                    ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
                    : static_cast<qint64>(0);
                const qint64 estimateCount = std::max(
                    static_cast<qint64>(m_precomputedTotalColumnsEstimate),
                    static_cast<qint64>(1));

                // Step 1: compute range using the estimate (always stable).
                const qint64 estTotalCols = std::max(maxColCount, estimateCount);

                // When the visible window covers most of the track, lock
                // the display to the full extent.  The 90% threshold
                // accounts for decimation rounding that can leave a few
                // extra columns beyond the widget width at max zoom-out.
                // Without the lock, centering on the playhead causes the
                // spectrogram to shift on seek and jitter during playback.
                if (estTotalCols <= 0
                    || static_cast<qint64>(visibleWindowCols) * 100
                           / estTotalCols >= 90) {
                    displayLeft = 0;
                    displayRight = estTotalCols - 1;
                } else {
                    displayLeft = std::max(static_cast<qint64>(0),
                        static_cast<qint64>(nowCol) - halfWindowCols);
                    displayRight = std::min(
                        estTotalCols - 1,
                        displayLeft + static_cast<qint64>(visibleWindowCols) - 1);
                    displayLeft = std::max<qint64>(
                        0, displayRight - static_cast<qint64>(visibleWindowCols) + 1);
                }

                // The file-metadata estimate can overshoot the actual
                // audible/decoded extent by seconds (e.g. trailing
                // silence counted by container duration but trimmed
                // during playback).  A fixed 128-column tolerance fails
                // at high zoom where 128 cols is ~0.2 s and the gap is
                // typically ~1 s. Use maxCol as the hard right edge only
                // when the decoded tail is plausibly at EOF. During rapid far
                // seeks the decoded tail can lag far behind the optimistic
                // playhead; clamping in that state makes the viewport race
                // forward as decode catches up and can map right-edge clicks
                // backward. The EOF clamp still drives playhead detachment and
                // keeps the time axis bounded by audible content.
                const bool decodedTailLooksFinal =
                    centeredDecodedTailLooksFinal(
                        maxColCount, estimateCount, visibleWindowCols);
                centeredDecodedTailLooksFinalForFrame = decodedTailLooksFinal;
                if (decodedTailLooksFinal
                    && static_cast<qint64>(maxColCount) - 1
                        < static_cast<qint64>(nowCol)
                            + static_cast<qint64>(halfWindowCols)) {
                    displayRight = std::min(displayRight, maxColCount - 1);
                    displayLeft = std::max<qint64>(
                        0, displayRight - static_cast<qint64>(visibleWindowCols) + 1);
                }

                // Jitter prevention: only apply when zoom hasn't changed
                const bool isSeekJump =
                    m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft
                    && displayLeft < m_precomputedCanvasDisplayLeft - 2;
                const bool canvasMatchesBackendScale =
                    m_precomputedCanvasHopSize == m_precomputedHopSize
                    && m_precomputedCanvasTotalColumnsEstimate
                        == m_precomputedTotalColumnsEstimate
                    && m_precomputedCanvasDisplayRight < estTotalCols;
                if (!isSeekJump
                    && m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft
                    && std::abs(effectiveZoom - m_precomputedCanvasZoomLevel) < 0.001
                    && canvasMatchesBackendScale) {
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
                FERROUS_SPECTROGRAM_TRACE_LOGF(
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

            const bool haveFreezeCache =
                !m_zoomFillFrozenDisplayImage.isNull()
                || m_zoomFillFrozenRetainedActive;
            bool rendererFreeze =
                m_zoomFillActive
                && haveFreezeCache
                && m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft;

            if (rollingMode) {
                drawPixels = w;
                drawX = -columnPhase * effectiveZoom;
            } else {
                const qint64 decodedRightEdge =
                    m_precomputedMaxColumnIndex >= 0
                        ? static_cast<qint64>(m_precomputedMaxColumnIndex)
                        : static_cast<qint64>(-1);
                const qint64 totalColsForScroll = std::max(
                    m_precomputedMaxColumnIndex >= 0
                        ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
                        : static_cast<qint64>(0),
                    std::max(static_cast<qint64>(m_precomputedTotalColumnsEstimate),
                             static_cast<qint64>(1)));
                const qint64 displaySpan = displayRight - displayLeft + 1;
                const bool nearFullTrack =
                    totalColsForScroll > 0
                    && displaySpan * 100 / totalColsForScroll >= 90;
                const bool eofClampedToDecodedContent =
                    centeredDecodedTailLooksFinalForFrame
                    && decodedRightEdge >= 0
                    && displayRight >= decodedRightEdge;
                const bool centeredScrolling =
                    !nearFullTrack
                    && displayLeft > 0
                    && !eofClampedToDecodedContent
                    && displayRight < totalColsForScroll - 1;
                centeredViewportScrollsWithPlayback = centeredScrolling;
                drawX = centeredScrolling ? -columnPhase * effectiveZoom : 0.0;
                drawPixels = gpuDrawWidthForSourceColumnsLocked(
                    w, displayLeft, displayRight, effectiveZoom);
            }

            const bool debugModeRequestsRetainedSourceTextureBody =
                gpuDebugMode == SpectrogramGpu::DebugMode::TextureNode
                || gpuDebugMode == SpectrogramGpu::DebugMode::ShaderSourceOnly
                || gpuDebugMode == SpectrogramGpu::DebugMode::Full;
            const bool retainedSourceTextureFitsGpuLimits =
                m_precomputedBinsPerColumn > 0
                && m_precomputedBinsPerColumn <= gpuMaxTextureColumns(currentWindow)
                && retainedSourcePageWidthForGpuLimit(currentWindow) > 0;
            const bool canUseRetainedSourceTextureBody =
                debugModeRequestsRetainedSourceTextureBody
                && currentWindow != nullptr
                && currentWindow->rendererInterface() != nullptr
                && currentWindow->rendererInterface()->graphicsApi()
                    == QSGRendererInterface::OpenGL
                && retainedSourceTextureFitsGpuLimits;
            const bool maintainCpuDisplayImage =
                currentWindow == nullptr
                || !canUseRetainedSourceTextureBody;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            if (currentWindow == nullptr) {
                profileFallbackReason = "no-window";
            } else if (!debugModeRequestsRetainedSourceTextureBody) {
                profileFallbackReason = "debug-cpu";
            } else if (!retainedSourceTextureFitsGpuLimits) {
                profileFallbackReason = "texture-limit";
            } else if (!canUseRetainedSourceTextureBody) {
                profileFallbackReason = "unsupported-api";
            }
#endif
            const bool retainedFreezeAvailableForFrame =
                rendererFreeze
                && canUseRetainedSourceTextureBody
                && m_zoomFillFrozenRetainedActive;
            if (!rollingMode && gpuDebugMode == SpectrogramGpu::DebugMode::Full) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                const auto lodUploadStart = std::chrono::steady_clock::now();
#endif
                lodRenderState = syncSpectrogramLodTextureLocked(
                    currentWindow,
                    displayLeft,
                    displayRight,
                    effectiveZoom);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                profileLodTextureUploadMs =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - lodUploadStart)
                        .count();
                profileLodAvailable =
                    lodRenderState.texture != nullptr && lodRenderState.primaryLevel > 0;
                profileLodPrimaryLevel = lodRenderState.primaryLevel;
                profileLodSecondaryLevel = lodRenderState.secondaryLevel;
                profileLodBlend = lodRenderState.blend;
                if (profileLodAvailable) {
                    profileLodMissingReason = "none";
                } else if (lodRenderState.primaryLevel > 0) {
                    profileLodMissingReason = "missing-page";
                } else {
                    profileLodMissingReason = "native";
                }
#endif
            } else {
                invalidateSpectrogramLodTextureStateLocked();
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                profileLodMissingReason = rollingMode ? "rolling" : "debug-mode";
#endif
            }
            const qint64 retainedRenderableRight = rollingMode
                ? displayRight
                : std::min(displayRight, static_cast<qint64>(m_precomputedMaxColumnIndex));
            const int retainedPageWidth = retainedSourcePageWidthForGpuLimit(currentWindow);
            const bool retainedPagesCoverBeforeUpload =
                canUseRetainedSourceTextureBody
                && retainedSourcePagesCoverDisplayRangeLocked(
                    displayLeft,
                    displayRight,
                    retainedRenderableRight,
                    rollingMode,
                    retainedPageWidth,
                    true);
            const qint64 seekStartedAtMs = SpectrogramSeekTrace::startedAtMs();
            const qint64 seekAgeMs = seekStartedAtMs > 0 ? paintNowMs - seekStartedAtMs : -1;
            const bool deferTinyRetainedUploadDuringSeek =
                shouldDeferTinyRetainedUploadDuringSeek(
                    visibleCols,
                    rendererFreeze,
                    retainedPagesCoverBeforeUpload,
                    seekAgeMs);
            const bool shouldSyncRetainedSourceTexture =
                canUseRetainedSourceTextureBody
                && (!retainedFreezeAvailableForFrame || m_zoomFillDataReady)
                && !deferTinyRetainedUploadDuringSeek;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            if (deferTinyRetainedUploadDuringSeek) {
                profileFallbackReason = "seek-tiny-retained-defer";
            }
#endif

            if (visibleCols > 0) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                const auto syncStart = std::chrono::steady_clock::now();
                const auto displayStart = syncStart;
                const quint64 displayGenerationBefore = m_gpuDisplayGeneration;
#endif
                syncPrecomputedDisplayImageLocked(
                    drawPixels,
                    h,
                    displayLeft,
                    displayRight,
                    rollingMode,
                    effectiveZoom,
                    maintainCpuDisplayImage);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                profileDisplayImageBuildMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - displayStart).count();
                profileCpuDisplayImageRebuilt =
                    profileCpuDisplayImageRebuilt
                    || (m_gpuDisplayGeneration != displayGenerationBefore
                        && !m_gpuDisplayImage.isNull());
#endif
                if (shouldSyncRetainedSourceTexture) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    const auto retainedUploadStart = std::chrono::steady_clock::now();
                    const bool retainedHadTexture = m_gpuSourceTexture.texture() != nullptr;
#endif
                    syncRetainedSourceTextureLocked(
                        currentWindow,
                        displayLeft,
                        displayRight,
                        rollingMode);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    profileRetainedUploadMs += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - retainedUploadStart).count();
                    profileRetainedTextureReset =
                        profileRetainedTextureReset
                        || (!retainedHadTexture && m_gpuSourceTexture.texture() != nullptr)
                        || m_gpuSourcePageTextureReset;
                    profileRetainedUploadSpanCount += static_cast<int>(m_gpuSourceUploadSpans.size());
                    for (const SpectrogramSourceUploadSpan &span : m_gpuSourceUploadSpans) {
                        profileRetainedUploadColumns += std::max(0, span.xEnd - span.xBegin + 1);
                    }
                    profileRetainedUploadSpanCount += m_gpuSourcePageUploadSpanCount;
                    profileRetainedUploadColumns += m_gpuSourcePageUploadColumns;
                    if (m_gpuSourcePageResetReason != RetainedPageResetReason::None) {
                        profileRetainedResetReason =
                            retainedPageResetReasonName(m_gpuSourcePageResetReason);
                    }
#endif
                }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                profileCacheSyncMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - syncStart).count();
#endif
                m_precomputedLastRightCol = nowCol;
                m_precomputedLastDisplaySeq = rollingMode
                    ? displayRight
                    : static_cast<qint64>(nowCol);
            }

            bool haveRetainedTextureSegments = false;
            bool retainedTextureSegmentsOverflowed = false;
            quint64 retainedSourcePageGeneration = m_precomputedSessionGeneration;
            const auto rebuildRetainedDrawSegments = [&]() {
                retainedBodyDrawSegments.clear();
                bool allSegmentsHaveTextures = !retainedTextureSegments.isEmpty();
                const int pageWidth = retainedSourcePageWidthForGpuLimit(currentWindow);
                for (const RetainedRenderSegment &segment : retainedTextureSegments) {
                    if (!segment.valid || segment.sourceRect.isEmpty()) {
                        allSegmentsHaveTextures = false;
                        continue;
                    }
                    const qint64 pageIndex = pageWidth > 0
                        ? segment.sourceRect.x() / pageWidth
                        : 0;
                    const int pageX = pageWidth > 0
                        ? segment.sourceRect.x() % pageWidth
                        : segment.sourceRect.x();
                    const RetainedSourcePageState *page = findRetainedSourcePageLocked(
                        m_precomputedTrackToken,
                        retainedSourcePageGeneration,
                        rollingMode,
                        pageIndex);
                    QSGTexture *pageTexture =
                        page != nullptr ? page->texture.texture() : nullptr;
                    if (pageTexture == nullptr
                        || pageX < page->pageXOrigin
                        || pageX + segment.sourceRect.width()
                            > page->pageXOrigin + page->width) {
                        allSegmentsHaveTextures = false;
                        continue;
                    }
                    retainedBodyDrawSegments.push_back(RetainedBodyDrawSegment{
                        .targetRect = segment.targetRect,
                        .sourceRect = QRect(
                            pageX - page->pageXOrigin,
                            segment.sourceRect.y(),
                            segment.sourceRect.width(),
                            segment.sourceRect.height()),
                        .texture = pageTexture,
                    });
                }
                return allSegmentsHaveTextures
                    && retainedBodyDrawSegments.size() == retainedTextureSegments.size();
            };
            const auto retainedScrollColumnsPerSecond = [this]() {
                return m_precomputedSampleRateHz > 0 && m_precomputedHopSize > 0
                    ? static_cast<double>(m_precomputedSampleRateHz)
                        / static_cast<double>(m_precomputedHopSize)
                    : 0.0;
            };
            const auto anchorCachedRetainedSourceForCurrentFrame = [&]() {
                m_zoomFillFrozenRetainedPositionSeconds = renderPositionSeconds;
                m_zoomFillFrozenRetainedColumnsPerSecond =
                    retainedScrollColumnsPerSecond();
                m_zoomFillFrozenRetainedPixelsPerSecond =
                    m_zoomFillFrozenRetainedColumnsPerSecond
                    * std::max(0.001, effectiveZoom);
                m_zoomFillFrozenRetainedAnchorX =
                    playheadPixel >= 0
                        ? static_cast<double>(
                              std::clamp(playheadPixel, 0, std::max(0, w - 1)))
                        : (width() > 0.0 ? width() * 0.5 : 0.0);
                m_zoomFillFrozenRetainedLastColumnDelta = 0.0;
                m_zoomFillFrozenRetainedGeneration = m_precomputedSessionGeneration;
            };
            const auto useCachedRetainedSourceSegments = [&]() {
                retainedSourceTexture = m_gpuSourceTexture.texture();
                retainedSourcePageGeneration = m_zoomFillFrozenRetainedGeneration;
                const double requestedSourceColumnDelta =
                    m_zoomFillFrozenRetainedColumnsPerSecond > 0.0
                        ? (renderPositionSeconds
                              - m_zoomFillFrozenRetainedPositionSeconds)
                            * m_zoomFillFrozenRetainedColumnsPerSecond
                        : 0.0;
                const double frozenSourceColumnDelta =
                    frozenRetainedSourceColumnDeltaForZoomFill(
                        requestedSourceColumnDelta,
                        m_zoomFillFrozenRetainedLastColumnDelta,
                        rollingMode,
                        centeredViewportScrollsWithPlayback);
                m_zoomFillFrozenRetainedLastColumnDelta =
                    frozenSourceColumnDelta;
                const double currentRetainedPixelsPerSecond =
                    retainedScrollColumnsPerSecond() * std::max(0.001, effectiveZoom);
                const double frozenTargetScale =
                    m_zoomFillFrozenRetainedPixelsPerSecond > 0.0
                        && std::isfinite(m_zoomFillFrozenRetainedPixelsPerSecond)
                    ? currentRetainedPixelsPerSecond
                        / m_zoomFillFrozenRetainedPixelsPerSecond
                    : 1.0;
                retainedTextureSegments =
                    scrollFrozenRetainedSegmentsForZoomFill(
                        m_zoomFillFrozenRetainedSegments,
                        frozenSourceColumnDelta,
                        retainedPageWidth,
                        frozenTargetScale,
                        m_zoomFillFrozenRetainedAnchorX);
                retainedTextureSegmentsOverflowed = false;
                haveRetainedTextureSegments = !retainedTextureSegments.isEmpty();
                rebuildRetainedDrawSegments();
            };
            const bool finalRetainedTextureCoversVisibleRange =
                canUseRetainedSourceTextureBody
                && retainedSourcePagesCoverDisplayRangeLocked(
                    displayLeft,
                    displayRight,
                    retainedRenderableRight,
                    rollingMode,
                    retainedPageWidth,
                    true);
            const bool retainedSourceReadyForReveal =
                retainedSourceReadyForZoomReveal(
                    canUseRetainedSourceTextureBody,
                    finalRetainedTextureCoversVisibleRange,
                    !m_gpuDisplayImage.isNull(),
                    m_gpuSourcePageUploadColumns,
                    m_gpuSourcePageTextureReset);
            const bool freezeCanRevealFinalFrame =
                rendererFreeze
                && m_zoomFillDataReady
                && !m_awaitingZoomData
                && retainedSourceReadyForReveal;
            const bool revealViaWarmedDisplayImage =
                freezeCanRevealFinalFrame
                && gpuDebugMode == SpectrogramGpu::DebugMode::Full
                && !finalRetainedTextureCoversVisibleRange
                && !m_gpuDisplayImage.isNull();
            const bool useFrozenRetainedSourceForFrame =
                shouldDrawFrozenRetainedSourceForZoomFill(
                    rendererFreeze,
                    retainedFreezeAvailableForFrame,
                    freezeCanRevealFinalFrame);

            if (useFrozenRetainedSourceForFrame) {
                useCachedRetainedSourceSegments();
            } else if (canUseRetainedSourceTextureBody
                && drawPixels > 0
                && currentWindow != nullptr) {
                retainedSourceTexture = m_gpuSourceTexture.texture();
                retainedSourcePageGeneration = m_precomputedSessionGeneration;
                const RetainedRenderPlan retainedPlan = buildRetainedRenderPlanLocked(
                    displayLeft,
                    displayRight,
                    retainedRenderableRight,
                    drawX,
                    drawPixels,
                    h,
                    effectiveZoom,
                    rollingMode,
                    retainedSourcePageWidthForGpuLimit(currentWindow));
                retainedTextureSegments = retainedPlan.segments;
                haveRetainedTextureSegments = retainedPlan.haveSegments;
                retainedTextureSegmentsOverflowed = retainedPlan.overflowed;
                if (retainedTextureSegmentsOverflowed) {
                    // Sparse centered windows can contain more than two visible
                    // runs when evicted columns leave holes inside the viewport.
                    // Fall back to the warmed CPU display image for this frame.
                    retainedTextureSegments = {};
                    retainedSourceTexture = nullptr;
                    haveRetainedTextureSegments = false;
                }
                rebuildRetainedDrawSegments();
            }
            bool retainedSourceReadyForFrame =
                !retainedBodyDrawSegments.isEmpty()
                && haveRetainedTextureSegments;
            const bool usePostRevealCachedRetainedSourceForFrame =
                !useFrozenRetainedSourceForFrame
                && canUseRetainedSourceTextureBody
                && shouldDrawCachedRetainedSourceForFrame(
                    rendererFreeze,
                    retainedFreezeAvailableForFrame,
                    freezeCanRevealFinalFrame,
                    retainedSourceReadyForFrame,
                    m_zoomFillFrozenRetainedAvailable);
            if (usePostRevealCachedRetainedSourceForFrame) {
                useCachedRetainedSourceSegments();
                retainedSourceReadyForFrame =
                    !retainedBodyDrawSegments.isEmpty()
                    && haveRetainedTextureSegments;
            }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            if (rendererFreeze && m_zoomFillDataReady && !m_awaitingZoomData) {
                const int remainingLogs = g_zoomFillRenderLogBudget.fetch_sub(1);
                if (remainingLogs > 0) {
                    struct ImageColumnStats {
                        int columns{-1};
                        int blackColumns{-1};
                        int dimColumns{-1};
                        int longestBlackStart{-1};
                        int longestBlackRun{-1};
                        int minPeak{-1};
                        int maxPeak{-1};
                    };
                    const auto collectImageStats = [](const QImage &image) {
                        ImageColumnStats stats;
                        if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
                            return stats;
                        }
                        stats.columns = image.width();
                        stats.blackColumns = 0;
                        stats.dimColumns = 0;
                        stats.longestBlackStart = -1;
                        stats.longestBlackRun = 0;
                        stats.minPeak = 255;
                        stats.maxPeak = 0;
                        int blackRunStart = -1;
                        int blackRunLength = 0;
                        for (int x = 0; x < image.width(); ++x) {
                            int columnPeak = 0;
                            for (int y = 0; y < image.height(); ++y) {
                                const QRgb pixel = image.pixel(x, y);
                                columnPeak = std::max(
                                    columnPeak,
                                    std::max(qRed(pixel), std::max(qGreen(pixel), qBlue(pixel))));
                            }
                            stats.minPeak = std::min(stats.minPeak, columnPeak);
                            stats.maxPeak = std::max(stats.maxPeak, columnPeak);
                            if (columnPeak <= 1) {
                                ++stats.blackColumns;
                                if (blackRunStart < 0) {
                                    blackRunStart = x;
                                    blackRunLength = 0;
                                }
                                ++blackRunLength;
                                if (blackRunLength > stats.longestBlackRun) {
                                    stats.longestBlackRun = blackRunLength;
                                    stats.longestBlackStart = blackRunStart;
                                }
                            } else {
                                blackRunStart = -1;
                                blackRunLength = 0;
                            }
                            if (columnPeak <= 8) {
                                ++stats.dimColumns;
                            }
                        }
                        return stats;
                    };
                    const ImageColumnStats cpuStats = collectImageStats(m_gpuDisplayImage);
                    const QSize retainedTextureSize =
                        retainedSourceTexture != nullptr
                            ? retainedSourceTexture->textureSize()
                            : QSize();
                    const qint64 decodedVisibleRight = rollingMode
                        ? displayRight
                        : std::min(displayRight, static_cast<qint64>(m_precomputedMaxColumnIndex));
                    const auto logSegment = [](const RetainedRenderSegment &segment) {
                        return QStringLiteral("valid=%1 src=%2+%3 dst=%4+%5")
                            .arg(segment.valid ? 1 : 0)
                            .arg(segment.sourceRect.x())
                            .arg(segment.sourceRect.width())
                            .arg(segment.targetRect.x(), 0, 'f', 1)
                            .arg(segment.targetRect.width(), 0, 'f', 1);
                    };
                    const QString segment0 = retainedTextureSegments.isEmpty()
                        ? QStringLiteral("none")
                        : logSegment(retainedTextureSegments[0]);
                    const QString segment1 = retainedTextureSegments.size() < 2
                        ? QStringLiteral("none")
                        : logSegment(retainedTextureSegments[1]);
                    FERROUS_SPECTROGRAM_PROFILE_LOGF(stderr,
                        "[ui-spectrogram] zoom_fill_reveal_gate reveal=%d renderer_freeze=%d "
                        "can_retained=%d retained_cover=%d retained_segments=%d overflow=%d "
                        "display=%lld..%lld decoded_right=%lld draw=%d filled=%d ez=%.4f "
                        "gpu_source_range=%lld..%lld gpu_source_token=%llu current_token=%llu "
                        "source_tex=%dx%d cpu_image=%dx%d cpu_black=%d cpu_dim=%d "
                        "cpu_longest_black=%d@%d cpu_peak=%d..%d seg0=\"%s\" seg1=\"%s\"\n",
                        freezeCanRevealFinalFrame ? 1 : 0,
                        rendererFreeze ? 1 : 0,
                        canUseRetainedSourceTextureBody ? 1 : 0,
                        finalRetainedTextureCoversVisibleRange ? 1 : 0,
                        haveRetainedTextureSegments ? 1 : 0,
                        retainedTextureSegmentsOverflowed ? 1 : 0,
                        static_cast<long long>(displayLeft),
                        static_cast<long long>(displayRight),
                        static_cast<long long>(decodedVisibleRight),
                        drawPixels,
                        visibleCols,
                        effectiveZoom,
                        static_cast<long long>(m_gpuSourceTextureMinColumnIndex),
                        static_cast<long long>(m_gpuSourceTextureMaxColumnIndex),
                        static_cast<unsigned long long>(m_gpuSourceTextureTrackToken),
                        static_cast<unsigned long long>(m_precomputedTrackToken),
                        retainedTextureSize.width(),
                        retainedTextureSize.height(),
                        m_gpuDisplayImage.width(),
                        m_gpuDisplayImage.height(),
                        cpuStats.blackColumns,
                        cpuStats.dimColumns,
                        cpuStats.longestBlackRun,
                        cpuStats.longestBlackStart,
                        cpuStats.minPeak,
                        cpuStats.maxPeak,
                        qPrintable(segment0),
                        qPrintable(segment1));
                }
            }
#endif
            if (freezeCanRevealFinalFrame) {
                m_zoomFillActive = false;
                clearZoomFillFrozenCacheLocked();
                rendererFreeze = false;
            }

            useRetainedSourceTextureBodyForFrame =
                canUseRetainedSourceTextureBody
                && !revealViaWarmedDisplayImage
                && retainedSourceReadyForFrame
                && (!rendererFreeze || useFrozenRetainedSourceForFrame);
            if (useRetainedSourceTextureBodyForFrame
                && !rendererFreeze
                && retainedSourceTexture == m_gpuSourceTexture.texture()) {
                m_zoomFillFrozenRetainedSegments = retainedTextureSegments;
                m_zoomFillFrozenRetainedAvailable = true;
                anchorCachedRetainedSourceForCurrentFrame();
            }
            if (!useRetainedSourceTextureBodyForFrame
                && retainedTextureSegmentsOverflowed
                && m_gpuDisplayImage.isNull()) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                profileFallbackReason = "retained-segment-overflow";
                const auto displayStart = std::chrono::steady_clock::now();
                const quint64 displayGenerationBefore = m_gpuDisplayGeneration;
#endif
                syncPrecomputedDisplayImageLocked(
                    drawPixels,
                    h,
                    displayLeft,
                    displayRight,
                    rollingMode,
                    effectiveZoom,
                    true);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                profileDisplayImageBuildMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - displayStart).count();
                profileCpuDisplayImageRebuilt =
                    profileCpuDisplayImageRebuilt
                    || (m_gpuDisplayGeneration != displayGenerationBefore
                        && !m_gpuDisplayImage.isNull());
#endif
            }
            const bool haveSelectedSpectrogramContent =
                useRetainedSourceTextureBodyForFrame
                    ? haveRetainedTextureSegments
                    : (rendererFreeze
                           ? !m_zoomFillFrozenDisplayImage.isNull()
                           : !m_gpuDisplayImage.isNull());

            if (m_zoomFillFreezePending
                && !rollingMode
                && haveSelectedSpectrogramContent
                && !rendererFreeze) {
                if (useRetainedSourceTextureBodyForFrame
                    && !retainedTextureSegments.isEmpty()) {
                    m_zoomFillFrozenRetainedSegments = retainedTextureSegments;
                    m_zoomFillFrozenRetainedAvailable = true;
                }
                captureZoomFillFrozenCacheLocked(
                    false,
                    effectiveZoom,
                    playheadPixel >= 0 ? static_cast<double>(playheadPixel) : -1.0);
                m_zoomFillFrozenRetainedActive =
                    m_zoomFillFrozenRetainedAvailable;
                m_zoomFillActive =
                    !m_zoomFillFrozenDisplayImage.isNull()
                    || m_zoomFillFrozenRetainedActive;
                m_zoomFillDataReady = false;
                m_zoomFillFreezePending = false;
            }

            if (haveSelectedSpectrogramContent && drawPixels > 0) {
                if (maintainCpuDisplayImage && m_gpuDisplayImage.isNull()) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    const auto displayStart = std::chrono::steady_clock::now();
                    const quint64 displayGenerationBefore = m_gpuDisplayGeneration;
#endif
                    syncPrecomputedDisplayImageLocked(
                        drawPixels,
                        h,
                        displayLeft,
                        displayRight,
                        rollingMode,
                        effectiveZoom,
                        true);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    profileDisplayImageBuildMs += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - displayStart).count();
                    profileCpuDisplayImageRebuilt =
                        profileCpuDisplayImageRebuilt
                        || (m_gpuDisplayGeneration != displayGenerationBefore
                            && !m_gpuDisplayImage.isNull());
#endif
                }
                syncPrecomputedGpuLookupImagesLocked();
                if (gpuDebugMode == SpectrogramGpu::DebugMode::Full) {
                    syncGpuRowLutImageLocked(h);
                }
                showSpectrogram = true;
                spectrogramTargetRect = QRectF(
                    drawX,
                    0.0,
                    static_cast<double>(drawPixels),
                    static_cast<double>(h));
                if (useRetainedSourceTextureBodyForFrame) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    profilePaintPath =
                        gpuDebugMode == SpectrogramGpu::DebugMode::TextureNode
                            ? "retained-texture-node"
                            : "retained-full";
#endif
                    spectrogramSourceRect = retainedTextureSegments.front().sourceRect;
                } else if (rendererFreeze) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    profilePaintPath = "frozen-display";
#endif
                    spectrogramNodeImage = m_zoomFillFrozenDisplayImage;
                    spectrogramImageGeneration = m_zoomFillFrozenDisplayGeneration;
                } else {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    profilePaintPath =
                        debugModeRequestsRetainedSourceTextureBody
                            ? "fallback-display"
                            : "cpu-display";
#endif
                    spectrogramNodeImage = m_gpuDisplayImage;
                    spectrogramImageGeneration = m_gpuDisplayGeneration;
                }
                spectrogramImageDebugMode =
                    (!useRetainedSourceTextureBodyForFrame
                     && debugModeRequestsRetainedSourceTextureBody)
                        ? SpectrogramGpu::DebugMode::TextureNode
                        : gpuDebugMode;
                if (!useRetainedSourceTextureBodyForFrame) {
                    spectrogramSourceRect = QRect(
                        0,
                        0,
                        spectrogramNodeImage.width(),
                        spectrogramNodeImage.height());
                }
                spectrogramDbRemapImage = m_gpuDbRemapImage;
                spectrogramPaletteImage = m_gpuPaletteImage;
                spectrogramRowLutImage = m_gpuRowLutImage;
                spectrogramDbRemapGeneration = m_gpuDbRemapGeneration;
                spectrogramPaletteGeneration = m_gpuPaletteGeneration;
                spectrogramRowLutGeneration = m_gpuRowLutGeneration;
                spectrogramRollingIncrementalScroll = false;
                spectrogramRollingShiftPixels = 0;
                spectrogramSubpixelOffset =
                    (useRetainedSourceTextureBodyForFrame
                        && gpuDebugMode == SpectrogramGpu::DebugMode::Full
                        && rollingMode)
                        ? m_gpuDisplaySubpixelOffset
                        : 0.0;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                const double gpuEffectiveZoom = useRetainedSourceTextureBodyForFrame
                    ? static_cast<double>(drawPixels)
                        / static_cast<double>(std::max<qint64>(1, displayRight - displayLeft + 1))
                    : static_cast<double>(drawPixels)
                        / static_cast<double>(std::max(1, spectrogramNodeImage.width()));
                const int remainingLogs = g_gpuDebugStageLogBudget.fetch_sub(1);
                if (remainingLogs > 0 && gpuDebugMode != SpectrogramGpu::DebugMode::Full) {
                    FERROUS_SPECTROGRAM_TRACE_LOGF(
                        stderr,
                        "[Qt-gpu-stage@%p] mode=%s rect=%.1f,%.1f %.1fx%.1f src=%dx%d zoom=%.4f\n",
                        static_cast<const void *>(this),
                        SpectrogramGpu::debugModeName(gpuDebugMode),
                        spectrogramTargetRect.x(),
                        spectrogramTargetRect.y(),
                        spectrogramTargetRect.width(),
                        spectrogramTargetRect.height(),
                        useRetainedSourceTextureBodyForFrame
                            ? spectrogramSourceRect.width()
                            : spectrogramNodeImage.width(),
                        useRetainedSourceTextureBodyForFrame
                            ? m_precomputedBinsPerColumn
                            : spectrogramNodeImage.height(),
                        gpuEffectiveZoom);
                }
#endif
            }

            if (!rollingMode && playheadPixel >= 0) {
                showPlayhead = true;
                playheadRect = QRectF(
                    static_cast<double>(std::clamp(playheadPixel, 0, w - 1)),
                    0.0,
                    1.0,
                    static_cast<double>(h));
                playheadLine =
                    buildPlayheadVerticalLineLocked(std::clamp(playheadPixel, 0, w - 1), h);
            }

            // Frequency grid — only depends on static parameters.
            // Never rebuilt during normal playback.
            if (m_gridEnabled && m_binsPerColumn > 1) {
                freqGridLines = buildFreqGridLinesLocked(w, h);
                const bool freqStale = m_freqGridDirty
                    || m_freqGridImage.isNull()
                    || m_freqGridImage.width() != w
                    || m_freqGridImage.height() != h
                    || m_sampleRateHz != m_freqGridCachedSampleRateHz
                    || m_binsPerColumn != m_freqGridCachedBinsPerColumn
                    || node->freqGridOverlayTexture == nullptr;
                if (freqStale) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    const auto overlayStart = std::chrono::steady_clock::now();
#endif
                    updateFreqGridOverlayLocked(w, h, false);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    profileOverlayBuildMs += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - overlayStart).count();
#endif
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
                timeGridLines =
                    buildTimeGridLinesLocked(w, h, displayLeft, rollingMode, columnsPerSecond, drawX);
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
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    const auto overlayStart = std::chrono::steady_clock::now();
#endif
                    updateTimeGridOverlayLocked(
                        w, h, kTimeGridPadding, displayLeft, rollingMode, cps, drawX,
                        false);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    profileOverlayBuildMs += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - overlayStart).count();
#endif
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
                crosshairVerticalLine = buildCrosshairVerticalLineLocked(w, h);
                crosshairHorizontalLine = buildCrosshairHorizontalLineLocked(w, h);
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
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    const auto overlayStart = std::chrono::steady_clock::now();
#endif
                    updateCrosshairOverlayLocked(
                        w, h, displayLeft, rollingMode, cps, drawX, false);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    profileOverlayBuildMs += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - overlayStart).count();
#endif
                    crosshairOverlayImage = m_crosshairImage;
                    crosshairOverlaySize = m_crosshairImage.size();
                    crosshairOverlayChanged = true;
                } else {
                    crosshairOverlaySize = m_crosshairImage.size();
                }
                showCrosshairOverlay = !m_crosshairImage.isNull();
            }
        }

        showOverlay = m_showFpsOverlay && m_fpsValue > 0;
        if (showOverlay) {
            if (m_overlayDirty || node->overlayTexture == nullptr) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                const auto overlayStart = std::chrono::steady_clock::now();
#endif
                updateOverlayImageLocked();
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                profileOverlayBuildMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - overlayStart).count();
#endif
                overlayImage = m_overlayImage;
                overlaySize = m_overlayImage.size();
                overlayChanged = true;
            } else {
                overlaySize = m_overlayImage.size();
            }
        }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        profilePendingColumns = 0;
        profileColumnCount = static_cast<size_t>(std::max<qint64>(0, m_ringWriteSeq - m_ringOldestSeq));
        profileBinsPerColumn = m_precomputedBinsPerColumn;
#endif
    }

    QVector<QSGTexture *> staleSpectrogramTextures;
    QSGTexture *oldOverlayTexture = nullptr;
    QSGTexture *oldFreqGridOverlayTexture = nullptr;
    QSGTexture *oldTimeGridOverlayTexture = nullptr;
    QSGTexture *oldCrosshairOverlayTexture = nullptr;
    const bool useTextureNodeBody =
        spectrogramImageDebugMode == SpectrogramGpu::DebugMode::TextureNode;
    const bool useSourceOnlySampleBody =
        spectrogramImageDebugMode == SpectrogramGpu::DebugMode::ShaderSourceOnly;
    const bool useStockMaterialBody =
        spectrogramImageDebugMode == SpectrogramGpu::DebugMode::StockMaterial;
    const bool useBodySampleMaterial =
        spectrogramImageDebugMode == SpectrogramGpu::DebugMode::Full;
    const bool useRetainedSourceTextureBody =
        (useTextureNodeBody || useSourceOnlySampleBody || useBodySampleMaterial)
        && useRetainedSourceTextureBodyForFrame;
    if (useRetainedSourceTextureBody) {
        staleSpectrogramTextures.push_back(node->spectrogramTextureResource);
        staleSpectrogramTextures.push_back(node->spectrogramTailTextureResource);
        node->spectrogramTextureResource = nullptr;
        node->spectrogramTailTextureResource = nullptr;
        node->spectrogramTextureScrollPixels = 0;
        node->spectrogramTextureWidth = 0;
        node->spectrogramTailTextureWidth = 0;
        node->spectrogramTextureUploaded = false;
        clearTailSegments(node);
    } else {
        spectrogramTextureChanged =
            showSpectrogram
            && !spectrogramNodeImage.isNull()
            && (nodeRecreated
                || !node->spectrogramTextureUploaded
                || node->spectrogramTextureGeneration != spectrogramImageGeneration
                || node->spectrogramTextureDebugMode != spectrogramImageDebugMode);
        if (spectrogramTextureChanged) {
            const int nextScrollPixels =
                node->spectrogramTextureScrollPixels + spectrogramRollingShiftPixels;
            const bool canAdvanceRollingTexture =
                spectrogramRollingIncrementalScroll
                && node->spectrogramTextureUploaded
                && node->spectrogramTextureResource != nullptr
                && node->spectrogramTextureDebugMode == SpectrogramGpu::DebugMode::Full
                && nextScrollPixels > 0;
            if (canAdvanceRollingTexture) {
                clearTailSegments(node);
                node->spectrogramTextureScrollPixels = nextScrollPixels;
                const auto uploadSegmentTexture =
                    [&](const QImage &image) -> QSGTexture * {
                        if (currentWindow == nullptr || image.isNull()) {
                            return nullptr;
                        }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                        const auto uploadStart = std::chrono::steady_clock::now();
#endif
                        QSGTexture *texture = createStandaloneWindowTexture(
                            currentWindow,
                            image,
                            QSGTexture::Nearest);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                        profileSpectrogramTextureUploadMs += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - uploadStart).count();
                        profileUsedTailSpectrogramUpload = true;
                        profileTailUploadWidth = image.width();
#endif
                        return texture;
                    };

                const int baseWidth = std::max(0, node->spectrogramTextureWidth);
                const int scrollPixels = std::clamp(
                    node->spectrogramTextureScrollPixels,
                    0,
                    std::max(0, spectrogramNodeImage.width()));
                if (scrollPixels > 0) {
                    const int tailWidth = std::min(scrollPixels, spectrogramNodeImage.width());
                    const int sourceX = spectrogramNodeImage.width() - tailWidth;
                    QImage tailImage = spectrogramNodeImage.copy(
                        sourceX,
                        0,
                        tailWidth,
                        spectrogramNodeImage.height());
                    staleSpectrogramTextures.push_back(node->spectrogramTailTextureResource);
                    node->spectrogramTailTextureResource = uploadSegmentTexture(tailImage);
                    node->spectrogramTailTextureWidth = tailWidth;
                } else {
                    staleSpectrogramTextures.push_back(node->spectrogramTailTextureResource);
                    node->spectrogramTailTextureResource = nullptr;
                    node->spectrogramTailTextureWidth = 0;
                }

                if (baseWidth > 0
                    && node->spectrogramTextureScrollPixels >= baseWidth
                    && node->spectrogramTailTextureResource != nullptr
                    && node->spectrogramTailTextureWidth == baseWidth) {
                    staleSpectrogramTextures.push_back(node->spectrogramTextureResource);
                    node->spectrogramTextureResource = node->spectrogramTailTextureResource;
                    node->spectrogramTextureWidth = node->spectrogramTailTextureWidth;
                    node->spectrogramTailTextureResource = nullptr;
                    node->spectrogramTailTextureWidth = 0;
                    node->spectrogramTextureScrollPixels = 0;
                }
                node->spectrogramTextureGeneration = spectrogramImageGeneration;
                node->spectrogramTextureDebugMode = spectrogramImageDebugMode;
                node->spectrogramTextureUploaded = true;
            } else {
                staleSpectrogramTextures.push_back(node->spectrogramTextureResource);
                staleSpectrogramTextures.push_back(node->spectrogramTailTextureResource);
                node->spectrogramTextureResource = nullptr;
                node->spectrogramTailTextureResource = nullptr;
                node->spectrogramTextureScrollPixels = 0;
                node->spectrogramTextureWidth = 0;
                node->spectrogramTailTextureWidth = 0;
                clearTailSegments(node);
                if (showSpectrogram && !spectrogramNodeImage.isNull() && currentWindow != nullptr) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    const auto uploadStart = std::chrono::steady_clock::now();
#endif
                    node->spectrogramTextureResource = createStandaloneWindowTexture(
                        currentWindow,
                        spectrogramNodeImage,
                        QSGTexture::Nearest);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
                    profileSpectrogramTextureUploadMs += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - uploadStart).count();
                    profileUsedFullSpectrogramUpload = true;
#endif
                    if (node->spectrogramTextureResource != nullptr) {
                        node->spectrogramTextureWidth = spectrogramNodeImage.width();
                        node->spectrogramTextureGeneration = spectrogramImageGeneration;
                        node->spectrogramTextureDebugMode = spectrogramImageDebugMode;
                        node->spectrogramTextureUploaded = true;
                    }
                }
            }
        }
    }
    const bool remapTextureStale =
        showSpectrogram
        && !useTextureNodeBody
        && !useStockMaterialBody
        && !useSourceOnlySampleBody
        && !spectrogramDbRemapImage.isNull()
        && (nodeRecreated
            || node->spectrogramDbRemapTextureResource == nullptr
            || node->spectrogramDbRemapTextureGeneration != spectrogramDbRemapGeneration);
    const bool paletteTextureStale =
        showSpectrogram
        && !useTextureNodeBody
        && !useStockMaterialBody
        && !useSourceOnlySampleBody
        && !spectrogramPaletteImage.isNull()
        && (nodeRecreated
            || node->spectrogramPaletteTextureResource == nullptr
            || node->spectrogramPaletteTextureGeneration != spectrogramPaletteGeneration);
    const bool rowLutTextureStale =
        showSpectrogram
        && useBodySampleMaterial
        && !spectrogramRowLutImage.isNull()
        && (nodeRecreated
            || node->spectrogramRowLutTextureResource == nullptr
            || node->spectrogramRowLutTextureGeneration != spectrogramRowLutGeneration);
    if (remapTextureStale) {
        staleSpectrogramTextures.push_back(node->spectrogramDbRemapTextureResource);
        node->spectrogramDbRemapTextureResource = createStandaloneWindowTexture(
            currentWindow,
            spectrogramDbRemapImage,
            QSGTexture::Nearest);
        node->spectrogramDbRemapTextureGeneration = spectrogramDbRemapGeneration;
    }
    if (paletteTextureStale) {
        staleSpectrogramTextures.push_back(node->spectrogramPaletteTextureResource);
        node->spectrogramPaletteTextureResource = createStandaloneWindowTexture(
            currentWindow,
            spectrogramPaletteImage,
            QSGTexture::Nearest);
        node->spectrogramPaletteTextureGeneration = spectrogramPaletteGeneration;
    }
    if (rowLutTextureStale) {
        staleSpectrogramTextures.push_back(node->spectrogramRowLutTextureResource);
        node->spectrogramRowLutTextureResource = createStandaloneWindowTexture(
            currentWindow,
            spectrogramRowLutImage,
            QSGTexture::Nearest);
        node->spectrogramRowLutTextureGeneration = spectrogramRowLutGeneration;
    }
    if (overlayChanged) {
        oldOverlayTexture = node->overlayTexture;
        node->overlayTexture = nullptr;
        if (!overlayImage.isNull() && currentWindow != nullptr) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            const auto uploadStart = std::chrono::steady_clock::now();
#endif
            node->overlayTexture = currentWindow->createTextureFromImage(overlayImage);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            profileOverlayTextureUploadMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - uploadStart).count();
            profileOverlayTextureUploaded = true;
#endif
            if (node->overlayTexture != nullptr) {
                node->overlayTexture->setFiltering(QSGTexture::Linear);
            }
        }
    }

    if (freqGridOverlayChanged) {
        oldFreqGridOverlayTexture = node->freqGridOverlayTexture;
        node->freqGridOverlayTexture = nullptr;
        if (!freqGridOverlayImage.isNull() && currentWindow != nullptr) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            const auto uploadStart = std::chrono::steady_clock::now();
#endif
            node->freqGridOverlayTexture = currentWindow->createTextureFromImage(freqGridOverlayImage);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            profileOverlayTextureUploadMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - uploadStart).count();
            profileFreqGridTextureUploaded = true;
#endif
            if (node->freqGridOverlayTexture != nullptr) {
                node->freqGridOverlayTexture->setFiltering(QSGTexture::Linear);
            }
        }
    }

    if (timeGridOverlayChanged) {
        oldTimeGridOverlayTexture = node->timeGridOverlayTexture;
        node->timeGridOverlayTexture = nullptr;
        if (!timeGridOverlayImage.isNull() && currentWindow != nullptr) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            const auto uploadStart = std::chrono::steady_clock::now();
#endif
            node->timeGridOverlayTexture = currentWindow->createTextureFromImage(timeGridOverlayImage);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            profileOverlayTextureUploadMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - uploadStart).count();
            profileTimeGridTextureUploaded = true;
#endif
            if (node->timeGridOverlayTexture != nullptr) {
                node->timeGridOverlayTexture->setFiltering(QSGTexture::Linear);
            }
        }
    }

    if (crosshairOverlayChanged) {
        oldCrosshairOverlayTexture = node->crosshairOverlayTexture;
        node->crosshairOverlayTexture = nullptr;
        if (!crosshairOverlayImage.isNull() && currentWindow != nullptr) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            const auto uploadStart = std::chrono::steady_clock::now();
#endif
            node->crosshairOverlayTexture = currentWindow->createTextureFromImage(crosshairOverlayImage);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
            profileOverlayTextureUploadMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - uploadStart).count();
            profileCrosshairTextureUploaded = true;
#endif
            if (node->crosshairOverlayTexture != nullptr) {
                node->crosshairOverlayTexture->setFiltering(QSGTexture::Linear);
            }
        }
    }
    const double sourceTargetScale =
        spectrogramSourceRect.width() > 0
            ? (spectrogramTargetRect.width()
               / static_cast<double>(spectrogramSourceRect.width()))
            : 0.0;
    const auto viewportWidthForSourceColumns = [&](int sourceColumns) {
        return std::max(0.0, static_cast<double>(sourceColumns) * sourceTargetScale);
    };

    QRectF baseTargetRect;
    QRect baseSourceRect;
    QRectF tailTargetRect;
    QRect tailSourceRect;
    QSGTexture *baseBodyTexture = nullptr;
    QSGTexture *tailBodyTexture = nullptr;
    if (useRetainedSourceTextureBody) {
        baseTargetRect = QRectF();
        baseSourceRect = QRect();
        tailTargetRect = QRectF();
        tailSourceRect = QRect();
    } else {
        baseTargetRect = spectrogramTargetRect;
        baseTargetRect.translate(-spectrogramSubpixelOffset, 0.0);
        baseSourceRect = spectrogramSourceRect;
        const int scrollPixels = std::clamp(
            node->spectrogramTextureScrollPixels,
            0,
            std::max(0, node->spectrogramTextureWidth - 1));
        if (scrollPixels > 0) {
            baseTargetRect.setWidth(
                viewportWidthForSourceColumns(node->spectrogramTextureWidth - scrollPixels));
            baseSourceRect = QRect(
                scrollPixels,
                0,
                node->spectrogramTextureWidth - scrollPixels,
                spectrogramSourceRect.height());
        }

        baseBodyTexture = node->spectrogramTextureResource;
        tailBodyTexture = node->spectrogramTailTextureResource;
        if (showSpectrogram && node->spectrogramTailTextureResource != nullptr) {
            tailTargetRect = QRectF(
                baseTargetRect.x() + baseTargetRect.width(),
                spectrogramTargetRect.y(),
                viewportWidthForSourceColumns(node->spectrogramTailTextureWidth),
                spectrogramTargetRect.height());
            tailSourceRect = QRect(
                0,
                0,
                node->spectrogramTailTextureWidth,
                spectrogramSourceRect.height());
        }
    }

    if (useTextureNodeBody
        && showSpectrogram
        && baseBodyTexture != nullptr
        && !baseSourceRect.isEmpty()) {
        const auto bodyTextureTransform =
            useRetainedSourceTextureBody
                ? QSGSimpleTextureNode::TextureCoordinatesTransformMode(
                      QSGSimpleTextureNode::MirrorVertically)
                : QSGSimpleTextureNode::TextureCoordinatesTransformMode(
                      QSGSimpleTextureNode::NoTransform);
        configureTextureNode(
            node->spectrogramTexture,
            baseBodyTexture,
            baseTargetRect,
            baseSourceRect,
            node->placeholderTexture,
            bodyTextureTransform);
    } else {
        configureTextureNode(
            node->spectrogramTexture,
            nullptr,
            QRectF(),
            QRect(),
            node->placeholderTexture);
    }

    const bool bodySourceSampleReady =
        useSourceOnlySampleBody
        && baseBodyTexture != nullptr;
    const bool bodyStockMaterialReady =
        useStockMaterialBody
        && baseBodyTexture != nullptr;
    const bool bodySampleMaterialReady =
        useBodySampleMaterial
        && baseBodyTexture != nullptr
        && node->spectrogramDbRemapTextureResource != nullptr
        && node->spectrogramPaletteTextureResource != nullptr
        && node->spectrogramRowLutTextureResource != nullptr;
    SpectrogramBodySampleMaterial::State bodySampleState;
    bodySampleState.sourceTexture =
        baseBodyTexture != nullptr
            ? baseBodyTexture
            : node->placeholderTexture;
    bodySampleState.dbRemapTexture =
        node->spectrogramDbRemapTextureResource != nullptr
            ? node->spectrogramDbRemapTextureResource
            : node->placeholderTexture;
    bodySampleState.paletteTexture =
        node->spectrogramPaletteTextureResource != nullptr
            ? node->spectrogramPaletteTextureResource
            : node->placeholderTexture;
    bodySampleState.rowLutTexture =
        node->spectrogramRowLutTextureResource != nullptr
            ? node->spectrogramRowLutTextureResource
            : node->placeholderTexture;
    bodySampleState.sourceRect = QRectF(baseSourceRect);
    bodySampleState.drawOffset = QPointF();
    bodySampleState.sourceTextureSize =
        baseBodyTexture != nullptr ? baseBodyTexture->textureSize() : QSize();
    bodySampleState.effectiveZoom =
        baseSourceRect.width() > 0
            ? static_cast<float>(
                  baseTargetRect.width()
                  / static_cast<double>(baseSourceRect.width()))
            : 1.0F;
    bodySampleState.sourceColumnOffset = static_cast<float>(baseSourceRect.x());
    if (lodRenderState.texture != nullptr && lodRenderState.primaryLevel > 0) {
        bodySampleState.sourceTexture = lodRenderState.texture;
        bodySampleState.secondarySourceTexture =
            lodRenderState.secondaryTexture != nullptr
                ? lodRenderState.secondaryTexture
                : lodRenderState.texture;
        bodySampleState.sourceRect = lodRenderState.sourceRect;
        bodySampleState.secondarySourceRect =
            lodRenderState.secondaryTexture != nullptr
                ? lodRenderState.secondarySourceRect
                : lodRenderState.sourceRect;
        bodySampleState.sourceTextureSize = lodRenderState.textureSize;
        bodySampleState.secondarySourceTextureSize =
            lodRenderState.secondaryTexture != nullptr
                ? lodRenderState.secondaryTextureSize
                : lodRenderState.textureSize;
        bodySampleState.sourceColumnOffset =
            static_cast<float>(lodRenderState.sourceRect.x());
        bodySampleState.lodBlend = lodRenderState.blend;
        bodySampleState.primaryLodLevel = lodRenderState.primaryLevel;
        bodySampleState.secondaryLodLevel = lodRenderState.secondaryLevel;
        bodySampleState.primaryPlaneCount = lodRenderState.primaryPlaneCount;
        bodySampleState.secondaryPlaneCount = lodRenderState.secondaryPlaneCount;
    }
    if (useStockMaterialBody) {
        syncSpectrogramStockMaterialNode(
            node->spectrogramStockMaterialNode,
            showSpectrogram && bodyStockMaterialReady ? baseTargetRect : QRectF(),
            baseBodyTexture,
            QRectF(baseSourceRect),
            node->placeholderTexture);
        syncSpectrogramBodySampleMaterialNode(
            node->spectrogramBodySampleNode,
            QRectF(),
            bodySampleState);
        syncSpectrogramSourceSampleMaterialNode(
            node->spectrogramSourceSampleNode,
            QRectF(),
            nullptr,
            QRectF(),
            node->placeholderTexture);
    } else if (useBodySampleMaterial) {
        syncSpectrogramBodySampleMaterialNode(
            node->spectrogramBodySampleNode,
            showSpectrogram && bodySampleMaterialReady ? baseTargetRect : QRectF(),
            bodySampleState);
        syncSpectrogramStockMaterialNode(
            node->spectrogramStockMaterialNode,
            QRectF(),
            nullptr,
            QRectF(),
            node->placeholderTexture);
        syncSpectrogramSourceSampleMaterialNode(
            node->spectrogramSourceSampleNode,
            QRectF(),
            nullptr,
            QRectF(),
            node->placeholderTexture);
    } else {
        syncSpectrogramBodySampleMaterialNode(
            node->spectrogramBodySampleNode,
            QRectF(),
            bodySampleState);
        syncSpectrogramStockMaterialNode(
            node->spectrogramStockMaterialNode,
            QRectF(),
            nullptr,
            QRectF(),
            node->placeholderTexture);
        syncSpectrogramSourceSampleMaterialNode(
            node->spectrogramSourceSampleNode,
            showSpectrogram && bodySourceSampleReady ? baseTargetRect : QRectF(),
            useSourceOnlySampleBody ? baseBodyTexture : nullptr,
            useSourceOnlySampleBody ? QRectF(baseSourceRect) : QRectF(),
            node->placeholderTexture);
    }

    const bool tailSourceSampleReady =
        useSourceOnlySampleBody
        && tailBodyTexture != nullptr;
    const bool tailStockMaterialReady =
        useStockMaterialBody
        && tailBodyTexture != nullptr;
    const bool tailSampleMaterialReady =
        useBodySampleMaterial
        && tailBodyTexture != nullptr
        && node->spectrogramDbRemapTextureResource != nullptr
        && node->spectrogramPaletteTextureResource != nullptr
        && node->spectrogramRowLutTextureResource != nullptr;
    SpectrogramBodySampleMaterial::State tailBodySampleState;
    tailBodySampleState.sourceTexture =
        tailBodyTexture != nullptr
            ? tailBodyTexture
            : node->placeholderTexture;
    tailBodySampleState.dbRemapTexture =
        node->spectrogramDbRemapTextureResource != nullptr
            ? node->spectrogramDbRemapTextureResource
            : node->placeholderTexture;
    tailBodySampleState.paletteTexture =
        node->spectrogramPaletteTextureResource != nullptr
            ? node->spectrogramPaletteTextureResource
            : node->placeholderTexture;
    tailBodySampleState.rowLutTexture =
        node->spectrogramRowLutTextureResource != nullptr
            ? node->spectrogramRowLutTextureResource
            : node->placeholderTexture;
    tailBodySampleState.sourceRect = QRectF(tailSourceRect);
    tailBodySampleState.drawOffset = QPointF();
    tailBodySampleState.sourceTextureSize =
        tailBodyTexture != nullptr ? tailBodyTexture->textureSize() : QSize();
    tailBodySampleState.effectiveZoom =
        tailSourceRect.width() > 0
            ? static_cast<float>(
                  tailTargetRect.width()
                  / static_cast<double>(tailSourceRect.width()))
            : 1.0F;
    tailBodySampleState.sourceColumnOffset = static_cast<float>(tailSourceRect.x());
    if (showSpectrogram && tailBodyTexture != nullptr) {
        if (useTextureNodeBody) {
            const auto tailTextureTransform =
                useRetainedSourceTextureBody
                    ? QSGSimpleTextureNode::TextureCoordinatesTransformMode(
                          QSGSimpleTextureNode::MirrorVertically)
                    : QSGSimpleTextureNode::TextureCoordinatesTransformMode(
                          QSGSimpleTextureNode::NoTransform);
            configureTextureNode(
                node->spectrogramTailTexture,
                tailBodyTexture,
                tailTargetRect,
                tailSourceRect,
                node->placeholderTexture,
                tailTextureTransform);
        } else {
            configureTextureNode(
                node->spectrogramTailTexture,
                nullptr,
                QRectF(),
                QRect(),
                node->placeholderTexture);
        }
    } else {
        configureTextureNode(
            node->spectrogramTailTexture,
            nullptr,
            QRectF(),
            QRect(),
            node->placeholderTexture);
    }
    if (useStockMaterialBody) {
        syncSpectrogramStockMaterialNode(
            node->spectrogramTailStockMaterialNode,
            showSpectrogram && tailStockMaterialReady ? tailTargetRect : QRectF(),
            tailBodyTexture,
            QRectF(tailSourceRect),
            node->placeholderTexture);
        syncSpectrogramBodySampleMaterialNode(
            node->spectrogramTailBodySampleNode,
            QRectF(),
            tailBodySampleState);
        syncSpectrogramSourceSampleMaterialNode(
            node->spectrogramTailSourceSampleNode,
            QRectF(),
            nullptr,
            QRectF(),
            node->placeholderTexture);
    } else if (useBodySampleMaterial) {
        syncSpectrogramBodySampleMaterialNode(
            node->spectrogramTailBodySampleNode,
            showSpectrogram && tailSampleMaterialReady ? tailTargetRect : QRectF(),
            tailBodySampleState);
        syncSpectrogramStockMaterialNode(
            node->spectrogramTailStockMaterialNode,
            QRectF(),
            nullptr,
            QRectF(),
            node->placeholderTexture);
        syncSpectrogramSourceSampleMaterialNode(
            node->spectrogramTailSourceSampleNode,
            QRectF(),
            nullptr,
            QRectF(),
            node->placeholderTexture);
    } else {
        syncSpectrogramBodySampleMaterialNode(
            node->spectrogramTailBodySampleNode,
            QRectF(),
            tailBodySampleState);
        syncSpectrogramStockMaterialNode(
            node->spectrogramTailStockMaterialNode,
            QRectF(),
            nullptr,
            QRectF(),
            node->placeholderTexture);
        syncSpectrogramSourceSampleMaterialNode(
            node->spectrogramTailSourceSampleNode,
            showSpectrogram && tailSourceSampleReady ? tailTargetRect : QRectF(),
            useSourceOnlySampleBody ? tailBodyTexture : nullptr,
            useSourceOnlySampleBody ? QRectF(tailSourceRect) : QRectF(),
            node->placeholderTexture);
    }

    if (useRetainedSourceTextureBody) {
        ensureRetainedBodySegmentNodeCount(node, retainedBodyDrawSegments.size());
        for (qsizetype i = 0; i < node->retainedBodySegmentNodes.size(); ++i) {
            const RetainedBodyDrawSegment *segment =
                i < retainedBodyDrawSegments.size() ? &retainedBodyDrawSegments[i] : nullptr;
            syncRetainedBodySegmentNode(
                node->retainedBodySegmentNodes[i],
                segment,
                spectrogramImageDebugMode,
                node->spectrogramDbRemapTextureResource,
                node->spectrogramPaletteTextureResource,
                node->spectrogramRowLutTextureResource,
                node->placeholderTexture);
        }
    } else {
        for (const RetainedBodySegmentNode &segmentNode : node->retainedBodySegmentNodes) {
            syncRetainedBodySegmentNode(
                segmentNode,
                nullptr,
                spectrogramImageDebugMode,
                node->spectrogramDbRemapTextureResource,
                node->spectrogramPaletteTextureResource,
                node->spectrogramRowLutTextureResource,
                node->placeholderTexture);
        }
    }

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const int remainingMaterialLogs = g_gpuMaterialLogBudget.fetch_sub(1);
    if (remainingMaterialLogs > 0 && gpuDebugMode != SpectrogramGpu::DebugMode::TextureNode) {
        const auto textureKey = [](QSGTexture *texture) -> long long {
            return texture != nullptr
                ? static_cast<long long>(texture->comparisonKey())
                : 0LL;
        };
        FERROUS_SPECTROGRAM_PROFILE_LOGF(
            stderr,
            "[Qt-gpu-material@%p] mode=%s show=%d body_ready=%d tail_ready=%d body_rect=%.1fx%.1f tail_rect=%.1fx%.1f "
            "src=%dx%d src_tex=%lld remap=%dx%d remap_tex=%lld palette=%dx%d palette_tex=%lld tail_tex=%lld tail_w=%d\n",
            static_cast<const void *>(this),
            SpectrogramGpu::debugModeName(gpuDebugMode),
            showSpectrogram ? 1 : 0,
            (bodySampleMaterialReady || bodyStockMaterialReady || bodySourceSampleReady) ? 1 : 0,
            (tailSampleMaterialReady || tailStockMaterialReady || tailSourceSampleReady) ? 1 : 0,
            baseTargetRect.width(),
            baseTargetRect.height(),
            tailTargetRect.width(),
            tailTargetRect.height(),
            spectrogramNodeImage.width(),
            spectrogramNodeImage.height(),
            textureKey(node->spectrogramTextureResource),
            spectrogramDbRemapImage.width(),
            spectrogramDbRemapImage.height(),
            textureKey(node->spectrogramDbRemapTextureResource),
            spectrogramPaletteImage.width(),
            spectrogramPaletteImage.height(),
            textureKey(node->spectrogramPaletteTextureResource),
            textureKey(node->spectrogramTailTextureResource),
            node->spectrogramTailTextureWidth);
    }
#endif

    const bool overlayLinesActive =
        !freqGridLines.isEmpty()
        || !timeGridLines.isEmpty()
        || playheadLine.has_value()
        || crosshairVerticalLine.has_value()
        || crosshairHorizontalLine.has_value();
    configureLineNode(
        node->overlayLines,
        freqGridLines,
        timeGridLines,
        playheadLine,
        crosshairVerticalLine,
        crosshairHorizontalLine);

    if (showPlayhead && !overlayLinesActive) {
        node->playhead->setRect(playheadRect);
        node->playhead->setColor(QColor(255, 255, 255, kPlayheadLineAlpha));
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

    for (QSGTexture *texture : staleSpectrogramTextures) {
        delete texture;
    }
    delete oldOverlayTexture;
    delete oldFreqGridOverlayTexture;
    delete oldTimeGridOverlayTexture;
    delete oldCrosshairOverlayTexture;

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
            FERROUS_SPECTROGRAM_PROFILE_LOGF(
                stderr,
                "[ui-spectrogram] paint_spike path=%s fallback=%s ms=%.3f cache_ms=%.3f "
                "display_ms=%.3f retained_upload_ms=%.3f overlay_build_ms=%.3f "
                "tex_ms=%.3f lod_tex_upload_ms=%.3f overlay_tex_ms=%.3f "
                "lod_primary=%d lod_secondary=%d lod_blend=%.3f lod_available=%d "
                "lod_missing_reason=%s cpu_rebuild=%d retained_reset=%d "
                "retained_reset_reason=%s retained_spans=%d retained_cols=%d "
                "full_upload=%d tail_upload=%d tail_w=%d "
                "ov=%d fg=%d tg=%d ch=%d pending=%zu cols=%zu bins=%d size=%dx%d\n",
                profilePaintPath,
                profileFallbackReason,
                paintMs,
                profileCacheSyncMs,
                profileDisplayImageBuildMs,
                profileRetainedUploadMs,
                profileOverlayBuildMs,
                profileSpectrogramTextureUploadMs,
                profileLodTextureUploadMs,
                profileOverlayTextureUploadMs,
                profileLodPrimaryLevel,
                profileLodSecondaryLevel,
                static_cast<double>(profileLodBlend),
                profileLodAvailable ? 1 : 0,
                profileLodMissingReason,
                profileCpuDisplayImageRebuilt ? 1 : 0,
                profileRetainedTextureReset ? 1 : 0,
                profileRetainedResetReason,
                profileRetainedUploadSpanCount,
                profileRetainedUploadColumns,
                profileUsedFullSpectrogramUpload ? 1 : 0,
                profileUsedTailSpectrogramUpload ? 1 : 0,
                profileTailUploadWidth,
                profileOverlayTextureUploaded ? 1 : 0,
                profileFreqGridTextureUploaded ? 1 : 0,
                profileTimeGridTextureUploaded ? 1 : 0,
                profileCrosshairTextureUploaded ? 1 : 0,
                profilePendingColumns,
                profileColumnCount,
                profileBinsPerColumn,
                w,
                h);
        }
        const double elapsed = std::chrono::duration<double>(paintEnd - m_profileLast).count();
        if (elapsed >= 1.0) {
            FERROUS_SPECTROGRAM_PROFILE_LOGF(
                stderr,
                "[ui-spectrogram] paints/s=%llu paint_ms/s=%.2f avg_ms=%.3f "
                "last_path=%s last_fallback=%s last_display_ms=%.3f "
                "last_retained_upload_ms=%.3f last_lod_primary=%d "
                "last_lod_secondary=%d last_lod_blend=%.3f "
                "last_lod_tex_upload_ms=%.3f last_lod_available=%d "
                "last_lod_missing_reason=%s last_retained_cols=%d cols=%zu bins=%d\n",
                static_cast<unsigned long long>(m_profilePaints),
                m_profilePaintMs,
                m_profilePaints > 0 ? (m_profilePaintMs / static_cast<double>(m_profilePaints)) : 0.0,
                profilePaintPath,
                profileFallbackReason,
                profileDisplayImageBuildMs,
                profileRetainedUploadMs,
                profileLodPrimaryLevel,
                profileLodSecondaryLevel,
                static_cast<double>(profileLodBlend),
                profileLodTextureUploadMs,
                profileLodAvailable ? 1 : 0,
                profileLodMissingReason,
                profileRetainedUploadColumns,
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
    m_gpuSourceTexture.release();
    m_gpuLodSourceTexture.release();
    m_gpuSecondaryLodSourceTexture.release();
    invalidateSpectrogramLodTextureStateLocked();
    m_sceneGraphGeneration += 1;
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
        invalidateRenderCachesLocked();
        m_freqGridDirty = true;
        m_timeGridDirty = true;
        m_crosshairDirty = true;
    }
    const int newWidth = static_cast<int>(newGeometry.width());
    const int oldWidth = static_cast<int>(oldGeometry.width());
    if (newWidth != oldWidth && newWidth > 0) {
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

    qint64 displayLeft = m_crosshairCachedDisplayLeft;
    bool rollingMode = m_crosshairCachedRollingMode;
    double drawX = m_crosshairCachedDrawX;
    if (m_displayMode == 1
        && width() > 0.0
        && m_precomputedTotalColumnsEstimate > 0) {
        const double effectiveZoom = effectiveZoomLocked();
        const int widgetWidth = std::max(1, static_cast<int>(std::floor(width())));
        const int visibleWindowCols = static_cast<int>(
            std::ceil(static_cast<double>(widgetWidth) / effectiveZoom));
        const int halfWindowCols = visibleWindowCols / 2;
        const auto now = std::chrono::steady_clock::now();
        const double renderPositionSeconds = currentRenderPositionSecondsLocked(now);
        const int nowCol = static_cast<int>(std::floor(
            std::max(0.0, renderPositionSeconds) * columnsPerSecond));
        const qint64 maxColCount = m_precomputedMaxColumnIndex >= 0
            ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
            : static_cast<qint64>(0);
        const qint64 estimateCount = std::max(
            static_cast<qint64>(m_precomputedTotalColumnsEstimate),
            static_cast<qint64>(1));
        const qint64 estTotalCols = std::max(maxColCount, estimateCount);
        qint64 displayRight = 0;
        if (static_cast<qint64>(visibleWindowCols) * 100 / estTotalCols >= 90) {
            displayLeft = 0;
            displayRight = estTotalCols - 1;
        } else {
            displayLeft = std::max<qint64>(
                0,
                static_cast<qint64>(nowCol) - static_cast<qint64>(halfWindowCols));
            displayRight = std::min(
                estTotalCols - 1,
                displayLeft + static_cast<qint64>(visibleWindowCols) - 1);
            displayLeft = std::max<qint64>(
                0,
                displayRight - static_cast<qint64>(visibleWindowCols) + 1);
        }
        const bool decodedTailLooksFinal =
            centeredDecodedTailLooksFinal(
                maxColCount, estimateCount, visibleWindowCols);
        if (decodedTailLooksFinal
            && maxColCount - 1
                < static_cast<qint64>(nowCol) + static_cast<qint64>(halfWindowCols)) {
            displayRight = std::min(displayRight, maxColCount - 1);
            displayLeft = std::max<qint64>(
                0,
                displayRight - static_cast<qint64>(visibleWindowCols) + 1);
        }
        const double columnF = std::max(0.0, renderPositionSeconds) * columnsPerSecond;
        const double columnPhase = std::clamp(columnF - std::floor(columnF), 0.0, 0.999);
        const qint64 displaySpan = displayRight - displayLeft + 1;
        const qint64 totalColsForScroll = std::max(maxColCount, estimateCount);
        const bool nearFullTrack =
            totalColsForScroll > 0
            && displaySpan * 100 / totalColsForScroll >= 90;
        const bool eofClampedToDecodedContent =
            decodedTailLooksFinal && maxColCount > 0 && displayRight >= maxColCount - 1;
        const bool centeredScrolling =
            !nearFullTrack
            && displayLeft > 0
            && !eofClampedToDecodedContent
            && displayRight < totalColsForScroll - 1;
        drawX = centeredScrolling ? -columnPhase * effectiveZoom : 0.0;
        rollingMode = false;
    }

    const double seconds = pixelToTimeSeconds(
        event->position().x(),
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

std::optional<QLineF> SpectrogramItem::buildCrosshairVerticalLineLocked(int width, int height) const {
    const bool localHover = m_hoverActive;
    const bool hasSharedX = m_crosshairSharedX >= 0.0;
    if (!m_crosshairEnabled || (!localHover && !hasSharedX) || width <= 0 || height <= 0) {
        return std::nullopt;
    }

    const int effectiveX = localHover
        ? std::clamp(static_cast<int>(m_hoverPosition.x()), 0, width - 1)
        : std::clamp(static_cast<int>(m_crosshairSharedX), 0, width - 1);
    return QLineF(
        static_cast<double>(effectiveX),
        0.0,
        static_cast<double>(effectiveX),
        static_cast<double>(height - 1));
}

std::optional<QLineF> SpectrogramItem::buildCrosshairHorizontalLineLocked(int width, int height) const {
    if (!m_crosshairEnabled || !m_hoverActive || width <= 0 || height <= 0) {
        return std::nullopt;
    }

    const int mouseY = std::clamp(static_cast<int>(m_hoverPosition.y()), 0, height - 1);
    return QLineF(
        0.0,
        static_cast<double>(mouseY),
        static_cast<double>(width - 1),
        static_cast<double>(mouseY));
}

QVector<QLineF> SpectrogramItem::buildFreqGridLinesLocked(int width, int height) const {
    QVector<QLineF> lines;
    if (!m_gridEnabled || width <= 0 || height <= 0 || m_binsPerColumn <= 1) {
        return lines;
    }

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
        lines.append(QLineF(0.0, static_cast<double>(pixelY),
            static_cast<double>(width - 1), static_cast<double>(pixelY)));
    }

    return lines;
}

QVector<QLineF> SpectrogramItem::buildTimeGridLinesLocked(
    int width,
    int height,
    qint64 displayLeft,
    bool rollingMode,
    double columnsPerSecond,
    double drawX) const {
    QVector<QLineF> lines;
    if (!m_gridEnabled || width <= 0 || height <= 0 || columnsPerSecond <= 0.0) {
        return lines;
    }

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

    const double ez = effectiveZoomLocked();
    const double timeLeft = pixelToTimeSeconds(
        0.0, displayLeft, rollingMode, m_rollingEpoch, columnsPerSecond, drawX, ez);
    const double timeRight = pixelToTimeSeconds(
        static_cast<double>(width - 1), displayLeft, rollingMode,
        m_rollingEpoch, columnsPerSecond, drawX, ez);
    const double trackTimeLeft = timeLeft - m_gaplessPositionOffset;
    const double trackTimeRight = timeRight - m_gaplessPositionOffset;
    const double startTrackTime = std::max(0.0,
        std::ceil(std::max(0.0, trackTimeLeft) / timeInterval) * timeInterval);

    for (double trackT = startTrackTime; trackT <= trackTimeRight; trackT += timeInterval) {
        const double continuousT = trackT + m_gaplessPositionOffset;
        const double pxF = timeToPixelX(
            continuousT, displayLeft, rollingMode, m_rollingEpoch, columnsPerSecond, drawX, ez);
        const int pixelX = static_cast<int>(std::round(pxF));
        if (pixelX < 0 || pixelX >= width) {
            continue;
        }
        lines.append(QLineF(
            static_cast<double>(pixelX), 0.0,
            static_cast<double>(pixelX), static_cast<double>(height - 1)));
    }

    return lines;
}

std::optional<QLineF> SpectrogramItem::buildPlayheadVerticalLineLocked(int pixelX, int height) const {
    if (pixelX < 0 || height <= 0) {
        return std::nullopt;
    }

    const double x = static_cast<double>(pixelX);
    return QLineF(x, 0.0, x, static_cast<double>(height - 1));
}

void SpectrogramItem::updateCrosshairOverlayLocked(
    int width, int height,
    qint64 displayLeft, bool rollingMode,
    double columnsPerSecond, double drawX, bool drawLinePrimitives) {
    // Determine the effective X for the vertical crosshair line.
    // Hovered pane uses its own mouse position; non-hovered panes
    // use the shared X coordinate propagated from the hovered pane.
    const bool localHover = m_hoverActive;
    const bool hasSharedX = m_crosshairSharedX >= 0.0;
    if (!m_crosshairEnabled || (!localHover && !hasSharedX)
        || width <= 0 || height <= 0) {
        m_crosshairImage = QImage();
        m_crosshairFreqLabelRect = QRect();
        m_crosshairTimeLabelRect = QRect();
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

    const bool drawFreqLabel = localHover;
    const bool drawTimeLabel = m_showTimeLabels;
    if (!drawLinePrimitives && !drawFreqLabel && !drawTimeLabel) {
        m_crosshairImage = QImage();
        m_crosshairFreqLabelRect = QRect();
        m_crosshairTimeLabelRect = QRect();
        m_crosshairDirty = false;
        m_crosshairCachedDisplayLeft = displayLeft;
        m_crosshairCachedDrawX = drawX;
        m_crosshairCachedRollingMode = rollingMode;
        m_crosshairCachedBinsPerColumn = m_binsPerColumn;
        return;
    }

    QRect nextFreqLabelRect;
    QRect nextTimeLabelRect;
    QString freqText;
    QString timeText;
    QFont labelFont;
    labelFont.setPixelSize(10);
    const QFontMetrics fm(labelFont);
    constexpr int pad = 3;

    if (drawFreqLabel) {
        const int mouseY = std::clamp(static_cast<int>(m_hoverPosition.y()), 0, height - 1);
        const double freqHz = pixelToFrequencyHzLocked(mouseY, height);
        if (freqHz >= 0.0) {
            freqText = formatFrequencyLabelPrecise(freqHz);
            const int textW = fm.horizontalAdvance(freqText);
            const int textH = fm.height();
            const int labelY = std::clamp(mouseY - textH / 2, 0, height - textH - 2 * pad);
            const int labelX = width - textW - 2 * pad - 2;
            nextFreqLabelRect = QRect(labelX, labelY, textW + 2 * pad, textH + 2 * pad);
        }
    }

    if (drawTimeLabel) {
        const double continuousTime = pixelToTimeSeconds(
            static_cast<double>(effectiveX), displayLeft, rollingMode,
            m_rollingEpoch, columnsPerSecond, drawX, effectiveZoomLocked());
        const double trackTime = continuousTime - m_gaplessPositionOffset;
        if (trackTime >= 0.0) {
            timeText = formatTimeLabelPrecise(trackTime);
            const int textW = fm.horizontalAdvance(timeText);
            const int textH = fm.height();
            const int labelX = std::clamp(effectiveX - textW / 2, 0, width - textW - 2 * pad);
            nextTimeLabelRect = QRect(
                labelX,
                height - textH - 2 * pad - 2,
                textW + 2 * pad,
                textH + 2 * pad);
        }
    }

    const bool reuseImageBuffer =
        !drawLinePrimitives
        && !m_crosshairImage.isNull()
        && m_crosshairImage.width() == width
        && m_crosshairImage.height() == height;
    if (!reuseImageBuffer) {
        m_crosshairImage = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
        m_crosshairImage.fill(Qt::transparent);
    }

    QPainter painter(&m_crosshairImage);
    painter.setRenderHint(QPainter::Antialiasing, false);

    if (reuseImageBuffer) {
        QRect clearRect = m_crosshairFreqLabelRect.adjusted(-1, -1, 1, 1)
            .united(m_crosshairTimeLabelRect.adjusted(-1, -1, 1, 1))
            .united(nextFreqLabelRect.adjusted(-1, -1, 1, 1))
            .united(nextTimeLabelRect.adjusted(-1, -1, 1, 1))
            .intersected(QRect(0, 0, width, height));
        if (!clearRect.isEmpty()) {
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(clearRect, Qt::transparent);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }
    }

    if (drawLinePrimitives) {
        const QColor lineColor(255, 255, 255, kCrosshairLineAlpha);
        QPen dotPen(lineColor);
        dotPen.setStyle(Qt::DotLine);
        dotPen.setWidthF(1.0);
        painter.setPen(dotPen);

        // Vertical line: full height on all panes (spans across panes visually).
        painter.drawLine(effectiveX, 0, effectiveX, height - 1);
    }

    if (localHover) {
        const int mouseY = std::clamp(static_cast<int>(m_hoverPosition.y()), 0, height - 1);
        if (drawLinePrimitives) {
            painter.drawLine(0, mouseY, width - 1, mouseY);
        }

        // Frequency label at RIGHT edge, aligned to cursor Y.
        painter.setFont(labelFont);

        const QColor labelBg(0, 0, 10, 160);
        const QColor labelFg(255, 255, 255, 220);
        if (!nextFreqLabelRect.isEmpty()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(labelBg);
            painter.drawRoundedRect(nextFreqLabelRect, 3, 3);
            painter.setPen(labelFg);
            painter.drawText(
                nextFreqLabelRect.x() + pad,
                nextFreqLabelRect.y() + pad + fm.ascent(),
                freqText);
        }
    }

    // Time label at BOTTOM edge — only on the bottom-most pane,
    // shown whenever any pane is hovered (via shared X).
    // Subtract gapless offset to show per-track time.
    if (m_showTimeLabels) {
        if (!nextTimeLabelRect.isEmpty()) {
            painter.setFont(labelFont);
            const QColor labelBg(0, 0, 10, 160);
            const QColor labelFg(255, 255, 255, 220);
            painter.setPen(Qt::NoPen);
            painter.setBrush(labelBg);
            painter.drawRoundedRect(nextTimeLabelRect, 3, 3);
            painter.setPen(labelFg);
            painter.drawText(
                nextTimeLabelRect.x() + pad,
                nextTimeLabelRect.bottom() - pad - fm.descent(),
                timeText);
        }
    }

    painter.end();
    m_crosshairFreqLabelRect = nextFreqLabelRect;
    m_crosshairTimeLabelRect = nextTimeLabelRect;
    m_crosshairDirty = false;
    m_crosshairCachedDisplayLeft = displayLeft;
    m_crosshairCachedDrawX = drawX;
    m_crosshairCachedRollingMode = rollingMode;
    m_crosshairCachedBinsPerColumn = m_binsPerColumn;
}

void SpectrogramItem::updateFreqGridOverlayLocked(int width, int height, bool drawLinePrimitives) {
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

        if (drawLinePrimitives) {
            painter.setPen(gridPen);
            painter.drawLine(0, pixelY, width - 1, pixelY);
        }

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
    bool rollingMode, double columnsPerSecond, double drawX, bool drawLinePrimitives) {
    if (!m_gridEnabled || width <= 0 || height <= 0 || columnsPerSecond <= 0.0) {
        m_timeGridImage = QImage();
        m_timeGridDirty = false;
        m_timeGridRenderDisplayLeft = displayLeft;
        m_timeGridRenderDrawX = drawX;
        m_timeGridCachedRollingMode = rollingMode;
        m_timeGridPadding = 0;
        return;
    }

    if (!drawLinePrimitives && !m_showTimeLabels) {
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

        if (drawLinePrimitives) {
            painter.setPen(gridPen);
            painter.drawLine(pixelX, 0, pixelX, height - 1);
        }

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
    m_gpuRowLutImage = QImage();
    m_gpuRowLutHeight = 0;
    m_gpuRowLutBins = 0;
}

bool SpectrogramItem::ensureZoomFillSnapshotImageLocked() {
    if (!m_gpuDisplayImage.isNull()) {
        return true;
    }
    if (!m_precomputedReady
        || m_precomputedBinsPerColumn <= 0
        || m_precomputedCanvasDisplayRight < m_precomputedCanvasDisplayLeft) {
        return false;
    }

    const int height = m_gpuDisplayHeight > 0
        ? m_gpuDisplayHeight
        : static_cast<int>(std::round(this->height()));
    const int itemWidth = std::max(1, static_cast<int>(std::round(width())));
    const int drawPixels = m_gpuDisplayWidth > 0
        ? m_gpuDisplayWidth
        : gpuDrawWidthForSourceColumnsLocked(
              itemWidth,
              m_precomputedCanvasDisplayLeft,
              m_precomputedCanvasDisplayRight,
              m_precomputedCanvasZoomLevel);
    if (drawPixels <= 0 || height <= 0) {
        return false;
    }

    ensureMapping(height);
    syncPrecomputedDisplayImageLocked(
        drawPixels,
        height,
        m_precomputedCanvasDisplayLeft,
        m_precomputedCanvasDisplayRight,
        m_precomputedCanvasRolling,
        m_precomputedCanvasZoomLevel,
        true);
    return !m_gpuDisplayImage.isNull();
}

void SpectrogramItem::captureZoomFillFrozenCacheLocked(
    bool allowCpuSnapshot,
    double effectiveZoom,
    double anchorX) {
    if (!m_zoomFillFrozenRetainedAvailable && allowCpuSnapshot) {
        ensureZoomFillSnapshotImageLocked();
    }
    m_zoomFillFrozenDisplayImage = m_gpuDisplayImage;
    m_zoomFillFrozenDisplayGeneration = m_gpuDisplayGeneration;
    m_zoomFillFrozenRetainedPositionSeconds =
        currentRenderPositionSecondsLocked(std::chrono::steady_clock::now());
    m_zoomFillFrozenRetainedColumnsPerSecond =
        m_precomputedSampleRateHz > 0 && m_precomputedHopSize > 0
            ? static_cast<double>(m_precomputedSampleRateHz)
                / static_cast<double>(m_precomputedHopSize)
            : 0.0;
    if (!std::isfinite(effectiveZoom) || effectiveZoom <= 0.0) {
        effectiveZoom = effectiveZoomLocked();
    }
    if (!std::isfinite(anchorX) || anchorX < 0.0) {
        anchorX = width() > 0.0 ? width() * 0.5 : 0.0;
    }
    m_zoomFillFrozenRetainedPixelsPerSecond =
        m_zoomFillFrozenRetainedColumnsPerSecond * std::max(0.001, effectiveZoom);
    m_zoomFillFrozenRetainedAnchorX = anchorX;
    m_zoomFillFrozenRetainedLastColumnDelta = 0.0;
    m_zoomFillFrozenRetainedGeneration = m_precomputedSessionGeneration;
}

void SpectrogramItem::clearZoomFillFrozenCacheLocked() {
    m_zoomFillFrozenDisplayImage = QImage();
    m_zoomFillFrozenDisplayGeneration = 0;
    m_zoomFillDataReady = false;
    m_zoomFillFrozenRetainedSegments = {};
    m_zoomFillFrozenRetainedPositionSeconds = 0.0;
    m_zoomFillFrozenRetainedColumnsPerSecond = 0.0;
    m_zoomFillFrozenRetainedPixelsPerSecond = 0.0;
    m_zoomFillFrozenRetainedAnchorX = 0.0;
    m_zoomFillFrozenRetainedLastColumnDelta = 0.0;
    m_zoomFillFrozenRetainedGeneration = 0;
    m_zoomFillFrozenRetainedAvailable = false;
    m_zoomFillFrozenRetainedActive = false;
    m_zoomFillFreezePending = false;
}

SpectrogramItem::RetainedPageTextureExtent SpectrogramItem::retainedPageTextureExtentForSpan(
    const int pageXBegin,
    const int pageXEnd,
    const int pageWidth) {
    if (pageWidth <= 0 || pageXEnd < pageXBegin) {
        return {};
    }

    const int clampedBegin = std::clamp(pageXBegin, 0, pageWidth - 1);
    const int clampedEnd = std::clamp(pageXEnd, clampedBegin, pageWidth - 1);
    if (clampedEnd < clampedBegin) {
        return {};
    }
    return RetainedPageTextureExtent{
        .xOrigin = 0,
        .width = pageWidth,
    };
}

bool SpectrogramItem::shouldDeferTinyRetainedUploadDuringSeek(
    const int visibleColumns,
    const bool rendererFreeze,
    const bool retainedPagesCoverVisibleRange,
    const qint64 seekAgeMs) {
    static constexpr int tinyVisibleColumnThreshold = 32;
    static constexpr qint64 seekDeferralWindowMs = 300;
    return visibleColumns > 0
        && visibleColumns <= tinyVisibleColumnThreshold
        && rendererFreeze
        && !retainedPagesCoverVisibleRange
        && seekAgeMs >= 0
        && seekAgeMs <= seekDeferralWindowMs;
}

bool SpectrogramItem::retainedSourceReadyForZoomReveal(
    const bool canUseRetainedSourceTextureBody,
    const bool retainedTextureCoversVisibleRange,
    const bool gpuDisplayImageAvailable,
    const int retainedUploadColumns,
    const bool retainedTextureReset) {
    if (!canUseRetainedSourceTextureBody) {
        return gpuDisplayImageAvailable;
    }
    return retainedTextureCoversVisibleRange
        && retainedUploadColumns <= 0
        && !retainedTextureReset;
}

bool SpectrogramItem::shouldDrawFrozenRetainedSourceForZoomFill(
    const bool rendererFreeze,
    const bool retainedFreezeAvailableForFrame,
    const bool freezeCanRevealFinalFrame) {
    return rendererFreeze
        && retainedFreezeAvailableForFrame
        && !freezeCanRevealFinalFrame;
}

bool SpectrogramItem::shouldDrawCachedRetainedSourceForFrame(
    const bool rendererFreeze,
    const bool retainedFreezeAvailableForFrame,
    const bool freezeCanRevealFinalFrame,
    const bool currentRetainedSourceReadyForFrame,
    const bool cachedRetainedAvailable) {
    if (rendererFreeze) {
        return shouldDrawFrozenRetainedSourceForZoomFill(
            true,
            retainedFreezeAvailableForFrame,
            freezeCanRevealFinalFrame);
    }
    return cachedRetainedAvailable && !currentRetainedSourceReadyForFrame;
}

bool SpectrogramItem::shouldCaptureZoomFillFreezeForHopChange(
    const bool zoomFillActive,
    const bool frozenDisplayAvailable,
    const bool frozenRetainedAvailable) {
    return !zoomFillActive
        || (!frozenDisplayAvailable && !frozenRetainedAvailable);
}

double SpectrogramItem::monotonicFrozenRetainedSourceColumnDelta(
    const double requestedColumnDelta,
    const double previousColumnDelta) {
    const double clampedPrevious = std::isfinite(previousColumnDelta)
        ? std::max(0.0, previousColumnDelta)
        : 0.0;
    if (!std::isfinite(requestedColumnDelta)) {
        return clampedPrevious;
    }
    return std::max(clampedPrevious, std::max(0.0, requestedColumnDelta));
}

double SpectrogramItem::frozenRetainedSourceColumnDeltaForZoomFill(
    const double requestedColumnDelta,
    const double previousColumnDelta,
    const bool rollingMode,
    const bool centeredViewportScrollsWithPlayback) {
    const double clampedPrevious = std::isfinite(previousColumnDelta)
        ? std::max(0.0, previousColumnDelta)
        : 0.0;
    // Stationary centered zoom-fill stays pinned. When the centered viewport
    // itself is moving, advance the frozen source in its own column space so
    // the target rect only carries the fractional phase used by the final
    // retained render at reveal.
    if (!rollingMode && !centeredViewportScrollsWithPlayback) {
        return clampedPrevious;
    }
    return monotonicFrozenRetainedSourceColumnDelta(
        requestedColumnDelta,
        clampedPrevious);
}

QVector<SpectrogramItem::RetainedRenderSegment>
SpectrogramItem::scrollFrozenRetainedSegmentsForZoomFill(
    const QVector<RetainedRenderSegment> &segments,
    const double sourceColumnDelta,
    const int pageWidth,
    const double targetScaleFactor,
    const double targetAnchorX) {
    if (segments.isEmpty()) {
        return segments;
    }

    const double safeSourceColumnDelta =
        std::isfinite(sourceColumnDelta) ? sourceColumnDelta : 0.0;
    const double safeTargetScaleFactor =
        std::isfinite(targetScaleFactor) && targetScaleFactor > 0.0
            ? targetScaleFactor
            : 1.0;
    const double safeTargetAnchorX =
        std::isfinite(targetAnchorX) ? targetAnchorX : 0.0;
    const auto scaleTargetRect = [&](const QRectF &targetRect) {
        if (std::abs(safeTargetScaleFactor - 1.0) < 0.0001) {
            return targetRect;
        }
        const double scaledX =
            safeTargetAnchorX
            + (targetRect.x() - safeTargetAnchorX) * safeTargetScaleFactor;
        return QRectF(
            scaledX,
            targetRect.y(),
            targetRect.width() * safeTargetScaleFactor,
            targetRect.height());
    };

    const double integerShiftDouble = std::floor(safeSourceColumnDelta);
    const double fractionalShift = safeSourceColumnDelta - integerShiftDouble;
    const qint64 integerShift =
        static_cast<qint64>(std::clamp(
            integerShiftDouble,
            static_cast<double>(std::numeric_limits<qint64>::min()),
            static_cast<double>(std::numeric_limits<qint64>::max())));

    QVector<RetainedRenderSegment> scrolled;
    scrolled.reserve(segments.size());
    for (const RetainedRenderSegment &segment : segments) {
        RetainedRenderSegment shifted = segment;
        if (segment.valid
            && !segment.sourceRect.isEmpty()
            && !segment.targetRect.isEmpty()) {
            const double sourceWidth =
                static_cast<double>(segment.sourceRect.width());
            const double sourceToTargetScale = sourceWidth > 0.0
                ? segment.targetRect.width() / sourceWidth
                : 0.0;
            const qint64 shiftedX =
                static_cast<qint64>(segment.sourceRect.x()) + integerShift;

            const double shiftedTargetX =
                segment.targetRect.x() - fractionalShift * sourceToTargetScale;
            if (pageWidth <= 0) {
                shifted.sourceRect.moveLeft(static_cast<int>(std::clamp(
                    shiftedX,
                    static_cast<qint64>(std::numeric_limits<int>::min()),
                    static_cast<qint64>(std::numeric_limits<int>::max()))));
                shifted.targetRect.moveLeft(shiftedTargetX);
                shifted.targetRect = scaleTargetRect(shifted.targetRect);
                scrolled.push_back(shifted);
                continue;
            }

            qint64 chunkX = shiftedX;
            qint64 remainingWidth = segment.sourceRect.width();
            while (remainingWidth > 0) {
                const qint64 pageIndex = chunkX >= 0
                    ? chunkX / pageWidth
                    : (chunkX - pageWidth + 1) / pageWidth;
                const qint64 pageEnd =
                    (pageIndex + 1) * static_cast<qint64>(pageWidth) - 1;
                const qint64 chunkWidth64 =
                    std::min(remainingWidth, pageEnd - chunkX + 1);
                if (chunkWidth64 <= 0) {
                    break;
                }
                const int chunkWidth = static_cast<int>(std::clamp(
                    chunkWidth64,
                    static_cast<qint64>(1),
                    static_cast<qint64>(std::numeric_limits<int>::max())));

                RetainedRenderSegment chunk = segment;
                chunk.sourceRect = QRect(
                    static_cast<int>(std::clamp(
                        chunkX,
                        static_cast<qint64>(std::numeric_limits<int>::min()),
                        static_cast<qint64>(std::numeric_limits<int>::max()))),
                    segment.sourceRect.y(),
                    chunkWidth,
                    segment.sourceRect.height());
                chunk.targetRect = QRectF(
                    shiftedTargetX
                        + static_cast<double>(chunkX - shiftedX)
                            * sourceToTargetScale,
                    segment.targetRect.y(),
                    static_cast<double>(chunkWidth) * sourceToTargetScale,
                    segment.targetRect.height());
                chunk.targetRect = scaleTargetRect(chunk.targetRect);
                scrolled.push_back(chunk);

                chunkX += chunkWidth64;
                remainingWidth -= chunkWidth64;
            }
            continue;
        }
        scrolled.push_back(shifted);
    }
    return scrolled;
}

QVector<SpectrogramItem::RetainedPageUploadSpan>
SpectrogramItem::retainedPageUploadSpansForPaintBudget(
    const QVector<RetainedPageUploadSpan> &spans,
    const int maxColumns,
    bool *budgetExhausted) {
    if (budgetExhausted != nullptr) {
        *budgetExhausted = false;
    }
    if (spans.isEmpty() || maxColumns <= 0) {
        return spans;
    }

    QVector<RetainedPageUploadSpan> bounded;
    int uploadedColumns = 0;
    for (const RetainedPageUploadSpan &span : spans) {
        const int spanWidth = span.pageXEnd - span.pageXBegin + 1;
        if (spanWidth <= 0) {
            continue;
        }
        const int remainingColumns = maxColumns - uploadedColumns;
        if (remainingColumns <= 0) {
            if (budgetExhausted != nullptr) {
                *budgetExhausted = true;
            }
            break;
        }
        if (spanWidth > remainingColumns) {
            RetainedPageUploadSpan clipped = span;
            clipped.pageXEnd = clipped.pageXBegin + remainingColumns - 1;
            clipped.displayRight = clipped.displayLeft + remainingColumns - 1;
            bounded.push_back(clipped);
            uploadedColumns += remainingColumns;
            if (budgetExhausted != nullptr) {
                *budgetExhausted = true;
            }
            break;
        }
        bounded.push_back(span);
        uploadedColumns += spanWidth;
    }

    if (bounded.size() < spans.size() && budgetExhausted != nullptr) {
        *budgetExhausted = true;
    }
    return bounded;
}

const char *SpectrogramItem::retainedPageResetReasonName(
    const RetainedPageResetReason reason) {
    switch (reason) {
    case RetainedPageResetReason::MissingPage:
        return "missing-page";
    case RetainedPageResetReason::PageWidthChanged:
        return "page-width-change";
    case RetainedPageResetReason::BinsChanged:
        return "bins-change";
    case RetainedPageResetReason::UploadReset:
        return "upload-reset";
    case RetainedPageResetReason::None:
        break;
    }
    return "none";
}

void SpectrogramItem::invalidateLegacyRetainedSourceTextureStateLocked() {
    m_gpuSourceUploadRgba.clear();
    m_gpuSourceUploadSpans.clear();
    m_gpuSourceTextureDisplayLeft = 0;
    m_gpuSourceTextureDisplayRight = -1;
    m_gpuSourceTextureRolling = false;
    m_gpuSourceTextureTrackToken = 0;
    m_gpuSourceTextureRingWriteSeq = -1;
    m_gpuSourceTextureRingOldestSeq = -1;
    m_gpuSourceTextureMinColumnIndex = 0;
    m_gpuSourceTextureMaxColumnIndex = -1;
    m_gpuSourceTexture.clearPendingSpans();
}

void SpectrogramItem::invalidateSpectrogramLodTextureStateLocked() {
    m_gpuLodUploadRgba.clear();
    m_gpuSecondaryLodUploadRgba.clear();
    m_gpuLodTextureLevel = 0;
    m_gpuLodTextureTrackToken = 0;
    m_gpuLodTextureGeneration = 0;
    m_gpuLodTextureStartColumn = 0;
    m_gpuLodTextureEndColumn = -1;
    m_gpuSecondaryLodTextureLevel = 0;
    m_gpuSecondaryLodTextureTrackToken = 0;
    m_gpuSecondaryLodTextureGeneration = 0;
    m_gpuSecondaryLodTextureStartColumn = 0;
    m_gpuSecondaryLodTextureEndColumn = -1;
}

void SpectrogramItem::invalidateRetainedSourceTextureStateLocked() {
    invalidateRetainedSourceTextureUploadStateLocked(false);
}

void SpectrogramItem::invalidateRetainedSourceTextureUploadStateLocked(
    const bool preservePageTextures) {
    invalidateLegacyRetainedSourceTextureStateLocked();
    if (preservePageTextures) {
        for (const auto &page : m_gpuSourcePages) {
            if (page == nullptr) {
                continue;
            }
            page->uploadedDisplayLeft = 0;
            page->uploadedDisplayRight = -1;
            page->dirtySpans.clear();
        }
    } else {
        m_gpuSourcePages.clear();
    }
    m_gpuSourcePageUploadSpanCount = 0;
    m_gpuSourcePageUploadColumns = 0;
    m_gpuSourcePageTextureReset = false;
    m_gpuSourcePageResetReason = RetainedPageResetReason::None;
}

void SpectrogramItem::invalidateRenderCachesLocked() {
    const bool hadGpuImages =
        !m_gpuDisplayImage.isNull()
        || !m_gpuDbRemapImage.isNull()
        || !m_gpuPaletteImage.isNull()
        || !m_gpuRowLutImage.isNull();
    m_precomputedCanvasDisplayLeft = 0;
    m_precomputedCanvasDisplayRight = -1;
    m_precomputedCanvasRolling = false;
    m_precomputedCanvasDirty = true;
    m_precomputedCanvasHopSize = 0;
    m_precomputedCanvasTotalColumnsEstimate = 0;
    m_gpuDisplayImage = QImage();
    clearZoomFillFrozenCacheLocked();
    m_gpuDbRemapImage = QImage();
    m_gpuPaletteImage = QImage();
    m_gpuRowLutImage = QImage();
    if (hadGpuImages) {
        bumpImageGeneration(m_gpuDisplayGeneration);
        bumpImageGeneration(m_gpuDbRemapGeneration);
        bumpImageGeneration(m_gpuPaletteGeneration);
        bumpImageGeneration(m_gpuRowLutGeneration);
    }
    m_gpuDisplaySourceGeneration = 0;
    m_gpuDisplayWidth = 0;
    m_gpuDisplayFilledWidth = 0;
    m_gpuDisplayHeight = 0;
    m_gpuDisplaySampleRateHz = 0;
    m_gpuDisplayBins = 0;
    m_gpuDisplayDbRange = 132.0;
    m_gpuDisplayMuted = false;
    m_gpuDisplayLogScale = false;
    m_gpuDisplayDisplayLeft = 0;
    m_gpuDisplayDisplayRight = -1;
    m_gpuDisplayRolling = false;
    m_gpuDisplayRingWriteSeq = -1;
    m_gpuDisplayRingOldestSeq = -1;
    m_gpuDisplayMaxColumnIndex = -1;
    m_gpuDisplayTrackToken = 0;
    m_gpuDisplayEffectiveZoom = 1.0;
    m_gpuDisplayLastUpdateWasIncremental = false;
    m_gpuDisplayLastShiftPixels = 0;
    m_gpuDisplayLastUpdatedColumns = 0;
    m_gpuDisplaySubpixelOffset = 0.0;
    invalidateRetainedSourceTextureStateLocked();
    invalidateSpectrogramLodTextureStateLocked();
    m_gpuDbRemapBins = 0;
    m_gpuDbRemapDbRange = 132.0;
    m_gpuPaletteMuted = false;
    m_gpuRowLutHeight = 0;
    m_gpuRowLutBins = 0;
}

int SpectrogramItem::gpuMaxTextureColumns(QQuickWindow *window) const {
    constexpr int kFallbackMaxTextureColumns = 8192;
    if (m_gpuMaxTextureColumns > 0
        && m_gpuMaxTextureColumns != std::numeric_limits<int>::max()) {
        return m_gpuMaxTextureColumns;
    }
    if (window == nullptr || window->rendererInterface() == nullptr) {
        return kFallbackMaxTextureColumns;
    }
    if (window->rendererInterface()->graphicsApi() == QSGRendererInterface::OpenGL) {
        if (QOpenGLContext *context = QOpenGLContext::currentContext()) {
            QOpenGLFunctions *functions = context->functions();
            if (functions != nullptr) {
                GLint limit = 0;
                functions->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
                if (limit > 0) {
                    return limit;
                }
            }
        }
    }
    return kFallbackMaxTextureColumns;
}

int SpectrogramItem::retainedSourcePageWidthForGpuLimit(QQuickWindow *window) const {
    return std::max(1, std::min(kRetainedSourcePageWidth, gpuMaxTextureColumns(window)));
}

bool SpectrogramItem::retainedSourceTextureFitsGpuLimit(
    QQuickWindow *window,
    const int capacityColumns,
    const int binsPerColumn) const {
    if (capacityColumns <= 0 || binsPerColumn <= 0) {
        return false;
    }

    const int maxTextureSize = gpuMaxTextureColumns(window);
    return capacityColumns <= maxTextureSize && binsPerColumn <= maxTextureSize;
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

QImage SpectrogramItem::buildPrecomputedDbRemapImageLocked() const {
    const auto dbRemap = buildPrecomputedDbRemapLocked();
    QImage image(256, 1, QImage::Format_RGBA8888);
    for (int x = 0; x < image.width(); ++x) {
        const quint8 value = dbRemap[static_cast<size_t>(x)];
        image.setPixelColor(x, 0, QColor(value, value, value, 255));
    }
    return image;
}

QImage SpectrogramItem::buildPrecomputedPaletteImageLocked() const {
    const auto &palette = m_channelMuted ? m_palette32Gray : m_palette32;
    QImage image(kGradientTableSize, 1, QImage::Format_RGBA8888);
    for (int x = 0; x < image.width(); ++x) {
        image.setPixelColor(x, 0, QColor::fromRgb(palette[static_cast<size_t>(x)]));
    }
    return image;
}

QImage SpectrogramItem::buildRowLutImageLocked(const int height) const {
    if (height <= 0
        || m_binsPerColumn <= 0
        || static_cast<int>(m_iToBin.size()) != height) {
        return {};
    }

    QImage image(1, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        const int bin = std::clamp(
            m_iToBin[static_cast<size_t>(y)],
            0,
            m_binsPerColumn - 1);
        uchar *pixel = image.scanLine(y);
        pixel[0] = static_cast<uchar>(bin & 0xff);
        pixel[1] = static_cast<uchar>((bin >> 8) & 0xff);
        pixel[2] = 0;
        pixel[3] = 255;
    }
    return image;
}

SpectrogramRetainedSourceLayout SpectrogramItem::buildRetainedSourceLayoutLocked(
    const qint64 displayLeft,
    const qint64 displayRight,
    const bool rollingMode) const {
    return SpectrogramRetainedSourceLayout{
        .capacityColumns = m_ringCapacity,
        .binsPerColumn = m_precomputedBinsPerColumn,
        .displayLeft = displayLeft,
        .displayRight = displayRight,
        .rollingMode = rollingMode,
    };
}

QVector<SpectrogramItem::RetainedPageSpan> SpectrogramItem::buildRetainedPageSpansLocked(
    const qint64 displayLeft,
    const qint64 displayRight,
    const qint64 renderableRight,
    const bool rollingMode,
    const int pageWidth) const {
    QVector<RetainedPageSpan> spans;
    if (displayRight < displayLeft
        || m_precomputedBinsPerColumn <= 0
        || pageWidth <= 0) {
        return spans;
    }

    const qint64 scanRight = std::min(displayRight, renderableRight);
    if (scanRight < displayLeft) {
        return spans;
    }

    RetainedPageSpan current;
    bool haveCurrent = false;
    int previousPageX = -1;
    qint64 previousDisplay = -1;

    const auto flushSpan = [&]() {
        if (haveCurrent && current.displayRight >= current.displayLeft) {
            spans.push_back(current);
        }
        current = RetainedPageSpan{};
        haveCurrent = false;
        previousPageX = -1;
        previousDisplay = -1;
    };

    for (qint64 displayIndex = displayLeft; displayIndex <= scanRight; ++displayIndex) {
        const int slot = ringSlotForDisplayIndexLocked(displayIndex, rollingMode);
        if (slot < 0) {
            flushSpan();
            continue;
        }

        const qint64 pageIndex = slot / pageWidth;
        const int pageX = slot % pageWidth;
        const bool extendsCurrent =
            haveCurrent
            && current.pageIndex == pageIndex
            && pageX == previousPageX + 1
            && displayIndex == previousDisplay + 1;
        if (!extendsCurrent) {
            flushSpan();
            current = RetainedPageSpan{
                .pageIndex = pageIndex,
                .pageXBegin = pageX,
                .pageXEnd = pageX,
                .displayLeft = displayIndex,
                .displayRight = displayIndex,
            };
            haveCurrent = true;
        } else {
            current.pageXEnd = pageX;
            current.displayRight = displayIndex;
        }
        previousPageX = pageX;
        previousDisplay = displayIndex;
    }

    flushSpan();
    return spans;
}

SpectrogramItem::RetainedSourcePageState *SpectrogramItem::findRetainedSourcePageLocked(
    const quint64 trackToken,
    const quint64 generation,
    const bool rollingMode,
    const qint64 pageIndex) const {
    for (const auto &page : m_gpuSourcePages) {
        if (page != nullptr
            && page->key.rollingMode == rollingMode
            && page->key.generation == generation
            && (rollingMode || page->key.trackToken == trackToken)
            && page->key.pageIndex == pageIndex) {
            return page.get();
        }
    }
    return nullptr;
}

QVector<qint64> SpectrogramItem::retainedSourcePageIndicesToKeepLocked(
    const QVector<RetainedPageSpan> &visiblePages,
    const bool rollingMode,
    const int pageWidth) const {
    QVector<qint64> pageIndices;
    if (visiblePages.isEmpty() || pageWidth <= 0 || m_ringCapacity <= 0) {
        return pageIndices;
    }

    const qint64 pageCount =
        (m_ringCapacity + static_cast<qint64>(pageWidth) - 1)
        / static_cast<qint64>(pageWidth);
    if (pageCount <= 0) {
        return pageIndices;
    }

    const auto appendPage = [&](qint64 pageIndex) {
        if (rollingMode) {
            pageIndex = ((pageIndex % pageCount) + pageCount) % pageCount;
        }
        if (pageIndex < 0 || pageIndex >= pageCount || pageIndices.contains(pageIndex)) {
            return;
        }
        pageIndices.push_back(pageIndex);
    };

    for (const RetainedPageSpan &span : visiblePages) {
        appendPage(span.pageIndex - 1);
        appendPage(span.pageIndex);
        appendPage(span.pageIndex + 1);
    }
    std::sort(pageIndices.begin(), pageIndices.end());
    return pageIndices;
}

QVector<qint64> SpectrogramItem::retainedFrozenPageIndicesToKeepForZoomFill(
    const QVector<RetainedRenderSegment> &segments,
    const int pageWidth) {
    QVector<qint64> pageIndices;
    if (segments.isEmpty() || pageWidth <= 0) {
        return pageIndices;
    }

    const auto appendPage = [&](const qint64 pageIndex) {
        if (pageIndex < 0 || pageIndices.contains(pageIndex)) {
            return;
        }
        pageIndices.push_back(pageIndex);
    };

    for (const RetainedRenderSegment &segment : segments) {
        if (!segment.valid || segment.sourceRect.isEmpty()) {
            continue;
        }
        const qint64 sourceLeft = segment.sourceRect.x();
        const qint64 sourceRight =
            static_cast<qint64>(segment.sourceRect.x())
            + segment.sourceRect.width() - 1;
        const qint64 leftPage = sourceLeft / pageWidth;
        const qint64 rightPage = sourceRight / pageWidth;
        appendPage(leftPage - 1);
        appendPage(leftPage);
        appendPage(rightPage);
        appendPage(rightPage + 1);
    }

    std::sort(pageIndices.begin(), pageIndices.end());
    return pageIndices;
}

QVector<SpectrogramItem::RetainedPageUploadSpan> SpectrogramItem::collectRetainedPageUploadSpansLocked(
    const qint64 displayLeft,
    const qint64 displayRight,
    const qint64 renderableRight,
    const bool rollingMode,
    const int pageWidth) const {
    QVector<RetainedPageUploadSpan> uploads;
    const QVector<RetainedPageSpan> pageSpans =
        buildRetainedPageSpansLocked(displayLeft, displayRight, renderableRight, rollingMode, pageWidth);
    for (const RetainedPageSpan &span : pageSpans) {
        const RetainedSourcePageState *page = findRetainedSourcePageLocked(
            m_precomputedTrackToken,
            m_precomputedSessionGeneration,
            rollingMode,
            span.pageIndex);
        RetainedPageResetReason resetReason = RetainedPageResetReason::None;
        if (page == nullptr) {
            resetReason = RetainedPageResetReason::MissingPage;
        } else if (span.pageXBegin < page->pageXOrigin
            || span.pageXEnd >= page->pageXOrigin + page->width) {
            resetReason = RetainedPageResetReason::PageWidthChanged;
        } else if (page->binsPerColumn != m_precomputedBinsPerColumn) {
            resetReason = RetainedPageResetReason::BinsChanged;
        }
        if (resetReason != RetainedPageResetReason::None) {
            uploads.push_back(RetainedPageUploadSpan{
                .pageIndex = span.pageIndex,
                .pageXBegin = span.pageXBegin,
                .pageXEnd = span.pageXEnd,
                .displayLeft = span.displayLeft,
                .displayRight = span.displayRight,
                .resetPage = true,
                .resetReason = resetReason,
            });
            continue;
        }

        if (page->uploadedDisplayLeft <= span.displayLeft
            && page->uploadedDisplayRight >= span.displayRight) {
            continue;
        }

        if (span.displayLeft < page->uploadedDisplayLeft) {
            const qint64 dirtyRight = std::min(span.displayRight, page->uploadedDisplayLeft - 1);
            const int dirtyWidth = static_cast<int>(dirtyRight - span.displayLeft + 1);
            uploads.push_back(RetainedPageUploadSpan{
                .pageIndex = span.pageIndex,
                .pageXBegin = span.pageXBegin,
                .pageXEnd = span.pageXBegin + dirtyWidth - 1,
                .displayLeft = span.displayLeft,
                .displayRight = dirtyRight,
                .resetPage = false,
            });
        }

        if (span.displayRight > page->uploadedDisplayRight) {
            const qint64 dirtyLeft = std::max(span.displayLeft, page->uploadedDisplayRight + 1);
            const int cleanPrefix = static_cast<int>(dirtyLeft - span.displayLeft);
            uploads.push_back(RetainedPageUploadSpan{
                .pageIndex = span.pageIndex,
                .pageXBegin = span.pageXBegin + cleanPrefix,
                .pageXEnd = span.pageXEnd,
                .displayLeft = dirtyLeft,
                .displayRight = span.displayRight,
                .resetPage = false,
            });
        }
    }
    return uploads;
}

SpectrogramItem::RetainedRenderPlan SpectrogramItem::buildRetainedRenderPlanLocked(
    const qint64 displayLeft,
    const qint64 displayRight,
    const qint64 renderableRight,
    const double drawX,
    const int drawPixels,
    const int height,
    const double effectiveZoom,
    const bool rollingMode,
    const int pageWidth) const {
    RetainedRenderPlan plan;
    if (displayRight < displayLeft
        || drawPixels <= 0
        || height <= 0
        || m_precomputedBinsPerColumn <= 0
        || pageWidth <= 0) {
        return plan;
    }

    const qint64 scanRight = std::min(displayRight, renderableRight);
    if (scanRight < displayLeft) {
        return plan;
    }

    const qint64 sourceColumns = std::max<qint64>(1, displayRight - displayLeft + 1);
    const double targetScale = rollingMode
        ? effectiveZoom
        : static_cast<double>(drawPixels) / static_cast<double>(sourceColumns);
    const int filledPixels = rollingMode
        ? std::min(
              drawPixels,
              static_cast<int>(std::ceil(static_cast<double>(sourceColumns) * effectiveZoom)))
        : drawPixels;
    const int contentXOffset = rollingMode ? std::max(0, drawPixels - filledPixels) : 0;
    plan.coversRange = true;

    const QVector<RetainedPageSpan> pageSpans =
        buildRetainedPageSpansLocked(displayLeft, displayRight, scanRight, rollingMode, pageWidth);
    qint64 expectedDisplayLeft = displayLeft;
    for (const RetainedPageSpan &span : pageSpans) {
        if (span.displayLeft > expectedDisplayLeft) {
            plan.coversRange = false;
        }
        const int runWidth = span.pageXEnd - span.pageXBegin + 1;
        if (runWidth <= 0) {
            continue;
        }
        const int sourceX = static_cast<int>(span.pageIndex * pageWidth)
            + span.pageXBegin;
        const double targetX =
            drawX
            + static_cast<double>(contentXOffset)
            + static_cast<double>(span.displayLeft - displayLeft) * targetScale;
        plan.segments.push_back(RetainedRenderSegment{
            QRect(sourceX, 0, runWidth, m_precomputedBinsPerColumn),
            QRectF(
                targetX,
                0.0,
                static_cast<double>(runWidth) * targetScale,
                static_cast<double>(height)),
            true,
        });
        plan.haveSegments = true;
        expectedDisplayLeft = span.displayRight + 1;
    }
    if (expectedDisplayLeft <= scanRight) {
        plan.coversRange = false;
    }
    return plan;
}

bool SpectrogramItem::retainedSourceTextureStateChangedLocked(
    QQuickWindow *window,
    const QSize &retainedTextureSize,
    const bool rollingMode) const {
    return !m_gpuSourceTexture.matches(window, retainedTextureSize)
        || m_gpuSourceTextureTrackToken != m_precomputedTrackToken
        || m_gpuSourceTextureRolling != rollingMode;
}

bool SpectrogramItem::retainedSourcePagesCoverDisplayRangeLocked(
    const qint64 displayLeft,
    const qint64 displayRight,
    const qint64 renderableRight,
    const bool rollingMode,
    const int pageWidth,
    const bool requireTextures) const {
    const QVector<RetainedPageSpan> pageSpans =
        buildRetainedPageSpansLocked(displayLeft, displayRight, renderableRight, rollingMode, pageWidth);
    if (pageSpans.isEmpty()) {
        return displayRight < displayLeft || renderableRight < displayLeft;
    }

    qint64 expectedDisplayLeft = displayLeft;
    for (const RetainedPageSpan &span : pageSpans) {
        if (span.displayLeft > expectedDisplayLeft) {
            return false;
        }
        const RetainedSourcePageState *page = findRetainedSourcePageLocked(
            m_precomputedTrackToken,
            m_precomputedSessionGeneration,
            rollingMode,
            span.pageIndex);
        if (page == nullptr
            || span.pageXBegin < page->pageXOrigin
            || span.pageXEnd >= page->pageXOrigin + page->width
            || page->binsPerColumn != m_precomputedBinsPerColumn
            || page->uploadedDisplayLeft > span.displayLeft
            || page->uploadedDisplayRight < span.displayRight
            || (requireTextures && page->texture.texture() == nullptr)) {
            return false;
        }
        expectedDisplayLeft = span.displayRight + 1;
    }

    const qint64 scanRight = std::min(displayRight, renderableRight);
    return expectedDisplayLeft > scanRight;
}

bool SpectrogramItem::shouldSyncLegacyRetainedSourceTextureLocked(
    QQuickWindow *window,
    const SpectrogramRetainedSourceLayout &layout,
    const bool retainedPagesCoverVisibleRange) const {
    Q_UNUSED(window);
    Q_UNUSED(layout);
    Q_UNUSED(retainedPagesCoverVisibleRange);
    return false;
}

QVector<SpectrogramSourceUploadSpan> SpectrogramItem::collectRetainedSourceUploadSpansLocked(
    const qint64 displayLeft,
    const qint64 displayRight,
    const bool rollingMode,
    const bool forceFullUpload) const {
    QVector<SpectrogramSourceUploadSpan> spans;
    if (m_ringCapacity <= 0
        || m_precomputedBinsPerColumn <= 0
        || displayRight < displayLeft) {
        return spans;
    }

    auto appendDisplayRange = [&](const qint64 rangeLeft, const qint64 rangeRight) {
        if (rangeRight < rangeLeft) {
            return;
        }

        int slotRunBegin = -1;
        int slotRunEnd = -1;
        for (qint64 displayIndex = rangeLeft; displayIndex <= rangeRight; ++displayIndex) {
            const int slot = ringSlotForDisplayIndexLocked(displayIndex, rollingMode);
            if (slot < 0) {
                if (slotRunBegin >= 0) {
                    spans.push_back({slotRunBegin, slotRunEnd});
                    slotRunBegin = -1;
                    slotRunEnd = -1;
                }
                continue;
            }

            if (slotRunBegin < 0) {
                slotRunBegin = slot;
                slotRunEnd = slot;
                continue;
            }

            if (slot == slotRunEnd + 1) {
                slotRunEnd = slot;
                continue;
            }

            spans.push_back({slotRunBegin, slotRunEnd});
            slotRunBegin = slot;
            slotRunEnd = slot;
        }

        if (slotRunBegin >= 0) {
            spans.push_back({slotRunBegin, slotRunEnd});
        }
    };

    if (forceFullUpload) {
        appendDisplayRange(displayLeft, displayRight);
        return mergeSpectrogramSourceUploadSpans(spans);
    }

    if (rollingMode) {
        const bool canIncrementallyAdvance =
            m_gpuSourceTextureDisplayRight >= m_gpuSourceTextureDisplayLeft
            && displayLeft >= m_gpuSourceTextureDisplayLeft
            && displayRight >= m_gpuSourceTextureDisplayRight;
        if (!canIncrementallyAdvance) {
            appendDisplayRange(displayLeft, displayRight);
        } else {
            appendDisplayRange(
                std::max(displayLeft, m_gpuSourceTextureDisplayRight + 1),
                displayRight);
        }
    } else {
        const bool haveUploadedColumnRange =
            m_gpuSourceTextureMaxColumnIndex >= m_gpuSourceTextureMinColumnIndex;
        if (!haveUploadedColumnRange) {
            appendDisplayRange(displayLeft, displayRight);
        } else {
            if (displayLeft < m_gpuSourceTextureMinColumnIndex) {
                appendDisplayRange(
                    displayLeft,
                    std::min(displayRight, m_gpuSourceTextureMinColumnIndex - 1));
            }
            if (displayRight > m_gpuSourceTextureMaxColumnIndex) {
                appendDisplayRange(
                    std::max(displayLeft, m_gpuSourceTextureMaxColumnIndex + 1),
                    displayRight);
            }
        }
    }

    return mergeSpectrogramSourceUploadSpans(spans);
}

void SpectrogramItem::syncRetainedSourcePagesLocked(
    QQuickWindow *window,
    const qint64 displayLeft,
    const qint64 displayRight,
    const qint64 renderableRight,
    const bool rollingMode) {
    m_gpuSourcePageResetReason = RetainedPageResetReason::None;
    if (window == nullptr
        || m_ringCapacity <= 0
        || m_precomputedBinsPerColumn <= 0
        || displayRight < displayLeft) {
        return;
    }
    m_gpuSourcePageUploadSpanCount = 0;
    m_gpuSourcePageUploadColumns = 0;
    m_gpuSourcePageTextureReset = false;

    const int pageWidth = retainedSourcePageWidthForGpuLimit(window);
    if (m_precomputedBinsPerColumn > gpuMaxTextureColumns(window)) {
        m_gpuSourcePages.clear();
        return;
    }
    if (std::min(displayRight, renderableRight) < displayLeft) {
        return;
    }

    const QVector<RetainedPageSpan> visiblePages =
        buildRetainedPageSpansLocked(displayLeft, displayRight, renderableRight, rollingMode, pageWidth);
    const QVector<RetainedPageUploadSpan> plannedUploadSpans =
        collectRetainedPageUploadSpansLocked(displayLeft, displayRight, renderableRight, rollingMode, pageWidth);
    bool uploadBudgetExhausted = false;
    const QVector<RetainedPageUploadSpan> uploadSpans =
        retainedPageUploadSpansForPaintBudget(
            plannedUploadSpans,
            kRetainedUploadColumnsPerPaint,
            &uploadBudgetExhausted);
    const QVector<qint64> currentPageIndicesToKeep =
        retainedSourcePageIndicesToKeepLocked(visiblePages, rollingMode, pageWidth);
    const QVector<qint64> frozenPageIndicesToKeep =
        m_zoomFillFrozenRetainedAvailable
        ? retainedFrozenPageIndicesToKeepForZoomFill(
              m_zoomFillFrozenRetainedSegments,
              pageWidth)
        : QVector<qint64>();

    std::erase_if(m_gpuSourcePages, [&](const auto &page) {
        if (page == nullptr
            || page->key.rollingMode != rollingMode) {
            return true;
        }
        if (!rollingMode && page->key.trackToken != m_precomputedTrackToken) {
            return true;
        }
        const bool currentGenerationPage =
            page->key.generation == m_precomputedSessionGeneration
            && currentPageIndicesToKeep.contains(page->key.pageIndex);
        const bool frozenGenerationPage =
            m_zoomFillFrozenRetainedAvailable
            && page->key.generation == m_zoomFillFrozenRetainedGeneration
            && frozenPageIndicesToKeep.contains(page->key.pageIndex);
        return !currentGenerationPage && !frozenGenerationPage;
    });

    const auto ensurePage = [&](const qint64 pageIndex) -> RetainedSourcePageState * {
        if (RetainedSourcePageState *existing = findRetainedSourcePageLocked(
                m_precomputedTrackToken,
                m_precomputedSessionGeneration,
                rollingMode,
                pageIndex)) {
            return existing;
        }
        auto page = std::make_unique<RetainedSourcePageState>();
        page->key = RetainedSourcePageKey{
            .trackToken = m_precomputedTrackToken,
            .generation = m_precomputedSessionGeneration,
            .rollingMode = rollingMode,
            .pageIndex = pageIndex,
        };
        RetainedSourcePageState *rawPage = page.get();
        m_gpuSourcePages.push_back(std::move(page));
        return rawPage;
    };

    const auto *ringData =
        reinterpret_cast<const quint8 *>(m_ringBuffer.constData());
    if (ringData == nullptr) {
        return;
    }

    for (const RetainedPageUploadSpan &span : uploadSpans) {
        RetainedSourcePageState *page = ensurePage(span.pageIndex);
        if (page == nullptr) {
            continue;
        }
        if (span.resetPage) {
            const RetainedPageTextureExtent extent = retainedPageTextureExtentForSpan(
                span.pageXBegin,
                span.pageXEnd,
                pageWidth);
            page->texture.release();
            page->pageXOrigin = extent.xOrigin;
            page->width = extent.width;
            page->uploadedDisplayLeft = 0;
            page->uploadedDisplayRight = -1;
            m_gpuSourcePageTextureReset = true;
            if (m_gpuSourcePageResetReason == RetainedPageResetReason::None) {
                m_gpuSourcePageResetReason = span.resetReason;
            }
        }
        const bool hadPageTexture = page->texture.texture() != nullptr;
        if (!page->texture.ensureCreated(window, page->width, m_precomputedBinsPerColumn)) {
            continue;
        }
        m_gpuSourcePageTextureReset =
            m_gpuSourcePageTextureReset
            || (!hadPageTexture && page->texture.texture() != nullptr);
        if (!hadPageTexture
            && page->texture.texture() != nullptr
            && m_gpuSourcePageResetReason == RetainedPageResetReason::None) {
            m_gpuSourcePageResetReason = RetainedPageResetReason::UploadReset;
        }
        page->binsPerColumn = m_precomputedBinsPerColumn;

        const int spanWidth = span.pageXEnd - span.pageXBegin + 1;
        if (spanWidth <= 0) {
            continue;
        }
        const int textureXBegin = span.pageXBegin - page->pageXOrigin;
        if (textureXBegin < 0 || textureXBegin + spanWidth > page->width) {
            continue;
        }

        m_gpuSourceUploadRgba.resize(spanWidth * m_precomputedBinsPerColumn * 4);
        m_gpuSourceUploadRgba.fill('\0');
        for (int bin = 0; bin < m_precomputedBinsPerColumn; ++bin) {
            uchar *row = reinterpret_cast<uchar *>(m_gpuSourceUploadRgba.data())
                + static_cast<qsizetype>(bin) * spanWidth * 4;
            for (int localX = 0; localX < spanWidth; ++localX) {
                const int slot = static_cast<int>(span.pageIndex) * pageWidth
                    + span.pageXBegin + localX;
                const quint8 value = ringData[slot * m_precomputedBinsPerColumn + bin];
                writeRgba8888GrayPixel(row, localX, value);
            }
        }

        if (!page->texture.uploadRgbaColumns(
                window,
                textureXBegin,
                spanWidth,
                m_precomputedBinsPerColumn,
                m_gpuSourceUploadRgba)) {
            continue;
        }
        const SpectrogramSourceUploadSpan uploadedSpan{
            textureXBegin,
            textureXBegin + spanWidth - 1,
        };
        page->texture.noteUploadedSpan(uploadedSpan);
        page->dirtySpans.push_back(uploadedSpan);
        m_gpuSourcePageUploadSpanCount += 1;
        m_gpuSourcePageUploadColumns += spanWidth;
        if (page->uploadedDisplayRight < page->uploadedDisplayLeft) {
            page->uploadedDisplayLeft = span.displayLeft;
            page->uploadedDisplayRight = span.displayRight;
        } else {
            page->uploadedDisplayLeft = std::min(page->uploadedDisplayLeft, span.displayLeft);
            page->uploadedDisplayRight = std::max(page->uploadedDisplayRight, span.displayRight);
        }
    }
    if (uploadBudgetExhausted) {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                update();
            },
            Qt::QueuedConnection);
    }
}

const SpectrogramItem::SpectrogramLodPage *SpectrogramItem::findSpectrogramLodPageLocked(
    const int level) const {
    if (level <= 0 || m_precomputedTrackToken == 0) {
        return nullptr;
    }
    const SpectrogramLodPage *selected = nullptr;
    for (const SpectrogramLodPage &page : m_spectrogramLodPages) {
        if (page.level != level
            || page.trackToken != m_precomputedTrackToken
            || page.binsPerColumn != m_precomputedBinsPerColumn
            || page.startColumn > page.endColumn
            || page.bytes.isEmpty()) {
            continue;
        }
        if (selected == nullptr || page.generation >= selected->generation) {
            selected = &page;
        }
    }
    return selected;
}

SpectrogramItem::SpectrogramLodRenderState SpectrogramItem::syncSpectrogramLodTextureLocked(
    QQuickWindow *window,
    const qint64 displayLeft,
    const qint64 displayRight,
    const double effectiveZoom) {
    SpectrogramLodRenderState renderState;
    if (window == nullptr || displayRight < displayLeft || m_precomputedBinsPerColumn <= 0) {
        invalidateSpectrogramLodTextureStateLocked();
        return renderState;
    }

    QVector<int> availableLevels;
    availableLevels.push_back(0);
    for (const SpectrogramLodPage &page : m_spectrogramLodPages) {
        if (!availableLevels.contains(page.level)) {
            availableLevels.push_back(page.level);
        }
    }
    std::sort(availableLevels.begin(), availableLevels.end());
    const SpectrogramLodSelection selection =
        selectSpectrogramLodLevelForTests(effectiveZoom, availableLevels);
    renderState.primaryLevel = selection.primaryLevel;
    renderState.secondaryLevel = selection.secondaryLevel;
    renderState.blend = static_cast<float>(selection.blend);
    if (selection.primaryLevel <= 0) {
        invalidateSpectrogramLodTextureStateLocked();
        return renderState;
    }

    const auto syncPageTexture =
        [&](const SpectrogramLodPage *page,
            SpectrogramSourceTexture &texture,
            QByteArray &uploadRgba,
            int &cachedLevel,
            quint64 &cachedTrackToken,
            quint64 &cachedGeneration,
            qint64 &cachedStartColumn,
            qint64 &cachedEndColumn,
            QSGTexture **outTexture,
            QSize *outTextureSize,
            QRectF *outSourceRect,
            int *outPlaneCount) -> bool {
        if (page == nullptr || page->planeCount <= 0 || page->sourceSpan <= 0) {
            return false;
        }

        const qint64 pageColumns = page->endColumn - page->startColumn + 1;
        if (pageColumns <= 0 || pageColumns > std::numeric_limits<int>::max()) {
            return false;
        }
        const qsizetype expectedBytes =
            static_cast<qsizetype>(pageColumns)
            * page->binsPerColumn
            * page->planeCount;
        if (page->bytes.size() < expectedBytes) {
            return false;
        }

        const QSize textureSize(
            static_cast<int>(pageColumns),
            page->binsPerColumn * page->planeCount);
        const bool textureStateChanged =
            !texture.matches(window, textureSize)
            || cachedLevel != page->level
            || cachedTrackToken != page->trackToken
            || cachedGeneration != page->generation
            || cachedStartColumn != page->startColumn
            || cachedEndColumn != page->endColumn;
        if (textureStateChanged) {
            if (!texture.ensureCreated(window, textureSize.width(), textureSize.height())) {
                return false;
            }
            uploadRgba.resize(textureSize.width() * textureSize.height() * 4);
            uploadRgba.fill('\0');
        const auto *source = reinterpret_cast<const quint8 *>(page->bytes.constData());
        for (int plane = 0; plane < page->planeCount; ++plane) {
            for (int bin = 0; bin < page->binsPerColumn; ++bin) {
                const int textureRow = plane * page->binsPerColumn + bin;
                uchar *row = reinterpret_cast<uchar *>(uploadRgba.data())
                    + static_cast<qsizetype>(textureRow) * textureSize.width() * 4;
                for (int x = 0; x < textureSize.width(); ++x) {
                    const qsizetype sourceOffset =
                        (static_cast<qsizetype>(x) * page->binsPerColumn + bin)
                        * page->planeCount
                        + plane;
                    writeRgba8888GrayPixel(row, x, source[sourceOffset]);
                }
            }
        }
        if (!texture.uploadRgbaColumns(
                window,
                0,
                textureSize.width(),
                textureSize.height(),
                uploadRgba)) {
            return false;
        }
        cachedLevel = page->level;
        cachedTrackToken = page->trackToken;
        cachedGeneration = page->generation;
        cachedStartColumn = page->startColumn;
        cachedEndColumn = page->endColumn;
        }

    const double lodLeft =
        static_cast<double>(displayLeft) / static_cast<double>(page->sourceSpan)
        - static_cast<double>(page->startColumn);
    const double lodWidth =
        static_cast<double>(displayRight - displayLeft + 1)
        / static_cast<double>(page->sourceSpan);
        if (outTexture != nullptr) {
            *outTexture = texture.texture();
        }
        if (outTextureSize != nullptr) {
            *outTextureSize = textureSize;
        }
        if (outSourceRect != nullptr) {
            *outSourceRect = QRectF(lodLeft, 0.0, lodWidth, page->binsPerColumn);
        }
        if (outPlaneCount != nullptr) {
            *outPlaneCount = page->planeCount;
        }
        return true;
    };

    const SpectrogramLodPage *primaryPage = findSpectrogramLodPageLocked(selection.primaryLevel);
    if (!syncPageTexture(
            primaryPage,
            m_gpuLodSourceTexture,
            m_gpuLodUploadRgba,
            m_gpuLodTextureLevel,
            m_gpuLodTextureTrackToken,
            m_gpuLodTextureGeneration,
            m_gpuLodTextureStartColumn,
            m_gpuLodTextureEndColumn,
            &renderState.texture,
            &renderState.textureSize,
            &renderState.sourceRect,
            &renderState.primaryPlaneCount)) {
        invalidateSpectrogramLodTextureStateLocked();
        return renderState;
    }

    const SpectrogramLodPage *secondaryPage = findSpectrogramLodPageLocked(selection.secondaryLevel);
    if (selection.secondaryLevel != selection.primaryLevel
        && syncPageTexture(
            secondaryPage,
            m_gpuSecondaryLodSourceTexture,
            m_gpuSecondaryLodUploadRgba,
            m_gpuSecondaryLodTextureLevel,
            m_gpuSecondaryLodTextureTrackToken,
            m_gpuSecondaryLodTextureGeneration,
            m_gpuSecondaryLodTextureStartColumn,
            m_gpuSecondaryLodTextureEndColumn,
            &renderState.secondaryTexture,
            &renderState.secondaryTextureSize,
            &renderState.secondarySourceRect,
            &renderState.secondaryPlaneCount)) {
        return renderState;
    }

    renderState.secondaryTexture = renderState.texture;
    renderState.secondaryTextureSize = renderState.textureSize;
    renderState.secondarySourceRect = renderState.sourceRect;
    renderState.secondaryLevel = renderState.primaryLevel;
    renderState.secondaryPlaneCount = renderState.primaryPlaneCount;
    renderState.blend = 0.0F;
    return renderState;
}

void SpectrogramItem::syncRetainedSourceTextureLocked(
    QQuickWindow *window,
    const qint64 displayLeft,
    const qint64 displayRight,
    const bool rollingMode) {
    m_gpuSourceUploadSpans.clear();
    m_gpuSourcePageUploadSpanCount = 0;
    m_gpuSourcePageUploadColumns = 0;
    m_gpuSourcePageTextureReset = false;
    if (window == nullptr
        || m_ringCapacity <= 0
        || m_precomputedBinsPerColumn <= 0
        || displayRight < displayLeft) {
        return;
    }

    const SpectrogramRetainedSourceLayout layout =
        buildRetainedSourceLayoutLocked(displayLeft, displayRight, rollingMode);
    const qint64 pageRenderableRight = rollingMode
        ? displayRight
        : std::min(displayRight, static_cast<qint64>(m_precomputedMaxColumnIndex));
    syncRetainedSourcePagesLocked(
        window,
        displayLeft,
        displayRight,
        pageRenderableRight,
        rollingMode);
    const bool retainedPagesCoverVisibleRange =
        retainedSourcePagesCoverDisplayRangeLocked(
            displayLeft,
            displayRight,
            pageRenderableRight,
            rollingMode,
            retainedSourcePageWidthForGpuLimit(window),
            true);
    if (!shouldSyncLegacyRetainedSourceTextureLocked(
            window,
            layout,
            retainedPagesCoverVisibleRange)) {
        invalidateLegacyRetainedSourceTextureStateLocked();
        return;
    }
    const QSize retainedTextureSize(layout.capacityColumns, layout.binsPerColumn);
    const bool textureStateChanged =
        retainedSourceTextureStateChangedLocked(window, retainedTextureSize, rollingMode);
    if (!m_gpuSourceTexture.ensureCreated(window, layout.capacityColumns, layout.binsPerColumn)) {
        return;
    }

    const auto noteVisibleColumnsUploaded = [&]() {
        if (rollingMode) {
            m_gpuSourceTextureMinColumnIndex = 0;
            m_gpuSourceTextureMaxColumnIndex = m_precomputedMaxColumnIndex;
            return;
        }

        const qint64 decodedVisibleRight =
            std::min(displayRight, static_cast<qint64>(m_precomputedMaxColumnIndex));
        if (decodedVisibleRight < displayLeft) {
            return;
        }

        if (textureStateChanged
            || m_gpuSourceTextureMaxColumnIndex < m_gpuSourceTextureMinColumnIndex) {
            m_gpuSourceTextureMinColumnIndex = displayLeft;
            m_gpuSourceTextureMaxColumnIndex = decodedVisibleRight;
            return;
        }

        m_gpuSourceTextureMinColumnIndex =
            std::min(m_gpuSourceTextureMinColumnIndex, displayLeft);
        m_gpuSourceTextureMaxColumnIndex =
            std::max(m_gpuSourceTextureMaxColumnIndex, decodedVisibleRight);
    };

    m_gpuSourceTexture.clearPendingSpans();
    if (textureStateChanged) {
        m_gpuSourceUploadRgba.resize(layout.capacityColumns * layout.binsPerColumn * 4);
        m_gpuSourceUploadRgba.fill('\0');
        if (!m_gpuSourceTexture.uploadRgbaColumns(
                window,
                0,
                layout.capacityColumns,
                layout.binsPerColumn,
                m_gpuSourceUploadRgba)) {
            return;
        }
    }

    m_gpuSourceUploadSpans = collectRetainedSourceUploadSpansLocked(
        displayLeft,
        displayRight,
        rollingMode,
        textureStateChanged);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_zoomFillActive || m_zoomFillDataReady) {
        const int remainingLogs = g_zoomFillSourceUploadLogBudget.fetch_sub(1);
        if (remainingLogs > 0) {
            int spanColumns = 0;
            int firstSpanBegin = -1;
            int firstSpanEnd = -1;
            for (const SpectrogramSourceUploadSpan &span : m_gpuSourceUploadSpans) {
                if (firstSpanBegin < 0) {
                    firstSpanBegin = span.xBegin;
                    firstSpanEnd = span.xEnd;
                }
                spanColumns += std::max(0, span.xEnd - span.xBegin + 1);
            }
            const qint64 decodedVisibleRight = rollingMode
                ? displayRight
                : std::min(displayRight, static_cast<qint64>(m_precomputedMaxColumnIndex));
            FERROUS_SPECTROGRAM_PROFILE_LOGF(stderr,
                "[ui-spectrogram] source_upload_gate zoom_fill=%d ready=%d texture_reset=%d "
                "spans=%lld span_cols=%d first_span=%d..%d display=%lld..%lld decoded_right=%lld "
                "gpu_source_range=%lld..%lld gpu_source_token=%llu current_token=%llu "
                "ring=%lld..%lld max_col=%d texture_size=%dx%d\n",
                m_zoomFillActive ? 1 : 0,
                m_zoomFillDataReady ? 1 : 0,
                textureStateChanged ? 1 : 0,
                static_cast<long long>(m_gpuSourceUploadSpans.size()),
                spanColumns,
                firstSpanBegin,
                firstSpanEnd,
                static_cast<long long>(displayLeft),
                static_cast<long long>(displayRight),
                static_cast<long long>(decodedVisibleRight),
                static_cast<long long>(m_gpuSourceTextureMinColumnIndex),
                static_cast<long long>(m_gpuSourceTextureMaxColumnIndex),
                static_cast<unsigned long long>(m_gpuSourceTextureTrackToken),
                static_cast<unsigned long long>(m_precomputedTrackToken),
                static_cast<long long>(m_ringOldestSeq),
                static_cast<long long>(m_ringWriteSeq),
                static_cast<int>(m_precomputedMaxColumnIndex),
                m_gpuSourceTexture.texture() != nullptr
                    ? m_gpuSourceTexture.texture()->textureSize().width()
                    : 0,
                m_gpuSourceTexture.texture() != nullptr
                    ? m_gpuSourceTexture.texture()->textureSize().height()
                    : 0);
        }
    }
#endif
    if (m_gpuSourceUploadSpans.isEmpty()) {
        noteVisibleColumnsUploaded();
        m_gpuSourceTextureDisplayLeft = displayLeft;
        m_gpuSourceTextureDisplayRight = displayRight;
        m_gpuSourceTextureRolling = rollingMode;
        m_gpuSourceTextureTrackToken = m_precomputedTrackToken;
        m_gpuSourceTextureRingWriteSeq = m_ringWriteSeq;
        m_gpuSourceTextureRingOldestSeq = m_ringOldestSeq;
        return;
    }

    const auto *ringData =
        reinterpret_cast<const quint8 *>(m_ringBuffer.constData());
    if (ringData == nullptr) {
        return;
    }

    for (const SpectrogramSourceUploadSpan &span : m_gpuSourceUploadSpans) {
        const int spanWidth = span.xEnd - span.xBegin + 1;
        if (spanWidth <= 0) {
            continue;
        }

        m_gpuSourceUploadRgba.resize(spanWidth * layout.binsPerColumn * 4);
        m_gpuSourceUploadRgba.fill('\0');
        for (int bin = 0; bin < layout.binsPerColumn; ++bin) {
            uchar *row = reinterpret_cast<uchar *>(m_gpuSourceUploadRgba.data())
                + static_cast<qsizetype>(bin) * spanWidth * 4;
            for (int localX = 0; localX < spanWidth; ++localX) {
                const int slot = span.xBegin + localX;
                const quint8 value = ringData[slot * layout.binsPerColumn + bin];
                writeRgba8888GrayPixel(row, localX, value);
            }
        }

        if (!m_gpuSourceTexture.uploadRgbaColumns(
                window,
                span.xBegin,
                spanWidth,
                layout.binsPerColumn,
                m_gpuSourceUploadRgba)) {
            return;
        }
        m_gpuSourceTexture.noteUploadedSpan(span);
    }

    noteVisibleColumnsUploaded();
    m_gpuSourceTextureDisplayLeft = displayLeft;
    m_gpuSourceTextureDisplayRight = displayRight;
    m_gpuSourceTextureRolling = rollingMode;
    m_gpuSourceTextureTrackToken = m_precomputedTrackToken;
    m_gpuSourceTextureRingWriteSeq = m_ringWriteSeq;
    m_gpuSourceTextureRingOldestSeq = m_ringOldestSeq;
}

void SpectrogramItem::syncPrecomputedGpuLookupImagesLocked() {
    const bool dbRemapDirty =
        m_gpuDbRemapImage.isNull()
        || m_gpuDbRemapBins != m_precomputedBinsPerColumn
        || std::abs(m_gpuDbRemapDbRange - m_dbRange) > 0.001;
    if (dbRemapDirty) {
        m_gpuDbRemapImage = buildPrecomputedDbRemapImageLocked();
        m_gpuDbRemapBins = m_precomputedBinsPerColumn;
        m_gpuDbRemapDbRange = m_dbRange;
        m_gpuDbRemapGeneration += 1;
    }

    const bool paletteDirty =
        m_gpuPaletteImage.isNull()
        || m_gpuPaletteMuted != m_channelMuted;
    if (paletteDirty) {
        m_gpuPaletteImage = buildPrecomputedPaletteImageLocked();
        m_gpuPaletteMuted = m_channelMuted;
        m_gpuPaletteGeneration += 1;
    }
}

void SpectrogramItem::syncGpuRowLutImageLocked(const int height) {
    const bool rowLutDirty =
        height <= 0
        || m_binsPerColumn <= 0
        || static_cast<int>(m_iToBin.size()) != height
        || m_gpuRowLutImage.isNull()
        || m_gpuRowLutHeight != height
        || m_gpuRowLutBins != m_binsPerColumn;
    if (!rowLutDirty) {
        return;
    }

    m_gpuRowLutImage = buildRowLutImageLocked(height);
    m_gpuRowLutHeight = height;
    m_gpuRowLutBins = m_binsPerColumn;
    m_gpuRowLutGeneration += 1;
}

int SpectrogramItem::gpuDrawWidthForSourceColumnsLocked(
    int width,
    qint64 displayLeft,
    qint64 displayRight,
    double effectiveZoom) const {
    const qint64 sourceColumns = std::max<qint64>(0, displayRight - displayLeft + 1);
    int drawPixels = std::min(width,
        static_cast<int>(std::ceil(
            static_cast<double>(sourceColumns) * effectiveZoom)));
    if (drawPixels < width
        && displayLeft == 0
        && sourceColumns + 5 >= static_cast<qint64>(width)) {
        drawPixels = width;
    }
    return drawPixels;
}

void SpectrogramItem::paintPrecomputedDisplayColumnsLocked(
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
    const QRgb *palette) const {
    if (image.isNull()
        || xBegin > xEnd
        || drawPixels <= 0
        || height <= 0
        || m_precomputedBinsPerColumn <= 0
        || displayRight < displayLeft
        || palette == nullptr) {
        return;
    }

    const int sourceColumns = static_cast<int>(displayRight - displayLeft + 1);
    filledPixels = std::clamp(filledPixels, 0, drawPixels);
    const int contentXOffset =
        rollingMode ? std::max(0, drawPixels - filledPixels) : 0;
    const int sourceBins = m_precomputedBinsPerColumn;
    const double gpuEffectiveZoom =
        static_cast<double>(filledPixels) / static_cast<double>(std::max(1, sourceColumns));
    const int maxBin = std::max(0, sourceBins - 1);
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

    auto sampleColumnRangeFromRing =
        [this, sourceBins, sourceColumns, displayLeft, displayRight, rollingMode](
            int columnFirst,
            int columnLast,
            int binLo,
            int binHi) -> double {
            const int clampedFirst = std::clamp(columnFirst, 0, sourceColumns - 1);
            const int clampedLast = std::clamp(columnLast, clampedFirst, sourceColumns - 1);
            const int clampedBinLo = std::clamp(binLo, 0, sourceBins - 1);
            const int clampedBinHi = std::clamp(binHi, clampedBinLo, sourceBins - 1);
            quint8 rawMax = 0;
            for (int column = clampedFirst; column <= clampedLast; ++column) {
                const qint64 displayIndex = displayLeft + static_cast<qint64>(column);
                if (displayIndex < displayLeft || displayIndex > displayRight) {
                    continue;
                }
                const int slot = ringSlotForDisplayIndexLocked(displayIndex, rollingMode);
                if (slot < 0) {
                    continue;
                }
                const int baseOff = slot * sourceBins;
                for (int bin = clampedBinLo; bin <= clampedBinHi; ++bin) {
                    rawMax = std::max(rawMax, static_cast<quint8>(m_ringBuffer[baseOff + bin]));
                }
            }
            return static_cast<double>(rawMax);
        };

    const int clampedXBegin = std::clamp(xBegin, 0, drawPixels - 1);
    const int clampedXEnd = std::clamp(xEnd, clampedXBegin, drawPixels - 1);
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

        const int target = m_logScale && static_cast<int>(m_iToBin.size()) == height
            ? m_iToBin[static_cast<size_t>(i)]
            : -1;
        int nextBin = index1;
        int span = 0;
        int leadingCount = 0;
        if (m_logScale
            && static_cast<int>(m_iToBin.size()) == height
            && i <= m_lowResEnd) {
            int j = 0;
            while (i + j < height && m_iToBin[static_cast<size_t>(i + j)] == target) {
                ++j;
            }

            const int nextI = std::min(i + j, height - 1);
            nextBin = std::clamp(m_iToBin[static_cast<size_t>(nextI)], 0, maxBin);
            span = j;
            int k = 0;
            while (i + k >= 0 && m_iToBin[static_cast<size_t>(i + k)] == target) {
                ++span;
                --k;
            }
            leadingCount = (-k) - 1;
        }

        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = clampedXBegin; x <= clampedXEnd; ++x) {
            const int contentX = x - contentXOffset;
            if (contentX < 0 || contentX >= filledPixels) {
                line[x] = qRgba(0, 0, 0, 255);
                continue;
            }
            const SpectrogramGpu::ColumnSpan spanInfo =
                SpectrogramGpu::resolveColumnSpanForPixel(
                    contentX,
                    sourceColumns,
                    gpuEffectiveZoom);
            double intensity = 0.0;
            if (spanInfo.firstColumn >= 0) {
                if (spanInfo.interpolate) {
                    const double leftRaw = sampleColumnRangeFromRing(
                        spanInfo.firstColumn,
                        spanInfo.firstColumn,
                        index0,
                        index1);
                    const double rightRaw = sampleColumnRangeFromRing(
                        spanInfo.lastColumn,
                        spanInfo.lastColumn,
                        index0,
                        index1);
                    intensity = linearInterpolate(
                        leftRaw,
                        rightRaw,
                        std::clamp(spanInfo.interpolationT, 0.0, 1.0));
                } else {
                    intensity = sampleColumnRangeFromRing(
                        spanInfo.firstColumn,
                        spanInfo.lastColumn,
                        index0,
                        index1);
                }
            }

            if (span > 1) {
                const double v1 = sampleColumnRangeFromRing(
                    std::max(0, spanInfo.firstColumn),
                    std::max(0, spanInfo.lastColumn),
                    nextBin,
                    nextBin);
                const double mu = (1.0 / static_cast<double>(span - 1))
                    * static_cast<double>(leadingCount);
                intensity = linearInterpolate(intensity, v1, std::clamp(mu, 0.0, 1.0));
            }

            const int remappedIndex = dbRemap[static_cast<size_t>(std::clamp(
                static_cast<int>(std::lround(intensity)),
                0,
                255))];
            const int paletteIndex = kGradientTableSize - 1
                - static_cast<int>(std::lround(
                    (static_cast<double>(kGradientTableSize - 1) / 255.0)
                    * static_cast<double>(remappedIndex)));
            line[x] = palette[static_cast<size_t>(std::clamp(
                paletteIndex,
                0,
                kGradientTableSize - 1))];
        }
    }
}

void SpectrogramItem::syncPrecomputedDisplayImageLocked(
    int drawPixels,
    int height,
    qint64 displayLeft,
    qint64 displayRight,
    bool rollingMode,
    double effectiveZoom,
    bool maintainImagePixels) {
    if (drawPixels <= 0
        || height <= 0
        || displayRight < displayLeft
        || m_precomputedBinsPerColumn <= 0) {
        const bool hadGpuDisplayImage = !m_gpuDisplayImage.isNull();
        m_gpuDisplayImage = QImage();
        if (hadGpuDisplayImage) {
            bumpImageGeneration(m_gpuDisplayGeneration);
        }
        m_gpuDisplaySourceGeneration = 0;
        m_gpuDisplayWidth = 0;
        m_gpuDisplayFilledWidth = 0;
        m_gpuDisplayHeight = 0;
        m_gpuDisplaySampleRateHz = 0;
        m_gpuDisplayBins = 0;
        m_gpuDisplayRingWriteSeq = -1;
        m_gpuDisplayRingOldestSeq = -1;
        m_gpuDisplayMaxColumnIndex = -1;
        m_gpuDisplayTrackToken = 0;
        m_gpuDisplayLastUpdateWasIncremental = false;
        m_gpuDisplayLastShiftPixels = 0;
        m_gpuDisplayLastUpdatedColumns = 0;
        m_gpuDisplaySubpixelOffset = 0.0;
        return;
    }

    m_precomputedCanvasDisplayLeft = displayLeft;
    m_precomputedCanvasDisplayRight = displayRight;
    m_precomputedCanvasRolling = rollingMode;
    m_precomputedCanvasZoomLevel = effectiveZoom;
    m_precomputedCanvasHopSize = m_precomputedHopSize;
    m_precomputedCanvasTotalColumnsEstimate = m_precomputedTotalColumnsEstimate;
    m_precomputedCanvasDirty = false;
    const int filledPixels = std::min(
        drawPixels,
        static_cast<int>(std::ceil(
            static_cast<double>(displayRight - displayLeft + 1) * effectiveZoom)));

    const bool displayConfigDirty =
        (maintainImagePixels && m_gpuDisplayImage.isNull())
        || m_gpuDisplayWidth != drawPixels
        || m_gpuDisplayHeight != height
        || m_gpuDisplaySampleRateHz != m_sampleRateHz
        || m_gpuDisplayBins != m_precomputedBinsPerColumn
        || std::abs(m_gpuDisplayDbRange - m_dbRange) > 0.001
        || m_gpuDisplayMuted != m_channelMuted
        || m_gpuDisplayLogScale != m_logScale;
    const bool displayWindowChanged =
        m_gpuDisplayDisplayLeft != displayLeft
        || m_gpuDisplayDisplayRight != displayRight
        || m_gpuDisplayRolling != rollingMode
        || m_gpuDisplayTrackToken != m_precomputedTrackToken;
    const bool displayZoomChanged =
        std::abs(effectiveZoom - m_gpuDisplayEffectiveZoom) > 0.001;
    const bool centeredVisibleCoverageIncomplete =
        !rollingMode && m_gpuDisplayMaxColumnIndex < displayRight;
    const bool centeredVisibleGrowthDirty =
        centeredVisibleCoverageIncomplete
        && m_gpuDisplayMaxColumnIndex != m_precomputedMaxColumnIndex;
    const bool contentDirty = rollingMode
        ? (displayWindowChanged || displayZoomChanged)
        : (displayWindowChanged || displayZoomChanged || centeredVisibleGrowthDirty);
    if (!displayConfigDirty && !contentDirty) {
        if (!maintainImagePixels && !m_gpuDisplayImage.isNull()) {
            m_gpuDisplayImage = QImage();
            bumpImageGeneration(m_gpuDisplayGeneration);
        }
        return;
    }

    m_gpuDisplayLastUpdateWasIncremental = false;
    m_gpuDisplayLastShiftPixels = 0;
    m_gpuDisplayLastUpdatedColumns = filledPixels;

    const qint64 advanceColumns =
        m_gpuDisplayRolling == rollingMode
            ? (displayLeft - m_gpuDisplayDisplayLeft)
            : 0;
    const qint64 advanceRight =
        m_gpuDisplayRolling == rollingMode
            ? (displayRight - m_gpuDisplayDisplayRight)
            : 0;
    const double requestedShiftPixels =
        static_cast<double>(advanceColumns) * effectiveZoom;
    const double totalShiftPixels =
        m_gpuDisplaySubpixelOffset + requestedShiftPixels;
    const int shiftPixels = std::max(
        0,
        static_cast<int>(std::floor(totalShiftPixels + 1e-9)));
    const double nextSubpixelOffset =
        std::clamp(
            totalShiftPixels - static_cast<double>(shiftPixels),
            0.0,
            0.999999);
    const bool displayConfigDirtyBeyondWidth =
        (maintainImagePixels && m_gpuDisplayImage.isNull())
        || m_gpuDisplayHeight != height
        || m_gpuDisplaySampleRateHz != m_sampleRateHz
        || m_gpuDisplayBins != m_precomputedBinsPerColumn
        || std::abs(m_gpuDisplayDbRange - m_dbRange) > 0.001
        || m_gpuDisplayMuted != m_channelMuted
        || m_gpuDisplayLogScale != m_logScale;
    const double requestedGrowthPixels =
        static_cast<double>(filledPixels - m_gpuDisplayFilledWidth);
    const bool canIncrementallyGrowRollingDisplay =
        contentDirty
        && rollingMode
        && !displayConfigDirtyBeyondWidth
        && m_gpuDisplayRolling == rollingMode
        && m_gpuDisplayTrackToken == m_precomputedTrackToken
        && displayLeft == m_gpuDisplayDisplayLeft
        && displayRight > m_gpuDisplayDisplayRight
        && filledPixels > m_gpuDisplayFilledWidth
        && std::abs(effectiveZoom - m_gpuDisplayEffectiveZoom) < 0.001
        && advanceColumns == 0
        && advanceRight > 0
        && std::abs(
               requestedGrowthPixels
               - static_cast<double>(std::lround(
                   static_cast<double>(advanceRight) * effectiveZoom)))
               < 0.001;
    const bool canIncrementallyAdvanceDisplay =
        contentDirty
        && !displayConfigDirty
        && advanceColumns > 0
        && advanceColumns == advanceRight
        && filledPixels == drawPixels
        && m_gpuDisplayFilledWidth == drawPixels
        && shiftPixels < drawPixels
        && totalShiftPixels > 0.0
        && std::abs(effectiveZoom - m_gpuDisplayEffectiveZoom) < 0.001
        && m_gpuDisplayRolling == rollingMode
        && displayLeft >= m_gpuDisplayDisplayLeft
        && displayRight >= m_gpuDisplayDisplayRight;
    const int oldContentOffset =
        rollingMode ? std::max(0, drawPixels - m_gpuDisplayFilledWidth) : 0;
    const int mixedShiftPixels = std::max(
        0,
        oldContentOffset + shiftPixels);
    const bool canIncrementallyCompleteAndAdvanceRollingDisplay =
        contentDirty
        && rollingMode
        && !displayConfigDirtyBeyondWidth
        && m_gpuDisplayRolling == rollingMode
        && m_gpuDisplayTrackToken == m_precomputedTrackToken
        && m_gpuDisplayFilledWidth > 0
        && m_gpuDisplayFilledWidth < drawPixels
        && filledPixels == drawPixels
        && advanceColumns > 0
        && advanceRight > advanceColumns
        && mixedShiftPixels > 0
        && mixedShiftPixels < drawPixels
        && std::abs(effectiveZoom - m_gpuDisplayEffectiveZoom) < 0.001
        && displayLeft >= m_gpuDisplayDisplayLeft
        && displayRight >= m_gpuDisplayDisplayRight;

    bool imagePixelsChanged = false;
    if (!maintainImagePixels) {
        if (!m_gpuDisplayImage.isNull()) {
            m_gpuDisplayImage = QImage();
            bumpImageGeneration(m_gpuDisplayGeneration);
        }

        if (canIncrementallyGrowRollingDisplay) {
            const int growthPixels = filledPixels - m_gpuDisplayFilledWidth;
            m_gpuDisplayLastUpdateWasIncremental = true;
            m_gpuDisplayLastShiftPixels = growthPixels;
            m_gpuDisplayLastUpdatedColumns = growthPixels;
            m_gpuDisplaySubpixelOffset = 0.0;
        } else if (canIncrementallyCompleteAndAdvanceRollingDisplay) {
            m_gpuDisplayLastUpdateWasIncremental = true;
            m_gpuDisplayLastShiftPixels = mixedShiftPixels;
            m_gpuDisplayLastUpdatedColumns = mixedShiftPixels;
            m_gpuDisplaySubpixelOffset = nextSubpixelOffset;
        } else if (canIncrementallyAdvanceDisplay) {
            m_gpuDisplayLastUpdateWasIncremental = true;
            m_gpuDisplayLastShiftPixels = shiftPixels;
            m_gpuDisplayLastUpdatedColumns = shiftPixels;
            m_gpuDisplaySubpixelOffset = nextSubpixelOffset;
        } else {
            m_gpuDisplaySubpixelOffset = 0.0;
        }

        m_gpuDisplaySourceGeneration = m_gpuDisplayGeneration;
        m_gpuDisplayWidth = drawPixels;
        m_gpuDisplayFilledWidth = filledPixels;
        m_gpuDisplayHeight = height;
        m_gpuDisplaySampleRateHz = m_sampleRateHz;
        m_gpuDisplayBins = m_precomputedBinsPerColumn;
        m_gpuDisplayDbRange = m_dbRange;
        m_gpuDisplayMuted = m_channelMuted;
        m_gpuDisplayLogScale = m_logScale;
        m_gpuDisplayDisplayLeft = displayLeft;
        m_gpuDisplayDisplayRight = displayRight;
        m_gpuDisplayRolling = rollingMode;
        m_gpuDisplayRingWriteSeq = m_ringWriteSeq;
        m_gpuDisplayRingOldestSeq = m_ringOldestSeq;
        m_gpuDisplayMaxColumnIndex = m_precomputedMaxColumnIndex;
        m_gpuDisplayTrackToken = m_precomputedTrackToken;
        m_gpuDisplayEffectiveZoom = effectiveZoom;
        return;
    }

    const auto dbRemap = buildPrecomputedDbRemapLocked();
    const auto &palette = m_channelMuted ? m_palette32Gray : m_palette32;

    if (canIncrementallyGrowRollingDisplay) {
        const int growthPixels = filledPixels - m_gpuDisplayFilledWidth;
        const int oldStartX = drawPixels - m_gpuDisplayFilledWidth;
        const int newStartX = drawPixels - filledPixels;
        for (int y = 0; y < height; ++y) {
            auto *line = reinterpret_cast<QRgb *>(m_gpuDisplayImage.scanLine(y));
            std::memmove(
                line + newStartX,
                line + oldStartX,
                static_cast<size_t>(m_gpuDisplayFilledWidth) * sizeof(QRgb));
        }
        paintPrecomputedDisplayColumnsLocked(
            m_gpuDisplayImage,
            drawPixels - growthPixels,
            drawPixels - 1,
            drawPixels,
            filledPixels,
            height,
            displayLeft,
            displayRight,
            rollingMode,
            dbRemap,
            palette.data());
        m_gpuDisplayLastUpdateWasIncremental = true;
        m_gpuDisplayLastShiftPixels = growthPixels;
        m_gpuDisplayLastUpdatedColumns = growthPixels;
        m_gpuDisplaySubpixelOffset = 0.0;
        imagePixelsChanged = true;
    } else if (canIncrementallyCompleteAndAdvanceRollingDisplay) {
        for (int y = 0; y < height; ++y) {
            auto *line = reinterpret_cast<QRgb *>(m_gpuDisplayImage.scanLine(y));
            std::memmove(
                line,
                line + mixedShiftPixels,
                static_cast<size_t>(drawPixels - mixedShiftPixels) * sizeof(QRgb));
        }
        paintPrecomputedDisplayColumnsLocked(
            m_gpuDisplayImage,
            drawPixels - mixedShiftPixels,
            drawPixels - 1,
            drawPixels,
            filledPixels,
            height,
            displayLeft,
            displayRight,
            rollingMode,
            dbRemap,
            palette.data());
        m_gpuDisplayLastUpdateWasIncremental = true;
        m_gpuDisplayLastShiftPixels = mixedShiftPixels;
        m_gpuDisplayLastUpdatedColumns = mixedShiftPixels;
        m_gpuDisplaySubpixelOffset = nextSubpixelOffset;
        imagePixelsChanged = true;
    } else if (canIncrementallyAdvanceDisplay) {
        if (shiftPixels > 0) {
            for (int y = 0; y < height; ++y) {
                auto *line = reinterpret_cast<QRgb *>(m_gpuDisplayImage.scanLine(y));
                std::memmove(
                    line,
                    line + shiftPixels,
                    static_cast<size_t>(drawPixels - shiftPixels) * sizeof(QRgb));
            }
            paintPrecomputedDisplayColumnsLocked(
                m_gpuDisplayImage,
                drawPixels - shiftPixels,
                drawPixels - 1,
                drawPixels,
                filledPixels,
                height,
                displayLeft,
                displayRight,
                rollingMode,
                dbRemap,
                palette.data());
            imagePixelsChanged = true;
        }
        m_gpuDisplayLastUpdateWasIncremental = true;
        m_gpuDisplayLastShiftPixels = shiftPixels;
        m_gpuDisplayLastUpdatedColumns = shiftPixels;
        m_gpuDisplaySubpixelOffset = nextSubpixelOffset;
    } else {
        m_gpuDisplayImage = QImage(drawPixels, height, QImage::Format_ARGB32_Premultiplied);
        m_gpuDisplayImage.fill(Qt::black);
        paintPrecomputedDisplayColumnsLocked(
            m_gpuDisplayImage,
            rollingMode ? std::max(0, drawPixels - filledPixels) : 0,
            drawPixels - 1,
            drawPixels,
            filledPixels,
            height,
            displayLeft,
            displayRight,
            rollingMode,
            dbRemap,
            palette.data());
        m_gpuDisplaySubpixelOffset = 0.0;
        imagePixelsChanged = true;
    }

    m_gpuDisplaySourceGeneration = m_gpuDisplayGeneration;
    if (imagePixelsChanged) {
        m_gpuDisplayGeneration += 1;
    }
    m_gpuDisplayWidth = drawPixels;
    m_gpuDisplayFilledWidth = filledPixels;
    m_gpuDisplayHeight = height;
    m_gpuDisplaySampleRateHz = m_sampleRateHz;
    m_gpuDisplayBins = m_precomputedBinsPerColumn;
    m_gpuDisplayDbRange = m_dbRange;
    m_gpuDisplayMuted = m_channelMuted;
    m_gpuDisplayLogScale = m_logScale;
    m_gpuDisplayDisplayLeft = displayLeft;
    m_gpuDisplayDisplayRight = displayRight;
    m_gpuDisplayRolling = rollingMode;
    m_gpuDisplayRingWriteSeq = m_ringWriteSeq;
    m_gpuDisplayRingOldestSeq = m_ringOldestSeq;
    m_gpuDisplayMaxColumnIndex = m_precomputedMaxColumnIndex;
    m_gpuDisplayTrackToken = m_precomputedTrackToken;
    m_gpuDisplayEffectiveZoom = effectiveZoom;
}

qint64 SpectrogramItem::firstMissingCenteredDisplayColumnLocked(
    const qint64 displayLeft,
    const qint64 displayRight) const {
    if (displayRight < displayLeft
        || displayLeft < 0
        || displayRight > std::numeric_limits<qint32>::max()) {
        return displayLeft;
    }
    for (qint64 displayIndex = displayLeft; displayIndex <= displayRight; ++displayIndex) {
        if (ringSlotForDisplayIndexLocked(displayIndex, false) < 0) {
            return displayIndex;
        }
    }
    return -1;
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
        FERROUS_SPECTROGRAM_PROFILE_LOGF(
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
    maybeDumpGpuDebugFrame();
    if (changed || (precomputedActive && playing)) {
        update();
    }
}

void SpectrogramItem::maybeDumpGpuDebugFrame() {
    if (m_gpuDumpFramesRemaining <= 0) {
        return;
    }

    QQuickWindow *currentWindow = window();
    if (currentWindow == nullptr || width() <= 0.0 || height() <= 0.0) {
        return;
    }

    const QImage frame = currentWindow->grabWindow();
    if (frame.isNull()) {
        return;
    }

    const qreal dpr = frame.devicePixelRatio();
    const QRectF sceneRect = mapRectToScene(boundingRect());
    const QRect cropRect(
        static_cast<int>(std::floor(sceneRect.x() * dpr)),
        static_cast<int>(std::floor(sceneRect.y() * dpr)),
        static_cast<int>(std::ceil(sceneRect.width() * dpr)),
        static_cast<int>(std::ceil(sceneRect.height() * dpr)));
    const QRect boundedCrop = cropRect.intersected(frame.rect());
    if (boundedCrop.isEmpty()) {
        return;
    }

    const QImage crop = frame.copy(boundedCrop);
    const DebugFrameStats stats = analyzeDebugFrame(crop);

    QDir dumpDir(m_gpuDumpDirectory);
    if (!dumpDir.exists()) {
        QDir().mkpath(m_gpuDumpDirectory);
    }
    const QString path = dumpDir.filePath(
        QStringLiteral("spectrogram-%1-%2-%3.png")
            .arg(reinterpret_cast<quintptr>(this), 0, 16)
            .arg(SpectrogramGpu::debugModeName(SpectrogramGpu::currentDebugMode()))
            .arg(m_gpuDumpFrameSerial++, 3, 10, QLatin1Char('0')));
    crop.save(path);

    std::fprintf(
        stderr,
        "[Qt-gpu-dump@%p] mode=%s path=%s crop=%dx%d nonblack=%d avg_luma=%.2f remaining=%d\n",
        static_cast<const void *>(this),
        SpectrogramGpu::debugModeName(SpectrogramGpu::currentDebugMode()),
        path.toUtf8().constData(),
        crop.width(),
        crop.height(),
        stats.nonBlackPixels,
        stats.averageLuma,
        m_gpuDumpFramesRemaining - 1);
    m_gpuDumpFramesRemaining -= 1;
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

void SpectrogramItem::noteSmoothnessServoLocked(
    double errorSeconds,
    bool regressedDuringPlayback) {
    if (!m_smoothnessProfile.active) {
        return;
    }

    const double errorMs = std::abs(errorSeconds) * 1000.0;
    m_smoothnessProfile.servoFrames += 1;
    m_smoothnessProfile.servoErrorMsTotal += errorMs;
    m_smoothnessProfile.maxServoErrorMs =
        std::max(m_smoothnessProfile.maxServoErrorMs, errorMs);
    if (regressedDuringPlayback) {
        m_smoothnessProfile.servoRegressionDrops += 1;
    }

    if (m_profileEnabled
        && errorMs >= 12.0
        && shouldLogProfileSpike(&m_profileLastServoSpike, std::chrono::steady_clock::now(), 0.10)) {
        FERROUS_SPECTROGRAM_PROFILE_LOGF(
            stderr,
            "[ui-spectrogram] position_servo error_ms=%.3f regressed=%d frames=%d drops=%d\n",
            errorMs,
            regressedDuringPlayback ? 1 : 0,
            m_smoothnessProfile.servoFrames,
            m_smoothnessProfile.servoRegressionDrops);
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

    const int canvasWidth = m_gpuDisplayWidth > 0 ? m_gpuDisplayWidth
        : std::max(1, static_cast<int>(std::floor(width())));
    const double headUnits = static_cast<double>(
        std::max<qint64>(0, m_precomputedCanvasDisplayRight + 1))
        + (m_precomputedCanvasRolling ? m_gpuDisplaySubpixelOffset : 0.0);
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
        FERROUS_SPECTROGRAM_PROFILE_LOGF(
            stderr,
            "[ui-spectrogram] smoothness_hitch_detected sample_rate_hz=%d rows_per_second=%.3f frames=%d pending_frames=%d gap_frames=%d severe_gap_frames=%d pending_gap_frames=%d stall_clusters=%d regressions=%d servo_frames=%d servo_drop_frames=%d servo_max_error_ms=%.3f pending_max=%d drain_passes=%d drained=%d advance_fallback_frames=%d advance_fallback_non_unity=%d advance_fallback_max_zoom_delta=%.6f paint_spikes=%d max_gap_ms=%.3f advanced=%d\n",
            m_sampleRateHz,
            rowsPerSecond,
            m_smoothnessProfile.framesObserved,
            m_smoothnessProfile.pendingFrames,
            m_smoothnessProfile.gapFrames,
            m_smoothnessProfile.severeGapFrames,
            m_smoothnessProfile.pendingGapFrames,
            m_smoothnessProfile.stallClusters,
            m_smoothnessProfile.regressionCount,
            m_smoothnessProfile.servoFrames,
            m_smoothnessProfile.servoRegressionDrops,
            m_smoothnessProfile.maxServoErrorMs,
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
        FERROUS_SPECTROGRAM_PROFILE_LOGF(
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
    summary.insert(QStringLiteral("servoFrames"), m_smoothnessProfile.servoFrames);
    summary.insert(
        QStringLiteral("servoRegressionDrops"),
        m_smoothnessProfile.servoRegressionDrops);
    summary.insert(QStringLiteral("servoErrorMsTotal"), m_smoothnessProfile.servoErrorMsTotal);
    summary.insert(QStringLiteral("maxServoErrorMs"), m_smoothnessProfile.maxServoErrorMs);
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

    FERROUS_SPECTROGRAM_PROFILE_LOGF(
        stderr,
        "[ui-spectrogram] smoothness_window reason=%s sample_rate_hz=%d rows_per_second=%.3f frames=%d pending_frames=%d gap_frames=%d severe_gap_frames=%d pending_gap_frames=%d stall_clusters=%d regressions=%d servo_frames=%d servo_drop_frames=%d servo_total_error_ms=%.3f servo_max_error_ms=%.3f drain_passes=%d drained=%d pending_max=%d advance_fallback_frames=%d advance_fallback_non_unity=%d advance_fallback_max_zoom_delta=%.6f paint_spikes=%d max_gap_ms=%.3f max_paint_ms=%.3f avg_paint_ms=%.3f incident=%d\n",
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
        m_smoothnessProfile.servoFrames,
        m_smoothnessProfile.servoRegressionDrops,
        m_smoothnessProfile.servoErrorMsTotal,
        m_smoothnessProfile.maxServoErrorMs,
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
        state.insert(QStringLiteral("servoFrames"), m_smoothnessProfile.servoFrames);
        state.insert(
            QStringLiteral("servoRegressionDrops"),
            m_smoothnessProfile.servoRegressionDrops);
        state.insert(QStringLiteral("servoErrorMsTotal"), m_smoothnessProfile.servoErrorMsTotal);
        state.insert(QStringLiteral("maxServoErrorMs"), m_smoothnessProfile.maxServoErrorMs);
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

    const int canvasWidth = m_gpuDisplayWidth > 0 ? m_gpuDisplayWidth
        : std::max(1, static_cast<int>(std::floor(width())));
    const double headUnits = static_cast<double>(
        std::max<qint64>(0, m_precomputedCanvasDisplayRight + 1))
        + (m_precomputedCanvasRolling ? m_gpuDisplaySubpixelOffset : 0.0);
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
            FERROUS_SPECTROGRAM_PROFILE_LOGF(
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
        FERROUS_SPECTROGRAM_PROFILE_LOGF(
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
