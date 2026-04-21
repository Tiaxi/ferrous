// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramRhiItem.h"

#include <QFile>
#include <QMatrix4x4>

#include <rhi/qshader.h>
#include <rhi/qrhi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>

namespace {

constexpr auto kVertexShaderResource = ":/rhi/SpectrogramRhi.vert.qsb";
constexpr auto kFragmentShaderResource = ":/rhi/SpectrogramRhi.frag.qsb";

struct SpectrogramRhiUniforms {
    float clipMatrix[16];
};

QShader loadShaderResource(const QString &resourcePath) {
    QFile shaderFile(resourcePath);
    if (!shaderFile.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QShader::fromSerialized(shaderFile.readAll());
}

QImage prepareSourceImage(const QImage &sourceImage) {
    if (sourceImage.isNull()) {
        return {};
    }
    if (sourceImage.format() == QImage::Format_RGBA8888) {
        return sourceImage;
    }
    return sourceImage.convertToFormat(QImage::Format_RGBA8888);
}

std::array<SpectrogramRhiVertex, 4> buildQuadVertices(
    const SpectrogramRhiSyncState &state,
    const QSize &targetSize) {
    const double targetWidth = static_cast<double>(std::max(1, targetSize.width()));
    const double left = (2.0 * state.drawX / targetWidth) - 1.0;
    const double right =
        (2.0 * (state.drawX + static_cast<double>(state.drawPixels)) / targetWidth) - 1.0;

    return {{
        SpectrogramRhiVertex{
            .x = static_cast<float>(left),
            .y = -1.0f,
            .u = 0.0f,
            .v = 1.0f,
        },
        SpectrogramRhiVertex{
            .x = static_cast<float>(right),
            .y = -1.0f,
            .u = 1.0f,
            .v = 1.0f,
        },
        SpectrogramRhiVertex{
            .x = static_cast<float>(left),
            .y = 1.0f,
            .u = 0.0f,
            .v = 0.0f,
        },
        SpectrogramRhiVertex{
            .x = static_cast<float>(right),
            .y = 1.0f,
            .u = 1.0f,
            .v = 0.0f,
        },
    }};
}

bool copyClipMatrix(SpectrogramRhiUniforms *uniforms, const QMatrix4x4 &matrix) {
    static_assert(sizeof(uniforms->clipMatrix) == sizeof(float) * 16);
    if (uniforms == nullptr) {
        return false;
    }
    std::memcpy(uniforms->clipMatrix, matrix.constData(), sizeof(uniforms->clipMatrix));
    return true;
}

class SpectrogramRhiRenderer final : public QQuickRhiItemRenderer {
public:
    void initialize(QRhiCommandBuffer *cb) override {
        Q_UNUSED(cb);
        ensureStaticResources();
    }

    void synchronize(QQuickRhiItem *item) override {
        m_syncState = static_cast<SpectrogramRhiItem *>(item)->debugLastSyncState();
    }

    void render(QRhiCommandBuffer *cb) override {
        QRhiRenderTarget *target = renderTarget();
        if (cb == nullptr || target == nullptr || rhi() == nullptr) {
            return;
        }

        QRhiResourceUpdateBatch *resourceUpdates = rhi()->nextResourceUpdateBatch();
        const bool canDrawSpectrogram = prepareDrawResources(resourceUpdates, target->pixelSize());

        cb->beginPass(target, Qt::black, QRhiDepthStencilClearValue(), resourceUpdates);
        if (canDrawSpectrogram && m_pipeline && m_srb && m_vertexBuffer) {
            cb->setGraphicsPipeline(m_pipeline.get());
            cb->setViewport(QRhiViewport(
                0.0f,
                0.0f,
                static_cast<float>(target->pixelSize().width()),
                static_cast<float>(target->pixelSize().height())));
            const QRhiCommandBuffer::VertexInput vertexBindings[] = {
                {m_vertexBuffer.get(), 0},
            };
            cb->setVertexInput(0, 1, vertexBindings);
            cb->setShaderResources(m_srb.get());
            cb->draw(4);
        }
        cb->endPass();
    }

private:
    bool ensureStaticResources() {
        if (rhi() == nullptr) {
            return false;
        }

        if (!m_vertexShader.isValid()) {
            m_vertexShader = loadShaderResource(QString::fromUtf8(kVertexShaderResource));
        }
        if (!m_fragmentShader.isValid()) {
            m_fragmentShader = loadShaderResource(QString::fromUtf8(kFragmentShaderResource));
        }
        if (!m_vertexShader.isValid() || !m_fragmentShader.isValid()) {
            return false;
        }

        if (!m_sampler) {
            m_sampler.reset(rhi()->newSampler(
                QRhiSampler::Nearest,
                QRhiSampler::Nearest,
                QRhiSampler::None,
                QRhiSampler::ClampToEdge,
                QRhiSampler::ClampToEdge));
            if (!m_sampler || !m_sampler->create()) {
                m_sampler.reset();
                return false;
            }
        }

        if (!m_vertexBuffer) {
            m_vertexBuffer.reset(rhi()->newBuffer(
                QRhiBuffer::Dynamic,
                QRhiBuffer::VertexBuffer,
                sizeof(SpectrogramRhiVertex) * 4));
            if (!m_vertexBuffer || !m_vertexBuffer->create()) {
                m_vertexBuffer.reset();
                return false;
            }
        }

        if (!m_uniformBuffer) {
            m_uniformBuffer.reset(rhi()->newBuffer(
                QRhiBuffer::Dynamic,
                QRhiBuffer::UniformBuffer,
                sizeof(SpectrogramRhiUniforms)));
            if (!m_uniformBuffer || !m_uniformBuffer->create()) {
                m_uniformBuffer.reset();
                return false;
            }
        }

        return true;
    }

    bool ensureSourceTexture(const QSize &textureSize) {
        if (rhi() == nullptr || !ensureStaticResources()) {
            return false;
        }
        if (textureSize.isEmpty()) {
            return false;
        }
        if (m_sourceTexture && m_sourceTextureSize == textureSize) {
            return true;
        }

        m_pipeline.reset();
        m_srb.reset();
        m_sourceTexture.reset();
        m_sourceTextureSize = QSize();
        m_uploadedGeneration = std::numeric_limits<quint64>::max();

        m_sourceTexture.reset(rhi()->newTexture(QRhiTexture::RGBA8, textureSize));
        if (!m_sourceTexture || !m_sourceTexture->create()) {
            m_sourceTexture.reset();
            return false;
        }

        m_sourceTextureSize = textureSize;
        return true;
    }

    bool ensureShaderResources() {
        if (!m_uniformBuffer || !m_sourceTexture || !m_sampler) {
            return false;
        }
        if (m_srb) {
            return true;
        }

        m_srb.reset(rhi()->newShaderResourceBindings());
        if (!m_srb) {
            return false;
        }
        m_srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage,
                m_uniformBuffer.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                m_sourceTexture.get(),
                m_sampler.get()),
        });
        if (!m_srb->create()) {
            m_srb.reset();
            return false;
        }

        return true;
    }

    bool ensurePipeline(QRhiRenderTarget *target) {
        if (target == nullptr || !ensureShaderResources()) {
            return false;
        }

        const int targetSampleCount = target->sampleCount();
        QRhiRenderPassDescriptor *renderPass = target->renderPassDescriptor();
        if (m_pipeline
            && m_pipelineSampleCount == targetSampleCount
            && m_pipelineRenderPass == renderPass) {
            return true;
        }

        QRhiVertexInputLayout inputLayout;
        inputLayout.setBindings({
            QRhiVertexInputBinding(sizeof(SpectrogramRhiVertex)),
        });
        inputLayout.setAttributes({
            QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
            QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, sizeof(float) * 2),
        });

        m_pipeline.reset(rhi()->newGraphicsPipeline());
        if (!m_pipeline) {
            return false;
        }
        m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        m_pipeline->setSampleCount(targetSampleCount);
        m_pipeline->setShaderStages({
            QRhiShaderStage(QRhiShaderStage::Vertex, m_vertexShader),
            QRhiShaderStage(QRhiShaderStage::Fragment, m_fragmentShader),
        });
        m_pipeline->setVertexInputLayout(inputLayout);
        m_pipeline->setShaderResourceBindings(m_srb.get());
        m_pipeline->setRenderPassDescriptor(renderPass);
        if (!m_pipeline->create()) {
            m_pipeline.reset();
            return false;
        }

        m_pipelineSampleCount = targetSampleCount;
        m_pipelineRenderPass = renderPass;
        return true;
    }

    bool prepareDrawResources(QRhiResourceUpdateBatch *resourceUpdates, const QSize &targetSize) {
        if (resourceUpdates == nullptr || targetSize.isEmpty()) {
            return false;
        }

        const QImage sourceImage = prepareSourceImage(m_syncState.sourceImage);
        if (sourceImage.isNull() || m_syncState.drawPixels <= 0) {
            return false;
        }
        if (!ensureSourceTexture(sourceImage.size())) {
            return false;
        }
        if (!ensurePipeline(renderTarget())) {
            return false;
        }

        if (m_uploadedGeneration != m_syncState.sourceGeneration) {
            resourceUpdates->uploadTexture(m_sourceTexture.get(), sourceImage);
            m_uploadedGeneration = m_syncState.sourceGeneration;
        }

        const auto quadVertices = buildQuadVertices(m_syncState, targetSize);
        resourceUpdates->updateDynamicBuffer(
            m_vertexBuffer.get(),
            0,
            sizeof(SpectrogramRhiVertex) * quadVertices.size(),
            quadVertices.data());

        SpectrogramRhiUniforms uniforms{};
        copyClipMatrix(&uniforms, rhi()->clipSpaceCorrMatrix());
        resourceUpdates->updateDynamicBuffer(
            m_uniformBuffer.get(),
            0,
            sizeof(SpectrogramRhiUniforms),
            &uniforms);

        return true;
    }

    SpectrogramRhiSyncState m_syncState;
    QShader m_vertexShader;
    QShader m_fragmentShader;
    std::unique_ptr<QRhiTexture> m_sourceTexture;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiBuffer> m_vertexBuffer;
    std::unique_ptr<QRhiBuffer> m_uniformBuffer;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
    QSize m_sourceTextureSize;
    quint64 m_uploadedGeneration{std::numeric_limits<quint64>::max()};
    int m_pipelineSampleCount{0};
    QRhiRenderPassDescriptor *m_pipelineRenderPass{nullptr};
};

} // namespace

SpectrogramRhiItem::SpectrogramRhiItem(QQuickItem *parent)
    : QQuickRhiItem(parent) {
    setVisible(true);
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptHoverEvents(false);
    setAlphaBlending(false);
    setColorBufferFormat(TextureFormat::RGBA8);
}

void SpectrogramRhiItem::setSyncState(const SpectrogramRhiSyncState &state) {
    m_syncState = state;
    update();
}

SpectrogramRhiSyncState SpectrogramRhiItem::debugLastSyncState() const {
    return m_syncState;
}

std::array<SpectrogramRhiVertex, 4> SpectrogramRhiItem::debugBuildQuadVertices(
    const SpectrogramRhiSyncState &state,
    const QSize &targetSize) {
    return buildQuadVertices(state, targetSize);
}

QQuickRhiItemRenderer *SpectrogramRhiItem::createRenderer() {
    return new SpectrogramRhiRenderer();
}
