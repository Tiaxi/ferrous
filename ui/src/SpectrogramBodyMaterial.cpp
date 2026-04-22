// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramBodyMaterial.h"

#include <QMatrix4x4>
#include <QSGGeometry>
#include <QSGMaterialShader>
#include <QSGTextureMaterial>
#include <QString>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

QSGTexture *spectrogramBodySampleTextureForBinding(
    const SpectrogramBodySampleMaterial::State &state,
    const int binding) {
    switch (binding) {
    case 0:
        return state.sourceTexture;
    case 1:
        return state.dbRemapTexture;
    case 2:
        return state.paletteTexture;
    default:
        return nullptr;
    }
}

QSGTexture *spectrogramSourceSampleTextureForBinding(
    const SpectrogramSourceSampleMaterial::State &state,
    const int binding) {
    return binding == 0 ? state.sourceTexture : nullptr;
}

bool spectrogramShouldConsumeSampledImageLogBudget(const QSize &textureSize) {
    return textureSize.width() > 1 || textureSize.height() > 1;
}

void spectrogramCommitTextureOperationsIfNeeded(
    QSGTexture *texture,
    QRhi *rhi,
    QRhiResourceUpdateBatch *resourceUpdates) {
    if (texture == nullptr) {
        return;
    }
    texture->commitTextureOperations(rhi, resourceUpdates);
}

namespace {

std::atomic<int> g_gpuSampledImageLogBudget{256};
std::atomic<int> g_gpuSampleUniformLogBudget{64};

struct MatrixUniformData {
    std::array<float, 16> matrix{};
};

static_assert(sizeof(MatrixUniformData) == 64, "Matrix-only Qt Quick shader UBO must match the baked Qt shader layout");

class SpectrogramBodySampleMaterialShader final : public QSGMaterialShader {
public:
    SpectrogramBodySampleMaterialShader() {
        setShaderFileName(VertexStage, SpectrogramBodySampleMaterial::vertexShaderResourcePath());
        setShaderFileName(FragmentStage, SpectrogramBodySampleMaterial::fragmentShaderResourcePath());
    }

    bool updateUniformData(
        RenderState &state,
        QSGMaterial *,
        QSGMaterial *oldMaterial) override {
        QByteArray *uniformData = state.uniformData();
        if (uniformData == nullptr) {
            return false;
        }

        bool changed = false;
        if (uniformData->size() != static_cast<int>(sizeof(MatrixUniformData))) {
            uniformData->resize(static_cast<int>(sizeof(MatrixUniformData)));
            uniformData->fill('\0');
            changed = true;
        }

        auto *buf = reinterpret_cast<MatrixUniformData *>(uniformData->data());
        if (oldMaterial == nullptr || state.isMatrixDirty()) {
            const QMatrix4x4 matrix = state.combinedMatrix();
            std::memcpy(buf->matrix.data(), matrix.constData(), sizeof(float) * buf->matrix.size());
            changed = true;
        }

        const int remainingLogs = g_gpuSampleUniformLogBudget.fetch_sub(1);
        if (remainingLogs > 0) {
            std::fprintf(
                stderr,
                "[Qt-gpu-sample-ubo] matrix_dirty=%d opacity=%.3f viewport=%dx%d\n",
                state.isMatrixDirty() ? 1 : 0,
                state.opacity(),
                state.viewportRect().width(),
                state.viewportRect().height());
        }

        return changed;
    }

    void updateSampledImage(
        RenderState &state,
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

        const auto *material = static_cast<SpectrogramBodySampleMaterial *>(newMaterial);
        *texture = spectrogramBodySampleTextureForBinding(material->state, binding);
        spectrogramCommitTextureOperationsIfNeeded(
            *texture, state.rhi(), state.resourceUpdateBatch());

        const QSize textureSize = *texture != nullptr ? (*texture)->textureSize() : QSize();
        if (spectrogramShouldConsumeSampledImageLogBudget(textureSize)) {
            const int remainingLogs = g_gpuSampledImageLogBudget.fetch_sub(1);
            if (remainingLogs > 0) {
            const auto textureKey = [](QSGTexture *sampledTexture) -> long long {
                return sampledTexture != nullptr
                    ? static_cast<long long>(sampledTexture->comparisonKey())
                    : 0LL;
            };
            const QRectF subRect =
                *texture != nullptr
                    ? (*texture)->normalizedTextureSubRect()
                    : QRectF();
            std::fprintf(
                stderr,
                "[Qt-gpu-sample-sampler] binding=%d tex=%lld size=%dx%d atlas=%d sub=%.4f,%.4f %.4fx%.4f\n",
                binding,
                textureKey(*texture),
                textureSize.width(),
                textureSize.height(),
                *texture != nullptr && (*texture)->isAtlasTexture() ? 1 : 0,
                subRect.x(),
                subRect.y(),
                subRect.width(),
                subRect.height());
            }
        }
    }
};

class SpectrogramSourceSampleMaterialShader final : public QSGMaterialShader {
public:
    SpectrogramSourceSampleMaterialShader() {
        setShaderFileName(VertexStage, SpectrogramSourceSampleMaterial::vertexShaderResourcePath());
        setShaderFileName(FragmentStage, SpectrogramSourceSampleMaterial::fragmentShaderResourcePath());
    }

    bool updateUniformData(
        RenderState &state,
        QSGMaterial *,
        QSGMaterial *oldMaterial) override {
        QByteArray *uniformData = state.uniformData();
        if (uniformData == nullptr) {
            return false;
        }

        bool changed = false;
        if (uniformData->size() != static_cast<int>(sizeof(MatrixUniformData))) {
            uniformData->resize(static_cast<int>(sizeof(MatrixUniformData)));
            uniformData->fill('\0');
            changed = true;
        }

        auto *buf = reinterpret_cast<MatrixUniformData *>(uniformData->data());
        if (oldMaterial == nullptr || state.isMatrixDirty()) {
            const QMatrix4x4 matrix = state.combinedMatrix();
            std::memcpy(buf->matrix.data(), matrix.constData(), sizeof(float) * buf->matrix.size());
            changed = true;
        }

        const int remainingLogs = g_gpuSampleUniformLogBudget.fetch_sub(1);
        if (remainingLogs > 0) {
            std::fprintf(
                stderr,
                "[Qt-gpu-source-sample-ubo] matrix_dirty=%d opacity=%.3f viewport=%dx%d\n",
                state.isMatrixDirty() ? 1 : 0,
                state.opacity(),
                state.viewportRect().width(),
                state.viewportRect().height());
        }

        return changed;
    }

    void updateSampledImage(
        RenderState &state,
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

        const auto *material = static_cast<SpectrogramSourceSampleMaterial *>(newMaterial);
        *texture = spectrogramSourceSampleTextureForBinding(material->state, binding);
        spectrogramCommitTextureOperationsIfNeeded(
            *texture, state.rhi(), state.resourceUpdateBatch());

        const QSize textureSize = *texture != nullptr ? (*texture)->textureSize() : QSize();
        if (spectrogramShouldConsumeSampledImageLogBudget(textureSize)) {
            const int remainingLogs = g_gpuSampledImageLogBudget.fetch_sub(1);
            if (remainingLogs > 0) {
            const auto textureKey = [](QSGTexture *sampledTexture) -> long long {
                return sampledTexture != nullptr
                    ? static_cast<long long>(sampledTexture->comparisonKey())
                    : 0LL;
            };
            const QRectF subRect =
                *texture != nullptr
                    ? (*texture)->normalizedTextureSubRect()
                    : QRectF();
            std::fprintf(
                stderr,
                "[Qt-gpu-source-sample-sampler] binding=%d tex=%lld size=%dx%d atlas=%d sub=%.4f,%.4f %.4fx%.4f\n",
                binding,
                textureKey(*texture),
                textureSize.width(),
                textureSize.height(),
                *texture != nullptr && (*texture)->isAtlasTexture() ? 1 : 0,
                subRect.x(),
                subRect.y(),
                subRect.width(),
                subRect.height());
            }
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

SpectrogramBodySampleMaterial::SpectrogramBodySampleMaterial() {
    setFlag(Blending, true);
    setFlag(RequiresFullMatrix, true);
    setFlag(NoBatching, true);
}

SpectrogramSourceSampleMaterial::SpectrogramSourceSampleMaterial() {
    setFlag(Blending, true);
    setFlag(RequiresFullMatrix, true);
    setFlag(NoBatching, true);
}

QString SpectrogramBodySampleMaterial::vertexShaderResourcePath() {
    return QStringLiteral(":/shaders/SpectrogramSample.vert.qsb");
}

QString SpectrogramBodySampleMaterial::fragmentShaderResourcePath() {
    return QStringLiteral(":/shaders/SpectrogramBodySample.frag.qsb");
}

QString SpectrogramSourceSampleMaterial::vertexShaderResourcePath() {
    return QStringLiteral(":/shaders/SpectrogramSample.vert.qsb");
}

QString SpectrogramSourceSampleMaterial::fragmentShaderResourcePath() {
    return QStringLiteral(":/shaders/SpectrogramSourceSample.frag.qsb");
}

QSGMaterialType *SpectrogramBodySampleMaterial::type() const {
    static QSGMaterialType sharedType;
    return &sharedType;
}

QSGMaterialType *SpectrogramSourceSampleMaterial::type() const {
    static QSGMaterialType sharedType;
    return &sharedType;
}

QSGMaterialShader *SpectrogramBodySampleMaterial::createShader(QSGRendererInterface::RenderMode) const {
    return new SpectrogramBodySampleMaterialShader();
}

QSGMaterialShader *SpectrogramSourceSampleMaterial::createShader(QSGRendererInterface::RenderMode) const {
    return new SpectrogramSourceSampleMaterialShader();
}

int SpectrogramBodySampleMaterial::compare(const QSGMaterial *other) const {
    const auto textureKey = [](QSGTexture *texture) -> qint64 {
        return texture != nullptr ? texture->comparisonKey() : 0;
    };

    if (other == nullptr || other->type() != type()) {
        return 1;
    }
    const auto *otherMaterial = static_cast<const SpectrogramBodySampleMaterial *>(other);
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

int SpectrogramSourceSampleMaterial::compare(const QSGMaterial *other) const {
    const auto textureKey = [](QSGTexture *texture) -> qint64 {
        return texture != nullptr ? texture->comparisonKey() : 0;
    };

    if (other == nullptr || other->type() != type()) {
        return 1;
    }
    const auto *otherMaterial = static_cast<const SpectrogramSourceSampleMaterial *>(other);
    const qint64 key = textureKey(state.sourceTexture);
    const qint64 otherKey = textureKey(otherMaterial->state.sourceTexture);
    if (key != otherKey) {
        return key < otherKey ? -1 : 1;
    }
    return 0;
}

QSGGeometryNode *createSpectrogramBodySampleMaterialNode() {
    auto *node = new QSGGeometryNode();
    node->setGeometry(createBodyGeometry());
    node->setFlag(QSGNode::OwnsGeometry);
    auto *material = new SpectrogramBodySampleMaterial();
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

QSGGeometryNode *createSpectrogramSourceSampleMaterialNode() {
    auto *node = new QSGGeometryNode();
    node->setGeometry(createBodyGeometry());
    node->setFlag(QSGNode::OwnsGeometry);
    auto *material = new SpectrogramSourceSampleMaterial();
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

QSGGeometryNode *createSpectrogramStockMaterialNode() {
    auto *node = new QSGGeometryNode();
    node->setGeometry(createBodyGeometry());
    node->setFlag(QSGNode::OwnsGeometry);
    auto *material = new QSGTextureMaterial();
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

void syncSpectrogramBodySampleMaterialNode(
    QSGGeometryNode *node,
    const QRectF &targetRect,
    const SpectrogramBodySampleMaterial::State &state) {
    if (node == nullptr || node->geometry() == nullptr) {
        return;
    }

    auto *material = dynamic_cast<SpectrogramBodySampleMaterial *>(node->material());
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
        ? state.sourceTexture->convertToNormalizedSourceRect(state.sourceRect)
        : QRectF(0.0, 0.0, 1.0, 1.0);
    const float leftU = static_cast<float>(textureSubRect.left());
    const float rightU = static_cast<float>(textureSubRect.left() + textureSubRect.width());
    const float topV = static_cast<float>(textureSubRect.top() + textureSubRect.height());
    const float bottomV = static_cast<float>(textureSubRect.top());

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

void syncSpectrogramSourceSampleMaterialNode(
    QSGGeometryNode *node,
    const QRectF &targetRect,
    QSGTexture *texture,
    const QRectF &sourceRect,
    QSGTexture *placeholderTexture) {
    if (node == nullptr || node->geometry() == nullptr) {
        return;
    }

    auto *material = dynamic_cast<SpectrogramSourceSampleMaterial *>(node->material());
    if (material == nullptr) {
        return;
    }

    QSGTexture *textureToUse = texture != nullptr ? texture : placeholderTexture;
    material->state.sourceTexture = textureToUse;
    material->state.sourceRect = sourceRect;

    const QRectF geometryRect =
        textureToUse != nullptr && !targetRect.isEmpty() ? targetRect : QRectF();
    auto *geometry = node->geometry();
    auto *vertices = geometry->vertexDataAsTexturedPoint2D();
    const QRectF textureSubRect =
        textureToUse != nullptr
            ? textureToUse->convertToNormalizedSourceRect(sourceRect)
            : QRectF(0.0, 0.0, 1.0, 1.0);
    const float leftU = static_cast<float>(textureSubRect.left());
    const float rightU = static_cast<float>(textureSubRect.left() + textureSubRect.width());
    const float topV = static_cast<float>(textureSubRect.top() + textureSubRect.height());
    const float bottomV = static_cast<float>(textureSubRect.top());

    vertices[0].set(
        static_cast<float>(geometryRect.left()),
        static_cast<float>(geometryRect.top()),
        leftU,
        topV);
    vertices[1].set(
        static_cast<float>(geometryRect.left()),
        static_cast<float>(geometryRect.bottom()),
        leftU,
        bottomV);
    vertices[2].set(
        static_cast<float>(geometryRect.right()),
        static_cast<float>(geometryRect.top()),
        rightU,
        topV);
    vertices[3].set(
        static_cast<float>(geometryRect.right()),
        static_cast<float>(geometryRect.bottom()),
        rightU,
        bottomV);
    geometry->markVertexDataDirty();
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
}

void syncSpectrogramStockMaterialNode(
    QSGGeometryNode *node,
    const QRectF &targetRect,
    QSGTexture *texture,
    const QRectF &sourceRect,
    QSGTexture *placeholderTexture) {
    if (node == nullptr || node->geometry() == nullptr) {
        return;
    }

    auto *material = dynamic_cast<QSGTextureMaterial *>(node->material());
    if (material == nullptr) {
        return;
    }

    QSGTexture *textureToUse = texture != nullptr ? texture : placeholderTexture;
    material->setTexture(textureToUse);
    material->setFiltering(QSGTexture::Nearest);
    material->setMipmapFiltering(QSGTexture::None);
    material->setHorizontalWrapMode(QSGTexture::ClampToEdge);
    material->setVerticalWrapMode(QSGTexture::ClampToEdge);

    const QRectF adjustedTarget = targetRect;
    const QRectF geometryRect =
        textureToUse != nullptr && !adjustedTarget.isEmpty()
        ? adjustedTarget
        : QRectF();

    auto *geometry = node->geometry();
    auto *vertices = geometry->vertexDataAsTexturedPoint2D();
    const QPointF topLeft = geometryRect.topLeft();
    const QPointF topRight = geometryRect.topRight();
    const QPointF bottomLeft = geometryRect.bottomLeft();
    const QPointF bottomRight = geometryRect.bottomRight();
    const QRectF effectiveSourceRect =
        textureToUse != nullptr
            ? (!sourceRect.isEmpty()
                   ? sourceRect
                   : QRectF(QPointF(0.0, 0.0), QSizeF(textureToUse->textureSize())))
            : QRectF(0.0, 0.0, 1.0, 1.0);
    const QRectF textureSubRect =
        textureToUse != nullptr
        ? textureToUse->convertToNormalizedSourceRect(effectiveSourceRect)
        : QRectF(0.0, 0.0, 1.0, 1.0);
    const float leftU = static_cast<float>(textureSubRect.left());
    const float rightU = static_cast<float>(textureSubRect.left() + textureSubRect.width());
    const float topV = static_cast<float>(textureSubRect.top());
    const float bottomV = static_cast<float>(textureSubRect.top() + textureSubRect.height());

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
