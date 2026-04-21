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
#include <vector>

namespace {

constexpr auto kVertexShaderResource = ":/rhi/SpectrogramRhi.vert.qsb";
constexpr auto kFragmentShaderResource = ":/rhi/SpectrogramRhi.frag.qsb";
constexpr auto kLineVertexShaderResource = ":/rhi/SpectrogramRhiLine.vert.qsb";
constexpr auto kLineFragmentShaderResource = ":/rhi/SpectrogramRhiLine.frag.qsb";

struct SpectrogramRhiUniforms {
    float clipMatrix[16];
};

struct SpectrogramRhiLineVertex {
    float x{0.0f};
    float y{0.0f};
    float r{0.0f};
    float g{0.0f};
    float b{0.0f};
    float a{0.0f};
};

QShader loadShaderResource(const QString &resourcePath) {
    QFile shaderFile(resourcePath);
    if (!shaderFile.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QShader::fromSerialized(shaderFile.readAll());
}

QImage prepareTextureImage(const QImage &image) {
    if (image.isNull()) {
        return {};
    }
    if (image.format() == QImage::Format_RGBA8888) {
        return image;
    }
    return image.convertToFormat(QImage::Format_RGBA8888);
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

float pixelToClipX(double x, const QSize &targetSize) {
    return static_cast<float>(
        (2.0 * x / static_cast<double>(std::max(1, targetSize.width()))) - 1.0);
}

float pixelToClipY(double y, const QSize &targetSize) {
    return static_cast<float>(
        1.0 - (2.0 * y / static_cast<double>(std::max(1, targetSize.height()))));
}

void appendLineVertices(
    std::vector<SpectrogramRhiLineVertex> *vertices,
    const QLineF &line,
    const QSize &targetSize,
    float r,
    float g,
    float b,
    float a) {
    if (vertices == nullptr) {
        return;
    }
    vertices->push_back(SpectrogramRhiLineVertex{
        .x = pixelToClipX(line.x1(), targetSize),
        .y = pixelToClipY(line.y1(), targetSize),
        .r = r,
        .g = g,
        .b = b,
        .a = a,
    });
    vertices->push_back(SpectrogramRhiLineVertex{
        .x = pixelToClipX(line.x2(), targetSize),
        .y = pixelToClipY(line.y2(), targetSize),
        .r = r,
        .g = g,
        .b = b,
        .a = a,
    });
}

std::vector<SpectrogramRhiLineVertex> buildLineVertices(
    const SpectrogramRhiSyncState &state,
    const QSize &targetSize) {
    std::vector<SpectrogramRhiLineVertex> vertices;
    vertices.reserve(
        (state.freqGridLines.size() + state.timeGridLines.size()
            + (state.playheadLine.has_value() ? 1 : 0)
            + (state.crosshairVerticalLine.has_value() ? 1 : 0)
            + (state.crosshairHorizontalLine.has_value() ? 1 : 0))
        * 2);

    for (const QLineF &line : state.freqGridLines) {
        appendLineVertices(&vertices, line, targetSize, 1.0f, 1.0f, 1.0f, 50.0f / 255.0f);
    }
    for (const QLineF &line : state.timeGridLines) {
        appendLineVertices(&vertices, line, targetSize, 1.0f, 1.0f, 1.0f, 50.0f / 255.0f);
    }
    if (state.playheadLine.has_value()) {
        appendLineVertices(
            &vertices, *state.playheadLine, targetSize, 1.0f, 1.0f, 1.0f, 128.0f / 255.0f);
    }
    if (state.crosshairVerticalLine.has_value()) {
        appendLineVertices(
            &vertices, *state.crosshairVerticalLine, targetSize, 1.0f, 1.0f, 1.0f, 140.0f / 255.0f);
    }
    if (state.crosshairHorizontalLine.has_value()) {
        appendLineVertices(
            &vertices, *state.crosshairHorizontalLine, targetSize, 1.0f, 1.0f, 1.0f, 140.0f / 255.0f);
    }

    return vertices;
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
        const bool canDrawLines = prepareLineResources(resourceUpdates, target->pixelSize());

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
        if (canDrawLines && m_linePipeline && m_lineVertexBuffer && m_lineVertexCount > 0) {
            cb->setGraphicsPipeline(m_linePipeline.get());
            cb->setViewport(QRhiViewport(
                0.0f,
                0.0f,
                static_cast<float>(target->pixelSize().width()),
                static_cast<float>(target->pixelSize().height())));
            const QRhiCommandBuffer::VertexInput lineVertexBindings[] = {
                {m_lineVertexBuffer.get(), 0},
            };
            cb->setVertexInput(0, 1, lineVertexBindings);
            cb->draw(m_lineVertexCount);
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
        if (!m_lineVertexShader.isValid()) {
            m_lineVertexShader = loadShaderResource(QString::fromUtf8(kLineVertexShaderResource));
        }
        if (!m_lineFragmentShader.isValid()) {
            m_lineFragmentShader = loadShaderResource(QString::fromUtf8(kLineFragmentShaderResource));
        }
        if (!m_vertexShader.isValid()
            || !m_fragmentShader.isValid()
            || !m_lineVertexShader.isValid()
            || !m_lineFragmentShader.isValid()) {
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

    bool ensureLineVertexBuffer(int vertexCount) {
        if (rhi() == nullptr || vertexCount <= 0) {
            return false;
        }
        if (m_lineVertexBuffer && m_lineVertexCapacity >= vertexCount) {
            return true;
        }

        m_lineVertexBuffer.reset();
        m_lineVertexCapacity = 0;

        const int capacity = std::max(vertexCount, 16);
        m_lineVertexBuffer.reset(rhi()->newBuffer(
            QRhiBuffer::Dynamic,
            QRhiBuffer::VertexBuffer,
            static_cast<quint32>(sizeof(SpectrogramRhiLineVertex) * capacity)));
        if (!m_lineVertexBuffer || !m_lineVertexBuffer->create()) {
            m_lineVertexBuffer.reset();
            return false;
        }

        m_lineVertexCapacity = capacity;
        return true;
    }

    bool ensureTexture(
        std::unique_ptr<QRhiTexture> *texture,
        QSize *cachedSize,
        quint64 *uploadedGeneration,
        const QSize &textureSize) {
        if (rhi() == nullptr || !ensureStaticResources()) {
            return false;
        }
        if (textureSize.isEmpty()) {
            return false;
        }
        if (texture == nullptr || cachedSize == nullptr || uploadedGeneration == nullptr) {
            return false;
        }
        if (*texture && *cachedSize == textureSize) {
            return true;
        }

        m_pipeline.reset();
        m_srb.reset();
        texture->reset();
        *cachedSize = QSize();
        *uploadedGeneration = std::numeric_limits<quint64>::max();

        texture->reset(rhi()->newTexture(QRhiTexture::RGBA8, textureSize));
        if (!*texture || !(*texture)->create()) {
            texture->reset();
            return false;
        }

        *cachedSize = textureSize;
        return true;
    }

    bool ensureShaderResources() {
        if (!m_uniformBuffer
            || !m_sourceTexture
            || !m_dbRemapTexture
            || !m_paletteTexture
            || !m_sampler) {
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
            QRhiShaderResourceBinding::sampledTexture(
                2,
                QRhiShaderResourceBinding::FragmentStage,
                m_dbRemapTexture.get(),
                m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                3,
                QRhiShaderResourceBinding::FragmentStage,
                m_paletteTexture.get(),
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

    bool ensureLinePipeline(QRhiRenderTarget *target) {
        if (target == nullptr || !ensureStaticResources()) {
            return false;
        }

        const int targetSampleCount = target->sampleCount();
        QRhiRenderPassDescriptor *renderPass = target->renderPassDescriptor();
        if (m_linePipeline
            && m_linePipelineSampleCount == targetSampleCount
            && m_linePipelineRenderPass == renderPass) {
            return true;
        }

        QRhiVertexInputLayout inputLayout;
        inputLayout.setBindings({
            QRhiVertexInputBinding(sizeof(SpectrogramRhiLineVertex)),
        });
        inputLayout.setAttributes({
            QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
            QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, sizeof(float) * 2),
        });

        m_linePipeline.reset(rhi()->newGraphicsPipeline());
        if (!m_linePipeline) {
            return false;
        }

        QRhiGraphicsPipeline::TargetBlend blendState;
        blendState.enable = true;
        blendState.srcColor = QRhiGraphicsPipeline::SrcAlpha;
        blendState.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        blendState.srcAlpha = QRhiGraphicsPipeline::One;
        blendState.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

        m_linePipeline->setTopology(QRhiGraphicsPipeline::Lines);
        m_linePipeline->setSampleCount(targetSampleCount);
        m_linePipeline->setTargetBlends({blendState});
        m_linePipeline->setShaderStages({
            QRhiShaderStage(QRhiShaderStage::Vertex, m_lineVertexShader),
            QRhiShaderStage(QRhiShaderStage::Fragment, m_lineFragmentShader),
        });
        m_linePipeline->setVertexInputLayout(inputLayout);
        m_linePipeline->setRenderPassDescriptor(renderPass);
        if (!m_linePipeline->create()) {
            m_linePipeline.reset();
            return false;
        }

        m_linePipelineSampleCount = targetSampleCount;
        m_linePipelineRenderPass = renderPass;
        return true;
    }

    bool prepareDrawResources(QRhiResourceUpdateBatch *resourceUpdates, const QSize &targetSize) {
        if (resourceUpdates == nullptr || targetSize.isEmpty()) {
            return false;
        }

        const QImage sourceImage = prepareTextureImage(m_syncState.sourceImage);
        const QImage dbRemapImage = prepareTextureImage(m_syncState.dbRemapImage);
        const QImage paletteImage = prepareTextureImage(m_syncState.paletteImage);
        if (sourceImage.isNull()
            || dbRemapImage.isNull()
            || paletteImage.isNull()
            || m_syncState.drawPixels <= 0) {
            return false;
        }
        if (!ensureTexture(
                &m_sourceTexture,
                &m_sourceTextureSize,
                &m_uploadedGeneration,
                sourceImage.size())
            || !ensureTexture(
                &m_dbRemapTexture,
                &m_dbRemapTextureSize,
                &m_dbRemapUploadedGeneration,
                dbRemapImage.size())
            || !ensureTexture(
                &m_paletteTexture,
                &m_paletteTextureSize,
                &m_paletteUploadedGeneration,
                paletteImage.size())) {
            return false;
        }
        if (!ensurePipeline(renderTarget())) {
            return false;
        }

        if (m_uploadedGeneration != m_syncState.sourceGeneration) {
            resourceUpdates->uploadTexture(m_sourceTexture.get(), sourceImage);
            m_uploadedGeneration = m_syncState.sourceGeneration;
        }
        if (m_dbRemapUploadedGeneration != m_syncState.dbRemapGeneration) {
            resourceUpdates->uploadTexture(m_dbRemapTexture.get(), dbRemapImage);
            m_dbRemapUploadedGeneration = m_syncState.dbRemapGeneration;
        }
        if (m_paletteUploadedGeneration != m_syncState.paletteGeneration) {
            resourceUpdates->uploadTexture(m_paletteTexture.get(), paletteImage);
            m_paletteUploadedGeneration = m_syncState.paletteGeneration;
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

    bool prepareLineResources(QRhiResourceUpdateBatch *resourceUpdates, const QSize &targetSize) {
        if (resourceUpdates == nullptr || targetSize.isEmpty()) {
            return false;
        }

        const std::vector<SpectrogramRhiLineVertex> lineVertices =
            buildLineVertices(m_syncState, targetSize);
        if (lineVertices.empty()) {
            m_lineVertexCount = 0;
            return false;
        }
        if (!ensureLineVertexBuffer(static_cast<int>(lineVertices.size()))) {
            return false;
        }
        if (!ensureLinePipeline(renderTarget())) {
            return false;
        }

        resourceUpdates->updateDynamicBuffer(
            m_lineVertexBuffer.get(),
            0,
            static_cast<quint32>(sizeof(SpectrogramRhiLineVertex) * lineVertices.size()),
            lineVertices.data());
        m_lineVertexCount = static_cast<quint32>(lineVertices.size());
        return true;
    }

    SpectrogramRhiSyncState m_syncState;
    QShader m_vertexShader;
    QShader m_fragmentShader;
    QShader m_lineVertexShader;
    QShader m_lineFragmentShader;
    std::unique_ptr<QRhiTexture> m_sourceTexture;
    std::unique_ptr<QRhiTexture> m_dbRemapTexture;
    std::unique_ptr<QRhiTexture> m_paletteTexture;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiBuffer> m_vertexBuffer;
    std::unique_ptr<QRhiBuffer> m_lineVertexBuffer;
    std::unique_ptr<QRhiBuffer> m_uniformBuffer;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
    std::unique_ptr<QRhiGraphicsPipeline> m_linePipeline;
    QSize m_sourceTextureSize;
    QSize m_dbRemapTextureSize;
    QSize m_paletteTextureSize;
    quint64 m_uploadedGeneration{std::numeric_limits<quint64>::max()};
    quint64 m_dbRemapUploadedGeneration{std::numeric_limits<quint64>::max()};
    quint64 m_paletteUploadedGeneration{std::numeric_limits<quint64>::max()};
    int m_pipelineSampleCount{0};
    QRhiRenderPassDescriptor *m_pipelineRenderPass{nullptr};
    int m_linePipelineSampleCount{0};
    QRhiRenderPassDescriptor *m_linePipelineRenderPass{nullptr};
    int m_lineVertexCapacity{0};
    quint32 m_lineVertexCount{0};
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
