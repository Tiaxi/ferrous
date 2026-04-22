// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramBodyMaterial.h"

#include <QMatrix4x4>
#include <QSGGeometry>
#include <QSGMaterialShader>
#include <QString>

#include <array>
#include <cstddef>
#include <cstring>

namespace {

struct UniformData {
    std::array<float, 16> matrix{};
    float opacity{1.0f};
};

static_assert(sizeof(UniformData) == 68, "Qt Quick shader UBO must match the baked Qt shader layout");

class SpectrogramBodyMaterialShader final : public QSGMaterialShader {
public:
    SpectrogramBodyMaterialShader() {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/SpectrogramBody.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/SpectrogramBody.frag.qsb"));
    }

    bool updateUniformData(
        RenderState &state,
        QSGMaterial *newMaterial,
        QSGMaterial *oldMaterial) override {
        Q_UNUSED(newMaterial);

        QByteArray *uniformData = state.uniformData();
        if (uniformData == nullptr) {
            return false;
        }

        bool changed = false;
        if (uniformData->size() != static_cast<int>(sizeof(UniformData))) {
            uniformData->resize(static_cast<int>(sizeof(UniformData)));
            uniformData->fill('\0');
            changed = true;
        }

        auto *buf = reinterpret_cast<UniformData *>(uniformData->data());
        if (oldMaterial == nullptr || state.isMatrixDirty()) {
            const QMatrix4x4 matrix = state.combinedMatrix();
            std::memcpy(buf->matrix.data(), matrix.constData(), sizeof(float) * buf->matrix.size());
            changed = true;
        }
        if (oldMaterial == nullptr || state.isOpacityDirty()) {
            buf->opacity = state.opacity();
            changed = true;
        }
        return changed;
    }

    void updateSampledImage(
        RenderState &,
        int binding,
        QSGTexture **texture,
        QSGMaterial *newMaterial,
        QSGMaterial *) override {
        if (binding != 1 || texture == nullptr || newMaterial == nullptr) {
            return;
        }

        const auto *material = static_cast<SpectrogramBodyMaterial *>(newMaterial);
        *texture = material->state.sourceTexture;
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
    setFlag(RequiresFullMatrix, true);
}

QSGMaterialType *SpectrogramBodyMaterial::type() const {
    static QSGMaterialType sharedType;
    return &sharedType;
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

    const qint64 sourceKey =
        state.sourceTexture != nullptr ? state.sourceTexture->comparisonKey() : 0;
    const qint64 otherSourceKey =
        otherMaterial->state.sourceTexture != nullptr
            ? otherMaterial->state.sourceTexture->comparisonKey()
            : 0;
    if (sourceKey != otherSourceKey) {
        return sourceKey < otherSourceKey ? -1 : 1;
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
    const QRectF adjustedTarget = targetRect.translated(state.drawOffset);
    const QRectF geometryRect =
        state.sourceTexture != nullptr && !adjustedTarget.isEmpty()
        ? adjustedTarget
        : QRectF();

    auto *geometry = node->geometry();
    auto *vertices = geometry->vertexDataAsTexturedPoint2D();
    const QPointF topLeft = geometryRect.topLeft();
    const QPointF topRight = geometryRect.topRight();
    const QPointF bottomLeft = geometryRect.bottomLeft();
    const QPointF bottomRight = geometryRect.bottomRight();
    const QRectF textureSubRect =
        state.sourceTexture != nullptr
        ? state.sourceTexture->normalizedTextureSubRect()
        : QRectF(0.0, 0.0, 1.0, 1.0);
    const QRectF sourceSubRect = state.sourceSubRect.normalized();
    const float leftU = static_cast<float>(
        textureSubRect.left() + (textureSubRect.width() * sourceSubRect.left()));
    const float rightU = static_cast<float>(
        textureSubRect.left()
        + (textureSubRect.width() * (sourceSubRect.left() + sourceSubRect.width())));
    const float topV = static_cast<float>(
        textureSubRect.top() + (textureSubRect.height() * sourceSubRect.top()));
    const float bottomV = static_cast<float>(
        textureSubRect.top()
        + (textureSubRect.height() * (sourceSubRect.top() + sourceSubRect.height())));

    vertices[0].set(static_cast<float>(topLeft.x()), static_cast<float>(topLeft.y()), leftU, topV);
    vertices[1].set(static_cast<float>(bottomLeft.x()), static_cast<float>(bottomLeft.y()), leftU, bottomV);
    vertices[2].set(static_cast<float>(topRight.x()), static_cast<float>(topRight.y()), rightU, topV);
    vertices[3].set(
        static_cast<float>(bottomRight.x()),
        static_cast<float>(bottomRight.y()),
        rightU,
        bottomV);
    geometry->markVertexDataDirty();
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
}
