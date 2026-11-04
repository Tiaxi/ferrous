// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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
    int geometryVertices{0};
    // The most vertices one geometry node holds.
    int largestGeometryVertices{0};
    // Labels laid out and rasterized because no earlier frame drew them.
    int labelRasters{0};
};
// What lines and curves become: what the window's renderer draws, or, for
// tests, geometry as for a native renderer under any renderer, which builds
// the nodes without drawing them.
enum class Primitives { ForWindow, Geometry };
QSGNode *render(QSGNode *previous, QQuickWindow *window, QSize size,
    const std::function<void(QPainter *)> &paint, bool rasterize = false,
    Primitives primitives = Primitives::ForWindow);
Statistics statistics(const QSGNode *node);
}
