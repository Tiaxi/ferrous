// SPDX-License-Identifier: GPL-3.0-or-later
#include "WaveformSceneGraph.h"

#include <QCache>
#include <QDataStream>
#include <QFont>
#include <QFontMetricsF>
#include <QHashFunctions>
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
#include <QSGTransformNode>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using Vertex = QSGGeometry::ColoredPoint2D;
struct Command {
    // Layer: the chunks of geometry kept from frame to frame.
    enum Kind { Rectangle, Image, Geometry, Layer } kind{Rectangle};
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
// A label as drawn: its text, font and color at a scale. Labels repeat
// from frame to frame, so finding one must cost less than drawing it.
struct LabelKey {
    QString text;
    QFont font;
    QRgb color{0};
    double scale{1.0};
    bool operator==(const LabelKey &other) const {
        return color == other.color && scale == other.scale && text == other.text
            && font == other.font;
    }
};
size_t qHash(const LabelKey &key, size_t seed = 0) {
    return qHashMulti(seed, key.text, key.font, key.color, key.scale);
}
struct Label {
    QImage image;
    // Where the image goes relative to the text's origin.
    QRect bounds;
};
struct Layer;
struct Scene final : QSGClipNode {
    Scene() : rasterCache(16 * 1024), labelCache(4 * 1024) {
        setIsRectangular(true);
        setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4));
        setFlag(QSGNode::OwnsGeometry);
    }
    ~Scene() override;
    QCache<QByteArray, QImage> rasterCache; // KiB, bounded independently of zoom
    QCache<LabelKey, Label> labelCache; // KiB
    std::map<std::tuple<qint64, int, int>, Texture> textures;
    std::vector<Command::Kind> kinds;
    WaveformSceneGraph::Statistics stats;
    QImage rasterFrame;
    double scale{1.0};
    // The chunks, a child where the frame draws them; none while no frame
    // draws chunks.
    Layer *layer{nullptr};
};

// A chunk's geometry at its translation. Qt's renderer makes a transform
// node with enough vertices a batch root when it first moves, which
// rebuilds all its batches: from then on, moving it changes a matrix only,
// and new geometry in it uploads that geometry alone. A slot is therefore
// kept, and given the next chunk, while the layer lasts; adding or taking
// away nodes would rebuild the batches of the whole layer.
struct Slot final : QSGTransformNode {
    std::vector<QSGGeometryNode *> nodes;
    int vertices{0};
};
struct Layer final : QSGNode {
    explicit Layer(Scene &scene) : scene(scene) {}
    ~Layer() override {
        if (scene.layer == this) scene.layer = nullptr;
    }
    struct Chunk {
        WaveformSceneGraph::Chunks::Stamp stamp{};
        // None for a chunk with no geometry.
        Slot *slot{nullptr};
    };
    Scene &scene;
    QByteArray key;
    std::map<qint64, Chunk> chunks;
    // Slots no chunk has, their geometry emptied.
    std::vector<Slot *> free;
};
Scene::~Scene() {
    // A layer a failed frame made, or moved, before it was put in place.
    if (layer != nullptr && layer->parent() == nullptr) delete layer;
    // Image nodes borrow textures; destroy them before their resources.
    while (firstChild()) delete firstChild();
}

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
// The most vertices a geometry node holds. Qt's renderer merges nodes of
// vertex-colored triangles into batches indexed with 16 bits, starting a
// new draw only between nodes; the vertices of one node past 65,535 would
// be drawn as the first ones again, and the lanes they belong to not at
// all. A multiple of 6, a quad's triangles, below 0xffff, which some APIs
// take for a primitive restart.
constexpr std::size_t kMaximumGeometryVertices = 65'532;
// Polyline bends whose half has a cosine below this (sharper than about
// 52 degrees) are bevelled: a miter's tip would reach beyond the bevel's
// edge by more than a tenth of the stroke's width.
constexpr double kMiterHalfBendCos = 0.9;

// A node of the scene: a command, or for geometry, a part of its vertices
// that one node holds.
struct Piece {
    const Command *command{nullptr};
    std::size_t firstVertex{0};
    std::size_t vertexCount{0};
};

std::vector<Piece> pieces(const std::vector<Command> &commands) {
    std::vector<Piece> result;
    result.reserve(commands.size());
    for (const Command &command : commands) {
        if (command.kind != Command::Geometry) {
            result.push_back({&command, 0, 0});
            continue;
        }
        for (std::size_t first = 0; first < command.vertices.size();
             first += kMaximumGeometryVertices) {
            result.push_back({&command, first,
                std::min(kMaximumGeometryVertices, command.vertices.size() - first)});
        }
    }
    return result;
}

void quad(std::vector<Vertex> &vertices, Vertex a, Vertex b, Vertex c, Vertex d) {
    vertices.insert(vertices.end(), {a, b, c, c, b, d});
}

QSGGeometryNode *newGeometryNode() {
    auto *node = new QSGGeometryNode;
    node->setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0));
    node->geometry()->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setMaterial(new QSGVertexColorMaterial);
    node->setFlag(QSGNode::OwnsGeometry);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

// Gives `slot` these vertices, in nodes of at most
// kMaximumGeometryVertices; nodes it no longer needs are emptied, not
// taken away.
void setSlotGeometry(Slot &slot, const std::vector<Vertex> &vertices,
    WaveformSceneGraph::Statistics &stats) {
    std::size_t index = 0;
    for (std::size_t first = 0; first < vertices.size(); first += kMaximumGeometryVertices, ++index) {
        if (index == slot.nodes.size()) {
            slot.nodes.push_back(newGeometryNode());
            slot.appendChildNode(slot.nodes.back());
        }
        const auto count = static_cast<int>(std::min(kMaximumGeometryVertices, vertices.size() - first));
        QSGGeometry *geometry = slot.nodes[index]->geometry();
        geometry->allocate(count);
        std::copy_n(vertices.begin() + static_cast<std::ptrdiff_t>(first), count,
            geometry->vertexDataAsColoredPoint2D());
        slot.nodes[index]->markDirty(QSGNode::DirtyGeometry);
        stats.largestGeometryVertices = std::max(stats.largestGeometryVertices, count);
    }
    for (; index < slot.nodes.size(); ++index) {
        if (slot.nodes[index]->geometry()->vertexCount() == 0) continue;
        slot.nodes[index]->geometry()->allocate(0);
        slot.nodes[index]->markDirty(QSGNode::DirtyGeometry);
    }
    slot.vertices = static_cast<int>(vertices.size());
}

void releaseSlot(Layer &layer, Slot *slot) {
    if (slot == nullptr) return;
    for (QSGGeometryNode *node : slot->nodes) {
        if (node->geometry()->vertexCount() == 0) continue;
        node->geometry()->allocate(0);
        node->markDirty(QSGNode::DirtyGeometry);
    }
    slot->vertices = 0;
    layer.free.push_back(slot);
}

Slot *acquireSlot(Layer &layer) {
    if (!layer.free.empty()) {
        Slot *slot = layer.free.back();
        layer.free.pop_back();
        return slot;
    }
    auto *slot = new Slot;
    layer.appendChildNode(slot);
    return slot;
}

// This adapter implements WaveformEditorItem::paint's drawing vocabulary:
// opaque body images with Source composition, then SourceOver overlays, all
// in viewport coordinates. The root provides the sample path's viewport clip.
// Extend the adapter and pixel comparisons if paint adds transforms, other
// composition modes, or smaller per-primitive clips.
class Engine final : public QPaintEngine {
public:
    Engine(Scene &scene, QSize size, bool software)
        : QPaintEngine(AlphaBlend | PorterDuff | PainterPaths | Antialiasing
                       | PrimitiveTransform | PixmapTransform),
          software(software), scene(scene), size(size) {}
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
        const int tileSize = software ? std::max(image.width(), image.height()) : WaveformSceneGraph::kTextureSize;
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
        // A batch of filled rectangles, such as the columns of a waveform,
        // is one geometry rather than a node each.
        const bool batch = count > 1 && !software;
        for (int i = 0; i < count; ++i) {
            const QPen pen = painter()->pen();
            const QBrush brush = painter()->brush();
            if (pen.style() == Qt::NoPen && brush.style() == Qt::SolidPattern && batch) {
                const QRectF rect = painter()->transform().mapRect(rects[i]);
                const QColor color = brush.color();
                if (rect.isEmpty() || color.alpha() == 0) continue;
                quad(geometry(), vertex(rect.topLeft(), color), vertex(rect.topRight(), color),
                    vertex(rect.bottomLeft(), color), vertex(rect.bottomRight(), color));
            } else if (pen.style() == Qt::NoPen && brush.style() == Qt::SolidPattern) {
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
        LabelKey key{text.text(), text.font(), painter()->pen().color().rgba(), scene.scale};
        const Label *label = scene.labelCache.object(key);
        Label uncached;
        if (!label) {
            // Laid out only when it is first drawn.
            ++labelRasters;
            const QFontMetricsF metrics(key.font);
            uncached.bounds = metrics.boundingRect(key.text).adjusted(-2, -2, 2, 2).toAlignedRect();
            if (!uncached.bounds.isEmpty()) {
                uncached.image = QImage((QSizeF(uncached.bounds.size()) * scene.scale).toSize(),
                    QImage::Format_ARGB32_Premultiplied);
                uncached.image.setDevicePixelRatio(scene.scale);
                uncached.image.fill(Qt::transparent);
                QPainter raster(&uncached.image); raster.setFont(key.font);
                raster.setPen(QColor::fromRgba(key.color));
                raster.drawText(-uncached.bounds.topLeft(), key.text); raster.end();
            }
            const int cost = std::max(1, static_cast<int>(uncached.image.sizeInBytes() / 1024));
            if (cost <= scene.labelCache.maxCost()) {
                auto cached = std::make_unique<Label>(std::move(uncached));
                label = cached.get();
                scene.labelCache.insert(std::move(key), cached.release(), cost);
            } else {
                label = &uncached;
            }
        }
        if (label->bounds.isEmpty()) return;
        drawImage(QRectF(position + label->bounds.topLeft(), label->bounds.size()), label->image,
            label->image.rect(), Qt::AutoColor);
    }
    void drawPath(const QPainterPath &path) override {
        const auto segments = std::exchange(strokeSegments, std::nullopt);
        if (path.isEmpty()) return;
        const QPen pen = painter()->pen();
        // Sample curves are sparse vector strokes. Keep them off the raster
        // path too: at high zoom their bounding box is still fullscreen-sized.
        if (!software && painter()->brush().style() == Qt::NoBrush
            && pen.style() == Qt::SolidLine) {
            if (segments) {
                appendPathSegments(path, pen, segments->first, segments->second);
                return;
            }
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
        const auto segments = std::exchange(strokeSegments, std::nullopt);
        if (count == 0) return;
        const QPen pen = painter()->pen();
        if (mode == PolylineMode && !software && pen.style() == Qt::SolidLine) {
            // Samples joined by straight segments: stroked as they are,
            // without a path to build and flatten.
            QPolygonF polyline(count);
            for (int i = 0; i < count; ++i) polyline[i] = painter()->transform().map(points[i]);
            appendPolylineStroke(geometry(), polyline, pen, 0.5 / scene.scale,
                segments ? segments->first : 0, segments ? segments->second : count);
            return;
        }
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

    // Builds the chunks the layer lacks, keeps and moves the others, and
    // puts the layer where the frame draws it.
    void drawChunks(const WaveformSceneGraph::Chunks &chunks) {
        if (scene.layer == nullptr) scene.layer = new Layer(scene);
        Layer &layer = *scene.layer;
        if (layer.key != chunks.key) {
            for (auto &[index, chunk] : layer.chunks) {
                Q_UNUSED(index);
                releaseSlot(layer, chunk.slot);
                ++chunkStats.chunksDropped;
            }
            layer.chunks.clear();
            layer.key = chunks.key;
        }
        for (auto it = layer.chunks.begin(); it != layer.chunks.end();) {
            if (it->first >= chunks.first && it->first <= chunks.last) {
                ++it;
                continue;
            }
            releaseSlot(layer, it->second.slot);
            ++chunkStats.chunksDropped;
            it = layer.chunks.erase(it);
        }
        for (qint64 index = chunks.first; index <= chunks.last; ++index) {
            const WaveformSceneGraph::Chunks::Stamp stamp = chunks.stamp(index);
            auto [it, added] = layer.chunks.try_emplace(index);
            Layer::Chunk &chunk = it->second;
            if (added || chunk.stamp != stamp) {
                chunk.stamp = stamp;
                const std::vector<Vertex> vertices = chunkGeometry(chunks, index);
                if (vertices.empty()) {
                    releaseSlot(layer, std::exchange(chunk.slot, nullptr));
                } else {
                    if (chunk.slot == nullptr) chunk.slot = acquireSlot(layer);
                    setSlotGeometry(*chunk.slot, vertices, chunkStats);
                }
                ++chunkStats.chunksBuilt;
                chunkStats.uploadedVertices += static_cast<int>(vertices.size());
            }
            ++chunkStats.chunks;
            if (chunk.slot == nullptr) continue;
            QMatrix4x4 matrix;
            matrix.translate(static_cast<float>(chunks.offset(index)), 0.0F);
            if (chunk.slot->matrix() != matrix) {
                chunk.slot->setMatrix(matrix);
                ++chunkStats.chunksMoved;
            }
            chunkStats.geometryVertices += chunk.slot->vertices;
            for (const QSGGeometryNode *node : chunk.slot->nodes) {
                chunkStats.largestGeometryVertices = std::max(
                    chunkStats.largestGeometryVertices, node->geometry()->vertexCount());
            }
        }
        Command command;
        command.kind = Command::Layer;
        commands.push_back(std::move(command));
    }

    std::vector<Command> commands;
    int labelRasters{0};
    // The segments of the next stroke that get geometry; see
    // WaveformSceneGraph::strokeOnlySegments.
    std::optional<std::pair<qsizetype, qsizetype>> strokeSegments;
    // The chunks' part of the frame's statistics.
    WaveformSceneGraph::Statistics chunkStats;
    const bool software;
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
    // The stroke of a flattened curve, or of its pieces `first` up to
    // `end`, those from each of these points to the next.
    static void appendStroke(std::vector<Vertex> &vertices, const QPolygonF &points, const QPen &pen, double fringe,
        qsizetype first = 0, qsizetype end = -1) {
        if (points.size() < 2) return;
        if (end < 0 || end > points.size() - 1) end = points.size() - 1;
        first = std::max<qsizetype>(0, first);
        if (first >= end) return;
        vertices.reserve(vertices.size() + static_cast<std::size_t>(end - first) * 18);
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
        QPointF previousNormal = normal(first);
        for (qsizetype i = first + 1; i <= end; ++i) {
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
    // A polyline's stroke, each segment with its own normal, so that it
    // keeps its width however steep the segments are and however sharply
    // they turn: alternating samples make nearly vertical segments, whose
    // neighbours' chord is level. Gentle bends are mitred, which adds no
    // vertices; sharper ones are bevelled, as QPainter's default pen joins
    // lines, with the wedge outside the bend filled. Only the segments
    // `first` up to `end` get geometry, joined to the others as they would
    // be.
    static void appendPolylineStroke(std::vector<Vertex> &vertices, const QPolygonF &points, const QPen &pen, double fringe,
        qsizetype first, qsizetype end) {
        if (points.size() < 2) return;
        const auto segments = static_cast<std::size_t>(points.size() - 1);
        const auto from = static_cast<std::size_t>(std::max<qsizetype>(0, first));
        const auto to = std::min(segments, static_cast<std::size_t>(std::max<qsizetype>(0, end)));
        if (from >= to) return;
        vertices.reserve(vertices.size() + (to - from) * 18);
        const double half = std::max(1.0, pen.widthF()) * 0.5;
        const double inner = std::max(0.0, half - fringe);
        const double outer = half + fringe;
        const QColor color = pen.color();
        std::vector<QPointF> normals(segments);
        for (std::size_t i = 0; i < segments; ++i) {
            const QPointF direction = points[static_cast<qsizetype>(i) + 1] - points[static_cast<qsizetype>(i)];
            const double length = std::hypot(direction.x(), direction.y());
            normals[i] = length > 0 ? QPointF(-direction.y() / length, direction.x() / length)
                : (i > 0 ? normals[i - 1] : QPointF(0, 1));
        }
        // The join at the end of segment i: its miter, the offset of a unit
        // of width there, or a bevel.
        const auto join = [&normals, segments](std::size_t i) -> std::pair<QPointF, bool> {
            if (i + 1 >= segments) return {normals[i], false};
            // cos² of half the bend: a miter is 1/cos as long as the width,
            // along the bisector of the two normals.
            const QPointF sum = normals[i] + normals[i + 1];
            const double halfBendCos2 = (sum.x() * sum.x() + sum.y() * sum.y()) * 0.25;
            if (halfBendCos2 >= kMiterHalfBendCos * kMiterHalfBendCos) return {sum / (2.0 * halfBendCos2), false};
            return {normals[i], true};
        };
        // The offset of a unit of width at the segment's start.
        QPointF start = normals[from];
        if (from > 0) {
            const auto [miter, bevelled] = join(from - 1);
            if (!bevelled) start = miter;
        }
        for (std::size_t i = from; i < to; ++i) {
            const QPointF normal = normals[i];
            const QPointF a = points[static_cast<qsizetype>(i)];
            const QPointF b = points[static_cast<qsizetype>(i) + 1];
            const auto [end, bevel] = join(i);
            quad(vertices, vertex(a - start * inner, color), vertex(a + start * inner, color),
                vertex(b - end * inner, color), vertex(b + end * inner, color));
            quad(vertices, vertex(a - start * outer, color, 0), vertex(a - start * inner, color),
                vertex(b - end * outer, color, 0), vertex(b - end * inner, color));
            quad(vertices, vertex(a + start * inner, color), vertex(a + start * outer, color, 0),
                vertex(b + end * inner, color), vertex(b + end * outer, color, 0));
            if (bevel) {
                // The wedge on the outside of the bend, between the two
                // segments' ends: its inside, then its edge.
                const QPointF next = normals[i + 1];
                const QPointF turn = points[static_cast<qsizetype>(i) + 2] - b;
                const double cross = (b - a).x() * turn.y() - (b - a).y() * turn.x();
                const double side = cross > 0 ? -1.0 : 1.0;
                vertices.insert(vertices.end(), {vertex(b, color),
                    vertex(b + normal * side * inner, color), vertex(b + next * side * inner, color)});
                quad(vertices, vertex(b + normal * side * inner, color), vertex(b + normal * side * outer, color, 0),
                    vertex(b + next * side * inner, color), vertex(b + next * side * outer, color, 0));
            }
            start = bevel ? normals[i + 1] : end;
        }
    }
    // A chunk's geometry, as its paint function draws it.
    std::vector<Vertex> chunkGeometry(const WaveformSceneGraph::Chunks &chunks, qint64 index);
    // A path's cubics and lines, from on-curve point `first` up to `end`:
    // each flattened as the whole path's are, with the others as their
    // neighbours.
    void appendPathSegments(const QPainterPath &path, const QPen &pen, qsizetype first, qsizetype end) {
        const QTransform transform = painter()->transform();
        QPolygonF polygon;
        // Where each on-curve point is in the polygon.
        std::vector<qsizetype> starts;
        for (int i = 0; i < path.elementCount(); ++i) {
            const QPainterPath::Element element = path.elementAt(i);
            const QPointF point = QPointF(element.x, element.y) * transform;
            if (element.type == QPainterPath::MoveToElement) {
                // drawDetailSlice draws a lane as one subpath.
                if (!polygon.isEmpty()) break;
                polygon.append(point);
            } else if (element.type == QPainterPath::LineToElement) {
                polygon.append(point);
            } else if (element.type == QPainterPath::CurveToElement && i + 2 < path.elementCount()) {
                // As toSubpathPolygons flattens it: on its own, from the
                // point before it.
                QPainterPath cubic;
                const QPainterPath::Element from = path.elementAt(i - 1);
                const QPainterPath::Element control = path.elementAt(i + 1);
                const QPainterPath::Element to = path.elementAt(i + 2);
                cubic.moveTo(from.x, from.y);
                cubic.cubicTo(element.x, element.y, control.x, control.y, to.x, to.y);
                const QList<QPolygonF> flattened = cubic.toSubpathPolygons(transform);
                if (!flattened.isEmpty()) polygon.append(flattened.front().mid(1));
                else polygon.append(QPointF(to.x, to.y) * transform);
                i += 2;
            } else {
                continue;
            }
            starts.push_back(polygon.size() - 1);
        }
        const auto points = static_cast<qsizetype>(starts.size());
        first = std::clamp<qsizetype>(first, 0, points);
        end = std::clamp<qsizetype>(end, first, points);
        if (end - first < 1 || first + 1 >= points) return;
        appendStroke(geometry(), polygon, pen, 0.5 / scene.scale, starts[static_cast<std::size_t>(first)],
            starts[static_cast<std::size_t>(std::min(end, points - 1))]);
    }
    Scene &scene;
    QSize size;
};
class Device final : public QPaintDevice {
public:
    Device(Scene &scene, QSize size, bool software) : engine(scene, size, software), size(size) {}
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

std::vector<Vertex> Engine::chunkGeometry(const WaveformSceneGraph::Chunks &chunks, qint64 index) {
    Device device(scene, size, false);
    QPainter painter(&device);
    chunks.paint(painter, index);
    painter.end();
    std::vector<Vertex> vertices;
    for (Command &command : device.engine.commands) {
        if (command.kind == Command::Geometry) {
            // Moved, not copied, where it is the first: usually the only one.
            if (vertices.empty()) vertices = std::move(command.vertices);
            else vertices.insert(vertices.end(), command.vertices.begin(), command.vertices.end());
        } else if (command.kind == Command::Rectangle) {
            // A lone marker: its quad, as a batch of them makes it.
            const QRectF &rect = command.target;
            quad(vertices, vertex(rect.topLeft(), command.color), vertex(rect.topRight(), command.color),
                vertex(rect.bottomLeft(), command.color), vertex(rect.bottomRight(), command.color));
        }
    }
    return vertices;
}
}

bool WaveformSceneGraph::drawChunks(QPainter &painter, const Chunks &chunks) {
    auto *engine = dynamic_cast<Engine *>(painter.paintEngine());
    if (engine == nullptr || engine->software) return false;
    engine->drawChunks(chunks);
    return true;
}

void WaveformSceneGraph::strokeOnlySegments(QPainter &painter, qsizetype first, qsizetype end) {
    if (auto *engine = dynamic_cast<Engine *>(painter.paintEngine())) engine->strokeSegments = {{first, end}};
}

QSGNode *WaveformSceneGraph::render(QSGNode *previous, QQuickWindow *window, QSize size,
    const std::function<void(QPainter *)> &paint, bool rasterize, Primitives primitives) {
    if (!window || size.isEmpty()) { delete previous; return nullptr; }
    auto *scene = static_cast<Scene *>(previous);
    if (!scene) scene = new Scene;
    const QRectF bounds(QPointF(), size);
    if (scene->clipRect() != bounds) {
        scene->setClipRect(bounds);
        QSGGeometry::updateRectGeometry(scene->geometry(), bounds);
        scene->markDirty(QSGNode::DirtyGeometry);
    }
    const bool software = primitives == Primitives::ForWindow
        && window->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
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
    scene->stats = device.engine.chunkStats;
    scene->stats.labelRasters = device.engine.labelRasters;
    for (auto &[key, texture] : scene->textures) { Q_UNUSED(key); texture.used = false; }
    const std::vector<Piece> framePieces = pieces(device.engine.commands);
    if (scene->layer != nullptr && scene->layer->parent() == scene) {
        // The layer stays in place while the frame draws it there; one it
        // draws elsewhere moves there, and one it does not draw goes, with
        // its chunks, as the nodes of other commands do.
        const auto drawn = std::find_if(framePieces.begin(), framePieces.end(),
            [](const Piece &piece) { return piece.command->kind == Command::Layer; });
        const auto kept = std::find(scene->kinds.begin(), scene->kinds.end(), Command::Layer);
        if (drawn != framePieces.end() && kept != scene->kinds.end()
            && drawn - framePieces.begin() != kept - scene->kinds.begin()) {
            scene->removeChildNode(scene->layer);
            scene->kinds.erase(kept);
        }
    }
    QSGNode *node = scene->firstChild();
    std::vector<Command::Kind> kinds;
    kinds.reserve(framePieces.size());
    for (const Piece &piece : framePieces) {
        const Command &command = *piece.command;
        const std::size_t index = kinds.size();
        const bool matching = node && index < scene->kinds.size() && scene->kinds[index] == command.kind
            && (command.kind != Command::Layer || node == scene->layer);
        if (node && !matching) {
            QSGNode *next = node->nextSibling(); delete node; node = next;
        }
        QSGNode *current = matching ? node : nullptr;
        if (!current) {
            if (command.kind == Command::Image) current = window->createImageNode();
            else if (command.kind == Command::Rectangle) current = window->createRectangleNode();
            else if (command.kind == Command::Layer) current = scene->layer;
            else current = newGeometryNode();
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
        } else if (command.kind == Command::Geometry) {
            auto *geometry = static_cast<QSGGeometryNode *>(current);
            const auto first = command.vertices.begin() + static_cast<std::ptrdiff_t>(piece.firstVertex);
            const int count = static_cast<int>(piece.vertexCount);
            geometry->geometry()->allocate(count);
            std::copy(first, first + count, geometry->geometry()->vertexDataAsColoredPoint2D());
            geometry->markDirty(QSGNode::DirtyGeometry);
            scene->stats.geometryVertices += count;
            scene->stats.uploadedVertices += count;
            scene->stats.largestGeometryVertices = std::max(
                scene->stats.largestGeometryVertices, count);
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
