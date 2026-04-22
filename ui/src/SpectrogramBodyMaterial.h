// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QPointF>
#include <QRectF>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGTexture>
#include <QSize>

class QRhi;
class QRhiResourceUpdateBatch;

class SpectrogramBodySampleMaterial final : public QSGMaterial {
public:
    struct State {
        QSGTexture *sourceTexture{nullptr};
        QSGTexture *dbRemapTexture{nullptr};
        QSGTexture *paletteTexture{nullptr};
        QRectF sourceRect;
        QPointF drawOffset;
    };

    SpectrogramBodySampleMaterial();

    static QString vertexShaderResourcePath();
    static QString fragmentShaderResourcePath();

    QSGMaterialType *type() const override;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode renderMode) const override;
    int compare(const QSGMaterial *other) const override;

    State state;
};

class SpectrogramSourceSampleMaterial final : public QSGMaterial {
public:
    struct State {
        QSGTexture *sourceTexture{nullptr};
        QRectF sourceRect;
    };

    SpectrogramSourceSampleMaterial();

    static QString vertexShaderResourcePath();
    static QString fragmentShaderResourcePath();

    QSGMaterialType *type() const override;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode renderMode) const override;
    int compare(const QSGMaterial *other) const override;

    State state;
};

QSGTexture *spectrogramBodySampleTextureForBinding(
    const SpectrogramBodySampleMaterial::State &state,
    int binding);
QSGTexture *spectrogramSourceSampleTextureForBinding(
    const SpectrogramSourceSampleMaterial::State &state,
    int binding);
void spectrogramCommitTextureOperationsIfNeeded(
    QSGTexture *texture,
    QRhi *rhi,
    QRhiResourceUpdateBatch *resourceUpdates);
bool spectrogramShouldConsumeSampledImageLogBudget(const QSize &textureSize);

QSGGeometryNode *createSpectrogramBodySampleMaterialNode();
QSGGeometryNode *createSpectrogramSourceSampleMaterialNode();
QSGGeometryNode *createSpectrogramStockMaterialNode();
void syncSpectrogramBodySampleMaterialNode(
    QSGGeometryNode *node,
    const QRectF &targetRect,
    const SpectrogramBodySampleMaterial::State &state);
void syncSpectrogramSourceSampleMaterialNode(
    QSGGeometryNode *node,
    const QRectF &targetRect,
    QSGTexture *texture,
    const QRectF &sourceRect,
    QSGTexture *placeholderTexture);
void syncSpectrogramStockMaterialNode(
    QSGGeometryNode *node,
    const QRectF &targetRect,
    QSGTexture *texture,
    const QRectF &sourceRect,
    QSGTexture *placeholderTexture);
