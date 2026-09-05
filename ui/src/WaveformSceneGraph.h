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
// Per-frame texture creation requests and retained image payload sizes. These
// exclude GPU submission, atlas allocation and driver overhead.
struct Statistics {
    quint64 textureUploads{0};
    quint64 uploadedBytes{0};
    quint64 residentBytes{0};
    int commands{0};
    int geometryVertices{0};
};
QSGNode *render(QSGNode *previous, QQuickWindow *window, QSize size,
    const std::function<void(QPainter *)> &paint, bool rasterize = false);
Statistics statistics(const QSGNode *node);
}
