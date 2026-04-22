// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QPointF>
#include <QRectF>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGTexture>
#include <QSize>

class SpectrogramBodyMaterial final : public QSGMaterial {
public:
    struct State {
        QSGTexture *sourceTexture{nullptr};
        QSGTexture *dbRemapTexture{nullptr};
        QSGTexture *paletteTexture{nullptr};
        QSize sourceSize;
        QSize targetSize;
        QRectF sourceSubRect{0.0, 0.0, 1.0, 1.0};
        QPointF drawOffset;
        double effectiveZoom{1.0};
        double subpixelOffset{0.0};
        int debugStage{0};
    };

    SpectrogramBodyMaterial();

    QSGMaterialType *type() const override;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode renderMode) const override;
    int compare(const QSGMaterial *other) const override;

    State state;
};

QSGGeometryNode *createSpectrogramBodyMaterialNode();
void syncSpectrogramBodyMaterialNode(
    QSGGeometryNode *node,
    const QRectF &targetRect,
    const SpectrogramBodyMaterial::State &state);
