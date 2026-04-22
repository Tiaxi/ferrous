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
        setShaderFileName(VertexStage, SpectrogramBodyMaterial::vertexShaderResourcePath());
        setShaderFileName(FragmentStage, SpectrogramBodyMaterial::fragmentShaderResourcePath());
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
        if (texture == nullptr || newMaterial == nullptr) {
            if (texture != nullptr) {
                *texture = nullptr;
            }
            return;
        }

        const auto *material = static_cast<SpectrogramBodyMaterial *>(newMaterial);
        switch (binding) {
        case 1:
            *texture = material->state.sourceTexture;
            break;
        case 2:
            *texture = material->state.dbRemapTexture;
            break;
        case 3:
            *texture = material->state.paletteTexture;
            break;
        default:
            *texture = nullptr;
            break;
        }
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

QString SpectrogramBodyMaterial::vertexShaderResourcePath() {
    return QStringLiteral(":/shaders/SpectrogramBody.vert.qsb");
}

QString SpectrogramBodyMaterial::fragmentShaderResourcePath() {
    return QStringLiteral(":/shaders/SpectrogramBody.frag.qsb");
}

QSGMaterialType *SpectrogramBodyMaterial::type() const {
    static QSGMaterialType sharedType;
    return &sharedType;
}

QSGMaterialShader *SpectrogramBodyMaterial::createShader(QSGRendererInterface::RenderMode) const {
    return new SpectrogramBodyMaterialShader();
}

int SpectrogramBodyMaterial::compare(const QSGMaterial *other) const {
    const auto textureKey = [](QSGTexture *texture) -> qint64 {
        return texture != nullptr ? texture->comparisonKey() : 0;
    };

    if (other == nullptr || other->type() != type()) {
        return 1;
    }
    const auto *otherMaterial = static_cast<const SpectrogramBodyMaterial *>(other);
    if (state.debugStage != otherMaterial->state.debugStage) {
        return state.debugStage - otherMaterial->state.debugStage;
    }

    const std::array<qint64, 3> keys{
        textureKey(state.sourceTexture),
        textureKey(state.dbRemapTexture),
        textureKey(state.paletteTexture),
    };
    const std::array<qint64, 3> otherKeys{
        textureKey(otherMaterial->state.sourceTexture),
        textureKey(otherMaterial->state.dbRemapTexture),
        textureKey(otherMaterial->state.paletteTexture),
    };
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] != otherKeys[i]) {
            return keys[i] < otherKeys[i] ? -1 : 1;
        }
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
