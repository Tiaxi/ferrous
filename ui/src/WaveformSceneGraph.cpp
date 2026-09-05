// SPDX-License-Identifier: GPL-3.0-or-later
#include "WaveformSceneGraph.h"

#include <QCache>
#include <QDataStream>
#include <QFontMetricsF>
#include <QImage>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGRectangleNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace {
using Vertex = QSGGeometry::ColoredPoint2D;
struct Command {
    enum Kind { Rectangle, Image, Geometry } kind{Rectangle};
    QRectF target;
    QRectF source;
    QColor color;
    QImage image;
    QRect textureRect;
    std::vector<Vertex> vertices;
};
struct Texture {
    std::unique_ptr<QSGTexture> texture;
    quint64 bytes{0};
    bool used{false};
};
struct Scene final : QSGClipNode {
    Scene() : rasterCache(16 * 1024) {
        setIsRectangular(true);
        setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4));
        setFlag(QSGNode::OwnsGeometry);
    }
    ~Scene() override {
        // Image nodes borrow textures; destroy them before their resources.
        while (firstChild()) delete firstChild();
    }
    QCache<QByteArray, QImage> rasterCache; // KiB, bounded independently of zoom
    std::map<std::tuple<qint64, int, int>, Texture> textures;
    std::vector<Command::Kind> kinds;
    WaveformSceneGraph::Statistics stats;
    QImage rasterFrame;
    double scale{1.0};
};

Vertex vertex(QPointF point, QColor color, double coverage = 1.0) {
    const double alpha = color.alphaF() * coverage;
    Vertex result;
    result.set(static_cast<float>(point.x()), static_cast<float>(point.y()),
        static_cast<uchar>(std::round(color.red() * alpha)),
        static_cast<uchar>(std::round(color.green() * alpha)),
        static_cast<uchar>(std::round(color.blue() * alpha)),
        static_cast<uchar>(std::round(255 * alpha)));
    return result;
}
void quad(std::vector<Vertex> &vertices, Vertex a, Vertex b, Vertex c, Vertex d) {
    vertices.insert(vertices.end(), {a, b, c, c, b, d});
}

// This adapter implements WaveformEditorItem::paint's drawing vocabulary:
// opaque body images with Source composition, then SourceOver overlays, all
// in viewport coordinates. The root provides the sample path's viewport clip.
// Extend the adapter and pixel comparisons if paint adds transforms, other
// composition modes, or smaller per-primitive clips.
class Engine final : public QPaintEngine {
public:
    Engine(Scene &scene, bool software)
        : QPaintEngine(AlphaBlend | PorterDuff | PainterPaths | Antialiasing
                       | PrimitiveTransform | PixmapTransform),
          scene(scene), software(software) {}
    bool begin(QPaintDevice *device) override {
        setPaintDevice(device); setActive(true); return true;
    }
    bool end() override { setActive(false); return true; }
    Type type() const override { return User; }
    void updateState(const QPaintEngineState &) override {}

    void drawImage(const QRectF &target, const QImage &image, const QRectF &source,
                   Qt::ImageConversionFlags) override {
        if (image.isNull() || target.isEmpty()) return;
        // A cache can be three viewports wide, exceeding common GPU limits.
        // Upload only intersecting, bounded tiles and retain them by image
        // revision and tile origin; scrolling within a tile changes UVs only.
        const int tileSize = software ? std::max(image.width(), image.height()) : 2048;
        const QRectF clipped = source.intersected(image.rect());
        if (clipped.isEmpty()) return;
        const double scaleX = target.width() / source.width();
        const double scaleY = target.height() / source.height();
        const int firstX = static_cast<int>(clipped.left()) / tileSize * tileSize;
        const int firstY = static_cast<int>(clipped.top()) / tileSize * tileSize;
        for (int y = firstY; y < clipped.bottom(); y += tileSize) {
            for (int x = firstX; x < clipped.right(); x += tileSize) {
                Command command;
                command.kind = Command::Image;
                command.textureRect = QRect(x, y, tileSize, tileSize).intersected(image.rect());
                const QRectF slice = clipped.intersected(command.textureRect);
                command.target = painter()->transform().mapRect(QRectF(
                    target.x() + (slice.x() - source.x()) * scaleX,
                    target.y() + (slice.y() - source.y()) * scaleY,
                    slice.width() * scaleX, slice.height() * scaleY));
                command.source = slice.translated(-command.textureRect.topLeft());
                command.image = image;
                commands.push_back(std::move(command));
            }
        }
    }
    void drawPixmap(const QRectF &target, const QPixmap &pixmap, const QRectF &source) override {
        drawImage(target, pixmap.toImage(), source, Qt::AutoColor);
    }
    void rectangle(QRectF rect, QColor color) {
        if (rect.isEmpty() || color.alpha() == 0) return;
        Command command;
        command.target = painter()->transform().mapRect(rect);
        command.color = color;
        commands.push_back(std::move(command));
    }
    void drawRects(const QRect *rects, int count) override {
        for (int i = 0; i < count; ++i) { const QRectF rect(rects[i]); drawRects(&rect, 1); }
    }
    void drawRects(const QRectF *rects, int count) override {
        for (int i = 0; i < count; ++i) {
            const QPen pen = painter()->pen();
            const QBrush brush = painter()->brush();
            if (pen.style() == Qt::NoPen && brush.style() == Qt::SolidPattern) {
                rectangle(rects[i], brush.color());
            } else if (!software && pen.style() == Qt::SolidLine
                && brush.style() == Qt::SolidPattern && brush.color() == pen.color()
                && painter()->testRenderHint(QPainter::Antialiasing)) {
                const double halfPen = std::max(1.0, pen.widthF()) * 0.5;
                const QRectF bounds = painter()->transform().mapRect(
                    rects[i].adjusted(-halfPen, -halfPen, halfPen, halfPen));
                appendFilledRectangle(geometry(), bounds, brush.color(), 0.5 / scene.scale);
            } else {
                QPainterPath path; path.addRect(rects[i]); drawPath(path);
            }
        }
    }
    void drawLines(const QLine *lines, int count) override {
        for (int i = 0; i < count; ++i) { const QLineF line(lines[i]); drawLines(&line, 1); }
    }
    void drawLines(const QLineF *lines, int count) override {
        const QPen pen = painter()->pen();
        if (pen.style() == Qt::NoPen) return;
        for (int i = 0; i < count; ++i) {
            const QLineF line = lines[i];
            if (pen.style() == Qt::SolidLine && !painter()->testRenderHint(QPainter::Antialiasing)
                && (line.dx() == 0 || line.dy() == 0)) {
                const double pixels = std::max(1.0, std::round(pen.widthF() * scene.scale));
                const double thickness = pixels / scene.scale;
                const double offset = std::floor(pixels * 0.5) / scene.scale;
                rectangle(QRectF(std::min(line.x1(), line.x2()) - (line.dy() == 0 ? 0 : offset),
                    std::min(line.y1(), line.y2()) - (line.dx() == 0 ? 0 : offset),
                    line.dx() == 0 ? thickness : std::abs(line.dx()) + 1,
                    line.dy() == 0 ? thickness : std::abs(line.dy()) + 1), pen.color());
            } else {
                QPainterPath path; path.moveTo(line.p1()); path.lineTo(line.p2()); drawPath(path);
            }
        }
    }
    void drawTextItem(const QPointF &position, const QTextItem &text) override {
        const QFont font = text.font();
        const QColor color = painter()->pen().color();
        const QFontMetricsF metrics(font);
        const QRect bounds = metrics.boundingRect(text.text()).adjusted(-2, -2, 2, 2).toAlignedRect();
        if (bounds.isEmpty()) return;
        QByteArray key;
        QDataStream stream(&key, QIODevice::WriteOnly);
        stream << quint8(1) << scene.scale << font << color << text.text();
        QImage *image = scene.rasterCache.object(key);
        QImage uncached;
        if (!image) {
            auto next = std::make_unique<QImage>((QSizeF(bounds.size()) * scene.scale).toSize(), QImage::Format_ARGB32_Premultiplied);
            next->setDevicePixelRatio(scene.scale);
            next->fill(Qt::transparent);
            QPainter raster(next.get()); raster.setFont(font); raster.setPen(color);
            raster.drawText(-bounds.topLeft(), text.text()); raster.end();
            const int cost = std::max(1, static_cast<int>(next->sizeInBytes() / 1024));
            if (cost <= scene.rasterCache.maxCost()) {
                image = next.get();
                scene.rasterCache.insert(key, next.release(), cost);
            } else {
                uncached = std::move(*next);
                image = &uncached;
            }
        }
        drawImage(QRectF(position + bounds.topLeft(), bounds.size()), *image, image->rect(), Qt::AutoColor);
    }
    void drawPath(const QPainterPath &path) override {
        if (path.isEmpty()) return;
        const QPen pen = painter()->pen();
        // Sample curves are sparse vector strokes. Keep them off the raster
        // path too: at high zoom their bounding box is still fullscreen-sized.
        if (!software && painter()->brush().style() == Qt::NoBrush
            && pen.style() == Qt::SolidLine) {
            const auto polygons = path.toSubpathPolygons(painter()->transform());
            for (const QPolygonF &polygon : polygons) appendStroke(geometry(), polygon, pen, 0.5 / scene.scale);
            return;
        }
        // Filled sample markers and rounded label backgrounds are tiny.
        // The software scene graph also uses this exact raster reference for
        // curves, since it does not implement arbitrary geometry materials.
        const double margin = std::max(1.0, pen.widthF()) + 2;
        const QRect bounds = path.boundingRect().adjusted(-margin, -margin, margin, margin).toAlignedRect();
        if (bounds.isEmpty()) return;
        const QPainterPath normalized = QTransform::fromTranslate(-bounds.x(), -bounds.y()).map(path);
        QByteArray key;
        QDataStream stream(&key, QIODevice::WriteOnly);
        stream << quint8(2) << scene.scale << normalized << bounds.size() << pen << painter()->brush()
               << static_cast<int>(painter()->renderHints());
        QImage *image = scene.rasterCache.object(key);
        QImage uncached;
        if (!image) {
            uncached = QImage((QSizeF(bounds.size()) * scene.scale).toSize(), QImage::Format_ARGB32_Premultiplied);
            uncached.setDevicePixelRatio(scene.scale);
            uncached.fill(Qt::transparent);
            QPainter raster(&uncached); raster.setPen(pen); raster.setBrush(painter()->brush());
            raster.setRenderHints(painter()->renderHints()); raster.drawPath(normalized); raster.end();
            const int cost = std::max(1, static_cast<int>(uncached.sizeInBytes() / 1024));
            if (cost <= scene.rasterCache.maxCost()) {
                image = new QImage(uncached); scene.rasterCache.insert(key, image, cost);
            } else image = &uncached;
        }
        drawImage(bounds, *image, image->rect(), Qt::AutoColor);
    }
    void drawPolygon(const QPointF *points, int count, PolygonDrawMode mode) override {
        if (count == 0) return;
        QPainterPath path; path.moveTo(points[0]);
        for (int i = 1; i < count; ++i) path.lineTo(points[i]);
        if (mode != PolylineMode) path.closeSubpath();
        drawPath(path);
    }
    void drawPolygon(const QPoint *points, int count, PolygonDrawMode mode) override {
        QPolygonF polygon; polygon.reserve(count);
        for (int i = 0; i < count; ++i) polygon.push_back(points[i]);
        drawPolygon(polygon.constData(), count, mode);
    }

    std::vector<Command> commands;
private:
    std::vector<Vertex> &geometry() {
        if (commands.empty() || commands.back().kind != Command::Geometry) {
            Command command; command.kind = Command::Geometry;
            commands.push_back(std::move(command));
        }
        return commands.back().vertices;
    }
    static void appendFilledRectangle(std::vector<Vertex> &vertices, QRectF bounds, QColor color, double fringe) {
        const QRectF inner = bounds.adjusted(fringe, fringe, -fringe, -fringe);
        const QRectF outer = bounds.adjusted(-fringe, -fringe, fringe, fringe);
        quad(vertices, vertex(inner.topLeft(), color), vertex(inner.topRight(), color),
            vertex(inner.bottomLeft(), color), vertex(inner.bottomRight(), color));
        const QPointF inside[] = {inner.topLeft(), inner.topRight(), inner.bottomRight(), inner.bottomLeft()};
        const QPointF outside[] = {outer.topLeft(), outer.topRight(), outer.bottomRight(), outer.bottomLeft()};
        for (int i = 0; i < 4; ++i) {
            const int next = (i + 1) % 4;
            quad(vertices, vertex(outside[i], color, 0), vertex(inside[i], color),
                vertex(outside[next], color, 0), vertex(inside[next], color));
        }
    }
    static void appendStroke(std::vector<Vertex> &vertices, const QPolygonF &points, const QPen &pen, double fringe) {
        if (points.size() < 2) return;
        vertices.reserve(vertices.size() + static_cast<std::size_t>(points.size() - 1) * 18);
        const double half = std::max(1.0, pen.widthF()) * 0.5;
        const double inner = std::max(0.0, half - fringe);
        const double outer = half + fringe;
        const QColor color = pen.color();
        auto normal = [&](qsizetype index) {
            const QPointF direction = points[std::min(index + 1, points.size() - 1)]
                - points[std::max<qsizetype>(index - 1, 0)];
            const double length = std::hypot(direction.x(), direction.y());
            return length > 0 ? QPointF(-direction.y() / length, direction.x() / length) : QPointF(0, 1);
        };
        QPointF previousNormal = normal(0);
        for (qsizetype i = 1; i < points.size(); ++i) {
            const QPointF nextNormal = normal(i);
            const QPointF a = points[i - 1]; const QPointF b = points[i];
            quad(vertices, vertex(a - previousNormal * inner, color), vertex(a + previousNormal * inner, color),
                vertex(b - nextNormal * inner, color), vertex(b + nextNormal * inner, color));
            quad(vertices, vertex(a - previousNormal * outer, color, 0), vertex(a - previousNormal * inner, color),
                vertex(b - nextNormal * outer, color, 0), vertex(b - nextNormal * inner, color));
            quad(vertices, vertex(a + previousNormal * inner, color), vertex(a + previousNormal * outer, color, 0),
                vertex(b + nextNormal * inner, color), vertex(b + nextNormal * outer, color, 0));
            previousNormal = nextNormal;
        }
    }
    Scene &scene;
    bool software;
};
class Device final : public QPaintDevice {
public:
    Device(Scene &scene, QSize size, bool software) : engine(scene, software), size(size) {}
    QPaintEngine *paintEngine() const override { return const_cast<Engine *>(&engine); }
    mutable Engine engine;
protected:
    int metric(PaintDeviceMetric metric) const override {
        switch (metric) {
        case PdmWidth: return size.width();
        case PdmHeight: return size.height();
        case PdmWidthMM: return qRound(size.width() * 25.4 / 96);
        case PdmHeightMM: return qRound(size.height() * 25.4 / 96);
        case PdmDpiX: case PdmDpiY: case PdmPhysicalDpiX: case PdmPhysicalDpiY: return 96;
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return static_cast<int>(devicePixelRatioFScale());
        case PdmDepth: return 32;
        case PdmNumColors: return 16'777'216;
        default: return 0;
        }
    }
private:
    QSize size;
};
}

QSGNode *WaveformSceneGraph::render(QSGNode *previous, QQuickWindow *window, QSize size,
    const std::function<void(QPainter *)> &paint, bool rasterize) {
    if (!window || size.isEmpty()) { delete previous; return nullptr; }
    auto *scene = static_cast<Scene *>(previous);
    if (!scene) scene = new Scene;
    const QRectF bounds(QPointF(), size);
    if (scene->clipRect() != bounds) {
        scene->setClipRect(bounds);
        QSGGeometry::updateRectGeometry(scene->geometry(), bounds);
        scene->markDirty(QSGNode::DirtyGeometry);
    }
    const bool software = window->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
    scene->scale = window->effectiveDevicePixelRatio();
    Device device(*scene, size, software);
    QPainter painter(&device);
    if (rasterize) {
        // The software backend has no arbitrary geometry material. Preserve
        // its original single-raster sample rendering, rather than creating
        // a separate temporary image for every curve and sample marker.
        while (scene->firstChild()) delete scene->firstChild();
        scene->textures.clear();
        scene->kinds.clear();
        const QSize pixels = (QSizeF(size) * scene->scale).toSize();
        if (scene->rasterFrame.size() != pixels) scene->rasterFrame = QImage(pixels, QImage::Format_RGB32);
        scene->rasterFrame.setDevicePixelRatio(scene->scale);
        QPainter raster(&scene->rasterFrame); paint(&raster); raster.end();
        painter.drawImage(bounds, scene->rasterFrame);
    } else {
        scene->rasterFrame = QImage();
        paint(&painter);
    }
    painter.end();
    scene->stats = {};
    for (auto &[key, texture] : scene->textures) { Q_UNUSED(key); texture.used = false; }
    QSGNode *node = scene->firstChild();
    std::vector<Command::Kind> kinds;
    kinds.reserve(device.engine.commands.size());
    for (const Command &command : device.engine.commands) {
        const std::size_t index = kinds.size();
        const bool matching = node && index < scene->kinds.size() && scene->kinds[index] == command.kind;
        if (node && !matching) {
            QSGNode *next = node->nextSibling(); delete node; node = next;
        }
        QSGNode *current = matching ? node : nullptr;
        if (!current) {
            if (command.kind == Command::Image) current = window->createImageNode();
            else if (command.kind == Command::Rectangle) current = window->createRectangleNode();
            else {
                auto *geometryNode = new QSGGeometryNode;
                geometryNode->setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0));
                geometryNode->geometry()->setDrawingMode(QSGGeometry::DrawTriangles);
                geometryNode->setMaterial(new QSGVertexColorMaterial);
                geometryNode->setFlag(QSGNode::OwnsGeometry); geometryNode->setFlag(QSGNode::OwnsMaterial);
                current = geometryNode;
            }
        }
        if (!current) {
            delete scene;
            return nullptr;
        }
        if (command.kind == Command::Image) {
            auto &entry = scene->textures[{command.image.cacheKey(), command.textureRect.x(), command.textureRect.y()}];
            if (!entry.texture) {
                const QImage pixels = command.textureRect == command.image.rect()
                    ? command.image : command.image.copy(command.textureRect);
                entry.texture.reset(window->createTextureFromImage(pixels, QQuickWindow::TextureCanUseAtlas));
                if (!entry.texture) {
                    if (!matching) delete current;
                    delete scene;
                    return nullptr;
                }
                entry.bytes = static_cast<quint64>(pixels.sizeInBytes());
                ++scene->stats.textureUploads; scene->stats.uploadedBytes += entry.bytes;
            }
            entry.used = true;
            auto *image = static_cast<QSGImageNode *>(current);
            image->setTexture(entry.texture.get()); image->setOwnsTexture(false);
            image->setFiltering(QSGTexture::Nearest);
            image->setRect(command.target); image->setSourceRect(command.source);
        } else if (command.kind == Command::Rectangle) {
            auto *rectangle = static_cast<QSGRectangleNode *>(current);
            rectangle->setRect(command.target); rectangle->setColor(command.color);
        } else {
            auto *geometry = static_cast<QSGGeometryNode *>(current);
            geometry->geometry()->allocate(static_cast<int>(command.vertices.size()));
            std::copy(command.vertices.begin(), command.vertices.end(), geometry->geometry()->vertexDataAsColoredPoint2D());
            geometry->markDirty(QSGNode::DirtyGeometry);
            scene->stats.geometryVertices += static_cast<int>(command.vertices.size());
        }
        if (!matching) {
            if (node) scene->insertChildNodeBefore(current, node); else scene->appendChildNode(current);
        } else node = node->nextSibling();
        kinds.push_back(command.kind);
    }
    while (node) { QSGNode *next = node->nextSibling(); delete node; node = next; }
    scene->kinds = std::move(kinds);
    for (auto iterator = scene->textures.begin(); iterator != scene->textures.end();) {
        if (!iterator->second.used) iterator = scene->textures.erase(iterator);
        else { scene->stats.residentBytes += iterator->second.bytes; ++iterator; }
    }
    scene->stats.commands = static_cast<int>(scene->kinds.size());
    return scene;
}
WaveformSceneGraph::Statistics WaveformSceneGraph::statistics(const QSGNode *node) {
    return node ? static_cast<const Scene *>(node)->stats : Statistics{};
}
