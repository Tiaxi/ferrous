// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QSGOpacityNode>

// A node of an item's scene that changes while it is drawn: nodes join it,
// or nodes under it move or fade. Qt's software renderer, which draws with
// QT_QUICK_BACKEND=software, in VMs without OpenGL and in the offscreen
// tests, places such a node by what it keeps of the node's parent. It keeps
// that of transform, clip and opacity nodes and of drawn ones, not of a
// plain QSGNode: under one, the node drew at the window's origin, unclipped,
// and was missing from the item until something above it moved. It also
// forgets only a removed node's own state, so a plain node made where a
// removed one was could take that one's place. An opacity node at 1 is kept
// and draws the same as a plain node with every renderer.
class SceneGraphParentNode : public QSGOpacityNode {};
