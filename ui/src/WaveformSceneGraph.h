// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QSize>
#include <QtGlobal>
#include <functional>

class QPainter;
class QQuickWindow;
class QSGNode;

// Render-thread adapter for the waveform's existing drawing vocabulary.
// Images stay resident, lines/curves become geometry, and only small labels
// are rasterized. Resources belong to the returned node, never the GUI item.
namespace WaveformSceneGraph {
// The largest image, in device pixels each way, that becomes a texture of
// its own; a larger one is copied into textures of this size.
inline constexpr int kTextureSize = 2048;
// Per-frame texture creation requests and retained image payload sizes. These
// exclude GPU submission, atlas allocation and driver overhead.
struct Statistics {
    quint64 textureUploads{0};
    quint64 uploadedBytes{0};
    quint64 residentBytes{0};
    int commands{0};
    // The vertices the frame draws, its chunks' included.
    int geometryVertices{0};
    // The most vertices one geometry node holds.
    int largestGeometryVertices{0};
    // The vertices the frame writes into geometry nodes: its own, and those
    // of the chunks it builds. Kept chunks write none.
    int uploadedVertices{0};
    // Labels laid out and rasterized because no earlier frame drew them.
    int labelRasters{0};
    // The chunks the frame shows, those of them it builds, those it drops,
    // and those whose translation it changes.
    int chunks{0};
    int chunksBuilt{0};
    int chunksDropped{0};
    int chunksMoved{0};
};
// What lines and curves become: what the window's renderer draws, or, for
// tests, geometry as for a native renderer under any renderer, which builds
// the nodes without drawing them.
enum class Primitives { ForWindow, Geometry };
QSGNode *render(QSGNode *previous, QQuickWindow *window, QSize size,
    const std::function<void(QPainter *)> &paint, bool rasterize = false,
    Primitives primitives = Primitives::ForWindow);
Statistics statistics(const QSGNode *node);

// Geometry kept from frame to frame: chunks along the track, each built
// once, in coordinates of its own, and moved into the view by a
// translation. Chunks share no pixels' drawing: each draws its own part.
struct Chunks {
    // Everything that the chunks' geometry depends on, but for the data
    // each chunk was built from: the chunks of another key are dropped.
    QByteArray key;
    // The chunks the frame shows, first to last.
    qint64 first{0};
    qint64 last{-1};
    // Where a chunk's origin is in the view, in logical pixels: a whole
    // number of device pixels, so that it is drawn as it was built.
    std::function<double(qint64)> offset;
    // What a chunk was built from: a kept chunk whose stamp changes, such
    // as one that new data covers further, is built again.
    std::function<quint64(qint64)> stamp;
    // Draws a chunk in its own coordinates: lines, curves and filled
    // rectangles, which become geometry. Images are not kept.
    std::function<void(QPainter &, qint64)> paint;
};
// Draws `chunks` with a painter of render's paint function, in the order
// of its other drawing: builds the chunks the scene lacks, keeps the
// others, translates them, and drops those it no longer shows. With any
// other painter, or for the software renderer, it draws nothing and
// returns false, for the caller to draw the frame as it is.
bool drawChunks(QPainter &painter, const Chunks &chunks);
// The next stroke `painter` draws, a polyline or a path of cubics, gives
// geometry only to its segments `first` up to `end`, those between
// consecutive points of it; the others are its context, so that a chunk's
// segments join their neighbours as one stroke's do. A painter other than
// render's draws the whole stroke.
void strokeOnlySegments(QPainter &painter, qsizetype first, qsizetype end);
}
