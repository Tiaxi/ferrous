#include "SpectrogramItem.h"

#include <QFontMetrics>
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

namespace {
constexpr double kMinFreqHz = 25.0;
constexpr double kReferenceHopSamples = 1024.0;
constexpr int kMaxPendingColumns = 512;
constexpr int kLivePendingColumns = 2;
constexpr int kMaxTileFragments = 96;
const QColor kBackgroundColor(0x0b, 0x0b, 0x0f);
const QColor kOverlayColor(190, 190, 200, 150);
constexpr std::array<std::array<int, 3>, 7> kGradientColors16{{
    {{65535, 65535, 65535}},
    {{65535, 65535, 65535}},
    {{65535, 63479, 0}},
    {{62194, 13878, 0}},
    {{45232, 0, 23387}},
    {{12336, 0, 29555}},
    {{1027, 256, 18247}},
}};

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
#endif

struct SpectrogramSceneNode final : public QSGNode {
    SpectrogramSceneNode() {
        background = new QSGSimpleRectNode();
        tilesRoot = new QSGNode();
        latest = new QSGSimpleTextureNode();
        overlay = new QSGSimpleTextureNode();
        appendChildNode(background);
        appendChildNode(tilesRoot);
        appendChildNode(latest);
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
        delete placeholderTexture;
    }

    QSGSimpleRectNode *background{nullptr};
    QSGNode *tilesRoot{nullptr};
    QSGSimpleTextureNode *latest{nullptr};
    QSGSimpleTextureNode *overlay{nullptr};
    QVector<QSGSimpleTextureNode *> tileFragments;
    QVector<QSGTexture *> tileTextures;
    QSGTexture *overlayTexture{nullptr};
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
} // namespace

SpectrogramItem::SpectrogramItem(QQuickItem *parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
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
}

double SpectrogramItem::dbRange() const {
    return m_dbRange;
}

void SpectrogramItem::setDbRange(double value) {
    QMutexLocker lock(&m_stateMutex);
    const double clamped = std::clamp(value, 50.0, 120.0);
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
    update();
}

bool SpectrogramItem::showFpsOverlay() const {
    return m_showFpsOverlay;
}

void SpectrogramItem::setShowFpsOverlay(bool value) {
    const bool next = value || m_forceFpsOverlay;
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
    update();
}

int SpectrogramItem::maxColumns() const {
    return m_maxColumns;
}

void SpectrogramItem::setMaxColumns(int value) {
    QMutexLocker lock(&m_stateMutex);
    const int clamped = std::clamp(value, 128, 4096);
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

void SpectrogramItem::reset() {
    QMutexLocker lock(&m_stateMutex);
    m_columns.clear();
    m_pendingColumns.clear();
    m_pendingPhase = 0.0;
    m_seedHistoryOnNextAppend = true;
    m_pendingDrainScheduled = false;
    m_lastRowAppendTime = std::chrono::steady_clock::time_point{};
    m_animationTickInitialized = false;
    m_binsPerColumn = 0;
    invalidateMapping();
    invalidateCanvas();
    update();
}

void SpectrogramItem::halt() {
    QMutexLocker lock(&m_stateMutex);
    m_pendingColumns.clear();
    m_pendingPhase = 0.0;
    m_pendingDrainScheduled = false;
    m_lastRowAppendTime = std::chrono::steady_clock::time_point{};
    m_animationTickInitialized = false;
    update();
}

void SpectrogramItem::appendRows(const QVariantList &rows) {
    QMutexLocker lock(&m_stateMutex);
    if (rows.isEmpty()) {
        return;
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
        m_pendingColumns.emplace_back(std::move(mapped));
        rowsAdded++;
    }

    while (static_cast<int>(m_pendingColumns.size()) > kMaxPendingColumns) {
        m_pendingColumns.pop_front();
    }
    if (rowsAdded <= 0) {
        return;
    }
    noteIncomingRowsLocked();
    if (m_seedHistoryOnNextAppend || m_columns.empty()) {
        absorbPendingHistoryLocked(kLivePendingColumns);
    }
    if (m_columns.empty()) {
        consumePendingColumnsLocked(1);
    }
    lock.unlock();
    update();
}

void SpectrogramItem::appendPackedRows(
    const QByteArray &packedRows,
    int rowCount,
    int binsPerRow,
    bool seedHistoryBurst) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const auto appendStart = std::chrono::steady_clock::now();
#endif
    QMutexLocker lock(&m_stateMutex);
    if (packedRows.isEmpty() || rowCount <= 0 || binsPerRow <= 0) {
        return;
    }
    const qsizetype expected = static_cast<qsizetype>(rowCount) * static_cast<qsizetype>(binsPerRow);
    if (packedRows.size() < expected) {
        return;
    }

    if (m_binsPerColumn <= 0) {
        m_binsPerColumn = binsPerRow;
        invalidateMapping();
    }
    if (m_binsPerColumn != binsPerRow) {
        return;
    }

    int appended = 0;
    const auto *src = reinterpret_cast<const quint8 *>(packedRows.constData());
    for (int r = 0; r < rowCount; ++r) {
        std::vector<quint8> col(static_cast<size_t>(binsPerRow));
        std::copy_n(
            src + static_cast<qsizetype>(r) * static_cast<qsizetype>(binsPerRow),
            binsPerRow,
            col.begin());
        m_pendingColumns.emplace_back(std::move(col));
        appended++;
    }
    while (static_cast<int>(m_pendingColumns.size()) > kMaxPendingColumns) {
        m_pendingColumns.pop_front();
    }
    if (appended <= 0) {
        return;
    }
    noteIncomingRowsLocked();
    if (seedHistoryBurst || m_seedHistoryOnNextAppend || m_columns.empty()) {
        absorbPendingHistoryLocked(kLivePendingColumns);
    }
    if (m_columns.empty()) {
        consumePendingColumnsLocked(1);
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileEnabled) {
        const auto now = std::chrono::steady_clock::now();
        const double appendMs = std::chrono::duration<double, std::milli>(now - appendStart).count();
        const int backlog = static_cast<int>(m_pendingColumns.size());
        if ((appendMs >= 2.0 || backlog >= 96)
            && shouldLogProfileSpike(&m_profileLastAppendSpike, now)) {
            std::fprintf(
                stderr,
                "[ui-spectrogram] append rows=%d bins=%d copy_ms=%.3f backlog=%d cols=%zu\n",
                appended,
                binsPerRow,
                appendMs,
                backlog,
                static_cast<size_t>(m_columns.size()));
        }
    }
#endif
    lock.unlock();
    update();
}

QSGNode *SpectrogramItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const auto paintStart = std::chrono::steady_clock::now();
#endif
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
    QVector<QImage> tileImages;
    QVector<int> tileDirtyIndexes;
    int tileCount = 0;
    bool showOverlay = false;
    bool overlayChanged = false;
    QImage overlayImage;
    QSize overlaySize;
    size_t profilePendingColumns = 0;
    size_t profileColumnCount = 0;
    int profileBinsPerColumn = 0;

    {
        QMutexLocker lock(&m_stateMutex);
        if (!m_columns.empty() && m_binsPerColumn > 0) {
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
                    scrollOffset = std::clamp(m_pendingPhase, 0.0, 0.999);
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
        profilePendingColumns = m_pendingColumns.size();
        profileColumnCount = m_columns.size();
        profileBinsPerColumn = m_binsPerColumn;
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
                    static_cast<double>(w) - scrollOffset,
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
    qDeleteAll(retiredTileTextures);

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileEnabled) {
        const auto paintEnd = std::chrono::steady_clock::now();
        const double paintMs = std::chrono::duration<double, std::milli>(paintEnd - paintStart).count();
        m_profilePaints += 1;
        m_profilePaintMs += paintMs;
        if (paintMs >= 4.0 && shouldLogProfileSpike(&m_profileLastPaintSpike, paintEnd)) {
            std::fprintf(
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
            std::fprintf(
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
}

void SpectrogramItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        QMutexLocker lock(&m_stateMutex);
        invalidateMapping();
        invalidateCanvas();
    }
    update();
}

void SpectrogramItem::rebuildPalette() {
    constexpr double scale = 255.0 / 65535.0;
    constexpr int numSegments = static_cast<int>(kGradientColors16.size()) - 1;
    for (int i = 0; i < kGradientTableSize; ++i) {
        const double position = static_cast<double>(i) / static_cast<double>(kGradientTableSize);
        const double m = static_cast<double>(numSegments) * position;
        const int n = std::clamp(static_cast<int>(std::floor(m)), 0, numSegments);
        const double f = std::clamp(m - static_cast<double>(n), 0.0, 1.0);
        const int n1 = std::min(numSegments, n + 1);

        const int r = static_cast<int>(
            (static_cast<double>(kGradientColors16[static_cast<size_t>(n)][0]) * scale)
            + f * ((static_cast<double>(kGradientColors16[static_cast<size_t>(n1)][0]) * scale)
                   - (static_cast<double>(kGradientColors16[static_cast<size_t>(n)][0]) * scale)));
        const int g = static_cast<int>(
            (static_cast<double>(kGradientColors16[static_cast<size_t>(n)][1]) * scale)
            + f * ((static_cast<double>(kGradientColors16[static_cast<size_t>(n1)][1]) * scale)
                   - (static_cast<double>(kGradientColors16[static_cast<size_t>(n)][1]) * scale)));
        const int b = static_cast<int>(
            (static_cast<double>(kGradientColors16[static_cast<size_t>(n)][2]) * scale)
            + f * ((static_cast<double>(kGradientColors16[static_cast<size_t>(n1)][2]) * scale)
                   - (static_cast<double>(kGradientColors16[static_cast<size_t>(n)][2]) * scale)));

        m_palette32[static_cast<size_t>(i)] = qRgb(
            std::clamp(r, 0, 255),
            std::clamp(g, 0, 255),
            std::clamp(b, 0, 255));
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
        auto *line = reinterpret_cast<QRgb *>(m_canvas.scanLine(y));
        line[x] = m_palette32[static_cast<size_t>(paletteIndex)];
    }
}

bool SpectrogramItem::consumePendingColumnsLocked(int requested) {
    if (requested <= 0 || m_pendingColumns.empty()) {
        return false;
    }
    const int toConsume = std::min(requested, static_cast<int>(m_pendingColumns.size()));
    if (toConsume <= 0) {
        return false;
    }

    const int w = std::max(1, static_cast<int>(std::floor(width())));
    const int h = std::max(1, static_cast<int>(std::floor(height())));
    ensureCanvas(w, h);
    if (!m_canvas.isNull() && m_canvasDirty) {
        rebuildCanvasFromColumns();
    }

    bool consumed = false;
    for (int i = 0; i < toConsume; ++i) {
        std::vector<quint8> col = std::move(m_pendingColumns.front());
        m_pendingColumns.pop_front();
        if (static_cast<int>(col.size()) != m_binsPerColumn) {
            continue;
        }
        m_columns.emplace_back(std::move(col));
        while (static_cast<int>(m_columns.size()) > m_maxColumns) {
            m_columns.pop_front();
        }
        if (!m_canvas.isNull()) {
            drawColumnAt(m_canvasWriteX, m_columns.back());
            m_canvasWriteX = (m_canvasWriteX + 1) % m_canvas.width();
            m_canvasFilledCols = std::min(m_canvas.width(), m_canvasFilledCols + 1);
        }
        consumed = true;
    }
    return consumed;
}

void SpectrogramItem::absorbPendingHistoryLocked(int retainPending) {
    const int retain = std::max(0, retainPending);
    const int pending = static_cast<int>(m_pendingColumns.size());
    const int absorb = std::max(0, pending - retain);
    if (absorb > 0) {
        consumePendingColumnsLocked(absorb);
        m_pendingPhase = std::clamp(m_pendingPhase, 0.0, 0.999);
    }
    m_seedHistoryOnNextAppend = false;
}

int SpectrogramItem::readyPendingColumnsLocked() const {
    if (m_pendingColumns.empty()) {
        return 0;
    }
    return std::min(
        static_cast<int>(std::floor(m_pendingPhase)),
        static_cast<int>(m_pendingColumns.size()));
}

int SpectrogramItem::maxDrainColumnsPerPassLocked() const {
    const double rowsPerSecond = targetRowsPerSecondLocked();
    if (!std::isfinite(rowsPerSecond) || rowsPerSecond <= 1.0) {
        return 1;
    }
    return std::clamp(static_cast<int>(std::ceil(rowsPerSecond / 30.0)), 1, 4);
}

bool SpectrogramItem::advanceAnimationLocked(double elapsedSeconds, bool *drainReady) {
    double dt = elapsedSeconds;
    if (!std::isfinite(dt) || dt <= 0.0 || dt > 0.25) {
        const double fallbackFps = m_fpsValue > 0 ? static_cast<double>(m_fpsValue) : 60.0;
        dt = 1.0 / std::max(30.0, fallbackFps);
    }

    const double rowsPerSecond = targetRowsPerSecondLocked();
    if (!std::isfinite(rowsPerSecond) || rowsPerSecond <= 1.0) {
        if (m_pendingPhase > 0.0) {
            m_pendingPhase = 0.0;
            return true;
        }
        return false;
    }

    const double prevPhase = m_pendingPhase;
    m_pendingPhase += rowsPerSecond * dt;

    if (m_pendingColumns.empty()) {
        const double idleSeconds = m_lastRowAppendTime.time_since_epoch().count() == 0
            ? 1.0
            : std::chrono::duration<double>(std::chrono::steady_clock::now() - m_lastRowAppendTime).count();
        if (idleSeconds > 0.30) {
            m_pendingPhase = 0.0;
        } else {
            m_pendingPhase = std::clamp(m_pendingPhase, 0.0, 0.999);
        }
    }

    if (drainReady != nullptr) {
        *drainReady = readyPendingColumnsLocked() > 0;
    }
    const bool phaseChanged = std::abs(m_pendingPhase - prevPhase) > 0.0001;
    return phaseChanged;
}

void SpectrogramItem::schedulePendingDrain() {
    bool shouldSchedule = false;
    {
        QMutexLocker lock(&m_stateMutex);
        if (!m_pendingDrainScheduled && readyPendingColumnsLocked() > 0) {
            m_pendingDrainScheduled = true;
            shouldSchedule = true;
        }
    }
    if (!shouldSchedule) {
        return;
    }

    QMetaObject::invokeMethod(
        this,
        [this]() {
            drainPendingColumns();
        },
        Qt::QueuedConnection);
}

void SpectrogramItem::drainPendingColumns() {
    bool consumed = false;
    {
        QMutexLocker lock(&m_stateMutex);
        m_pendingDrainScheduled = false;
        const int ready = readyPendingColumnsLocked();
        const int consume = std::min(ready, maxDrainColumnsPerPassLocked());
        if (consume > 0) {
            consumed = consumePendingColumnsLocked(consume);
            if (consumed) {
                m_pendingPhase = std::max(0.0, m_pendingPhase - static_cast<double>(consume));
            }
        }
    }

    if (consumed) {
        update();
    }
}

void SpectrogramItem::noteIncomingRowsLocked() {
    m_lastRowAppendTime = std::chrono::steady_clock::now();
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

    const double dbRange = std::clamp(m_dbRange, 50.0, 120.0);
    const double dbScale = 10.0 / std::log(10.0);

    for (const QVariant &value : row) {
        const double v = value.toDouble();
        int idx = 0;
        if (v >= 0.0 && v <= 255.0 && std::floor(v) == v) {
            idx = static_cast<int>(v);
        } else {
            const double db = v > 0.0 ? (dbScale * std::log(v)) : -200.0;
            const double xdb = std::clamp(db + dbRange - 63.0, 0.0, dbRange);
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
    update();
}

void SpectrogramItem::handleWindowAfterAnimating() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();

    bool advanced = false;
    bool pending = false;
    bool drainReady = false;
    double elapsed = 0.0;
    QMutexLocker lock(&m_stateMutex);
    if (m_animationTickInitialized) {
        elapsed = std::chrono::duration<double>(now - m_lastAnimationTick).count();
    }
    m_lastAnimationTick = now;
    m_animationTickInitialized = true;

    const int prev = m_fpsValue;
    bool changed = false;
    if (m_showFpsOverlay) {
        updateFpsEstimateLocked();
        changed = m_fpsValue != prev;
    }
    advanced = advanceAnimationLocked(elapsed, &drainReady);
    pending = !m_pendingColumns.empty();
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileEnabled
        && elapsed >= 0.025
        && shouldLogProfileSpike(&m_profileLastFrameGapSpike, now)) {
        std::fprintf(
            stderr,
            "[ui-spectrogram] frame_gap ms=%.3f pending=%zu phase=%.3f fps=%d advanced=%d\n",
            elapsed * 1000.0,
            static_cast<size_t>(m_pendingColumns.size()),
            m_pendingPhase,
            m_fpsValue,
            advanced ? 1 : 0);
    }
#endif
    lock.unlock();
    if (drainReady) {
        schedulePendingDrain();
    }
    if (changed || advanced || pending) {
        update();
    }
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
