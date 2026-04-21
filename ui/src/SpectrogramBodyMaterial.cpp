// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramBodyMaterial.h"

#include <QSGGeometry>
#include <QSGMaterialShader>
#include <QString>

namespace {

class SpectrogramBodyMaterialShader final : public QSGMaterialShader {
public:
    SpectrogramBodyMaterialShader() = default;

    bool updateUniformData(RenderState &state, QSGMaterial *, QSGMaterial *) override {
        QByteArray *uniformData = state.uniformData();
        if (uniformData == nullptr) {
            return false;
        }
        uniformData->resize(16);
        uniformData->fill('\0');
        return true;
    }

    void updateSampledImage(
        RenderState &,
        int,
        QSGTexture **,
        QSGMaterial *,
        QSGMaterial *) override {
    }
};

QSGGeometry *createBodyGeometry() {
    auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4);
    geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    return geometry;
}

} // namespace

SpectrogramBodyMaterial::SpectrogramBodyMaterial() {
    setFlag(Blending, true);
}

QSGMaterialType *SpectrogramBodyMaterial::type() const {
    return const_cast<QSGMaterialType *>(&m_type);
}

QSGMaterialShader *SpectrogramBodyMaterial::createShader(QSGRendererInterface::RenderMode) const {
    return new SpectrogramBodyMaterialShader();
}

int SpectrogramBodyMaterial::compare(const QSGMaterial *other) const {
    if (other == nullptr || other->type() != type()) {
        return 1;
    }
    const auto *otherMaterial = static_cast<const SpectrogramBodyMaterial *>(other);
    if (state.debugStage != otherMaterial->state.debugStage) {
        return state.debugStage - otherMaterial->state.debugStage;
    }
    if (state.sourceTexture != otherMaterial->state.sourceTexture) {
        return state.sourceTexture < otherMaterial->state.sourceTexture ? -1 : 1;
    }
    return 0;
}

QSGGeometryNode *createSpectrogramBodyMaterialNode() {
    auto *node = new QSGGeometryNode();
    node->setGeometry(createBodyGeometry());
    node->setFlag(QSGNode::OwnsGeometry);
    auto *material = new SpectrogramBodyMaterial();
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

void syncSpectrogramBodyMaterialNode(
    QSGGeometryNode *node,
    const QRectF &targetRect,
    const SpectrogramBodyMaterial::State &state) {
    if (node == nullptr || node->geometry() == nullptr) {
        return;
    }

    auto *material = static_cast<SpectrogramBodyMaterial *>(node->material());
    if (material == nullptr) {
        return;
    }

    material->state = state;

    auto *geometry = node->geometry();
    auto *vertices = geometry->vertexDataAsTexturedPoint2D();
    const QPointF topLeft = targetRect.topLeft();
    const QPointF topRight = targetRect.topRight();
    const QPointF bottomLeft = targetRect.bottomLeft();
    const QPointF bottomRight = targetRect.bottomRight();

    vertices[0].set(static_cast<float>(topLeft.x()), static_cast<float>(topLeft.y()), 0.0f, 0.0f);
    vertices[1].set(static_cast<float>(bottomLeft.x()), static_cast<float>(bottomLeft.y()), 0.0f, 1.0f);
    vertices[2].set(static_cast<float>(topRight.x()), static_cast<float>(topRight.y()), 1.0f, 0.0f);
    vertices[3].set(
        static_cast<float>(bottomRight.x()),
        static_cast<float>(bottomRight.y()),
        1.0f,
        1.0f);
    geometry->markVertexDataDirty();
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
}
