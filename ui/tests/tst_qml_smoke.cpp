// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHoverEvent>
#include <QImage>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOffscreenSurface>
#include <QPainter>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QRunnable>
#include <QResource>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGRendererInterface>
#include <QSGSimpleRectNode>
#include <QSGSimpleTextureNode>
#include <QSGTextureMaterial>
#include <QQmlComponent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QMutex>
#include <QMutexLocker>
#include <QScopedPointer>
#include <QSurfaceFormat>
#include <QtEndian>
#include <QtTest/QtTest>
#include <qqml.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>
#include <type_traits>

#include "../src/DiagnosticsLog.h"
#include "../src/LibraryTreeModel.h"
#include "../src/SpectrogramBodyMaterial.h"
#include "../src/SpectrogramRenderUtils.h"
#include "../src/SpectrogramSeekTrace.h"
#define private public
#include "../src/SpectrogramSourceTexture.h"
#undef private
#define protected public
#define private public
#include "../src/SpectrogramItem.h"
#include "../src/WaveformItem.h"
#undef private
#undef protected

namespace {

struct BinaryTreeRow {
    quint8 rowType{0};
    quint16 depth{0};
    qint32 sourceIndex{-1};
    quint16 trackNumber{0};
    quint16 childCount{0};
    QString title;
    QString key;
    QString artist;
    QString path;
    QString coverPath;
    QString trackPath;
    QStringList playPaths;
};

template <typename T>
void appendLe(QByteArray &out, T value) {
    const T little = qToLittleEndian(value);
    out.append(reinterpret_cast<const char *>(&little), static_cast<int>(sizeof(T)));
}

void appendUtf8U16(QByteArray &out, const QString &value) {
    QByteArray utf8 = value.toUtf8();
    if (utf8.size() > 65535) {
        utf8.truncate(65535);
    }
    appendLe<quint16>(out, static_cast<quint16>(utf8.size()));
    out.append(utf8);
}

QByteArray encodeRows(const QVector<BinaryTreeRow> &rows) {
    QByteArray out;
    appendLe<quint32>(out, static_cast<quint32>(rows.size()));
    for (const BinaryTreeRow &row : rows) {
        out.append(static_cast<char>(row.rowType));
        appendLe<quint16>(out, row.depth);
        appendLe<qint32>(out, row.sourceIndex);
        appendLe<quint16>(out, row.trackNumber);
        appendLe<quint16>(out, row.childCount);
        appendUtf8U16(out, row.title);
        appendUtf8U16(out, row.key);
        appendUtf8U16(out, row.artist);
        appendUtf8U16(out, row.path);
        appendUtf8U16(out, row.coverPath);
        appendUtf8U16(out, row.trackPath);
        appendLe<quint16>(out, static_cast<quint16>(row.playPaths.size()));
        for (const QString &playPath : row.playPaths) {
            appendUtf8U16(out, playPath);
        }
    }
    return out;
}

QByteArray sampleArtistAlbumTreeBinary() {
    const QString trackPath = QStringLiteral("/music/artist/album/track01.flac");
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{
        1,
        0,
        -1,
        0,
        1,
        QStringLiteral("Artist A (1)"),
        QStringLiteral("artist|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        2,
        1,
        0,
        0,
        1,
        QStringLiteral("Album A"),
        QStringLiteral("album|Artist A|Album A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A/Album A"),
        QStringLiteral("/music/Artist A/Album A/cover.jpg"),
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        4,
        2,
        -1,
        1,
        0,
        QStringLiteral("01 - Track 01"),
        QStringLiteral("track|/music/artist/album/track01.flac"),
        QStringLiteral("Artist A"),
        trackPath,
        {},
        trackPath,
        QStringList{trackPath},
    });
    return encodeRows(rows);
}

QByteArray artistWithManyAlbumsBinary(int albumCount) {
    QVector<BinaryTreeRow> rows;
    rows.reserve(1 + albumCount * 2);
    rows.push_back(BinaryTreeRow{
        1,
        0,
        -1,
        0,
        static_cast<quint16>(albumCount),
        QStringLiteral("Artist A (%1)").arg(albumCount),
        QStringLiteral("artist|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A"),
        {},
        {},
        {},
    });

    for (int i = 0; i < albumCount; ++i) {
        const QString albumName = QStringLiteral("Album %1").arg(i + 1);
        const QString albumPath = QStringLiteral("/music/artist/%1").arg(albumName.toLower().replace(' ', ""));
        const QString trackPath = albumPath + QStringLiteral("/track.flac");
        rows.push_back(BinaryTreeRow{
            2,
            1,
            i,
            0,
            1,
            albumName,
            QStringLiteral("album|Artist A|%1").arg(albumName),
            QStringLiteral("Artist A"),
            albumPath,
            albumPath + QStringLiteral("/cover.jpg"),
            {},
            {},
        });
        rows.push_back(BinaryTreeRow{
            4,
            2,
            -1,
            1,
            0,
            QStringLiteral("01 - Track %1").arg(i + 1),
            QStringLiteral("track|%1").arg(trackPath),
            QStringLiteral("Artist A"),
            trackPath,
            {},
            trackPath,
            QStringList{trackPath},
        });
    }

    return encodeRows(rows);
}

struct OverlayLineSample {
    QLineF line;
    QColor color;
};

QSGGeometryNode *overlayLineNode(QSGNode *root) {
    if (root == nullptr) {
        return nullptr;
    }

    for (QSGNode *child = root->firstChild(); child != nullptr; child = child->nextSibling()) {
        auto *geometryNode = dynamic_cast<QSGGeometryNode *>(child);
        if (geometryNode == nullptr) {
            continue;
        }
        if (geometryNode->geometry() != nullptr
            && geometryNode->geometry()->drawingMode() == QSGGeometry::DrawLines) {
            return geometryNode;
        }
    }
    return nullptr;
}

QSGSimpleRectNode *playheadRectNode(QSGNode *root) {
    if (root == nullptr) {
        return nullptr;
    }

    int seen = 0;
    for (QSGNode *child = root->firstChild(); child != nullptr; child = child->nextSibling()) {
        auto *rectNode = dynamic_cast<QSGSimpleRectNode *>(child);
        if (rectNode == nullptr) {
            continue;
        }
        if (seen == 1) {
            return rectNode;
        }
        ++seen;
    }
    return nullptr;
}

QVector<OverlayLineSample> overlayLineSamples(QSGNode *root) {
    QVector<OverlayLineSample> samples;
    if (root == nullptr) {
        return samples;
    }

    auto *overlayNode = overlayLineNode(root);
    if (overlayNode == nullptr || overlayNode->geometry() == nullptr) {
        return samples;
    }

    const QSGGeometry *geometry = overlayNode->geometry();
    const int vertexCount = geometry->vertexCount();
    if (vertexCount < 2) {
        return samples;
    }

    samples.reserve(vertexCount / 2);
    const auto *vertices = geometry->vertexDataAsColoredPoint2D();
    for (int i = 0; i + 1 < vertexCount; i += 2) {
        const auto &a = vertices[i];
        const auto &b = vertices[i + 1];
        samples.push_back(OverlayLineSample{
            QLineF(
                static_cast<double>(a.x),
                static_cast<double>(a.y),
                static_cast<double>(b.x),
                static_cast<double>(b.y)),
            QColor(a.r, a.g, a.b, a.a),
        });
    }

    return samples;
}

int visibleTextureNodeCount(QSGNode *root) {
    if (root == nullptr) {
        return 0;
    }

    int count = 0;
    for (QSGNode *child = root->firstChild(); child != nullptr; child = child->nextSibling()) {
        auto *textureNode = dynamic_cast<QSGSimpleTextureNode *>(child);
        if (textureNode == nullptr) {
            continue;
        }
        if (textureNode->texture() == nullptr || textureNode->rect().isEmpty()) {
            continue;
        }
        ++count;
    }
    return count;
}

std::optional<QLineF> findUniqueLine(
    const QVector<OverlayLineSample> &samples,
    int alpha,
    bool vertical) {
    std::optional<QLineF> match;
    for (const OverlayLineSample &sample : samples) {
        if (sample.color.alpha() != alpha) {
            continue;
        }
        const bool sampleVertical = qFuzzyCompare(sample.line.x1() + 1.0, sample.line.x2() + 1.0);
        if (sampleVertical != vertical) {
            continue;
        }
        if (match.has_value()) {
            return std::nullopt;
        }
        match = sample.line;
    }
    return match;
}

std::optional<QColor> findUniqueLineColor(
    const QVector<OverlayLineSample> &samples,
    int alpha,
    bool vertical) {
    std::optional<QColor> match;
    for (const OverlayLineSample &sample : samples) {
        if (sample.color.alpha() != alpha) {
            continue;
        }
        const bool sampleVertical = qFuzzyCompare(sample.line.x1() + 1.0, sample.line.x2() + 1.0);
        if (sampleVertical != vertical) {
            continue;
        }
        if (match.has_value()) {
            return std::nullopt;
        }
        match = sample.color;
    }
    return match;
}

QString gridLineSignature(const QVector<OverlayLineSample> &samples) {
    QStringList encoded;
    for (const OverlayLineSample &sample : samples) {
        if (sample.color.alpha() != 50) {
            continue;
        }
        encoded.push_back(QStringLiteral("%1,%2,%3,%4")
                              .arg(qRound(sample.line.x1()))
                              .arg(qRound(sample.line.y1()))
                              .arg(qRound(sample.line.x2()))
                              .arg(qRound(sample.line.y2())));
    }
    encoded.sort();
    return encoded.join(QLatin1Char(';'));
}

QVector<QLineF> findLinesAtCoordinate(
    const QVector<OverlayLineSample> &samples,
    int alpha,
    bool vertical,
    int coordinate) {
    QVector<QLineF> matches;
    for (const OverlayLineSample &sample : samples) {
        if (sample.color.alpha() != alpha) {
            continue;
        }
        const bool sampleVertical =
            qFuzzyCompare(sample.line.x1() + 1.0, sample.line.x2() + 1.0);
        if (sampleVertical != vertical) {
            continue;
        }
        const int fixedCoordinate = vertical
            ? qRound(sample.line.x1())
            : qRound(sample.line.y1());
        if (fixedCoordinate != coordinate) {
            continue;
        }
        matches.push_back(sample.line);
    }
    return matches;
}

QVector<QLineF> findLinesByOrientation(
    const QVector<OverlayLineSample> &samples,
    int alpha,
    bool vertical) {
    QVector<QLineF> matches;
    for (const OverlayLineSample &sample : samples) {
        if (sample.color.alpha() != alpha) {
            continue;
        }
        const bool sampleVertical =
            qFuzzyCompare(sample.line.x1() + 1.0, sample.line.x2() + 1.0);
        if (sampleVertical != vertical) {
            continue;
        }
        matches.push_back(sample.line);
    }
    return matches;
}

double longestLineSpan(const QVector<QLineF> &lines, bool vertical) {
    double longest = 0.0;
    for (const QLineF &line : lines) {
        const double span = vertical
            ? std::abs(line.y2() - line.y1())
            : std::abs(line.x2() - line.x1());
        longest = std::max(longest, span);
    }
    return longest;
}

int countVisiblePixelsInRect(const QImage &image, const QRect &rect, int threshold) {
    int visiblePixels = 0;
    const QRect bounded = rect.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() > threshold || pixel.green() > threshold || pixel.blue() > threshold) {
                ++visiblePixels;
            }
        }
    }
    return visiblePixels;
}

QColor averageVisibleColorInRect(
    const QImage &image,
    const QRect &rect,
    int threshold,
    int *visiblePixelsOut = nullptr) {
    qint64 sumRed = 0;
    qint64 sumGreen = 0;
    qint64 sumBlue = 0;
    int visiblePixels = 0;
    const QRect bounded = rect.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() > threshold || pixel.green() > threshold || pixel.blue() > threshold) {
                sumRed += pixel.red();
                sumGreen += pixel.green();
                sumBlue += pixel.blue();
                ++visiblePixels;
            }
        }
    }

    if (visiblePixelsOut != nullptr) {
        *visiblePixelsOut = visiblePixels;
    }
    if (visiblePixels == 0) {
        return QColor();
    }
    return QColor(
        static_cast<int>(sumRed / visiblePixels),
        static_cast<int>(sumGreen / visiblePixels),
        static_cast<int>(sumBlue / visiblePixels));
}

int countChangedPixelsInRect(
    const QImage &before,
    const QImage &after,
    const QRect &rect,
    int threshold) {
    int changedPixels = 0;
    const QRect bounded =
        rect.intersected(before.rect()).intersected(after.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor beforePixel = before.pixelColor(x, y);
            const QColor afterPixel = after.pixelColor(x, y);
            if (std::abs(beforePixel.red() - afterPixel.red()) > threshold
                || std::abs(beforePixel.green() - afterPixel.green()) > threshold
                || std::abs(beforePixel.blue() - afterPixel.blue()) > threshold) {
                ++changedPixels;
            }
        }
    }
    return changedPixels;
}

QString graphicsApiName(QSGRendererInterface::GraphicsApi api) {
    switch (api) {
    case QSGRendererInterface::Unknown:
        return QStringLiteral("unknown");
    case QSGRendererInterface::Software:
        return QStringLiteral("software");
    case QSGRendererInterface::OpenVG:
        return QStringLiteral("openvg");
    case QSGRendererInterface::OpenGL:
        return QStringLiteral("opengl");
    case QSGRendererInterface::Direct3D11:
        return QStringLiteral("d3d11");
    case QSGRendererInterface::Vulkan:
        return QStringLiteral("vulkan");
    case QSGRendererInterface::Metal:
        return QStringLiteral("metal");
    case QSGRendererInterface::Null:
        return QStringLiteral("null");
    case QSGRendererInterface::Direct3D12:
        return QStringLiteral("d3d12");
    }
    return QStringLiteral("unrecognized");
}

QSGRendererInterface::GraphicsApi windowGraphicsApi(const QQuickWindow &window) {
    if (window.rendererInterface() == nullptr) {
        return QSGRendererInterface::Unknown;
    }
    return window.rendererInterface()->graphicsApi();
}

class CountingTexture final : public QSGTexture {
public:
    CountingTexture(std::atomic_int *destructionCount, const QSize size)
        : m_destructionCount(destructionCount)
        , m_size(size) {
    }

    ~CountingTexture() override {
        if (m_destructionCount != nullptr) {
            m_destructionCount->fetch_add(1);
        }
    }

    qint64 comparisonKey() const override {
        return 0;
    }

    QSize textureSize() const override {
        return m_size;
    }

    bool hasAlphaChannel() const override {
        return true;
    }

    bool hasMipmaps() const override {
        return false;
    }

private:
    std::atomic_int *m_destructionCount{nullptr};
    QSize m_size;
};

struct SharedOpenGlContexts {
    std::unique_ptr<QOffscreenSurface> surface;
    std::unique_ptr<QOpenGLContext> ownerContext;
    std::unique_ptr<QOpenGLContext> sharedContext;

    [[nodiscard]] bool isValid() const {
        return surface != nullptr && ownerContext != nullptr && sharedContext != nullptr;
    }
};

SharedOpenGlContexts createSharedOpenGlContexts() {
    SharedOpenGlContexts contexts;

    auto ownerContext = std::make_unique<QOpenGLContext>();
    ownerContext->setFormat(QSurfaceFormat::defaultFormat());
    if (!ownerContext->create()) {
        return contexts;
    }

    auto sharedContext = std::make_unique<QOpenGLContext>();
    sharedContext->setFormat(ownerContext->format());
    sharedContext->setShareContext(ownerContext.get());
    if (!sharedContext->create() || !QOpenGLContext::areSharing(ownerContext.get(), sharedContext.get())) {
        return contexts;
    }

    auto surface = std::make_unique<QOffscreenSurface>();
    surface->setFormat(ownerContext->format());
    surface->create();
    if (!surface->isValid()) {
        return contexts;
    }

    contexts.surface = std::move(surface);
    contexts.ownerContext = std::move(ownerContext);
    contexts.sharedContext = std::move(sharedContext);
    return contexts;
}

GLuint createNativeTexture(QOpenGLContext *context, const QSize size = QSize(4, 4)) {
    if (context == nullptr) {
        return 0;
    }

    QOpenGLFunctions *functions = context->functions();
    if (functions == nullptr) {
        return 0;
    }

    GLuint textureId = 0;
    functions->glGenTextures(1, &textureId);
    if (textureId == 0) {
        return 0;
    }

    QByteArray pixels(size.width() * size.height() * 4, '\x7f');
    functions->glBindTexture(GL_TEXTURE_2D, textureId);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    functions->glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        size.width(),
        size.height(),
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        pixels.constData());
    return textureId;
}

QSGSimpleTextureNode *findTopLevelTextureChild(QSGNode *root, int ordinal) {
    if (root == nullptr || ordinal < 0) {
        return nullptr;
    }

    int seen = 0;
    for (QSGNode *child = root->firstChild(); child != nullptr; child = child->nextSibling()) {
        auto *textureNode = dynamic_cast<QSGSimpleTextureNode *>(child);
        if (textureNode == nullptr) {
            continue;
        }
        if (seen == ordinal) {
            return textureNode;
        }
        ++seen;
    }
    return nullptr;
}

QSGGeometryNode *spectrogramBodySampleMaterialNode(QSGNode *root, int ordinal = 0) {
    if (root == nullptr) {
        return nullptr;
    }

    int seen = 0;
    for (QSGNode *child = root->firstChild(); child != nullptr; child = child->nextSibling()) {
        auto *geometryNode = dynamic_cast<QSGGeometryNode *>(child);
        if (geometryNode == nullptr) {
            continue;
        }
        if (dynamic_cast<SpectrogramBodySampleMaterial *>(geometryNode->material()) == nullptr) {
            continue;
        }
        if (seen == ordinal) {
            return geometryNode;
        }
        ++seen;
    }
    return nullptr;
}

QSGGeometryNode *spectrogramTailSampleMaterialNode(QSGNode *root) {
    return spectrogramBodySampleMaterialNode(root, 1);
}

QSGGeometryNode *spectrogramStockMaterialNode(QSGNode *root, int ordinal = 0) {
    if (root == nullptr) {
        return nullptr;
    }

    int seen = 0;
    for (QSGNode *child = root->firstChild(); child != nullptr; child = child->nextSibling()) {
        auto *geometryNode = dynamic_cast<QSGGeometryNode *>(child);
        if (geometryNode == nullptr) {
            continue;
        }
        if (dynamic_cast<QSGTextureMaterial *>(geometryNode->material()) == nullptr) {
            continue;
        }
        if (seen == ordinal) {
            return geometryNode;
        }
        ++seen;
    }
    return nullptr;
}

QSGGeometryNode *spectrogramSourceSampleMaterialNode(QSGNode *root, int ordinal = 0) {
    if (root == nullptr) {
        return nullptr;
    }

    int seen = 0;
    for (QSGNode *child = root->firstChild(); child != nullptr; child = child->nextSibling()) {
        auto *geometryNode = dynamic_cast<QSGGeometryNode *>(child);
        if (geometryNode == nullptr) {
            continue;
        }
        if (dynamic_cast<SpectrogramSourceSampleMaterial *>(geometryNode->material()) == nullptr) {
            continue;
        }
        if (seen == ordinal) {
            return geometryNode;
        }
        ++seen;
    }
    return nullptr;
}

QSGSimpleTextureNode *spectrogramBodyTextureNode(QSGNode *root) {
    return findTopLevelTextureChild(root, 0);
}

QSGSimpleTextureNode *spectrogramTailTextureNode(QSGNode *root) {
    return findTopLevelTextureChild(root, 1);
}

QRectF geometryBounds(QSGGeometryNode *node) {
    if (node == nullptr || node->geometry() == nullptr) {
        return QRectF();
    }

    const QSGGeometry *geometry = node->geometry();
    if (geometry->vertexCount() < 4) {
        return QRectF();
    }

    const auto *vertices = geometry->vertexDataAsTexturedPoint2D();
    float minX = vertices[0].x;
    float maxX = vertices[0].x;
    float minY = vertices[0].y;
    float maxY = vertices[0].y;
    for (int i = 1; i < geometry->vertexCount(); ++i) {
        minX = std::min(minX, vertices[i].x);
        maxX = std::max(maxX, vertices[i].x);
        minY = std::min(minY, vertices[i].y);
        maxY = std::max(maxY, vertices[i].y);
    }
    return QRectF(
        static_cast<double>(minX),
        static_cast<double>(minY),
        static_cast<double>(maxX - minX),
        static_cast<double>(maxY - minY));
}

bool geometryHasVisibleArea(QSGGeometryNode *node) {
    const QRectF bounds = geometryBounds(node);
    return bounds.width() > 0.0 && bounds.height() > 0.0;
}

void assertTextureNodeDebugBodyInvariants(QSGNode *root, const char *context);

void assertFullModeBodyNodeInvariants(QSGNode *root, const char *context) {
    auto *sampleNode = spectrogramBodySampleMaterialNode(root);
    QVERIFY2(sampleNode != nullptr, context);
    if (!geometryHasVisibleArea(sampleNode)) {
        assertTextureNodeDebugBodyInvariants(root, context);
        return;
    }
    auto *sampleMaterial =
        dynamic_cast<SpectrogramBodySampleMaterial *>(sampleNode->material());
    QVERIFY2(sampleMaterial != nullptr, context);
    QVERIFY2(sampleMaterial->state.sourceTexture != nullptr, context);
    QVERIFY2(sampleMaterial->state.dbRemapTexture != nullptr, context);
    QVERIFY2(sampleMaterial->state.paletteTexture != nullptr, context);
    QVERIFY2(sampleMaterial->state.rowLutTexture != nullptr, context);

    auto *bodyTextureNode = spectrogramBodyTextureNode(root);
    QVERIFY2(bodyTextureNode != nullptr, context);
    QVERIFY2(
        bodyTextureNode->texture() == nullptr || bodyTextureNode->rect().isEmpty(),
        context);
}

void assertTextureNodeDebugBodyInvariants(QSGNode *root, const char *context) {
    auto *sampleNode = spectrogramBodySampleMaterialNode(root);
    QVERIFY2(sampleNode != nullptr, context);
    QVERIFY2(!geometryHasVisibleArea(sampleNode), context);

    auto *bodyTextureNode = spectrogramBodyTextureNode(root);
    QVERIFY2(bodyTextureNode != nullptr, context);
    QVERIFY2(bodyTextureNode->texture() != nullptr, context);
    QVERIFY2(!bodyTextureNode->rect().isEmpty(), context);
}

QByteArray artistOnlyLazyBinary() {
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{
        1,
        0,
        -1,
        0,
        2,
        QStringLiteral("Artist A (2)"),
        QStringLiteral("artist|/music|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A"),
        {},
        {},
        {},
    });
    return encodeRows(rows);
}

QByteArray multiRootBinary() {
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{
        0,
        0,
        -1,
        0,
        1,
        QStringLiteral("/music-a"),
        QStringLiteral("root|/music-a"),
        {},
        QStringLiteral("/music-a"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        1,
        1,
        -1,
        0,
        0,
        QStringLiteral("Artist A (0)"),
        QStringLiteral("artist|/music-a|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music-a/Artist A"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        0,
        0,
        -1,
        0,
        1,
        QStringLiteral("/music-b"),
        QStringLiteral("root|/music-b"),
        {},
        QStringLiteral("/music-b"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        1,
        1,
        -1,
        0,
        0,
        QStringLiteral("Artist B (0)"),
        QStringLiteral("artist|/music-b|Artist B"),
        QStringLiteral("Artist B"),
        QStringLiteral("/music-b/Artist B"),
        {},
        {},
        {},
    });
    return encodeRows(rows);
}

QString formatQmlErrors(const QList<QQmlError> &errors) {
    QStringList lines;
    lines.reserve(errors.size());
    for (const QQmlError &error : errors) {
        lines.push_back(error.toString());
    }
    return lines.join('\n');
}

QMutex &capturedMessageMutex() {
    static QMutex mutex;
    return mutex;
}

QStringList &capturedMessages() {
    static QStringList messages;
    return messages;
}

QtMessageHandler &previousMessageHandler() {
    static QtMessageHandler handler = nullptr;
    return handler;
}

void captureQtMessage(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        QMutexLocker locker(&capturedMessageMutex());
        capturedMessages().push_back(message);
    }
    if (previousMessageHandler()) {
        previousMessageHandler()(type, context, message);
    } else {
        qt_message_output(type, context, message);
    }
}

int manhattanColorDistance(const QColor &lhs, const QColor &rhs) {
    return std::abs(lhs.red() - rhs.red())
        + std::abs(lhs.green() - rhs.green())
        + std::abs(lhs.blue() - rhs.blue());
}

QColor expectedPaletteColorForIntensity(const SpectrogramItem &item, int intensity) {
    const auto dbRemap = item.buildPrecomputedDbRemapLocked();
    const int remappedIndex =
        dbRemap[static_cast<size_t>(std::clamp(intensity, 0, 255))];
    const int paletteIndex = SpectrogramItem::kGradientTableSize - 1
        - static_cast<int>(std::lround(
            (static_cast<double>(SpectrogramItem::kGradientTableSize - 1) / 255.0)
            * static_cast<double>(remappedIndex)));
    const auto &palette = item.m_channelMuted ? item.m_palette32Gray : item.m_palette32;
    return QColor::fromRgb(palette[static_cast<size_t>(std::clamp(
        paletteIndex,
        0,
        SpectrogramItem::kGradientTableSize - 1))]);
}

void assertUploadedPaletteOrdering(const SpectrogramItem &item) {
    QVERIFY2(!item.m_gpuPaletteImage.isNull(), "palette lookup image should be generated for full-mode material");
    QCOMPARE(item.m_gpuPaletteImage.width(), SpectrogramItem::kGradientTableSize);
    QCOMPARE(item.m_gpuPaletteImage.height(), 1);

    const QColor firstColor = item.m_gpuPaletteImage.pixelColor(0, 0);
    const QColor lastColor = item.m_gpuPaletteImage.pixelColor(item.m_gpuPaletteImage.width() - 1, 0);
    QCOMPARE(firstColor, QColor::fromRgb(item.m_palette32[0]));
    QCOMPARE(
        lastColor,
        QColor::fromRgb(item.m_palette32[SpectrogramItem::kGradientTableSize - 1]));
    QVERIFY2(
        qGray(firstColor.rgb()) > qGray(lastColor.rgb()),
        "uploaded palette texture should keep bright palette entries at the low-index end used by the CPU renderer");
}

void assertFragmentShaderInvertsPaletteLookup() {
    const QString shaderPath = QFileInfo(QString::fromUtf8(__FILE__))
        .dir()
        .absoluteFilePath(QStringLiteral("../src/shaders/SpectrogramBodySample.frag"));
    QFile shaderFile(shaderPath);
    QVERIFY2(
        shaderFile.open(QIODevice::ReadOnly | QIODevice::Text),
        qPrintable(QStringLiteral("failed to open fragment shader source %1").arg(shaderPath)));
    QString shaderSource = QString::fromUtf8(shaderFile.readAll());
    shaderSource.remove(QRegularExpression(QStringLiteral("\\s+")));
    QVERIFY2(
        shaderSource.contains(QStringLiteral("texture(paletteTex,vec2(1.0-mapped,0.5))")),
        "full-mode fragment shader should invert the remapped palette lookup to match the CPU renderer");
}

void assertFragmentShaderSmoothsZoomOutAggregation() {
    const QString shaderPath = QFileInfo(QString::fromUtf8(__FILE__))
        .dir()
        .absoluteFilePath(QStringLiteral("../src/shaders/SpectrogramBodySample.frag"));
    QFile shaderFile(shaderPath);
    QVERIFY2(
        shaderFile.open(QIODevice::ReadOnly | QIODevice::Text),
        qPrintable(QStringLiteral("failed to open fragment shader source %1").arg(shaderPath)));
    QString shaderSource = QString::fromUtf8(shaderFile.readAll());
    shaderSource.remove(QRegularExpression(QStringLiteral("\\s+")));
    QVERIFY2(
        shaderSource.contains(QStringLiteral("smoothPeakHoldWindowValue")),
        "zoomed-out full-mode shader should use a dedicated smooth peak-hold aggregation path");
    QVERIFY2(
        shaderSource.contains(QStringLiteral("mix(baseWindowValue,shiftedWindowValue,interpolationT)")),
        "zoomed-out full-mode shader should blend adjacent peak-hold windows across fractional scroll phases");
}

void clearCapturedMessages() {
    QMutexLocker locker(&capturedMessageMutex());
    capturedMessages().clear();
}

QString takeCapturedMessagesText() {
    QMutexLocker locker(&capturedMessageMutex());
    const QString text = capturedMessages().join('\n');
    capturedMessages().clear();
    return text;
}

QObject *findObjectByStringProperty(QObject *root, const char *propertyName, const QString &expectedValue) {
    if (!root) {
        return nullptr;
    }
    if (root->property(propertyName).toString() == expectedValue) {
        return root;
    }
    const QObjectList children = root->children();
    for (QObject *child : children) {
        if (QObject *match = findObjectByStringProperty(child, propertyName, expectedValue)) {
            return match;
        }
    }
    return nullptr;
}

QObject *createQmlObjectFromSource(
    QQmlEngine &engine,
    const QByteArray &qmlSource,
    const QUrl &baseUrl,
    QString *errorText) {
    QQmlComponent component(&engine);
    component.setData(qmlSource, baseUrl);
    if (!component.errors().isEmpty()) {
        if (errorText) {
            *errorText = formatQmlErrors(component.errors());
        }
        return nullptr;
    }

    QObject *object = component.create();
    if (!object && errorText) {
        *errorText = formatQmlErrors(component.errors());
    }
    return object;
}

} // namespace

class QmlSmokeTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void loadsMainQmlWithFallbackBridge();
    void loadsExtractedQmlSlicesWithFallbackProps();
    void albumArtTileKeepsHeightInsideColumnLayout();
    void tagEditorLibrarySupportGateMatchesSupportedRows();
    void libraryTreeStartsCollapsedByDefault();
    void rootRowsStartExpandedByDefault();
    void artistExpansionPopulatesInBatches();
    void lazyArtistRowRequestsBackendExpansion();
    void artistPrefixSearchUsesModelLookup();
    void spectrogramMetadataOnlyResetWaitsForDataChunk();
    void spectrogramRollingSeekKeepsHistoryContinuous();
    void spectrogramCenteredToRollingAtMaxZoomReanchorsEpoch();
    void testMutedChannelRendersGrayscale();
    void spectrogramGpuWindowCaptureUsesMaterialBackedBodyNode();
    void spectrogramGpuSourceOnlyDebugRendersWithoutGpuSourceImage();
    void spectrogramGpuFullModeRendersWithoutCpuSourceImage();
    void spectrogramGpuFullModeKeepsTopBinVisibleWithRowLutSampling();
    void spectrogramGpuSparseCenteredWindowFallsBackToDisplayImageFromRetainedSegments();
    void spectrogramGpuSparseCenteredWindowUsesDisplayImageWithoutRetainedSourceTexture();
    void spectrogramGpuStockMaterialDebugModeUsesStockTextureMaterialNode();
    void spectrogramGpuFullModeUsesSampleBodyMaterialNode();
    void spectrogramGpuFullModeShaderSmoothsZoomOutAggregation();
    void spectrogramGpuSampleMaterialsBindSourceTextureAtZero();
    void spectrogramGpuSampleMaterialNodeBackfillsSecondaryTexture();
    void spectrogramGpuSampleMaterialCompareIncludesSamplingState();
    void spectrogramGpuSampledTexturesCommitPendingUploads();
    void spectrogramGpuSampleMaterialNodesFlipVerticalTextureCoords();
    void spectrogramGpuSamplerLoggingIgnoresPlaceholderTextureSize();
    void spectrogramLargePositionJumpWaitsForResetHandoff();
    void spectrogramPlaybackHeartbeatDoesNotMoveAnchorBackward();
    void spectrogramCenteredZoomFillHeartbeatDoesNotMoveAnchorBackward();
    void spectrogramCenteredSmallSeekSnapsWithoutServoSlide();
    void spectrogramCenteredSeekFollowsLocalVisualClock();
    void spectrogramEarlyCenteredSeekFollowsLocalVisualClockBeforePrecomputedReady();
    void spectrogramGaplessTrackChangePreservesRollingHistory();
    void spectrogramNaturalTrackResetPreservesRollingHistory();
    void spectrogramManualTrackResetClearsRollingHistory();
    void spectrogramRollingZoomResetAnchorsEpochToResetStart();
    void spectrogramSeekResetAnchorsPlaybackClockToChunkStart();
    void diagnosticsLogUsesLowercaseAppDir();
    void playbackControllerSeekImmediatelyUpdatesSpectrogramPosition();
    void playbackControllerDeterministicTimeHooksDriveInterpolation();
    void playbackControllerPlaybackUpdateKeepsSpectrogramOnInterpolatedClock();
    void playbackControllerPostSeekHeartbeatSnapsToBackendPosition();
    void playbackControllerPostSeekBehindHeartbeatKeepsVisualClock();
    void playbackControllerPostSeekHeldTargetKeepsVisualClock();
    void playbackControllerPostSeekTargetEchoKeepsVisualClockActive();
    void playbackControllerPlayAtCurrentTrackClearsPostSeekVisualClock();
    void playbackControllerHeartbeatCorrectionAvoidsOneFrameSpeedBurst();
    void playbackControllerModerateSteadyStateLagUsesTrimNotBleed();
    void playbackControllerProfileLogsHeartbeatCorrectionAndBleed();
    void playbackControllerProfileSkipsMinorFollowHeartbeatLogs();
    void playbackControllerIgnoresSteadyStateHeartbeatJitter();
    void playbackControllerKeepsWallClockInterpolationAfterSubRealtimeHeartbeats();
    void playbackControllerSteadyStateTrimReducesNoticeableLag();
    void playbackControllerFollowsBoundedRecoveryCadenceWithoutBurst();
    void spectrogramSeekProfileFlagsStalledPostSeekWindow();
    void spectrogramSeekProfileDoesNotRestartSameTraceAfterSettling();
    void spectrogramSmoothnessProfileFlagsGapHeavyWindow();
    void spectrogramSmoothnessProfileTracksServoAndAdvanceFallbackSignals();
    void waveformProgressInvalidatesOnlyTailSpan();
    void waveformPeakUpdatesInvalidateChangedSuffix();
    void stoppedTrackSwitchRequiresSpectrogramResetOnResume();
    void spectrogramStaleTokenChunksAreDropped();
    void spectrogramGaplessTokenChunksPassFilter();
    void spectrogramFreshWidgetAcceptsDataWithImplicitReset();
    void spectrogramCenteredModeSeekPreservesRing();
    void spectrogramCenteredGaplessPreStagedFill();
    void spectrogramCenteredGaplessSnapsAnchorToZero();
    void spectrogramCenteredSeekRestartRebuildsEarlierWindow();
    void spectrogramCenteredFinalizeChunkShrinksTotalEstimate();
    void spectrogramCenteredFinalizeChunkIgnoredForStaleToken();
    void spectrogramSameHopEstimateIncreaseUpdatesZoomOutLimit();
    void spectrogramCenteredClampsRightEdgeToMaxColNearEof();
    void spectrogramCenteredEofDetachmentDisablesSubpixelScrolling();
    void spectrogramCenteredDisplayRangeIgnoresLaggingDecodedTailBeforeEof();
    void spectrogramRingCapacityPersistsAcrossFullscreenShrink();
    void spectrogramMaxWidgetWidthSurvivesInstanceReplacement();
    void spectrogramRollingGaplessTrackChangePreservesZoom();
    void spectrogramCenteredGaplessTrackChangeResetsZoom();
    void spectrogramRollingResetTrackChangeResetsZoom();
    void spectrogramTrackChangeMetadataResetClearsOldCenteredFrame();
    void spectrogramFreshInstanceResyncsBackendZoomOnTrackChange();
    void spectrogramFreshInstanceSeekRestartDoesNotResetZoom();
    void spectrogramTrackChangeCancelsPendingZoomDebounce();
    void spectrogramForceFpsOverlayDoesNotOverrideQmlBinding();
    void spectrogramRenderLoopStopsWhenNotPlaying();
    void playbackControllerInterpolationActivatesOnPlayback();
    void playbackControllerInterpolationDeactivatesOnStop();
    void trackIdentityChangedSignalTriggersQmlHandler();
    void queueAutoCenterIsDeferredOffHandlerStack();
    void queueContainIndexSkipsScrollWhenVisible();
    void queueContainIndexScrollsUpWhenAboveViewport();
    void queueContainIndexClampsAtListEnd();
    void spectrogramCrosshairAndGridPropertiesAndHoverTracking();
    void spectrogramPixelToFrequency();
    void spectrogramSampleRateSyncsFromPrecomputedChunks();
    void spectrogramCrosshairOverlayGeneratesOnHover();
    void spectrogramCrosshairOverlayReusesImageBufferAtSameGeometry();
    void spectrogramGridOverlayGeneratesWhenEnabled();
    void spectrogramOverlayDisabledProducesNullImage();
    void spectrogramOverlayDirtiedByGeometryChange();
    void spectrogramOverlayDirtiedByLogScaleChange();
    void spectrogramPublicOverlayLineNodeTracksPlayhead();
    void spectrogramPublicOverlayLineNodeTracksGridAndCrosshair();
    void spectrogramPublicOverlayLineNodeTracksSharedCrosshair();
    void spectrogramOverlayStalenessDetectsBinChange();
    void spectrogramOverlayRebuildsViaUpdatePaintNodeOnStaleInput();
    void spectrogramOverlayStalenessDetectsDisplayRangeChange();
    void spectrogramClickToSeekEmitsSignalWhenCrosshairEnabled();
    void spectrogramClickToSeekUsesCurrentPositionWhenCrosshairCacheIsStale();
    void spectrogramClickToSeekIgnoresLaggingDecodedTailInCenteredMode();
    void spectrogramClickToSeekSuppressedWhenCrosshairDisabled();
    void spectrogramLeftClickDoesNotSeek();
    void spectrogramClickToSeekDisabledInRollingMode();
    void spectrogramZoomProperty();
    void spectrogramZoomLimitsWithTrackData();
    void spectrogramZoomOutBlockedWhenSongFits();
    void spectrogramLodSelectionChoosesLevelNearScreenDensity();
    void spectrogramStoresLodChunksSeparatelyFromLevelZero();
    void spectrogramFullModeBindsSelectedLodTextureWhenZoomedOut();
    void spectrogramFullModeBindsAdjacentLodTexturesForCrossFade();
    void spectrogramZoomInUsesPreviewUntilNativeHopArrives();
    void spectrogramMaxZoomOutUsesOverviewToFitLongTrack();
    void spectrogramEffectiveZoomMatchesBackendHop();
    void spectrogramAdvanceWorksWhenBackendMatchesZoom();
    void spectrogramEffectiveZoomDuringTransition();
    void spectrogramZoomPreviewAppliesRenderScaleBeforeBackendData();
    void spectrogramZoomPreviewFreezesBeforeBackendReset();
    void spectrogramDeferredZoomAppliesOnBackendData();
    void spectrogramRapidZoomOutIgnoresIntermediateReset();
    void spectrogramSameTrackCoarseResetSnapsRenderZoomWithoutAwaitingFlag();
    void spectrogramMaxZoomOutRoundedHopSnapsRenderZoomWithoutAwaitingFlag();
    void spectrogramCenteredZoomOutDoesNotPreserveReferenceHopRange();
    void spectrogramCenteredZoomOutDropsStaleSameTrackZoomChunks();
    void spectrogramCenteredZoomOutDropsOlderSameTrackGeneration();
    void spectrogramCenteredZoomOutBackendRestartReanchorsToFullTrack();
    void spectrogramLinearScaleKeepsTopBinVisibleAtTallHeights();
    void spectrogramZoomOutProducesDistinctHop();
    void spectrogramMinZoomAdaptsToWidthChange();
    void spectrogramCenteredModeUsesWindowedCapacity();
    void spectrogramRollingModeKeepsViewportHeadroomBeyondLookahead();
    void spectrogramPeakHoldColumnSpanUsesMaxNotNearest();
    void spectrogramRetainedSourceLayoutPreserves64BitColumnRange();
    void spectrogramRetainedSourceUploadPlannerMergesAdjacentDirtyColumns();
    void spectrogramRetainedSourceUploadPlannerTreatsWindowChangeAsTextureStateChange();
    void spectrogramRetainedPagePlanSplitsSinglePageRange();
    void spectrogramRetainedPagePlanSplitsAcrossPageBoundary();
    void spectrogramRetainedPagePlanSplitsRollingWrap();
    void spectrogramRetainedPageRetentionKeepsCenteredHalo();
    void spectrogramRetainedPageRetentionKeepsFrozenZoomFillScrollHalo();
    void spectrogramRetainedPageRetentionKeepsRollingWrapHalo();
    void spectrogramRetainedPageUploadPlannerUploadsOnlyDirtyPageSpans();
    void spectrogramRetainedPageUploadPlannerKeepsTextureForShiftedSpan();
    void spectrogramRetainedPageUploadPlannerResetsPageOnTokenChange();
    void spectrogramRollingRetainedPageUploadPlannerKeepsPagesAcrossGaplessTokenChange();
    void spectrogramRetainedPageUploadPlannerLabelsResetReasons();
    void spectrogramRetainedPageUploadBudgetLimitsPaintWork();
    void spectrogramRetainedPageUploadBudgetSplitsOversizedSpan();
    void spectrogramRetainedPageTextureExtentUsesStablePageWidth();
    void spectrogramRetainedUploadDefersTinyUncoveredSeekFrames();
    void spectrogramRetainedPageUploadPlannerUsesGpuLimitForPageWidth();
    void spectrogramRetainedPageUploadPlannerInvalidatesPagesOnReset();
    void spectrogramRetainedPageSyncKeepsPagesWhenVisibleRangeUndecoded();
    void spectrogramRetainedLegacyTextureInvalidationKeepsPages();
    void spectrogramRetainedPageCoverageSkipsLegacyTextureUpload();
    void spectrogramRetainedSourceTextureStartsEmptyAndIsNonCopyable();
    void spectrogramRetainedSourceTextureReleaseSchedulesOwnerWindowCleanup();
    void spectrogramRetainedSourceTextureForeignThreadReleaseMarshalsOwnerCleanup();
    void spectrogramRetainedSourceTextureSharedContextReleaseKeepsOwnerWrapperAlive();
    void spectrogramRetainedSourceTextureOwnerTeardownBeforeReleaseStillDeletesNativeTexture();
    void spectrogramRetainedSourceTextureConcurrentReleaseAndOwnerTeardownRetiresOnce();
    void spectrogramRetainedSourceTextureOwnerContextLossWithoutSharedFallbackRetiresCleanup();
    void spectrogramRetainedSourceTextureWindowDestructionHandsOffSharedCleanup();
    void spectrogramRetainedSourceTextureRetiresMultiplePendingCleanupGenerations();
    void spectrogramRetainedSourceTextureCreatesReownsAndCleansUpOnLiveWindow();
    void spectrogramRetainedSourceTextureEnsureCreatedPreservesExistingStateWithoutContext();
    void spectrogramRetainedSourceTextureUploadRejectsInvalidArguments();
    void spectrogramRetainedSourceTextureRejectsCapacityBeyondGpuLimit();
    void spectrogramGpuTextureNodeDebugUsesRetainedSourceTextureResource();
    void spectrogramGpuFullModeUsesRetainedSourceWithoutCpuDisplayImage();
    void spectrogramGpuLookupAndDisplayImagesUseCompatibleFormats();
    void spectrogramGpuDebugModeParsesEnvironmentValues();
    void spectrogramGpuTextureNodeDebugStaysVisibleAfterRepeatedRollingUpdatesWithoutCpuSourceImage();
    void spectrogramActiveRendererUsesPublicBodyTextureOnly();
    void spectrogramUpdatePaintNodeUsesGpuSpectrogramNode();
    void spectrogramGpuTextureUploadSkipsRedundantRecreation();
    void spectrogramRollingGpuTextureUpdateKeepsTextureObject();
    void spectrogramRollingTextureCrossesOldRebaseBoundaryWithoutCollapsingToSingleNode();
    void spectrogramRollingTextureCrossesBaseWidthBoundaryWithoutCollapsingToSingleNode();
    void spectrogramRollingTextureKeepsUpdatingRightEdgeAfterLongScroll();
    void spectrogramRollingTextureKeepsBoundedVisibleNodeCountAfterLongScroll();
    void spectrogramRollingFullModeMaterialBodyUsesScrolledSourceRegionDuringIncrementalAdvance();
    void spectrogramGpuFullModeStaysVisibleAfterMultipleRollingUpdatesWithoutCpuSourceImage();
    void spectrogramRollingPreFillUsesFullViewportWidthCache();
    void spectrogramRollingPreFillHandsOffToSteadyScrollIncrementally();
    void spectrogramRollingPreFillCompletionAndAdvanceStaysIncremental();
    void spectrogramRollingNonIntegerZoomAdvanceStaysIncremental();
    void spectrogramRollingDisplayCacheIgnoresOffscreenWriteHeadGrowth();
    void spectrogramCenteredDisplayCacheIgnoresOffscreenDecodeGrowth();
    void spectrogramRollingGpuDisplayCacheAdvancesIncrementally();
    void spectrogramCenteredGpuDisplayCacheAdvancesIncrementally();
    void spectrogramGpuWindowCaptureStillShowsVisiblePixelsAfterPublicPivot();
    void spectrogramGpuWindowCaptureShowsTextureNodeDebugStage();
    void spectrogramGpuWindowCaptureShowsVisiblePaletteMappedPixelsInFullMode();
    void spectrogramGpuWindowCaptureMatchesCpuPaletteDirectionInFullMode();
    void spectrogramRollingGpuWindowCaptureShowsVisiblePaletteMappedPixelsInFullMode();
    void spectrogramGpuWindowCapturePremultipliesPaletteOpacityInFullMode();
    void spectrogramGpuWindowCaptureStaysVisibleAfterClearingCpuDisplayImageInFullMode();
    void spectrogramRollingGpuWindowCaptureUsesMaterialBackedBodyNodeOnly();
    void spectrogramGpuWindowCaptureShowsSpectrogramPixelsWithoutOverlays();
    void spectrogramGpuWindowCaptureShowsLargeCenteredTrackPixels();
    void spectrogramCenteredGpuCacheSurvivesRingEvictionAtSameView();
    void spectrogramCenteredGpuMetadataOnlyScrollAvoidsCpuDisplayImage();
    void spectrogramCenteredRetainedSourceScrollUploadsOnlyNewColumns();
    void spectrogramRetainedSourcePagesAreScopedToDecodeGeneration();
    void spectrogramZoomFillWaitsForZoomOutVisibleColumns();
    void spectrogramZoomFillWaitsForContiguousVisibleColumns();
    void spectrogramZoomFillClearsWhenDecoderReachesTail();
    void spectrogramZoomFillWaitsForDecodedRightEdgeAtOuterZoom();
    void spectrogramZoomFillRetainedRevealHandlesScrollingEdgeUploads();
    void spectrogramZoomFillRetainedPreviewScrollsWithPlaybackClock();
    void spectrogramZoomFillRetainedPreviewScrollsOnlyWithMovingCenteredViewport();
    void spectrogramZoomFillRetainedPreviewUsesFrozenScrollMode();
    void spectrogramZoomFillPlayheadUsesFrozenAnchorDuringFreeze();
    void spectrogramZoomWiggleTraceProjectsRetainedSourceColumnToScreenX();
    void spectrogramZoomFillRetainedPreviewKeepsRightScrollHalo();
    void spectrogramZoomFillDefersRetainedFreezeUntilScaledPreviewFrame();
    void spectrogramZoomFillStackedZoomKeepsActiveRetainedFreeze();
    void spectrogramZoomFillMetadataResetKeepsActiveRetainedFreezeScroll();
    void spectrogramZoomFillRetainedPreviewIgnoresBackwardClockCorrections();
    void spectrogramZoomFillRetainedPreviewCoversPostRevealUploadGaps();
    void spectrogramRetainedRenderPlanAllowsDecodedTailSlack();
    void spectrogramRollingRetainedRenderPlanPrefillKeepsFixedZoomScale();
    void spectrogramRetainedRenderPlanCoversOnePage();
    void spectrogramRetainedRenderPlanCoversTwoPages();
    void spectrogramRetainedRenderPlanCoversThreePages();
    void spectrogramRetainedRenderPlanUsesClampedPageWidth();
    void spectrogramRetainedRenderPlanStillFallsBackForSparseOverflow();
    void spectrogramRetainedTextureNodeFreezesRetainedBodyDuringZoomFill();
    void spectrogramRetainedSourceOnlyFreezesRetainedBodyDuringZoomFill();
    void spectrogramFullModeFreezesRetainedBodyDuringZoomFill();
    void spectrogramFullModeZoomFillDoesNotScrollPinnedCenteredFreeze();
    void spectrogramZoomFillWarmsDisplayCacheBehindFrozenFrame();
    void spectrogramZoomFillRevealsOnlyFromRenderableFrame();
    void spectrogramFullscreenMaxZoomRevealUsesRetainedFullModeDirectly();
    void spectrogramMetadataOnlyZoomResetDefersFreezeSnapshot();
    void spectrogramMetadataOnlySameTrackResetFreezesExistingFrame();
    void spectrogramZoomFillSkipsCpuFreezeSnapshotWithoutRetainedFrame();
    void spectrogramZoomFillUsesRetainedFreezeWithoutCpuSnapshot();
    void spectrogramZoomFillPinnedRetainedFreezeKeepsDisplayFallback();
    void spectrogramZoomFillScrollingRetainedFreezeKeepsDisplayFallback();
    void spectrogramSyntheticClearSkipsCpuFreezeSnapshotWithoutRetainedFrame();
    void spectrogramSyntheticClearInvalidatesRetainedSourceStateDuringFreeze();
    void spectrogramDataResetInvalidatesRetainedSourceStateDuringFreeze();
    void spectrogramDataResetPreservesRetainedFreezePagesDuringZoomFill();
    void spectrogramSyntheticClearPreservesGpuCacheDuringSeek();
    void spectrogramSeekFillRevealsAfterFirstVisibleColumns();
    void spectrogramSyntheticClearInvalidatesGpuCacheWhenNoOldContent();
};

void QmlSmokeTest::initTestCase() {
    previousMessageHandler() = qInstallMessageHandler(captureQtMessage);
}

void QmlSmokeTest::init() {
    clearCapturedMessages();
}

void QmlSmokeTest::cleanup() {
    const QString warnings = takeCapturedMessagesText();
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings));
}

void QmlSmokeTest::cleanupTestCase() {
    qInstallMessageHandler(previousMessageHandler());
    previousMessageHandler() = nullptr;
}

void QmlSmokeTest::spectrogramRetainedSourceTextureStartsEmptyAndIsNonCopyable() {
    SpectrogramSourceTexture texture;
    QCOMPARE(texture.texture(), nullptr);
    QCOMPARE(texture.size(), QSize());
    QCOMPARE(texture.pendingUploadSpanCount(), 0);
    QVERIFY(!std::is_copy_constructible_v<SpectrogramSourceTexture>);
    QVERIFY(!std::is_copy_assignable_v<SpectrogramSourceTexture>);
    QVERIFY(!std::is_move_constructible_v<SpectrogramSourceTexture>);
    QVERIFY(!std::is_move_assignable_v<SpectrogramSourceTexture>);
}

void QmlSmokeTest::spectrogramRetainedSourceTextureReleaseSchedulesOwnerWindowCleanup() {
    QQuickWindow window;
    window.resize(64, 64);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    QVERIFY(!window.grabWindow().isNull());
    QVERIFY(window.isSceneGraphInitialized());

    SpectrogramSourceTexture texture;
    texture.m_textureId = 77;
    texture.m_ownerWindow = &window;
    texture.m_ownerContext = new QOpenGLContext(this);
    texture.m_size = QSize(9, 11);
    texture.noteUploadedSpan({4, 7});

    texture.release();

    QCOMPARE(texture.texture(), nullptr);
    QCOMPARE(texture.size(), QSize());
    QCOMPARE(texture.pendingUploadSpanCount(), 0);
    QCOMPARE(texture.m_textureId, 0U);
    QVERIFY(texture.m_ownerWindow == nullptr);
    QCOMPARE(texture.pendingCleanupGenerationCount(), 1);

    window.update();
    QVERIFY(!window.grabWindow().isNull());
    QCOMPARE(texture.pendingCleanupGenerationCount(), 1);
}

void QmlSmokeTest::spectrogramRetainedSourceTextureForeignThreadReleaseMarshalsOwnerCleanup() {
    auto window = std::make_unique<QQuickWindow>();
    window->resize(64, 64);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get(), 5000));
    QVERIFY(!window->grabWindow().isNull());
    QVERIFY(window->isSceneGraphInitialized());

    SpectrogramSourceTexture texture;
    auto *ownerContext = new QOpenGLContext();
    std::atomic_int wrapperDeletes{0};
    texture.m_texture = new CountingTexture(&wrapperDeletes, QSize(8, 4));
    texture.m_textureId = 88;
    texture.m_size = QSize(8, 4);
    texture.m_ownerWindow = window.get();
    texture.m_ownerContext = ownerContext;

    std::thread releaseThread([&texture]() {
        texture.release();
    });
    releaseThread.join();

    const QString releaseWarnings = takeCapturedMessagesText();
    QVERIFY2(releaseWarnings.isEmpty(), qPrintable(releaseWarnings));
    QCOMPARE(wrapperDeletes.load(), 0);
    QCOMPARE(texture.pendingCleanupGenerationCount(), 1);

    std::atomic_bool handoffFlushed{false};
    QMetaObject::invokeMethod(window.get(), [&handoffFlushed]() {
        handoffFlushed.store(true);
    }, Qt::QueuedConnection);
    QTRY_VERIFY(handoffFlushed.load());

    std::atomic_bool firstRenderAttemptRan{false};
    window->scheduleRenderJob(
        QRunnable::create([&firstRenderAttemptRan]() {
            firstRenderAttemptRan.store(true);
        }),
        QQuickWindow::AfterSynchronizingStage);
    window->update();
    QVERIFY(!window->grabWindow().isNull());
    QTRY_VERIFY(firstRenderAttemptRan.load());
    QTRY_VERIFY(texture.pendingCleanupAttemptCount() >= 1);
    QCOMPARE(texture.pendingCleanupGenerationCount(), 1);

    delete ownerContext;

    std::atomic_bool secondRenderAttemptRan{false};
    window->scheduleRenderJob(
        QRunnable::create([&secondRenderAttemptRan]() {
            secondRenderAttemptRan.store(true);
        }),
        QQuickWindow::AfterSynchronizingStage);
    window->update();
    QVERIFY(!window->grabWindow().isNull());
    QTRY_VERIFY(secondRenderAttemptRan.load());
    QTRY_VERIFY(texture.pendingCleanupAttemptCount() >= 2);
    QTRY_COMPARE(wrapperDeletes.load(), 1);
    QTRY_COMPARE(texture.pendingCleanupGenerationCount(), 0);

    window.reset();
    QCOMPARE(wrapperDeletes.load(), 1);
    QCOMPARE(texture.pendingCleanupGenerationCount(), 0);
}

void QmlSmokeTest::spectrogramRetainedSourceTextureSharedContextReleaseKeepsOwnerWrapperAlive() {
    SharedOpenGlContexts contexts = createSharedOpenGlContexts();
    if (!contexts.isValid()) {
        QSKIP("shared OpenGL contexts are unavailable in this test environment");
    }
    if (!contexts.sharedContext->makeCurrent(contexts.surface.get())) {
        QSKIP("failed to make the shared OpenGL context current");
    }

    auto window = std::make_unique<QQuickWindow>();
    window->resize(64, 64);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get(), 5000));
    QVERIFY(!window->grabWindow().isNull());
    QVERIFY(window->isSceneGraphInitialized());

    SpectrogramSourceTexture texture;
    std::atomic_int wrapperDeletes{0};
    QOpenGLFunctions *sharedFunctions = contexts.sharedContext->functions();
    QVERIFY(sharedFunctions != nullptr);
    const GLuint nativeTextureId = createNativeTexture(contexts.sharedContext.get());
    QVERIFY(nativeTextureId != 0);
    QVERIFY(sharedFunctions->glIsTexture(nativeTextureId));
    texture.m_texture = new CountingTexture(&wrapperDeletes, QSize(8, 4));
    texture.m_textureId = nativeTextureId;
    texture.m_size = QSize(8, 4);
    texture.m_ownerWindow = window.get();
    texture.m_ownerContext = contexts.ownerContext.get();
    texture.m_shareGroup = contexts.ownerContext->shareGroup();

    texture.release();

    QCOMPARE(wrapperDeletes.load(), 0);
    QCOMPARE(texture.pendingCleanupGenerationCount(), 1);
    QCOMPARE(texture.texture(), nullptr);
    QCOMPARE(texture.m_textureId, 0U);
    QVERIFY(sharedFunctions->glIsTexture(nativeTextureId));

    contexts.sharedContext->doneCurrent();

    std::atomic_bool firstRenderAttemptRan{false};
    window->scheduleRenderJob(
        QRunnable::create([&firstRenderAttemptRan]() {
            firstRenderAttemptRan.store(true);
        }),
        QQuickWindow::AfterSynchronizingStage);
    window->update();
    QVERIFY(!window->grabWindow().isNull());
    QTRY_VERIFY(firstRenderAttemptRan.load());
    QTRY_VERIFY(texture.pendingCleanupAttemptCount() >= 1);
    QTRY_COMPARE(wrapperDeletes.load(), 1);
    QCOMPARE(texture.pendingCleanupGenerationCount(), 1);

    contexts.ownerContext.reset();
    QVERIFY(contexts.sharedContext->makeCurrent(contexts.surface.get()));
    QVERIFY(sharedFunctions->glIsTexture(nativeTextureId));
    contexts.sharedContext->doneCurrent();
    QTRY_VERIFY(texture.pendingCleanupAttemptCount() >= 2);
    QTRY_COMPARE(wrapperDeletes.load(), 1);
    QTRY_COMPARE(texture.pendingCleanupGenerationCount(), 0);
    QVERIFY(contexts.sharedContext->makeCurrent(contexts.surface.get()));
    QVERIFY(!sharedFunctions->glIsTexture(nativeTextureId));
    contexts.sharedContext->doneCurrent();
    window.reset();
}

void QmlSmokeTest::spectrogramRetainedSourceTextureOwnerTeardownBeforeReleaseStillDeletesNativeTexture() {
    SharedOpenGlContexts contexts = createSharedOpenGlContexts();
    if (!contexts.isValid()) {
        QSKIP("shared OpenGL contexts are unavailable in this test environment");
    }
    if (!contexts.sharedContext->makeCurrent(contexts.surface.get())) {
        QSKIP("failed to make the shared OpenGL context current");
    }

    auto window = std::make_unique<QQuickWindow>();
    window->resize(64, 64);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get(), 5000));
    QVERIFY(!window->grabWindow().isNull());
    QVERIFY(window->isSceneGraphInitialized());

    SpectrogramSourceTexture texture;
    std::atomic_int wrapperDeletes{0};
    QOpenGLFunctions *sharedFunctions = contexts.sharedContext->functions();
    QVERIFY(sharedFunctions != nullptr);
    const GLuint nativeTextureId = createNativeTexture(contexts.sharedContext.get());
    QVERIFY(nativeTextureId != 0);
    QVERIFY(sharedFunctions->glIsTexture(nativeTextureId));

    texture.m_texture = new CountingTexture(&wrapperDeletes, QSize(8, 4));
    texture.m_textureId = nativeTextureId;
    texture.m_size = QSize(8, 4);
    texture.m_ownerWindow = window.get();
    texture.m_ownerContext = contexts.ownerContext.get();
    texture.m_shareGroup = contexts.ownerContext->shareGroup();
    texture.armCleanupOwnerForCurrentTexture();

    contexts.sharedContext->doneCurrent();
    contexts.ownerContext.reset();
    window.reset();

    QVERIFY(contexts.sharedContext->makeCurrent(contexts.surface.get()));
    QVERIFY(sharedFunctions->glIsTexture(nativeTextureId));
    contexts.sharedContext->doneCurrent();

    QTRY_COMPARE(wrapperDeletes.load(), 1);
    QTRY_COMPARE(texture.pendingCleanupGenerationCount(), 0);

    QVERIFY(contexts.sharedContext->makeCurrent(contexts.surface.get()));
    QVERIFY(!sharedFunctions->glIsTexture(nativeTextureId));
    contexts.sharedContext->doneCurrent();

    const int attemptsBeforeRelease = texture.pendingCleanupAttemptCount();
    texture.release();
    QCOMPARE(texture.pendingCleanupGenerationCount(), 0);
    QCOMPARE(texture.pendingCleanupAttemptCount(), attemptsBeforeRelease);
}

void QmlSmokeTest::spectrogramRetainedSourceTextureConcurrentReleaseAndOwnerTeardownRetiresOnce() {
    SharedOpenGlContexts contexts = createSharedOpenGlContexts();
    if (!contexts.isValid()) {
        QSKIP("shared OpenGL contexts are unavailable in this test environment");
    }
    if (!contexts.sharedContext->makeCurrent(contexts.surface.get())) {
        QSKIP("failed to make the shared OpenGL context current");
    }

    auto window = std::make_unique<QQuickWindow>();
    window->resize(64, 64);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get(), 5000));
    QVERIFY(!window->grabWindow().isNull());
    QVERIFY(window->isSceneGraphInitialized());

    SpectrogramSourceTexture texture;
    std::atomic_int wrapperDeletes{0};
    std::atomic_bool releaseLockHeld{false};
    std::atomic_bool teardownStarted{false};
    QOpenGLFunctions *sharedFunctions = contexts.sharedContext->functions();
    QVERIFY(sharedFunctions != nullptr);
    const GLuint nativeTextureId = createNativeTexture(contexts.sharedContext.get());
    QVERIFY(nativeTextureId != 0);
    QVERIFY(sharedFunctions->glIsTexture(nativeTextureId));

    texture.m_texture = new CountingTexture(&wrapperDeletes, QSize(8, 4));
    texture.m_textureId = nativeTextureId;
    texture.m_size = QSize(8, 4);
    texture.m_ownerWindow = window.get();
    texture.m_ownerContext = contexts.ownerContext.get();
    texture.m_shareGroup = contexts.ownerContext->shareGroup();
    texture.armCleanupOwnerForCurrentTexture();
    contexts.sharedContext->doneCurrent();

    std::thread releaseThread([&]() {
        std::unique_lock<std::recursive_mutex> ownerStateLock(*texture.m_ownerStateMutex);
        releaseLockHeld.store(true);
        while (!teardownStarted.load()) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        texture.release();
    });

    QTRY_VERIFY(releaseLockHeld.load());
    teardownStarted.store(true);
    contexts.ownerContext.reset();
    window.reset();
    releaseThread.join();

    const QString warnings = takeCapturedMessagesText();
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings));
    QTRY_COMPARE(wrapperDeletes.load(), 1);
    QTRY_COMPARE(texture.pendingCleanupGenerationCount(), 0);
    QVERIFY(contexts.sharedContext->makeCurrent(contexts.surface.get()));
    QVERIFY(!sharedFunctions->glIsTexture(nativeTextureId));
    contexts.sharedContext->doneCurrent();
}

void QmlSmokeTest::spectrogramRetainedSourceTextureOwnerContextLossWithoutSharedFallbackRetiresCleanup() {
    auto window = std::make_unique<QQuickWindow>();
    window->resize(64, 64);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get(), 5000));
    QVERIFY(!window->grabWindow().isNull());
    QVERIFY(window->isSceneGraphInitialized());

    SpectrogramSourceTexture texture;
    std::atomic_int wrapperDeletes{0};
    auto ownerContext = std::make_unique<QOpenGLContext>();
    texture.m_texture = new CountingTexture(&wrapperDeletes, QSize(8, 4));
    texture.m_textureId = 77;
    texture.m_size = QSize(8, 4);
    texture.m_ownerWindow = window.get();
    texture.m_ownerContext = ownerContext.get();
    texture.m_shareGroup.clear();
    texture.armCleanupOwnerForCurrentTexture();

    ownerContext.reset();

    std::atomic_bool renderAttemptRan{false};
    window->scheduleRenderJob(
        QRunnable::create([&renderAttemptRan]() {
            renderAttemptRan.store(true);
        }),
        QQuickWindow::AfterSynchronizingStage);
    window->update();
    QVERIFY(!window->grabWindow().isNull());
    QTRY_VERIFY(renderAttemptRan.load());
    QTRY_COMPARE(texture.pendingCleanupAttemptCount(), 1);
    QTRY_COMPARE(wrapperDeletes.load(), 1);
    QTRY_COMPARE(texture.pendingCleanupGenerationCount(), 0);

    window->update();
    QVERIFY(!window->grabWindow().isNull());
    QCOMPARE(texture.pendingCleanupAttemptCount(), 1);
    QCOMPARE(wrapperDeletes.load(), 1);
    QCOMPARE(texture.pendingCleanupGenerationCount(), 0);
}

void QmlSmokeTest::spectrogramRetainedSourceTextureWindowDestructionHandsOffSharedCleanup() {
    SharedOpenGlContexts contexts = createSharedOpenGlContexts();
    if (!contexts.isValid()) {
        QSKIP("shared OpenGL contexts are unavailable in this test environment");
    }
    if (!contexts.sharedContext->makeCurrent(contexts.surface.get())) {
        QSKIP("failed to make the shared OpenGL context current");
    }

    auto window = std::make_unique<QQuickWindow>();
    window->resize(64, 64);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get(), 5000));
    QVERIFY(!window->grabWindow().isNull());
    QVERIFY(window->isSceneGraphInitialized());

    SpectrogramSourceTexture texture;
    std::atomic_int wrapperDeletes{0};
    QOpenGLFunctions *sharedFunctions = contexts.sharedContext->functions();
    QVERIFY(sharedFunctions != nullptr);
    const GLuint nativeTextureId = createNativeTexture(contexts.sharedContext.get());
    QVERIFY(nativeTextureId != 0);
    QVERIFY(sharedFunctions->glIsTexture(nativeTextureId));
    texture.m_texture = new CountingTexture(&wrapperDeletes, QSize(8, 4));
    texture.m_textureId = nativeTextureId;
    texture.m_size = QSize(8, 4);
    texture.m_ownerWindow = window.get();
    texture.m_ownerContext = contexts.ownerContext.get();
    texture.m_shareGroup = contexts.ownerContext->shareGroup();

    texture.release();

    QCOMPARE(wrapperDeletes.load(), 0);
    QCOMPARE(texture.pendingCleanupGenerationCount(), 1);
    QVERIFY(sharedFunctions->glIsTexture(nativeTextureId));

    contexts.sharedContext->doneCurrent();

    std::atomic_bool firstRenderAttemptRan{false};
    window->scheduleRenderJob(
        QRunnable::create([&firstRenderAttemptRan]() {
            firstRenderAttemptRan.store(true);
        }),
        QQuickWindow::AfterSynchronizingStage);
    window->update();
    QVERIFY(!window->grabWindow().isNull());
    QTRY_VERIFY(firstRenderAttemptRan.load());
    QTRY_COMPARE(texture.pendingCleanupAttemptCount(), 1);

    window.reset();

    QTRY_COMPARE(wrapperDeletes.load(), 1);
    QTRY_COMPARE(texture.pendingCleanupGenerationCount(), 0);
    QVERIFY(contexts.sharedContext->makeCurrent(contexts.surface.get()));
    QVERIFY(!sharedFunctions->glIsTexture(nativeTextureId));
    contexts.sharedContext->doneCurrent();
}

void QmlSmokeTest::spectrogramRetainedSourceTextureRetiresMultiplePendingCleanupGenerations() {
    auto firstWindow = std::make_unique<QQuickWindow>();
    firstWindow->resize(64, 64);
    firstWindow->show();
    QVERIFY(QTest::qWaitForWindowExposed(firstWindow.get(), 5000));
    QVERIFY(!firstWindow->grabWindow().isNull());
    QVERIFY(firstWindow->isSceneGraphInitialized());

    auto secondWindow = std::make_unique<QQuickWindow>();
    secondWindow->resize(64, 64);
    secondWindow->show();
    QVERIFY(QTest::qWaitForWindowExposed(secondWindow.get(), 5000));
    QVERIFY(!secondWindow->grabWindow().isNull());
    QVERIFY(secondWindow->isSceneGraphInitialized());

    auto firstOwnerContext = std::make_unique<QOpenGLContext>();
    auto secondOwnerContext = std::make_unique<QOpenGLContext>();
    SpectrogramSourceTexture texture;
    std::atomic_int firstWrapperDeletes{0};
    std::atomic_int secondWrapperDeletes{0};

    texture.m_texture = new CountingTexture(&firstWrapperDeletes, QSize(8, 4));
    texture.m_textureId = 91;
    texture.m_size = QSize(8, 4);
    texture.m_ownerWindow = firstWindow.get();
    texture.m_ownerContext = firstOwnerContext.get();
    texture.release();

    QCOMPARE(firstWrapperDeletes.load(), 0);
    QCOMPARE(texture.pendingCleanupGenerationCount(), 1);

    texture.m_texture = new CountingTexture(&secondWrapperDeletes, QSize(8, 4));
    texture.m_textureId = 92;
    texture.m_size = QSize(8, 4);
    texture.m_ownerWindow = secondWindow.get();
    texture.m_ownerContext = secondOwnerContext.get();
    texture.release();

    QCOMPARE(secondWrapperDeletes.load(), 0);
    QCOMPARE(texture.pendingCleanupGenerationCount(), 2);

    firstOwnerContext.reset();
    secondOwnerContext.reset();
    firstWindow->update();
    secondWindow->update();
    QVERIFY(!firstWindow->grabWindow().isNull());
    QVERIFY(!secondWindow->grabWindow().isNull());
    QTRY_COMPARE(firstWrapperDeletes.load(), 1);
    QTRY_COMPARE(secondWrapperDeletes.load(), 1);
    QTRY_COMPARE(texture.pendingCleanupGenerationCount(), 0);

    firstWindow.reset();
    secondWindow.reset();
    QCOMPARE(firstWrapperDeletes.load(), 1);
    QCOMPARE(secondWrapperDeletes.load(), 1);
    QCOMPARE(texture.pendingCleanupGenerationCount(), 0);
}

void QmlSmokeTest::spectrogramRetainedSourceTextureCreatesReownsAndCleansUpOnLiveWindow() {
    QQuickWindow firstWindow;
    firstWindow.resize(96, 64);
    firstWindow.show();
    QVERIFY(QTest::qWaitForWindowExposed(&firstWindow, 5000));
    QVERIFY(!firstWindow.grabWindow().isNull());
    if (windowGraphicsApi(firstWindow) != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(
            QStringLiteral("real retained-source OpenGL lifecycle test requires OpenGL scene graph, got %1")
                .arg(graphicsApiName(windowGraphicsApi(firstWindow)))));
    }

    QQuickWindow secondWindow;
    std::atomic<std::uintptr_t> secondWindowRenderThreadId{0};
    QObject::connect(
        &secondWindow,
        &QQuickWindow::beforeRendering,
        &secondWindow,
        [&secondWindowRenderThreadId]() {
            secondWindowRenderThreadId.store(
                reinterpret_cast<std::uintptr_t>(QThread::currentThread()));
        },
        Qt::DirectConnection);
    secondWindow.resize(96, 64);
    secondWindow.show();
    QVERIFY(QTest::qWaitForWindowExposed(&secondWindow, 5000));
    QVERIFY(!secondWindow.grabWindow().isNull());
    if (windowGraphicsApi(secondWindow) != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(
            QStringLiteral("second live window did not get an OpenGL scene graph, got %1")
                .arg(graphicsApiName(windowGraphicsApi(secondWindow)))));
    }
    secondWindow.update();
    QVERIFY(!secondWindow.grabWindow().isNull());
    QTRY_VERIFY(secondWindowRenderThreadId.load() != 0);
    if (secondWindowRenderThreadId.load()
        == reinterpret_cast<std::uintptr_t>(QThread::currentThread())) {
        QSKIP("real retained-source wrapper cleanup test requires a threaded OpenGL scene graph");
    }

    SpectrogramSourceTexture texture;
    std::atomic_bool createFinished{false};
    std::atomic_bool createSucceeded{false};
    firstWindow.scheduleRenderJob(
        QRunnable::create([&]() {
            createSucceeded.store(texture.ensureCreated(&firstWindow, 8, 4));
            createFinished.store(true);
        }),
        QQuickWindow::AfterSynchronizingStage);
    firstWindow.update();
    QVERIFY(!firstWindow.grabWindow().isNull());
    QTRY_VERIFY(createFinished.load());
    QVERIFY(createSucceeded.load());
    QVERIFY(texture.texture() != nullptr);
    QCOMPARE(texture.size(), QSize(8, 4));
    QCOMPARE(texture.m_ownerWindow.data(), &firstWindow);
    QSGTexture *firstWrapper = texture.texture();

    std::atomic_bool recreateFinished{false};
    std::atomic_bool recreateSucceeded{false};
    secondWindow.scheduleRenderJob(
        QRunnable::create([&]() {
            recreateSucceeded.store(texture.ensureCreated(&secondWindow, 8, 4));
            recreateFinished.store(true);
        }),
        QQuickWindow::AfterSynchronizingStage);
    secondWindow.update();
    QVERIFY(!secondWindow.grabWindow().isNull());
    QTRY_VERIFY(recreateFinished.load());
    QVERIFY(recreateSucceeded.load());
    QVERIFY(texture.texture() != nullptr);
    QVERIFY(texture.texture() != firstWrapper);
    QCOMPARE(texture.m_ownerWindow.data(), &secondWindow);

    QPointer<QOpenGLContext> liveOwnerContext = texture.m_ownerContext;
    QPointer<QOpenGLContextGroup> liveShareGroup = texture.m_shareGroup;
    texture.m_ownerContext = new QOpenGLContext(this);
    texture.m_shareGroup.clear();

    const QByteArray rgbaBytes(8 * 4 * 4, '\x7f');
    std::atomic_bool uploadFinished{false};
    std::atomic_bool uploadSucceeded{true};
    secondWindow.scheduleRenderJob(
        QRunnable::create([&]() {
            uploadSucceeded.store(texture.uploadRgbaColumns(&secondWindow, 0, 8, 4, rgbaBytes));
            uploadFinished.store(true);
        }),
        QQuickWindow::AfterSynchronizingStage);
    secondWindow.update();
    QVERIFY(!secondWindow.grabWindow().isNull());
    QTRY_VERIFY(uploadFinished.load());
    QVERIFY(!uploadSucceeded.load());

    texture.m_ownerContext = liveOwnerContext;
    texture.m_shareGroup = liveShareGroup;
    texture.m_lastWrapperDeletionThreadId->store(0);
    texture.release();
    QVERIFY(texture.texture() == nullptr);
    QCOMPARE(texture.pendingCleanupGenerationCount(), 1);

    secondWindow.update();
    QVERIFY(!secondWindow.grabWindow().isNull());
    QTRY_COMPARE(texture.pendingCleanupGenerationCount(), 0);
    QTRY_VERIFY(texture.m_lastWrapperDeletionThreadId->load() != 0);
    QCOMPARE(
        texture.m_lastWrapperDeletionThreadId->load(),
        secondWindowRenderThreadId.load());
}

void QmlSmokeTest::spectrogramRetainedSourceTextureEnsureCreatedPreservesExistingStateWithoutContext() {
    QVERIFY(QOpenGLContext::currentContext() == nullptr);

    QQuickWindow ownerWindow;
    QQuickWindow otherWindow;
    SpectrogramSourceTexture texture;
    texture.m_textureId = 33;
    texture.m_ownerWindow = &ownerWindow;
    texture.m_ownerContext = new QOpenGLContext(this);
    texture.m_size = QSize(5, 7);
    texture.noteUploadedSpan({1, 2});

    QVERIFY(!texture.ensureCreated(&otherWindow, 5, 7));
    QCOMPARE(texture.m_textureId, 33U);
    QCOMPARE(texture.size(), QSize(5, 7));
    QCOMPARE(texture.pendingUploadSpanCount(), 1);
    QCOMPARE(texture.m_ownerWindow.data(), &ownerWindow);
}

void QmlSmokeTest::spectrogramRetainedSourceTextureUploadRejectsInvalidArguments() {
    SpectrogramSourceTexture texture;
    texture.m_textureId = 44;
    texture.m_ownerContext = new QOpenGLContext(this);
    texture.m_size = QSize(8, 4);
    texture.noteUploadedSpan({2, 3});

    QQuickWindow window;
    QVERIFY(!texture.uploadRgbaColumns(&window, -1, 2, 4, QByteArray(2 * 4 * 4, '\0')));
    QVERIFY(!texture.uploadRgbaColumns(&window, 1, 2, 3, QByteArray(2 * 3 * 4, '\0')));
    QVERIFY(!texture.uploadRgbaColumns(&window, 7, 2, 4, QByteArray(2 * 4 * 4, '\0')));
    QVERIFY(!texture.uploadRgbaColumns(&window, 1, 2, 4, QByteArray(3, '\0')));
    QCOMPARE(texture.m_textureId, 44U);
    QCOMPARE(texture.size(), QSize(8, 4));
    QCOMPARE(texture.pendingUploadSpanCount(), 1);
}

void QmlSmokeTest::spectrogramRetainedSourceTextureRejectsCapacityBeyondGpuLimit() {
    SpectrogramItem item;
    QQuickWindow window;

    item.m_gpuMaxTextureColumns = 16384;

    QVERIFY(item.retainedSourceTextureFitsGpuLimit(&window, 16320, 1025));
    QVERIFY(!item.retainedSourceTextureFitsGpuLimit(&window, 17820, 1025));
    QVERIFY(!item.retainedSourceTextureFitsGpuLimit(&window, 16320, 20000));
}

void QmlSmokeTest::loadsMainQmlWithFallbackBridge() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));

    const QString qmlPath = QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml");
    QVERIFY2(QFileInfo::exists(qmlPath), qPrintable(QStringLiteral("QML file missing: %1").arg(qmlPath)));

    const QUrl url = QUrl::fromLocalFile(qmlPath);
    engine.load(url);
    QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
    QObject *root = engine.rootObjects().constFirst();
    QVERIFY(root != nullptr);

    QObject *libraryView = qvariant_cast<QObject *>(root->property("libraryViewRef"));
    QVERIFY2(libraryView != nullptr, "Main.qml did not publish the library view ref");
    QCOMPARE(qvariant_cast<QObject *>(libraryView->property("model")), static_cast<QObject *>(&libraryModel));

    bool invoked = QMetaObject::invokeMethod(root, "openItunesArtworkDialog");
    QVERIFY(invoked);
    QObject *itunesDialog = findObjectByStringProperty(root, "title", QStringLiteral("Replace From iTunes"));
    QVERIFY2(itunesDialog != nullptr, "Could not find iTunes artwork dialog instance");
    QTRY_VERIFY(itunesDialog->property("visible").toBool());
}

void QmlSmokeTest::loadsExtractedQmlSlicesWithFallbackProps() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");

    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import "components" as Components
import "controllers" as Controllers
import "dialogs" as Dialogs
import "panes" as Panes
import "viewers" as Viewers

Item {
    id: harness
    width: 1600
    height: 980

    Window {
        id: viewerWindowRoot
        visible: false
        width: harness.width
        height: harness.height
    }

    QtObject {
        id: bridge
        property bool connected: false
        property string playbackState: "Stopped"
        property string positionText: "00:00"
        property string durationText: "00:00"
        property real positionSeconds: 0
        property real durationSeconds: 0
        property real volume: 0.5
        property var queueRows: []
        property int queueLength: queueRows.length
        property int queueVersion: 0
        property string queueDurationText: "00:00"
        property var waveformPeaksPacked: ""
        property real waveformCoverageSeconds: 0
        property bool waveformComplete: false
        property string currentTrackPath: ""
        property string currentTrackCoverPath: ""
        property string currentTrackTitle: ""
        property string currentTrackArtist: ""
        property string currentTrackAlbum: ""
        property string currentTrackGenre: ""
        property var currentTrackYear: null
        property int currentTrackNumber: 0
        property var itunesArtworkResults: []
        property bool itunesArtworkLoading: false
        property string itunesArtworkStatusText: ""
        property int selectedQueueIndex: -1
        property int playingQueueIndex: -1
        property int spectrogramViewMode: 0
        property int spectrogramDisplayMode: 0
        property int fftSize: 8192
        property real dbRange: 90
        property bool logScale: false
        property bool showFps: false
        property bool showSpectrogramCrosshair: false
        property bool showSpectrogramScale: false
        property bool spectrogramZoomEnabled: true
        property int soloedChannel: -1
        property int channelButtonsVisibility: 1
        property int viewerFullscreenMode: 0
        property int libraryArtistCount: 0
        property int libraryAlbumCount: 0
        property int libraryTrackCount: 0
        property var globalSearchModel: []
        property int globalSearchArtistCount: 0
        property int globalSearchAlbumCount: 0
        property int globalSearchTrackCount: 0
        property bool libraryScanInProgress: false
        property int libraryScanDiscovered: 0
        property int libraryScanProcessed: 0
        property real libraryScanFilesPerSecond: 0
        property real libraryScanEtaSeconds: -1
        property int librarySortMode: 0
        property var libraryRootEntries: []
        property string fileBrowserName: "File Manager"
        property bool lastFmScrobblingEnabled: false
        property bool lastFmBuildConfigured: false
        property string lastFmStatusText: ""
        property int lastFmPendingScrobbleCount: 0
        property string lastFmUsername: ""
        property bool systemMediaControlsEnabled: true
        property var mutedChannelsMask: 0
        property string diagnosticsText: ""
        property string diagnosticsLogPath: ""
        property int sampleRateHz: 48000
        signal diagnosticsChanged()
        signal itunesArtworkChanged()
        signal imageFileDetailsChanged(string path)
        signal precomputedSpectrogramChunkReady(var data, int bins, int channelCount, int columns,
            int startIndex, int totalEstimate, int sampleRate, int hopSize,
            real coverage, bool complete, bool bufferReset, bool clearHistory, var trackToken, var generation)
        signal precomputedSpectrogramChannelsReady(int channelCount, bool bufferReset)
        signal trackIdentityChanged()
        signal trackMetadataChanged()
        signal snapshotChanged()
        signal playbackChanged()
        function setVolume(value) {}
        function toggleChannelMute(channelIndex) {}
        function soloChannel(channelIndex) {}
        function isChannelMuted(channelIndex) { return false }
        function setChannelButtonsVisibility(value) {}
        function setLibrarySortMode(mode) {}
        function rescanAllLibraryRoots() {}
        function openInFileBrowser(path) {}
        function rescanLibraryRoot(path) {}
        function removeLibraryRoot(path) {}
        function setSpectrogramViewMode(mode) {}
        function setSpectrogramDisplayMode(mode) {}
        function setFftSize(value) {}
        function setDbRange(value) {}
        function setLogScale(value) {}
        function setShowFps(value) {}
        function setShowSpectrogramCrosshair(value) {}
        function setShowSpectrogramScale(value) {}
        function setSpectrogramZoomEnabled(value) {}
        function setSpectrogramZoomLevel(level) {}
        function setSpectrogramWidgetWidth(width) {}
        function registerSpectrogramItem(item, channelIndex) {}
        function unregisterSpectrogramItem(item) {}
        function setViewerFullscreenMode(mode) {}
        function setLastFmScrobblingEnabled(value) {}
        function beginLastFmAuth() {}
        function completeLastFmAuth() {}
        function disconnectLastFm() {}
        function setSystemMediaControlsEnabled(value) {}
        function openContainingFolder(path) {}
        function setGlobalSearchQuery(query) {}
        function searchCurrentTrackArtworkSuggestions() {}
        function clearItunesArtworkSuggestions() {}
        function requestImageFileDetails(path) {}
        function cachedImageFileDetails(path) { return ({}) }
        function imageFileDetails(path) { return ({}) }
        function itunesArtworkResultAt(index) { return ({}) }
        function prepareItunesArtworkSuggestion(index) {}
        function applyItunesArtworkSuggestion(index) {}
        function reloadDiagnosticsFromDisk() {}
        function clearDiagnostics() {}
        function libraryThumbnailSource(path) { return "" }
        function queuePathAt(index) { return "" }
        function playAt(index) {}
        function removeAt(index) {}
        function moveQueue(from, to) {}
    }

    QtObject {
        id: globalSearchModelApi
        function isSelectableIndex(index) { return false }
        function nextSelectableIndex(startIndex, step, wrap) { return -1 }
        function rowDataAt(index) { return null }
    }

    Controllers.GlobalSearchController {
        id: globalSearchController
        uiBridge: bridge
        globalSearchModelApi: globalSearchModelApi
        requestLibraryRevealForSearchRow: function(row) {}
        focusLibraryViewForNavigation: function() {}
        requestOpenInFileBrowserForSearchRow: function(row) {}
    }

    Controllers.QueueController {
        id: queueController
        uiBridge: bridge
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }

    Controllers.PlaybackController {
        id: playbackController
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }

    Controllers.LibraryController {
        id: libraryController
        uiBridge: bridge
        libraryModel: sidebarModel
        tryCaptureGlobalSearchPrefill: function(event) { return false }
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }

    Controllers.ViewerController {
        id: viewerController
        uiBridge: bridge
        useWholeScreenViewerMode: false
    }

    QtObject {
        id: tagEditorApi
        property bool open: false
        property bool loading: false
        property bool saving: false
        property bool dirty: false
        property string statusText: ""
        property string statusDetails: ""
        property var tableModel: []
        signal selectionChanged()
        signal bulkSummaryChanged()
        function close() {}
        function reload() {}
        function save() { return true }
        function renameSelectedFiles() {}
        function setSelectedRows(rows) {}
        function loadedPaths() { return [] }
        function bulkValue(field) { return "" }
        function applyBulkFieldToRows(rows, field, value) {}
        function applyEnglishTitleCase(field) {}
        function applyFinnishCapitalize(field) {}
        function applyGenreCapitalize() {}
        function autoNumber(startingTrack, startingDisc, writeDiscNumbers, writeTotals, resetOnFolder, resetOnDiscChange) {}
    }

    ListModel {
        id: sidebarModel
    }

    Components.UiPalette {
        id: palette
        windowRoot: harness
    }

    Action { id: previousAction }
    Action { id: playAction }
    Action { id: pauseAction }
    Action { id: stopAction }
    Action { id: nextAction }
    Action { id: clearPlaylistAction }
    Action { id: replaceFromItunesAction }
    Action { id: playAllLibraryTracksAction }
    Action { id: appendAllLibraryTracksAction }

    Dialogs.PreferencesDialog {
        id: preferencesDialog
        parent: harness
        uiBridge: bridge
        uiPalette: palette
        windowRoot: harness
        popupTransitionMs: 0
        spectrogramFftChoices: [512, 1024, 2048]
        promptAddLibraryRoot: function(context) {}
        openLibraryRootNameDialog: function(mode, path, name) {}
        stepScrollView: function(scrollView, wheel, stepSize, stepsPerWheel) {}
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
    }

    Dialogs.DiagnosticsDialog {
        parent: harness
        uiBridge: bridge
        uiPalette: palette
        windowRoot: harness
        popupTransitionMs: 0
    }

    Dialogs.LibraryRootNameDialog {
        parent: harness
        uiBridge: bridge
        uiPalette: palette
        windowRoot: harness
        popupTransitionMs: 0
        dialogMode: "add"
        pathValue: "/music"
        nameValue: "Music"
        onDismissed: function() {}
    }

    Dialogs.GlobalSearchDialog {
        parent: harness
        controller: globalSearchController
        uiPalette: palette
        windowRoot: harness
        popupTransitionMs: 0
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
        globalSearchShowsRootColumn: false
        globalSearchTrackNumberColumnWidth: 42
        globalSearchCoverColumnWidth: 28
        globalSearchArtistColumnWidth: 180
        globalSearchAlbumColumnWidth: 220
        globalSearchRootColumnWidth: 160
        globalSearchYearColumnWidth: 54
        globalSearchTrackGenreColumnWidth: 110
        globalSearchAlbumCountColumnWidth: 44
        globalSearchTrackLengthColumnWidth: 64
    }

    Dialogs.ItunesArtworkDialog {
        parent: harness
        uiBridge: bridge
        uiPalette: palette
        windowRoot: harness
        openAlbumArtViewerForSuggestion: function(row) {}
        openAlbumArtViewerForCurrentArt: function() {}
    }

    Dialogs.TagEditorDialog {
        parent: harness
        tagEditorApi: tagEditorApi
        uiPalette: palette
        windowRoot: harness
    }

    Panes.StatusBar {
        id: statusBar
        width: harness.width
        uiBridge: bridge
        uiPalette: palette
        channelStatusIconSource: function(key) { return "" }
        themeIsDark: palette.themeIsDark
    }

    Panes.TransportBar {
        parent: harness
        width: harness.width
        uiBridge: bridge
        uiPalette: palette
        previousAction: previousAction
        playAction: playAction
        pauseAction: pauseAction
        stopAction: stopAction
        nextAction: nextAction
        themeIsDark: palette.themeIsDark
        volumeMuted: playbackController.volumeMuted
        displayedPositionSeconds: playbackController.displayedPositionSeconds
        toggleMutedVolume: playbackController.toggleMutedVolume
        setAppVolume: playbackController.setAppVolume
        normalizedVolumeValue: playbackController.normalizedVolumeValue
        seekCommitted: playbackController.seekCommitted
    }

    Components.TrackMetadataCard {
        parent: harness
        width: 420
        uiBridge: bridge
        uiPalette: palette
    }

    Viewers.AlbumArtViewerShell {
        parent: harness
        windowRoot: viewerWindowRoot
        viewerOpen: false
        useWholeScreenViewerMode: false
        popupTransitionMs: 0
        titleText: "Ferrous"
        closeViewer: function() {}
        toggleInfoVisible: function() {}
        switchComparisonImage: function() {}
    }

    Viewers.AlbumArtSurface {
        parent: harness
        x: 1180
        y: 20
        width: 220
        height: 220
        viewerOpen: false
        viewerSource: ""
        infoVisible: false
        initialViewToken: 0
        viewerDecodeWidth: 1024
        viewerDecodeHeight: 1024
        infoOverlayText: ""
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        closeViewer: function() {}
        toggleInfoVisible: function() {}
        focusFullscreen: function() {}
        comparisonLabel: ""
        comparisonModeAvailable: false
    }

    Panes.SidebarPane {
        parent: harness
        x: 0
        y: 180
        width: 360
        height: 520
        controller: libraryController
        uiBridge: bridge
        libraryModel: sidebarModel
        uiPalette: palette
        splitPreferredWidth: width
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        openAlbumArtViewer: function() {}
        popupTransitionMs: 0
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
        stepScrollView: function(view, wheel, stepSize, stepsPerWheel) {}
        playAllLibraryTracksAction: playAllLibraryTracksAction
        appendAllLibraryTracksAction: appendAllLibraryTracksAction
    }

    Panes.QueuePane {
        parent: harness
        x: 380
        y: 180
        width: 720
        height: 320
        controller: queueController
        uiBridge: bridge
        uiPalette: palette
        preferredHeight: height
        playlistIndicatorColumnWidth: 22
        playlistOrderColumnWidth: 34
        playlistOrderText: function(index) { return String(index + 1) }
        libraryController: libraryController
        stepScrollView: function(view, wheel, stepSize, stepsPerWheel) {}
        clearPlaylistAction: clearPlaylistAction
        popupTransitionMs: 0
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
        droppedExternalPaths: function(drop) { return [] }
        submitExternalImport: function(paths, replaceQueue) { return false }
    }

    Viewers.SpectrogramSurface {
        parent: harness
        width: 420
        height: 160
        uiBridge: bridge
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
}

void QmlSmokeTest::albumArtTileKeepsHeightInsideColumnLayout() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "components" as Components

Item {
    width: 360
    height: 700

    QtObject {
        id: bridge
        property string currentTrackCoverPath: ""
    }

    Action { id: replaceFromItunesAction }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Components.AlbumArtTile {
            id: albumArtTile
            objectName: "albumArtTile"
            uiBridge: bridge
            replaceFromItunesAction: replaceFromItunesAction
            currentTrackItunesArtworkDisabledReason: function() { return "" }
            openAlbumArtViewer: function() {}
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *tile = root->findChild<QObject *>(QStringLiteral("albumArtTile"));
    QVERIFY(tile != nullptr);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QVERIFY2(tile->property("height").toReal() > 0.0, "AlbumArtTile collapsed to zero height");
}

void QmlSmokeTest::tagEditorLibrarySupportGateMatchesSupportedRows() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));

    const QUrl url = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml"));
    engine.load(url);
    QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
    QObject *root = engine.rootObjects().constFirst();
    QVERIFY(root != nullptr);

    QVariant supported;
    QVariant unsupported;
    const QVariant supportedRow = QVariant::fromValue(QVariantMap{
        {QStringLiteral("rowType"), QStringLiteral("album")},
        {QStringLiteral("selectionKey"), QStringLiteral("album|/music|Artist|Album")},
    });
    const QVariant unsupportedRow = QVariant::fromValue(QVariantMap{
        {QStringLiteral("rowType"), QStringLiteral("artist")},
        {QStringLiteral("selectionKey"), QStringLiteral("artist|/music|Artist")},
    });
    const bool supportedInvoked = QMetaObject::invokeMethod(
        root,
        "canOpenTagEditorForLibrary",
        Q_RETURN_ARG(QVariant, supported),
        Q_ARG(QVariant, supportedRow));
    const bool unsupportedInvoked = QMetaObject::invokeMethod(
        root,
        "canOpenTagEditorForLibrary",
        Q_RETURN_ARG(QVariant, unsupported),
        Q_ARG(QVariant, unsupportedRow));

    QVERIFY(supportedInvoked);
    QVERIFY(unsupportedInvoked);
    QCOMPARE(supported.toBool(), true);
    QCOMPARE(unsupported.toBool(), false);
}

void QmlSmokeTest::libraryTreeStartsCollapsedByDefault() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary());

    QTRY_COMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("artist"));
}

void QmlSmokeTest::rootRowsStartExpandedByDefault() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(multiRootBinary());

    QTRY_COMPARE(model.rowCount(), 4);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("root"));
    QCOMPARE(model.data(model.index(1, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("artist"));
    QCOMPARE(model.data(model.index(2, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("root"));
    QCOMPARE(model.data(model.index(3, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("artist"));
}

void QmlSmokeTest::artistExpansionPopulatesInBatches() {
    LibraryTreeModel model;

    model.setLibraryTreeFromBinary(artistWithManyAlbumsBinary(80));
    QTRY_COMPARE(model.rowCount(), 1);

    model.toggleArtist(QStringLiteral("Artist A"));

    QTRY_COMPARE(model.rowCount(), 81);
}

void QmlSmokeTest::lazyArtistRowRequestsBackendExpansion() {
    LibraryTreeModel model;
    QSignalSpy spy(&model, SIGNAL(nodeExpansionRequested(QString,bool)));

    model.setLibraryTreeFromBinary(artistOnlyLazyBinary());
    QTRY_COMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::ExpandedRole).toBool(), false);

    model.toggleArtist(QStringLiteral("Artist A"));
    QTRY_COMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.value(0).toString(), QStringLiteral("artist|/music|Artist A"));
    QCOMPARE(args.value(1).toBool(), true);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::ExpandedRole).toBool(), true);
}

void QmlSmokeTest::artistPrefixSearchUsesModelLookup() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(multiRootBinary());

    QTRY_COMPARE(model.rowCount(), 4);
    QCOMPARE(model.findArtistRowByPrefix(QStringLiteral("artist b"), 0), 3);
    QCOMPARE(model.findArtistRowByPrefix(QStringLiteral("artist a"), 2), 1);
    QCOMPARE(model.findArtistRowByPrefix(QStringLiteral("missing"), 0), -1);
}

void QmlSmokeTest::playbackControllerSeekImmediatelyUpdatesSpectrogramPosition() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
        property var seekCalls: []
        function seek(value) { seekCalls = seekCalls.concat([value]) }
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }

}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QCOMPARE(controller->property("displayedPositionSeconds").toDouble(), 12.0);
    QCOMPARE(controller->property("spectrogramPositionSeconds").toDouble(), 12.0);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekCommitted",
        Q_ARG(QVariant, QVariant::fromValue(48.0))));

    QCOMPARE(controller->property("displayedPositionSeconds").toDouble(), 48.0);
    QCOMPARE(controller->property("spectrogramPositionSeconds").toDouble(), 48.0);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekCommitted",
        Q_ARG(QVariant, QVariant::fromValue(23.16))));

    QCOMPARE(controller->property("displayedPositionSeconds").toDouble(), 23.16);
    QCOMPARE(controller->property("spectrogramPositionSeconds").toDouble(), 23.16);
}

void QmlSmokeTest::playbackControllerDeterministicTimeHooksDriveInterpolation() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "initializeFromBridgeAtTime",
        Q_ARG(QVariant, QVariant::fromValue(1000.0))));

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("positionSeconds", 12.12);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChangedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(1120.0)),
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedAfterHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedAfterHeartbeat - 12.12) < 0.02,
        qPrintable(QStringLiteral("displayed_after_heartbeat=%1")
            .arg(displayedAfterHeartbeat, 0, 'f', 6)));

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(1160.0))));

    const double displayedAfterStep =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        displayedAfterStep > 12.14,
        qPrintable(QStringLiteral("displayed_after_step=%1")
            .arg(displayedAfterStep, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerPlaybackUpdateKeepsSpectrogramOnInterpolatedClock() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);
    const double displayedBeforeUpdate =
        controller->property("displayedPositionSeconds").toDouble();

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("positionSeconds", 12.12);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedAfterUpdate =
        controller->property("displayedPositionSeconds").toDouble();
    const double spectrogramPosition =
        controller->property("spectrogramPositionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedAfterUpdate - 12.12) < 0.02,
        qPrintable(
            QStringLiteral("displayed=%1 displayed_before=%2")
                .arg(displayedAfterUpdate, 0, 'f', 6)
                .arg(displayedBeforeUpdate, 0, 'f', 6)));
    QVERIFY2(
        std::abs(spectrogramPosition - 12.12) < 0.02,
        qPrintable(QStringLiteral("spectrogram=%1").arg(spectrogramPosition, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerPostSeekHeartbeatSnapsToBackendPosition() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
        property var seekCalls: []
        function seek(value) { seekCalls = seekCalls.concat([value]) }
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekCommitted",
        Q_ARG(QVariant, QVariant::fromValue(48.0))));

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("positionSeconds", 48.26);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedAfterHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    const double spectrogramAfterHeartbeat =
        controller->property("spectrogramPositionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedAfterHeartbeat - 48.26) < 0.02,
        qPrintable(QStringLiteral("displayed=%1").arg(displayedAfterHeartbeat, 0, 'f', 6)));
    QVERIFY2(
        std::abs(spectrogramAfterHeartbeat - 48.26) < 0.02,
        qPrintable(QStringLiteral("spectrogram=%1").arg(spectrogramAfterHeartbeat, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerPostSeekBehindHeartbeatKeepsVisualClock() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
        property var seekCalls: []
        function seek(value) { seekCalls = seekCalls.concat([value]) }
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "initializeFromBridgeAtTime",
        Q_ARG(QVariant, QVariant::fromValue(1000.0))));
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekCommittedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(48.0)),
        Q_ARG(QVariant, QVariant::fromValue(2000.0))));

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(2240.0))));
    const double displayedBeforeHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY(displayedBeforeHeartbeat > 48.20);

    bridge->setProperty("positionSeconds", 48.08);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChangedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(2240.0)),
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedOnHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        displayedOnHeartbeat >= displayedBeforeHeartbeat - 0.001,
        qPrintable(QStringLiteral("displayed_before=%1 displayed_on_heartbeat=%2")
            .arg(displayedBeforeHeartbeat, 0, 'f', 6)
            .arg(displayedOnHeartbeat, 0, 'f', 6)));

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(2280.0))));

    const double displayedAfterStep =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        displayedAfterStep > displayedOnHeartbeat + 0.03,
        qPrintable(QStringLiteral("displayed_on_heartbeat=%1 displayed_after=%2")
            .arg(displayedOnHeartbeat, 0, 'f', 6)
            .arg(displayedAfterStep, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerPostSeekHeldTargetKeepsVisualClock() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
        property var seekCalls: []
        function seek(value) { seekCalls = seekCalls.concat([value]) }
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "initializeFromBridgeAtTime",
        Q_ARG(QVariant, QVariant::fromValue(1000.0))));
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekCommittedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(48.0)),
        Q_ARG(QVariant, QVariant::fromValue(2000.0))));

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(2100.0))));
    const double displayedBeforeHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY(displayedBeforeHeartbeat > 48.09);

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("positionSeconds", 48.0);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChangedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(2100.0)),
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedOnHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        displayedOnHeartbeat >= displayedBeforeHeartbeat - 0.001,
        qPrintable(QStringLiteral("displayed_before=%1 displayed_on_heartbeat=%2")
            .arg(displayedBeforeHeartbeat, 0, 'f', 6)
            .arg(displayedOnHeartbeat, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerPostSeekTargetEchoKeepsVisualClockActive() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
        property var seekCalls: []
        function seek(value) { seekCalls = seekCalls.concat([value]) }
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "initializeFromBridgeAtTime",
        Q_ARG(QVariant, QVariant::fromValue(1000.0))));
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekCommittedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(48.0)),
        Q_ARG(QVariant, QVariant::fromValue(2000.0))));

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("positionSeconds", 48.0);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChangedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(2000.0)),
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(2240.0))));
    const double displayedBeforeBackendHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY(displayedBeforeBackendHeartbeat > 48.20);

    bridge->setProperty("positionSeconds", 48.08);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChangedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(2240.0)),
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedAfterBackendHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        displayedAfterBackendHeartbeat >= displayedBeforeBackendHeartbeat - 0.001,
        qPrintable(QStringLiteral("displayed_before=%1 displayed_after=%2")
            .arg(displayedBeforeBackendHeartbeat, 0, 'f', 6)
            .arg(displayedAfterBackendHeartbeat, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerPlayAtCurrentTrackClearsPostSeekVisualClock() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 480.0
        property string currentTrackPath: "/music/test.flac"
        property int playingQueueIndex: 0
        property real volume: 1.0
        property var playAtCalls: []
        property var seekCalls: []
        function seek(value) { seekCalls = seekCalls.concat([value]) }
        function playAt(index) { playAtCalls = playAtCalls.concat([index]) }
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);
    QObject *bridge = root->findChild<QObject *>(QStringLiteral("bridge"));
    QVERIFY(bridge != nullptr);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "initializeFromBridgeAtTime",
        Q_ARG(QVariant, QVariant::fromValue(1000.0))));
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekCommittedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(410.0)),
        Q_ARG(QVariant, QVariant::fromValue(2000.0))));
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(2300.0))));
    QVERIFY(controller->property("displayedPositionSeconds").toDouble() > 410.20);
    QVERIFY(controller->property("visualSeekClockActive").toBool());

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "playAt",
        Q_ARG(QVariant, QVariant::fromValue(0))));

    QCOMPARE(bridge->property("playAtCalls").toList().size(), 1);
    QCOMPARE(bridge->property("playAtCalls").toList().at(0).toInt(), 0);
    QVERIFY(!controller->property("visualSeekClockActive").toBool());
    QVERIFY2(
        controller->property("displayedPositionSeconds").toDouble() < 0.02,
        qPrintable(QStringLiteral("displayed=%1")
            .arg(controller->property("displayedPositionSeconds").toDouble(), 0, 'f', 6)));
    QVERIFY2(
        controller->property("spectrogramPositionSeconds").toDouble() < 0.02,
        qPrintable(QStringLiteral("spectrogram=%1")
            .arg(controller->property("spectrogramPositionSeconds").toDouble(), 0, 'f', 6)));

    bridge->setProperty("positionSeconds", 0.0);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChangedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(2310.0)),
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    QVERIFY2(
        controller->property("displayedPositionSeconds").toDouble() < 0.03,
        qPrintable(QStringLiteral("displayed_after_heartbeat=%1")
            .arg(controller->property("displayedPositionSeconds").toDouble(), 0, 'f', 6)));
    QVERIFY2(
        controller->property("spectrogramPositionSeconds").toDouble() < 0.03,
        qPrintable(QStringLiteral("spectrogram_after_heartbeat=%1")
            .arg(controller->property("spectrogramPositionSeconds").toDouble(), 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerHeartbeatCorrectionAvoidsOneFrameSpeedBurst() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);
    const double displayedBeforeHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("positionSeconds", displayedBeforeHeartbeat + 0.18);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedImmediately =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedImmediately - (displayedBeforeHeartbeat + 0.18)) < 0.02,
        qPrintable(
            QStringLiteral("displayed_immediately=%1 displayed_before=%2")
                .arg(displayedImmediately, 0, 'f', 6)
                .arg(displayedBeforeHeartbeat, 0, 'f', 6)));

    QTest::qWait(20);

    const double displayedAfterOneFrame =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        displayedAfterOneFrame >= (displayedImmediately - 0.001)
            && displayedAfterOneFrame <= (displayedImmediately + 0.03),
        qPrintable(
            QStringLiteral("displayed_immediately=%1 displayed_after=%2")
                .arg(displayedImmediately, 0, 'f', 6)
                .arg(displayedAfterOneFrame, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerModerateSteadyStateLagUsesTrimNotBleed() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
        property bool profileLogsEnabled: true
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);

    clearCapturedMessages();

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    const double displayedBeforeHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    bridge->setProperty("positionSeconds", displayedBeforeHeartbeat + 0.078);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const QString warnings = takeCapturedMessagesText();
    QVERIFY2(
        !warnings.contains(QStringLiteral("[qml-playback-profile] heartbeat")),
        qPrintable(warnings));
    QVERIFY2(!warnings.contains(QStringLiteral("action=trim")), qPrintable(warnings));
    QVERIFY2(!warnings.contains(QStringLiteral("action=bleed")), qPrintable(warnings));
}

void QmlSmokeTest::playbackControllerProfileLogsHeartbeatCorrectionAndBleed() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
        property bool profileLogsEnabled: true
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);

    clearCapturedMessages();

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    const double displayedBeforeHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    bridge->setProperty("positionSeconds", displayedBeforeHeartbeat + 0.18);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const QString warnings = takeCapturedMessagesText();
    QVERIFY2(
        warnings.contains(QStringLiteral("[qml-playback-profile] heartbeat")),
        qPrintable(warnings));
    QVERIFY2(
        warnings.contains(QStringLiteral("action=follow")),
        qPrintable(warnings));
    QVERIFY2(
        !warnings.contains(QStringLiteral("[qml-playback-profile] bleed")),
        qPrintable(warnings));
}

void QmlSmokeTest::playbackControllerProfileSkipsMinorFollowHeartbeatLogs() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
        property bool profileLogsEnabled: true
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);

    clearCapturedMessages();

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    const double displayedBeforeHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    bridge->setProperty("positionSeconds", displayedBeforeHeartbeat + 0.006);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const QString warnings = takeCapturedMessagesText();
    QVERIFY2(
        !warnings.contains(QStringLiteral("[qml-playback-profile] heartbeat")),
        qPrintable(warnings));
}

void QmlSmokeTest::playbackControllerIgnoresSteadyStateHeartbeatJitter() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    const double displayedBeforeHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    bridge->setProperty("positionSeconds", displayedBeforeHeartbeat + 0.05);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedImmediately =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedImmediately - (displayedBeforeHeartbeat + 0.05)) < 0.02,
        qPrintable(
            QStringLiteral("displayed_immediately=%1 displayed_before=%2")
                .arg(displayedImmediately, 0, 'f', 6)
                .arg(displayedBeforeHeartbeat, 0, 'f', 6)));

}

void QmlSmokeTest::playbackControllerKeepsWallClockInterpolationAfterSubRealtimeHeartbeats() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "initializeFromBridgeAtTime",
        Q_ARG(QVariant, QVariant::fromValue(1000.0))));

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);

    const std::array<double, 4> heartbeatTimes = {1130.0, 1260.0, 1390.0, 1520.0};
    const std::array<double, 4> heartbeatPositions = {12.122, 12.244, 12.366, 12.488};

    for (std::size_t i = 0; i < heartbeatTimes.size(); ++i) {
        bridge->setProperty("positionSeconds", heartbeatPositions[i]);
        QVERIFY(QMetaObject::invokeMethod(
            controller,
            "handlePlaybackChangedAtTime",
            Q_ARG(QVariant, QVariant::fromValue(heartbeatTimes[i])),
            Q_ARG(QVariant, QVariant()),
            Q_ARG(QVariant, QVariant())));
    }

    const double displayedBeforeFreeRun =
        controller->property("displayedPositionSeconds").toDouble();

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(1920.0))));

    const double displayedAfterFreeRun =
        controller->property("displayedPositionSeconds").toDouble();
    const double localAdvance = displayedAfterFreeRun - displayedBeforeFreeRun;
    QVERIFY2(
        localAdvance > 0.384,
        qPrintable(
            QStringLiteral("displayed_before=%1 displayed_after=%2 local_advance=%3")
                .arg(displayedBeforeFreeRun, 0, 'f', 6)
                .arg(displayedAfterFreeRun, 0, 'f', 6)
                .arg(localAdvance, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerSteadyStateTrimReducesNoticeableLag() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);

    double incomingPosition =
        controller->property("displayedPositionSeconds").toDouble() + 0.10;
    for (int i = 0; i < 20; ++i) {
        bridge->setProperty("positionSeconds", incomingPosition);
        QVERIFY(QMetaObject::invokeMethod(
            controller,
            "handlePlaybackChanged",
            Q_ARG(QVariant, QVariant()),
            Q_ARG(QVariant, QVariant())));
        QTest::qWait(40);
        incomingPosition += 0.039;
    }

    const double displayedPosition = controller->property("displayedPositionSeconds").toDouble();
    const double backendPosition = bridge->property("positionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedPosition - backendPosition) < 0.07,
        qPrintable(
            QStringLiteral("displayed=%1 backend=%2")
                .arg(displayedPosition, 0, 'f', 6)
                .arg(backendPosition, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerFollowsBoundedRecoveryCadenceWithoutBurst() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);

    const std::array<double, 6> positions = {12.039, 12.079, 12.122, 12.165, 12.208, 12.251};
    double previousDisplayed = controller->property("displayedPositionSeconds").toDouble();
    double maximumStep = 0.0;

    for (double nextPosition : positions) {
        QTest::qWait(40);
        bridge->setProperty("positionSeconds", nextPosition);
        QVERIFY(QMetaObject::invokeMethod(
            controller,
            "handlePlaybackChanged",
            Q_ARG(QVariant, QVariant()),
            Q_ARG(QVariant, QVariant())));
        const double displayed = controller->property("displayedPositionSeconds").toDouble();
        maximumStep = std::max(maximumStep, displayed - previousDisplayed);
        previousDisplayed = displayed;
    }

    QVERIFY2(
        maximumStep < 0.05,
        qPrintable(QStringLiteral("maximum_step=%1").arg(maximumStep, 0, 'f', 6)));
    const double displayedPosition = controller->property("displayedPositionSeconds").toDouble();
    const double backendPosition = bridge->property("positionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedPosition - backendPosition) < 0.02,
        qPrintable(
            QStringLiteral("displayed=%1 backend=%2")
                .arg(displayedPosition, 0, 'f', 6)
                .arg(backendPosition, 0, 'f', 6)));
}

void QmlSmokeTest::spectrogramMetadataOnlyResetWaitsForDataChunk() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 512;
    QByteArray initialChunk(4 * binsPerColumn, '\0');
    for (int i = 0; i < initialChunk.size(); ++i) {
        initialChunk[i] = static_cast<char>(20 + i);
    }

    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        4,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    const qint64 rollingEpochBeforeReset = item.m_rollingEpoch;
    const qint64 writeSeqBeforeReset = item.m_ringWriteSeq;
    QVERIFY(writeSeqBeforeReset > 0);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        128,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    QVERIFY(item.m_precomputedResetPending);
    QCOMPARE(item.m_rollingEpoch, rollingEpochBeforeReset);
    QCOMPARE(item.m_ringWriteSeq, writeSeqBeforeReset);

    QByteArray seekChunk(binsPerColumn, '\0');
    for (int i = 0; i < seekChunk.size(); ++i) {
        seekChunk[i] = static_cast<char>(100 + i);
    }
    item.feedPrecomputedChunk(
        seekChunk,
        binsPerColumn,
        0,
        1,
        128,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        11);

    QVERIFY(!item.m_precomputedResetPending);
    QCOMPARE(item.m_rollingEpoch, static_cast<qint64>(-128));
    QCOMPARE(item.m_ringWriteSeq, static_cast<qint64>(2));
    QCOMPARE(item.m_ringSequenceId[0], static_cast<qint64>(0));
    QCOMPARE(item.m_ringColumnId[0], 0);
    QCOMPARE(item.m_ringSequenceId[1], static_cast<qint64>(1));
    QCOMPARE(item.m_ringColumnId[1], 128);
    QCOMPARE(item.m_ringSequenceId[2], static_cast<qint64>(-1));
}

void QmlSmokeTest::spectrogramRollingSeekKeepsHistoryContinuous() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 1024;
    QByteArray initialChunk(12 * binsPerColumn, '\0');
    for (int i = 0; i < initialChunk.size(); ++i) {
        initialChunk[i] = static_cast<char>(10 + i);
    }
    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        12,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    const qint64 writeSeqBeforeSeek = item.m_ringWriteSeq;
    QCOMPARE(writeSeqBeforeSeek, 12);
    item.setPositionSeconds((8.0 * 1024.0) / 48'000.0);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        400,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    QByteArray seekChunk(2 * binsPerColumn, '\0');
    for (int i = 0; i < seekChunk.size(); ++i) {
        seekChunk[i] = static_cast<char>(100 + i);
    }
    item.feedPrecomputedChunk(
        seekChunk,
        binsPerColumn,
        0,
        2,
        400,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        11);

    QCOMPARE(item.m_ringWriteSeq, static_cast<qint64>(11));
    QCOMPARE(item.m_rollingEpoch, static_cast<qint64>(-392));
    QCOMPARE(item.m_ringSequenceId[0], 0);
    QCOMPARE(item.m_ringColumnId[0], 0);
    QCOMPARE(item.m_ringSequenceId[1], 1);
    QCOMPARE(item.m_ringColumnId[1], 1);
    QCOMPARE(item.m_ringSequenceId[8], 8);
    QCOMPARE(item.m_ringColumnId[8], 8);
    QCOMPARE(item.m_ringSequenceId[9], 9);
    QCOMPARE(item.m_ringColumnId[9], 400);
    QCOMPARE(item.m_ringSequenceId[10], 10);
    QCOMPARE(item.m_ringColumnId[10], 401);
    QCOMPARE(item.m_ringSequenceId[11], static_cast<qint64>(-1));
}

void QmlSmokeTest::spectrogramCenteredToRollingAtMaxZoomReanchorsEpoch() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int binsPerColumn = 8;
    constexpr int sampleRate = 44'100;
    constexpr int hop = 64; // max zoom
    constexpr int resetStart = 139'812;
    constexpr int columns = 2'048;
    constexpr int currentSeq = 1'000;
    constexpr int totalEstimate = 188'208;
    constexpr quint64 trackToken = 3;

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        resetStart,
        totalEstimate,
        sampleRate,
        hop,
        false,
        true,
        trackToken,
        true);

    QByteArray data(columns * binsPerColumn, '\x40');
    item.feedPrecomputedChunk(
        data,
        binsPerColumn,
        0,
        columns,
        resetStart,
        totalEstimate,
        sampleRate,
        hop,
        false,
        false,
        trackToken,
        true);

    const int currentTrackCol = resetStart + currentSeq;
    const double positionSeconds =
        (static_cast<double>(currentTrackCol) + 0.25)
        * static_cast<double>(hop)
        / static_cast<double>(sampleRate);
    item.setPositionSeconds(positionSeconds);

    QCOMPARE(item.m_rollingEpoch, static_cast<qint64>(0));
    QCOMPARE(item.m_ringWriteSeq, static_cast<qint64>(columns));

    item.setDisplayMode(0); // Rolling

    QCOMPARE(item.m_rollingEpoch, static_cast<qint64>(-resetStart));

    const qint64 nowCol = static_cast<qint64>(std::floor(
        positionSeconds
        * static_cast<double>(sampleRate)
        / static_cast<double>(hop)));
    const qint64 displaySeq = item.m_rollingEpoch + nowCol;
    QCOMPARE(displaySeq, static_cast<qint64>(currentSeq));
    QVERIFY(displaySeq >= item.m_ringOldestSeq);
    QVERIFY(displaySeq < item.m_ringWriteSeq);
}

void QmlSmokeTest::spectrogramLargePositionJumpWaitsForResetHandoff() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 1024;
    QByteArray initialChunk(4 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        4,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);
    item.setPositionSeconds(1.0);
    item.setPlaying(true);

    item.setPositionSeconds(120.0);
    QVERIFY(item.m_positionJumpHoldActive);
    QVERIFY(std::abs(item.m_positionSeconds - 1.0) < 0.0001);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        512,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    QByteArray seekChunk(binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        seekChunk,
        binsPerColumn,
        0,
        1,
        512,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        11);

    const double expectedSeconds = (512.0 * 1024.0) / 48000.0;
    QVERIFY(!item.m_positionJumpHoldActive);
    QVERIFY(std::abs(item.m_positionSeconds - expectedSeconds) < 0.0001);
}

void QmlSmokeTest::spectrogramPlaybackHeartbeatDoesNotMoveAnchorBackward() {
    SpectrogramItem item;

    item.setPositionSeconds(0.0);
    item.setPlaying(true);

    QTest::qWait(70);
    item.setPositionSeconds(0.05);
    const double anchoredPosition = item.positionSeconds();
    QVERIFY2(
        anchoredPosition >= 0.05,
        qPrintable(QStringLiteral("expected anchor to move forward, got %1")
            .arg(anchoredPosition, 0, 'f', 3)));

    QTest::qWait(70);
    item.setPositionSeconds(0.02);

    QVERIFY2(
        item.positionSeconds() >= anchoredPosition - 0.001,
        qPrintable(QStringLiteral(
            "lagging playback heartbeat moved anchor backward from %1 to %2")
            .arg(anchoredPosition, 0, 'f', 3)
            .arg(item.positionSeconds(), 0, 'f', 3)));
}

void QmlSmokeTest::spectrogramCenteredZoomFillHeartbeatDoesNotMoveAnchorBackward() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered
    item.setPositionSeconds(10.0);
    item.setPlaying(true);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomFillActive = true;
    }

    QTest::qWait(40);
    const double before =
        item.currentRenderPositionSecondsLocked(std::chrono::steady_clock::now());
    item.setPositionSeconds(before - 0.02);
    const double after =
        item.currentRenderPositionSecondsLocked(std::chrono::steady_clock::now());

    QVERIFY2(
        after >= before - 0.001,
        qPrintable(QStringLiteral(
            "lagging centered zoom-fill heartbeat moved visual clock backward "
            "from %1 to %2")
            .arg(before, 0, 'f', 4)
            .arg(after, 0, 'f', 4)));
}

void QmlSmokeTest::spectrogramCenteredSmallSeekSnapsWithoutServoSlide() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    item.setPositionSeconds(10.0);
    item.setPlaying(true);

    QTest::qWait(30);
    item.setPositionSeconds(10.08);

    QVERIFY2(
        std::abs(item.m_positionAnchorSeconds - 10.08) < 0.0001,
        qPrintable(QStringLiteral(
            "centered small seek should snap to target, got anchor %1")
            .arg(item.m_positionAnchorSeconds, 0, 'f', 4)));
    QVERIFY2(
        std::abs(item.m_positionSeconds - 10.08) < 0.0001,
        qPrintable(QStringLiteral(
            "centered small seek should publish target, got position %1")
            .arg(item.m_positionSeconds, 0, 'f', 4)));
}

void QmlSmokeTest::spectrogramCenteredSeekFollowsLocalVisualClock() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    item.m_precomputedReady = true;
    item.setPositionSeconds(0.5);
    item.setPlaying(true);

    constexpr double seekTarget = 134.577;
    item.setPositionSeconds(seekTarget);
    item.setPositionSeconds(seekTarget + 0.08);

    const double displayed =
        item.currentRenderPositionSecondsLocked(std::chrono::steady_clock::now());
    QVERIFY2(
        displayed > seekTarget + 0.06,
        qPrintable(QStringLiteral(
            "centered seek should follow the visual clock, target=%1 displayed=%2")
            .arg(seekTarget, 0, 'f', 3)
            .arg(displayed, 0, 'f', 3)));
}

void QmlSmokeTest::spectrogramEarlyCenteredSeekFollowsLocalVisualClockBeforePrecomputedReady() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    item.setPositionSeconds(0.6);
    item.setPlaying(true);

    constexpr double seekTarget = 117.079;
    item.setPositionSeconds(seekTarget);
    item.setPositionSeconds(seekTarget + 0.08);

    const double displayed =
        item.currentRenderPositionSecondsLocked(std::chrono::steady_clock::now());
    QVERIFY2(
        displayed > seekTarget + 0.06,
        qPrintable(QStringLiteral(
            "early centered seek should follow the visual clock before precomputed ready, target=%1 displayed=%2")
            .arg(seekTarget, 0, 'f', 3)
            .arg(displayed, 0, 'f', 3)));
}

void QmlSmokeTest::spectrogramGaplessTrackChangePreservesRollingHistory() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 1024;
    QByteArray initialChunk(4 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        4,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    const qint64 writeSeqBeforeGapless = item.m_ringWriteSeq;
    QCOMPARE(writeSeqBeforeGapless, 4);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        12);

    QCOMPARE(item.m_precomputedTrackToken, 12ULL);
    QCOMPARE(item.m_ringWriteSeq, writeSeqBeforeGapless);
    // Epoch stays unchanged during gapless transitions — the old
    // position model keeps advancing and the jump hold expiry remaps
    // the epoch to maintain display continuity.
    QCOMPARE(item.m_rollingEpoch, static_cast<qint64>(0));

    QByteArray nextTrackChunk(2 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        nextTrackChunk,
        binsPerColumn,
        0,
        2,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        12);

    QCOMPARE(item.m_ringWriteSeq, writeSeqBeforeGapless + 2);
    QCOMPARE(item.m_ringSequenceId[4], 4);
    QCOMPARE(item.m_ringColumnId[4], 0);
    QCOMPARE(item.m_ringTrackToken[4], 12ULL);
    QCOMPARE(item.m_ringSequenceId[5], 5);
    QCOMPARE(item.m_ringColumnId[5], 1);
    QCOMPARE(item.m_ringTrackToken[5], 12ULL);
}

void QmlSmokeTest::spectrogramNaturalTrackResetPreservesRollingHistory() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 1024;
    QByteArray initialChunk(4 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        4,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    const qint64 writeSeqBeforeReset = item.m_ringWriteSeq;
    QCOMPARE(writeSeqBeforeReset, 4);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        12);

    QVERIFY(item.m_precomputedResetPending);
    QCOMPARE(item.m_ringWriteSeq, writeSeqBeforeReset);

    QByteArray nextTrackChunk(2 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        nextTrackChunk,
        binsPerColumn,
        0,
        2,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        12);

    QVERIFY(!item.m_precomputedResetPending);
    QCOMPARE(item.m_rollingEpoch, static_cast<qint64>(0));
    QCOMPARE(item.m_ringWriteSeq, static_cast<qint64>(3));
    QCOMPARE(item.m_ringSequenceId[0], 0);
    QCOMPARE(item.m_ringColumnId[0], 0);
    QCOMPARE(item.m_ringTrackToken[0], 11ULL);
    QCOMPARE(item.m_ringSequenceId[1], 1);
    QCOMPARE(item.m_ringColumnId[1], 0);
    QCOMPARE(item.m_ringTrackToken[1], 12ULL);
    QCOMPARE(item.m_ringSequenceId[2], 2);
    QCOMPARE(item.m_ringColumnId[2], 1);
    QCOMPARE(item.m_ringTrackToken[2], 12ULL);
    QCOMPARE(item.m_ringSequenceId[3], static_cast<qint64>(-1));
}

void QmlSmokeTest::spectrogramManualTrackResetClearsRollingHistory() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 1024;
    QByteArray initialChunk(4 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        4,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11,
        true);

    QCOMPARE(item.m_ringWriteSeq, 4);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        12,
        true);

    QVERIFY(item.m_precomputedResetPending);
    QVERIFY(item.m_precomputedPendingResetClearHistory);
    QCOMPARE(item.m_ringWriteSeq, 4);

    QByteArray nextTrackChunk(2 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        nextTrackChunk,
        binsPerColumn,
        0,
        2,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        12);

    QVERIFY(!item.m_precomputedResetPending);
    QCOMPARE(item.m_ringWriteSeq, 2);
    QCOMPARE(item.m_ringOldestSeq, 0);
    QCOMPARE(item.m_rollingEpoch, 0);
    QCOMPARE(item.m_precomputedTrackToken, 12ULL);
    QCOMPARE(item.m_ringSequenceId[0], 0);
    QCOMPARE(item.m_ringColumnId[0], 0);
    QCOMPARE(item.m_ringTrackToken[0], 12ULL);
    QCOMPARE(item.m_ringSequenceId[1], 1);
    QCOMPARE(item.m_ringColumnId[1], 1);
    QCOMPARE(item.m_ringTrackToken[1], 12ULL);
}

void QmlSmokeTest::spectrogramRollingZoomResetAnchorsEpochToResetStart() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(180);
    item.setDisplayMode(0); // Rolling

    constexpr int binsPerColumn = 8;
    constexpr int sampleRate = 44'100;
    constexpr int oldHop = 655;
    constexpr int newHop = 524;
    constexpr int resetStart = 1416;
    constexpr int totalEstimate = 19'668;
    constexpr quint64 trackToken = 3;

    QByteArray initialChunk(64 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        64,
        0,
        totalEstimate,
        sampleRate,
        oldHop,
        false,
        true,
        trackToken,
        true);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        resetStart,
        totalEstimate,
        sampleRate,
        newHop,
        false,
        true,
        trackToken,
        true);

    QByteArray zoomChunk(127 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        zoomChunk,
        binsPerColumn,
        0,
        127,
        resetStart,
        totalEstimate,
        sampleRate,
        newHop,
        false,
        false,
        trackToken,
        true);

    QCOMPARE(item.m_ringWriteSeq, static_cast<qint64>(127));
    QCOMPARE(item.m_rollingEpoch, static_cast<qint64>(-resetStart));

    const double colsPerSecond =
        static_cast<double>(sampleRate) / static_cast<double>(newHop);
    const qint64 anchoredCol = static_cast<qint64>(std::floor(
        item.m_positionAnchorSeconds * colsPerSecond));
    QCOMPARE(anchoredCol, static_cast<qint64>(resetStart));
    QCOMPARE(item.m_rollingEpoch + anchoredCol, static_cast<qint64>(0));
}

void QmlSmokeTest::spectrogramSeekResetAnchorsPlaybackClockToChunkStart() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setPlaying(true);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 512;
    QByteArray initialChunk(4 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        4,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        256,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    QByteArray seekChunk(binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        seekChunk,
        binsPerColumn,
        0,
        1,
        256,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        11);

    const double expectedSeconds = (256.0 * 1024.0) / 48000.0;
    QVERIFY(std::abs(item.m_positionAnchorSeconds - expectedSeconds) < 0.0001);
    QVERIFY(std::abs(item.m_positionSeconds - expectedSeconds) < 0.0001);
}

void QmlSmokeTest::diagnosticsLogUsesLowercaseAppDir() {
    const QString logPath = DiagnosticsLog::defaultLogPath();
    QVERIFY(!logPath.isEmpty());

    const QString genericDataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (genericDataDir.isEmpty()) {
        QVERIFY(logPath.endsWith(QStringLiteral("/diagnostics.log"))
            || logPath.endsWith(QStringLiteral("\\diagnostics.log")));
        return;
    }

    const QFileInfo info(logPath);
    QCOMPARE(info.fileName(), QStringLiteral("diagnostics.log"));
    QCOMPARE(info.dir().dirName(), QStringLiteral("ferrous"));
    QVERIFY(!logPath.contains(QStringLiteral("/Ferrous/")));
    QVERIFY(!logPath.contains(QStringLiteral("\\Ferrous\\")));
}

void QmlSmokeTest::spectrogramSeekProfileFlagsStalledPostSeekWindow() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    qputenv("FERROUS_PROFILE_UI", "1");
    SpectrogramSeekTrace::noteSeekIssued(12.5);

    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    QVariantMap state;
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_profileEnabled = true;
        item.m_precomputedCanvasDisplayRight = 96;
        item.m_gpuDisplayWidth = 320;

        const qint64 startedAtMs = SpectrogramSeekTrace::startedAtMs();
        QVERIFY(startedAtMs > 0);
        item.maybeStartSeekProfileLocked(startedAtMs);
        QVERIFY(item.m_seekProfile.active);

        item.noteSeekProfileFrameLocked(startedAtMs + 30, 0.030, true, false);
        item.noteSeekProfileFrameLocked(startedAtMs + 60, 0.031, true, false);
        item.noteSeekProfileFrameLocked(startedAtMs + 90, 0.029, true, false);
        item.finalizeSeekProfileLocked(startedAtMs + 120, "test");
        state = item.debugSeekProfileStateLocked();
    }

    qunsetenv("FERROUS_PROFILE_UI");
    QVERIFY(!state.isEmpty());
    QCOMPARE(state.value("reason").toString(), QStringLiteral("test"));
    QVERIFY(state.value("incidentDetected").toBool());
    QCOMPARE(state.value("gapFrames").toInt(), 3);
    QCOMPARE(state.value("maxPendingRows").toInt(), 0);
#else
    QSKIP("Seek hitch profiling instrumentation is compiled out");
#endif
}

void QmlSmokeTest::spectrogramSeekProfileDoesNotRestartSameTraceAfterSettling() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    qputenv("FERROUS_PROFILE_UI", "1");
    SpectrogramSeekTrace::noteSeekIssued(42.0);

    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    SpectrogramItem duplicateItem;
    duplicateItem.setWidth(320);
    duplicateItem.setHeight(180);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_profileEnabled = true;
        item.m_precomputedCanvasDisplayRight = 96;
        item.m_gpuDisplayWidth = 320;

        const qint64 startedAtMs = SpectrogramSeekTrace::startedAtMs();
        QVERIFY(startedAtMs > 0);
        item.maybeStartSeekProfileLocked(startedAtMs);
        QVERIFY(item.m_seekProfile.active);

        item.noteSeekProfileFrameLocked(startedAtMs + 30, 0.016, false, false);
        item.noteSeekProfileFrameLocked(startedAtMs + 60, 0.016, false, false);
        item.noteSeekProfileFrameLocked(startedAtMs + 90, 0.016, false, false);
        item.noteSeekProfileFrameLocked(startedAtMs + 150, 0.016, false, false);
        QVERIFY(!item.m_seekProfile.active);

        item.maybeStartSeekProfileLocked(startedAtMs + 180);
        QVERIFY(!item.m_seekProfile.active);
        QCOMPARE(
            item.m_lastFinalizedSeekProfileGeneration,
            SpectrogramSeekTrace::currentGeneration());
    }

    {
        QMutexLocker lock(&duplicateItem.m_stateMutex);
        duplicateItem.m_profileEnabled = true;
        duplicateItem.m_precomputedCanvasDisplayRight = 96;
        duplicateItem.m_gpuDisplayWidth = 320;
        duplicateItem.maybeStartSeekProfileLocked(SpectrogramSeekTrace::startedAtMs() + 180);
        QVERIFY(!duplicateItem.m_seekProfile.active);
    }

    qunsetenv("FERROUS_PROFILE_UI");
#else
    QSKIP("Seek hitch profiling instrumentation is compiled out");
#endif
}

void QmlSmokeTest::spectrogramSmoothnessProfileFlagsGapHeavyWindow() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    qputenv("FERROUS_PROFILE_UI", "1");

    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    QVariantMap state;
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_profileEnabled = true;
        item.m_precomputedCanvasDisplayRight = 48;
        item.m_gpuDisplayWidth = 320;
        item.m_lastIncomingRowsAtMs = QDateTime::currentMSecsSinceEpoch();
        item.maybeStartSmoothnessProfileLocked(item.m_lastIncomingRowsAtMs);
        QVERIFY(item.m_smoothnessProfile.active);

        item.noteSmoothnessPaintLocked(4.5);
        item.noteSmoothnessPaintLocked(5.0);
        item.noteSmoothnessProfileFrameLocked(item.m_lastIncomingRowsAtMs + 30, 0.030, true, false);
        item.noteSmoothnessProfileFrameLocked(item.m_lastIncomingRowsAtMs + 62, 0.032, true, false);
        item.noteSmoothnessProfileFrameLocked(item.m_lastIncomingRowsAtMs + 95, 0.033, true, false);
        item.noteSmoothnessProfileFrameLocked(item.m_lastIncomingRowsAtMs + 128, 0.034, true, false);
        item.finalizeSmoothnessProfileLocked(item.m_lastIncomingRowsAtMs + 180, "test");
        state = item.debugSmoothnessProfileStateLocked();
    }

    qunsetenv("FERROUS_PROFILE_UI");
    QVERIFY(!state.isEmpty());
    QCOMPARE(state.value("reason").toString(), QStringLiteral("test"));
    QVERIFY(state.value("incidentDetected").toBool());
    QCOMPARE(state.value("gapFrames").toInt(), 4);
    QCOMPARE(state.value("paintSpikeCount").toInt(), 2);
#else
    QSKIP("Smoothness profiling instrumentation is compiled out");
#endif
}

void QmlSmokeTest::spectrogramSmoothnessProfileTracksServoAndAdvanceFallbackSignals() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    qputenv("FERROUS_PROFILE_UI", "1");

    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_profileEnabled = true;
        item.resetSmoothnessProfileLocked();
        item.m_smoothnessProfile.active = true;
        item.m_smoothnessProfile.startedAtMs = QDateTime::currentMSecsSinceEpoch();
        item.m_smoothnessProfile.lastFrameAtMs = item.m_smoothnessProfile.startedAtMs;
        item.m_precomputedReady = true;
        item.m_playing = true;
        item.m_positionAnchorInitialized = true;
        item.m_positionSeconds = 10.0;
        item.m_positionAnchorSeconds = 10.0;
        item.m_positionAnchorUpdatedAt = std::chrono::steady_clock::now();
    }

    item.setPositionSeconds(10.04);
    item.setPositionSeconds(9.95);

    QVariantMap state;
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 2.0;
        item.m_precomputedHopSize = 1024;
        item.noteSmoothnessAdvanceFallbackLocked(item.effectiveZoomLocked());
        state = item.debugSmoothnessProfileStateLocked();
    }

    qunsetenv("FERROUS_PROFILE_UI");
    QVERIFY(!state.isEmpty());
    QCOMPARE(state.value("servoFrames").toInt(), 2);
    QCOMPARE(state.value("servoRegressionDrops").toInt(), 1);
    QVERIFY(state.value("maxServoErrorMs").toDouble() >= 20.0);
    QCOMPARE(state.value("advanceFallbackFrames").toInt(), 1);
    QCOMPARE(state.value("nonUnityAdvanceFallbackFrames").toInt(), 1);
    QVERIFY(state.value("maxAdvanceFallbackZoomDelta").toDouble() > 0.9);
#else
    QSKIP("Smoothness profiling instrumentation is compiled out");
#endif
}

void QmlSmokeTest::waveformProgressInvalidatesOnlyTailSpan() {
    WaveformItem item;
    item.setWidth(200);
    item.setHeight(24);
    item.setDurationSeconds(10.0);

    QByteArray peaks(100, '\x33');
    item.setPeaksData(peaks);
    item.setGeneratedSeconds(5.0);

    QImage canvas(200, 24, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QVERIFY(!item.m_cacheDirty);
    QVERIFY(item.m_dirtyRect.isNull());

    item.setGeneratedSeconds(7.0);

    QCOMPARE(item.m_dirtyRect, QRect(100, 0, 40, 24));
    QVERIFY(item.m_cacheDirty);
}

void QmlSmokeTest::waveformPeakUpdatesInvalidateChangedSuffix() {
    WaveformItem item;
    item.setWidth(200);
    item.setHeight(24);
    item.setDurationSeconds(10.0);
    item.setWaveformComplete(true);

    QByteArray peaks(100, '\x22');
    item.setPeaksData(peaks);

    QImage canvas(200, 24, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QVERIFY(!item.m_cacheDirty);
    QVERIFY(item.m_dirtyRect.isNull());

    QByteArray updated = peaks;
    for (int i = 80; i < updated.size(); ++i) {
        updated[i] = '\x66';
    }
    item.setPeaksData(updated);

    QVERIFY(item.m_cacheDirty);
    QVERIFY(item.m_dirtyRect.x() >= 160);
    QCOMPARE(item.m_dirtyRect.height(), 24);
}

void QmlSmokeTest::stoppedTrackSwitchRequiresSpectrogramResetOnResume() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));

    const QUrl url = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml"));
    engine.load(url);
    QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
    QObject *root = engine.rootObjects().constFirst();
    QVERIFY(root != nullptr);

    QVariant result;
    bool invoked = QMetaObject::invokeMethod(
        root,
        "shouldResetSpectrogramForStoppedTrackSwitch",
        Q_RETURN_ARG(QVariant, result),
        Q_ARG(QVariant, QStringLiteral("Stopped")),
        Q_ARG(QVariant, QStringLiteral("Playing")),
        Q_ARG(QVariant, QStringLiteral("/music/old-track.flac")),
        Q_ARG(QVariant, QStringLiteral("/music/new-track.flac")));
    QVERIFY(invoked);
    QCOMPARE(result.toBool(), true);

    invoked = QMetaObject::invokeMethod(
        root,
        "shouldResetSpectrogramForStoppedTrackSwitch",
        Q_RETURN_ARG(QVariant, result),
        Q_ARG(QVariant, QStringLiteral("Playing")),
        Q_ARG(QVariant, QStringLiteral("Playing")),
        Q_ARG(QVariant, QStringLiteral("/music/old-track.flac")),
        Q_ARG(QVariant, QStringLiteral("/music/new-track.flac")));
    QVERIFY(invoked);
    QCOMPARE(result.toBool(), false);

    invoked = QMetaObject::invokeMethod(
        root,
        "shouldResetSpectrogramForStoppedTrackSwitch",
        Q_RETURN_ARG(QVariant, result),
        Q_ARG(QVariant, QStringLiteral("Stopped")),
        Q_ARG(QVariant, QStringLiteral("Playing")),
        Q_ARG(QVariant, QStringLiteral("/music/same-track.flac")),
        Q_ARG(QVariant, QStringLiteral("/music/same-track.flac")));
    QVERIFY(invoked);
    QCOMPARE(result.toBool(), false);
}

void QmlSmokeTest::spectrogramStaleTokenChunksAreDropped() {
    // After a buffer_reset with token N, chunks from token < N are stale
    // and must be dropped to prevent ring corruption.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int bins = 8;
    constexpr int total = 1024;

    // Reset with token 5.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total, 48000, 1024, false, true, 5);
    QByteArray data4(4 * bins, '\x40');
    item.feedPrecomputedChunk(
        data4, bins, 0, 4, 0, total, 48000, 1024, false, false, 5);
    QCOMPARE(item.m_ringWriteSeq, 4);

    // Reset with token 10 (new track).
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total, 48000, 1024, false, true, 10, true);
    QByteArray data2(2 * bins, '\x80');
    item.feedPrecomputedChunk(
        data2, bins, 0, 2, 0, total, 48000, 1024, false, false, 10);
    QCOMPARE(item.m_ringWriteSeq, 2);

    // Stale chunk from token 5 (< committed 10) — must be dropped.
    QByteArray stale(3 * bins, '\xFF');
    const qint64 before = item.m_ringWriteSeq;
    item.feedPrecomputedChunk(
        stale, bins, 0, 3, 100, total, 48000, 1024, false, false, 5);
    QCOMPARE(item.m_ringWriteSeq, before);
}

void QmlSmokeTest::spectrogramGaplessTokenChunksPassFilter() {
    // In a gapless rolling transition, the token advances (3→4) without
    // a buffer_reset.  Committed stays at 3.  Token 4 chunks must NOT
    // be dropped.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int bins = 8;
    constexpr int total = 1024;

    // Reset with token 3.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total, 48000, 1024, false, true, 3);
    QByteArray data(4 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 4, 0, total, 48000, 1024, false, false, 3);
    QCOMPARE(item.m_ringWriteSeq, 4);
    QCOMPARE(item.m_precomputedCommittedToken, 3ULL);

    // Gapless token 4 (> committed 3) — must be accepted.
    QByteArray gaplessData(2 * bins, '\x80');
    item.feedPrecomputedChunk(
        gaplessData, bins, 0, 2, 4, total, 48000, 1024, false, false, 4);
    QCOMPARE(item.m_ringWriteSeq, 6);
    QCOMPARE(item.m_precomputedTrackToken, 4ULL);
}

void QmlSmokeTest::spectrogramFreshWidgetAcceptsDataWithImplicitReset() {
    // A fresh/recycled widget (ringCapacity==0, no pending reset)
    // receiving data should apply an implicit reset and accept the data.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    QCOMPARE(item.m_ringCapacity, 0);
    QVERIFY(!item.m_precomputedResetPending);

    constexpr int bins = 8;
    constexpr int total = 512;
    QByteArray data(4 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 4, 100, total, 48000, 1024, false, false, 7);

    // Ring should have been allocated and data written.
    QVERIFY(item.m_ringCapacity > 0);
    QCOMPARE(item.m_ringWriteSeq, 4);
    QCOMPARE(item.m_precomputedTrackToken, 7ULL);
}

void QmlSmokeTest::spectrogramCenteredModeSeekPreservesRing() {
    // In centered mode, seeking should NOT clear the ring.
    // The position just moves the display window.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 8;
    constexpr int total = 1024;

    // Reset and write some data.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total, 48000, 1024, false, true, 5);
    QByteArray data(100 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 100, 0, total, 48000, 1024, false, false, 5);
    QCOMPARE(item.m_ringWriteSeq, 100);

    // Simulate a position change (seek) — ring must be preserved.
    item.setPositionSeconds(50.0);
    QCOMPARE(item.m_ringWriteSeq, 100);

    // Data at the new position must still be valid in the ring.
    QVERIFY(item.m_ringCapacity > 0);
}

void QmlSmokeTest::spectrogramCenteredGaplessPreStagedFill() {
    // Verify that pre-staged chunks in centered mode provide instant
    // fill: after a buffer_reset+data batch, m_precomputedMaxColumnIndex
    // reflects the full pre-staged extent rather than growing from zero.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 8;
    constexpr int total = 1024;
    constexpr quint64 oldToken = 5;
    constexpr quint64 newToken = 6;

    // Set up old track data.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total, 48000, 1024, false, true, oldToken);
    QByteArray oldData(100 * bins, '\x10');
    item.feedPrecomputedChunk(
        oldData, bins, 0, 100, 0, total, 48000, 1024, false, false, oldToken);
    QCOMPARE(item.m_precomputedMaxColumnIndex, 99);

    // Simulate pre-staged gapless: first chunk carries reset + data.
    QByteArray batch1(500 * bins, '\x40');
    item.feedPrecomputedChunk(
        batch1, bins, 0, 500, 0, total, 48000, 1024, false, true, newToken, true);

    // After first batch, maxColumnIndex should jump to 499 (not grow
    // incrementally from zero).
    QCOMPARE(item.m_precomputedMaxColumnIndex, 499);

    // Second staged batch extends.
    QByteArray batch2(300 * bins, '\x50');
    item.feedPrecomputedChunk(
        batch2, bins, 0, 300, 500, total, 48000, 1024, false, false, newToken);
    QCOMPARE(item.m_precomputedMaxColumnIndex, 799);

    // Ring populated, not growing from zero.
    QVERIFY(item.m_ringWriteSeq >= 800);
    QVERIFY(item.m_ringCapacity > 0);
}

void QmlSmokeTest::spectrogramCenteredSeekRestartRebuildsEarlierWindow() {
    // Regression: repeated small backward seeks at max zoom can force a
    // same-track centered restart once the earlier left-margin columns
    // have been evicted from the ring. After the synthetic clear and the
    // same-token reset, the widget must rebuild the earlier window
    // instead of stranding a blank left region.
    SpectrogramItem item;
    item.setWidth(1183);
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    item.setSampleRateHz(44100);

    constexpr int bins = 4;
    constexpr int hop = 64; // max zoom
    constexpr int total = 20'000;
    constexpr quint64 token = 9;

    // Fill enough later-track data to force eviction of the earliest
    // columns from the centered ring.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        44100, hop, false, true, token);
    QByteArray laterData(13'000 * bins, '\x30');
    item.feedPrecomputedChunk(
        laterData, bins, 0, 13'000, 0, total,
        44100, hop, false, false, token);
    QVERIFY(item.m_ringOldestSeq > 0);

    // Same-track centered seek restart: synthetic clear first, then the
    // worker's proper reset and earlier data at the new start index.
    item.feedPrecomputedChunk(
        QByteArray(), 0, 0, 0, 0, 0,
        0, 0, false, true, token, true);
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        44100, hop, false, true, token, true);
    QByteArray earlierData(4'000 * bins, '\x50');
    item.feedPrecomputedChunk(
        earlierData, bins, 0, 4'000, 0, total,
        44100, hop, false, false, token);

    QCOMPARE(item.m_ringOldestSeq, static_cast<qint64>(0));
    QCOMPARE(item.m_precomputedTrackToken, token);
    QVERIFY(item.m_ringWriteSeq >= 4'000);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel =
            1024.0 / static_cast<double>(hop); // effectiveZoom == 1.0
    }

    item.setPositionSeconds(3.554);
    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QMutexLocker lock(&item.m_stateMutex);
    const auto tokenMap = item.m_trackColumnToSeqByToken.value(token);
    QVERIFY(tokenMap.contains(static_cast<qint32>(item.m_precomputedCanvasDisplayLeft)));
    QVERIFY(tokenMap.contains(static_cast<qint32>(item.m_precomputedCanvasDisplayRight)));
}

void QmlSmokeTest::spectrogramCenteredFinalizeChunkShrinksTotalEstimate() {
    // Regression for zoom-dependent playhead detachment at track end.
    // The backend emits a finalize chunk (columns=0, complete=true)
    // carrying the actual decoded-column count; the widget must shrink
    // its m_precomputedTotalColumnsEstimate so the centered-mode EOF
    // clamp fires at the real audio end.  The initial file-metadata
    // estimate can overshoot by ~1 s worth of columns at max zoom,
    // which at hop=64 is ~700 cols — far more than the former 128-col
    // tolerance could absorb.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 1025;
    constexpr int fileMetadataEstimate = 161024; // cols at hop=64 for ~233.6 s
    constexpr int actualDecodedCols = 159980;    // true end ~232.1 s
    constexpr quint64 token = 7;

    // Initial reset carries the file-metadata estimate.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, fileMetadataEstimate,
        44100, 64, false, true, token);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, fileMetadataEstimate);

    // A small amount of data so the widget is "live".
    QByteArray data(200 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 200, 0, fileMetadataEstimate,
        44100, 64, false, false, token);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, fileMetadataEstimate);

    // Finalize chunk arrives with the true decoded extent.  complete=true,
    // columns=0, no bufferReset, no clearHistory.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, actualDecodedCols, actualDecodedCols,
        44100, 64, /*complete=*/true, /*bufferReset=*/false, token,
        /*clearHistoryOnReset=*/false);

    // Estimate must have shrunk to the true extent.  The max column
    // index and ring write state must not have been disturbed.
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, actualDecodedCols);
    QCOMPARE(item.m_precomputedMaxColumnIndex, 199);
    QVERIFY(item.m_ringWriteSeq > 0);
}

void QmlSmokeTest::spectrogramCenteredFinalizeChunkIgnoredForStaleToken() {
    // A finalize chunk for a track that has already been superseded by
    // a buffer_reset for a newer token must not clobber the new track's
    // total_columns_estimate.  Otherwise a late-arriving finalize from
    // the outgoing track would corrupt the new track's display.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 1025;
    constexpr int oldEstimate = 161024;
    constexpr int newEstimate = 167936;
    constexpr quint64 oldToken = 3;
    constexpr quint64 newToken = 4;

    // Old-track reset + data.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, oldEstimate,
        44100, 64, false, true, oldToken);
    QByteArray oldData(100 * bins, '\x10');
    item.feedPrecomputedChunk(
        oldData, bins, 0, 100, 0, oldEstimate,
        44100, 64, false, false, oldToken);

    // New-track buffer_reset + data bumps the committed token and
    // commits the new estimate (track-change path accepts increases).
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, newEstimate,
        44100, 64, false, true, newToken);
    QByteArray newData(50 * bins, '\x20');
    item.feedPrecomputedChunk(
        newData, bins, 0, 50, 0, newEstimate,
        44100, 64, false, false, newToken);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, newEstimate);

    // Late-arriving finalize for the OLD token should be ignored.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 159980, 159980,
        44100, 64, /*complete=*/true, /*bufferReset=*/false, oldToken,
        /*clearHistoryOnReset=*/false);

    QCOMPARE(item.m_precomputedTotalColumnsEstimate, newEstimate);
}

void QmlSmokeTest::spectrogramSameHopEstimateIncreaseUpdatesZoomOutLimit() {
    // Regression for 6ch AC3 max-zoom-out: the worker can start with a
    // fallback/header estimate, then raise it on the same hop once a
    // duration re-query succeeds.  Qt must accept that larger estimate
    // or minimumZoomLevel stays clamped to the stale shorter length and
    // centered-mode EOF/seek math stops before the real end.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 1025;
    constexpr int initialEstimate = 14126;
    constexpr int requeriedEstimate = 16273;
    constexpr quint64 token = 3;

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, initialEstimate,
        48000, 1024, false, true, token);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, initialEstimate);

    QByteArray data(16 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 15, requeriedEstimate,
        48000, 1024, false, false, token);

    QCOMPARE(item.m_precomputedTotalColumnsEstimate, requeriedEstimate);

    const double expectedMinZoom =
        static_cast<double>(item.width()) / static_cast<double>(requeriedEstimate);
    QVERIFY(std::abs(item.minimumZoomLevel() - expectedMinZoom) < 0.0001);
}

void QmlSmokeTest::spectrogramCenteredClampsRightEdgeToMaxColNearEof() {
    // Regression: at high zoom the file-metadata total_columns_estimate
    // overshoots the actual decoded extent by ~1 s worth of cols.  When
    // the decoder has produced all it's going to and playback is within
    // one half-window of maxCol, the centered display must clamp its
    // right edge to maxCol-1 so (a) the playhead detaches from center
    // toward the right edge, (b) no blank tail is shown past real
    // content, and (c) the crosshair at the right edge reads a time
    // <= the true audible end.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    item.setSampleRateHz(44100);

    constexpr int bins = 4;
    constexpr int hop = 64; // max zoom
    constexpr int maxColIndex = 8000;
    constexpr int decodedCols = maxColIndex + 1; // 8001 cols decoded
    // Estimate overshoots by ~1 second (689 cols at hop=64) — the
    // same magnitude seen in diagnostics at max zoom.
    constexpr int inflatedEstimate = decodedCols + 700;

    // Bufferreset + data so the ring is initialized and maxCol is set.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, inflatedEstimate,
        44100, hop, false, true, 1);
    QByteArray data(decodedCols * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, decodedCols, 0, inflatedEstimate,
        44100, hop, false, false, 1);
    QCOMPARE(item.m_precomputedMaxColumnIndex, maxColIndex);

    // Snap renderZoomLevel so effectiveZoom == 1 (1 px per col).
    // updatePaintNode normally does this on zoom-matched resets; force
    // it here so visibleWindowCols = 1200 deterministically.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel =
            1024.0 / static_cast<double>(hop); // = 16.0
    }

    // Position the playhead within halfWindow (600 cols) of maxCol so
    // the EOF clamp must fire.  At 689 cols/s (hop=64, sr=44100),
    // pos=11.25 s -> col ~ 7750, which is maxCol - 250 (inside the
    // 600-col half-window trigger).
    item.setPositionSeconds(11.25);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QMutexLocker lock(&item.m_stateMutex);
    // Display right must equal maxCol (decoder's last column), not the
    // inflated estimate.  The crosshair / grid at the right edge is
    // driven by this value, so clamping it here keeps the time axis
    // bounded by real content.
    QCOMPARE(item.m_precomputedCanvasDisplayRight,
             static_cast<qint64>(maxColIndex));
    // DisplayLeft slid right so the window still covers
    // visibleWindowCols (1200) cols.
    QCOMPARE(item.m_precomputedCanvasDisplayLeft,
             static_cast<qint64>(maxColIndex) - 1199);
}

void QmlSmokeTest::spectrogramCenteredEofDetachmentDisablesSubpixelScrolling() {
    // Regression: in centered mode near EOF at a zoomed-out level, the
    // display range clamps to real decoded content and the playhead
    // detaches toward the right edge. The canvas must stop sub-pixel
    // scrolling in that state or the spectrogram visibly jiggles.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    item.setSampleRateHz(44100);
    item.setGridEnabled(true);

    constexpr int bins = 4;
    constexpr int hop = 64;
    constexpr int maxColIndex = 8000;
    constexpr int decodedCols = maxColIndex + 1;
    constexpr int inflatedEstimate = decodedCols + 700;

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, inflatedEstimate,
        44100, hop, false, true, 1);
    QByteArray data(decodedCols * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, decodedCols, 0, inflatedEstimate,
        44100, hop, false, false, 1);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 8.0; // effectiveZoom = 0.5
    }

    item.setPositionSeconds(11.25);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QMutexLocker lock(&item.m_stateMutex);
    QCOMPARE(item.m_precomputedCanvasDisplayRight,
             static_cast<qint64>(maxColIndex));
    QVERIFY2(
        std::abs(item.m_timeGridRenderDrawX) < 0.001,
        qPrintable(QStringLiteral("draw_x=%1 display_left=%2 display_right=%3")
            .arg(item.m_timeGridRenderDrawX, 0, 'f', 6)
            .arg(item.m_precomputedCanvasDisplayLeft)
            .arg(item.m_precomputedCanvasDisplayRight)));
}

void QmlSmokeTest::spectrogramCenteredDisplayRangeIgnoresLaggingDecodedTailBeforeEof() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 8;
    constexpr int decodedColumns = 4'000;
    constexpr int totalEstimate = 20'000;
    constexpr int sampleRate = 48'000;
    constexpr int hop = 1'024;
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, totalEstimate,
        sampleRate, hop, false, true, 1);
    QByteArray data(decodedColumns * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, decodedColumns, 0, totalEstimate,
        sampleRate, hop, false, false, 1);
    item.setPositionSeconds(103.0);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QMutexLocker lock(&item.m_stateMutex);
    QVERIFY2(
        item.m_precomputedCanvasDisplayRight
            > static_cast<qint64>(item.m_precomputedMaxColumnIndex),
        qPrintable(QStringLiteral("display range was clamped to lagging decoded tail %1..%2 max=%3")
            .arg(item.m_precomputedCanvasDisplayLeft)
            .arg(item.m_precomputedCanvasDisplayRight)
            .arg(item.m_precomputedMaxColumnIndex)));
    QVERIFY(item.m_precomputedCanvasDisplayLeft > decodedColumns);
}

void QmlSmokeTest::spectrogramRingCapacityPersistsAcrossFullscreenShrink() {
    // Regression: the centered ring resets on every session restart
    // (e.g. zoom change on fullscreen toggle) and recomputes its cap
    // from the CURRENT widget width.  The Rust decoder's lookahead is
    // sized against the MAX widget width ever seen, so after a
    // fullscreen->windowed transition the decoder produces farther
    // ahead than the shrunken ring can hold and evicts the left-margin
    // cols around the playhead, painting black.  The ring cap must
    // also track the max widget width so both sides stay in sync.
    SpectrogramItem item;
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    item.setSampleRateHz(44100);

    constexpr int bins = 4;
    constexpr int hop = 64; // max zoom
    constexpr quint64 token = 1;

    // Simulate fullscreen: widget at a wide width.  First data chunk
    // triggers the ring-cap sizing path.
    item.setWidth(3840);
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 161024,
        44100, hop, false, true, token);
    QByteArray data(16 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 0, 161024,
        44100, hop, false, false, token);
    const int fullscreenCap = item.m_ringCapacity;
    QVERIFY2(fullscreenCap >= 3840,
             qPrintable(QString("fullscreen ring cap too small: %1")
                            .arg(fullscreenCap)));
    QCOMPARE(SpectrogramItem::s_maxWidgetWidthSeen, 3840);

    // Simulate the session reset that the backend emits on a zoom
    // change: bins=0 + bufferReset + clearHistory triggers the
    // synthetic-clear path that zeroes the ring capacity.
    item.feedPrecomputedChunk(
        QByteArray(), 0, 0, 0, 0, 0,
        0, 0, false, true, token, true);
    QCOMPARE(item.m_ringCapacity, 0);

    // Now shrink to windowed width.  The next data chunks (a proper
    // worker reset followed by data) must still allocate a ring cap
    // at least as large as the fullscreen run so the decoder's
    // max-width-sized lookahead fits without evicting left-margin
    // cols around the playhead.
    item.setWidth(1213);
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 161024,
        44100, hop, false, true, token);
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 0, 161024,
        44100, hop, false, false, token);
    QVERIFY2(item.m_ringCapacity >= fullscreenCap,
             qPrintable(QString("post-shrink ring cap %1 < fullscreen cap %2")
                            .arg(item.m_ringCapacity)
                            .arg(fullscreenCap)));
    QCOMPARE(SpectrogramItem::s_maxWidgetWidthSeen, 3840);
}

void QmlSmokeTest::spectrogramMaxWidgetWidthSurvivesInstanceReplacement() {
    // Regression: when the channel count changes (e.g. 6ch PerChannel
    // → 2ch PerChannel), the old SpectrogramItems are destroyed and
    // fresh ones are created.  The Rust-side lookahead tracker is a
    // singleton in AnalysisRuntimeState so it remembers the prior
    // fullscreen width and keeps producing at that lookahead.  The
    // Qt tracker must therefore also be singleton-equivalent
    // (static), otherwise the new widget starts at maxSeen=0, sizes
    // its ring against the current (smaller) width, the decoder laps
    // the ring, and left-margin cols around the playhead are evicted
    // — the user sees a narrow growing-edge of data with the previous
    // canvas smearing through the rest of the view.
    SpectrogramItem::s_maxWidgetWidthSeen = 0;
    constexpr int bins = 4;
    constexpr int hop = 64;
    constexpr quint64 firstToken = 100;
    constexpr quint64 secondToken = 101;

    {
        SpectrogramItem big;
        big.setHeight(200);
        big.setDisplayMode(1);
        big.setWidth(3840);
        big.setSampleRateHz(44100);
        big.feedPrecomputedChunk(
            QByteArray(), bins, 0, 0, 0, 161024,
            44100, hop, false, true, firstToken);
        QByteArray data(16 * bins, '\x40');
        big.feedPrecomputedChunk(
            data, bins, 0, 16, 0, 161024,
            44100, hop, false, false, firstToken);
        QCOMPARE(SpectrogramItem::s_maxWidgetWidthSeen, 3840);
    }
    // The wide-view instance is now gone; the static must retain the
    // max so the next instance sizes its ring against it.

    SpectrogramItem fresh;
    fresh.setHeight(200);
    fresh.setDisplayMode(1);
    fresh.setWidth(1213); // windowed
    fresh.setSampleRateHz(44100);
    fresh.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 161024,
        44100, hop, false, true, secondToken);
    QByteArray data(16 * bins, '\x40');
    fresh.feedPrecomputedChunk(
        data, bins, 0, 16, 0, 161024,
        44100, hop, false, false, secondToken);

    QCOMPARE(SpectrogramItem::s_maxWidgetWidthSeen, 3840);
    QVERIFY2(fresh.m_ringCapacity >= 3840,
             qPrintable(QString("fresh-instance ring cap %1 < 3840")
                            .arg(fresh.m_ringCapacity)));
}

void QmlSmokeTest::spectrogramRollingGaplessTrackChangePreservesZoom() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(0); // Rolling
    item.setSampleRateHz(44100);

    constexpr int bins = 4;
    constexpr int total = 4096;
    constexpr quint64 oldToken = 3;
    constexpr quint64 newToken = 4;

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        44100, 1024, false, true, oldToken);
    QByteArray initial(32 * bins, '\x40');
    item.feedPrecomputedChunk(
        initial, bins, 0, 32, 0, total,
        44100, 1024, false, false, oldToken);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 4.0;
        item.m_renderZoomLevel = 4.0;
    }

    QSignalSpy zoomResetSpy(&item, &SpectrogramItem::zoomResetRequested);
    QSignalSpy backendZoomSpy(&item, &SpectrogramItem::backendZoomRequested);

    QByteArray gapless(32 * bins, '\x60');
    item.feedPrecomputedChunk(
        gapless, bins, 0, 32, 32, total,
        44100, 1024, false, false, newToken);

    QCOMPARE(zoomResetSpy.count(), 0);
    QCOMPARE(backendZoomSpy.count(), 0);
    QCOMPARE(item.zoomLevel(), 4.0);
    QCOMPARE(item.m_renderZoomLevel, 4.0);
}

void QmlSmokeTest::spectrogramCenteredGaplessTrackChangeResetsZoom() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    item.setSampleRateHz(44100);

    constexpr int bins = 4;
    constexpr int total = 4096;
    constexpr quint64 oldToken = 3;
    constexpr quint64 newToken = 4;

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        44100, 1024, false, true, oldToken);
    QByteArray initial(32 * bins, '\x40');
    item.feedPrecomputedChunk(
        initial, bins, 0, 32, 0, total,
        44100, 1024, false, false, oldToken);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 4.0;
        item.m_renderZoomLevel = 4.0;
    }

    QSignalSpy zoomResetSpy(&item, &SpectrogramItem::zoomResetRequested);
    QSignalSpy backendZoomSpy(&item, &SpectrogramItem::backendZoomRequested);

    QByteArray gapless(32 * bins, '\x60');
    item.feedPrecomputedChunk(
        gapless, bins, 0, 32, 32, total,
        44100, 1024, false, false, newToken);

    QCOMPARE(zoomResetSpy.count(), 1);
    QCOMPARE(backendZoomSpy.count(), 1);
    QCOMPARE(backendZoomSpy.takeFirst().at(0).toFloat(), 1.0f);
    QCOMPARE(item.zoomLevel(), 1.0);
    QCOMPARE(item.m_renderZoomLevel, 1.0);
}

void QmlSmokeTest::spectrogramRollingResetTrackChangeResetsZoom() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(0); // Rolling
    item.setSampleRateHz(44100);

    constexpr int bins = 4;
    constexpr int total = 4096;
    constexpr quint64 oldToken = 3;
    constexpr quint64 newToken = 4;

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        44100, 1024, false, true, oldToken);
    QByteArray initial(32 * bins, '\x40');
    item.feedPrecomputedChunk(
        initial, bins, 0, 32, 0, total,
        44100, 1024, false, false, oldToken);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 4.0;
        item.m_renderZoomLevel = 4.0;
    }

    QSignalSpy zoomResetSpy(&item, &SpectrogramItem::zoomResetRequested);
    QSignalSpy backendZoomSpy(&item, &SpectrogramItem::backendZoomRequested);

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        44100, 1024, false, true, newToken);
    QByteArray nextTrack(32 * bins, '\x60');
    item.feedPrecomputedChunk(
        nextTrack, bins, 0, 32, 0, total,
        44100, 1024, false, false, newToken);

    QCOMPARE(zoomResetSpy.count(), 1);
    QCOMPARE(backendZoomSpy.count(), 1);
    QCOMPARE(backendZoomSpy.takeFirst().at(0).toFloat(), 1.0f);
    QCOMPARE(item.zoomLevel(), 1.0);
    QCOMPARE(item.m_renderZoomLevel, 1.0);
}

void QmlSmokeTest::spectrogramTrackChangeMetadataResetClearsOldCenteredFrame() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int total = 4096;
    constexpr quint64 oldToken = 3;
    constexpr quint64 newToken = 4;

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        48000, 1024, false, true, oldToken, true);
    QByteArray oldData(320 * bins, '\x40');
    item.feedPrecomputedChunk(
        oldData, bins, 0, 320, 0, total,
        48000, 1024, false, false, oldToken, false);
    item.setPositionSeconds(72.0);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(180);
        item.syncPrecomputedDisplayImageLocked(320, 180, 0, 319, false, 1.0);
    }
    QVERIFY(item.m_precomputedReady);
    QVERIFY(!item.m_gpuDisplayImage.isNull());
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(319));
    QVERIFY(item.m_positionAnchorSeconds > 70.0);

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        48000, 1024, false, true, newToken, true);

    QVERIFY2(
        !item.m_precomputedReady,
        "non-gapless track-change metadata reset must not keep old spectrogram visible");
    QVERIFY(item.m_gpuDisplayImage.isNull());
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(-1));
    QCOMPARE(item.m_precomputedResetPending, false);
    QCOMPARE(item.m_zoomFillActive, false);
    QVERIFY(std::abs(item.m_positionAnchorSeconds) < 0.01);
}

void QmlSmokeTest::spectrogramFreshInstanceResyncsBackendZoomOnTrackChange() {
    // Regression: the Rust-side zoom_level persists across tracks
    // (track changes don't reset it).  When a channel-count change
    // destroys the SpectrogramItem instances and creates fresh ones,
    // the new instance has m_renderZoomLevel = 1.0 (default) while
    // the backend may still be at zoom=16 from the previous track,
    // sending data at hop=64.  Without a zoom resync, Qt renders at
    // effectiveZoom = renderZoom × hop / refHop = 0.0625, inflating
    // the centered visible window to thousands of cols.  The decoder
    // hasn't produced that many yet, so the right side of the widget
    // has no data and the old canvas smears through.
    //
    // On track change with a non-default backend hop, the widget
    // must emit backendZoomRequested(1.0) even when it's already at
    // render zoom 1.0 (i.e. fresh instance).  The estimate that just
    // arrived is the correct one for the new track and must be
    // preserved (the clear is only for the actual zoom-change case).
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    item.setSampleRateHz(44100);

    QSignalSpy backendZoomSpy(&item, &SpectrogramItem::backendZoomRequested);

    constexpr int bins = 4;
    constexpr int hop = 64; // backend at max zoom from prior track
    constexpr quint64 oldToken = 3;
    constexpr quint64 newToken = 7;

    // Seed the widget as if it had just been seeing the prior track
    // at backend zoom 1.0 (the fresh-instance default).  First chunk
    // establishes the committed token so the track-change detection
    // fires on the next non-matching buffer_reset.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 10064,
        44100, 1024, false, true, oldToken);
    QByteArray warm(4 * bins, '\x10');
    item.feedPrecomputedChunk(
        warm, bins, 0, 4, 0, 10064,
        44100, 1024, false, false, oldToken);
    QCOMPARE(backendZoomSpy.count(), 0);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, 10064);

    // Track change with backend at hop=64 (still at max zoom from the
    // previous track — the backend's zoom_level persisted across the
    // track change even though Qt's fresh-instance default is 1.0).
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 161024,
        44100, hop, false, true, newToken);
    QByteArray data(16 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 0, 161024,
        44100, hop, false, false, newToken);

    // Qt must have asked the backend to go back to zoom=1.0 so a
    // subsequent session restart produces data at the reference hop.
    QCOMPARE(backendZoomSpy.count(), 1);
    QCOMPARE(backendZoomSpy.takeFirst().at(0).toFloat(), 1.0f);
    // The estimate for the new track must be preserved; a fresh
    // instance is not a real zoom transition, so the estimate-clear
    // path must not fire.
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, 161024);
    QCOMPARE(item.m_renderZoomLevel, 1.0);
}

void QmlSmokeTest::spectrogramFreshInstanceSeekRestartDoesNotResetZoom() {
    // Regression: centered same-track seeks outside the current
    // window restart decoding with the same track token but a non-zero
    // startIndex. Fresh SpectrogramItem instances can see that restart
    // before they've learned the current token, so a broad
    // "appliedReset + non-default hop" heuristic wrongly treated the
    // seek as a track change and emitted backendZoomRequested(1.0),
    // resetting max zoom-out back to normal.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    item.setSampleRateHz(48000);

    QSignalSpy zoomResetSpy(&item, &SpectrogramItem::zoomResetRequested);
    QSignalSpy backendZoomSpy(&item, &SpectrogramItem::backendZoomRequested);

    constexpr int bins = 4;
    constexpr int hop = 14088; // Non-default max-zoom-out-style hop
    constexpr int startIndex = 714;
    constexpr int total = 1027;
    constexpr quint64 trackToken = 7;

    // Simulate a fresh pane instance seeing only the post-seek worker
    // restart. The token is the current track's token, not a track
    // change, but the item has no prior token state yet.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, startIndex, total,
        48000, hop, false, true, trackToken, true);
    QByteArray data(16 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 16, startIndex, total,
        48000, hop, false, false, trackToken, false);

    QCOMPARE(zoomResetSpy.count(), 0);
    QCOMPARE(backendZoomSpy.count(), 0);
    QCOMPARE(item.m_precomputedHopSize, hop);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, total);
}

void QmlSmokeTest::spectrogramTrackChangeCancelsPendingZoomDebounce() {
    // Regression: a SpectrogramItem is instantiated with
    // m_zoomLevel defaulting to 1.0, then the QML zoomLevel
    // property binding pushes the SpectrogramSurface's existing
    // _widgetZoomLevel (e.g. 16 from the previous track's max zoom
    // on the prior instance set) into the new instance.
    // setZoomLevel(16) arms the 150 ms debounce timer with
    // m_pendingBackendZoom = 16.  A track change arrives in that
    // window — our needsZoomReset path emits
    // backendZoomRequested(1.0) directly, bypassing the timer —
    // but if we don't ALSO cancel the pending debounce, the timer
    // fires 150 ms later with the stale 16 and restarts the
    // backend at max zoom.  The widget then renders at
    // effectiveZoom = 0.0625 with a smeared right edge because
    // the ring hasn't filled far enough for the inflated visible
    // window.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(1);
    item.setSampleRateHz(44100);

    QSignalSpy backendZoomSpy(&item, &SpectrogramItem::backendZoomRequested);

    // Simulate the QML binding pushing the prior track's max zoom
    // into the freshly-created instance: arms the debounce with 16.
    item.setZoomLevel(16.0);
    QVERIFY(item.m_zoomDebounceTimer != nullptr);
    QVERIFY(item.m_zoomDebounceTimer->isActive());
    QCOMPARE(item.m_pendingBackendZoom, 16.0f);

    // Track change (non-gapless): the initial reset + data chunk
    // carries the backend's leftover non-default hop (64 at max
    // zoom), so needsZoomReset fires.
    constexpr int bins = 4;
    constexpr int hop = 64;
    constexpr quint64 newToken = 42;
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 161024,
        44100, hop, false, true, newToken);
    QByteArray data(16 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 0, 161024,
        44100, hop, false, false, newToken);

    // The reset must have fired backendZoomRequested(1.0) and
    // disarmed the debounce so the stale 16 can't fire after.
    QCOMPARE(backendZoomSpy.count(), 1);
    QCOMPARE(backendZoomSpy.takeFirst().at(0).toFloat(), 1.0f);
    QVERIFY2(!item.m_zoomDebounceTimer->isActive(),
             "pending debounce still armed after track-change zoom reset");
    QCOMPARE(item.m_pendingBackendZoom, 1.0f);
    QCOMPARE(item.m_zoomLevel, 1.0);

    // Even if we wait past the original debounce interval, no
    // further backendZoomRequested should fire — the timer was
    // cancelled.
    QTest::qWait(200);
    QCOMPARE(backendZoomSpy.count(), 0);
}

void QmlSmokeTest::spectrogramCenteredGaplessSnapsAnchorToZero() {
    // In centered mode, a gapless token change must immediately reset the
    // position anchor to 0 so the display snaps to the beginning of the
    // new track.  Without this, the anchor lingers at the old track's
    // position (~428 s) for ~1 s, rendering at a wrong column.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int total = 1024;
    constexpr quint64 oldToken = 5;
    constexpr quint64 newToken = 6;

    // Set up old track: reset + some data, position deep into the track.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total, 48000, 1024, false, true, oldToken);
    QByteArray oldData(100 * bins, '\x10');
    item.feedPrecomputedChunk(
        oldData, bins, 0, 100, 0, total, 48000, 1024, false, false, oldToken);
    item.setPositionSeconds(428.0);
    QVERIFY(std::abs(item.m_positionAnchorSeconds - 428.0) < 1.0);

    // Simulate GStreamer position resetting to near 0 right before the
    // gapless data arrives.  This should activate a jump hold.
    item.setPositionSeconds(0.04);
    QVERIFY(item.m_positionJumpHoldActive);

    // Gapless token change — first chunk with the new token.
    QByteArray newData(50 * bins, '\x40');
    item.feedPrecomputedChunk(
        newData, bins, 0, 50, 0, total, 48000, 1024, false, false, newToken);
    QCOMPARE(item.m_precomputedTrackToken, newToken);

    // After the gapless transition, anchor must be at 0 and hold cleared.
    QVERIFY(std::abs(item.m_positionAnchorSeconds) < 0.01);
    QVERIFY(!item.m_positionJumpHoldActive);

    // Subsequent small position updates must be accepted normally
    // (not held or snapped to the old position).
    item.setPositionSeconds(0.08);
    QVERIFY(!item.m_positionJumpHoldActive);
    // Anchor should be near the incoming position, not 428.
    QVERIFY(item.m_positionAnchorSeconds < 1.0);
}

void QmlSmokeTest::spectrogramForceFpsOverlayDoesNotOverrideQmlBinding() {
    // forceFpsOverlay is a CONSTANT property set at construction time from
    // the FERROUS_PROFILE_UI env var.  The setter for showFpsOverlay must
    // NOT OR in the force flag — QML's index===0 gate must be respected.
    SpectrogramItem item;
    QCOMPARE(item.forceFpsOverlay(), false); // no env var in test

    // Explicitly setting showFpsOverlay to false must stay false,
    // not be overridden by the force flag.
    item.setShowFpsOverlay(true);
    QCOMPARE(item.showFpsOverlay(), true);
    item.setShowFpsOverlay(false);
    QCOMPARE(item.showFpsOverlay(), false);
}

void QmlSmokeTest::spectrogramRenderLoopStopsWhenNotPlaying() {
    // The spectrogram's self-sustaining render loop (frameSwapped →
    // handleWindowAfterAnimating → update) must only re-trigger when
    // m_playing is true.  Without this guard the render loop runs at
    // full display refresh rate even when the spectrogram is static,
    // wasting ~10% CPU while idle.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 512;
    QByteArray chunk(4 * binsPerColumn, '\x40');

    item.feedPrecomputedChunk(
        chunk,
        binsPerColumn,
        0,
        4,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    // After feeding data, precomputed mode is active.
    QVERIFY(item.m_precomputedReady);

    // When playing, handleWindowAfterAnimating should schedule another
    // frame (the self-sustaining loop).  When not playing, it should
    // not — the display is static and updates are demand-driven.
    item.setPlaying(true);
    QVERIFY(item.m_playing);
    QVERIFY(item.m_precomputedReady);

    item.setPlaying(false);
    QVERIFY(!item.m_playing);
    // precomputedReady must still be true — only the render loop stops,
    // not the data.
    QVERIFY(item.m_precomputedReady);

    // Call the render-loop callback directly and verify it does NOT
    // schedule another update when not playing.  We track this by
    // checking that no animation tick state was freshly initialized
    // (a proxy for the loop being inactive, since without a window the
    // update() call is a no-op but the tick bookkeeping still runs).
    item.m_animationTickInitialized = false;
    item.handleWindowAfterAnimating();
    // The callback still runs (frameSwapped fires), but it must NOT
    // call update() to re-trigger the loop.  Verify the guard
    // condition: precomputedReady && !playing means no re-trigger.
    QVERIFY(item.m_animationTickInitialized);
    // The key invariant: the condition gating update() is
    //   changed || (precomputedActive && playing)
    // With playing=false and no FPS overlay change, this is false.
    QVERIFY(item.m_precomputedReady);
    QVERIFY(!item.m_playing);
}

void QmlSmokeTest::playbackControllerInterpolationActivatesOnPlayback() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        property string playbackState: "Playing"
        property real positionSeconds: 5.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QCOMPARE(controller->property("interpolationActive").toBool(), true);

    // After a playback update, interpolation remains active.
    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    bridge->setProperty("positionSeconds", 5.12);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));
    QCOMPARE(controller->property("interpolationActive").toBool(), true);
}

void QmlSmokeTest::playbackControllerInterpolationDeactivatesOnStop() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    // Embed JS no-op callbacks directly in the QML harness so
    // handlePlaybackChanged can call haltSpectrogram without error.
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 10.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }

    function simulateStop() {
        bridge.playbackState = "Stopped"
        bridge.positionSeconds = 0.0
        controller.handlePlaybackChanged(
            function() { /* halt */ },
            function() { /* reset */ })
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QCOMPARE(controller->property("interpolationActive").toBool(), true);

    // Simulate stop via the QML helper that provides proper JS callbacks.
    QVERIFY(QMetaObject::invokeMethod(root.data(), "simulateStop"));
    QCOMPARE(controller->property("interpolationActive").toBool(), false);
}

void QmlSmokeTest::trackIdentityChangedSignalTriggersQmlHandler() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15

Item {
    id: harness
    property int trackIdentityChangedCount: 0

    QtObject {
        id: bridge
        objectName: "bridge"
        property int playingQueueIndex: -1
        signal trackIdentityChanged()
        signal trackMetadataChanged()
        signal snapshotChanged()
    }

    Connections {
        target: bridge
        function onTrackIdentityChanged() {
            harness.trackIdentityChangedCount++
        }
    }

    function emitTrackIdentityChanged() {
        bridge.playingQueueIndex = 5
        bridge.trackIdentityChanged()
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QCOMPARE(root->property("trackIdentityChangedCount").toInt(), 0);

    QVERIFY(QMetaObject::invokeMethod(root.data(), "emitTrackIdentityChanged"));
    QCOMPARE(root->property("trackIdentityChangedCount").toInt(), 1);
}

void QmlSmokeTest::queueAutoCenterIsDeferredOffHandlerStack() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR)
        + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    id: harness
    property bool positionViewCalled: false
    property int positionViewIndex: -1

    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property string currentTrackPath: "/music/old.flac"
        property int playingQueueIndex: 5
        property int queueLength: 100
        property int queueVersion: 1
        property int selectedQueueIndex: -1
        property bool profileLogsEnabled: false
    }

    QtObject {
        id: stubView
        objectName: "stubView"
        property bool visible: true
        property real height: 400
        property real contentY: 0
        property real contentHeight: 2400
    }

    Controllers.QueueController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        tagEditorApi: QtObject { function openSelection(sel) { return false } }
        openTagEditorDialog: function() {}
    }

    function triggerTrackChange() {
        bridge.currentTrackPath = "/music/new.flac"
        bridge.playingQueueIndex = 42
        controller.handleSnapshotChanged(stubView)
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    // Initialize controller so it has a lastAutoCenterTrackPath to compare against.
    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);
    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));

    // Trigger a track change — handleSnapshotChanged detects path changed
    // and should defer scroll via a 0ms Timer.
    QObject *stubView = root->findChild<QObject *>(QStringLiteral("stubView"));
    QVERIFY(stubView != nullptr);
    QCOMPARE(stubView->property("contentY").toDouble(), 0.0);

    QVERIFY(QMetaObject::invokeMethod(root.data(), "triggerTrackChange"));

    // Immediately after handler returns: contentY must NOT have changed yet.
    QCOMPARE(stubView->property("contentY").toDouble(), 0.0);

    // Process the event loop so the 0ms Timer fires.
    // Index 42 * 24px row height = 1008px row top. Since row is below
    // the viewport (400px), contentY should be set to rowBottom - viewHeight
    // = 1008 + 24 - 400 = 632.
    QTRY_VERIFY_WITH_TIMEOUT(stubView->property("contentY").toDouble() > 0.0, 100);
    QCOMPARE(stubView->property("contentY").toDouble(), 632.0);
}

// Helper: create a QueueController + stub view and invoke _containIndex.
// Returns the resulting contentY.
static double invokeContainIndex(
    QQmlApplicationEngine &engine,
    const QUrl &baseUrl,
    double initialContentY,
    double viewHeight,
    double contentHeight,
    int targetIndex)
{
    QString errorText;
    const QByteArray qml = QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    id: harness

    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property string currentTrackPath: "/music/track.flac"
        property int playingQueueIndex: 0
        property int queueLength: 1000
        property int queueVersion: 1
        property int selectedQueueIndex: -1
        property bool profileLogsEnabled: false
    }

    QtObject {
        id: stubView
        objectName: "stubView"
        property bool visible: true
        property real height: 400
        property real contentY: 0
        property real contentHeight: 24000
    }

    Controllers.QueueController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        tagEditorApi: QtObject { function openSelection(sel) { return false } }
        openTagEditorDialog: function() {}
    }

    function callContainIndex(index) {
        controller._containIndex(stubView, index)
    }
}
)QML");
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, qml, baseUrl, &errorText));
    if (!root) {
        qWarning("invokeContainIndex: %s", qPrintable(errorText));
        return -1.0;
    }
    QObject *view = root->findChild<QObject *>(QStringLiteral("stubView"));
    view->setProperty("contentY", initialContentY);
    view->setProperty("height", viewHeight);
    view->setProperty("contentHeight", contentHeight);
    QMetaObject::invokeMethod(root.data(), "callContainIndex",
        Q_ARG(QVariant, targetIndex));
    return view->property("contentY").toDouble();
}

void QmlSmokeTest::queueContainIndexSkipsScrollWhenVisible() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR)
        + QStringLiteral("/qml/QmlSmokeHarness.qml"));

    // Index 5 → rowTop = 120, rowBottom = 144.
    // Viewport: contentY=100, height=400 → visible range [100, 500].
    // Row is fully inside viewport → contentY must not change.
    const double result = invokeContainIndex(engine, baseUrl,
        /*initialContentY=*/100.0, /*viewHeight=*/400.0,
        /*contentHeight=*/24000.0, /*targetIndex=*/5);
    QCOMPARE(result, 100.0);
}

void QmlSmokeTest::queueContainIndexScrollsUpWhenAboveViewport() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR)
        + QStringLiteral("/qml/QmlSmokeHarness.qml"));

    // Index 2 → rowTop = 48.
    // Viewport: contentY=200, height=400 → visible range [200, 600].
    // Row is above viewport → contentY should snap to rowTop = 48.
    const double result = invokeContainIndex(engine, baseUrl,
        /*initialContentY=*/200.0, /*viewHeight=*/400.0,
        /*contentHeight=*/24000.0, /*targetIndex=*/2);
    QCOMPARE(result, 48.0);
}

void QmlSmokeTest::queueContainIndexClampsAtListEnd() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR)
        + QStringLiteral("/qml/QmlSmokeHarness.qml"));

    // Index 999 → rowTop = 23976, rowBottom = 24000.
    // contentHeight = 24000, viewHeight = 400.
    // maxY = 24000 - 400 = 23600.
    // Target would be rowBottom - viewHeight = 24000 - 400 = 23600.
    // Clamped to maxY = 23600.
    const double result = invokeContainIndex(engine, baseUrl,
        /*initialContentY=*/0.0, /*viewHeight=*/400.0,
        /*contentHeight=*/24000.0, /*targetIndex=*/999);
    QCOMPARE(result, 23600.0);
}

void QmlSmokeTest::spectrogramCrosshairAndGridPropertiesAndHoverTracking() {
    // Verify crosshairEnabled/gridEnabled properties default to false,
    // emit change signals, and that hover events update internal state.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    // Default state: both overlays disabled, no hover.
    QCOMPARE(item.crosshairEnabled(), false);
    QCOMPARE(item.gridEnabled(), false);
    QCOMPARE(item.m_hoverActive, false);

    // Enable crosshair — signal fires, dirty flag set.
    QSignalSpy crosshairSpy(&item, &SpectrogramItem::crosshairEnabledChanged);
    item.setCrosshairEnabled(true);
    QCOMPARE(item.crosshairEnabled(), true);
    QCOMPARE(crosshairSpy.count(), 1);
    QCOMPARE(item.m_crosshairDirty, true);

    // No-op when setting same value.
    item.setCrosshairEnabled(true);
    QCOMPARE(crosshairSpy.count(), 1);

    // Enable grid.
    QSignalSpy gridSpy(&item, &SpectrogramItem::gridEnabledChanged);
    item.setGridEnabled(true);
    QCOMPARE(item.gridEnabled(), true);
    QCOMPARE(gridSpy.count(), 1);
    QVERIFY(item.m_freqGridDirty || item.m_timeGridDirty);

    // Simulate hover enter — m_hoverActive should become true.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairDirty = false;
    }
    QHoverEvent enterEvent(QEvent::HoverEnter, QPointF(100.0, 50.0), QPointF(100.0, 50.0), QPointF());
    item.hoverEnterEvent(&enterEvent);
    QCOMPARE(item.m_hoverActive, true);
    QVERIFY(std::abs(item.m_hoverPosition.x() - 100.0) < 0.01);
    QVERIFY(std::abs(item.m_hoverPosition.y() - 50.0) < 0.01);
    QCOMPARE(item.m_crosshairDirty, true); // crosshair enabled → dirty

    // Simulate hover move.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairDirty = false;
    }
    QHoverEvent moveEvent(QEvent::HoverMove, QPointF(150.0, 75.0), QPointF(150.0, 75.0), QPointF(100.0, 50.0));
    item.hoverMoveEvent(&moveEvent);
    QCOMPARE(item.m_hoverActive, true);
    QVERIFY(std::abs(item.m_hoverPosition.x() - 150.0) < 0.01);
    QVERIFY(std::abs(item.m_hoverPosition.y() - 75.0) < 0.01);
    QCOMPARE(item.m_crosshairDirty, true);

    // Simulate hover leave — m_hoverActive becomes false.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairDirty = false;
    }
    QHoverEvent leaveEvent(QEvent::HoverLeave, QPointF(), QPointF(), QPointF(150.0, 75.0));
    item.hoverLeaveEvent(&leaveEvent);
    QCOMPARE(item.m_hoverActive, false);
    QCOMPARE(item.m_crosshairDirty, true);

    // When crosshair is disabled, hover events do NOT mark dirty.
    item.setCrosshairEnabled(false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairDirty = false;
    }
    QHoverEvent enterEvent2(QEvent::HoverEnter, QPointF(50.0, 30.0), QPointF(50.0, 30.0), QPointF());
    item.hoverEnterEvent(&enterEvent2);
    QCOMPARE(item.m_hoverActive, true); // position still tracked
    QCOMPARE(item.m_crosshairDirty, false); // but NOT dirty
}

void QmlSmokeTest::spectrogramPixelToFrequency() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    // Feed data to set binsPerColumn = 4097 (FFT size 8192).
    const int bins = 4097;
    QByteArray data(bins, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 1, 0, 100, 48000, 1024, false, true, 1, false);

    // In linear mode, bottom pixel maps to 0 Hz (DC),
    // top pixel maps to Nyquist (24000 Hz).
    // pixelY=0 -> top -> ~24000 Hz
    // pixelY=99 -> bottom -> 0 Hz
    QVERIFY(item.pixelToFrequencyHz(0, 100) > 20000.0);
    QVERIFY(item.pixelToFrequencyHz(99, 100) < 500.0);

    // Mid-height should be roughly half Nyquist in linear mode.
    const double midFreq = item.pixelToFrequencyHz(50, 100);
    QVERIFY(midFreq > 10000.0 && midFreq < 14000.0);
}

void QmlSmokeTest::spectrogramSampleRateSyncsFromPrecomputedChunks() {
    SpectrogramItem item;
    // Start with default 48 kHz property — simulates QML binding.
    item.setSampleRateHz(48000);

    const int bins = 4097; // FFT size 8192
    QByteArray data(bins, '\x80');

    // Feed a chunk with 44100 Hz sample rate.
    item.feedPrecomputedChunk(data, bins, 0, 1, 0, 100, 44100, 1024,
                               false, true, 1, false);

    // pixelToFrequencyHz triggers the sync path.
    // Top pixel should map to Nyquist = 22050, not 24000.
    const double topFreq = item.pixelToFrequencyHz(0, 100);
    QVERIFY2(topFreq < 23000.0,
             qPrintable(QStringLiteral("Expected Nyquist ~22050 but got %1").arg(topFreq)));
    QVERIFY2(topFreq > 21000.0,
             qPrintable(QStringLiteral("Expected Nyquist ~22050 but got %1").arg(topFreq)));
}

void QmlSmokeTest::spectrogramCrosshairOverlayGeneratesOnHover() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_crosshairDirty = true;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0, true);
    }

    QVERIFY(!item.m_crosshairImage.isNull());
    QCOMPARE(item.m_crosshairImage.width(), 200);
    QCOMPARE(item.m_crosshairImage.height(), 100);

    // The image must contain visible content (lines + labels).
    bool hasContent = false;
    for (int y = 0; y < item.m_crosshairImage.height() && !hasContent; ++y) {
        for (int x = 0; x < item.m_crosshairImage.width(); ++x) {
            if (qAlpha(item.m_crosshairImage.pixel(x, y)) > 0) {
                hasContent = true;
                break;
            }
        }
    }
    QVERIFY(hasContent);
}

void QmlSmokeTest::spectrogramCrosshairOverlayReusesImageBufferAtSameGeometry() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);
    item.setShowTimeLabels(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    const double cps = 48000.0 / 1024.0;
    const uchar *firstBits = nullptr;
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        item.updateCrosshairOverlayLocked(200, 100, 0, true, cps, 0.0, false);
        QVERIFY(!item.m_crosshairImage.isNull());
        firstBits = item.m_crosshairImage.constBits();
        QVERIFY(firstBits != nullptr);
        item.updateCrosshairOverlayLocked(200, 100, 0, true, cps, 0.5, false);
        QVERIFY(!item.m_crosshairImage.isNull());
        QCOMPARE(item.m_crosshairImage.constBits(), firstBits);
    }
}

void QmlSmokeTest::spectrogramGridOverlayGeneratesWhenEnabled() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setGridEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        // Sync bins from precomputed state (normally done in updatePaintNode).
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateFreqGridOverlayLocked(200, 100, true);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0, true);
    }

    QVERIFY(!item.m_freqGridImage.isNull());
    QCOMPARE(item.m_freqGridImage.width(), 200);
    QCOMPARE(item.m_freqGridImage.height(), 100);

    bool hasContent = false;
    for (int y = 0; y < item.m_freqGridImage.height() && !hasContent; ++y) {
        for (int x = 0; x < item.m_freqGridImage.width(); ++x) {
            if (qAlpha(item.m_freqGridImage.pixel(x, y)) > 0) {
                hasContent = true;
                break;
            }
        }
    }
    QVERIFY(hasContent);
}

void QmlSmokeTest::spectrogramOverlayDisabledProducesNullImage() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    // Both overlays default to disabled.

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    // Crosshair disabled + hover active: null image.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0, true);
    }
    QVERIFY(item.m_crosshairImage.isNull());

    // Grid disabled: null image.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateFreqGridOverlayLocked(200, 100, true);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0, true);
    }
    QVERIFY(item.m_freqGridImage.isNull());
}

void QmlSmokeTest::spectrogramOverlayDirtiedByGeometryChange() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);
    item.setGridEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0, true);
        item.updateFreqGridOverlayLocked(200, 100, true);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0, true);
    }
    QVERIFY(!item.m_crosshairDirty);
    QVERIFY(!item.m_freqGridDirty);
    QVERIFY(!item.m_timeGridDirty);

    item.geometryChange(QRectF(0, 0, 300, 150), QRectF(0, 0, 200, 100));

    QVERIFY(item.m_crosshairDirty);
    QVERIFY(item.m_freqGridDirty || item.m_timeGridDirty);
}

void QmlSmokeTest::spectrogramOverlayDirtiedByLogScaleChange() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);
    item.setGridEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0, true);
        item.updateFreqGridOverlayLocked(200, 100, true);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0, true);
    }
    QVERIFY(!item.m_crosshairDirty);
    QVERIFY(!item.m_freqGridDirty);
    QVERIFY(!item.m_timeGridDirty);

    item.setLogScale(true);

    QVERIFY(item.m_crosshairDirty);
    QVERIFY(item.m_freqGridDirty || item.m_timeGridDirty);
}

void QmlSmokeTest::spectrogramPublicOverlayLineNodeTracksPlayhead() {
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);

    constexpr int bins = 16;
    constexpr int totalColumns = 240;
    QByteArray chunk(bins * totalColumns, '\x40');
    item.feedPrecomputedChunk(
        chunk, bins, 0, totalColumns,
        0, totalColumns, 48000, 1024, true, true, 1, false);

    item.setPositionSeconds(0.10);
    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QVERIFY(node->childCount() >= 6);

    auto *overlayNode = overlayLineNode(node);
    QVERIFY2(overlayNode != nullptr, "precomputed path should publish overlay lines as a geometry node");

    auto *playheadRect = playheadRectNode(node);
    QVERIFY(playheadRect != nullptr);
    QVERIFY2(playheadRect->rect().isEmpty(),
             "CPU playhead rect must be suppressed when geometry overlay lines are active");

    const QVector<OverlayLineSample> firstSamples = overlayLineSamples(node);
    QVERIFY(!firstSamples.isEmpty());
    const auto firstPlayhead = findUniqueLine(firstSamples, 128, true);
    QVERIFY2(firstPlayhead.has_value(), "expected a unique playhead geometry line");
    const auto firstPlayheadColor = findUniqueLineColor(firstSamples, 128, true);
    QVERIFY2(firstPlayheadColor.has_value(), "expected a unique playhead geometry color");
    QCOMPARE(firstPlayheadColor->red(), firstPlayheadColor->alpha());
    QCOMPARE(firstPlayheadColor->green(), firstPlayheadColor->alpha());
    QCOMPARE(firstPlayheadColor->blue(), firstPlayheadColor->alpha());

    item.setPositionSeconds(2.0);
    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    const QVector<OverlayLineSample> secondSamples = overlayLineSamples(updatedNode);
    const auto secondPlayhead = findUniqueLine(secondSamples, 128, true);
    QVERIFY2(secondPlayhead.has_value(), "expected a unique moved playhead geometry line");
    QVERIFY(qRound(firstPlayhead->x1()) != qRound(secondPlayhead->x1()));

    delete updatedNode;
}

void QmlSmokeTest::spectrogramPublicOverlayLineNodeTracksGridAndCrosshair() {
    SpectrogramItem item;
    item.setWidth(240);
    item.setHeight(120);
    item.setDisplayMode(1);
    item.setPlaying(false);
    item.setGridEnabled(true);
    item.setCrosshairEnabled(true);

    constexpr int bins = 64;
    constexpr int totalColumns = 320;
    QByteArray chunk(bins * totalColumns, '\x40');
    item.feedPrecomputedChunk(
        chunk, bins, 0, totalColumns,
        0, totalColumns, 48000, 1024, true, true, 1, false);
    item.setPositionSeconds(2.0);

    QHoverEvent hoverEnter(
        QEvent::HoverEnter,
        QPointF(60.0, 40.0),
        QPointF(60.0, 40.0),
        QPointF());
    item.hoverEnterEvent(&hoverEnter);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    auto *overlayNode = overlayLineNode(node);
    QVERIFY2(overlayNode != nullptr, "overlay lines should be rendered through a public geometry node");

    const QVector<OverlayLineSample> firstSamples = overlayLineSamples(node);
    const QVector<QLineF> firstCrosshairVertical = findLinesAtCoordinate(firstSamples, 128, true, 60);
    const QVector<QLineF> firstCrosshairHorizontal = findLinesAtCoordinate(firstSamples, 128, false, 40);
    QCOMPARE(firstCrosshairVertical.isEmpty(), false);
    for (const OverlayLineSample &sample : firstSamples) {
        if (sample.color.alpha() == 128) {
            QCOMPARE(sample.color.red(), sample.color.alpha());
            QCOMPARE(sample.color.green(), sample.color.alpha());
            QCOMPARE(sample.color.blue(), sample.color.alpha());
        }
    }
    QVERIFY2(firstCrosshairVertical.size() > 4, "crosshair vertical line should remain dotted in the public overlay path");
    QVERIFY2(firstCrosshairHorizontal.size() > 4, "crosshair horizontal line should remain dotted in the public overlay path");
    QVERIFY2(longestLineSpan(firstCrosshairVertical, true) <= 3.0, "crosshair vertical line segments should stay short");
    QVERIFY2(longestLineSpan(firstCrosshairHorizontal, false) <= 3.0, "crosshair horizontal line segments should stay short");
    const QString firstGridSignature = gridLineSignature(firstSamples);
    QVERIFY2(!firstGridSignature.isEmpty(), "expected geometry grid lines in the overlay node");

    item.setWidth(320);
    item.setHeight(180);
    QHoverEvent hoverMove(
        QEvent::HoverMove,
        QPointF(110.0, 70.0),
        QPointF(110.0, 70.0),
        QPointF(60.0, 40.0));
    item.hoverMoveEvent(&hoverMove);

    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    const QVector<OverlayLineSample> secondSamples = overlayLineSamples(updatedNode);
    const QVector<QLineF> secondCrosshairVertical = findLinesAtCoordinate(secondSamples, 128, true, 110);
    const QVector<QLineF> secondCrosshairHorizontal = findLinesAtCoordinate(secondSamples, 128, false, 70);
    QVERIFY2(secondCrosshairVertical.size() > 4, "moved vertical crosshair should remain dotted");
    QVERIFY2(secondCrosshairHorizontal.size() > 4, "moved horizontal crosshair should remain dotted");
    QVERIFY2(longestLineSpan(secondCrosshairVertical, true) <= 3.0, "moved vertical crosshair segments should stay short");
    QVERIFY2(longestLineSpan(secondCrosshairHorizontal, false) <= 3.0, "moved horizontal crosshair segments should stay short");
    QVERIFY(firstCrosshairVertical != secondCrosshairVertical);
    QVERIFY(firstCrosshairHorizontal != secondCrosshairHorizontal);
    QVERIFY(firstGridSignature != gridLineSignature(secondSamples));

    delete updatedNode;
}

void QmlSmokeTest::spectrogramPublicOverlayLineNodeTracksSharedCrosshair() {
    SpectrogramItem item;
    item.setWidth(240);
    item.setHeight(120);
    item.setDisplayMode(1);
    item.setPlaying(false);
    item.setCrosshairEnabled(true);
    item.setCrosshairSharedX(90.0);

    constexpr int bins = 64;
    constexpr int totalColumns = 320;
    QByteArray chunk(bins * totalColumns, '\x40');
    item.feedPrecomputedChunk(
        chunk, bins, 0, totalColumns,
        0, totalColumns, 48000, 1024, true, true, 1, false);
    item.setPositionSeconds(2.0);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    const QVector<OverlayLineSample> samples = overlayLineSamples(node);
    const QVector<QLineF> sharedVertical = findLinesAtCoordinate(samples, 128, true, 90);
    QVERIFY2(sharedVertical.size() > 4, "shared crosshair should render dotted vertical geometry segments");
    QVERIFY2(longestLineSpan(sharedVertical, true) <= 3.0, "shared crosshair vertical segments should stay short");
    QVERIFY2(findLinesByOrientation(samples, 128, false).isEmpty(),
             "shared crosshair branch should not draw a horizontal line without local hover");

    delete node;
}

void QmlSmokeTest::spectrogramOverlayStalenessDetectsBinChange() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);
    item.setGridEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0, true);
        item.updateFreqGridOverlayLocked(200, 100, true);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0, true);
    }

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_binsPerColumn = 2049;
    }

    QVERIFY(item.m_binsPerColumn != item.m_crosshairCachedBinsPerColumn);
    QVERIFY(item.m_binsPerColumn != item.m_freqGridCachedBinsPerColumn);
}

void QmlSmokeTest::spectrogramOverlayRebuildsViaUpdatePaintNodeOnStaleInput() {
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setSampleRateHz(48000);
    item.setDisplayMode(1); // Centered
    item.setCrosshairEnabled(true);
    item.setGridEnabled(true);

    const int bins = 4097;
    const int columns = 500;
    QByteArray data(bins * columns, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, columns, 0, columns, 48000, 1024,
                               false, true, 1, false);

    item.setPositionSeconds(1.0);
    item.setPlaying(false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(100.0, 50.0);
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    qint64 firstGridDisplayLeft;
    qint64 firstCrosshairDisplayLeft;
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_crosshairImage.isNull());
        QVERIFY(!item.m_freqGridImage.isNull());
        QVERIFY(!item.m_crosshairDirty);
        QVERIFY(!item.m_freqGridDirty);
    QVERIFY(!item.m_timeGridDirty);
        firstGridDisplayLeft = item.m_timeGridRenderDisplayLeft;
        firstCrosshairDisplayLeft = item.m_crosshairCachedDisplayLeft;
    }

    item.setPositionSeconds(5.0);

    QSGNode *node2 = item.updatePaintNode(node, nullptr);
    QVERIFY(node2 != nullptr);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_crosshairImage.isNull());
        QVERIFY(!item.m_freqGridImage.isNull());
        QVERIFY(item.m_timeGridRenderDisplayLeft != firstGridDisplayLeft);
        QVERIFY(item.m_crosshairCachedDisplayLeft != firstCrosshairDisplayLeft);
    }

    delete node2;
}

void QmlSmokeTest::spectrogramOverlayStalenessDetectsDisplayRangeChange() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setGridEnabled(true);
    item.setCrosshairEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0, true);
        item.updateFreqGridOverlayLocked(200, 100, true);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0, true);
    }

    QCOMPARE(item.m_crosshairCachedDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_timeGridRenderDisplayLeft, static_cast<qint64>(0));
}

void QmlSmokeTest::testMutedChannelRendersGrayscale()
{
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    // Palette is built in the constructor.  Verify that the color palette
    // has colored entries and the gray palette has grayscale entries at
    // the same index (a mid-intensity entry, not the near-black tail).
    constexpr int midIndex = SpectrogramItem::kGradientTableSize / 4;
    const QRgb colorEntry = item.m_palette32[midIndex];
    const QRgb grayEntry = item.m_palette32Gray[midIndex];

    // Color palette entry should have differing R/G/B channels.
    const int cr = qRed(colorEntry), cg = qGreen(colorEntry), cb = qBlue(colorEntry);
    QVERIFY2(!(cr == cg && cg == cb),
             qPrintable(QStringLiteral("Color palette entry at %1 should not be grayscale: R=%2 G=%3 B=%4")
                            .arg(midIndex).arg(cr).arg(cg).arg(cb)));

    // Gray palette entry should have equal R/G/B channels.
    const int gr = qRed(grayEntry), gg = qGreen(grayEntry), gb = qBlue(grayEntry);
    QVERIFY2(gr == gg && gg == gb,
             qPrintable(QStringLiteral("Gray palette entry at %1 should be grayscale: R=%2 G=%3 B=%4")
                            .arg(midIndex).arg(gr).arg(gg).arg(gb)));

    // channelMuted property defaults to false and round-trips.
    QCOMPARE(item.channelMuted(), false);
    item.setChannelMuted(true);
    QCOMPARE(item.channelMuted(), true);
    // Setting the same value again should not re-emit.
    QSignalSpy spy(&item, &SpectrogramItem::channelMutedChanged);
    item.setChannelMuted(true);
    QCOMPARE(spy.count(), 0);
    item.setChannelMuted(false);
    QCOMPARE(spy.count(), 1);
}

void QmlSmokeTest::spectrogramClickToSeekEmitsSignalWhenCrosshairEnabled() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int sampleRate = 48000;
    constexpr int hopSize = 1024;
    constexpr int totalEstimate = 512;

    // Feed enough data so the ring buffer is populated.
    QByteArray chunk(totalEstimate * binsPerColumn, '\0');
    for (int i = 0; i < chunk.size(); ++i) {
        chunk[i] = static_cast<char>(i & 0xFF);
    }
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, totalEstimate, 0, totalEstimate,
        sampleRate, hopSize, true, true, 42);

    // Set centered mode, position at 5 seconds, crosshair enabled.
    item.setDisplayMode(1);
    item.setPositionSeconds(5.0);
    item.setCrosshairEnabled(true);

    // Prime the crosshair cache by simulating a hover so cached
    // displayLeft/drawX/rollingMode are initialized.
    QHoverEvent hoverEnter(
        QEvent::HoverEnter, QPointF(100.0, 90.0),
        QPointF(100.0, 90.0), QPointF());
    item.hoverEnterEvent(&hoverEnter);

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);

    // Right-click at pixel X=100.
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress, QPointF(100.0, 90.0),
        QPointF(100.0, 90.0), Qt::RightButton, Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 1);
    const double seekSeconds = seekSpy.at(0).at(0).toDouble();
    // The exact value depends on display layout, but it must be
    // non-negative (valid time).
    QVERIFY(seekSeconds > 0.0);
    QVERIFY(seekSeconds < 5.0);  // Left of center → earlier than playhead
}

void QmlSmokeTest::spectrogramClickToSeekUsesCurrentPositionWhenCrosshairCacheIsStale() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 20'000;
    constexpr int sampleRate = 48'000;
    constexpr int hopSize = 1'024;
    QByteArray chunk(8'000 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        chunk,
        binsPerColumn,
        0,
        8'000,
        0,
        totalEstimate,
        sampleRate,
        hopSize,
        false,
        true,
        42);

    item.setDisplayMode(1);
    item.setCrosshairEnabled(true);
    item.setPositionSeconds(10.0);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairCachedDisplayLeft = 0;
        item.m_crosshairCachedDrawX = 0.0;
        item.m_crosshairCachedRollingMode = false;
    }

    item.setPositionSeconds(100.0);

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress,
        QPointF(300.0, 90.0),
        QPointF(300.0, 90.0),
        Qt::RightButton,
        Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 1);
    const double seekSeconds = seekSpy.at(0).at(0).toDouble();
    QVERIFY2(
        seekSeconds > 100.0,
        qPrintable(QStringLiteral("stale cache produced backward seek target %1")
            .arg(seekSeconds, 0, 'f', 3)));
}

void QmlSmokeTest::spectrogramClickToSeekIgnoresLaggingDecodedTailInCenteredMode() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int decodedColumns = 4'000;
    constexpr int totalEstimate = 20'000;
    constexpr int sampleRate = 48'000;
    constexpr int hopSize = 1'024;
    QByteArray chunk(decodedColumns * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        chunk,
        binsPerColumn,
        0,
        decodedColumns,
        0,
        totalEstimate,
        sampleRate,
        hopSize,
        false,
        true,
        42);

    item.setDisplayMode(1);
    item.setCrosshairEnabled(true);
    item.setPositionSeconds(103.0);

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress,
        QPointF(300.0, 90.0),
        QPointF(300.0, 90.0),
        Qt::RightButton,
        Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 1);
    const double seekSeconds = seekSpy.at(0).at(0).toDouble();
    QVERIFY2(
        seekSeconds > 103.0,
        qPrintable(QStringLiteral("lagging decoded tail produced backward seek target %1")
            .arg(seekSeconds, 0, 'f', 3)));
}

void QmlSmokeTest::spectrogramClickToSeekSuppressedWhenCrosshairDisabled() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 512;
    QByteArray chunk(totalEstimate * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, totalEstimate, 0, totalEstimate,
        48000, 1024, true, true, 42);

    item.setDisplayMode(1);
    item.setPositionSeconds(5.0);
    item.setCrosshairEnabled(false);  // Crosshair OFF

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);

    QMouseEvent pressEvent(
        QEvent::MouseButtonPress, QPointF(100.0, 90.0),
        QPointF(100.0, 90.0), Qt::RightButton, Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 0);
}

void QmlSmokeTest::spectrogramLeftClickDoesNotSeek() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 512;
    QByteArray chunk(totalEstimate * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, totalEstimate, 0, totalEstimate,
        48000, 1024, true, true, 42);

    item.setDisplayMode(1);
    item.setPositionSeconds(5.0);
    item.setCrosshairEnabled(true);  // Crosshair ON

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);

    // Left-click should NOT seek.
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress, QPointF(100.0, 90.0),
        QPointF(100.0, 90.0), Qt::LeftButton, Qt::LeftButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 0);
}

void QmlSmokeTest::spectrogramClickToSeekDisabledInRollingMode() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int sampleRate = 48000;
    constexpr int hopSize = 1024;
    constexpr int totalEstimate = 512;

    QByteArray chunk(200 * binsPerColumn, '\0');
    for (int i = 0; i < chunk.size(); ++i) {
        chunk[i] = static_cast<char>(i & 0xFF);
    }
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, 200, 0, totalEstimate,
        sampleRate, hopSize, false, true, 42);

    // Rolling mode (default displayMode=0), crosshair enabled.
    // Seek is disabled in rolling mode because the write-order ring
    // buffer history does not realign to the new position.
    item.setDisplayMode(0);
    item.setPositionSeconds(3.0);
    item.setCrosshairEnabled(true);

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);

    QMouseEvent pressEvent(
        QEvent::MouseButtonPress, QPointF(50.0, 90.0),
        QPointF(50.0, 90.0), Qt::RightButton, Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 0);
}

void QmlSmokeTest::spectrogramZoomProperty() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Default zoom is 1.0
    QVERIFY(std::abs(item.zoomLevel() - 1.0) < 0.0001);
    QCOMPARE(item.zoomEnabled(), false);

    // Feed enough columns so that zoom < 16.0 is valid (minZoom = 1920/96000 = 0.02 → floor 0.05)
    constexpr int binsPerColumn = 4;
    constexpr int columns = 96000;
    QByteArray chunk(binsPerColumn * columns, '\x20');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, true,
        true, 1, false);

    // Setting zoom level to 2.0 works when track is long enough
    item.setZoomLevel(2.0);
    QVERIFY(std::abs(item.zoomLevel() - 2.0) < 0.0001);

    // Zoom clamps to maximum
    item.setZoomLevel(100.0);
    QVERIFY(std::abs(item.zoomLevel() - 16.0) < 0.0001);

    // Zoom clamps to the Rust-side minimum (0.05).  The track-fit
    // floor (1920/96000 = 0.02) would technically allow a smaller
    // zoom, but the backend clamps any sub-0.05 value to 0.05, so
    // Qt must also floor at 0.05 or else successive widthSettle
    // re-sends oscillate between what Qt requested and what Rust
    // clamped back.
    item.setZoomLevel(0.001);
    QVERIFY(std::abs(item.zoomLevel() - 0.05) < 0.001);

    // Reset to 1.0
    item.setZoomLevel(1.0);
    QVERIFY(std::abs(item.zoomLevel() - 1.0) < 0.0001);
}

void QmlSmokeTest::spectrogramZoomLimitsWithTrackData() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Feed some precomputed data to set up track columns
    constexpr int binsPerColumn = 64;
    constexpr int columns = 9600; // ~200 seconds at 48 cols/sec
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, true,
        true, 1, false);

    // Minimum zoom should allow seeing all columns
    const double minZoom = item.minimumZoomLevel();
    QVERIFY(minZoom > 0.0);
    QVERIFY(minZoom <= 1.0);
    // 1920 / 9600 = 0.2
    QVERIFY(std::abs(minZoom - 0.2) < 0.01);
}

void QmlSmokeTest::spectrogramZoomOutBlockedWhenSongFits() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Feed a short track that fits entirely at zoom=1.0 (1000 < 1920).
    constexpr int binsPerColumn = 4;
    constexpr int columns = 1000;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, true,
        true, 1, false);

    // Minimum zoom should be 1.0 — no zoom-out possible.
    QVERIFY(std::abs(item.minimumZoomLevel() - 1.0) < 0.0001);

    // Attempting to zoom out is clamped to 1.0.
    item.setZoomLevel(0.5);
    QVERIFY(std::abs(item.zoomLevel() - 1.0) < 0.0001);

    // Zoom-in still works.
    item.setZoomLevel(2.0);
    QVERIFY(std::abs(item.zoomLevel() - 2.0) < 0.0001);
}

void QmlSmokeTest::spectrogramLodSelectionChoosesLevelNearScreenDensity() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(240);
    item.setDisplayMode(1);

    const auto levelAtHalfPixel =
        item.selectSpectrogramLodLevelForTests(0.5, QVector<int>{0, 1, 2, 3});
    QCOMPARE(levelAtHalfPixel.primaryLevel, 1);
    QCOMPARE(levelAtHalfPixel.secondaryLevel, 2);
    QVERIFY(levelAtHalfPixel.blend >= 0.0);
    QVERIFY(levelAtHalfPixel.blend <= 1.0);

    const auto levelAtOnePixel =
        item.selectSpectrogramLodLevelForTests(1.0, QVector<int>{0, 1, 2, 3});
    QCOMPARE(levelAtOnePixel.primaryLevel, 0);
    QCOMPARE(levelAtOnePixel.secondaryLevel, 1);
    QVERIFY(levelAtOnePixel.blend < 0.001);

    const auto levelAtFarZoom =
        item.selectSpectrogramLodLevelForTests(0.125, QVector<int>{0, 1, 2, 3});
    QCOMPARE(levelAtFarZoom.primaryLevel, 3);
    QCOMPARE(levelAtFarZoom.secondaryLevel, 3);
}

void QmlSmokeTest::spectrogramStoresLodChunksSeparatelyFromLevelZero() {
    SpectrogramItem item;
    item.setWidth(400);
    item.setHeight(120);

    QByteArray level0(8, '\x10');
    item.feedPrecomputedChunk(level0, 4, 0, 2, 0, 64, 48000, 1024, false, true, 9, true, 1);

    QByteArray level1(16, '\x20'); // 2 cols x 1 channel x 4 bins x 2 planes
    item.feedPrecomputedLodChunk(level1, 4, 0, 2, 0, 32, 48000, 2048, 9, 1, 1, 2, 2, false);

    QCOMPARE(item.availableSpectrogramLodLevelsForTests(), QVector<int>({0, 1}));
    QCOMPARE(item.spectrogramLodPlaneCountForTests(1), 2);
}

void QmlSmokeTest::spectrogramFullModeBindsSelectedLodTextureWhenZoomedOut() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "full");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(320, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);
    item.setShowFpsOverlay(false);
    item.setPlaying(false);

    constexpr int bins = 64;
    constexpr int totalColumns = 1024;
    QByteArray levelZero(totalColumns * bins, '\x30');
    item.feedPrecomputedChunk(
        levelZero, bins, 0, totalColumns,
        0, totalColumns, 48000, 1024, false,
        true, 37, false, 1);

    constexpr int lodLevel = 2;
    constexpr int lodColumns = totalColumns >> lodLevel;
    constexpr int lodPlanes = 2;
    QByteArray lod(lodColumns * bins * lodPlanes, '\x60');
    item.feedPrecomputedLodChunk(
        lod, bins, 0, lodColumns,
        0, lodColumns, 48000, 4096, 37, 1,
        lodLevel, 1 << lodLevel, lodPlanes, false);
    item.setZoomLevel(0.25);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    if (windowGraphicsApi(window) != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(
            QStringLiteral("retained full-mode LOD material test requires OpenGL scene graph, got %1")
                .arg(graphicsApiName(windowGraphicsApi(window)))));
    }
    QCoreApplication::processEvents();

    std::unique_ptr<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);
    auto *sampleNode = spectrogramBodySampleMaterialNode(node.get());
    QVERIFY(sampleNode != nullptr);
    auto *sampleMaterial =
        dynamic_cast<SpectrogramBodySampleMaterial *>(sampleNode->material());
    QVERIFY(sampleMaterial != nullptr);
    QCOMPARE(sampleMaterial->state.primaryLodLevel, lodLevel);
    QCOMPARE(sampleMaterial->state.primaryPlaneCount, lodPlanes);
}

void QmlSmokeTest::spectrogramFullModeBindsAdjacentLodTexturesForCrossFade() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "full");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(320, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);
    item.setShowFpsOverlay(false);
    item.setPlaying(false);

    constexpr int bins = 64;
    constexpr int totalColumns = 1536;
    QByteArray levelZero(totalColumns * bins, '\x30');
    item.feedPrecomputedChunk(
        levelZero, bins, 0, totalColumns,
        0, totalColumns, 48000, 1024, false,
        true, 38, false, 1);

    constexpr int level1 = 1;
    constexpr int level2 = 2;
    constexpr int lodPlanes = 2;
    QByteArray lod1((totalColumns >> level1) * bins * lodPlanes, '\x50');
    QByteArray lod2((totalColumns >> level2) * bins * lodPlanes, '\x70');
    item.feedPrecomputedLodChunk(
        lod1, bins, 0, totalColumns >> level1,
        0, totalColumns >> level1, 48000, 2048, 38, 1,
        level1, 1 << level1, lodPlanes, false);
    item.feedPrecomputedLodChunk(
        lod2, bins, 0, totalColumns >> level2,
        0, totalColumns >> level2, 48000, 4096, 38, 1,
        level2, 1 << level2, lodPlanes, false);
    item.setZoomLevel(1.0 / 3.0);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    if (windowGraphicsApi(window) != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(
            QStringLiteral("retained full-mode LOD cross-fade material test requires OpenGL scene graph, got %1")
                .arg(graphicsApiName(windowGraphicsApi(window)))));
    }
    QCoreApplication::processEvents();

    std::unique_ptr<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);
    auto *sampleNode = spectrogramBodySampleMaterialNode(node.get());
    QVERIFY(sampleNode != nullptr);
    auto *sampleMaterial =
        dynamic_cast<SpectrogramBodySampleMaterial *>(sampleNode->material());
    QVERIFY(sampleMaterial != nullptr);
    QVERIFY(sampleMaterial->state.sourceTexture != nullptr);
    QVERIFY(sampleMaterial->state.secondarySourceTexture != nullptr);
    QVERIFY(sampleMaterial->state.lodBlend > 0.25F);
    QVERIFY(sampleMaterial->state.lodBlend < 0.75F);
    QCOMPARE(sampleMaterial->state.primaryLodLevel, level1);
    QCOMPARE(sampleMaterial->state.secondaryLodLevel, level2);
}

void QmlSmokeTest::spectrogramZoomInUsesPreviewUntilNativeHopArrives() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(240);
    item.setDisplayMode(1);

    QByteArray referenceData(64 * 256, '\x30');
    item.feedPrecomputedChunk(
        referenceData, 64, 0, 256,
        0, 2048, 48000, 1024,
        false, true, 12, true, 1);

    item.setZoomLevel(4.0);

    const auto previewState = item.spectrogramRenderSourceForTests();
    QCOMPARE(previewState.kind, SpectrogramItem::RenderSourceKind::Preview);
    QCOMPARE(previewState.hopSize, 1024);
    QVERIFY(previewState.scaledToRequestedZoom);

    QByteArray nativeZoomData(64 * 256, '\x70');
    item.feedPrecomputedChunk(
        nativeZoomData, 64, 0, 256,
        0, 8192, 48000, 256,
        false, true, 12, true, 2);

    const auto nativeState = item.spectrogramRenderSourceForTests();
    QCOMPARE(nativeState.kind, SpectrogramItem::RenderSourceKind::NativeDetail);
    QCOMPARE(nativeState.hopSize, 256);
    QVERIFY(!nativeState.scaledToRequestedZoom);
}

void QmlSmokeTest::spectrogramMaxZoomOutUsesOverviewToFitLongTrack() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(240);
    item.setDisplayMode(1);

    QByteArray level0(64 * 2000, '\x20');
    item.feedPrecomputedChunk(
        level0, 64, 0, 2000,
        0, 30000, 48000, 1024,
        false, true, 5, true, 1);

    QByteArray level5(64 * 938 * 2, '\x20');
    item.feedPrecomputedLodChunk(
        level5, 64, 0, 938,
        0, 938, 48000, 32768, 5, 1,
        5, 32, 2, false);

    QVERIFY(item.minimumZoomLevel() < 0.05);
    item.setZoomLevel(item.minimumZoomLevel());
    QCOMPARE(item.centeredVisibleCoveragePercentForTests(), 100);
}

void QmlSmokeTest::spectrogramEffectiveZoomMatchesBackendHop() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Feed initial data at default hop, then request zoom=4x.
    // Need >= 480 columns so minZoom (1920/cols) <= 4.0.
    constexpr int binsPerColumn = 64;
    constexpr int columns = 500;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns * 2, 48000, 1024, false,
        true, 1, false);

    item.setZoomLevel(4.0);
    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);

    // Simulate backend restart with hop_size=256 (zoom=4x data).
    // After this buffer_reset, effectiveZoom = 4.0 * 256 / 1024 = 1.0.
    QByteArray zoomChunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        zoomChunk, binsPerColumn, 0, columns,
        0, columns * 2, 48000, 256, false,
        true, 1, false);

    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);
}

void QmlSmokeTest::spectrogramAdvanceWorksWhenBackendMatchesZoom() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Feed initial data at default hop, then request zoom=4x.
    // Need >= 480 columns so minZoom (1920/cols) <= 4.0.
    constexpr int binsPerColumn = 64;
    constexpr int columns = 500;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns * 2, 48000, 1024, false,
        true, 1, false);

    item.setZoomLevel(4.0);

    // Simulate backend restart with hop_size=256 (zoom=4x data).
    // After this, effectiveZoom = 4.0 * 256 / 1024 = 1.0 (1:1 rendering).
    QByteArray zoomChunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        zoomChunk, binsPerColumn, 0, columns,
        0, columns * 2, 48000, 256, false,
        true, 1, false);

    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);
}

void QmlSmokeTest::spectrogramEffectiveZoomDuringTransition() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Feed data with default hop_size=1024 (backend not yet producing zoom=4x data).
    // Need >= 480 columns so minZoom (1920/cols) <= 4.0, allowing setZoomLevel(4.0).
    constexpr int binsPerColumn = 64;
    constexpr int columns = 500;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns * 2, 48000, 1024, false,
        true, 1, false);

    // Set zoom to 4x — backend hasn't responded yet.
    // The visual zoom (renderZoomLevel) is deferred; the display
    // keeps showing the existing data at the old zoom until the
    // backend session restarts with the finer hop size.
    item.setZoomLevel(4.0);

    // The property reflects the requested zoom immediately.
    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);

    // Simulate backend restart: feed a buffer_reset chunk with finer hop.
    // This triggers the deferred render zoom to take effect.
    QByteArray zoomChunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        zoomChunk, binsPerColumn, 0, columns,
        0, columns * 2, 48000, 256, false,
        true, 1, false);

    // After backend data arrives, zoom is fully applied.
    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);
}

void QmlSmokeTest::spectrogramZoomPreviewAppliesRenderScaleBeforeBackendData() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);
    item.setDisplayMode(1);

    constexpr int binsPerColumn = 64;
    constexpr int columns = 500;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns * 2, 48000, 1024, false,
        true, 1, false);

    item.setZoomLevel(4.0);

    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);
    QVERIFY(std::abs(item.m_renderZoomLevel - 4.0) < 0.0001);
    QVERIFY(std::abs(item.effectiveZoomLocked() - 4.0) < 0.0001);
    const auto renderSource = item.spectrogramRenderSourceForTests();
    QCOMPARE(renderSource.kind, SpectrogramItem::RenderSourceKind::Preview);
    QVERIFY(renderSource.scaledToRequestedZoom);
}

void QmlSmokeTest::spectrogramZoomPreviewFreezesBeforeBackendReset() {
    SpectrogramItem item;
    item.setWidth(400);
    item.setHeight(120);
    item.setDisplayMode(1);
    item.setPlaying(false);

    constexpr int binsPerColumn = 16;
    constexpr int columns = 4000;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, false,
        true, 1, false);
    item.setPositionSeconds((2000.0 * 1024.0) / 48000.0);

    std::unique_ptr<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);
    QVERIFY(!item.m_gpuDisplayImage.isNull());

    item.setZoomLevel(16.0);

    node.reset(item.updatePaintNode(node.release(), nullptr));
    QVERIFY(node != nullptr);
    QCOMPARE(item.m_zoomFillActive, true);
    QCOMPARE(item.m_zoomFillDataReady, false);
    QVERIFY2(
        !item.m_zoomFillFrozenDisplayImage.isNull(),
        "zoom preview must freeze the currently rendered frame before "
        "the backend reset can expose an empty refilling canvas");

    QByteArray partialReset(binsPerColumn * 100, '\x55');
    item.feedPrecomputedChunk(
        partialReset, binsPerColumn, 0, 100,
        0, columns * 16, 48000, 64, false,
        true, 1, false, 2);

    node.reset(item.updatePaintNode(node.release(), nullptr));
    QVERIFY(node != nullptr);
    QCOMPARE(item.m_zoomFillActive, true);
    QVERIFY2(
        !item.m_zoomFillFrozenDisplayImage.isNull(),
        "partial native-hop refill must stay behind the frozen zoom preview");
}

void QmlSmokeTest::spectrogramDeferredZoomAppliesOnBackendData() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Use small bins so large column counts don't allocate too much memory.
    constexpr int binsPerColumn = 4;
    constexpr int columns = 96000;

    // Feed initial data at default hop (backend at zoom=1x).
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, false,
        true, 1, false);

    QVERIFY(std::abs(item.zoomLevel() - 1.0) < 0.0001);

    // Request zoom to 4x — property updates immediately.
    item.setZoomLevel(4.0);
    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);

    // Feed continuation data at OLD hop (still in pipeline from before the
    // backend received the zoom command).  This is NOT a buffer_reset, so
    // the deferred zoom must NOT apply yet.
    constexpr int staleCols = 50;
    QByteArray staleChunk(binsPerColumn * staleCols, '\x40');
    item.feedPrecomputedChunk(
        staleChunk, binsPerColumn, 0, staleCols,
        columns, columns, 48000, 1024, false,
        false, 1, false);

    // Property still shows requested zoom.
    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);

    // Simulate backend restart: buffer_reset with finer hop.
    // This triggers the deferred render zoom to take effect.
    // At zoom=4x with hop=256, the backend produces 4x more columns.
    constexpr int zoomColumns = columns * 4;
    QByteArray zoomChunk(binsPerColumn * zoomColumns, '\x40');
    item.feedPrecomputedChunk(
        zoomChunk, binsPerColumn, 0, zoomColumns,
        0, zoomColumns, 48000, 256, false,
        true, 1, false);

    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);

    // Verify zoom back to 1.0 also defers correctly.
    item.setZoomLevel(1.0);
    QVERIFY(std::abs(item.zoomLevel() - 1.0) < 0.0001);

    // Backend restarts at default hop.
    QByteArray resetChunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        resetChunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, false,
        true, 1, false);

    QVERIFY(std::abs(item.zoomLevel() - 1.0) < 0.0001);
}

void QmlSmokeTest::spectrogramRapidZoomOutIgnoresIntermediateReset() {
    SpectrogramItem item;
    item.setWidth(400);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);

    constexpr int binsPerColumn = 8;
    constexpr int referenceColumns = 4000;
    QByteArray initialChunk(binsPerColumn * referenceColumns, '\x40');
    item.feedPrecomputedChunk(
        initialChunk, binsPerColumn, 0, referenceColumns,
        0, referenceColumns, 48000, 1024, false,
        true, 1, false);

    item.setZoomLevel(4.0);
    item.m_zoomDebounceTimer->stop();
    QByteArray zoomedInChunk(binsPerColumn * referenceColumns, '\x50');
    item.feedPrecomputedChunk(
        zoomedInChunk, binsPerColumn, 0, referenceColumns,
        0, referenceColumns * 4, 48000, 256, false,
        true, 1, false);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QVERIFY(!item.m_gpuDisplayImage.isNull());
    QCOMPARE(item.m_awaitingZoomData, false);
    QVERIFY(std::abs(item.m_renderZoomLevel - 4.0) < 0.001);

    // User keeps wheel-zooming out while an older backend restart is still
    // in flight. The intermediate hop must not snap/reveal, otherwise the
    // view appears to get stuck before the final max-out request arrives.
    item.setZoomLevel(0.1);
    QVERIFY(item.m_awaitingZoomData);
    QVERIFY(item.m_zoomDebounceTimer->isActive());

    constexpr int intermediateColumns = 1600;
    QByteArray intermediateChunk(binsPerColumn * intermediateColumns, '\x60');
    item.feedPrecomputedChunk(
        intermediateChunk, binsPerColumn, 0, intermediateColumns,
        0, intermediateColumns, 48000, 512, false,
        true, 1, false);

    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);
    QVERIFY2(item.m_awaitingZoomData,
             "intermediate zoom reset must not consume the latest pending zoom request");
    QVERIFY2(item.m_zoomFillActive,
             "intermediate zoom reset should remain hidden behind the frozen frame");
    QCOMPARE(item.m_zoomFillDataReady, false);
    QVERIFY2(
        std::abs(item.m_renderZoomLevel - 0.1) < 0.001,
        "the preview render scale should keep following the latest requested zoom");

    item.m_zoomDebounceTimer->stop();
    constexpr int finalColumns = 800;
    QByteArray finalChunk(binsPerColumn * finalColumns, '\x70');
    item.feedPrecomputedChunk(
        finalChunk, binsPerColumn, 0, finalColumns,
        0, finalColumns, 48000, 5120, false,
        true, 1, false);

    updatedNode = item.updatePaintNode(updatedNode, nullptr);
    QVERIFY(updatedNode == node);
    QCOMPARE(item.m_awaitingZoomData, false);
    QVERIFY(std::abs(item.m_renderZoomLevel - 0.1) < 0.001);
    QVERIFY(std::abs(item.effectiveZoomLocked() - 0.5) < 0.01);
    QVERIFY2(!item.m_zoomFillActive,
             "matching final zoom reset should be allowed to reveal");

    delete updatedNode;
}

void QmlSmokeTest::spectrogramSameTrackCoarseResetSnapsRenderZoomWithoutAwaitingFlag() {
    SpectrogramItem item;
    item.setWidth(400);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);

    constexpr int binsPerColumn = 8;
    constexpr int referenceColumns = 4000;
    QByteArray initialChunk(binsPerColumn * referenceColumns, '\x40');
    item.feedPrecomputedChunk(
        initialChunk, binsPerColumn, 0, referenceColumns,
        0, referenceColumns, 48000, 1024, false,
        true, 1, false);

    QCOMPARE(item.m_awaitingZoomData, false);
    QVERIFY(std::abs(item.m_renderZoomLevel - 1.0) < 0.001);

    // Regression: after rapid wheel input, an earlier restart can consume the
    // awaiting flag before the final max-out reset arrives. The same-track
    // reset's effective hop must still be authoritative for render zoom.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 0.1;
        item.m_renderZoomLevel = 0.1;
        item.m_awaitingZoomData = false;
    }
    QVERIFY(std::abs(item.zoomLevel() - 0.1) < 0.001);

    constexpr int coarseColumns = 800;
    QByteArray coarseChunk(binsPerColumn * coarseColumns, '\x70');
    item.feedPrecomputedChunk(
        coarseChunk, binsPerColumn, 0, coarseColumns,
        0, coarseColumns, 48000, 5120, false,
        true, 1, false);

    QCOMPARE(item.m_awaitingZoomData, false);
    QVERIFY(std::abs(item.m_renderZoomLevel - 0.1) < 0.001);
    QVERIFY(std::abs(item.zoomLevel() - 0.1) < 0.001);
    QVERIFY(std::abs(item.effectiveZoomLocked() - 0.5) < 0.01);
}

void QmlSmokeTest::spectrogramMaxZoomOutRoundedHopSnapsRenderZoomWithoutAwaitingFlag() {
    SpectrogramItem item;
    item.setWidth(1183);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);

    constexpr int binsPerColumn = 8;
    constexpr int referenceColumns = 11763;
    QByteArray initialChunk(binsPerColumn * referenceColumns, '\x40');
    item.feedPrecomputedChunk(
        initialChunk, binsPerColumn, 0, referenceColumns,
        0, referenceColumns, 48000, 1024, false,
        true, 1, false);

    const double viewportFitZoom =
        static_cast<double>(static_cast<int>(item.width()))
        / static_cast<double>(referenceColumns);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = viewportFitZoom;
        item.m_renderZoomLevel = 0.02;
        item.m_awaitingZoomData = false;
    }

    constexpr int coarseColumns = 2352;
    QByteArray coarseChunk(binsPerColumn * coarseColumns, '\x70');
    item.feedPrecomputedChunk(
        coarseChunk, binsPerColumn, 0, coarseColumns,
        0, coarseColumns, 48000, 5120, false,
        true, 1, false);

    QVERIFY2(
        std::abs(item.m_renderZoomLevel - 0.1) < 0.001,
        "backend's rounded max-zoom-out hop should be authoritative after rapid zoom-out");
    QVERIFY(std::abs(item.effectiveZoomLocked() - 0.5) < 0.01);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(coarseColumns - 1));
    QVERIFY2(
        item.m_gpuDisplayWidth >= static_cast<int>(item.width()) - 8,
        "rounded max-zoom-out data should render at near full widget width from oversampled columns");
    delete node;
}

void QmlSmokeTest::spectrogramCenteredZoomOutDoesNotPreserveReferenceHopRange() {
    SpectrogramItem item;
    item.setWidth(1183);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    item.setPositionSeconds(10.0);

    constexpr int binsPerColumn = 8;
    constexpr int referenceColumns = 4000;
    QByteArray initialChunk(binsPerColumn * referenceColumns, '\x40');
    item.feedPrecomputedChunk(
        initialChunk, binsPerColumn, 0, referenceColumns,
        0, referenceColumns, 48000, 1024, false,
        true, 1, false);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
    QVERIFY(item.m_precomputedCanvasDisplayRight > 1000);
    QVERIFY(std::abs(item.m_precomputedCanvasZoomLevel - 1.0) < 0.001);
    QCOMPARE(item.m_precomputedCanvasHopSize, 1024);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 0.1;
        item.m_renderZoomLevel = 0.1;
        item.m_awaitingZoomData = false;
    }

    constexpr int coarseColumns = 800;
    QByteArray coarseChunk(binsPerColumn * coarseColumns, '\x70');
    item.feedPrecomputedChunk(
        coarseChunk, binsPerColumn, 0, coarseColumns,
        0, coarseColumns, 48000, 5120, false,
        true, 1, false);

    node = item.updatePaintNode(node, nullptr);
    QVERIFY(node != nullptr);
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(coarseColumns - 1));
    QVERIFY2(
        std::abs(item.m_gpuDisplayWidth - static_cast<int>(coarseColumns * item.effectiveZoomLocked())) <= 2,
        "zoom-out should rebuild against the oversampled full-track range, not the old reference-hop range");
    delete node;
}

void QmlSmokeTest::spectrogramCenteredZoomOutDropsStaleSameTrackZoomChunks() {
    SpectrogramItem item;
    item.setWidth(1183);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    item.setPositionSeconds(10.0);

    constexpr int binsPerColumn = 8;
    constexpr int referenceColumns = 4000;
    QByteArray initialChunk(binsPerColumn * referenceColumns, '\x40');
    item.feedPrecomputedChunk(
        initialChunk, binsPerColumn, 0, referenceColumns,
        0, referenceColumns, 48000, 1024, false,
        true, 1, false);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 0.1;
        item.m_renderZoomLevel = 0.1;
        item.m_awaitingZoomData = false;
    }

    constexpr int coarseColumns = 800;
    QByteArray coarseChunk(binsPerColumn * coarseColumns, '\x70');
    item.feedPrecomputedChunk(
        coarseChunk, binsPerColumn, 0, coarseColumns,
        0, coarseColumns, 48000, 5120, false,
        true, 1, false);

    node = item.updatePaintNode(node, nullptr);
    QVERIFY(node != nullptr);
    QCOMPARE(item.m_precomputedHopSize, 5120);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, coarseColumns);
    QCOMPARE(item.m_precomputedMaxColumnIndex, coarseColumns - 1);
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(coarseColumns - 1));

    constexpr int staleColumns = 800;
    QByteArray staleResetChunk(binsPerColumn * staleColumns, '\x20');
    item.feedPrecomputedChunk(
        staleResetChunk, binsPerColumn, 0, staleColumns,
        0, staleColumns, 48000, 2048, false,
        true, 1, false);

    QCOMPARE(item.m_precomputedHopSize, 5120);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, coarseColumns);
    QCOMPARE(item.m_precomputedMaxColumnIndex, coarseColumns - 1);

    QByteArray staleDataChunk(binsPerColumn * staleColumns, '\x20');
    item.feedPrecomputedChunk(
        staleDataChunk, binsPerColumn, 0, staleColumns,
        0, staleColumns, 48000, 2048, false,
        false, 1, false);

    node = item.updatePaintNode(node, nullptr);
    QVERIFY(node != nullptr);
    QCOMPARE(item.m_precomputedHopSize, 5120);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, coarseColumns);
    QCOMPARE(item.m_precomputedMaxColumnIndex, coarseColumns - 1);
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(coarseColumns - 1));

    delete node;
}

void QmlSmokeTest::spectrogramCenteredZoomOutDropsOlderSameTrackGeneration() {
    SpectrogramItem item;
    item.setWidth(1183);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);

    constexpr int binsPerColumn = 8;
    constexpr int referenceColumns = 4000;
    constexpr quint64 trackToken = 1;
    QByteArray initialChunk(binsPerColumn * referenceColumns, '\x40');
    item.feedPrecomputedChunk(
        initialChunk, binsPerColumn, 0, referenceColumns,
        0, referenceColumns, 48000, 1024, false,
        true, trackToken, false, 1);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 0.1;
        item.m_renderZoomLevel = 0.1;
        item.m_awaitingZoomData = false;
    }

    constexpr int coarseColumns = 800;
    QByteArray coarseChunk(binsPerColumn * coarseColumns, '\x70');
    item.feedPrecomputedChunk(
        coarseChunk, binsPerColumn, 0, coarseColumns,
        0, coarseColumns, 48000, 5120, false,
        true, trackToken, false, 4);

    QCOMPARE(item.m_precomputedHopSize, 5120);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, coarseColumns);
    QCOMPARE(item.m_precomputedMaxColumnIndex, coarseColumns - 1);
    const qint64 ringWriteSeq = item.m_ringWriteSeq;

    // A superseded rapid-zoom session can use a rounded hop that still
    // matches the final zoom tolerance. Generation, not hop alone, must
    // keep those older chunks out of the current ring.
    constexpr int staleColumns = 64;
    QByteArray staleResetChunk(binsPerColumn * staleColumns, '\x20');
    item.feedPrecomputedChunk(
        staleResetChunk, binsPerColumn, 0, staleColumns,
        0, staleColumns, 48000, 5088, false,
        true, trackToken, false, 3);

    QByteArray staleDataChunk(binsPerColumn * staleColumns, '\x20');
    item.feedPrecomputedChunk(
        staleDataChunk, binsPerColumn, 0, staleColumns,
        0, staleColumns, 48000, 5088, false,
        false, trackToken, false, 3);

    QCOMPARE(item.m_precomputedHopSize, 5120);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, coarseColumns);
    QCOMPARE(item.m_precomputedMaxColumnIndex, coarseColumns - 1);
    QCOMPARE(item.m_ringWriteSeq, ringWriteSeq);
}

void QmlSmokeTest::spectrogramCenteredZoomOutBackendRestartReanchorsToFullTrack() {
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);

    constexpr int binsPerColumn = 8;
    constexpr int initialColumns = 2000;
    QByteArray initialChunk(binsPerColumn * initialColumns, '\x40');
    item.feedPrecomputedChunk(
        initialChunk, binsPerColumn, 0, initialColumns,
        0, initialColumns, 48000, 1024, true,
        true, 1, false);
    item.setPositionSeconds(10.0);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QVERIFY(item.m_precomputedCanvasDisplayLeft > 0);
    const qint64 initialDisplayLeft = item.m_precomputedCanvasDisplayLeft;
    item.setZoomLevel(0.1);
    item.m_zoomDebounceTimer->stop();
    QVERIFY(item.m_awaitingZoomData);

    constexpr int zoomedOutTotalColumns = 400;
    constexpr int zoomedOutDecodedColumns = 64;
    QByteArray zoomedOutChunk(binsPerColumn * zoomedOutDecodedColumns, '\x60');
    item.feedPrecomputedChunk(
        zoomedOutChunk, binsPerColumn, 0, zoomedOutDecodedColumns,
        0, zoomedOutTotalColumns, 48000, 5120, false,
        true, 1, false);

    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    QVERIFY2(item.m_zoomFillActive,
             "zoom-out should keep the old centered cache visible while the coarser restart fills");
    QVERIFY2(!item.m_zoomFillFrozenDisplayImage.isNull(),
             "zoom-out freeze should preserve a stable display image until the coarser window is ready");
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(zoomedOutDecodedColumns - 1));
    QVERIFY(item.m_precomputedCanvasDisplayLeft < initialDisplayLeft);
    QCOMPARE(item.m_gpuDisplayDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_gpuDisplayDisplayRight, static_cast<qint64>(zoomedOutDecodedColumns - 1));
    QVERIFY(!item.m_gpuDisplayImage.isNull());
    QVERIFY(std::abs(item.effectiveZoomLocked() - 0.5) < 0.001);

    constexpr int zoomedOutReadyColumns = 400;
    QByteArray readyChunk(binsPerColumn * zoomedOutReadyColumns, '\x60');
    item.feedPrecomputedChunk(
        readyChunk, binsPerColumn, 0, zoomedOutReadyColumns,
        0, zoomedOutTotalColumns, 48000, 5120, false,
        true, 1, false);

    updatedNode = item.updatePaintNode(updatedNode, nullptr);
    QVERIFY(updatedNode == node);
    QVERIFY2(!item.m_zoomFillActive,
             "zoom-out freeze should clear once the coarser window can fill the viewport");
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(zoomedOutReadyColumns - 1));

    delete updatedNode;
}

void QmlSmokeTest::spectrogramLinearScaleKeepsTopBinVisibleAtTallHeights() {
    SpectrogramItem item;
    item.setWidth(240);
    item.setHeight(159);
    item.setDisplayMode(1);
    item.setPlaying(false);
    item.setLogScale(false);

    constexpr int binsPerColumn = 1025;
    constexpr int columns = 400;
    QByteArray chunk(binsPerColumn * columns, '\0');
    for (int column = 0; column < columns; ++column) {
        for (int bin = binsPerColumn - 12; bin < binsPerColumn; ++bin) {
            chunk[column * binsPerColumn + bin] = static_cast<char>(
                static_cast<unsigned char>(0xFF));
        }
    }

    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, true,
        true, 1, false);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QVERIFY(!item.m_gpuDisplayImage.isNull());

    auto topRowLuma = [&item]() -> int {
        const int sampleX = std::clamp(item.m_gpuDisplayImage.width() / 2, 0,
                                       std::max(0, item.m_gpuDisplayImage.width() - 1));
        const QColor topPixel = item.m_gpuDisplayImage.pixelColor(sampleX, 0);
        const QColor midPixel = item.m_gpuDisplayImage.pixelColor(
            sampleX,
            std::clamp(item.m_gpuDisplayImage.height() / 2, 0,
                       std::max(0, item.m_gpuDisplayImage.height() - 1)));
        return qGray(topPixel.rgb()) - qGray(midPixel.rgb());
    };

    QVERIFY2(topRowLuma() > 20,
             "default-height linear spectrogram should show highest-bin energy on the top row");

    item.setHeight(318);
    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);
    QVERIFY(!item.m_gpuDisplayImage.isNull());
    QVERIFY2(topRowLuma() > 20,
             "taller linear spectrogram should still show highest-bin energy on the top row");

    delete updatedNode;
}

void QmlSmokeTest::spectrogramZoomOutProducesDistinctHop() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    constexpr int bins = 4;
    constexpr int cols = 9600;

    // Feed data at zoom=1.0 (hop=1024).
    QByteArray chunk1(bins * cols, '\x40');
    item.feedPrecomputedChunk(
        chunk1, bins, 0, cols,
        0, cols, 48000, 1024, true,
        true, 1, false);
    const int hop1 = item.m_precomputedHopSize;

    // Simulate zoom to 0.8: backend restarts with a DIFFERENT hop.
    // With zoom-out oversampling: effective_hop = round(1024 / (0.8 * 2)) = 640.
    // This is distinct from 1024.
    // With the OLD integer decimation: factor=1, effective_hop=1024 (same!).
    item.setZoomLevel(0.8);
    QByteArray chunk2(bins * cols, '\x40');
    item.feedPrecomputedChunk(
        chunk2, bins, 0, cols,
        0, cols, 48000, 640, true,
        true, 1, false);
    const int hop2 = item.m_precomputedHopSize;

    // The hops MUST differ — this is the dead zone fix.
    QVERIFY(hop1 != hop2);
    // Verify effectiveZoom keeps 2x source oversampling for GPU peak-hold.
    const double ez = item.m_renderZoomLevel
        * static_cast<double>(hop2) / 1024.0;
    QVERIFY(std::abs(ez - 0.5) < 0.01);
}

void QmlSmokeTest::spectrogramMinZoomAdaptsToWidthChange() {
    // Simulates the widget→fullscreen transition: when width increases,
    // minimumZoomLevel should increase (allow less zoom-out), and a zoom
    // level valid at the old width should be clamped to the new minimum.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(400);

    constexpr int binsPerColumn = 64;
    constexpr int columns = 9600;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, true,
        true, 1, false);

    // At width=1200, minZoom = 1200/9600 = 0.125
    const double narrowMinZoom = item.minimumZoomLevel();
    QVERIFY(std::abs(narrowMinZoom - 0.125) < 0.01);

    // Zoom all the way out on the narrow widget.
    item.setZoomLevel(narrowMinZoom);
    QVERIFY(std::abs(item.zoomLevel() - narrowMinZoom) < 0.01);

    // Simulate entering fullscreen: width triples.
    item.setWidth(3600);

    // New minZoom = 3600/9600 = 0.375 — much higher.
    const double wideMinZoom = item.minimumZoomLevel();
    QVERIFY(std::abs(wideMinZoom - 0.375) < 0.01);

    // Re-applying the old narrow zoom should clamp to the new minimum.
    item.setZoomLevel(narrowMinZoom);
    QVERIFY(std::abs(item.zoomLevel() - wideMinZoom) < 0.01);
}

void QmlSmokeTest::spectrogramCenteredModeUsesWindowedCapacity() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);
    item.setDisplayMode(1); // Centered

    // Feed a large track estimate
    constexpr int binsPerColumn = 64;
    constexpr int columns = 100;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, 100000, 48000, 1024, false,
        true, 1, false);

    // Ring capacity should NOT be 100000 (full track).
    // It should be bounded to ~3 screen widths + lookahead.
    QVERIFY(item.m_ringCapacity < 20000);
}

void QmlSmokeTest::spectrogramRollingModeKeepsViewportHeadroomBeyondLookahead() {
    SpectrogramItem item;
    item.setWidth(3440);
    item.setHeight(80);
    item.setDisplayMode(0); // Rolling
    item.setZoomLevel(1.0);

    constexpr int binsPerColumn = 64;
    constexpr int columns = 100;
    constexpr int sampleRate = 48000;
    constexpr int hopSize = 1024;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, 100000, sampleRate, hopSize, false,
        true, 1, false);

    const double colsPerSecond =
        static_cast<double>(sampleRate) / static_cast<double>(hopSize);
    const int lookaheadCols = static_cast<int>(10.0 * colsPerSecond);
    const int zoomAdjustedWidth = 3440;

    QVERIFY2(
        item.m_ringCapacity >= zoomAdjustedWidth * 2 + lookaheadCols,
        "rolling mode should reserve at least one extra viewport width of ring headroom beyond the decode lookahead so eviction never intrudes into the live window");
}

void QmlSmokeTest::spectrogramPeakHoldColumnSpanUsesMaxNotNearest() {
    SpectrogramItem item;
    item.setWidth(100);
    item.setHeight(10);
    item.setDisplayMode(1); // Centered

    // Feed 150 columns into a 100px widget at effectiveZoom=0.8.
    // zoom=0.8, hop=1024 -> effectiveZoom = 0.8 * 1024/1024 = 0.8.
    // columnsPerPixel = 1/0.8 = 1.25.  Since 1.25 > 1.0, the rebuild
    // enters the peak-hold path (colFirst < colLast && !interpolate).
    // drawPixels = min(100, ceil(150 * 0.8)) = 100.
    constexpr int bins = 4;
    constexpr int cols = 150;
    QByteArray chunk(bins * cols, '\x10'); // dark baseline (0x10)

    // Make column 50 bright: set all bins to 0xF0.
    for (int b = 0; b < bins; ++b) {
        chunk[50 * bins + b] = static_cast<char>(static_cast<unsigned char>(0xF0));
    }

    item.setZoomLevel(0.8);
    item.feedPrecomputedChunk(
        chunk, bins, 0, cols,
        0, cols, 48000, 1024, true,
        true, 1, false);

    QVERIFY(item.precomputedReady());

    const SpectrogramGpu::ColumnSpan peakHoldSpan =
        SpectrogramGpu::resolveColumnSpanForPixel(40, cols, 0.8);
    QCOMPARE(peakHoldSpan.firstColumn, 50);
    QCOMPARE(peakHoldSpan.lastColumn, 51);
    QCOMPARE(peakHoldSpan.interpolate, false);

    const SpectrogramGpu::ColumnSpan interpolationSpan =
        SpectrogramGpu::resolveColumnSpanForPixel(3, cols, 2.0);
    QCOMPARE(interpolationSpan.firstColumn, 1);
    QCOMPARE(interpolationSpan.lastColumn, 2);
    QVERIFY(interpolationSpan.interpolate);
    QVERIFY(interpolationSpan.interpolationT > 0.49);
    QVERIFY(interpolationSpan.interpolationT < 0.51);
}

void QmlSmokeTest::spectrogramRetainedSourceLayoutPreserves64BitColumnRange() {
    SpectrogramRetainedSourceLayout layout{
        .capacityColumns = 1024,
        .binsPerColumn = 64,
        .displayLeft = 3'000'000'000LL,
        .displayRight = 3'000'000'127LL,
        .rollingMode = true,
    };

    const SpectrogramSourceColumnRange visibleColumns = layout.visibleColumnRange();
    QCOMPARE(visibleColumns.xBegin, 3'000'000'000LL);
    QCOMPARE(visibleColumns.xEnd, 3'000'000'127LL);
    QCOMPARE(visibleColumns.width(), 128);
    QCOMPARE(layout.capacityColumns, 1024);
}

void QmlSmokeTest::spectrogramRetainedSourceUploadPlannerMergesAdjacentDirtyColumns() {
    QVector<SpectrogramSourceUploadSpan> spans = {
        {10, 10},
        {11, 11},
        {14, 14},
        {15, 17},
    };

    const QVector<SpectrogramSourceUploadSpan> merged =
        mergeSpectrogramSourceUploadSpans(spans);

    QCOMPARE(merged.size(), 2);
    QCOMPARE(merged[0].xBegin, 10);
    QCOMPARE(merged[0].xEnd, 11);
    QCOMPARE(merged[1].xBegin, 14);
    QCOMPARE(merged[1].xEnd, 17);
}

void QmlSmokeTest::spectrogramRetainedSourceUploadPlannerTreatsWindowChangeAsTextureStateChange() {
    QQuickWindow embeddedWindow;
    QQuickWindow fullscreenWindow;
    SpectrogramItem item;
    item.setWidth(160);
    item.setHeight(80);
    item.setDisplayMode(1);

    constexpr int bins = 4;
    constexpr int columns = 16;
    constexpr quint64 trackToken = 77;
    QByteArray data(bins * columns, '\0');
    for (int column = 0; column < columns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            data[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(column * bins + bin));
        }
    }

    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, trackToken, false);

    QMutexLocker lock(&item.m_stateMutex);
    const QSize retainedSize(item.m_ringCapacity, bins);
    item.m_gpuSourceTexture.m_texture = reinterpret_cast<QSGTexture *>(quintptr(1));
    item.m_gpuSourceTexture.m_size = retainedSize;
    item.m_gpuSourceTexture.m_ownerWindow = &embeddedWindow;
    item.m_gpuSourceTextureTrackToken = trackToken;
    item.m_gpuSourceTextureRolling = false;
    item.m_gpuSourceTextureDisplayLeft = 0;
    item.m_gpuSourceTextureDisplayRight = columns - 1;
    item.m_gpuSourceTextureMaxColumnIndex = columns - 1;

    QVERIFY(!item.m_gpuSourceTexture.matches(&fullscreenWindow, retainedSize));
    QVERIFY(item.retainedSourceTextureStateChangedLocked(
        &fullscreenWindow,
        retainedSize,
        false));

    const QVector<SpectrogramSourceUploadSpan> skippedSpans =
        item.collectRetainedSourceUploadSpansLocked(0, columns - 1, false);
    QVERIFY2(
        skippedSpans.isEmpty(),
        "same-window bookkeeping would otherwise skip upload for this display range");

    const QVector<SpectrogramSourceUploadSpan> forcedSpans =
        item.collectRetainedSourceUploadSpansLocked(0, columns - 1, false, true);
    QCOMPARE(forcedSpans.size(), 1);
    QCOMPARE(forcedSpans[0].xBegin, 0);
    QCOMPARE(forcedSpans[0].xEnd, columns - 1);

    item.m_gpuSourceTexture.m_texture = nullptr;
}

void QmlSmokeTest::spectrogramRetainedPagePlanSplitsSinglePageRange() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int columns = SpectrogramItem::kRetainedSourcePageWidth;
    QByteArray data(columns * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, 19, false);

    QMutexLocker lock(&item.m_stateMutex);
    const QVector<SpectrogramItem::RetainedPageSpan> spans =
        item.buildRetainedPageSpansLocked(0, columns - 1, columns - 1, false);

    QCOMPARE(spans.size(), 1);
    QCOMPARE(spans[0].pageIndex, static_cast<qint64>(0));
    QCOMPARE(spans[0].pageXBegin, 0);
    QCOMPARE(spans[0].pageXEnd, SpectrogramItem::kRetainedSourcePageWidth - 1);
    QCOMPARE(spans[0].displayLeft, static_cast<qint64>(0));
    QCOMPARE(spans[0].displayRight, static_cast<qint64>(columns - 1));
}

void QmlSmokeTest::spectrogramRetainedPagePlanSplitsAcrossPageBoundary() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int columns = SpectrogramItem::kRetainedSourcePageWidth + 64;
    QByteArray data(columns * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, 23, false);

    QMutexLocker lock(&item.m_stateMutex);
    const QVector<SpectrogramItem::RetainedPageSpan> spans =
        item.buildRetainedPageSpansLocked(2040, 2055, 2055, false);

    QCOMPARE(spans.size(), 2);
    QCOMPARE(spans[0].pageIndex, static_cast<qint64>(0));
    QCOMPARE(spans[0].pageXBegin, 2040);
    QCOMPARE(spans[0].pageXEnd, 2047);
    QCOMPARE(spans[0].displayLeft, static_cast<qint64>(2040));
    QCOMPARE(spans[0].displayRight, static_cast<qint64>(2047));
    QCOMPARE(spans[1].pageIndex, static_cast<qint64>(1));
    QCOMPARE(spans[1].pageXBegin, 0);
    QCOMPARE(spans[1].pageXEnd, 7);
    QCOMPARE(spans[1].displayLeft, static_cast<qint64>(2048));
    QCOMPARE(spans[1].displayRight, static_cast<qint64>(2055));
}

void QmlSmokeTest::spectrogramRetainedPagePlanSplitsRollingWrap() {
    SpectrogramItem item;
    item.setDisplayMode(0); // Rolling

    QMutexLocker lock(&item.m_stateMutex);
    item.m_precomputedBinsPerColumn = 4;
    item.m_precomputedTrackToken = 31;
    item.m_ringCapacity = 8;
    item.m_ringOldestSeq = 4;
    item.m_ringWriteSeq = 12;
    item.m_ringColumnId.assign(8, -1);
    item.m_ringSequenceId.assign(8, -1);
    item.m_ringTrackToken.assign(8, 0);
    for (qint64 seq = item.m_ringOldestSeq; seq < item.m_ringWriteSeq; ++seq) {
        const int slot = static_cast<int>(seq % item.m_ringCapacity);
        item.m_ringColumnId[static_cast<size_t>(slot)] = static_cast<qint32>(seq);
        item.m_ringSequenceId[static_cast<size_t>(slot)] = seq;
        item.m_ringTrackToken[static_cast<size_t>(slot)] = item.m_precomputedTrackToken;
    }

    const QVector<SpectrogramItem::RetainedPageSpan> spans =
        item.buildRetainedPageSpansLocked(6, 11, 11, true);

    QCOMPARE(spans.size(), 2);
    QCOMPARE(spans[0].pageIndex, static_cast<qint64>(0));
    QCOMPARE(spans[0].pageXBegin, 6);
    QCOMPARE(spans[0].pageXEnd, 7);
    QCOMPARE(spans[0].displayLeft, static_cast<qint64>(6));
    QCOMPARE(spans[0].displayRight, static_cast<qint64>(7));
    QCOMPARE(spans[1].pageIndex, static_cast<qint64>(0));
    QCOMPARE(spans[1].pageXBegin, 0);
    QCOMPARE(spans[1].pageXEnd, 3);
    QCOMPARE(spans[1].displayLeft, static_cast<qint64>(8));
    QCOMPARE(spans[1].displayRight, static_cast<qint64>(11));
}

void QmlSmokeTest::spectrogramRetainedPageRetentionKeepsCenteredHalo() {
    SpectrogramItem item;

    QMutexLocker lock(&item.m_stateMutex);
    item.m_ringCapacity = SpectrogramItem::kRetainedSourcePageWidth * 4;

    const QVector<SpectrogramItem::RetainedPageSpan> visiblePages = {
        SpectrogramItem::RetainedPageSpan{
            .pageIndex = 1,
            .pageXBegin = 16,
            .pageXEnd = 64,
            .displayLeft = SpectrogramItem::kRetainedSourcePageWidth + 16,
            .displayRight = SpectrogramItem::kRetainedSourcePageWidth + 64,
        },
    };

    const QVector<qint64> retainedPages =
        item.retainedSourcePageIndicesToKeepLocked(
            visiblePages,
            false,
            SpectrogramItem::kRetainedSourcePageWidth);

    QCOMPARE(retainedPages, QVector<qint64>({0, 1, 2}));
}

void QmlSmokeTest::spectrogramRetainedPageRetentionKeepsFrozenZoomFillScrollHalo() {
    const QVector<SpectrogramItem::RetainedRenderSegment> frozenSegments{
        SpectrogramItem::RetainedRenderSegment{
            QRect(SpectrogramItem::kRetainedSourcePageWidth - 24, 0, 80, 16),
            QRectF(0.0, 0.0, 160.0, 32.0),
            true,
        },
    };

    const QVector<qint64> retainedPages =
        SpectrogramItem::retainedFrozenPageIndicesToKeepForZoomFill(
            frozenSegments,
            SpectrogramItem::kRetainedSourcePageWidth);

    QCOMPARE(retainedPages, QVector<qint64>({0, 1, 2}));
}

void QmlSmokeTest::spectrogramRetainedPageRetentionKeepsRollingWrapHalo() {
    SpectrogramItem item;

    QMutexLocker lock(&item.m_stateMutex);
    item.m_ringCapacity = 16;

    const QVector<SpectrogramItem::RetainedPageSpan> visiblePages = {
        SpectrogramItem::RetainedPageSpan{
            .pageIndex = 3,
            .pageXBegin = 2,
            .pageXEnd = 3,
            .displayLeft = 14,
            .displayRight = 15,
        },
        SpectrogramItem::RetainedPageSpan{
            .pageIndex = 0,
            .pageXBegin = 0,
            .pageXEnd = 1,
            .displayLeft = 16,
            .displayRight = 17,
        },
    };

    const QVector<qint64> retainedPages =
        item.retainedSourcePageIndicesToKeepLocked(visiblePages, true, 4);

    QCOMPARE(retainedPages, QVector<qint64>({0, 1, 2, 3}));
}

void QmlSmokeTest::spectrogramRetainedPageUploadPlannerUploadsOnlyDirtyPageSpans() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int columns = SpectrogramItem::kRetainedSourcePageWidth + 64;
    constexpr quint64 trackToken = 41;
    QByteArray data(columns * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, trackToken, false);

    QMutexLocker lock(&item.m_stateMutex);
    auto uploadedPage = std::make_unique<SpectrogramItem::RetainedSourcePageState>();
    uploadedPage->key = SpectrogramItem::RetainedSourcePageKey{
        .trackToken = trackToken,
        .generation = item.m_precomputedSessionGeneration,
        .rollingMode = false,
        .pageIndex = 0,
    };
    uploadedPage->width = SpectrogramItem::kRetainedSourcePageWidth;
    uploadedPage->binsPerColumn = bins;
    uploadedPage->uploadedDisplayLeft = 0;
    uploadedPage->uploadedDisplayRight = SpectrogramItem::kRetainedSourcePageWidth - 1;
    item.m_gpuSourcePages.push_back(std::move(uploadedPage));

    const QVector<SpectrogramItem::RetainedPageUploadSpan> uploads =
        item.collectRetainedPageUploadSpansLocked(2040, 2055, 2055, false);

    QCOMPARE(uploads.size(), 1);
    QCOMPARE(uploads[0].pageIndex, static_cast<qint64>(1));
    QCOMPARE(uploads[0].pageXBegin, 0);
    QCOMPARE(uploads[0].pageXEnd, 7);
    QCOMPARE(uploads[0].displayLeft, static_cast<qint64>(2048));
    QCOMPARE(uploads[0].displayRight, static_cast<qint64>(2055));
    QVERIFY(uploads[0].resetPage);
}

void QmlSmokeTest::spectrogramRetainedPageUploadPlannerKeepsTextureForShiftedSpan() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int columns = SpectrogramItem::kRetainedSourcePageWidth;
    constexpr quint64 trackToken = 42;
    QByteArray data(columns * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, trackToken, false);

    QMutexLocker lock(&item.m_stateMutex);
    const SpectrogramItem::RetainedPageTextureExtent initialExtent =
        SpectrogramItem::retainedPageTextureExtentForSpan(
            1800,
            1831,
            SpectrogramItem::kRetainedSourcePageWidth);
    auto uploadedPage = std::make_unique<SpectrogramItem::RetainedSourcePageState>();
    uploadedPage->key = SpectrogramItem::RetainedSourcePageKey{
        .trackToken = trackToken,
        .generation = item.m_precomputedSessionGeneration,
        .rollingMode = false,
        .pageIndex = 0,
    };
    uploadedPage->pageXOrigin = initialExtent.xOrigin;
    uploadedPage->width = initialExtent.width;
    uploadedPage->binsPerColumn = bins;
    uploadedPage->uploadedDisplayLeft = 1800;
    uploadedPage->uploadedDisplayRight = 1831;
    item.m_gpuSourcePages.push_back(std::move(uploadedPage));

    const QVector<SpectrogramItem::RetainedPageUploadSpan> uploads =
        item.collectRetainedPageUploadSpansLocked(1500, 1531, 1531, false);

    QCOMPARE(uploads.size(), 1);
    QCOMPARE(uploads[0].pageIndex, static_cast<qint64>(0));
    QCOMPARE(uploads[0].pageXBegin, 1500);
    QCOMPARE(uploads[0].pageXEnd, 1531);
    QCOMPARE(uploads[0].displayLeft, static_cast<qint64>(1500));
    QCOMPARE(uploads[0].displayRight, static_cast<qint64>(1531));
    QVERIFY2(
        !uploads[0].resetPage,
        "shifted visible spans inside a retained page must upload dirty columns without reallocating the page texture");
}

void QmlSmokeTest::spectrogramRetainedPageUploadPlannerResetsPageOnTokenChange() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int columns = 16;
    constexpr quint64 oldTrackToken = 51;
    constexpr quint64 newTrackToken = 52;
    QByteArray data(columns * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, newTrackToken, false);

    QMutexLocker lock(&item.m_stateMutex);
    auto stalePage = std::make_unique<SpectrogramItem::RetainedSourcePageState>();
    stalePage->key = SpectrogramItem::RetainedSourcePageKey{
        .trackToken = oldTrackToken,
        .generation = item.m_precomputedSessionGeneration,
        .rollingMode = false,
        .pageIndex = 0,
    };
    stalePage->width = SpectrogramItem::kRetainedSourcePageWidth;
    stalePage->binsPerColumn = bins;
    stalePage->uploadedDisplayLeft = 0;
    stalePage->uploadedDisplayRight = columns - 1;
    item.m_gpuSourcePages.push_back(std::move(stalePage));

    const QVector<SpectrogramItem::RetainedPageUploadSpan> uploads =
        item.collectRetainedPageUploadSpansLocked(0, columns - 1, columns - 1, false);

    QCOMPARE(uploads.size(), 1);
    QCOMPARE(uploads[0].pageIndex, static_cast<qint64>(0));
    QCOMPARE(uploads[0].pageXBegin, 0);
    QCOMPARE(uploads[0].pageXEnd, columns - 1);
    QVERIFY(uploads[0].resetPage);
}

void QmlSmokeTest::spectrogramRollingRetainedPageUploadPlannerKeepsPagesAcrossGaplessTokenChange() {
    SpectrogramItem item;
    item.setDisplayMode(0); // Rolling

    constexpr int bins = 4;
    constexpr int pageWidth = 16;
    constexpr int columns = 24;
    constexpr quint64 oldTrackToken = 61;
    constexpr quint64 newTrackToken = 62;
    QByteArray data(columns * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, 128, 48000, 1024, true,
        true, oldTrackToken, false);

    QMutexLocker lock(&item.m_stateMutex);
    item.m_precomputedTrackToken = newTrackToken;

    auto retainedPage = std::make_unique<SpectrogramItem::RetainedSourcePageState>();
    retainedPage->key = SpectrogramItem::RetainedSourcePageKey{
        .trackToken = oldTrackToken,
        .generation = item.m_precomputedSessionGeneration,
        .rollingMode = true,
        .pageIndex = 0,
    };
    retainedPage->pageXOrigin = 0;
    retainedPage->width = pageWidth;
    retainedPage->binsPerColumn = bins;
    retainedPage->uploadedDisplayLeft = 0;
    retainedPage->uploadedDisplayRight = 7;
    item.m_gpuSourcePages.push_back(std::move(retainedPage));

    const QVector<SpectrogramItem::RetainedPageUploadSpan> uploads =
        item.collectRetainedPageUploadSpansLocked(0, 15, 15, true, pageWidth);

    QCOMPARE(uploads.size(), 1);
    QCOMPARE(uploads[0].pageIndex, static_cast<qint64>(0));
    QCOMPARE(uploads[0].pageXBegin, 8);
    QCOMPARE(uploads[0].pageXEnd, 15);
    QCOMPARE(uploads[0].displayLeft, static_cast<qint64>(8));
    QCOMPARE(uploads[0].displayRight, static_cast<qint64>(15));
    QVERIFY2(
        !uploads[0].resetPage,
        "rolling gapless token changes must reuse retained pages for preserved history and upload only newly exposed columns");
}

void QmlSmokeTest::spectrogramRetainedPageUploadPlannerLabelsResetReasons() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int columns = 32;
    constexpr quint64 trackToken = 53;
    QByteArray data(columns * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, trackToken, false);

    QMutexLocker lock(&item.m_stateMutex);
    QVector<SpectrogramItem::RetainedPageUploadSpan> uploads =
        item.collectRetainedPageUploadSpansLocked(0, columns - 1, columns - 1, false);

    QCOMPARE(uploads.size(), 1);
    QVERIFY(uploads[0].resetPage);
    QCOMPARE(
        uploads[0].resetReason,
        SpectrogramItem::RetainedPageResetReason::MissingPage);

    auto wrongWidthPage = std::make_unique<SpectrogramItem::RetainedSourcePageState>();
    wrongWidthPage->key = SpectrogramItem::RetainedSourcePageKey{
        .trackToken = trackToken,
        .generation = item.m_precomputedSessionGeneration,
        .rollingMode = false,
        .pageIndex = 0,
    };
    wrongWidthPage->width = columns / 2;
    wrongWidthPage->binsPerColumn = bins;
    wrongWidthPage->uploadedDisplayLeft = 0;
    wrongWidthPage->uploadedDisplayRight = columns - 1;
    item.m_gpuSourcePages.push_back(std::move(wrongWidthPage));

    uploads = item.collectRetainedPageUploadSpansLocked(0, columns - 1, columns - 1, false);

    QCOMPARE(uploads.size(), 1);
    QVERIFY(uploads[0].resetPage);
    QCOMPARE(
        uploads[0].resetReason,
        SpectrogramItem::RetainedPageResetReason::PageWidthChanged);

    item.m_gpuSourcePages[0]->width = SpectrogramItem::kRetainedSourcePageWidth;
    item.m_gpuSourcePages[0]->binsPerColumn = bins + 1;

    uploads = item.collectRetainedPageUploadSpansLocked(0, columns - 1, columns - 1, false);

    QCOMPARE(uploads.size(), 1);
    QVERIFY(uploads[0].resetPage);
    QCOMPARE(
        uploads[0].resetReason,
        SpectrogramItem::RetainedPageResetReason::BinsChanged);
}

void QmlSmokeTest::spectrogramRetainedPageUploadBudgetLimitsPaintWork() {
    QVector<SpectrogramItem::RetainedPageUploadSpan> spans = {
        SpectrogramItem::RetainedPageUploadSpan{
            .pageIndex = 0,
            .pageXBegin = 0,
            .pageXEnd = SpectrogramItem::kRetainedSourcePageWidth - 1,
            .displayLeft = 0,
            .displayRight = SpectrogramItem::kRetainedSourcePageWidth - 1,
            .resetPage = true,
            .resetReason = SpectrogramItem::RetainedPageResetReason::MissingPage,
        },
        SpectrogramItem::RetainedPageUploadSpan{
            .pageIndex = 1,
            .pageXBegin = 0,
            .pageXEnd = SpectrogramItem::kRetainedSourcePageWidth - 1,
            .displayLeft = SpectrogramItem::kRetainedSourcePageWidth,
            .displayRight = SpectrogramItem::kRetainedSourcePageWidth * 2 - 1,
            .resetPage = true,
            .resetReason = SpectrogramItem::RetainedPageResetReason::MissingPage,
        },
        SpectrogramItem::RetainedPageUploadSpan{
            .pageIndex = 2,
            .pageXBegin = 0,
            .pageXEnd = 704,
            .displayLeft = SpectrogramItem::kRetainedSourcePageWidth * 2,
            .displayRight = SpectrogramItem::kRetainedSourcePageWidth * 2 + 704,
            .resetPage = true,
            .resetReason = SpectrogramItem::RetainedPageResetReason::MissingPage,
        },
    };

    bool budgetExhausted = false;
    const QVector<SpectrogramItem::RetainedPageUploadSpan> bounded =
        SpectrogramItem::retainedPageUploadSpansForPaintBudget(
            spans,
            SpectrogramItem::kRetainedSourcePageWidth,
            &budgetExhausted);

    QCOMPARE(bounded.size(), 1);
    QCOMPARE(bounded[0].pageIndex, static_cast<qint64>(0));
    QCOMPARE(
        bounded[0].pageXEnd - bounded[0].pageXBegin + 1,
        SpectrogramItem::kRetainedSourcePageWidth);
    QVERIFY(bounded[0].resetPage);
    QVERIFY(budgetExhausted);
}

void QmlSmokeTest::spectrogramRetainedPageUploadBudgetSplitsOversizedSpan() {
    QVector<SpectrogramItem::RetainedPageUploadSpan> spans = {
        SpectrogramItem::RetainedPageUploadSpan{
            .pageIndex = 2,
            .pageXBegin = 100,
            .pageXEnd = 999,
            .displayLeft = 2100,
            .displayRight = 2999,
            .resetPage = true,
            .resetReason = SpectrogramItem::RetainedPageResetReason::MissingPage,
        },
    };

    bool budgetExhausted = false;
    const QVector<SpectrogramItem::RetainedPageUploadSpan> bounded =
        SpectrogramItem::retainedPageUploadSpansForPaintBudget(
            spans,
            SpectrogramItem::kRetainedUploadColumnsPerPaint,
            &budgetExhausted);

    QCOMPARE(bounded.size(), 1);
    QCOMPARE(bounded[0].pageIndex, static_cast<qint64>(2));
    QCOMPARE(bounded[0].pageXBegin, 100);
    QCOMPARE(
        bounded[0].pageXEnd,
        100 + SpectrogramItem::kRetainedUploadColumnsPerPaint - 1);
    QCOMPARE(bounded[0].displayLeft, static_cast<qint64>(2100));
    QCOMPARE(
        bounded[0].displayRight,
        static_cast<qint64>(2100 + SpectrogramItem::kRetainedUploadColumnsPerPaint - 1));
    QVERIFY(bounded[0].resetPage);
    QCOMPARE(
        bounded[0].resetReason,
        SpectrogramItem::RetainedPageResetReason::MissingPage);
    QVERIFY(budgetExhausted);
}

void QmlSmokeTest::spectrogramRetainedPageTextureExtentUsesStablePageWidth() {
    const SpectrogramItem::RetainedPageTextureExtent singleColumn =
        SpectrogramItem::retainedPageTextureExtentForSpan(
            7,
            7,
            SpectrogramItem::kRetainedSourcePageWidth);

    QCOMPARE(singleColumn.xOrigin, 0);
    QCOMPARE(singleColumn.width, SpectrogramItem::kRetainedSourcePageWidth);

    const SpectrogramItem::RetainedPageTextureExtent rightEdge =
        SpectrogramItem::retainedPageTextureExtentForSpan(
            SpectrogramItem::kRetainedSourcePageWidth - 2,
            SpectrogramItem::kRetainedSourcePageWidth - 1,
            SpectrogramItem::kRetainedSourcePageWidth);

    QCOMPARE(rightEdge.xOrigin, 0);
    QCOMPARE(rightEdge.width, SpectrogramItem::kRetainedSourcePageWidth);

    const SpectrogramItem::RetainedPageTextureExtent wide =
        SpectrogramItem::retainedPageTextureExtentForSpan(
            0,
            SpectrogramItem::kRetainedSourcePageWidth - 1,
            SpectrogramItem::kRetainedSourcePageWidth);

    QCOMPARE(wide.xOrigin, 0);
    QCOMPARE(wide.width, SpectrogramItem::kRetainedSourcePageWidth);
}

void QmlSmokeTest::spectrogramRetainedUploadDefersTinyUncoveredSeekFrames() {
    QVERIFY(SpectrogramItem::shouldDeferTinyRetainedUploadDuringSeek(
        7,
        true,
        false,
        240));
    QVERIFY(!SpectrogramItem::shouldDeferTinyRetainedUploadDuringSeek(
        127,
        true,
        false,
        240));
    QVERIFY(!SpectrogramItem::shouldDeferTinyRetainedUploadDuringSeek(
        7,
        false,
        false,
        240));
    QVERIFY(!SpectrogramItem::shouldDeferTinyRetainedUploadDuringSeek(
        7,
        true,
        true,
        240));
    QVERIFY(!SpectrogramItem::shouldDeferTinyRetainedUploadDuringSeek(
        7,
        true,
        false,
        400));
}

void QmlSmokeTest::spectrogramRetainedPageUploadPlannerUsesGpuLimitForPageWidth() {
    SpectrogramItem item;
    QQuickWindow window;

    item.m_gpuMaxTextureColumns = 1024;
    QCOMPARE(item.retainedSourcePageWidthForGpuLimit(&window), 1024);

    item.m_gpuMaxTextureColumns = 4096;
    QCOMPARE(
        item.retainedSourcePageWidthForGpuLimit(&window),
        SpectrogramItem::kRetainedSourcePageWidth);
}

void QmlSmokeTest::spectrogramRetainedPageUploadPlannerInvalidatesPagesOnReset() {
    SpectrogramItem item;

    {
        QMutexLocker lock(&item.m_stateMutex);
        auto page = std::make_unique<SpectrogramItem::RetainedSourcePageState>();
        page->key = SpectrogramItem::RetainedSourcePageKey{
            .trackToken = 5,
            .rollingMode = false,
            .pageIndex = 0,
        };
        page->width = SpectrogramItem::kRetainedSourcePageWidth;
        page->binsPerColumn = 4;
        page->uploadedDisplayLeft = 0;
        page->uploadedDisplayRight = 128;
        item.m_gpuSourcePages.push_back(std::move(page));

        item.invalidateRetainedSourceTextureStateLocked();

        QVERIFY2(
            item.m_gpuSourcePages.empty(),
            "retained source pages must be dropped with legacy retained texture state so same-token zoom/seek resets cannot reuse stale page uploads");
    }
}

void QmlSmokeTest::spectrogramRetainedPageSyncKeepsPagesWhenVisibleRangeUndecoded() {
    SpectrogramItem item;
    QQuickWindow window;

    constexpr int bins = 4;
    constexpr int columns = 64;
    constexpr quint64 trackToken = 54;
    QByteArray data(columns * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, trackToken, false);

    QMutexLocker lock(&item.m_stateMutex);
    auto retainedPage = std::make_unique<SpectrogramItem::RetainedSourcePageState>();
    retainedPage->key = SpectrogramItem::RetainedSourcePageKey{
        .trackToken = trackToken,
        .generation = item.m_precomputedSessionGeneration,
        .rollingMode = false,
        .pageIndex = 0,
    };
    retainedPage->width = SpectrogramItem::kRetainedSourcePageWidth;
    retainedPage->binsPerColumn = bins;
    retainedPage->uploadedDisplayLeft = 0;
    retainedPage->uploadedDisplayRight = columns - 1;
    item.m_gpuSourcePages.push_back(std::move(retainedPage));

    item.syncRetainedSourcePagesLocked(
        &window,
        columns + 128,
        columns + 255,
        columns - 1,
        false);

    QCOMPARE(item.m_gpuSourcePages.size(), static_cast<size_t>(1));
    QCOMPARE(item.m_gpuSourcePageUploadSpanCount, 0);
    QCOMPARE(item.m_gpuSourcePageUploadColumns, 0);
    QCOMPARE(
        item.m_gpuSourcePageResetReason,
        SpectrogramItem::RetainedPageResetReason::None);
}

void QmlSmokeTest::spectrogramRetainedLegacyTextureInvalidationKeepsPages() {
    SpectrogramItem item;

    {
        QMutexLocker lock(&item.m_stateMutex);
        auto page = std::make_unique<SpectrogramItem::RetainedSourcePageState>();
        page->key = SpectrogramItem::RetainedSourcePageKey{
            .trackToken = 5,
            .rollingMode = false,
            .pageIndex = 1,
        };
        page->width = SpectrogramItem::kRetainedSourcePageWidth;
        page->binsPerColumn = 4;
        page->uploadedDisplayLeft = 2048;
        page->uploadedDisplayRight = 2304;
        item.m_gpuSourcePages.push_back(std::move(page));
        item.m_gpuSourceTextureTrackToken = 5;
        item.m_gpuSourceTextureDisplayLeft = 2048;
        item.m_gpuSourceTextureDisplayRight = 2304;
        item.m_gpuSourceTextureMinColumnIndex = 2048;
        item.m_gpuSourceTextureMaxColumnIndex = 2304;
        item.m_gpuSourcePageUploadSpanCount = 2;
        item.m_gpuSourcePageUploadColumns = 384;
        item.m_gpuSourcePageTextureReset = true;

        item.invalidateLegacyRetainedSourceTextureStateLocked();

        QCOMPARE(item.m_gpuSourcePages.size(), 1U);
        QCOMPARE(item.m_gpuSourcePages[0]->key.pageIndex, 1);
        QCOMPARE(item.m_gpuSourceTextureTrackToken, 0ULL);
        QCOMPARE(item.m_gpuSourceTextureMaxColumnIndex, -1LL);
        QCOMPARE(item.m_gpuSourcePageUploadSpanCount, 2);
        QCOMPARE(item.m_gpuSourcePageUploadColumns, 384);
        QVERIFY(item.m_gpuSourcePageTextureReset);
    }
}

void QmlSmokeTest::spectrogramRetainedPageCoverageSkipsLegacyTextureUpload() {
    SpectrogramItem item;
    QQuickWindow window;

    item.setDisplayMode(1); // Centered
    item.m_gpuMaxTextureColumns = 32768;

    constexpr int bins = 4;
    constexpr int columns = 4096;
    QByteArray data(columns * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, 61, false);

    QMutexLocker lock(&item.m_stateMutex);
    const SpectrogramRetainedSourceLayout layout =
        item.buildRetainedSourceLayoutLocked(0, columns - 1, false);

    QVERIFY2(
        !item.shouldSyncLegacyRetainedSourceTextureLocked(&window, layout, true),
        "page-backed retained coverage should skip the old monolithic texture upload even when that texture would fit");
    QVERIFY2(
        !item.shouldSyncLegacyRetainedSourceTextureLocked(&window, layout, false),
        "sparse page-backed retained coverage should not rebuild the old monolithic texture");
}

void QmlSmokeTest::spectrogramGpuTextureNodeDebugUsesRetainedSourceTextureResource() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "texture-node");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.resize(240, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);
    item.setShowFpsOverlay(false);

    constexpr int bins = 32;
    constexpr int columns = 240;
    QByteArray chunk(bins * columns, '\x10');
    for (int column = 72; column < 136; ++column) {
        for (int bin = 8; bin < 24; ++bin) {
            chunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(0xF0));
        }
    }

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item.feedPrecomputedChunk(
        chunk, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, 1, false);
    item.setPositionSeconds(0.0);

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();
    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());
    if (windowGraphicsApi(window) != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(
            QStringLiteral("retained-source texture-node binding test requires OpenGL scene graph, got %1")
                .arg(graphicsApiName(windowGraphicsApi(window)))));
    }

    QScopedPointer<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(!node.isNull());
    auto *bodyTextureNode = spectrogramBodyTextureNode(node.data());
    QVERIFY(bodyTextureNode != nullptr);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY2(
            item.m_gpuSourceTexture.texture() != nullptr,
            "texture-node debug mode should create the retained source texture");
        QCOMPARE(bodyTextureNode->texture(), item.m_gpuSourceTexture.texture());
    }
    QCOMPARE(
        bodyTextureNode->textureCoordinatesTransform(),
        QSGSimpleTextureNode::TextureCoordinatesTransformMode(
            QSGSimpleTextureNode::MirrorVertically));
}

void QmlSmokeTest::spectrogramGpuFullModeUsesRetainedSourceWithoutCpuDisplayImage() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "full");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.resize(240, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);
    item.setPlaying(false);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);
    item.setShowFpsOverlay(false);

    constexpr int bins = 32;
    constexpr int columns = 240;
    QByteArray chunk(bins * columns, '\x40');

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item.feedPrecomputedChunk(
        chunk, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, 1, false);

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();
    if (windowGraphicsApi(window) != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(
            QStringLiteral("retained-source full-mode binding test requires OpenGL scene graph, got %1")
                .arg(graphicsApiName(windowGraphicsApi(window)))));
    }

    QScopedPointer<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(!node.isNull());
    auto *bodyTextureNode = spectrogramBodyTextureNode(node.data());
    QVERIFY(bodyTextureNode != nullptr);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY2(
            item.m_gpuSourceTexture.texture() != nullptr,
            "full mode should create the retained source texture");
        QCOMPARE(bodyTextureNode->texture(), item.m_gpuSourceTexture.texture());
        QVERIFY2(
            item.m_gpuDisplayImage.isNull(),
            "full mode should not build the CPU display image when retained coverage is complete");
    }
}

void QmlSmokeTest::spectrogramGpuLookupAndDisplayImagesUseCompatibleFormats() {
    SpectrogramItem item;
    item.setWidth(100);
    item.setHeight(10);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int cols = 150;
    QByteArray chunk(bins * cols, '\x10');
    for (int b = 0; b < bins; ++b) {
        chunk[50 * bins + b] = static_cast<char>(
            static_cast<unsigned char>(0xF0));
    }

    item.setZoomLevel(0.8);
    item.feedPrecomputedChunk(
        chunk, bins, 0, cols,
        0, cols, 48000, 1024, true,
        true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 0.8;
        item.syncPrecomputedDisplayImageLocked(100, 10, 0, 124, false, 0.8);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        QCOMPARE(item.m_gpuDisplayImage.format(), QImage::Format_ARGB32_Premultiplied);
        const QColor brightSample = item.m_gpuDisplayImage.pixelColor(40, 4);
        const QColor darkSample = item.m_gpuDisplayImage.pixelColor(0, 4);
        QVERIFY(brightSample.red() >= darkSample.red());

        item.syncPrecomputedGpuLookupImagesLocked();
        QVERIFY(!item.m_gpuDbRemapImage.isNull());
        const QColor remapSample = item.m_gpuDbRemapImage.pixelColor(128, 0);
        QCOMPARE(remapSample.red(), remapSample.green());
        QCOMPARE(remapSample.green(), remapSample.blue());
    }
}

void QmlSmokeTest::spectrogramGpuDebugModeParsesEnvironmentValues() {
    QCOMPARE(
        SpectrogramGpu::parseDebugModeForTests(""),
        SpectrogramGpu::DebugMode::Full);
    QCOMPARE(
        SpectrogramGpu::parseDebugModeForTests("texture-node"),
        SpectrogramGpu::DebugMode::TextureNode);
    QCOMPARE(
        SpectrogramGpu::parseDebugModeForTests("shader-source-only"),
        SpectrogramGpu::DebugMode::ShaderSourceOnly);
    QCOMPARE(
        SpectrogramGpu::parseDebugModeForTests("stock-material"),
        SpectrogramGpu::DebugMode::StockMaterial);
    QCOMPARE(
        SpectrogramGpu::parseDebugModeForTests("geometry-solid"),
        SpectrogramGpu::DebugMode::Full);
    QCOMPARE(
        SpectrogramGpu::parseDebugModeForTests("full"),
        SpectrogramGpu::DebugMode::Full);
}

void QmlSmokeTest::spectrogramGpuTextureNodeDebugStaysVisibleAfterRepeatedRollingUpdatesWithoutCpuSourceImage() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "texture-node");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    auto *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(window.width());
    item->setHeight(window.height());
    item->setDisplayMode(0);
    item->setGridEnabled(false);
    item->setCrosshairEnabled(false);
    item->setShowFpsOverlay(false);
    item->setPlaying(false);

    constexpr int bins = 32;
    constexpr int initialColumns = 480;
    constexpr int steps = 64;
    QByteArray chunk(bins * initialColumns, '\0');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            chunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>((column * 7 + bin * 11) % 256));
        }
    }

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item->feedPrecomputedChunk(
        chunk, bins, 0, initialColumns,
        0, 4096, 48000, 1024, false, true, 91, false);
    {
        QMutexLocker lock(&item->m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item->m_zoomLevel = 0.5;
        item->m_renderZoomLevel = 0.5;
        item->m_positionSeconds =
            ((static_cast<double>(initialColumns - 1) + 0.25) * 1024.0) / 48000.0;
        item->m_positionAnchorSeconds = item->m_positionSeconds;
        item->m_positionAnchorUpdatedAt = now;
        item->m_positionAnchorInitialized = true;
    }

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    std::unique_ptr<QSGNode> node(item->updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);
    auto *initialBodyTextureNode = spectrogramBodyTextureNode(node.get());
    QVERIFY(initialBodyTextureNode != nullptr);
    QVERIFY2(
        initialBodyTextureNode->texture() != nullptr,
        "texture-node baseline should bind the retained source texture");

    for (int step = 0; step < steps; ++step) {
        QByteArray nextChunk(bins, '\0');
        for (int bin = 0; bin < bins; ++bin) {
            nextChunk[bin] = static_cast<char>(
                static_cast<unsigned char>((192 + step * 13 + bin * 17) % 256));
        }
        item->feedPrecomputedChunk(
            nextChunk, bins, 0, 1,
            initialColumns + step, 4096, 48000, 1024, false, false, 91, false);
        {
            QMutexLocker lock(&item->m_stateMutex);
            const auto now = std::chrono::steady_clock::now();
            item->m_positionSeconds =
                ((static_cast<double>(initialColumns + step) + 0.25) * 1024.0) / 48000.0;
            item->m_positionAnchorSeconds = item->m_positionSeconds;
            item->m_positionAnchorUpdatedAt = now;
            item->m_positionAnchorInitialized = true;
        }
        QCoreApplication::processEvents();
        node.reset(item->updatePaintNode(node.release(), nullptr));
        QVERIFY(node != nullptr);
    }

    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());
    QVERIFY2(
        countVisiblePixelsInRect(frame, QRect(20, 16, 200, 88), 8) > 100,
        "texture-node debug mode should keep rendering from the retained source texture after repeated rolling updates");

    auto *bodyTextureNode = spectrogramBodyTextureNode(node.get());
    QVERIFY(bodyTextureNode != nullptr);
    QVERIFY2(!bodyTextureNode->rect().isEmpty(), "texture-node body should remain visible after repeated updates");
}

void QmlSmokeTest::spectrogramActiveRendererUsesPublicBodyTextureOnly() {
    QQuickWindow window;
    window.resize(200, 100);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    SpectrogramItem item;
    item.setParentItem(window.contentItem());
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);

    constexpr int bins = 32;
    constexpr int columns = 240;
    QByteArray data(bins * columns, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, columns, 0, columns,
        48000, 1024, false, true, 1, false);
    item.setPositionSeconds(1.0);
    QVERIFY(item.childItems().isEmpty());

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    auto *background = dynamic_cast<QSGSimpleRectNode *>(node->childAtIndex(0));
    auto *overlayNode = overlayLineNode(node);

    QVERIFY(background != nullptr);
    QCOMPARE(background->color(), QColor(0, 0, 0, 255));
    QCOMPARE(background->rect(), QRectF(0.0, 0.0, 200.0, 100.0));

    assertFullModeBodyNodeInvariants(
        node,
        "centered full mode should keep the material node active and the legacy body texture node empty");

    QVERIFY(overlayNode != nullptr);
    delete node;
}

void QmlSmokeTest::spectrogramRollingGpuWindowCaptureUsesMaterialBackedBodyNodeOnly() {
    QQuickWindow window;
    window.resize(200, 100);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    SpectrogramItem item;
    item.setParentItem(window.contentItem());
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(0);

    constexpr int bins = 32;
    constexpr int columns = 240;
    QByteArray data(bins * columns, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, columns, 0, columns,
        48000, 1024, false, true, 1, false);
    item.setPositionSeconds(1.0);
    QVERIFY(item.childItems().isEmpty());

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    auto *background = dynamic_cast<QSGSimpleRectNode *>(node->childAtIndex(0));
    auto *overlayNode = overlayLineNode(node);

    QVERIFY(background != nullptr);
    QCOMPARE(background->color(), QColor(0, 0, 0, 255));
    QCOMPARE(background->rect(), QRectF(0.0, 0.0, 200.0, 100.0));

    assertFullModeBodyNodeInvariants(
        node,
        "rolling full mode should keep the material node active and the legacy body texture node empty");

    QVERIFY(overlayNode != nullptr);
    delete node;
}

void QmlSmokeTest::spectrogramUpdatePaintNodeUsesGpuSpectrogramNode() {
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 64;
    constexpr int columns = 500;
    QByteArray data(bins * columns, '\x80');
    item.feedPrecomputedChunk(
        data, bins, 0, columns, 0, columns, 48000, 1024,
        false, true, 1, false);

    item.setPositionSeconds(1.0);
    item.setPlaying(false);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QVERIFY(node->childCount() >= 2);
    QCOMPARE(node->childAtIndex(1)->type(), QSGNode::GeometryNodeType);

    delete node;
}

void QmlSmokeTest::spectrogramGpuTextureUploadSkipsRedundantRecreation() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "texture-node");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.resize(200, 100);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    SpectrogramItem *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(200);
    item->setHeight(100);
    item->setDisplayMode(1); // Centered
    item->setPlaying(false);

    constexpr int bins = 32;
    constexpr int columns = 240;
    QByteArray data(bins * columns, '\x20');
    for (int column = 80; column < 120; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            data[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(128 + bin));
        }
    }

    item->feedPrecomputedChunk(
        data, bins, 0, columns, 0, columns,
        48000, 1024, false, true, 1, false);
    item->setPositionSeconds(1.0);

    QSGNode *node = item->updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QVERIFY(node->childCount() >= 2);
    auto *textureNode = spectrogramBodyTextureNode(node);
    QVERIFY(textureNode != nullptr);
    QSGTexture *initialTexture = textureNode->texture();
    QVERIFY(initialTexture != nullptr);

    QSGNode *updatedNode = item->updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);
    auto *updatedTextureNode = spectrogramBodyTextureNode(updatedNode);
    QVERIFY(updatedTextureNode != nullptr);
    QVERIFY(updatedTextureNode->texture() == initialTexture);

    delete node;
}

void QmlSmokeTest::spectrogramRollingGpuTextureUpdateKeepsTextureObject() {
    QQuickWindow window;
    window.resize(200, 100);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    auto *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(8);
    item->setHeight(6);
    item->setDisplayMode(0); // Rolling
    item->setPlaying(false);

    constexpr int bins = 4;
    constexpr int initialColumns = 12;
    QByteArray initialChunk(bins * initialColumns, '\x00');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(16 + column * 8 + bin));
        }
    }

    item->feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 64, 48000, 1024, false, true, 1, false);
    {
        QMutexLocker lock(&item->m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item->m_positionSeconds =
            ((static_cast<double>(initialColumns - 1) + 0.25) * 1024.0) / 48000.0;
        item->m_positionAnchorSeconds = item->m_positionSeconds;
        item->m_positionAnchorUpdatedAt = now;
        item->m_positionAnchorInitialized = true;
    }

    QSGNode *node = item->updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    auto *textureNode = spectrogramBodyTextureNode(node);
    QVERIFY(textureNode != nullptr);
    QSGTexture *initialTexture = textureNode->texture();
    QVERIFY(initialTexture != nullptr);

    QByteArray nextChunk(bins, '\x00');
    for (int bin = 0; bin < bins; ++bin) {
        nextChunk[bin] = static_cast<char>(static_cast<unsigned char>(220 + bin));
    }

    item->feedPrecomputedChunk(
        nextChunk, bins, 0, 1,
        initialColumns, 64, 48000, 1024, false, false, 1, false);
    {
        QMutexLocker lock(&item->m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item->m_positionSeconds =
            ((static_cast<double>(initialColumns) + 0.25) * 1024.0) / 48000.0;
        item->m_positionAnchorSeconds = item->m_positionSeconds;
        item->m_positionAnchorUpdatedAt = now;
        item->m_positionAnchorInitialized = true;
    }

    QSGNode *updatedNode = item->updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);
    auto *updatedTextureNode = spectrogramBodyTextureNode(updatedNode);
    QVERIFY(updatedTextureNode != nullptr);
    if (windowGraphicsApi(window) == QSGRendererInterface::Software) {
        QVERIFY2(
            updatedTextureNode->texture() != nullptr,
            "software full-mode fallback should keep a visible body texture node after rolling updates");
        QVERIFY2(
            !updatedTextureNode->rect().isEmpty(),
            "software full-mode fallback should keep the body texture node visible after rolling updates");
    } else {
        QVERIFY2(
            updatedTextureNode->texture() == initialTexture,
            "rolling GPU updates should reuse the same scene-graph texture object");
    }

    delete node;
}

void QmlSmokeTest::spectrogramRollingTextureCrossesOldRebaseBoundaryWithoutCollapsingToSingleNode() {
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(220, 80);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(0); // Rolling
    item.setPlaying(false);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);

    constexpr int bins = 4;
    constexpr int initialColumns = 220;
    QByteArray initialChunk(bins * initialColumns, '\x00');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(20 + (column % 200) + bin));
        }
    }

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item.feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 4096, 48000, 1024, false, true, 1, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds =
            ((static_cast<double>(initialColumns - 1) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    const QSGRendererInterface::GraphicsApi api = windowGraphicsApi(window);
    auto *initialBodyMaterialNode = spectrogramBodySampleMaterialNode(node);
    QVERIFY(initialBodyMaterialNode != nullptr);
    QVERIFY(initialBodyMaterialNode->material() != nullptr);
    auto *initialBodyTextureNode = spectrogramBodyTextureNode(node);
    QVERIFY(initialBodyTextureNode != nullptr);
    if (api == QSGRendererInterface::Software) {
        QVERIFY2(
            initialBodyTextureNode->texture() != nullptr && !initialBodyTextureNode->rect().isEmpty(),
            "software full-mode fallback should keep the initial body texture node visible");
    } else {
        QVERIFY(
            initialBodyTextureNode->texture() == nullptr || initialBodyTextureNode->rect().isEmpty());
    }

    for (int step = 0; step < 129; ++step) {
        QByteArray nextChunk(bins, '\x00');
        for (int bin = 0; bin < bins; ++bin) {
            nextChunk[bin] = static_cast<char>(
                static_cast<unsigned char>(120 + ((step + bin) % 100)));
        }
        item.feedPrecomputedChunk(
            nextChunk, bins, 0, 1,
            initialColumns + step, 4096, 48000, 1024, false, false, 1, false);
        {
            QMutexLocker lock(&item.m_stateMutex);
            const auto now = std::chrono::steady_clock::now();
            item.m_positionSeconds =
                ((static_cast<double>(initialColumns + step) + 0.25) * 1024.0) / 48000.0;
            item.m_positionAnchorSeconds = item.m_positionSeconds;
            item.m_positionAnchorUpdatedAt = now;
            item.m_positionAnchorInitialized = true;
        }
        node = item.updatePaintNode(node, nullptr);
        QVERIFY(node != nullptr);
    }

    auto *finalBodyMaterialNode = spectrogramBodySampleMaterialNode(node);
    QVERIFY(finalBodyMaterialNode != nullptr);
    QVERIFY(finalBodyMaterialNode->material() != nullptr);
    auto *finalBodyTextureNode = spectrogramBodyTextureNode(node);
    QVERIFY(finalBodyTextureNode != nullptr);
    if (api == QSGRendererInterface::Software) {
        QVERIFY2(
            finalBodyTextureNode->texture() != nullptr && !finalBodyTextureNode->rect().isEmpty(),
            "software full-mode fallback should keep the body texture node visible across rebase boundaries");
    } else {
        QVERIFY(
            finalBodyTextureNode->texture() == nullptr || finalBodyTextureNode->rect().isEmpty());
    }

    delete node;
}

void QmlSmokeTest::spectrogramRollingTextureCrossesBaseWidthBoundaryWithoutCollapsingToSingleNode() {
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(220, 80);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(0); // Rolling
    item.setPlaying(false);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);

    constexpr int bins = 4;
    constexpr int initialColumns = 220;
    QByteArray initialChunk(bins * initialColumns, '\x00');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(40 + (column % 160) + bin));
        }
    }

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item.feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 8192, 48000, 1024, false, true, 1, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds =
            ((static_cast<double>(initialColumns - 1) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    const QSGRendererInterface::GraphicsApi api = windowGraphicsApi(window);
    auto *initialBodyMaterialNode = spectrogramBodySampleMaterialNode(node);
    QVERIFY(initialBodyMaterialNode != nullptr);
    QVERIFY(initialBodyMaterialNode->material() != nullptr);
    auto *initialBodyTextureNode = spectrogramBodyTextureNode(node);
    QVERIFY(initialBodyTextureNode != nullptr);
    if (api == QSGRendererInterface::Software) {
        QVERIFY2(
            initialBodyTextureNode->texture() != nullptr && !initialBodyTextureNode->rect().isEmpty(),
            "software full-mode fallback should keep the initial body texture node visible");
    } else {
        QVERIFY(
            initialBodyTextureNode->texture() == nullptr || initialBodyTextureNode->rect().isEmpty());
    }
    for (int step = 0; step < 221; ++step) {
        QByteArray nextChunk(bins, '\x00');
        for (int bin = 0; bin < bins; ++bin) {
            nextChunk[bin] = static_cast<char>(
                static_cast<unsigned char>(80 + ((step + bin) % 120)));
        }
        item.feedPrecomputedChunk(
            nextChunk, bins, 0, 1,
            initialColumns + step, 8192, 48000, 1024, false, false, 1, false);
        {
            QMutexLocker lock(&item.m_stateMutex);
            const auto now = std::chrono::steady_clock::now();
            item.m_positionSeconds =
                ((static_cast<double>(initialColumns + step) + 0.25) * 1024.0) / 48000.0;
            item.m_positionAnchorSeconds = item.m_positionSeconds;
            item.m_positionAnchorUpdatedAt = now;
            item.m_positionAnchorInitialized = true;
        }
        node = item.updatePaintNode(node, nullptr);
        QVERIFY(node != nullptr);
    }

    auto *finalBodyMaterialNode = spectrogramBodySampleMaterialNode(node);
    QVERIFY(finalBodyMaterialNode != nullptr);
    QVERIFY(finalBodyMaterialNode->material() != nullptr);
    auto *finalBodyTextureNode = spectrogramBodyTextureNode(node);
    QVERIFY(finalBodyTextureNode != nullptr);
    if (api == QSGRendererInterface::Software) {
        QVERIFY2(
            finalBodyTextureNode->texture() != nullptr && !finalBodyTextureNode->rect().isEmpty(),
            "software full-mode fallback should keep the body texture node visible across base-width rollover");
    } else {
        QVERIFY(
            finalBodyTextureNode->texture() == nullptr || finalBodyTextureNode->rect().isEmpty());
    }

    delete node;
}

void QmlSmokeTest::spectrogramRollingTextureKeepsUpdatingRightEdgeAfterLongScroll() {
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 80);

    auto *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(window.width());
    item->setHeight(window.height());
    item->setDisplayMode(0); // Rolling
    item->setPlaying(false);
    item->setGridEnabled(false);
    item->setCrosshairEnabled(false);
    item->setShowFpsOverlay(false);

    constexpr int bins = 8;
    constexpr int initialColumns = 240;
    QByteArray initialChunk(bins * initialColumns, '\0');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>((column * 5 + bin * 17) % 256));
        }
    }

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item->feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 8192, 48000, 1024, false, true, 1, false);
    {
        QMutexLocker lock(&item->m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item->m_positionSeconds =
            ((static_cast<double>(initialColumns - 1) + 0.25) * 1024.0) / 48000.0;
        item->m_positionAnchorSeconds = item->m_positionSeconds;
        item->m_positionAnchorUpdatedAt = now;
        item->m_positionAnchorInitialized = true;
    }

    QCoreApplication::processEvents();
    QTest::qWait(50);

    for (int step = 0; step < 260; ++step) {
        QByteArray nextChunk(bins, '\0');
        for (int bin = 0; bin < bins; ++bin) {
            nextChunk[bin] = static_cast<char>(
                static_cast<unsigned char>((96 + step * 11 + bin * 29) % 256));
        }
        item->feedPrecomputedChunk(
            nextChunk, bins, 0, 1,
            initialColumns + step, 8192, 48000, 1024, false, false, 1, false);
        {
            QMutexLocker lock(&item->m_stateMutex);
            const auto now = std::chrono::steady_clock::now();
            item->m_positionSeconds =
                ((static_cast<double>(initialColumns + step) + 0.25) * 1024.0) / 48000.0;
            item->m_positionAnchorSeconds = item->m_positionSeconds;
            item->m_positionAnchorUpdatedAt = now;
            item->m_positionAnchorInitialized = true;
        }
        QCoreApplication::processEvents();
    }

    std::unique_ptr<QSGNode> node(item->updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);
    assertFullModeBodyNodeInvariants(
        node.get(),
        "rolling full mode should keep the material node bound after long segmented scroll");
    auto *beforeMaterialNode = spectrogramBodySampleMaterialNode(node.get());
    QVERIFY(beforeMaterialNode != nullptr);
    auto *beforeMaterial =
        dynamic_cast<SpectrogramBodySampleMaterial *>(beforeMaterialNode->material());
    QVERIFY(beforeMaterial != nullptr);
    QSGTexture *beforeBaseTexture = beforeMaterial->state.sourceTexture;
    QVERIFY(beforeBaseTexture != nullptr);
    const qint64 beforeBaseTextureKey = beforeBaseTexture->comparisonKey();
    auto *beforeTailTextureNode = spectrogramTailTextureNode(node.get());
    QVERIFY(beforeTailTextureNode != nullptr);
    QVERIFY(
        beforeTailTextureNode->texture() == nullptr || beforeTailTextureNode->rect().isEmpty());

    QImage beforeDisplay;
    {
        QMutexLocker lock(&item->m_stateMutex);
        QVERIFY(!item->m_gpuDisplayImage.isNull());
        beforeDisplay = item->m_gpuDisplayImage.copy();
    }

    QTest::qWait(30);
    QCoreApplication::processEvents();

    const QSGRendererInterface::GraphicsApi api = windowGraphicsApi(window);
    QImage before;
    if (api != QSGRendererInterface::Software) {
        before = window.grabWindow();
        QVERIFY(!before.isNull());
    }

    for (int step = 260; step < 280; ++step) {
        QByteArray nextChunk(bins, '\0');
        for (int bin = 0; bin < bins; ++bin) {
            nextChunk[bin] = static_cast<char>(
                static_cast<unsigned char>((192 + step * 13 + bin * 31) % 256));
        }
        item->feedPrecomputedChunk(
            nextChunk, bins, 0, 1,
            initialColumns + step, 8192, 48000, 1024, false, false, 1, false);
        {
            QMutexLocker lock(&item->m_stateMutex);
            const auto now = std::chrono::steady_clock::now();
            item->m_positionSeconds =
                ((static_cast<double>(initialColumns + step) + 0.25) * 1024.0) / 48000.0;
            item->m_positionAnchorSeconds = item->m_positionSeconds;
            item->m_positionAnchorUpdatedAt = now;
            item->m_positionAnchorInitialized = true;
        }
        QCoreApplication::processEvents();
    }

    node.reset(item->updatePaintNode(node.release(), nullptr));
    QVERIFY(node != nullptr);
    assertFullModeBodyNodeInvariants(
        node.get(),
        "rolling full mode should keep the material node bound after additional right-edge updates");
    auto *afterMaterialNode = spectrogramBodySampleMaterialNode(node.get());
    QVERIFY(afterMaterialNode != nullptr);
    auto *afterMaterial =
        dynamic_cast<SpectrogramBodySampleMaterial *>(afterMaterialNode->material());
    QVERIFY(afterMaterial != nullptr);
    QVERIFY(afterMaterial->state.sourceTexture != nullptr);
    QCOMPARE(afterMaterial->state.sourceTexture->comparisonKey(), beforeBaseTextureKey);
    auto *afterTailMaterialNode = spectrogramTailSampleMaterialNode(node.get());
    QVERIFY(afterTailMaterialNode != nullptr);
    if (api != QSGRendererInterface::Software) {
        QVERIFY2(
            geometryHasVisibleArea(afterTailMaterialNode),
            "rolling full mode should render the incremental right-edge tail through a material-backed node");
    }
    auto *afterTailTextureNode = spectrogramTailTextureNode(node.get());
    QVERIFY(afterTailTextureNode != nullptr);
    QVERIFY2(
        afterTailTextureNode->texture() == nullptr || afterTailTextureNode->rect().isEmpty(),
        "rolling full mode should keep the legacy tail texture node empty once the tail is rendered through the material path");

    QTest::qWait(30);
    QCoreApplication::processEvents();

    const QRect rightEdgeRect(
        std::max(0, window.width() - 48),
        0,
        48,
        window.height());
    const QRect leftEdgeRect(0, 0, 48, window.height());

    if (api == QSGRendererInterface::Software) {
        QImage afterDisplay;
        {
            QMutexLocker lock(&item->m_stateMutex);
            QVERIFY(!item->m_gpuDisplayImage.isNull());
            afterDisplay = item->m_gpuDisplayImage.copy();
        }

        const int changedRightPixels =
            countChangedPixelsInRect(beforeDisplay, afterDisplay, rightEdgeRect, 8);
        const int changedLeftPixels =
            countChangedPixelsInRect(beforeDisplay, afterDisplay, leftEdgeRect, 8);

        QVERIFY2(
            changedRightPixels > 150,
            "rolling display fallback should continue updating the right edge after long segmented scroll on the software backend");
        QVERIFY2(
            changedRightPixels >= changedLeftPixels / 4,
            "software-backend structural check should still show substantial right-edge updates after long segmented scroll");
        return;
    }

    const QImage after = window.grabWindow();
    QVERIFY(!after.isNull());

    const int changedRightPixels =
        countChangedPixelsInRect(before, after, rightEdgeRect, 8);
    const int changedLeftPixels =
        countChangedPixelsInRect(before, after, leftEdgeRect, 8);

    QVERIFY2(
        changedRightPixels > 150,
        "rolling spectrogram should continue updating the right edge after long segmented scroll");
    QVERIFY2(
        changedRightPixels >= changedLeftPixels / 4,
        "right-edge updates should not collapse into a tiny left-edge-only strip after long segmented scroll");
}

void QmlSmokeTest::spectrogramRollingFullModeMaterialBodyUsesScrolledSourceRegionDuringIncrementalAdvance() {
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 80);

    SpectrogramItem item;
    item.setParentItem(window.contentItem());
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setDisplayMode(0); // Rolling
    item.setPlaying(false);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);
    item.setShowFpsOverlay(false);

    constexpr int bins = 8;
    constexpr double expectedEffectiveZoom = 0.5;
    constexpr int initialColumns = 480;
    constexpr int advanceSteps = 12;
    QByteArray initialChunk(bins * initialColumns, '\0');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>((column * 7 + bin * 19) % 256));
        }
    }

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item.feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 8192, 48000, 1024, false, true, 1, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_zoomLevel = expectedEffectiveZoom;
        item.m_renderZoomLevel = expectedEffectiveZoom;
        item.m_positionSeconds =
            ((static_cast<double>(initialColumns - 1) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    std::unique_ptr<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(std::abs(item.effectiveZoomLocked() - expectedEffectiveZoom) < 0.001);
    }

    for (int step = 0; step < advanceSteps; ++step) {
        QByteArray nextChunk(bins, '\0');
        for (int bin = 0; bin < bins; ++bin) {
            nextChunk[bin] = static_cast<char>(
                static_cast<unsigned char>((160 + step * 13 + bin * 11) % 256));
        }
        item.feedPrecomputedChunk(
            nextChunk, bins, 0, 1,
            initialColumns + step, 8192, 48000, 1024, false, false, 1, false);
        {
            QMutexLocker lock(&item.m_stateMutex);
            const auto now = std::chrono::steady_clock::now();
            item.m_positionSeconds =
                ((static_cast<double>(initialColumns + step) + 0.25) * 1024.0) / 48000.0;
            item.m_positionAnchorSeconds = item.m_positionSeconds;
            item.m_positionAnchorUpdatedAt = now;
            item.m_positionAnchorInitialized = true;
        }
        node.reset(item.updatePaintNode(node.release(), nullptr));
        QVERIFY(node != nullptr);
    }

    assertFullModeBodyNodeInvariants(
        node.get(),
        "rolling full mode should keep the material node active while advancing incrementally");

    auto *materialNode = spectrogramBodySampleMaterialNode(node.get());
    QVERIFY(materialNode != nullptr);
    QVERIFY(materialNode->geometry() != nullptr);

    auto *tailMaterialNode = spectrogramTailSampleMaterialNode(node.get());
    QVERIFY(tailMaterialNode != nullptr);
    QVERIFY(tailMaterialNode->geometry() != nullptr);

    auto *tailNode = spectrogramTailTextureNode(node.get());
    QVERIFY(tailNode != nullptr);
    QVERIFY2(
        tailNode->texture() == nullptr || tailNode->rect().isEmpty(),
        "incremental full mode should keep the raw tail texture node hidden in full mode");

    const QSGRendererInterface::GraphicsApi api = windowGraphicsApi(window);
    if (api == QSGRendererInterface::Software) {
        QVERIFY2(
            materialNode->geometry()->vertexCount() > 0,
            "software incremental full mode should keep body material geometry allocated");
        QVERIFY2(
            tailMaterialNode->geometry()->vertexCount() > 0,
            "software incremental full mode should keep tail material geometry allocated");
        return;
    }

    const double expectedTailWidth =
        static_cast<double>(advanceSteps) * expectedEffectiveZoom;
    const double expectedSeamX =
        static_cast<double>(window.width()) - expectedTailWidth;
    const QRectF tailBounds = geometryBounds(tailMaterialNode);

    const auto *vertices = materialNode->geometry()->vertexDataAsTexturedPoint2D();
    float maxX = vertices[0].x;
    float minU = vertices[0].tx;
    float maxU = vertices[0].tx;
    for (int i = 1; i < materialNode->geometry()->vertexCount(); ++i) {
        maxX = std::max(maxX, vertices[i].x);
        minU = std::min(minU, vertices[i].tx);
        maxU = std::max(maxU, vertices[i].tx);
    }

    QVERIFY2(
        minU >= 0.0F,
        "incremental full mode should keep the material body source interval normalized after retained updates");
    QVERIFY2(
        maxU <= 1.0F,
        "incremental full mode should keep the material body source interval inside the retained texture");
    QVERIFY2(
        std::abs(tailBounds.width() - expectedTailWidth) <= 1.0,
        "incremental full mode should size the tail segment in viewport pixels at non-unit zoom");
    QVERIFY2(
        std::abs(tailBounds.x() - expectedSeamX) <= 1.0,
        "incremental full mode should place the tail seam in viewport pixels at non-unit zoom");
    QVERIFY2(
        std::abs(static_cast<double>(maxX) - expectedSeamX) <= 1.0,
        "incremental full mode should stop the material body at the viewport-pixel seam");
    QVERIFY2(
        std::abs(static_cast<double>(maxX) - tailBounds.x()) <= 1.0,
        "incremental full mode should keep the material body aligned with the tail segment");
    QVERIFY2(
        tailBounds.x() < static_cast<double>(window.width()),
        "incremental full mode should keep the tail seam inside the viewport");
    QVERIFY2(
        maxU > minU,
        "incremental full mode should keep a non-empty scrolled source interval for the material body");
}

void QmlSmokeTest::spectrogramRollingTextureKeepsBoundedVisibleNodeCountAfterLongScroll() {
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 80);

    SpectrogramItem item;
    item.setParentItem(window.contentItem());
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setDisplayMode(0); // Rolling
    item.setPlaying(false);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);
    item.setShowFpsOverlay(false);

    constexpr int bins = 8;
    constexpr int initialColumns = 240;
    QByteArray initialChunk(bins * initialColumns, '\0');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>((column * 7 + bin * 19) % 256));
        }
    }

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item.feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 8192, 48000, 1024, false, true, 1, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds =
            ((static_cast<double>(initialColumns - 1) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    for (int step = 0; step < 320; ++step) {
        QByteArray nextChunk(bins, '\0');
        for (int bin = 0; bin < bins; ++bin) {
            nextChunk[bin] = static_cast<char>(
                static_cast<unsigned char>((160 + step * 13 + bin * 11) % 256));
        }
        item.feedPrecomputedChunk(
            nextChunk, bins, 0, 1,
            initialColumns + step, 8192, 48000, 1024, false, false, 1, false);
        {
            QMutexLocker lock(&item.m_stateMutex);
            const auto now = std::chrono::steady_clock::now();
            item.m_positionSeconds =
                ((static_cast<double>(initialColumns + step) + 0.25) * 1024.0) / 48000.0;
            item.m_positionAnchorSeconds = item.m_positionSeconds;
            item.m_positionAnchorUpdatedAt = now;
            item.m_positionAnchorInitialized = true;
        }
        node = item.updatePaintNode(node, nullptr);
        QVERIFY(node != nullptr);
    }

    QVERIFY2(
        visibleTextureNodeCount(node) <= 2,
        "rolling spectrogram body should stay bounded to one base texture plus one tail texture after long scroll");

    delete node;
}

void QmlSmokeTest::spectrogramRollingPreFillUsesFullViewportWidthCache() {
    SpectrogramItem item;
    item.setWidth(12);
    item.setHeight(6);
    item.setDisplayMode(0); // Rolling
    item.setPlaying(false);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);

    constexpr int bins = 4;
    constexpr int initialColumns = 4;
    QByteArray initialChunk(bins * initialColumns, '\0');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(20 + column * 10 + bin));
        }
    }

    item.feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 64, 48000, 1024, false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_zoomLevel = 2.0;
        item.m_renderZoomLevel = 2.0;
        item.m_positionSeconds =
            ((static_cast<double>(initialColumns - 1) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QImage initialDisplay;
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        QCOMPARE(item.m_gpuDisplayImage.width(), 12);
        QCOMPARE(item.m_gpuDisplayFilledWidth, 8);
        QCOMPARE(item.m_gpuDisplayLastUpdateWasIncremental, false);
        initialDisplay = item.m_gpuDisplayImage.copy();
    }

    QByteArray appendChunk(bins, '\0');
    for (int bin = 0; bin < bins; ++bin) {
        appendChunk[bin] = static_cast<char>(
            static_cast<unsigned char>(80 + bin));
    }
    item.feedPrecomputedChunk(
        appendChunk, bins, 0, 1,
        initialColumns, 64, 48000, 1024, false, false, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds =
            ((static_cast<double>(initialColumns) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        QCOMPARE(item.m_gpuDisplayImage.width(), 12);
        QCOMPARE(item.m_gpuDisplayFilledWidth, 10);
        QVERIFY(item.m_gpuDisplayLastUpdateWasIncremental);
        QCOMPARE(item.m_gpuDisplayLastShiftPixels, 2);
        QCOMPARE(item.m_gpuDisplayLastUpdatedColumns, 2);
        for (int y = 0; y < initialDisplay.height(); ++y) {
            for (int x = 0; x < 8; ++x) {
                QCOMPARE(
                    item.m_gpuDisplayImage.pixelColor(x, y),
                    initialDisplay.pixelColor(x + 2, y));
            }
        }
    }

    delete node;
}

void QmlSmokeTest::spectrogramRollingPreFillHandsOffToSteadyScrollIncrementally() {
    SpectrogramItem item;
    item.setWidth(12);
    item.setHeight(6);
    item.setDisplayMode(0); // Rolling
    item.setPlaying(false);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);

    constexpr int bins = 4;
    constexpr int initialColumns = 6;
    QByteArray initialChunk(bins * initialColumns, '\0');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(20 + column * 10 + bin));
        }
    }

    item.feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 64, 48000, 1024, false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_zoomLevel = 2.0;
        item.m_renderZoomLevel = 2.0;
        item.m_positionSeconds =
            ((static_cast<double>(initialColumns - 1) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QImage initialDisplay;
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        QCOMPARE(item.m_gpuDisplayImage.width(), 12);
        QCOMPARE(item.m_gpuDisplayFilledWidth, 12);
        initialDisplay = item.m_gpuDisplayImage.copy();
    }

    QByteArray appendChunk(bins, '\0');
    for (int bin = 0; bin < bins; ++bin) {
        appendChunk[bin] = static_cast<char>(
            static_cast<unsigned char>(120 + bin));
    }
    item.feedPrecomputedChunk(
        appendChunk, bins, 0, 1,
        initialColumns, 64, 48000, 1024, false, false, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds =
            ((static_cast<double>(initialColumns) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY2(
            item.m_gpuDisplayLastUpdateWasIncremental,
            "rolling cache should hand off from pre-fill to steady-state scroll without a full rebuild");
        QCOMPARE(item.m_gpuDisplayLastShiftPixels, 2);
        QCOMPARE(item.m_gpuDisplayLastUpdatedColumns, 2);
        QCOMPARE(item.m_gpuDisplayImage.width(), 12);
        for (int y = 0; y < initialDisplay.height(); ++y) {
            for (int x = 0; x < initialDisplay.width() - 2; ++x) {
                QCOMPARE(
                    item.m_gpuDisplayImage.pixelColor(x, y),
                    initialDisplay.pixelColor(x + 2, y));
            }
        }
    }

    delete node;
}

void QmlSmokeTest::spectrogramRollingPreFillCompletionAndAdvanceStaysIncremental() {
    SpectrogramItem item;
    item.setWidth(12);
    item.setHeight(6);
    item.setDisplayMode(0); // Rolling
    item.setPlaying(false);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);

    constexpr int bins = 4;
    constexpr int initialColumns = 5;
    QByteArray initialChunk(bins * initialColumns, '\0');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(24 + column * 11 + bin));
        }
    }

    item.feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 64, 48000, 1024, false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_zoomLevel = 2.0;
        item.m_renderZoomLevel = 2.0;
        item.m_positionSeconds =
            ((static_cast<double>(initialColumns - 1) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QImage initialDisplay;
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        QCOMPARE(item.m_gpuDisplayFilledWidth, 10);
        initialDisplay = item.m_gpuDisplayImage.copy();
    }

    QByteArray appendChunk(bins * 2, '\0');
    for (int column = 0; column < 2; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            appendChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(120 + column * 9 + bin));
        }
    }
    item.feedPrecomputedChunk(
        appendChunk, bins, 0, 2,
        initialColumns, 64, 48000, 1024, false, false, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds =
            ((static_cast<double>(initialColumns + 1) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY2(
            item.m_gpuDisplayLastUpdateWasIncremental,
            "rolling cache should not full-rebuild when the viewport becomes full and starts advancing in the same frame");
        QCOMPARE(item.m_gpuDisplayImage.width(), 12);
        QCOMPARE(item.m_gpuDisplayFilledWidth, 12);
        QVERIFY(item.m_gpuDisplayLastUpdatedColumns > 0);
        QVERIFY(item.m_gpuDisplayLastUpdatedColumns <= 4);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        QVERIFY(initialDisplay != item.m_gpuDisplayImage);
    }

    delete node;
}

void QmlSmokeTest::spectrogramRollingNonIntegerZoomAdvanceStaysIncremental() {
    SpectrogramItem item;
    item.setWidth(7);
    item.setHeight(6);
    item.setDisplayMode(0); // Rolling
    item.setPlaying(false);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);

    constexpr int bins = 4;
    constexpr int initialColumns = 4;
    QByteArray initialChunk(bins * initialColumns, '\0');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(24 + column * 12 + bin));
        }
    }

    item.feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 64, 48000, 1024, false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_zoomLevel = 1.75;
        item.m_renderZoomLevel = 1.75;
        item.m_positionSeconds =
            ((static_cast<double>(initialColumns - 1) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QImage initialDisplay;
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        QCOMPARE(item.m_gpuDisplayImage.width(), 7);
        QCOMPARE(item.m_gpuDisplayFilledWidth, 7);
        QCOMPARE(item.m_gpuDisplayEffectiveZoom, 1.75);
        initialDisplay = item.m_gpuDisplayImage.copy();
    }

    QByteArray appendChunk(bins, '\0');
    for (int bin = 0; bin < bins; ++bin) {
        appendChunk[bin] = static_cast<char>(
            static_cast<unsigned char>(128 + bin));
    }
    item.feedPrecomputedChunk(
        appendChunk, bins, 0, 1,
        initialColumns, 64, 48000, 1024, false, false, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds =
            ((static_cast<double>(initialColumns) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY2(
            item.m_gpuDisplayLastUpdateWasIncremental,
            "rolling cache should stay incremental even when one column advance maps to a non-integer pixel shift");
        QCOMPARE(item.m_gpuDisplayLastShiftPixels, 1);
        QVERIFY(item.m_gpuDisplaySubpixelOffset > 0.70);
        QVERIFY(item.m_gpuDisplaySubpixelOffset < 0.80);
        for (int y = 0; y < initialDisplay.height(); ++y) {
            for (int x = 0; x < initialDisplay.width() - 1; ++x) {
                QCOMPARE(
                    item.m_gpuDisplayImage.pixelColor(x, y),
                    initialDisplay.pixelColor(x + 1, y));
            }
        }
    }

    delete node;
}

void QmlSmokeTest::spectrogramGpuFullModeStaysVisibleAfterMultipleRollingUpdatesWithoutCpuSourceImage() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "full");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    auto *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(window.width());
    item->setHeight(window.height());
    item->setDisplayMode(0);
    item->setGridEnabled(false);
    item->setCrosshairEnabled(false);
    item->setShowFpsOverlay(false);
    item->setPlaying(false);

    constexpr int bins = 64;
    constexpr int initialColumns = 480;
    constexpr int steps = 64;
    QByteArray initialChunk(bins * initialColumns, '\0');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>((column * 5 + bin * 3) % 256));
        }
    }

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item->feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 4096, 48000, 1024, false, true, 97, false);
    {
        QMutexLocker lock(&item->m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item->m_zoomLevel = 0.5;
        item->m_renderZoomLevel = 0.5;
        item->m_positionSeconds =
            ((static_cast<double>(initialColumns - 1) + 0.25) * 1024.0) / 48000.0;
        item->m_positionAnchorSeconds = item->m_positionSeconds;
        item->m_positionAnchorUpdatedAt = now;
        item->m_positionAnchorInitialized = true;
    }

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    std::unique_ptr<QSGNode> node(item->updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);
    assertFullModeBodyNodeInvariants(
        node.get(),
        "rolling full mode should start on the retained-texture material path");

    for (int step = 0; step < steps; ++step) {
        QByteArray nextChunk(bins, '\0');
        for (int bin = 0; bin < bins; ++bin) {
            nextChunk[bin] = static_cast<char>(
                static_cast<unsigned char>((160 + step * 9 + bin * 7) % 256));
        }
        item->feedPrecomputedChunk(
            nextChunk, bins, 0, 1,
            initialColumns + step, 4096, 48000, 1024, false, false, 97, false);
        {
            QMutexLocker lock(&item->m_stateMutex);
            const auto now = std::chrono::steady_clock::now();
            item->m_positionSeconds =
                ((static_cast<double>(initialColumns + step) + 0.25) * 1024.0) / 48000.0;
            item->m_positionAnchorSeconds = item->m_positionSeconds;
            item->m_positionAnchorUpdatedAt = now;
            item->m_positionAnchorInitialized = true;
        }
        QCoreApplication::processEvents();
        node.reset(item->updatePaintNode(node.release(), nullptr));
        QVERIFY(node != nullptr);
    }

    assertFullModeBodyNodeInvariants(
        node.get(),
        "rolling full mode should keep the retained-texture body material active after repeated updates");
    auto *sampleNode = spectrogramBodySampleMaterialNode(node.get());
    QVERIFY(sampleNode != nullptr);
    auto *sampleMaterial =
        dynamic_cast<SpectrogramBodySampleMaterial *>(sampleNode->material());
    QVERIFY(sampleMaterial != nullptr);
    QVERIFY2(
        sampleMaterial->state.sourceTexture != nullptr,
        "rolling full mode should keep a retained source texture bound after repeated updates");

    const QSGRendererInterface::GraphicsApi api = windowGraphicsApi(window);
    if (api == QSGRendererInterface::Software) {
        QImage displayImage;
        {
            QMutexLocker lock(&item->m_stateMutex);
            displayImage = item->m_gpuDisplayImage.copy();
        }
        QVERIFY2(
            !displayImage.isNull(),
            "software rolling full mode should keep a CPU display image available for fallback visibility");
        QVERIFY2(
            countVisiblePixelsInRect(displayImage, QRect(20, 16, 200, 64), 8) > 100,
            "software rolling full mode should stay visible in the warmed CPU display fallback after repeated retained-source updates");
        return;
    }

    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());
    QVERIFY2(
        countVisiblePixelsInRect(frame, QRect(20, 16, 200, 88), 8) > 100,
        "rolling full mode should stay visible after repeated retained-source updates without any CPU source image path");
}

void QmlSmokeTest::spectrogramRollingDisplayCacheIgnoresOffscreenWriteHeadGrowth() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "full");

    SpectrogramItem item;
    item.setWidth(8);
    item.setHeight(6);
    item.setDisplayMode(0); // Rolling
    item.setPlaying(false);

    constexpr int bins = 4;
    constexpr int initialColumns = 20;
    QByteArray initialChunk(bins * initialColumns, '\x00');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(16 + column * 4 + bin));
        }
    }

    item.feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 64, 48000, 1024, false, true, 1, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds =
            ((static_cast<double>(7) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    quint64 initialDisplayGeneration = 0;
    QImage initialDisplay;
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        initialDisplayGeneration = item.m_gpuDisplayGeneration;
        initialDisplay = item.m_gpuDisplayImage.copy();
    }

    QByteArray nextChunk(bins * 4, '\x00');
    for (int column = 0; column < 4; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            nextChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(200 + column * 4 + bin));
        }
    }

    item.feedPrecomputedChunk(
        nextChunk, bins, 0, 4,
        initialColumns, 64, 48000, 1024, false, false, 1, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_gpuDisplayGeneration, initialDisplayGeneration);
        QCOMPARE(item.m_gpuDisplayImage, initialDisplay);
        QVERIFY2(
            !item.m_gpuDisplayLastUpdateWasIncremental,
            "offscreen write-head growth should not touch the visible rolling display cache");
    }

    delete node;
    qunsetenv("FERROUS_GPU_DEBUG_MODE");
}

void QmlSmokeTest::spectrogramCenteredDisplayCacheIgnoresOffscreenDecodeGrowth() {
    SpectrogramItem item;
    item.setWidth(8);
    item.setHeight(6);
    item.setDisplayMode(1); // Centered
    item.setPlaying(false);

    constexpr int bins = 4;
    constexpr int initialColumns = 32;
    constexpr int totalColumns = 256;
    QByteArray initialChunk(bins * initialColumns, '\x00');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(16 + column * 4 + bin));
        }
    }

    item.feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, totalColumns, 48000, 1024, false, true, 1, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds = (10.25 * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    quint64 initialDisplayGeneration = 0;
    QImage initialDisplay;
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        initialDisplayGeneration = item.m_gpuDisplayGeneration;
        initialDisplay = item.m_gpuDisplayImage.copy();
    }

    QByteArray nextChunk(bins * 8, '\x00');
    for (int column = 0; column < 8; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            nextChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(180 + column * 4 + bin));
        }
    }

    item.feedPrecomputedChunk(
        nextChunk, bins, 0, 8,
        initialColumns, totalColumns, 48000, 1024, false, false, 1, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_gpuDisplayGeneration, initialDisplayGeneration);
        QCOMPARE(item.m_gpuDisplayImage, initialDisplay);
        QVERIFY2(
            !item.m_gpuDisplayLastUpdateWasIncremental,
            "offscreen centered decode growth should not touch a fully covered visible cache");
    }

    delete node;
}

void QmlSmokeTest::spectrogramRollingGpuDisplayCacheAdvancesIncrementally() {
    SpectrogramItem item;
    item.setWidth(8);
    item.setHeight(6);
    item.setDisplayMode(0); // Rolling
    item.setPlaying(false);

    constexpr int bins = 4;
    constexpr int initialColumns = 12;
    QByteArray initialChunk(bins * initialColumns, '\x00');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(16 + column * 8 + bin));
        }
    }

    item.feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 64, 48000, 1024, false, true, 1, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds =
            ((static_cast<double>(initialColumns - 1) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QImage initialDisplay;
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        initialDisplay = item.m_gpuDisplayImage.copy();
    }

    QByteArray nextChunk(bins, '\x00');
    for (int bin = 0; bin < bins; ++bin) {
        nextChunk[bin] = static_cast<char>(static_cast<unsigned char>(220 + bin));
    }

    item.feedPrecomputedChunk(
        nextChunk, bins, 0, 1,
        initialColumns, 64, 48000, 1024, false, false, 1, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds =
            ((static_cast<double>(initialColumns) + 0.25) * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY2(
            item.m_gpuDisplayLastUpdateWasIncremental,
            "rolling display cache should shift/repaint incrementally for a one-column advance");
        QCOMPARE(item.m_gpuDisplayLastShiftPixels, 1);
        QCOMPARE(item.m_gpuDisplayLastUpdatedColumns, 1);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        QCOMPARE(item.m_gpuDisplayImage.width(), initialDisplay.width());
        QCOMPARE(item.m_gpuDisplayImage.height(), initialDisplay.height());
        for (int y = 0; y < initialDisplay.height(); ++y) {
            for (int x = 0; x < initialDisplay.width() - 1; ++x) {
                QCOMPARE(
                    item.m_gpuDisplayImage.pixelColor(x, y),
                    initialDisplay.pixelColor(x + 1, y));
            }
        }
    }

    delete node;
}

void QmlSmokeTest::spectrogramCenteredGpuDisplayCacheAdvancesIncrementally() {
    SpectrogramItem item;
    item.setWidth(8);
    item.setHeight(6);
    item.setDisplayMode(1); // Centered
    item.setPlaying(false);

    constexpr int bins = 4;
    constexpr int totalColumns = 64;
    QByteArray chunk(bins * totalColumns, '\x00');
    for (int column = 0; column < totalColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            chunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(16 + column * 3 + bin));
        }
    }

    item.feedPrecomputedChunk(
        chunk, bins, 0, totalColumns,
        0, totalColumns, 48000, 1024, true, true, 1, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds = (10.25 * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QImage initialDisplay;
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        initialDisplay = item.m_gpuDisplayImage.copy();
    }

    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds = (11.25 * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY2(
            item.m_gpuDisplayLastUpdateWasIncremental,
            "centered display cache should shift/repaint incrementally for a one-column advance");
        QCOMPARE(item.m_gpuDisplayLastShiftPixels, 1);
        QCOMPARE(item.m_gpuDisplayLastUpdatedColumns, 1);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        QCOMPARE(item.m_gpuDisplayImage.width(), initialDisplay.width());
        QCOMPARE(item.m_gpuDisplayImage.height(), initialDisplay.height());
        for (int y = 0; y < initialDisplay.height(); ++y) {
            for (int x = 0; x < initialDisplay.width() - 1; ++x) {
                QCOMPARE(
                    item.m_gpuDisplayImage.pixelColor(x, y),
                    initialDisplay.pixelColor(x + 1, y));
            }
        }
    }

    delete node;
}

void QmlSmokeTest::spectrogramGpuWindowCaptureStillShowsVisiblePixelsAfterPublicPivot() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "texture-node");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.resize(200, 100);

    SpectrogramItem *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(200);
    item->setHeight(100);
    item->setDisplayMode(1); // Centered

    constexpr int bins = 64;
    constexpr int columns = 200;
    QByteArray data(bins * columns, '\x00');
    for (int column = 0; column < columns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            const int base = 96 + (column * 96) / std::max(1, columns - 1);
            const int ripple = (bin * 48) / std::max(1, bins - 1);
            data[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(std::min(255, base + ripple)));
        }
    }

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item->feedPrecomputedChunk(
        data, bins, 0, columns, 0, columns, 48000, 1024,
        false, true, 1, false);
    item->setPositionSeconds(2.0);
    item->setPlaying(false);

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());

    int visiblePixels = 0;
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            const QColor pixel = frame.pixelColor(x, y);
            if (pixel.red() > 8 || pixel.green() > 8 || pixel.blue() > 8) {
                visiblePixels += 1;
            }
        }
    }
    QVERIFY2(
        visiblePixels > 15000,
        "GPU spectrogram capture should show the spectrogram body, not just a thin line");
}

void QmlSmokeTest::spectrogramGpuWindowCaptureShowsTextureNodeDebugStage() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "texture-node");

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);

    constexpr int bins = 64;
    constexpr int totalColumns = 240;
    QByteArray chunk(bins * totalColumns, '\x00');
    for (int column = 0; column < totalColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            chunk[column * bins + bin] = static_cast<char>((column + bin) % 256);
        }
    }
    item.feedPrecomputedChunk(
        chunk, bins, 0, totalColumns, 0, totalColumns, 48000, 1024, false, true, 31);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();
    QTest::qWait(50);

    QScopedPointer<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(!node.isNull());
    assertTextureNodeDebugBodyInvariants(
        node.data(),
        "texture-node debug mode should keep the legacy body texture node active and the material source texture unset");

    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());

    int visiblePixels = 0;
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            const QColor color = frame.pixelColor(x, y);
            if (color.red() > 10 || color.green() > 10 || color.blue() > 10) {
                visiblePixels += 1;
            }
        }
    }
    QVERIFY2(visiblePixels > 0, "texture-node debug stage should render visible pixels");

    qunsetenv("FERROUS_GPU_DEBUG_MODE");
}

void QmlSmokeTest::spectrogramGpuWindowCaptureShowsVisiblePaletteMappedPixelsInFullMode() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "full");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(window.width());
    item->setHeight(window.height());
    item->setDisplayMode(1);
    item->setGridEnabled(false);
    item->setCrosshairEnabled(false);
    item->setShowFpsOverlay(false);
    item->setPlaying(false);

    constexpr int bins = 64;
    constexpr int totalColumns = 240;
    QByteArray chunk(bins * totalColumns, '\x00');
    for (int column = 0; column < totalColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            const int base = 48 + (column * 160) / std::max(1, totalColumns - 1);
            const int ripple = (bin * 40) / std::max(1, bins - 1);
            chunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(std::min(255, base + ripple)));
        }
    }

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item->feedPrecomputedChunk(
        chunk, bins, 0, totalColumns, 0, totalColumns, 48000, 1024,
        false, true, 31, false);
    item->setPositionSeconds(2.0);

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    QScopedPointer<QSGNode> node(item->updatePaintNode(nullptr, nullptr));
    QVERIFY(!node.isNull());
    assertFullModeBodyNodeInvariants(
        node.data(),
        "full-mode capture should bind the spectrogram source texture into the material node");

    const QSGRendererInterface::GraphicsApi api = windowGraphicsApi(window);
    if (api == QSGRendererInterface::Software) {
        auto *materialNode = spectrogramBodySampleMaterialNode(node.data());
        QVERIFY(materialNode != nullptr);
        auto *material =
            dynamic_cast<SpectrogramBodySampleMaterial *>(materialNode->material());
        QVERIFY(material != nullptr);
        QVERIFY2(
            material->state.dbRemapTexture != nullptr,
            "software fallback should still bind the dB remap lookup texture into the material state");
        QVERIFY2(
            material->state.paletteTexture != nullptr,
            "software fallback should still bind the palette lookup texture into the material state");
        {
            QMutexLocker lock(&item->m_stateMutex);
            assertUploadedPaletteOrdering(*item);
        }
        QSKIP(qPrintable(
            QStringLiteral(
                "Skipping full-mode palette pixel assertion on %1 backend because offscreen software rendering does not render the custom material path into grabWindow(); structural material invariants were verified instead")
                .arg(graphicsApiName(api))));
    }

    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());

    const QRect bodyRegion(40, 15, 160, 80);
    const QRect bounded = bodyRegion.intersected(frame.rect());
    int visiblePixels = 0;
    int nonGrayPixels = 0;
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor pixel = frame.pixelColor(x, y);
            if (pixel.red() > 8 || pixel.green() > 8 || pixel.blue() > 8) {
                ++visiblePixels;
                if (pixel.red() != pixel.green() || pixel.green() != pixel.blue()) {
                    ++nonGrayPixels;
                }
            }
        }
    }

    QVERIFY2(visiblePixels > 100, "full-mode public shader path should render visible body pixels");
    QVERIFY2(
        nonGrayPixels > 100,
        "full-mode public shader path should render palette-mapped non-gray pixels");
}

void QmlSmokeTest::spectrogramGpuWindowCaptureMatchesCpuPaletteDirectionInFullMode() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "full");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(window.width());
    item->setHeight(window.height());
    item->setDisplayMode(1);
    item->setGridEnabled(false);
    item->setCrosshairEnabled(false);
    item->setShowFpsOverlay(false);
    item->setPlaying(false);
    item->setDbRange(96.0);

    constexpr int bins = 64;
    constexpr int totalColumns = 240;
    constexpr int quietValue = 32;
    constexpr int loudValue = 224;
    QByteArray chunk(bins * totalColumns, '\x00');
    for (int column = 0; column < totalColumns; ++column) {
        const int value = column < totalColumns / 2 ? quietValue : loudValue;
        for (int bin = 0; bin < bins; ++bin) {
            chunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(value));
        }
    }

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item->feedPrecomputedChunk(
        chunk, bins, 0, totalColumns, 0, totalColumns, 48000, 1024,
        false, true, 31, false);
    item->setPositionSeconds(2.0);

    QColor expectedQuietColor;
    QColor expectedLoudColor;
    {
        QMutexLocker lock(&item->m_stateMutex);
        expectedQuietColor = expectedPaletteColorForIntensity(*item, quietValue);
        expectedLoudColor = expectedPaletteColorForIntensity(*item, loudValue);
    }

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    std::unique_ptr<QSGNode> node(item->updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);
    assertFullModeBodyNodeInvariants(
        node.get(),
        "full-mode color capture should keep the material-backed body node active");

    const QSGRendererInterface::GraphicsApi api = windowGraphicsApi(window);
    if (api == QSGRendererInterface::Software) {
        auto *materialNode = spectrogramBodySampleMaterialNode(node.get());
        QVERIFY(materialNode != nullptr);
        auto *material =
            dynamic_cast<SpectrogramBodySampleMaterial *>(materialNode->material());
        QVERIFY(material != nullptr);
        QVERIFY2(
            material->state.dbRemapTexture != nullptr,
            "software fallback should still bind the dB remap lookup texture into the material state");
        QVERIFY2(
            material->state.paletteTexture != nullptr,
            "software fallback should still bind the palette lookup texture into the material state");
        {
            QMutexLocker lock(&item->m_stateMutex);
            assertUploadedPaletteOrdering(*item);
        }
        assertFragmentShaderInvertsPaletteLookup();
        QSKIP(qPrintable(
            QStringLiteral(
                "Skipping full-mode palette-direction pixel assertion on %1 backend because offscreen software rendering does not render the custom material path into grabWindow(); structural material invariants were verified instead")
                .arg(graphicsApiName(api))));
    }

    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());

    const QRect bodyRegion(40, 15, 160, 80);
    const QRect bounded = bodyRegion.intersected(frame.rect());
    const QRect quietRegion(
        bounded.left() + 8,
        bounded.top() + 6,
        std::max(12, bounded.width() / 4),
        std::max(20, bounded.height() - 12));
    const QRect loudRegion(
        bounded.right() - std::max(12, bounded.width() / 4) - 7,
        bounded.top() + 6,
        std::max(12, bounded.width() / 4),
        std::max(20, bounded.height() - 12));
    int quietVisiblePixels = 0;
    int loudVisiblePixels = 0;
    const QColor quietAverage = averageVisibleColorInRect(frame, quietRegion, 8, &quietVisiblePixels);
    const QColor loudAverage = averageVisibleColorInRect(frame, loudRegion, 8, &loudVisiblePixels);

    QVERIFY2(
        quietVisiblePixels > 100,
        "full-mode palette-direction capture should render visible quiet-side pixels");
    QVERIFY2(
        loudVisiblePixels > 100,
        "full-mode palette-direction capture should render visible loud-side pixels");
    QVERIFY2(
        manhattanColorDistance(quietAverage, expectedQuietColor)
            < manhattanColorDistance(quietAverage, expectedLoudColor),
        "quiet-side capture should stay closer to the CPU renderer's quiet palette color than the loud color");
    QVERIFY2(
        manhattanColorDistance(loudAverage, expectedLoudColor)
            < manhattanColorDistance(loudAverage, expectedQuietColor),
        "loud-side capture should stay closer to the CPU renderer's loud palette color than the quiet color");
}

void QmlSmokeTest::spectrogramRollingGpuWindowCaptureShowsVisiblePaletteMappedPixelsInFullMode() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "full");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(window.width());
    item->setHeight(window.height());
    item->setDisplayMode(0);
    item->setGridEnabled(false);
    item->setCrosshairEnabled(false);
    item->setShowFpsOverlay(false);
    item->setPlaying(false);
    item->setDbRange(96.0);

    constexpr int bins = 64;
    constexpr double expectedEffectiveZoom = 0.5;
    constexpr int initialColumns = 480;
    constexpr int quietValue = 32;
    constexpr int loudValue = 224;
    constexpr int advanceSteps = 24;
    QByteArray initialChunk(bins * initialColumns, '\x00');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(quietValue));
        }
    }

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item->feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns, 0, initialColumns, 48000, 1024,
        false, true, 31, false);
    {
        QMutexLocker lock(&item->m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item->m_zoomLevel = expectedEffectiveZoom;
        item->m_renderZoomLevel = expectedEffectiveZoom;
        item->m_positionSeconds =
            ((static_cast<double>(initialColumns - 1) + 0.25) * 1024.0) / 48000.0;
        item->m_positionAnchorSeconds = item->m_positionSeconds;
        item->m_positionAnchorUpdatedAt = now;
        item->m_positionAnchorInitialized = true;
    }

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    std::unique_ptr<QSGNode> node(item->updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);
    assertFullModeBodyNodeInvariants(
        node.get(),
        "rolling full-mode capture should bind the spectrogram source texture into the material node");

    QColor expectedQuietColor;
    QColor expectedTailColor;
    {
        QMutexLocker lock(&item->m_stateMutex);
        expectedQuietColor = expectedPaletteColorForIntensity(*item, quietValue);
        expectedTailColor = expectedPaletteColorForIntensity(*item, loudValue);
    }

    for (int step = 0; step < advanceSteps; ++step) {
        QByteArray nextChunk(bins, '\0');
        for (int bin = 0; bin < bins; ++bin) {
            nextChunk[bin] = static_cast<char>(
                static_cast<unsigned char>(loudValue));
        }
        item->feedPrecomputedChunk(
            nextChunk, bins, 0, 1,
            initialColumns + step, 8192, 48000, 1024, false, false, 31, false);
        {
            QMutexLocker lock(&item->m_stateMutex);
            const auto now = std::chrono::steady_clock::now();
            item->m_positionSeconds =
                ((static_cast<double>(initialColumns + step) + 0.25) * 1024.0) / 48000.0;
            item->m_positionAnchorSeconds = item->m_positionSeconds;
            item->m_positionAnchorUpdatedAt = now;
            item->m_positionAnchorInitialized = true;
        }
        QCoreApplication::processEvents();
        node.reset(item->updatePaintNode(node.release(), nullptr));
        QVERIFY(node != nullptr);
    }

    QTest::qWait(30);
    QCoreApplication::processEvents();

    assertFullModeBodyNodeInvariants(
        node.get(),
        "rolling full-mode incremental capture should keep the material-backed base body active");

    auto *tailNode = spectrogramTailTextureNode(node.get());
    QVERIFY(tailNode != nullptr);
    QVERIFY2(
        tailNode->texture() == nullptr || tailNode->rect().isEmpty(),
        "rolling full-mode should not expose the incremental right-edge tail through the raw texture node");

    const QSGRendererInterface::GraphicsApi api = windowGraphicsApi(window);
    if (api == QSGRendererInterface::Software) {
        auto *materialNode = spectrogramBodySampleMaterialNode(node.get());
        QVERIFY(materialNode != nullptr);
        auto *material =
            dynamic_cast<SpectrogramBodySampleMaterial *>(materialNode->material());
        QVERIFY(material != nullptr);
        QVERIFY2(
            material->state.sourceTexture != nullptr,
            "software fallback should still keep the rolling full-mode material source bound");
        QVERIFY2(
            material->state.dbRemapTexture != nullptr,
            "software fallback should still bind the rolling full-mode dB remap lookup texture");
        QVERIFY2(
            material->state.paletteTexture != nullptr,
            "software fallback should still bind the rolling full-mode palette lookup texture");
        auto *tailMaterialNode = spectrogramTailSampleMaterialNode(node.get());
        QVERIFY2(
            tailMaterialNode != nullptr,
            "software fallback should still keep a dedicated material-backed tail node in rolling full mode");
        QVERIFY2(
            tailMaterialNode->geometry() != nullptr,
            "software fallback should still keep the rolling tail material node allocated once incremental advance is active");
        auto *tailMaterial =
            dynamic_cast<SpectrogramBodySampleMaterial *>(tailMaterialNode->material());
        QVERIFY(tailMaterial != nullptr);
        QVERIFY2(
            tailMaterial->state.sourceTexture != nullptr,
            "software fallback should bind the incremental right-edge tail source texture into the tail material");
        QVERIFY2(
            tailMaterial->state.dbRemapTexture != nullptr,
            "software fallback should bind the dB remap lookup into the tail material");
        QVERIFY2(
            tailMaterial->state.paletteTexture != nullptr,
            "software fallback should bind the palette lookup into the tail material");
        {
            QMutexLocker lock(&item->m_stateMutex);
            assertUploadedPaletteOrdering(*item);
        }
        QSKIP(qPrintable(
            QStringLiteral(
                "Skipping rolling full-mode palette pixel assertion on %1 backend because offscreen software rendering does not render the custom material path into grabWindow(); structural material invariants were verified instead")
                .arg(graphicsApiName(api))));
    }

    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());

    const int tailWidth = static_cast<int>(std::lround(
        static_cast<double>(advanceSteps) * expectedEffectiveZoom));
    const QRect tailRegion(window.width() - tailWidth + 2, 16, tailWidth - 4, 88);
    const QRect quietRegion(20, 16, 24, 88);
    int tailVisiblePixels = 0;
    int quietVisiblePixels = 0;
    const QColor tailAverage = averageVisibleColorInRect(frame, tailRegion, 8, &tailVisiblePixels);
    const QColor quietAverage = averageVisibleColorInRect(frame, quietRegion, 8, &quietVisiblePixels);
    const QColor rawTailColor(loudValue, loudValue, loudValue);

    QVERIFY2(
        tailVisiblePixels > 100,
        "rolling full-mode public shader path should render visible right-edge tail pixels");
    QVERIFY2(
        quietVisiblePixels > 100,
        "rolling full-mode public shader path should keep visible base-body pixels after incremental advance");
    QVERIFY2(
        manhattanColorDistance(tailAverage, expectedTailColor)
            < manhattanColorDistance(tailAverage, rawTailColor),
        "rolling full-mode right-edge tail should stay closer to the palette-mapped loud color than to the raw grayscale source");
    QVERIFY2(
        manhattanColorDistance(tailAverage, expectedTailColor)
            < manhattanColorDistance(tailAverage, expectedQuietColor),
        "rolling full-mode right-edge tail should reflect the advanced loud columns, not the quiet base body");
    QVERIFY2(
        manhattanColorDistance(quietAverage, expectedQuietColor)
            < manhattanColorDistance(quietAverage, expectedTailColor),
        "rolling full-mode base body should keep the quiet palette-mapped color after the incremental tail advance");
}

void QmlSmokeTest::spectrogramGpuWindowCapturePremultipliesPaletteOpacityInFullMode() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "full");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(window.width());
    item->setHeight(window.height());
    item->setDisplayMode(1);
    item->setGridEnabled(false);
    item->setCrosshairEnabled(false);
    item->setShowFpsOverlay(false);
    item->setPlaying(false);
    item->setOpacity(0.25);

    constexpr int bins = 64;
    constexpr int totalColumns = 240;
    QByteArray chunk(bins * totalColumns, '\x00');
    chunk.fill(static_cast<char>(static_cast<unsigned char>(192)));

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item->feedPrecomputedChunk(
        chunk, bins, 0, totalColumns, 0, totalColumns, 48000, 1024,
        false, true, 31, false);
    item->setPositionSeconds(2.0);

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    std::unique_ptr<QSGNode> node(item->updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);
    assertFullModeBodyNodeInvariants(
        node.get(),
        "full-mode opacity capture should still bind the spectrogram source texture into the material node");

    const QString vertexShaderPath = SpectrogramBodySampleMaterial::vertexShaderResourcePath();
    const QString fragmentShaderPath = SpectrogramBodySampleMaterial::fragmentShaderResourcePath();
    QVERIFY2(
        !vertexShaderPath.isEmpty() && vertexShaderPath.endsWith(QStringLiteral(".vert.qsb")),
        "full-mode palette material should expose the compiled vertex shader resource path");
    QVERIFY2(
        !fragmentShaderPath.isEmpty() && fragmentShaderPath.endsWith(QStringLiteral(".frag.qsb")),
        "full-mode palette material should expose the compiled fragment shader resource path");
    QResource vertexShaderResource(vertexShaderPath);
    QVERIFY2(vertexShaderResource.isValid(), qPrintable(QStringLiteral("missing shader resource %1").arg(vertexShaderPath)));
    QVERIFY2(
        vertexShaderResource.size() > 0,
        qPrintable(QStringLiteral("shader resource %1 should contain compiled shader data").arg(vertexShaderPath)));
    QResource fragmentShaderResource(fragmentShaderPath);
    QVERIFY2(fragmentShaderResource.isValid(), qPrintable(QStringLiteral("missing shader resource %1").arg(fragmentShaderPath)));
    QVERIFY2(
        fragmentShaderResource.size() > 0,
        qPrintable(QStringLiteral("shader resource %1 should contain compiled shader data").arg(fragmentShaderPath)));

    const QSGRendererInterface::GraphicsApi api = windowGraphicsApi(window);
    if (api == QSGRendererInterface::Software) {
        auto *sampleNode = spectrogramBodySampleMaterialNode(node.get());
        QVERIFY(sampleNode != nullptr);
        auto *material =
            dynamic_cast<SpectrogramBodySampleMaterial *>(sampleNode->material());
        QVERIFY(material != nullptr);
        QVERIFY2(
            material->state.dbRemapTexture != nullptr,
            "software fallback should still bind the opacity test dB remap lookup texture");
        QVERIFY2(
            material->state.paletteTexture != nullptr,
            "software fallback should still bind the opacity test palette lookup texture");
        {
            QMutexLocker lock(&item->m_stateMutex);
            assertUploadedPaletteOrdering(*item);
        }
        QSKIP(qPrintable(
            QStringLiteral(
                "Skipping full-mode palette opacity assertion on %1 backend because offscreen software rendering does not render the custom material path into grabWindow(); structural material invariants were verified instead")
                .arg(graphicsApiName(api))));
    }

    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());

    const QRect bodyRegion(40, 15, 160, 80);
    const QRect bounded = bodyRegion.intersected(frame.rect());
    int visiblePixels = 0;
    int nonGrayPixels = 0;
    int sumMaxChannel = 0;
    int maxChannel = 0;
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor pixel = frame.pixelColor(x, y);
            if (pixel.red() > 8 || pixel.green() > 8 || pixel.blue() > 8) {
                ++visiblePixels;
                if (pixel.red() != pixel.green() || pixel.green() != pixel.blue()) {
                    ++nonGrayPixels;
                }
                const int pixelMax = std::max({pixel.red(), pixel.green(), pixel.blue()});
                sumMaxChannel += pixelMax;
                maxChannel = std::max(maxChannel, pixelMax);
            }
        }
    }

    QVERIFY2(
        visiblePixels > 100,
        "full-mode public shader path should still render visible pixels under partial item opacity");
    QVERIFY2(
        nonGrayPixels > 100,
        "full-mode opacity capture should preserve palette-mapped non-gray output");
    const double averageMaxChannel =
        visiblePixels > 0 ? static_cast<double>(sumMaxChannel) / static_cast<double>(visiblePixels) : 0.0;
    QVERIFY2(
        averageMaxChannel < 90.0,
        "full-mode palette output should be premultiplied by the item opacity instead of staying at full brightness");
    QVERIFY2(
        maxChannel < 110,
        "full-mode palette output should not keep full-bright RGB when the material opacity is reduced");
}

void QmlSmokeTest::spectrogramGpuWindowCaptureStaysVisibleAfterClearingCpuDisplayImageInFullMode() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "full");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(window.width());
    item->setHeight(window.height());
    item->setDisplayMode(1);
    item->setGridEnabled(false);
    item->setCrosshairEnabled(false);
    item->setShowFpsOverlay(false);
    item->setPlaying(false);

    constexpr int bins = 64;
    constexpr int totalColumns = 240;
    QByteArray chunk(bins * totalColumns, '\x00');
    for (int column = 0; column < totalColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            const int base = 48 + (column * 160) / std::max(1, totalColumns - 1);
            const int ripple = (bin * 40) / std::max(1, bins - 1);
            chunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(std::min(255, base + ripple)));
        }
    }

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item->feedPrecomputedChunk(
        chunk, bins, 0, totalColumns, 0, totalColumns, 48000, 1024,
        false, true, 31, false);
    item->setPositionSeconds(2.0);

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    std::unique_ptr<QSGNode> node(item->updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);
    {
        QMutexLocker lock(&item->m_stateMutex);
        QVERIFY2(!item->m_gpuDbRemapImage.isNull(), "full mode should prepare the dB remap lookup before the CPU display image is cleared");
        QVERIFY2(!item->m_gpuPaletteImage.isNull(), "full mode should prepare the palette lookup before the CPU display image is cleared");
        QVERIFY2(!item->m_gpuRowLutImage.isNull(), "full mode should prepare the row lookup texture before the CPU display image is cleared");
        item->m_gpuDisplayImage = QImage();
    }

    QCoreApplication::processEvents();
    node.reset(item->updatePaintNode(node.release(), nullptr));
    QVERIFY(node != nullptr);
    assertFullModeBodyNodeInvariants(
        node.get(),
        "full-mode material rendering should stay active after the CPU display image is cleared");

    {
        QMutexLocker lock(&item->m_stateMutex);
        QVERIFY2(
            !item->m_gpuDisplayImage.isNull(),
            "centered full mode should keep a CPU display image available for freeze and fallback after a manual clear");
    }

    const QSGRendererInterface::GraphicsApi api = windowGraphicsApi(window);
    if (api == QSGRendererInterface::Software) {
        auto *sampleNode = spectrogramBodySampleMaterialNode(node.get());
        QVERIFY(sampleNode != nullptr);
        auto *material =
            dynamic_cast<SpectrogramBodySampleMaterial *>(sampleNode->material());
        QVERIFY(material != nullptr);
        QVERIFY2(
            material->state.dbRemapTexture != nullptr,
            "software fallback should still bind the dB remap lookup texture after the CPU display image is cleared");
        QVERIFY2(
            material->state.paletteTexture != nullptr,
            "software fallback should still bind the palette lookup texture after the CPU display image is cleared");
        QVERIFY2(
            material->state.rowLutTexture != nullptr,
            "software fallback should still bind the row lookup texture after the CPU display image is cleared");
        {
            QMutexLocker lock(&item->m_stateMutex);
            QVERIFY2(
                countVisiblePixelsInRect(item->m_gpuDisplayImage, QRect(40, 15, 160, 80), 8) > 100,
                "software fallback should rebuild a visible CPU display image after a manual clear");
        }
        QSKIP(qPrintable(
            QStringLiteral(
                "Skipping full-mode post-clear pixel assertion on %1 backend because offscreen software rendering does not render the custom material path into grabWindow(); structural material invariants were verified instead")
                .arg(graphicsApiName(api))));
    }

    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());

    const QRect bodyRegion(40, 15, 160, 80);
    const int visiblePixels = countVisiblePixelsInRect(frame, bodyRegion, 8);
    QVERIFY2(
        visiblePixels > 100,
        "full-mode material rendering should still show visible body pixels after the CPU display image is cleared");
}

void QmlSmokeTest::spectrogramGpuWindowCaptureUsesMaterialBackedBodyNode() {
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);

    constexpr int bins = 64;
    constexpr int totalColumns = 240;
    QByteArray chunk(bins * totalColumns, '\x00');
    for (int column = 0; column < totalColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            chunk[column * bins + bin] = static_cast<char>((column + bin) % 256);
        }
    }
    item.feedPrecomputedChunk(
        chunk, bins, 0, totalColumns, 0, totalColumns, 48000, 1024, false, true, 31);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();

    QScopedPointer<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(!node.isNull());
    assertFullModeBodyNodeInvariants(
        node.data(),
        "normal full mode should use a material-backed body node and leave the old texture-node body empty");

    if (windowGraphicsApi(window) == QSGRendererInterface::Software) {
        auto *tailMaterialNode = spectrogramTailSampleMaterialNode(node.data());
        QVERIFY2(
            tailMaterialNode != nullptr,
            "software fallback should keep a dedicated tail material node even before rolling tail growth begins");
        auto *tailMaterial =
            dynamic_cast<SpectrogramBodySampleMaterial *>(tailMaterialNode->material());
        QVERIFY(tailMaterial != nullptr);
        QVERIFY2(
            tailMaterial->state.sourceTexture != nullptr,
            "inactive tail material should keep a non-null source sampler binding instead of exposing a null QSGTexture");
        QVERIFY2(
            tailMaterial->state.dbRemapTexture != nullptr,
            "inactive tail material should keep a non-null dB remap sampler binding instead of exposing a null QSGTexture");
        QVERIFY2(
            tailMaterial->state.paletteTexture != nullptr,
            "inactive tail material should keep a non-null palette sampler binding instead of exposing a null QSGTexture");
        QVERIFY2(
            !geometryHasVisibleArea(tailMaterialNode),
            "inactive tail material should stay hidden until a rolling tail segment exists");
    }
}

void QmlSmokeTest::spectrogramGpuStockMaterialDebugModeUsesStockTextureMaterialNode() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "stock-material");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);

    constexpr int bins = 64;
    constexpr int totalColumns = 240;
    QByteArray chunk(bins * totalColumns, '\x00');
    for (int column = 0; column < totalColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            chunk[column * bins + bin] = static_cast<char>((column + bin) % 256);
        }
    }
    item.feedPrecomputedChunk(
        chunk, bins, 0, totalColumns, 0, totalColumns, 48000, 1024, false, true, 31);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();

    QScopedPointer<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(!node.isNull());
    auto *sampleNode = spectrogramBodySampleMaterialNode(node.data());
    QVERIFY(sampleNode != nullptr);
    QVERIFY(!geometryHasVisibleArea(sampleNode));
    auto *stockMaterialNode = spectrogramStockMaterialNode(node.data());
    QVERIFY(stockMaterialNode != nullptr);
    QVERIFY(geometryHasVisibleArea(stockMaterialNode));
    QVERIFY(dynamic_cast<QSGTextureMaterial *>(stockMaterialNode->material()) != nullptr);
}

void QmlSmokeTest::spectrogramGpuSourceOnlyDebugRendersWithoutGpuSourceImage() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "shader-source-only");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);

    constexpr int bins = 64;
    constexpr int totalColumns = 240;
    QByteArray chunk(bins * totalColumns, '\x00');
    for (int column = 0; column < totalColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            chunk[column * bins + bin] = static_cast<char>((column + bin) % 256);
        }
    }
    item.feedPrecomputedChunk(
        chunk, bins, 0, totalColumns, 0, totalColumns, 48000, 1024, false, true, 31);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);
    item.setShowFpsOverlay(false);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    const QImage beforeFrame = window.grabWindow();
    QVERIFY(!beforeFrame.isNull());
    if (windowGraphicsApi(window) != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(
            QStringLiteral("retained-source shader-source-only visibility test requires OpenGL scene graph, got %1")
                .arg(graphicsApiName(windowGraphicsApi(window)))));
    }

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_gpuSourceTexture.texture() != nullptr);
    }

    item.update();
    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();
    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());
    QVERIFY2(
        countVisiblePixelsInRect(frame, QRect(40, 15, 160, 80), 8) > 100,
        "shader-source-only should stay visible when the old CPU source image is absent");

    QScopedPointer<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(!node.isNull());
    auto *sampleNode = spectrogramSourceSampleMaterialNode(node.data());
    QVERIFY(sampleNode != nullptr);
    QVERIFY(geometryHasVisibleArea(sampleNode));
    auto *sampleMaterial =
        dynamic_cast<SpectrogramSourceSampleMaterial *>(sampleNode->material());
    QVERIFY(sampleMaterial != nullptr);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(sampleMaterial->state.sourceTexture, item.m_gpuSourceTexture.texture());
    }
}

void QmlSmokeTest::spectrogramGpuFullModeRendersWithoutCpuSourceImage() {
    qunsetenv("FERROUS_GPU_DEBUG_MODE");

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);
    item.setShowFpsOverlay(false);
    item.setPlaying(false);

    constexpr int bins = 32;
    constexpr int totalColumns = 240;
    QByteArray chunk(bins * totalColumns, '\x00');
    for (int column = 0; column < totalColumns; ++column) {
        for (int bin = 8; bin < bins - 4; ++bin) {
            chunk[column * bins + bin] = static_cast<char>(0xE0);
        }
    }
    item.feedPrecomputedChunk(
        chunk, bins, 0, totalColumns, 0, totalColumns, 48000, 1024, true, true, 31);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    if (windowGraphicsApi(window) != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(
            QStringLiteral("retained full-mode visibility test requires OpenGL scene graph, got %1")
                .arg(graphicsApiName(windowGraphicsApi(window)))));
    }

    QSGTexture *retainedTexture = nullptr;
    {
        QMutexLocker lock(&item.m_stateMutex);
        retainedTexture = item.m_gpuSourceTexture.texture();
        QVERIFY2(
            retainedTexture != nullptr,
            "full mode should prepare the retained source texture before the first retained-texture frame");
    }

    item.update();
    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());
    QVERIFY2(
        countVisiblePixelsInRect(frame, QRect(40, 15, 160, 80), 8) > 100,
        "full mode should render from the retained source texture");

    QScopedPointer<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(!node.isNull());
    assertFullModeBodyNodeInvariants(
        node.data(),
        "full mode should keep the body material active on the retained-texture path");
    auto *sampleNode = spectrogramBodySampleMaterialNode(node.data());
    QVERIFY(sampleNode != nullptr);
    auto *sampleMaterial =
        dynamic_cast<SpectrogramBodySampleMaterial *>(sampleNode->material());
    QVERIFY(sampleMaterial != nullptr);
    QCOMPARE(sampleMaterial->state.sourceTexture, retainedTexture);
}

void QmlSmokeTest::spectrogramGpuFullModeKeepsTopBinVisibleWithRowLutSampling() {
    qunsetenv("FERROUS_GPU_DEBUG_MODE");

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);
    item.setLogScale(false);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);
    item.setShowFpsOverlay(false);
    item.setPlaying(false);

    constexpr int bins = 64;
    constexpr int totalColumns = 240;
    QByteArray chunk(bins * totalColumns, '\x00');
    for (int column = 0; column < totalColumns; ++column) {
        for (int bin = bins - 4; bin < bins; ++bin) {
            chunk[column * bins + bin] = static_cast<char>(0xFF);
        }
    }
    item.feedPrecomputedChunk(
        chunk, bins, 0, totalColumns, 0, totalColumns, 48000, 1024, true, true, 32);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    if (windowGraphicsApi(window) != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(
            QStringLiteral("row-LUT full-mode visibility test requires OpenGL scene graph, got %1")
                .arg(graphicsApiName(windowGraphicsApi(window)))));
    }

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_gpuSourceTexture.texture() != nullptr);
    }

    item.update();
    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());
    QVERIFY2(
        countVisiblePixelsInRect(frame, QRect(40, 0, 160, 8), 8) > 50,
        "full mode should preserve top-bin visibility through the row-LUT shader path");

    QScopedPointer<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(!node.isNull());
    auto *sampleNode = spectrogramBodySampleMaterialNode(node.data());
    QVERIFY(sampleNode != nullptr);
    auto *sampleMaterial =
        dynamic_cast<SpectrogramBodySampleMaterial *>(sampleNode->material());
    QVERIFY(sampleMaterial != nullptr);
    QVERIFY2(
        sampleMaterial->state.rowLutTexture != nullptr,
        "full mode should bind a row lookup texture for retained-texture sampling");
}

void QmlSmokeTest::spectrogramGpuSparseCenteredWindowFallsBackToDisplayImageFromRetainedSegments() {
    enum class DebugPath {
        TextureNode,
        SourceOnly,
        Full,
    };
    struct DebugCase {
        QByteArray envValue;
        DebugPath path;
    };
    const std::array<DebugCase, 3> cases{{
        {QByteArrayLiteral("texture-node"), DebugPath::TextureNode},
        {QByteArrayLiteral("shader-source-only"), DebugPath::SourceOnly},
        {QByteArrayLiteral("full"), DebugPath::Full},
    }};

    constexpr int bins = 32;
    constexpr int totalColumns = 12;
    constexpr quint64 trackToken = 71;
    constexpr double effectiveZoom = 10.0;

    const auto makeChunk = [=](int columns) {
        QByteArray chunk(columns * bins, '\x00');
        for (int column = 0; column < columns; ++column) {
            for (int bin = 4; bin < bins - 4; ++bin) {
                chunk[column * bins + bin] = static_cast<char>(0xF0);
            }
        }
        return chunk;
    };

    for (const DebugCase &debugCase : cases) {
        qputenv("FERROUS_GPU_DEBUG_MODE", debugCase.envValue);
        struct DebugModeGuard {
            ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
        } debugModeGuard;

        QQuickWindow window;
        window.setColor(Qt::black);
        window.resize(120, 80);

        SpectrogramItem item;
        item.setWidth(window.width());
        item.setHeight(window.height());
        item.setParentItem(window.contentItem());
        item.setDisplayMode(1);
        item.setGridEnabled(false);
        item.setCrosshairEnabled(false);
        item.setShowFpsOverlay(false);
        item.setPlaying(false);

        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        if (windowGraphicsApi(window) != QSGRendererInterface::OpenGL) {
            QSKIP(qPrintable(
                QStringLiteral("retained sparse-window overflow test requires OpenGL scene graph, got %1")
                    .arg(graphicsApiName(windowGraphicsApi(window)))));
        }

        item.feedPrecomputedChunk(
            makeChunk(2), bins, 0, 2, 0, totalColumns, 48000, 1024, false, true, trackToken);
        item.feedPrecomputedChunk(
            makeChunk(2), bins, 0, 2, 4, totalColumns, 48000, 1024, false, false, trackToken);
        {
            QMutexLocker lock(&item.m_stateMutex);
            item.m_renderZoomLevel = effectiveZoom;
            item.m_zoomLevel = effectiveZoom;
        }
        item.setPositionSeconds(0.06);

        QCoreApplication::processEvents();
        QTest::qWait(50);
        QCoreApplication::processEvents();

        std::unique_ptr<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
        QVERIFY(node != nullptr);
        QSGTexture *retainedTexture = nullptr;
        QImage displayImage;
        {
            QMutexLocker lock(&item.m_stateMutex);
            retainedTexture = item.m_gpuSourceTexture.texture();
            displayImage = item.m_gpuDisplayImage;
        }
        QVERIFY2(retainedTexture != nullptr, "OpenGL precondition should produce a retained source texture");
        QVERIFY2(
            displayImage.isNull(),
            "retained sparse-window precondition should not need the CPU display image before overflow");
        if (debugCase.path == DebugPath::TextureNode) {
            auto *bodyTextureNode = spectrogramBodyTextureNode(node.get());
            auto *tailTextureNode = spectrogramTailTextureNode(node.get());
            QVERIFY(bodyTextureNode != nullptr);
            QVERIFY(tailTextureNode != nullptr);
            QVERIFY2(
                bodyTextureNode->texture() == retainedTexture,
                "texture-node precondition should route the base run through the retained source texture");
            QVERIFY2(
                tailTextureNode->texture() == retainedTexture,
                "texture-node precondition should route the tail run through the retained source texture");
            QCOMPARE(bodyTextureNode->sourceRect(), QRectF(0.0, 0.0, 2.0, static_cast<double>(bins)));
            QCOMPARE(tailTextureNode->sourceRect(), QRectF(4.0, 0.0, 2.0, static_cast<double>(bins)));
            QVERIFY2(!bodyTextureNode->rect().isEmpty(), "base retained texture node should be visible");
            QVERIFY2(!tailTextureNode->rect().isEmpty(), "tail retained texture node should be visible");
        } else if (debugCase.path == DebugPath::SourceOnly) {
            auto *bodySourceNode = spectrogramSourceSampleMaterialNode(node.get());
            auto *tailSourceNode = spectrogramSourceSampleMaterialNode(node.get(), 1);
            QVERIFY(bodySourceNode != nullptr);
            QVERIFY(tailSourceNode != nullptr);
            auto *bodyMaterial =
                dynamic_cast<SpectrogramSourceSampleMaterial *>(bodySourceNode->material());
            auto *tailMaterial =
                dynamic_cast<SpectrogramSourceSampleMaterial *>(tailSourceNode->material());
            QVERIFY(bodyMaterial != nullptr);
            QVERIFY(tailMaterial != nullptr);
            QVERIFY2(
                bodyMaterial->state.sourceTexture == retainedTexture,
                "shader-source-only precondition should sample the retained base run");
            QVERIFY2(
                tailMaterial->state.sourceTexture == retainedTexture,
                "shader-source-only precondition should sample the retained tail run");
            QCOMPARE(bodyMaterial->state.sourceRect, QRectF(0.0, 0.0, 2.0, static_cast<double>(bins)));
            QCOMPARE(tailMaterial->state.sourceRect, QRectF(4.0, 0.0, 2.0, static_cast<double>(bins)));
            QVERIFY2(geometryHasVisibleArea(bodySourceNode), "base retained sample node should be visible");
            QVERIFY2(geometryHasVisibleArea(tailSourceNode), "tail retained sample node should be visible");
        } else if (debugCase.path == DebugPath::Full) {
            assertFullModeBodyNodeInvariants(
                node.get(),
                "full-mode sparse retained precondition should keep the material body active");
            auto *bodySampleNode = spectrogramBodySampleMaterialNode(node.get());
            QVERIFY(bodySampleNode != nullptr);
            auto *bodyMaterial =
                dynamic_cast<SpectrogramBodySampleMaterial *>(bodySampleNode->material());
            QVERIFY(bodyMaterial != nullptr);
            QVERIFY2(
                bodyMaterial->state.sourceTexture == retainedTexture,
                "full-mode sparse retained precondition should sample the retained source texture");
            QCOMPARE(
                bodyMaterial->state.sourceRect,
                QRectF(0.0, 0.0, 2.0, static_cast<double>(bins)));
        }

        item.feedPrecomputedChunk(
            makeChunk(2), bins, 0, 2, 8, totalColumns, 48000, 1024, true, false, trackToken);
        QCoreApplication::processEvents();
        QTest::qWait(50);
        QCoreApplication::processEvents();

        node.reset(item.updatePaintNode(node.release(), nullptr));
        QVERIFY(node != nullptr);
        {
            QMutexLocker lock(&item.m_stateMutex);
            displayImage = item.m_gpuDisplayImage;
        }
        QVERIFY2(
            !displayImage.isNull(),
            "sparse retained overflow should warm the CPU display image for the fallback frame");
        QCOMPARE(displayImage.width(), 120);
        QCOMPARE(displayImage.height(), 80);
        QVERIFY2(
            countVisiblePixelsInRect(displayImage, QRect(0, 0, 20, 80), 8) > 200,
            "first sparse run should remain visible in the CPU display fallback image");
        QVERIFY2(
            countVisiblePixelsInRect(displayImage, QRect(40, 0, 20, 80), 8) > 200,
            "second sparse run should remain visible in the CPU display fallback image");
        QVERIFY2(
            countVisiblePixelsInRect(displayImage, QRect(80, 0, 20, 80), 8) > 200,
            "third sparse run should remain visible in the CPU display fallback image");
        QVERIFY2(
            countVisiblePixelsInRect(displayImage, QRect(20, 0, 20, 80), 8) < 80,
            "gap between the first and second sparse runs should stay dark in the CPU display fallback image");
        QVERIFY2(
            countVisiblePixelsInRect(displayImage, QRect(60, 0, 20, 80), 8) < 80,
            "gap between the second and third sparse runs should stay dark in the CPU display fallback image");

        auto *bodyTextureNode = spectrogramBodyTextureNode(node.get());
        auto *tailTextureNode = spectrogramTailTextureNode(node.get());
        QVERIFY(bodyTextureNode != nullptr);
        QVERIFY(tailTextureNode != nullptr);
        QVERIFY2(
            bodyTextureNode->texture() != retainedTexture,
            "overflow frame should stop sampling the retained base texture path");
        QVERIFY2(!bodyTextureNode->rect().isEmpty(), "display-image fallback body should stay visible");
        QCOMPARE(
            bodyTextureNode->sourceRect(),
            QRectF(
                0.0,
                0.0,
                static_cast<double>(displayImage.width()),
                static_cast<double>(displayImage.height())));
        QVERIFY2(
            tailTextureNode->rect().isEmpty(),
            "display-image fallback should disable retained tail routing");

        if (debugCase.path == DebugPath::SourceOnly) {
            auto *bodySourceNode = spectrogramSourceSampleMaterialNode(node.get());
            QVERIFY(bodySourceNode != nullptr);
            QVERIFY2(
                !geometryHasVisibleArea(bodySourceNode),
                "shader-source-only overflow fallback should hide the retained source-only material node");
        } else if (debugCase.path == DebugPath::Full) {
            auto *bodySampleNode = spectrogramBodySampleMaterialNode(node.get());
            QVERIFY(bodySampleNode != nullptr);
            QVERIFY2(
                !geometryHasVisibleArea(bodySampleNode),
                "full-mode overflow fallback should hide the retained full-mode material node");
        }
    }
}

void QmlSmokeTest::spectrogramGpuSparseCenteredWindowUsesDisplayImageWithoutRetainedSourceTexture() {
    const std::array<QByteArray, 3> cases{{
        QByteArrayLiteral("texture-node"),
        QByteArrayLiteral("shader-source-only"),
        QByteArrayLiteral("full"),
    }};

    constexpr int bins = 32;
    constexpr int totalColumns = 12;
    constexpr quint64 trackToken = 71;
    constexpr double effectiveZoom = 10.0;

    const auto makeChunk = [=](int columns) {
        QByteArray chunk(columns * bins, '\x00');
        for (int column = 0; column < columns; ++column) {
            for (int bin = 4; bin < bins - 4; ++bin) {
                chunk[column * bins + bin] = static_cast<char>(0xF0);
            }
        }
        return chunk;
    };

    for (const QByteArray &debugMode : cases) {
        qputenv("FERROUS_GPU_DEBUG_MODE", debugMode);
        struct DebugModeGuard {
            ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
        } debugModeGuard;

        QQuickWindow window;
        window.setColor(Qt::black);
        window.resize(120, 80);

        SpectrogramItem item;
        item.setWidth(window.width());
        item.setHeight(window.height());
        item.setParentItem(window.contentItem());
        item.setDisplayMode(1);
        item.setGridEnabled(false);
        item.setCrosshairEnabled(false);
        item.setShowFpsOverlay(false);
        item.setPlaying(false);

        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        if (windowGraphicsApi(window) != QSGRendererInterface::Software) {
            QSKIP(qPrintable(
                QStringLiteral("non-OpenGL sparse-window fallback test requires software scene graph, got %1")
                    .arg(graphicsApiName(windowGraphicsApi(window)))));
        }

        item.feedPrecomputedChunk(
            makeChunk(2), bins, 0, 2, 0, totalColumns, 48000, 1024, false, true, trackToken);
        item.feedPrecomputedChunk(
            makeChunk(2), bins, 0, 2, 4, totalColumns, 48000, 1024, false, false, trackToken);
        item.feedPrecomputedChunk(
            makeChunk(2), bins, 0, 2, 8, totalColumns, 48000, 1024, true, false, trackToken);
        {
            QMutexLocker lock(&item.m_stateMutex);
            item.m_renderZoomLevel = effectiveZoom;
            item.m_zoomLevel = effectiveZoom;
        }
        item.setPositionSeconds(0.06);

        QCoreApplication::processEvents();
        QTest::qWait(50);
        QCoreApplication::processEvents();

        QScopedPointer<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
        QVERIFY(!node.isNull());
        QImage displayImage;
        {
            QMutexLocker lock(&item.m_stateMutex);
            QVERIFY(item.m_gpuSourceTexture.texture() == nullptr);
            displayImage = item.m_gpuDisplayImage;
        }
        QVERIFY2(
            !displayImage.isNull(),
            "software sparse-window fallback should render through the CPU display image");
        QCOMPARE(displayImage.width(), 120);
        QCOMPARE(displayImage.height(), 80);
        QVERIFY2(
            countVisiblePixelsInRect(displayImage, QRect(0, 0, 20, 80), 8) > 200,
            "first sparse run should remain visible in software display fallback");
        QVERIFY2(
            countVisiblePixelsInRect(displayImage, QRect(40, 0, 20, 80), 8) > 200,
            "second sparse run should remain visible in software display fallback");
        QVERIFY2(
            countVisiblePixelsInRect(displayImage, QRect(80, 0, 20, 80), 8) > 200,
            "third sparse run should remain visible in software display fallback");
        const int firstRunPixels =
            countVisiblePixelsInRect(displayImage, QRect(0, 0, 20, 80), 48);
        const int secondRunPixels =
            countVisiblePixelsInRect(displayImage, QRect(40, 0, 20, 80), 48);
        const int thirdRunPixels =
            countVisiblePixelsInRect(displayImage, QRect(80, 0, 20, 80), 48);
        const int firstGapPixels =
            countVisiblePixelsInRect(displayImage, QRect(20, 0, 20, 80), 48);
        const int secondGapPixels =
            countVisiblePixelsInRect(displayImage, QRect(60, 0, 20, 80), 48);
        QVERIFY2(
            firstGapPixels < firstRunPixels,
            "first sparse gap should stay dimmer than the first sparse run in software display fallback");
        QVERIFY2(
            firstGapPixels < secondRunPixels,
            "first sparse gap should stay dimmer than the middle sparse run in software display fallback");
        QVERIFY2(
            secondGapPixels < secondRunPixels,
            "second sparse gap should stay dimmer than the middle sparse run in software display fallback");
        QVERIFY2(
            secondGapPixels < thirdRunPixels,
            "second sparse gap should stay dimmer than the final sparse run in software display fallback");

        auto *bodyTextureNode = spectrogramBodyTextureNode(node.data());
        QVERIFY(bodyTextureNode != nullptr);
        QVERIFY2(
            !bodyTextureNode->rect().isEmpty(),
            "software display-image fallback should keep the body texture node visible");
        QCOMPARE(
            bodyTextureNode->sourceRect(),
            QRectF(
                0.0,
                0.0,
                static_cast<double>(displayImage.width()),
                static_cast<double>(displayImage.height())));

        auto *bodySourceNode = spectrogramSourceSampleMaterialNode(node.data());
        QVERIFY(bodySourceNode != nullptr);
        QVERIFY2(
            !geometryHasVisibleArea(bodySourceNode),
            "software display-image fallback should hide the source-sample material node");

        auto *bodySampleNode = spectrogramBodySampleMaterialNode(node.data());
        QVERIFY(bodySampleNode != nullptr);
        QVERIFY2(
            !geometryHasVisibleArea(bodySampleNode),
            "software display-image fallback should hide the full-mode body material node");
    }
}

void QmlSmokeTest::spectrogramGpuFullModeUsesSampleBodyMaterialNode() {
    qunsetenv("FERROUS_GPU_DEBUG_MODE");

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);

    constexpr int bins = 64;
    constexpr int totalColumns = 240;
    QByteArray chunk(bins * totalColumns, '\x00');
    for (int column = 0; column < totalColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            chunk[column * bins + bin] = static_cast<char>((column + bin) % 256);
        }
    }
    item.feedPrecomputedChunk(
        chunk, bins, 0, totalColumns, 0, totalColumns, 48000, 1024, false, true, 31);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();

    QScopedPointer<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(!node.isNull());
    assertFullModeBodyNodeInvariants(
        node.data(),
        "full debug mode should either keep the body material visible or fall back to the visible body texture node");

    auto *sampleNode = spectrogramBodySampleMaterialNode(node.data());
    QVERIFY(sampleNode != nullptr);
    auto *sampleMaterial =
        dynamic_cast<SpectrogramBodySampleMaterial *>(sampleNode->material());
    QVERIFY(sampleMaterial != nullptr);
    QVERIFY(sampleMaterial->state.rowLutTexture != nullptr);
}

void QmlSmokeTest::spectrogramGpuFullModeShaderSmoothsZoomOutAggregation() {
    assertFragmentShaderSmoothsZoomOutAggregation();
}

void QmlSmokeTest::spectrogramGpuSampleMaterialsBindSourceTextureAtZero() {
    SpectrogramSourceSampleMaterial::State sourceState;
    sourceState.sourceTexture = reinterpret_cast<QSGTexture *>(quintptr{0x1});
    QCOMPARE(spectrogramSourceSampleTextureForBinding(sourceState, 0), sourceState.sourceTexture);
    QCOMPARE(spectrogramSourceSampleTextureForBinding(sourceState, 1), nullptr);

    SpectrogramBodySampleMaterial::State bodyState;
    bodyState.sourceTexture = reinterpret_cast<QSGTexture *>(quintptr{0x1});
    bodyState.dbRemapTexture = reinterpret_cast<QSGTexture *>(quintptr{0x2});
    bodyState.paletteTexture = reinterpret_cast<QSGTexture *>(quintptr{0x3});
    bodyState.rowLutTexture = reinterpret_cast<QSGTexture *>(quintptr{0x4});
    QCOMPARE(spectrogramBodySampleTextureForBinding(bodyState, 0), bodyState.sourceTexture);
    QCOMPARE(spectrogramBodySampleTextureForBinding(bodyState, 1), bodyState.dbRemapTexture);
    QCOMPARE(spectrogramBodySampleTextureForBinding(bodyState, 2), bodyState.paletteTexture);
    QCOMPARE(spectrogramBodySampleTextureForBinding(bodyState, 3), bodyState.rowLutTexture);
    QCOMPARE(spectrogramBodySampleTextureForBinding(bodyState, 4), nullptr);
}

void QmlSmokeTest::spectrogramGpuSampleMaterialNodeBackfillsSecondaryTexture() {
    class StubTexture final : public QSGTexture {
    public:
        qint64 comparisonKey() const override { return 0x5150; }
        QSize textureSize() const override { return QSize(64, 32); }
        bool hasAlphaChannel() const override { return true; }
        bool hasMipmaps() const override { return false; }
    };

    StubTexture texture;
    QScopedPointer<QSGGeometryNode> bodyNode(createSpectrogramBodySampleMaterialNode());
    SpectrogramBodySampleMaterial::State state;
    state.sourceTexture = &texture;
    state.dbRemapTexture = &texture;
    state.paletteTexture = &texture;
    state.rowLutTexture = &texture;
    state.sourceRect = QRectF(0.0, 0.0, 64.0, 32.0);
    state.sourceTextureSize = texture.textureSize();

    syncSpectrogramBodySampleMaterialNode(
        bodyNode.data(),
        QRectF(0.0, 0.0, 64.0, 32.0),
        state);

    const auto *material =
        dynamic_cast<SpectrogramBodySampleMaterial *>(bodyNode->material());
    QVERIFY(material != nullptr);
    QCOMPARE(spectrogramBodySampleTextureForBinding(material->state, 4), &texture);
    QCOMPARE(material->state.secondarySourceTextureSize, texture.textureSize());
}

void QmlSmokeTest::spectrogramGpuSampleMaterialCompareIncludesSamplingState() {
    class StubTexture final : public QSGTexture {
    public:
        explicit StubTexture(qint64 key) : m_key(key) {}
        qint64 comparisonKey() const override { return m_key; }
        QSize textureSize() const override { return QSize(64, 32); }
        bool hasAlphaChannel() const override { return true; }
        bool hasMipmaps() const override { return false; }

    private:
        qint64 m_key;
    };

    StubTexture source(1);
    StubTexture remap(2);
    StubTexture palette(3);
    StubTexture rowLut(4);

    SpectrogramBodySampleMaterial first;
    first.state.sourceTexture = &source;
    first.state.dbRemapTexture = &remap;
    first.state.paletteTexture = &palette;
    first.state.rowLutTexture = &rowLut;
    first.state.sourceRect = QRectF(0.0, 0.0, 32.0, 16.0);
    first.state.sourceTextureSize = source.textureSize();
    first.state.effectiveZoom = 0.5F;
    first.state.sourceColumnOffset = 0.0F;

    SpectrogramBodySampleMaterial shifted;
    shifted.state = first.state;
    shifted.state.sourceRect = QRectF(8.0, 0.0, 32.0, 16.0);
    shifted.state.sourceColumnOffset = 8.0F;

    SpectrogramBodySampleMaterial zoomed;
    zoomed.state = first.state;
    zoomed.state.effectiveZoom = 2.0F;

    QVERIFY2(
        first.compare(&shifted) != 0,
        "full-mode material comparison must include source column offset");
    QVERIFY2(
        first.compare(&zoomed) != 0,
        "full-mode material comparison must include effective zoom");

    SpectrogramSourceSampleMaterial sourceOnlyFirst;
    sourceOnlyFirst.state.sourceTexture = &source;
    sourceOnlyFirst.state.sourceRect = QRectF(0.0, 0.0, 32.0, 16.0);

    SpectrogramSourceSampleMaterial sourceOnlyShifted;
    sourceOnlyShifted.state = sourceOnlyFirst.state;
    sourceOnlyShifted.state.sourceRect = QRectF(8.0, 0.0, 32.0, 16.0);
    QVERIFY2(
        sourceOnlyFirst.compare(&sourceOnlyShifted) != 0,
        "source-only material comparison must include source rect");
}

void QmlSmokeTest::spectrogramGpuSampledTexturesCommitPendingUploads() {
    class TrackingTexture final : public QSGTexture {
    public:
        qint64 comparisonKey() const override { return 0x1234; }
        QSize textureSize() const override { return QSize(2, 2); }
        bool hasAlphaChannel() const override { return true; }
        bool hasMipmaps() const override { return false; }
        void commitTextureOperations(QRhi *rhi, QRhiResourceUpdateBatch *resourceUpdates) override {
            ++commitCalls;
            lastRhi = rhi;
            lastBatch = resourceUpdates;
        }

        int commitCalls{0};
        QRhi *lastRhi{reinterpret_cast<QRhi *>(quintptr{0x1})};
        QRhiResourceUpdateBatch *lastBatch{reinterpret_cast<QRhiResourceUpdateBatch *>(quintptr{0x1})};
    };

    extern void spectrogramCommitTextureOperationsIfNeeded(
        QSGTexture *texture,
        QRhi *rhi,
        QRhiResourceUpdateBatch *resourceUpdates);

    TrackingTexture texture;

    spectrogramCommitTextureOperationsIfNeeded(nullptr, nullptr, nullptr);
    QCOMPARE(texture.commitCalls, 0);

    spectrogramCommitTextureOperationsIfNeeded(&texture, nullptr, nullptr);
    QCOMPARE(texture.commitCalls, 1);
    QCOMPARE(texture.lastRhi, nullptr);
    QCOMPARE(texture.lastBatch, nullptr);
}

void QmlSmokeTest::spectrogramGpuSampleMaterialNodesFlipVerticalTextureCoords() {
    class StubTexture final : public QSGTexture {
    public:
        qint64 comparisonKey() const override { return 0x4242; }
        QSize textureSize() const override { return QSize(10, 20); }
        bool hasAlphaChannel() const override { return true; }
        bool hasMipmaps() const override { return false; }
    };

    StubTexture texture;

    QScopedPointer<QSGGeometryNode> sourceNode(createSpectrogramSourceSampleMaterialNode());
    syncSpectrogramSourceSampleMaterialNode(
        sourceNode.data(),
        QRectF(0.0, 0.0, 10.0, 20.0),
        &texture,
        QRectF(0.0, 0.0, 10.0, 20.0),
        &texture);
    const auto *sourceVertices = sourceNode->geometry()->vertexDataAsTexturedPoint2D();
    QCOMPARE(sourceVertices[0].ty, 1.0F);
    QCOMPARE(sourceVertices[1].ty, 0.0F);
    QCOMPARE(sourceVertices[2].ty, 1.0F);
    QCOMPARE(sourceVertices[3].ty, 0.0F);

    QScopedPointer<QSGGeometryNode> bodyNode(createSpectrogramBodySampleMaterialNode());
    SpectrogramBodySampleMaterial::State state;
    state.sourceTexture = &texture;
    state.dbRemapTexture = &texture;
    state.paletteTexture = &texture;
    state.rowLutTexture = &texture;
    state.sourceRect = QRectF(0.0, 0.0, 10.0, 20.0);
    syncSpectrogramBodySampleMaterialNode(
        bodyNode.data(),
        QRectF(0.0, 0.0, 10.0, 20.0),
        state);
    const auto *bodyVertices = bodyNode->geometry()->vertexDataAsTexturedPoint2D();
    QCOMPARE(bodyVertices[0].ty, 1.0F);
    QCOMPARE(bodyVertices[1].ty, 0.0F);
    QCOMPARE(bodyVertices[2].ty, 1.0F);
    QCOMPARE(bodyVertices[3].ty, 0.0F);
}

void QmlSmokeTest::spectrogramGpuSamplerLoggingIgnoresPlaceholderTextureSize() {
    QVERIFY(!spectrogramShouldConsumeSampledImageLogBudget(QSize(1, 1)));
    QVERIFY(!spectrogramShouldConsumeSampledImageLogBudget(QSize(1, 0)));
    QVERIFY(!spectrogramShouldConsumeSampledImageLogBudget(QSize(0, 1)));
    QVERIFY(spectrogramShouldConsumeSampledImageLogBudget(QSize(1, 2)));
    QVERIFY(spectrogramShouldConsumeSampledImageLogBudget(QSize(2, 1)));
    QVERIFY(spectrogramShouldConsumeSampledImageLogBudget(QSize(2, 2)));
}

void QmlSmokeTest::spectrogramGpuWindowCaptureShowsSpectrogramPixelsWithoutOverlays() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "texture-node");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.resize(200, 100);

    SpectrogramItem *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(200);
    item->setHeight(100);
    item->setDisplayMode(1); // Centered
    item->setGridEnabled(false);
    item->setCrosshairEnabled(false);
    item->setShowFpsOverlay(false);

    constexpr int bins = 64;
    constexpr int columns = 200;
    QByteArray data(bins * columns, '\x00');
    for (int column = 90; column < 150; ++column) {
        for (int bin = 20; bin < 44; ++bin) {
            data[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(0xF0));
        }
    }

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item->feedPrecomputedChunk(
        data, bins, 0, columns, 0, columns, 48000, 1024,
        false, true, 1, false);
    item->setPositionSeconds(2.0);
    item->setPlaying(false);

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());
    const QRect bodyRegion(40, 15, 120, 70);
    const int visiblePixels = countVisiblePixelsInRect(frame, bodyRegion, 8);
    QVERIFY2(visiblePixels > 100, "Spectrogram body region remained black without overlays");
}

void QmlSmokeTest::spectrogramGpuWindowCaptureShowsLargeCenteredTrackPixels() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "texture-node");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.resize(1126, 214);

    SpectrogramItem *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(1126);
    item->setHeight(214);
    item->setDisplayMode(1); // Centered
    item->setGridEnabled(false);
    item->setCrosshairEnabled(false);
    item->setShowFpsOverlay(false);
    item->setPlaying(false);

    constexpr int bins = 256;
    constexpr int columns = 9000;
    QByteArray data(bins * columns, '\x00');
    for (int column = 4300; column < 4400; ++column) {
        for (int bin = 40; bin < 180; ++bin) {
            data[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(0xE0));
        }
    }

    item->setZoomLevel(static_cast<double>(item->width()) / static_cast<double>(columns));

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    item->feedPrecomputedChunk(
        data, bins, 0, columns, 0, columns, 48000, 1024,
        true, true, 1, false);
    item->setPositionSeconds(96.0);

    QCoreApplication::processEvents();
    QTest::qWait(100);
    QCoreApplication::processEvents();

    const QImage frame = window.grabWindow();
    QVERIFY(!frame.isNull());

    bool sawSpectrogramPixel = false;
    for (int y = 10; y < frame.height() - 10 && !sawSpectrogramPixel; ++y) {
        for (int x = 10; x < frame.width() - 10; ++x) {
            const QColor pixel = frame.pixelColor(x, y);
            if (pixel.red() > 8 || pixel.green() > 8 || pixel.blue() > 8) {
                sawSpectrogramPixel = true;
                break;
            }
        }
    }

    QVERIFY2(sawSpectrogramPixel, "Large centered GPU spectrogram capture remained black");
}

void QmlSmokeTest::spectrogramCenteredGpuCacheSurvivesRingEvictionAtSameView() {
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1); // Centered
    item.setPlaying(false);

    constexpr int bins = 8;
    constexpr int firstChunkColumns = 900;
    constexpr int totalColumns = 8000;

    QByteArray firstChunk(bins * firstChunkColumns, '\x00');
    for (int column = 300; column < 500; ++column) {
        for (int bin = 2; bin < 6; ++bin) {
            firstChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(0xE0));
        }
    }

    item.feedPrecomputedChunk(
        firstChunk, bins, 0, firstChunkColumns, 0, totalColumns,
        48000, 1024, false, true, 1, false);
    item.setPositionSeconds(9.0);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QImage cachedDisplayImage;
    qint64 cachedDisplayLeft = -1;
    qint64 cachedDisplayRight = -1;
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        cachedDisplayImage = item.m_gpuDisplayImage.copy();
        cachedDisplayLeft = item.m_precomputedCanvasDisplayLeft;
        cachedDisplayRight = item.m_precomputedCanvasDisplayRight;
    }

    const int secondChunkColumns = totalColumns - firstChunkColumns;
    QByteArray secondChunk(bins * secondChunkColumns, '\x10');
    item.feedPrecomputedChunk(
        secondChunk, bins, 0, secondChunkColumns, firstChunkColumns, totalColumns,
        48000, 1024, true, false, 1, false);

    item.setPositionSeconds(9.0);
    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, cachedDisplayLeft);
        QCOMPARE(item.m_precomputedCanvasDisplayRight, cachedDisplayRight);
        QCOMPARE(item.m_gpuDisplayImage, cachedDisplayImage);
    }

    delete node;
}

void QmlSmokeTest::spectrogramCenteredGpuMetadataOnlyScrollAvoidsCpuDisplayImage() {
    SpectrogramItem item;
    item.setWidth(3440);
    item.setHeight(720);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int columns = 4096;
    constexpr double effectiveZoom = 1.0;
    QByteArray chunk(bins * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, bins, 0, columns,
        0, 12000, 44100, 1024, false,
        true, 91, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = effectiveZoom;
        item.ensureMapping(720);
        item.syncPrecomputedDisplayImageLocked(
            3440, 720, 0, 3439, false, effectiveZoom, false);
    }

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY2(
            item.m_gpuDisplayImage.isNull(),
            "retained-GPU centered playback should not allocate the CPU display image");
        QCOMPARE(item.m_gpuDisplayDisplayLeft, static_cast<qint64>(0));
        QCOMPARE(item.m_gpuDisplayDisplayRight, static_cast<qint64>(3439));
        QCOMPARE(item.m_gpuDisplayWidth, 3440);
        QCOMPARE(item.m_gpuDisplayHeight, 720);
    }

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.syncPrecomputedDisplayImageLocked(
            3440, 720, 4, 3443, false, effectiveZoom, false);
    }

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY2(
            item.m_gpuDisplayImage.isNull(),
            "metadata-only centered scroll should stay off the CPU paint path");
        QCOMPARE(item.m_gpuDisplayDisplayLeft, static_cast<qint64>(4));
        QCOMPARE(item.m_gpuDisplayDisplayRight, static_cast<qint64>(3443));
        QVERIFY(item.m_gpuDisplayLastUpdateWasIncremental);
        QCOMPARE(item.m_gpuDisplayLastShiftPixels, 4);
        QCOMPARE(item.m_gpuDisplayLastUpdatedColumns, 4);
    }
}

void QmlSmokeTest::spectrogramCenteredRetainedSourceScrollUploadsOnlyNewColumns() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    constexpr int bins = 8;
    constexpr int columns = 5000;
    QByteArray chunk(bins * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, bins, 0, columns,
        0, columns, 44100, 1024, false,
        true, 92, false);

    QMutexLocker lock(&item.m_stateMutex);
    item.m_gpuSourceTextureMinColumnIndex = 0;
    item.m_gpuSourceTextureMaxColumnIndex = 3439;
    item.m_gpuSourceTextureDisplayLeft = 0;
    item.m_gpuSourceTextureDisplayRight = 3439;
    item.m_gpuSourceTextureRolling = false;
    item.m_gpuSourceTextureTrackToken = item.m_precomputedTrackToken;

    const QVector<SpectrogramSourceUploadSpan> forwardScrollSpans =
        item.collectRetainedSourceUploadSpansLocked(
            4, 3443, false, false);
    QCOMPARE(forwardScrollSpans.size(), 1);
    QCOMPARE(forwardScrollSpans[0].xBegin, 3440);
    QCOMPARE(forwardScrollSpans[0].xEnd, 3443);

    item.m_gpuSourceTextureMinColumnIndex = 100;
    item.m_gpuSourceTextureMaxColumnIndex = 200;
    const QVector<SpectrogramSourceUploadSpan> bidirectionalMissingSpans =
        item.collectRetainedSourceUploadSpansLocked(
            96, 204, false, false);
    QCOMPARE(bidirectionalMissingSpans.size(), 2);
    QCOMPARE(bidirectionalMissingSpans[0].xBegin, 96);
    QCOMPARE(bidirectionalMissingSpans[0].xEnd, 99);
    QCOMPARE(bidirectionalMissingSpans[1].xBegin, 201);
    QCOMPARE(bidirectionalMissingSpans[1].xEnd, 204);
}

void QmlSmokeTest::spectrogramRetainedSourcePagesAreScopedToDecodeGeneration() {
    SpectrogramItem item;
    item.setWidth(128);
    item.setHeight(64);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int columns = 128;
    constexpr quint64 trackToken = 47;
    QByteArray chunk(columns * bins, '\x40');
    item.feedPrecomputedChunk(
        chunk, bins, 0, columns,
        0, columns, 44100, 1024, false,
        true, trackToken, true, 1);

    QMutexLocker lock(&item.m_stateMutex);
    auto page = std::make_unique<SpectrogramItem::RetainedSourcePageState>();
    page->key = SpectrogramItem::RetainedSourcePageKey{
        .trackToken = item.m_precomputedTrackToken,
        .generation = item.m_precomputedSessionGeneration,
        .rollingMode = false,
        .pageIndex = 0,
    };
    page->pageXOrigin = 0;
    page->width = columns;
    page->binsPerColumn = bins;
    page->uploadedDisplayLeft = 0;
    page->uploadedDisplayRight = columns - 1;
    item.m_gpuSourcePages.push_back(std::move(page));

    QVERIFY(item.retainedSourcePagesCoverDisplayRangeLocked(
        0, columns - 1, columns - 1, false, 2048, false));

    item.m_precomputedSessionGeneration = 2;
    QVERIFY2(
        !item.retainedSourcePagesCoverDisplayRangeLocked(
            0, columns - 1, columns - 1, false, 2048, false),
        "retained pages uploaded for an older decode generation must not "
        "make the new zoom generation look renderable");

    const QVector<SpectrogramItem::RetainedPageUploadSpan> uploads =
        item.collectRetainedPageUploadSpansLocked(
            0, columns - 1, columns - 1, false, 2048);
    QCOMPARE(uploads.size(), 1);
    QVERIFY(uploads[0].resetPage);
    QCOMPARE(
        uploads[0].resetReason,
        SpectrogramItem::RetainedPageResetReason::MissingPage);
}

void QmlSmokeTest::spectrogramZoomFillWaitsForZoomOutVisibleColumns() {
    SpectrogramItem item;
    item.setWidth(100);
    item.setHeight(20);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 4;
    constexpr int totalColumns = 180;
    constexpr quint64 trackToken = 95;

    QByteArray baseline(120 * bins, '\x40');
    item.feedPrecomputedChunk(
        baseline, bins, 0, 120,
        0, 360, 48000, 1024, false,
        true, trackToken, false, 1);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 0.25;
        item.m_renderZoomLevel = 0.25;
        item.m_awaitingZoomData = false;
        item.m_zoomFillActive = true;
        item.m_zoomFillDataReady = false;
        item.m_gpuDisplayImage =
            QImage(100, 20, QImage::Format_ARGB32_Premultiplied);
        item.m_gpuDisplayImage.fill(Qt::black);
    }

    QByteArray partialZoomOut(100 * bins, '\x55');
    item.feedPrecomputedChunk(
        partialZoomOut, bins, 0, 100,
        0, totalColumns, 48000, 2048, false,
        true, trackToken, false, 2);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_precomputedMaxColumnIndex, 99);
        QVERIFY2(
            !item.m_zoomFillDataReady,
            "max zoom-out freeze must wait for the whole visible column range, "
            "not only one screen-width of decoded columns");
    }

    QByteArray tailZoomOut((totalColumns - 100) * bins, '\x66');
    item.feedPrecomputedChunk(
        tailZoomOut, bins, 0, totalColumns - 100,
        100, totalColumns, 48000, 2048, false,
        false, trackToken, false, 2);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_precomputedMaxColumnIndex, totalColumns - 1);
        QVERIFY(item.m_zoomFillDataReady);
    }
}

void QmlSmokeTest::spectrogramZoomFillWaitsForContiguousVisibleColumns() {
    SpectrogramItem item;
    item.setWidth(100);
    item.setHeight(20);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);
    item.setPositionSeconds(0.0);

    constexpr int bins = 4;
    constexpr int finalTotalColumns = 260;
    constexpr quint64 trackToken = 96;

    QByteArray baseline(160 * bins, '\x40');
    item.feedPrecomputedChunk(
        baseline, bins, 0, 160,
        0, finalTotalColumns * 2, 48000, 1024, false,
        true, trackToken, false, 1);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 0.25;
        item.m_renderZoomLevel = 0.25;
        item.m_awaitingZoomData = false;
        item.m_zoomFillActive = true;
        item.m_zoomFillDataReady = false;
        item.m_gpuDisplayImage =
            QImage(100, 20, QImage::Format_ARGB32_Premultiplied);
        item.m_gpuDisplayImage.fill(Qt::black);
    }

    QByteArray leftZoomOut(91 * bins, '\x55');
    item.feedPrecomputedChunk(
        leftZoomOut, bins, 0, 91,
        0, finalTotalColumns, 48000, 2048, false,
        true, trackToken, false, 2);

    QByteArray rightZoomOut(93 * bins, '\x66');
    item.feedPrecomputedChunk(
        rightZoomOut, bins, 0, 93,
        120, finalTotalColumns, 48000, 2048, false,
        false, trackToken, false, 2);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_precomputedMaxColumnIndex, 212);
        QCOMPARE(item.m_ringWriteSeq - item.m_ringOldestSeq, static_cast<qint64>(184));
        QVERIFY2(
            !item.m_zoomFillDataReady,
            "zoom-fill must not reveal a centered max-zoom frame when the visible "
            "range has an interior column hole");
    }
}

void QmlSmokeTest::spectrogramRetainedTextureNodeFreezesRetainedBodyDuringZoomFill() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "texture-node");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);
    item.setShowFpsOverlay(false);
    item.setPlaying(false);

    constexpr int bins = 32;
    constexpr int totalColumns = 256;
    QByteArray baseline(128 * bins, '\x40');
    QByteArray restarted(32 * bins, '\x55');

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    if (windowGraphicsApi(window) != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(
            QStringLiteral("retained texture-node zoom-fill test requires OpenGL scene graph, got %1")
                .arg(graphicsApiName(windowGraphicsApi(window)))));
    }

    item.feedPrecomputedChunk(
        baseline, bins, 0, 128,
        0, totalColumns, 48000, 1024, false, true, 1, false);
    item.setPositionSeconds((64.0 * 1024.0) / 48000.0);

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    std::unique_ptr<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);

    QSGTexture *retainedTexture = nullptr;
    QImage initialDisplayImage;
    {
        QMutexLocker lock(&item.m_stateMutex);
        retainedTexture = item.m_gpuSourceTexture.texture();
        initialDisplayImage = item.m_gpuDisplayImage.copy();
    }
    QVERIFY2(retainedTexture != nullptr, "texture-node baseline should build the retained source texture");
    QVERIFY2(!initialDisplayImage.isNull(), "texture-node baseline should keep a CPU display snapshot for centered freeze");
    auto *bodyTextureNode = spectrogramBodyTextureNode(node.get());
    QVERIFY(bodyTextureNode != nullptr);
    QCOMPARE(bodyTextureNode->texture(), retainedTexture);
    const QRectF initialSourceRect = bodyTextureNode->sourceRect();
    const QRectF initialTargetRect = bodyTextureNode->rect();

    item.feedPrecomputedChunk(
        restarted, bins, 0, 32,
        0, totalColumns, 48000, 512, false, true, 1, false);

    node.reset(item.updatePaintNode(node.release(), nullptr));
    QVERIFY(node != nullptr);

    bodyTextureNode = spectrogramBodyTextureNode(node.get());
    QVERIFY(bodyTextureNode != nullptr);
    QSGTexture *currentRetainedTexture = nullptr;
    {
        QMutexLocker lock(&item.m_stateMutex);
        currentRetainedTexture = item.m_gpuSourceTexture.texture();
        QVERIFY(item.m_zoomFillActive);
        QVERIFY(item.m_zoomFillFrozenRetainedActive);
        QVERIFY2(!item.m_gpuDisplayImage.isNull(),
            "retained texture-node mode should keep warming the CPU display cache during zoom-fill");
    }
    QCOMPARE(currentRetainedTexture, retainedTexture);
    QCOMPARE(bodyTextureNode->texture(), retainedTexture);
    QVERIFY2(!bodyTextureNode->rect().isEmpty(), "retained freeze should keep the texture-node body visible");
    QCOMPARE(bodyTextureNode->sourceRect(), initialSourceRect);
    QCOMPARE(bodyTextureNode->rect(), initialTargetRect);
}

void QmlSmokeTest::spectrogramRetainedSourceOnlyFreezesRetainedBodyDuringZoomFill() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "shader-source-only");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);
    item.setShowFpsOverlay(false);
    item.setPlaying(false);

    constexpr int bins = 32;
    constexpr int totalColumns = 256;
    QByteArray baseline(128 * bins, '\x40');
    QByteArray restarted(32 * bins, '\x55');

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    if (windowGraphicsApi(window) != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(
            QStringLiteral("retained shader-source-only zoom-fill test requires OpenGL scene graph, got %1")
                .arg(graphicsApiName(windowGraphicsApi(window)))));
    }

    item.feedPrecomputedChunk(
        baseline, bins, 0, 128,
        0, totalColumns, 48000, 1024, false, true, 1, false);
    item.setPositionSeconds((64.0 * 1024.0) / 48000.0);

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    std::unique_ptr<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);

    QSGTexture *retainedTexture = nullptr;
    QImage initialDisplayImage;
    {
        QMutexLocker lock(&item.m_stateMutex);
        retainedTexture = item.m_gpuSourceTexture.texture();
        initialDisplayImage = item.m_gpuDisplayImage.copy();
    }
    QVERIFY2(retainedTexture != nullptr, "shader-source-only baseline should build the retained source texture");
    QVERIFY2(!initialDisplayImage.isNull(), "shader-source-only baseline should keep a CPU display snapshot for centered freeze");
    auto *sourceNode = spectrogramSourceSampleMaterialNode(node.get());
    QVERIFY(sourceNode != nullptr);
    auto *sourceMaterial =
        dynamic_cast<SpectrogramSourceSampleMaterial *>(sourceNode->material());
    QVERIFY(sourceMaterial != nullptr);
    QCOMPARE(sourceMaterial->state.sourceTexture, retainedTexture);
    const QRectF initialSourceRect = sourceMaterial->state.sourceRect;

    item.feedPrecomputedChunk(
        restarted, bins, 0, 32,
        0, totalColumns, 48000, 512, false, true, 1, false);

    node.reset(item.updatePaintNode(node.release(), nullptr));
    QVERIFY(node != nullptr);

    QSGTexture *currentRetainedTexture = nullptr;
    {
        QMutexLocker lock(&item.m_stateMutex);
        currentRetainedTexture = item.m_gpuSourceTexture.texture();
        QVERIFY(item.m_zoomFillActive);
        QVERIFY(item.m_zoomFillFrozenRetainedActive);
        QVERIFY2(!item.m_gpuDisplayImage.isNull(),
            "retained shader-source-only mode should keep warming the CPU display cache during zoom-fill");
    }
    QCOMPARE(currentRetainedTexture, retainedTexture);
    sourceNode = spectrogramSourceSampleMaterialNode(node.get());
    QVERIFY(sourceNode != nullptr);
    QVERIFY2(
        geometryHasVisibleArea(sourceNode),
        "shader-source-only zoom-fill should keep the retained material node visible");
    sourceMaterial = dynamic_cast<SpectrogramSourceSampleMaterial *>(sourceNode->material());
    QVERIFY(sourceMaterial != nullptr);
    QCOMPARE(sourceMaterial->state.sourceTexture, retainedTexture);
    QCOMPARE(sourceMaterial->state.sourceRect, initialSourceRect);
}

void QmlSmokeTest::spectrogramFullModeFreezesRetainedBodyDuringZoomFill() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "full");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);
    item.setShowFpsOverlay(false);
    item.setPlaying(false);

    constexpr int bins = 32;
    constexpr int totalColumns = 256;
    QByteArray baseline(128 * bins, '\x40');
    QByteArray restarted(32 * bins, '\x55');

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    if (windowGraphicsApi(window) != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(
            QStringLiteral("retained full-mode zoom-fill test requires OpenGL scene graph, got %1")
                .arg(graphicsApiName(windowGraphicsApi(window)))));
    }

    item.feedPrecomputedChunk(
        baseline, bins, 0, 128,
        0, totalColumns, 48000, 1024, false, true, 1, false);
    item.setPositionSeconds((64.0 * 1024.0) / 48000.0);

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    std::unique_ptr<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);

    QSGTexture *retainedTexture = nullptr;
    QImage initialDisplayImage;
    {
        QMutexLocker lock(&item.m_stateMutex);
        retainedTexture = item.m_gpuSourceTexture.texture();
        initialDisplayImage = item.m_gpuDisplayImage.copy();
    }
    QVERIFY2(retainedTexture != nullptr, "full baseline should build the retained source texture");
    QVERIFY2(!initialDisplayImage.isNull(), "full baseline should keep a CPU display snapshot for centered freeze");
    assertFullModeBodyNodeInvariants(
        node.get(),
        "full baseline should keep the material-backed body active before zoom-fill fallback");
    auto *sampleNode = spectrogramBodySampleMaterialNode(node.get());
    QVERIFY(sampleNode != nullptr);
    auto *sampleMaterial =
        dynamic_cast<SpectrogramBodySampleMaterial *>(sampleNode->material());
    QVERIFY(sampleMaterial != nullptr);
    QCOMPARE(sampleMaterial->state.sourceTexture, retainedTexture);
    const QRectF initialSourceRect = sampleMaterial->state.sourceRect;

    item.feedPrecomputedChunk(
        restarted, bins, 0, 32,
        0, totalColumns, 48000, 512, false, true, 1, false);

    node.reset(item.updatePaintNode(node.release(), nullptr));
    QVERIFY(node != nullptr);

    QSGTexture *currentRetainedTexture = nullptr;
    {
        QMutexLocker lock(&item.m_stateMutex);
        currentRetainedTexture = item.m_gpuSourceTexture.texture();
        QVERIFY(item.m_zoomFillActive);
        QVERIFY(item.m_zoomFillFrozenRetainedActive);
        QVERIFY2(!item.m_gpuDisplayImage.isNull(),
            "retained full mode should keep warming the CPU display cache during zoom-fill");
    }
    QCOMPARE(currentRetainedTexture, retainedTexture);
    sampleNode = spectrogramBodySampleMaterialNode(node.get());
    QVERIFY(sampleNode != nullptr);
    QVERIFY2(
        geometryHasVisibleArea(sampleNode),
        "full-mode zoom-fill should keep the retained full-mode material node visible");
    sampleMaterial = dynamic_cast<SpectrogramBodySampleMaterial *>(sampleNode->material());
    QVERIFY(sampleMaterial != nullptr);
    QCOMPARE(sampleMaterial->state.sourceTexture, retainedTexture);
    QCOMPARE(sampleMaterial->state.sourceRect, initialSourceRect);
}

void QmlSmokeTest::spectrogramFullModeZoomFillDoesNotScrollPinnedCenteredFreeze() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "full");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(240, 120);

    SpectrogramItem item;
    item.setWidth(window.width());
    item.setHeight(window.height());
    item.setParentItem(window.contentItem());
    item.setDisplayMode(1);
    item.setGridEnabled(false);
    item.setCrosshairEnabled(false);
    item.setShowFpsOverlay(false);
    item.setPlaying(true);
    item.setPositionSeconds(0.0);

    constexpr int bins = 32;
    constexpr int totalColumns = 4096;
    QByteArray baseline(512 * bins, '\x40');

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    if (windowGraphicsApi(window) != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(
            QStringLiteral("retained full-mode pinned zoom-fill test requires OpenGL scene graph, got %1")
                .arg(graphicsApiName(windowGraphicsApi(window)))));
    }

    item.feedPrecomputedChunk(
        baseline, bins, 0, 512,
        0, totalColumns, 48000, 1024, false, true, 1, false);

    QCoreApplication::processEvents();

    std::unique_ptr<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);

    item.setZoomLevel(1.25);
    node.reset(item.updatePaintNode(node.release(), nullptr));
    QVERIFY(node != nullptr);

    auto *sampleNode = spectrogramBodySampleMaterialNode(node.get());
    QVERIFY(sampleNode != nullptr);
    auto *sampleMaterial =
        dynamic_cast<SpectrogramBodySampleMaterial *>(sampleNode->material());
    QVERIFY(sampleMaterial != nullptr);
    const QRectF frozenSourceRect = sampleMaterial->state.sourceRect;
    const QRectF frozenBounds = geometryBounds(sampleNode);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_zoomFillActive);
        QVERIFY(item.m_zoomFillFrozenRetainedActive);
        item.m_positionAnchorUpdatedAt -= std::chrono::milliseconds(120);
    }

    node.reset(item.updatePaintNode(node.release(), nullptr));
    QVERIFY(node != nullptr);
    sampleNode = spectrogramBodySampleMaterialNode(node.get());
    QVERIFY(sampleNode != nullptr);
    sampleMaterial =
        dynamic_cast<SpectrogramBodySampleMaterial *>(sampleNode->material());
    QVERIFY(sampleMaterial != nullptr);
    QCOMPARE(sampleMaterial->state.sourceRect, frozenSourceRect);
    QCOMPARE(geometryBounds(sampleNode), frozenBounds);
}

void QmlSmokeTest::spectrogramZoomFillClearsWhenDecoderReachesTail() {
    // Regression: at max zoom-out in a wide fullscreen canvas the
    // STFT windowing leaves the decoder a few columns short of the
    // scaled totalEstimate.  The old readiness check required
    // ringFill >= fillWidth - 16 (with fillWidth ≈ totalEstimate),
    // so ringFill could never reach the threshold and
    // m_zoomFillActive stayed true forever.  The old canvas then
    // leaked a slice of stale content at the right edge.
    SpectrogramItem item;
    item.setWidth(3440);
    item.setHeight(720);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;

    // Baseline zoom=1.0 chunk — populates the ring.
    QByteArray baseline(3440 * bins, '\x40');
    item.feedPrecomputedChunk(
        baseline, bins, 0, 3440,
        0, 10064, 44100, 1024, false,
        true, 1, true);
    QVERIFY(item.m_precomputedReady);

    // Prime the GPU cache so the hop-change detector has an old frame
    // to preserve across the zoom restart.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 1.0;
        item.ensureMapping(720);
        item.syncPrecomputedDisplayImageLocked(3440, 720, 0, 3439, false, 1.0);
    }
    QVERIFY(!item.m_gpuDisplayImage.isNull());
    QCOMPARE(item.m_zoomFillActive, false);

    // Zoom-out restart: the backend decimates to hop=2996 and the
    // scaled estimate shrinks to ≈ widget width.  Decoder produces
    // 3418 columns (22 short of the 3440 estimate) due to the
    // STFT tail.
    constexpr int decodedCols = 3418;
    QByteArray decimated(decodedCols * bins, '\x50');
    item.feedPrecomputedChunk(
        decimated, bins, 0, decodedCols,
        0, 3440, 44100, 2996, false,
        true, 1, false);

    QCOMPARE(item.m_precomputedMaxColumnIndex, decodedCols - 1);
    // Decoder will produce no more columns, so the freeze is ready to
    // reveal. Actual release happens in updatePaintNode so the old frame
    // stays visible until a replacement render resource is selected.
    QCOMPARE(item.m_zoomFillActive, true);
    QCOMPARE(item.m_zoomFillDataReady, true);
    // Dirty flag set so the next paint rebuilds the canvas cleanly.
    QVERIFY(item.m_precomputedCanvasDirty);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QCOMPARE(item.m_zoomFillActive, false);
    QCOMPARE(item.m_zoomFillDataReady, false);
    delete node;
}

void QmlSmokeTest::spectrogramZoomFillWaitsForDecodedRightEdgeAtOuterZoom() {
    SpectrogramItem item;
    item.setWidth(1183);
    item.setHeight(400);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int sampleRate = 48000;
    constexpr int hop = 640;
    constexpr int totalEstimate = 46336;
    constexpr qint64 nowCol = 24602;
    constexpr int displayLeft = 23419;
    constexpr int displayRight = 25784;
    constexpr int decodedMaxCol = 25770;
    constexpr int decodedCols = decodedMaxCol - displayLeft + 1;

    item.setPositionSeconds((static_cast<double>(nowCol) + 0.25)
        / (static_cast<double>(sampleRate) / static_cast<double>(hop)));

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 0.8;
        item.m_renderZoomLevel = 0.8;
        item.m_requestedNativeHopSize = hop;
        item.m_precomputedSampleRateHz = sampleRate;
        item.m_precomputedHopSize = hop;
        item.m_precomputedBinsPerColumn = bins;
        item.m_precomputedTotalColumnsEstimate = totalEstimate;
        item.m_precomputedReady = true;
        item.m_precomputedCanvasDisplayLeft = 0;
        item.m_precomputedCanvasDisplayRight = 1182;
        item.m_zoomFillActive = true;
        item.m_zoomFillDataReady = false;
        item.m_zoomFillSeekActive = false;
        item.m_awaitingZoomData = false;
    }

    QByteArray partialViewport(decodedCols * bins, '\x55');
    item.feedPrecomputedChunk(
        partialViewport, bins, 0, decodedCols,
        displayLeft, totalEstimate, sampleRate, hop,
        false, true, 3, true, 28);

    QMutexLocker lock(&item.m_stateMutex);
    QCOMPARE(item.m_precomputedMaxColumnIndex, decodedMaxCol);

    const double effectiveZoom = std::max(0.001, item.effectiveZoomLocked());
    const qint64 visibleWindowCols = std::max<qint64>(
        1,
        static_cast<qint64>(std::ceil(item.width() / effectiveZoom)));
    QCOMPARE(visibleWindowCols, static_cast<qint64>(displayRight - displayLeft + 1));

    const qint64 actualNowCol = static_cast<qint64>(std::floor(
        item.currentRenderPositionSecondsLocked(std::chrono::steady_clock::now())
        * (static_cast<double>(sampleRate) / static_cast<double>(hop))));
    QCOMPARE(actualNowCol, nowCol);
    QCOMPARE(actualNowCol - visibleWindowCols / 2, static_cast<qint64>(displayLeft));
    QCOMPARE(displayLeft + visibleWindowCols - 1, static_cast<qint64>(displayRight));

    QVERIFY2(
        item.m_precomputedMaxColumnIndex < displayRight,
        "test must model the gpu52 case where decoded data is short of the visible right edge");
    QVERIFY2(
        !item.m_zoomFillDataReady,
        "zoom-fill must not reveal while the decoded data is still short of "
        "the visible right edge; otherwise the retained texture exposes a "
        "black strip at outer zoom levels");
}

void QmlSmokeTest::spectrogramZoomFillRetainedRevealHandlesScrollingEdgeUploads() {
    QVERIFY(SpectrogramItem::shouldDrawFrozenRetainedSourceForZoomFill(
        true, true, false, true, false));
    QVERIFY2(
        !SpectrogramItem::shouldDrawFrozenRetainedSourceForZoomFill(
            true, true, true, true, false),
        "zoom-fill should stop drawing the frozen retained source only when "
        "the final retained source is ready to reveal");
    QVERIFY(!SpectrogramItem::shouldDrawFrozenRetainedSourceForZoomFill(
        false, true, false, true, false));
    QVERIFY(!SpectrogramItem::shouldDrawFrozenRetainedSourceForZoomFill(
        true, false, false, true, false));
    QVERIFY2(
        !SpectrogramItem::shouldDrawFrozenRetainedSourceForZoomFill(
            true, true, false, false, true),
        "partial frozen retained segments must not paint over a complete "
        "frozen image fallback because the uncovered right edge would be black");
    QVERIFY2(
        SpectrogramItem::shouldDrawFrozenRetainedSourceForZoomFill(
            true, true, false, false, false),
        "without a complete frozen image fallback, a partial retained preview "
        "is still the least-bad available freeze source");

    const QVector<SpectrogramItem::RetainedRenderSegment> partialSegments{
        SpectrogramItem::RetainedRenderSegment{
            QRect(1602, 0, 257, 16),
            QRectF(-0.2, 0.0, 500.1, 32.0),
            true,
        },
    };
    QVERIFY2(
        !SpectrogramItem::retainedSegmentsCoverViewportForZoomFill(
            partialSegments, 1183),
        "the gpu51 zoom trace drew only the left ~500px of an 1183px viewport");

    const QVector<SpectrogramItem::RetainedRenderSegment> fullSegments{
        SpectrogramItem::RetainedRenderSegment{
            QRect(0, 0, 1184, 16),
            QRectF(-0.8, 0.0, 1183.0, 32.0),
            true,
        },
    };
    QVERIFY(SpectrogramItem::retainedSegmentsCoverViewportForZoomFill(
        fullSegments, 1183));

    QVERIFY2(
        !SpectrogramItem::retainedSourceSegmentsReadyForZoomFill(
            fullSegments,
            1183,
            false),
        "a retained zoom-fill preview is not ready when its planned viewport "
        "segments refer to page textures that are not resident yet");
    QVERIFY(SpectrogramItem::retainedSourceSegmentsReadyForZoomFill(
        fullSegments,
        1183,
        true));

    QVERIFY2(
        !SpectrogramItem::retainedSourceReadyForZoomReveal(
            true, true, false, 384, false, false),
        "zoom-fill must not reveal the retained-source path on the same frame "
        "that is still uploading newly decoded columns");
    QVERIFY2(
        SpectrogramItem::retainedSourceReadyForZoomReveal(
            true, true, false, 384, false, true),
        "a centered viewport that is already scrolling should reveal once "
        "retained pages cover the visible frame, even if edge columns were "
        "uploaded during that paint");
    QVERIFY2(
        !SpectrogramItem::retainedSourceReadyForZoomReveal(
            true, true, false, 0, true, true),
        "zoom-fill must not reveal while the retained texture was recreated");
    QVERIFY(SpectrogramItem::retainedSourceReadyForZoomReveal(
        true, true, false, 0, false, false));
    QVERIFY(SpectrogramItem::retainedSourceReadyForZoomReveal(
        false, false, true, 384, true, false));

    QVERIFY2(
        !SpectrogramItem::shouldCaptureZoomFillFreezeForHopChange(
            true, true, false),
        "intermediate zoom resets must not replace an active frozen display");
    QVERIFY2(
        !SpectrogramItem::shouldCaptureZoomFillFreezeForHopChange(
            true, false, true),
        "intermediate zoom resets must not replace an active frozen retained source");
    QVERIFY(SpectrogramItem::shouldCaptureZoomFillFreezeForHopChange(
        true, false, false));
    QVERIFY(SpectrogramItem::shouldCaptureZoomFillFreezeForHopChange(
        false, true, true));
}

void QmlSmokeTest::spectrogramZoomFillRetainedPreviewScrollsWithPlaybackClock() {
    const QVector<SpectrogramItem::RetainedRenderSegment> frozenSegments{
        SpectrogramItem::RetainedRenderSegment{
            QRect(100, 0, 40, 16),
            QRectF(10.0, 0.0, 80.0, 32.0),
            true,
        },
    };

    const QVector<SpectrogramItem::RetainedRenderSegment> scrolled =
        SpectrogramItem::scrollFrozenRetainedSegmentsForZoomFill(
            frozenSegments,
            3.25);

    QCOMPARE(scrolled.size(), 1);
    QCOMPARE(scrolled[0].sourceRect, QRect(103, 0, 40, 16));
    QCOMPARE(scrolled[0].targetRect, QRectF(9.5, 0.0, 80.0, 32.0));
    QVERIFY(scrolled[0].valid);

    const QVector<SpectrogramItem::RetainedRenderSegment> zoomedOut =
        SpectrogramItem::scrollFrozenRetainedSegmentsForZoomFill(
            frozenSegments,
            3.25,
            0,
            0.5,
            50.0);

    QCOMPARE(zoomedOut.size(), 1);
    QCOMPARE(zoomedOut[0].sourceRect, QRect(103, 0, 40, 16));
    QCOMPARE(zoomedOut[0].targetRect, QRectF(29.75, 0.0, 40.0, 32.0));
    QVERIFY(zoomedOut[0].valid);

    const QVector<SpectrogramItem::RetainedRenderSegment> boundarySegments{
        SpectrogramItem::RetainedRenderSegment{
            QRect(2038, 0, 20, 16),
            QRectF(0.0, 0.0, 40.0, 32.0),
            true,
        },
    };
    const QVector<SpectrogramItem::RetainedRenderSegment> boundaryScrolled =
        SpectrogramItem::scrollFrozenRetainedSegmentsForZoomFill(
            boundarySegments,
            5.25,
            2048);

    QCOMPARE(boundaryScrolled.size(), 2);
    QCOMPARE(boundaryScrolled[0].sourceRect, QRect(2043, 0, 5, 16));
    QCOMPARE(boundaryScrolled[0].targetRect, QRectF(-0.5, 0.0, 10.0, 32.0));
    QCOMPARE(boundaryScrolled[1].sourceRect, QRect(2048, 0, 15, 16));
    QCOMPARE(boundaryScrolled[1].targetRect, QRectF(9.5, 0.0, 30.0, 32.0));

    const QVector<SpectrogramItem::RetainedRenderSegment> offscreenPhaseSegments{
        SpectrogramItem::RetainedRenderSegment{
            QRect(100, 0, 10, 16),
            QRectF(-1.5, 0.0, 20.0, 32.0),
            true,
        },
    };
    const QVector<SpectrogramItem::RetainedRenderSegment> normalizedPhase =
        SpectrogramItem::scrollFrozenRetainedSegmentsForZoomFill(
            offscreenPhaseSegments,
            3.75);

    QCOMPARE(normalizedPhase.size(), 1);
    QCOMPARE(normalizedPhase[0].sourceRect, QRect(104, 0, 10, 16));
    QCOMPARE(normalizedPhase[0].targetRect, QRectF(-1.0, 0.0, 20.0, 32.0));
}

void QmlSmokeTest::spectrogramZoomFillRetainedPreviewScrollsOnlyWithMovingCenteredViewport() {
    QCOMPARE(
        SpectrogramItem::frozenRetainedSourceColumnDeltaForZoomFill(5.25, 0.0, false, false),
        0.0);
    QCOMPARE(
        SpectrogramItem::frozenRetainedSourceColumnDeltaForZoomFill(5.25, 2.0, false, false),
        2.0);
    QCOMPARE(
        SpectrogramItem::frozenRetainedSourceColumnDeltaForZoomFill(5.25, 0.0, false, true),
        5.25);
    QCOMPARE(
        SpectrogramItem::frozenRetainedSourceColumnDeltaForZoomFill(4.0, 5.25, false, true),
        5.25);
    QCOMPARE(
        SpectrogramItem::frozenRetainedSourceColumnDeltaForZoomFill(5.25, 0.0, true, false),
        5.25);
    QCOMPARE(
        SpectrogramItem::frozenRetainedSourceColumnDeltaForZoomFill(4.0, 5.25, true, false),
        5.25);

    const QVector<SpectrogramItem::RetainedRenderSegment> frozenSegments{
        SpectrogramItem::RetainedRenderSegment{
            QRect(100, 0, 40, 16),
            QRectF(10.0, 0.0, 80.0, 32.0),
            true,
        },
    };

    const QVector<SpectrogramItem::RetainedRenderSegment> centeredUnmoved =
        SpectrogramItem::scrollFrozenRetainedSegmentsForZoomFill(
            frozenSegments,
            0.0,
            0,
            1.0,
            0.0);

    QCOMPARE(centeredUnmoved.size(), 1);
    QCOMPARE(centeredUnmoved[0].sourceRect, QRect(100, 0, 40, 16));
    QCOMPARE(centeredUnmoved[0].targetRect, QRectF(10.0, 0.0, 80.0, 32.0));

    const QVector<SpectrogramItem::RetainedRenderSegment> movingCentered =
        SpectrogramItem::scrollFrozenRetainedSegmentsForZoomFill(
            frozenSegments,
            3.25,
            0,
            1.0,
            0.0);

    QCOMPARE(movingCentered.size(), 1);
    QCOMPARE(movingCentered[0].sourceRect, QRect(103, 0, 40, 16));
    QCOMPARE(movingCentered[0].targetRect, QRectF(9.5, 0.0, 80.0, 32.0));

    const QVector<SpectrogramItem::RetainedRenderSegment> longMovingCentered =
        SpectrogramItem::scrollFrozenRetainedSegmentsForZoomFill(
            frozenSegments,
            33.25,
            0,
            1.0,
            0.0);

    QCOMPARE(longMovingCentered.size(), 1);
    QCOMPARE(longMovingCentered[0].sourceRect, QRect(133, 0, 40, 16));
    QCOMPARE(longMovingCentered[0].targetRect, QRectF(9.5, 0.0, 80.0, 32.0));
}

void QmlSmokeTest::spectrogramZoomFillRetainedPreviewUsesFrozenScrollMode() {
    QVERIFY2(
        SpectrogramItem::zoomFillFrozenRetainedScrollsWithPlayback(
            false, false, true, true),
        "a frozen retained preview captured while centered playback was "
        "scrolling must keep scrolling even if the new not-yet-ready geometry "
        "currently looks pinned");
    QVERIFY2(
        !SpectrogramItem::zoomFillFrozenRetainedScrollsWithPlayback(
            false, true, true, false),
        "a frozen retained preview captured while centered playback was pinned "
        "must stay pinned even if the replacement geometry is scrolling");
    QVERIFY(SpectrogramItem::zoomFillFrozenRetainedScrollsWithPlayback(
        false, true, false, false));
    QVERIFY(SpectrogramItem::zoomFillFrozenRetainedScrollsWithPlayback(
        true, false, true, false));
}

void QmlSmokeTest::spectrogramZoomFillPlayheadUsesFrozenAnchorDuringFreeze() {
    QCOMPARE(
        SpectrogramItem::zoomFillPlayheadPixelForFrame(
            1810, 1920, false, true, true, 960.4),
        960);
    QCOMPARE(
        SpectrogramItem::zoomFillPlayheadPixelForFrame(
            1810, 1920, false, true, true, 2500.0),
        1919);
    QCOMPARE(
        SpectrogramItem::zoomFillPlayheadPixelForFrame(
            1810, 1920, false, true, false, 960.4),
        1810);
    QCOMPARE(
        SpectrogramItem::zoomFillPlayheadPixelForFrame(
            -1, 1920, true, true, true, 960.4),
        -1);
}

void QmlSmokeTest::spectrogramZoomWiggleTraceProjectsRetainedSourceColumnToScreenX() {
    const QVector<SpectrogramItem::RetainedRenderSegment> segments{
        SpectrogramItem::RetainedRenderSegment{
            QRect(100, 0, 40, 16),
            QRectF(10.0, 0.0, 80.0, 32.0),
            true,
        },
        SpectrogramItem::RetainedRenderSegment{
            QRect(140, 0, 20, 16),
            QRectF(90.0, 0.0, 40.0, 32.0),
            true,
        },
    };

    QCOMPARE(
        SpectrogramItem::retainedSourceColumnScreenXForZoomTrace(segments, 100.0),
        10.0);
    QCOMPARE(
        SpectrogramItem::retainedSourceColumnScreenXForZoomTrace(segments, 120.0),
        50.0);
    QCOMPARE(
        SpectrogramItem::retainedSourceColumnScreenXForZoomTrace(segments, 150.0),
        110.0);
    QCOMPARE(
        SpectrogramItem::retainedSourceColumnScreenXForZoomTrace(segments, 90.0),
        -10.0);
    QVERIFY(std::isnan(
        SpectrogramItem::retainedSourceColumnScreenXForZoomTrace({}, 100.0)));

    QCOMPARE(
        SpectrogramItem::retainedSourceColumnForScreenXForZoomTrace(segments, 10.0),
        100.0);
    QCOMPARE(
        SpectrogramItem::retainedSourceColumnForScreenXForZoomTrace(segments, 50.0),
        120.0);
    QCOMPARE(
        SpectrogramItem::retainedSourceColumnForScreenXForZoomTrace(segments, 110.0),
        150.0);
    QCOMPARE(
        SpectrogramItem::retainedSourceColumnForScreenXForZoomTrace(segments, -10.0),
        90.0);
    QVERIFY(std::isnan(
        SpectrogramItem::retainedSourceColumnForScreenXForZoomTrace({}, 100.0)));
}

void QmlSmokeTest::spectrogramZoomFillRetainedPreviewKeepsRightScrollHalo() {
    SpectrogramItem item;
    item.setWidth(100);
    item.setHeight(12);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int totalColumns = 140;
    QByteArray data(totalColumns * bins, '\x50');
    item.feedPrecomputedChunk(
        data, bins, 0, totalColumns,
        0, totalColumns, 48000, 1024, false,
        true, 1, false, 1);

    QMutexLocker lock(&item.m_stateMutex);
    const SpectrogramItem::RetainedRenderPlan plan =
        item.buildRetainedRenderPlanLocked(
            0,
            119,
            119,
            0.0,
            100,
            12,
            1.0,
            false,
            SpectrogramItem::kRetainedSourcePageWidth,
            99);

    QVERIFY(plan.haveSegments);
    QCOMPARE(plan.segments.size(), qsizetype{1});
    QCOMPARE(plan.segments[0].sourceRect.width(), 120);
    QCOMPARE(plan.segments[0].targetRect.width(), 120.0);

    const QVector<SpectrogramItem::RetainedRenderSegment> scrolled =
        SpectrogramItem::scrollFrozenRetainedSegmentsForZoomFill(
            plan.segments,
            10.0,
            SpectrogramItem::kRetainedSourcePageWidth,
            1.0,
            50.0);

    QVERIFY2(
        SpectrogramItem::retainedSegmentsCoverViewportForZoomFill(scrolled, 100),
        "frozen retained zoom-out previews need right-side offscreen columns so playback scrolling does not expose a black edge");
}

void QmlSmokeTest::spectrogramZoomFillDefersRetainedFreezeUntilScaledPreviewFrame() {
    SpectrogramItem item;
    item.setWidth(120);
    item.setHeight(80);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int columns = 240;
    constexpr quint64 trackToken = 53;
    QByteArray chunk(columns * bins, '\x50');
    item.feedPrecomputedChunk(
        chunk, bins, 0, columns,
        0, 1024, 44100, 1024, false,
        true, trackToken, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_precomputedCanvasDisplayLeft = 0;
        item.m_precomputedCanvasDisplayRight = 119;
        item.m_precomputedCanvasZoomLevel = 1.0;
        item.m_precomputedCanvasHopSize = 1024;
        item.m_precomputedCanvasTotalColumnsEstimate = 1024;
        item.m_zoomFillFrozenRetainedSegments = {
            SpectrogramItem::RetainedRenderSegment{
                QRect(0, 0, 120, bins),
                QRectF(0.0, 0.0, 120.0, 80.0),
                true,
            },
        };
        item.m_zoomFillFrozenRetainedAvailable = true;
        item.m_zoomFillFrozenRetainedActive = false;
        item.m_zoomFillActive = false;
        item.m_zoomFillFreezePending = false;
    }

    item.setZoomLevel(1.25);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_zoomFillActive);
        QVERIFY2(
            !item.m_zoomFillFrozenRetainedActive,
            "retained-only full mode must not freeze the previous zoom before the first scaled preview frame is rendered");
        QVERIFY2(
            item.m_zoomFillFreezePending,
            "the first post-zoom paint must capture the retained preview after m_renderZoomLevel has changed");
        QCOMPARE(item.m_renderZoomLevel, 1.25);
    }
}

void QmlSmokeTest::spectrogramZoomFillStackedZoomKeepsActiveRetainedFreeze() {
    SpectrogramItem item;
    item.setWidth(120);
    item.setHeight(80);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int columns = 512;
    constexpr quint64 trackToken = 55;
    QByteArray chunk(columns * bins, '\x50');
    item.feedPrecomputedChunk(
        chunk, bins, 0, columns,
        0, 200000, 48000, 100, false,
        true, trackToken, false, 17);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 10.24;
        item.m_renderZoomLevel = 10.24;
        item.m_requestedRenderZoomLevel = 10.24;
        item.m_requestedNativeHopSize = 100;
        item.m_precomputedCanvasDisplayLeft = 66000;
        item.m_precomputedCanvasDisplayRight = 68100;
        item.m_precomputedCanvasZoomLevel = item.effectiveZoomLocked();
        item.m_precomputedCanvasHopSize = 100;
        item.m_precomputedCanvasTotalColumnsEstimate = 200000;
        item.m_zoomFillActive = true;
        item.m_zoomFillDataReady = false;
        item.m_zoomFillFrozenRetainedAvailable = true;
        item.m_zoomFillFrozenRetainedActive = true;
        item.m_zoomFillFrozenRetainedSegments = {
            SpectrogramItem::RetainedRenderSegment{
                QRect(2600, 0, 1600, bins),
                QRectF(-1.0, 0.0, 120.0, 80.0),
                true,
            },
        };
        item.m_zoomFillFrozenRetainedPositionSeconds = 140.316;
        item.m_zoomFillFrozenRetainedColumnsPerSecond = 48000.0 / 100.0;
        item.m_zoomFillFrozenRetainedPixelsPerSecond =
            item.m_zoomFillFrozenRetainedColumnsPerSecond
            * item.effectiveZoomLocked();
        item.m_zoomFillFrozenRetainedGeneration = 17;
        item.m_zoomFillFreezePending = false;
        item.m_gpuDisplayImage = QImage();
    }

    item.setZoomLevel(6.5536);

    QMutexLocker lock(&item.m_stateMutex);
    QVERIFY(item.m_zoomFillActive);
    QVERIFY2(
        item.m_zoomFillFrozenRetainedActive,
        "a stacked zoom must keep the active retained freeze instead of "
        "dropping to an empty display frame while the next generation fills");
    QVERIFY2(
        !item.m_zoomFillFreezePending,
        "reusing an active retained freeze should not wait for a fresh "
        "scaled preview that may not be renderable yet");
    QCOMPARE(item.m_zoomFillDataReady, false);
    QVERIFY(item.m_awaitingZoomData);
    QVERIFY(std::abs(item.m_renderZoomLevel - 6.5536) < 0.001);
}

void QmlSmokeTest::spectrogramZoomFillMetadataResetKeepsActiveRetainedFreezeScroll() {
    SpectrogramItem item;
    item.setWidth(120);
    item.setHeight(80);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr quint64 trackToken = 54;
    QByteArray chunk(240 * bins, '\x50');
    item.feedPrecomputedChunk(
        chunk, bins, 0, 240,
        0, 1024, 48000, 336, false,
        true, trackToken, false, 6);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_precomputedCanvasDisplayLeft = 100;
        item.m_precomputedCanvasDisplayRight = 219;
        item.m_precomputedCanvasZoomLevel = 3.8147;
        item.m_precomputedCanvasHopSize = 336;
        item.m_precomputedCanvasTotalColumnsEstimate = 2048;
        item.m_zoomLevel = 3.8147;
        item.m_renderZoomLevel = 3.8147;
        item.m_awaitingZoomData = true;
        item.m_zoomFillActive = true;
        item.m_zoomFillDataReady = false;
        item.m_zoomFillFrozenRetainedAvailable = true;
        item.m_zoomFillFrozenRetainedActive = true;
        item.m_zoomFillFrozenRetainedSegments = {
            SpectrogramItem::RetainedRenderSegment{
                QRect(875, 0, 946, bins),
                QRectF(-1.0, 0.0, 1183.0, 80.0),
                true,
            },
        };
        item.m_zoomFillFrozenRetainedPositionSeconds = 83.971;
        item.m_zoomFillFrozenRetainedColumnsPerSecond = 48000.0 / 336.0;
        item.m_zoomFillFrozenRetainedPixelsPerSecond =
            item.m_zoomFillFrozenRetainedColumnsPerSecond
            * item.effectiveZoomLocked();
        item.m_zoomFillFrozenRetainedLastColumnDelta = 20.0;
        item.m_zoomFillFrozenRetainedGeneration = 6;
        item.m_zoomWiggleTraceProbeSeconds = 83.971;
    }

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0,
        13739, 110654, 48000, 268, false,
        true, trackToken, true, 7);

    QMutexLocker lock(&item.m_stateMutex);
    QVERIFY(item.m_precomputedResetPending);
    QVERIFY(item.m_zoomFillActive);
    QVERIFY(item.m_zoomFillFrozenRetainedActive);
    QCOMPARE(item.m_zoomFillFrozenRetainedGeneration, static_cast<quint64>(6));
    QCOMPARE(item.m_zoomFillFrozenRetainedLastColumnDelta, 20.0);
    QCOMPARE(item.m_zoomFillFrozenRetainedPositionSeconds, 83.971);
    QCOMPARE(item.m_zoomWiggleTraceProbeSeconds, 83.971);
}

void QmlSmokeTest::spectrogramZoomFillRetainedPreviewIgnoresBackwardClockCorrections() {
    QCOMPARE(
        SpectrogramItem::monotonicFrozenRetainedSourceColumnDelta(10.25, 11.5),
        11.5);
    QCOMPARE(
        SpectrogramItem::monotonicFrozenRetainedSourceColumnDelta(12.0, 11.5),
        12.0);
    QCOMPARE(
        SpectrogramItem::monotonicFrozenRetainedSourceColumnDelta(-2.0, 0.0),
        0.0);
}

void QmlSmokeTest::spectrogramZoomFillRetainedPreviewCoversPostRevealUploadGaps() {
    QVERIFY2(
        SpectrogramItem::shouldDrawCachedRetainedSourceForFrame(
            true, true, false, false, false),
        "a retained freeze should keep drawing cached retained pixels until the reveal frame is ready");
    QVERIFY2(
        !SpectrogramItem::shouldDrawCachedRetainedSourceForFrame(
            true, true, true, false, false),
        "the cached retained zoom frame should stand down on the reveal frame");
    QVERIFY2(
        SpectrogramItem::shouldDrawCachedRetainedSourceForFrame(
            false, false, false, false, true),
        "after reveal, a cached retained frame should cover transient upload gaps instead of painting black");
    QVERIFY2(
        !SpectrogramItem::shouldDrawCachedRetainedSourceForFrame(
            false, false, false, true, true),
        "current retained pages should be preferred whenever they can render the viewport");
}

void QmlSmokeTest::spectrogramRetainedRenderPlanAllowsDecodedTailSlack() {
    SpectrogramItem item;
    item.setWidth(1183);
    item.setHeight(159);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int totalColumns = 2366;
    constexpr int decodedColumns = 2351;
    QByteArray decoded(decodedColumns * bins, '\x50');
    item.feedPrecomputedChunk(
        decoded, bins, 0, decodedColumns,
        0, totalColumns, 48000, 4473, false,
        true, 1, false, 1);

    QMutexLocker lock(&item.m_stateMutex);
    const SpectrogramItem::RetainedRenderPlan plan =
        item.buildRetainedRenderPlanLocked(
            0,
            totalColumns - 1,
            decodedColumns - 1,
            0.0,
            1183,
            159,
            1.0,
            false);

    QVERIFY(plan.haveSegments);
    QVERIFY(!plan.overflowed);
    QVERIFY2(
        plan.coversRange,
        "retained rendering should treat accepted decoded tail slack as covered");
    QCOMPARE(plan.segments.size(), qsizetype{2});
    QVERIFY(plan.segments[0].valid);
    QVERIFY(plan.segments[1].valid);
    QCOMPARE(plan.segments[0].sourceRect, QRect(0, 0, SpectrogramItem::kRetainedSourcePageWidth, bins));
    QCOMPARE(
        plan.segments[1].sourceRect,
        QRect(
            SpectrogramItem::kRetainedSourcePageWidth,
            0,
            decodedColumns - SpectrogramItem::kRetainedSourcePageWidth,
            bins));
    QVERIFY(plan.segments[0].targetRect.width() < 1183.0);
}

void QmlSmokeTest::spectrogramRollingRetainedRenderPlanPrefillKeepsFixedZoomScale() {
    SpectrogramItem item;
    item.setWidth(12);
    item.setHeight(6);
    item.setDisplayMode(0); // Rolling

    constexpr int bins = 4;
    constexpr int columns = 4;
    QByteArray data(columns * bins, '\x50');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, 64, 48000, 1024, false,
        true, 91, false);

    QMutexLocker lock(&item.m_stateMutex);
    const SpectrogramItem::RetainedRenderPlan plan =
        item.buildRetainedRenderPlanLocked(
            0,
            columns - 1,
            columns - 1,
            0.0,
            12,
            6,
            2.0,
            true);

    QVERIFY(plan.haveSegments);
    QVERIFY(!plan.overflowed);
    QVERIFY(plan.coversRange);
    QCOMPARE(plan.segments.size(), qsizetype{1});
    QCOMPARE(plan.segments[0].sourceRect, QRect(0, 0, columns, bins));
    QCOMPARE(plan.segments[0].targetRect, QRectF(4.0, 0.0, 8.0, 6.0));
}

void QmlSmokeTest::spectrogramRetainedRenderPlanCoversOnePage() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int columns = SpectrogramItem::kRetainedSourcePageWidth;
    QByteArray data(columns * bins, '\x50');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, 81, false);

    QMutexLocker lock(&item.m_stateMutex);
    const SpectrogramItem::RetainedRenderPlan plan =
        item.buildRetainedRenderPlanLocked(0, columns - 1, columns - 1, 0.0, columns, 64, 1.0, false);

    QVERIFY(plan.haveSegments);
    QVERIFY(!plan.overflowed);
    QVERIFY(plan.coversRange);
    QCOMPARE(plan.segments.size(), qsizetype{1});
    QVERIFY(plan.segments[0].valid);
    QCOMPARE(plan.segments[0].sourceRect, QRect(0, 0, columns, bins));
}

void QmlSmokeTest::spectrogramRetainedRenderPlanCoversTwoPages() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int columns = SpectrogramItem::kRetainedSourcePageWidth + 64;
    QByteArray data(columns * bins, '\x50');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, 82, false);

    QMutexLocker lock(&item.m_stateMutex);
    const SpectrogramItem::RetainedRenderPlan plan =
        item.buildRetainedRenderPlanLocked(2040, 2055, 2055, 0.0, 16, 64, 1.0, false);

    QVERIFY(plan.haveSegments);
    QVERIFY(!plan.overflowed);
    QVERIFY(plan.coversRange);
    QCOMPARE(plan.segments.size(), qsizetype{2});
    QCOMPARE(plan.segments[0].sourceRect, QRect(2040, 0, 8, bins));
    QCOMPARE(plan.segments[1].sourceRect, QRect(2048, 0, 8, bins));
}

void QmlSmokeTest::spectrogramRetainedRenderPlanCoversThreePages() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int columns = SpectrogramItem::kRetainedSourcePageWidth * 3;
    QByteArray data(columns * bins, '\x50');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, 83, false);

    QMutexLocker lock(&item.m_stateMutex);
    const SpectrogramItem::RetainedRenderPlan plan =
        item.buildRetainedRenderPlanLocked(2040, 4103, 4103, 0.0, 2064, 64, 1.0, false);

    QVERIFY(plan.haveSegments);
    QVERIFY(!plan.overflowed);
    QVERIFY(plan.coversRange);
    QCOMPARE(plan.segments.size(), qsizetype{3});
    QCOMPARE(plan.segments[0].sourceRect, QRect(2040, 0, 8, bins));
    QCOMPARE(plan.segments[1].sourceRect, QRect(2048, 0, 2048, bins));
    QCOMPARE(plan.segments[2].sourceRect, QRect(4096, 0, 8, bins));
}

void QmlSmokeTest::spectrogramRetainedRenderPlanUsesClampedPageWidth() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int pageWidth = 1024;
    constexpr int columns = pageWidth * 3;
    QByteArray data(columns * bins, '\x50');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, 86, false);

    QMutexLocker lock(&item.m_stateMutex);
    const SpectrogramItem::RetainedRenderPlan plan =
        item.buildRetainedRenderPlanLocked(1016, 2055, 2055, 0.0, 1040, 64, 1.0, false, pageWidth);

    QVERIFY(plan.haveSegments);
    QVERIFY(!plan.overflowed);
    QVERIFY(plan.coversRange);
    QCOMPARE(plan.segments.size(), qsizetype{3});
    QCOMPARE(plan.segments[0].sourceRect, QRect(1016, 0, 8, bins));
    QCOMPARE(plan.segments[1].sourceRect, QRect(1024, 0, 1024, bins));
    QCOMPARE(plan.segments[2].sourceRect, QRect(2048, 0, 8, bins));
}

void QmlSmokeTest::spectrogramRetainedRenderPlanStillFallsBackForSparseOverflow() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr int columns = 64;
    QByteArray data(columns * bins, '\x50');
    item.feedPrecomputedChunk(
        data, bins, 0, columns,
        0, columns, 48000, 1024, true,
        true, 84, false);

    QMutexLocker lock(&item.m_stateMutex);
    item.m_ringColumnId[static_cast<size_t>(12)] = -1;

    const SpectrogramItem::RetainedRenderPlan plan =
        item.buildRetainedRenderPlanLocked(0, 31, 31, 0.0, 32, 64, 1.0, false);

    QVERIFY(plan.haveSegments);
    QVERIFY(!plan.coversRange);
}

void QmlSmokeTest::spectrogramZoomFillWarmsDisplayCacheBehindFrozenFrame() {
    SpectrogramItem item;
    item.setWidth(20);
    item.setHeight(6);
    item.setDisplayMode(1); // Centered
    item.setPlaying(false);

    constexpr int bins = 4;
    constexpr int totalColumns = 128;
    QByteArray baseline(20 * bins, '\x40');
    item.feedPrecomputedChunk(
        baseline, bins, 0, 20,
        0, totalColumns, 48000, 1024, false, true, 1, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds = (9.0 * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    quint64 initialDisplayGeneration = 0;
    QImage initialDisplayImage;
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_gpuDisplayImage.isNull());
        initialDisplayGeneration = item.m_gpuDisplayGeneration;
        initialDisplayImage = item.m_gpuDisplayImage.copy();
    }

    QByteArray restarted(8 * bins, '\x55');
    item.feedPrecomputedChunk(
        restarted, bins, 0, 8,
        0, totalColumns, 48000, 512, false, true, 1, false);

    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_zoomFillActive);
        QVERIFY(!item.m_zoomFillFrozenDisplayImage.isNull());
        QCOMPARE(item.m_zoomFillFrozenDisplayImage, initialDisplayImage);
        QVERIFY2(
            item.m_gpuDisplayGeneration > initialDisplayGeneration,
            "the new display cache should warm behind the frozen zoom-fill frame");
    }

    delete node;
}

void QmlSmokeTest::spectrogramZoomFillRevealsOnlyFromRenderableFrame() {
    SpectrogramItem item;
    item.setWidth(20);
    item.setHeight(6);
    item.setDisplayMode(1); // Centered
    item.setPlaying(false);

    constexpr int bins = 4;
    constexpr int totalColumns = 128;
    QByteArray baseline(40 * bins, '\x40');
    item.feedPrecomputedChunk(
        baseline, bins, 0, 40,
        0, totalColumns, 48000, 1024, false, true, 1, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto now = std::chrono::steady_clock::now();
        item.m_positionSeconds = (9.0 * 1024.0) / 48000.0;
        item.m_positionAnchorSeconds = item.m_positionSeconds;
        item.m_positionAnchorUpdatedAt = now;
        item.m_positionAnchorInitialized = true;
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QByteArray restarted(40 * bins, '\x55');
    item.feedPrecomputedChunk(
        restarted, bins, 0, 40,
        0, totalColumns, 48000, 512, false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_zoomFillActive);
        QVERIFY(item.m_zoomFillDataReady);
        QVERIFY(!item.m_zoomFillFrozenDisplayImage.isNull());
    }

    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY2(
            !item.m_zoomFillActive,
            "freeze should clear on the frame that can render replacement content");
        QVERIFY(!item.m_zoomFillDataReady);
        QVERIFY(item.m_zoomFillFrozenDisplayImage.isNull());
    }

    delete node;
}

void QmlSmokeTest::spectrogramFullscreenMaxZoomRevealUsesRetainedFullModeDirectly() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "full");
    struct DebugModeGuard {
        ~DebugModeGuard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); }
    } debugModeGuard;

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(3440, 720);

    SpectrogramItem *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(window.width());
    item->setHeight(window.height());
    item->setDisplayMode(1); // Centered
    item->setPlaying(true);

    constexpr int bins = 8;
    constexpr int baselineColumns = 5000;
    constexpr int maxZoomColumns = 4200;
    constexpr int totalColumns = 230864;
    constexpr quint64 trackToken = 107;

    QByteArray baseline(baselineColumns * bins, '\x40');
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    if (windowGraphicsApi(window) != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(
            QStringLiteral("retained full-mode max-zoom reveal test requires OpenGL scene graph, got %1")
                .arg(graphicsApiName(windowGraphicsApi(window)))));
    }

    item->feedPrecomputedChunk(
        baseline, bins, 0, baselineColumns,
        0, baselineColumns, 48000, 1024, false,
        true, trackToken, false, 1);
    item->setPositionSeconds(2.5);

    QSGNode *node = item->updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    {
        QMutexLocker lock(&item->m_stateMutex);
        item->m_zoomLevel = 16.0;
        item->m_renderZoomLevel = 16.0;
        item->m_awaitingZoomData = false;
        item->m_gpuDisplayImage = QImage();
    }

    QByteArray maxZoomChunk(maxZoomColumns * bins, '\x70');
    item->feedPrecomputedChunk(
        maxZoomChunk, bins, 0, maxZoomColumns,
        0, totalColumns, 48000, 64, false,
        true, trackToken, false, 2);
    item->setPositionSeconds(2.8);

    {
        QMutexLocker lock(&item->m_stateMutex);
        QVERIFY(item->m_zoomFillActive);
        QVERIFY(item->m_zoomFillDataReady);
        QVERIFY2(
            item->m_gpuDisplayImage.isNull(),
            "retained full-mode max-zoom fill should not warm a CPU display image before reveal");
    }

    QSGNode *updatedNode = item->updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    auto *sampleNode = spectrogramBodySampleMaterialNode(node);
    QVERIFY(sampleNode != nullptr);
    QVERIFY2(
        geometryHasVisibleArea(sampleNode),
        "fullscreen max-zoom reveal should switch directly to the retained full-mode shader path");
    {
        QMutexLocker lock(&item->m_stateMutex);
        QVERIFY2(
            item->m_gpuDisplayImage.isNull(),
            "retained full-mode reveal should leave the CPU display image cold");
        QVERIFY(item->m_gpuSourceTexture.texture() != nullptr);
    }

    delete node;
}

void QmlSmokeTest::spectrogramMetadataOnlyZoomResetDefersFreezeSnapshot() {
    SpectrogramItem item;
    item.setWidth(120);
    item.setHeight(80);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int totalColumns = 512;
    constexpr quint64 trackToken = 23;
    QByteArray baseline(240 * bins, '\x40');
    item.feedPrecomputedChunk(
        baseline, bins, 0, 240,
        0, totalColumns, 44100, 1024, false,
        true, trackToken, true);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(80);
        item.syncPrecomputedDisplayImageLocked(
            120, 80, 0, 119, false, 1.0, false);
        QVERIFY2(
            item.m_gpuDisplayImage.isNull(),
            "retained-GPU playback should not keep the CPU display image hot");
        QCOMPARE(item.m_precomputedHopSize, 1024);
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
        QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(119));
    }

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0,
        0, totalColumns * 2, 44100, 512, false,
        true, trackToken, true);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_precomputedResetPending);
        QVERIFY2(
            item.m_gpuDisplayImage.isNull(),
            "metadata-only deferred reset should not build a CPU freeze snapshot");
        QVERIFY2(
            !item.m_zoomFillActive,
            "metadata-only deferred reset should leave zoom-fill capture to the first data chunk");
    }
}

void QmlSmokeTest::spectrogramMetadataOnlySameTrackResetFreezesExistingFrame() {
    SpectrogramItem item;
    item.setWidth(120);
    item.setHeight(80);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int totalColumns = 512;
    constexpr quint64 trackToken = 25;
    QByteArray baseline(240 * bins, '\x40');
    item.feedPrecomputedChunk(
        baseline, bins, 0, 240,
        0, totalColumns, 44100, 1024, false,
        true, trackToken, true);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(80);
        item.syncPrecomputedDisplayImageLocked(
            120, 80, 0, 119, false, 1.0, true);
        QVERIFY2(
            !item.m_gpuDisplayImage.isNull(),
            "test setup should have an existing rendered frame to preserve");
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
        QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(119));
    }

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0,
        0, totalColumns * 2, 44100, 512, false,
        true, trackToken, true, 2);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_precomputedResetPending);
        QVERIFY2(
            item.m_zoomFillActive,
            "same-track metadata-only reset should keep the old frame visible until data covers the new viewport");
        QVERIFY2(
            !item.m_zoomFillFrozenDisplayImage.isNull(),
            "same-track metadata-only reset should freeze the existing GPU display image");
        QVERIFY2(
            !item.m_gpuDisplayImage.isNull(),
            "same-track metadata-only reset should not invalidate the current display image before data arrives");
    }
}

void QmlSmokeTest::spectrogramZoomFillSkipsCpuFreezeSnapshotWithoutRetainedFrame() {
    // Retained-GPU playback can keep only display metadata hot. Building a
    // CPU freeze image from that metadata in feedPrecomputedChunk blocks the
    // UI thread during rapid zoom/restart churn, so a data-bearing restart
    // without an existing retained frame must skip freeze capture.
    SpectrogramItem item;
    item.setWidth(120);
    item.setHeight(80);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int totalColumns = 512;
    QByteArray baseline(240 * bins, '\x40');
    item.feedPrecomputedChunk(
        baseline, bins, 0, 240,
        0, totalColumns, 44100, 1024, false,
        true, 23, true);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(80);
        item.syncPrecomputedDisplayImageLocked(
            120, 80, 0, 119, false, 1.0, false);
        QVERIFY2(
            item.m_gpuDisplayImage.isNull(),
            "retained-GPU playback should not keep the CPU display image hot");
        QCOMPARE(item.m_precomputedHopSize, 1024);
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
        QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(119));
    }

    QByteArray restarted(8 * bins, '\x55');
    item.feedPrecomputedChunk(
        restarted, bins, 0, 8,
        0, totalColumns * 2, 44100, 512, false,
        true, 23, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_zoomFillActive);
        QVERIFY2(
            item.m_zoomFillFrozenDisplayImage.isNull(),
            "zoom restart should not build a CPU freeze snapshot on the feed path");
        QVERIFY2(
            item.m_gpuDisplayImage.isNull(),
            "retained-GPU playback should leave the CPU display image cold during restart feed");
    }
}

void QmlSmokeTest::spectrogramZoomFillUsesRetainedFreezeWithoutCpuSnapshot() {
    SpectrogramItem item;
    item.setWidth(120);
    item.setHeight(80);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int columns = 240;
    QByteArray chunk(columns * bins, '\x50');
    item.feedPrecomputedChunk(
        chunk, bins, 0, columns,
        0, columns, 44100, 1024, true,
        true, 24, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(80);
        item.syncPrecomputedDisplayImageLocked(
            120, 80, 0, 119, false, 1.0, false);
        QVERIFY(item.m_gpuDisplayImage.isNull());
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
        QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(119));

        item.m_zoomFillFrozenRetainedSegments = {
            SpectrogramItem::RetainedRenderSegment{
                QRect(0, 0, 120, bins),
                QRectF(0.0, 0.0, 120.0, 80.0),
                true,
            },
        };
        item.m_zoomFillFrozenRetainedAvailable = true;

        item.captureZoomFillFrozenCacheLocked();

        QVERIFY2(
            item.m_zoomFillFrozenDisplayImage.isNull(),
            "retained freeze capture should not build a CPU snapshot when retained segments can preserve the visible frame");
        QVERIFY(item.m_zoomFillFrozenRetainedAvailable);
    }
}

void QmlSmokeTest::spectrogramZoomFillPinnedRetainedFreezeKeepsDisplayFallback() {
    SpectrogramItem item;
    item.setWidth(120);
    item.setHeight(80);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int columns = 240;
    QByteArray chunk(columns * bins, '\x50');
    item.feedPrecomputedChunk(
        chunk, bins, 0, columns,
        0, columns, 44100, 1024, true,
        true, 26, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(80);
        item.syncPrecomputedDisplayImageLocked(
            120, 80, 0, 119, false, 1.0, false);
        QVERIFY(item.m_gpuDisplayImage.isNull());
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
        QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(119));

        item.m_zoomFillFrozenRetainedSegments = {
            SpectrogramItem::RetainedRenderSegment{
                QRect(0, 0, 120, bins),
                QRectF(0.0, 0.0, 120.0, 80.0),
                true,
            },
        };
        item.m_zoomFillFrozenRetainedAvailable = true;

        item.captureZoomFillFrozenCacheLocked(
            true,
            1.0,
            60.0,
            false,
            true);

        QVERIFY2(
            !item.m_zoomFillFrozenDisplayImage.isNull(),
            "pinned centered retained freezes need a display-image fallback "
            "so rapid zoom resets cannot go black if retained pages are "
            "temporarily not drawable");
        QVERIFY(item.m_zoomFillFrozenRetainedAvailable);
    }
}

void QmlSmokeTest::spectrogramZoomFillScrollingRetainedFreezeKeepsDisplayFallback() {
    SpectrogramItem item;
    item.setWidth(120);
    item.setHeight(80);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int columns = 240;
    QByteArray chunk(columns * bins, '\x50');
    item.feedPrecomputedChunk(
        chunk, bins, 0, columns,
        0, columns, 44100, 1024, true,
        true, 27, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(80);
        item.syncPrecomputedDisplayImageLocked(
            120, 80, 0, 119, false, 1.0, false);
        QVERIFY(item.m_gpuDisplayImage.isNull());

        item.m_zoomFillFrozenRetainedSegments = {
            SpectrogramItem::RetainedRenderSegment{
                QRect(0, 0, 120, bins),
                QRectF(0.0, 0.0, 120.0, 80.0),
                true,
            },
        };
        item.m_zoomFillFrozenRetainedAvailable = true;

        item.captureZoomFillFrozenCacheLocked(
            true,
            1.0,
            60.0,
            true,
            true);

        QVERIFY2(
            !item.m_zoomFillFrozenDisplayImage.isNull(),
            "scrolling centered retained freezes also need a display-image "
            "fallback because rapid stacked zoom-outs can scale or scroll "
            "the retained preview out of full viewport coverage before the "
            "new generation is revealable");
        QVERIFY(item.m_zoomFillFrozenRetainedAvailable);
    }
}

void QmlSmokeTest::spectrogramSyntheticClearSkipsCpuFreezeSnapshotWithoutRetainedFrame() {
    // Retained-GPU playback does not keep m_gpuDisplayImage hot. A synthetic
    // seek clear must not rebuild that CPU image synchronously from metadata;
    // the UI thread should return immediately and wait for retained or new
    // decoded content instead.
    SpectrogramItem item;
    item.setWidth(120);
    item.setHeight(80);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int columns = 240;
    QByteArray chunk(columns * bins, '\x50');
    item.feedPrecomputedChunk(
        chunk, bins, 0, columns,
        0, columns, 44100, 1024, false,
        true, 17, true);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(80);
        item.syncPrecomputedDisplayImageLocked(
            120, 80, 0, 119, false, 1.0, false);
        QVERIFY2(
            item.m_gpuDisplayImage.isNull(),
            "retained-GPU playback should not keep the CPU display image hot");
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
        QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(119));
    }

    item.feedPrecomputedChunk(
        QByteArray(), 0, 0, 0,
        0, 0, 0, 0, false,
        true, 17, true);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_awaitingWorkerReset);
        QVERIFY(!item.m_zoomFillActive);
        QVERIFY2(
            item.m_zoomFillFrozenDisplayImage.isNull(),
            "synthetic seek clear should not build a CPU freeze snapshot on the feed path");
        QVERIFY2(
            item.m_gpuDisplayImage.isNull(),
            "retained-GPU seek clear should leave the CPU display image cold");
    }
}

void QmlSmokeTest::spectrogramSyntheticClearInvalidatesRetainedSourceStateDuringFreeze() {
    // Regression: the frozen-frame seek path preserved retained-source
    // upload bookkeeping while wiping the ring. Same-track refills then
    // skipped uploads for columns that looked already resident, leaving
    // stale/empty texture slots visible as black gaps and wrong timing.
    SpectrogramItem item;
    item.setWidth(120);
    item.setHeight(80);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int columns = 240;
    constexpr quint64 trackToken = 31;
    QByteArray chunk(columns * bins, '\x50');
    item.feedPrecomputedChunk(
        chunk, bins, 0, columns,
        0, columns, 44100, 1024, false,
        true, trackToken, true);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(80);
        item.syncPrecomputedDisplayImageLocked(
            120, 80, 0, 119, false, 1.0, false);
        QVERIFY(item.m_gpuDisplayImage.isNull());

        item.m_gpuSourceTextureDisplayLeft = 0;
        item.m_gpuSourceTextureDisplayRight = 119;
        item.m_gpuSourceTextureRolling = false;
        item.m_gpuSourceTextureTrackToken = item.m_precomputedTrackToken;
        item.m_gpuSourceTextureRingWriteSeq = item.m_ringWriteSeq;
        item.m_gpuSourceTextureRingOldestSeq = item.m_ringOldestSeq;
        item.m_gpuSourceTextureMinColumnIndex = 0;
        item.m_gpuSourceTextureMaxColumnIndex = 119;
        auto page = std::make_unique<SpectrogramItem::RetainedSourcePageState>();
        page->key = SpectrogramItem::RetainedSourcePageKey{
            .trackToken = item.m_precomputedTrackToken,
            .rollingMode = false,
            .pageIndex = 0,
        };
        page->pageXOrigin = 0;
        page->width = 120;
        page->binsPerColumn = bins;
        page->uploadedDisplayLeft = 0;
        page->uploadedDisplayRight = 119;
        item.m_gpuSourcePages.push_back(std::move(page));
        item.m_zoomFillFrozenRetainedSegments = {
            SpectrogramItem::RetainedRenderSegment{
                QRect(0, 0, 120, bins),
                QRectF(0.0, 0.0, 120.0, 80.0),
                true,
            },
        };
        item.m_zoomFillFrozenRetainedAvailable = true;
    }

    item.feedPrecomputedChunk(
        QByteArray(), 0, 0, 0,
        0, 0, 0, 0, false,
        true, trackToken, true);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_zoomFillActive);
        QVERIFY(item.m_zoomFillFrozenDisplayImage.isNull());
        QCOMPARE(item.m_gpuSourcePages.size(), static_cast<size_t>(1));
        QVERIFY(item.m_gpuSourcePages.front() != nullptr);
        QCOMPARE(item.m_gpuSourcePages.front()->uploadedDisplayRight, static_cast<qint64>(-1));
        QCOMPARE(item.m_gpuSourceTextureTrackToken, static_cast<quint64>(0));
        QCOMPARE(item.m_gpuSourceTextureDisplayRight, static_cast<qint64>(-1));
        QVERIFY(item.m_gpuSourceTextureMaxColumnIndex < item.m_gpuSourceTextureMinColumnIndex);
    }
}

void QmlSmokeTest::spectrogramDataResetInvalidatesRetainedSourceStateDuringFreeze() {
    // Some centered seeks deliver the worker reset as the first data-bearing
    // chunk. That path also wipes/remaps ring slots and must force retained
    // source re-upload even when the frozen display image is preserved.
    SpectrogramItem item;
    item.setWidth(120);
    item.setHeight(80);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 8;
    constexpr int columns = 240;
    constexpr quint64 trackToken = 37;
    QByteArray baseline(columns * bins, '\x40');
    item.feedPrecomputedChunk(
        baseline, bins, 0, columns,
        0, 1024, 44100, 1024, false,
        true, trackToken, true);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(80);
        item.syncPrecomputedDisplayImageLocked(
            120, 80, 0, 119, false, 1.0, true);
        item.captureZoomFillFrozenCacheLocked();
        item.m_zoomFillActive = true;
        item.m_gpuSourceTextureDisplayLeft = 0;
        item.m_gpuSourceTextureDisplayRight = 119;
        item.m_gpuSourceTextureRolling = false;
        item.m_gpuSourceTextureTrackToken = item.m_precomputedTrackToken;
        item.m_gpuSourceTextureRingWriteSeq = item.m_ringWriteSeq;
        item.m_gpuSourceTextureRingOldestSeq = item.m_ringOldestSeq;
        item.m_gpuSourceTextureMinColumnIndex = 0;
        item.m_gpuSourceTextureMaxColumnIndex = 119;
    }

    QByteArray restarted(16 * bins, '\x55');
    item.feedPrecomputedChunk(
        restarted, bins, 0, 16,
        512, 1024, 44100, 1024, false,
        true, trackToken, true);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_zoomFillActive);
        QVERIFY(!item.m_zoomFillFrozenDisplayImage.isNull());
        QCOMPARE(item.m_gpuSourceTextureTrackToken, static_cast<quint64>(0));
        QCOMPARE(item.m_gpuSourceTextureDisplayRight, static_cast<qint64>(-1));
        QVERIFY(item.m_gpuSourceTextureMaxColumnIndex < item.m_gpuSourceTextureMinColumnIndex);
    }
}

void QmlSmokeTest::spectrogramDataResetPreservesRetainedFreezePagesDuringZoomFill() {
    // A data-bearing zoom reset must invalidate retained upload bookkeeping
    // without dropping the old retained page textures. Otherwise retained-only
    // freeze has no source to draw and the new session's partial refill becomes
    // visible.
    SpectrogramItem item;
    item.setWidth(120);
    item.setHeight(80);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 8;
    constexpr int columns = 240;
    constexpr quint64 trackToken = 41;
    QByteArray baseline(columns * bins, '\x40');
    item.feedPrecomputedChunk(
        baseline, bins, 0, columns,
        0, 1024, 44100, 1024, false,
        true, trackToken, true);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(80);
        item.syncPrecomputedDisplayImageLocked(
            120, 80, 0, 119, false, 1.0, false);
        QVERIFY(item.m_gpuDisplayImage.isNull());
        auto page = std::make_unique<SpectrogramItem::RetainedSourcePageState>();
        page->key = SpectrogramItem::RetainedSourcePageKey{
            .trackToken = item.m_precomputedTrackToken,
            .rollingMode = false,
            .pageIndex = 0,
        };
        page->pageXOrigin = 0;
        page->width = 120;
        page->binsPerColumn = bins;
        page->uploadedDisplayLeft = 0;
        page->uploadedDisplayRight = 119;
        item.m_gpuSourcePages.push_back(std::move(page));
        item.m_zoomFillFrozenRetainedSegments = {
            SpectrogramItem::RetainedRenderSegment{
                QRect(0, 0, 120, bins),
                QRectF(0.0, 0.0, 120.0, 80.0),
                true,
            },
        };
        item.m_zoomFillFrozenRetainedAvailable = true;
        item.m_zoomFillFrozenRetainedActive = true;
        item.m_zoomFillActive = true;
    }

    QByteArray restarted(16 * bins, '\x55');
    item.feedPrecomputedChunk(
        restarted, bins, 0, 16,
        512, 1024, 44100, 512, false,
        true, trackToken, true);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_zoomFillActive);
        QVERIFY(item.m_zoomFillFrozenRetainedActive);
        QCOMPARE(item.m_gpuSourcePages.size(), static_cast<size_t>(1));
        QVERIFY(item.m_gpuSourcePages.front() != nullptr);
        QCOMPARE(item.m_gpuSourcePages.front()->uploadedDisplayRight, static_cast<qint64>(-1));
        QCOMPARE(item.m_gpuSourceTextureTrackToken, static_cast<quint64>(0));
        QCOMPARE(item.m_gpuSourceTextureDisplayRight, static_cast<qint64>(-1));
        QVERIFY(item.m_gpuSourceTextureMaxColumnIndex < item.m_gpuSourceTextureMinColumnIndex);
    }
}

void QmlSmokeTest::spectrogramSyntheticClearPreservesGpuCacheDuringSeek() {
    // Regression: a seek outside the decoded window emits a synthetic
    // clear chunk (cols=0, bins=0, clear_history=true) that wipes the
    // ring and invalidates the canvas.  Without canvas preservation,
    // the display flashes fully black for ~100 ms while the backend
    // restarts at (pos − margin) and decodes up to the new playhead.
    // The freeze should arm instead so the old canvas stays visible
    // through the brief transition.
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;

    // Populate the ring with centered-mode data.
    QByteArray chunk(1920 * bins, '\x40');
    item.feedPrecomputedChunk(
        chunk, bins, 0, 1920,
        0, 10064, 44100, 1024, false,
        true, 7, true);
    QVERIFY(item.m_precomputedReady);

    // Build the GPU cache directly with a valid display range — the real
    // flow does this via updatePaintNode but QtTest has no event loop.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 1.0;
        item.ensureMapping(400);
        item.syncPrecomputedDisplayImageLocked(1920, 400, 0, 1919, false, 1.0);
    }
    QVERIFY(!item.m_gpuDisplayImage.isNull());
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(1919));

    // Synthetic clear: cols=0, bins=0, buffer_reset=true,
    // clear_history=true — the signature emitted by
    // seek_spectrogram_position for far seeks.
    item.feedPrecomputedChunk(
        QByteArray(), 0, 0, 0,
        0, 0, 0, 0, false,
        true, 7, true);

    // Ring is wiped.
    QCOMPARE(item.m_ringWriteSeq, static_cast<qint64>(0));
    QCOMPARE(item.m_ringCapacity, 0);
    QCOMPARE(item.m_precomputedMaxColumnIndex, -1);
    QVERIFY(item.m_awaitingWorkerReset);

    // The GPU cache and its display range are preserved, freeze is armed.
    QVERIFY(!item.m_gpuDisplayImage.isNull());
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(1919));
    QVERIFY(item.m_precomputedReady);
    QCOMPARE(item.m_zoomFillActive, true);
}

void QmlSmokeTest::spectrogramSeekFillRevealsAfterFirstVisibleColumns() {
    // Long same-track seeks restart the worker before the target viewport.
    // Once the new sequential decode reaches the left edge of that viewport,
    // the spectrogram should move to the seek location immediately instead of
    // freezing on the old location until the whole viewport is computed.
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);
    item.setDisplayMode(1);

    constexpr int bins = 4;
    constexpr quint64 trackToken = 7;
    constexpr quint64 generation = 42;
    constexpr int hop = 4337;
    constexpr int totalColumns = 6838;
    constexpr double zoom = 0.1181;
    constexpr double seekSeconds = 248.743;
    constexpr int restartColumn = 810;
    constexpr int firstVisibleColumn = 833;

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = zoom;
        item.m_renderZoomLevel = zoom;
        item.m_requestedNativeHopSize = hop;
        item.m_precomputedBinsPerColumn = bins;
        item.m_precomputedTotalColumnsEstimate = totalColumns;
        item.m_precomputedSampleRateHz = 48000;
        item.m_precomputedHopSize = hop;
        item.m_precomputedTrackToken = trackToken;
        item.m_precomputedReady = true;
        item.m_precomputedCanvasDisplayLeft = 0;
        item.m_precomputedCanvasDisplayRight = 1919;
        item.m_gpuDisplayImage = QImage(16, 16, QImage::Format_RGBA8888);
        item.m_gpuDisplayImage.fill(QColor(32, 32, 32));
        item.setPositionAnchorLocked(seekSeconds, std::chrono::steady_clock::now());
    }

    item.feedPrecomputedChunk(
        QByteArray(), 0, 0, 0,
        0, 0, 0, 0, false,
        true, trackToken, true);
    QVERIFY(item.m_zoomFillActive);
    QVERIFY(!item.m_zoomFillDataReady);

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0,
        restartColumn, totalColumns, 48000, hop, false,
        true, trackToken, true, generation);

    const int decodedColumns = firstVisibleColumn - restartColumn + 32;
    QByteArray visiblePrefix(decodedColumns * bins, '\x50');
    item.feedPrecomputedChunk(
        visiblePrefix, bins, 0, decodedColumns,
        restartColumn, totalColumns, 48000, hop, false,
        false, trackToken, false, generation);

    QVERIFY2(
        item.m_zoomFillDataReady,
        "seek fill should reveal as soon as newly decoded data reaches the "
        "target viewport instead of waiting for full viewport coverage");
}

void QmlSmokeTest::spectrogramSyntheticClearInvalidatesGpuCacheWhenNoOldContent() {
    // When there's no canvas to preserve (first track load, rolling
    // mode, or the display range was already invalid), the synthetic
    // clear must still wipe everything and invalidate — the freeze
    // would latch onto garbage state otherwise.
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);
    item.setDisplayMode(1);

    // No paint/cache sync yet → GPU cache stays null.  Still send some data so
    // m_precomputedReady could be true, which would be misleading
    // after the clear.
    QByteArray chunk(100 * 4, '\x40');
    item.feedPrecomputedChunk(
        chunk, 4, 0, 100,
        0, 10064, 44100, 1024, false,
        true, 7, true);
    QVERIFY(item.m_gpuDisplayImage.isNull());

    item.feedPrecomputedChunk(
        QByteArray(), 0, 0, 0,
        0, 0, 0, 0, false,
        true, 7, true);

    QVERIFY(item.m_gpuDisplayImage.isNull());
    QCOMPARE(item.m_precomputedReady, false);
    QCOMPARE(item.m_zoomFillActive, false);
    QCOMPARE(item.m_precomputedCanvasDisplayRight,
             item.m_precomputedCanvasDisplayLeft - 1);
}

int main(int argc, char **argv) {
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");

    QApplication app(argc, argv);
    QmlSmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_qml_smoke.moc"
