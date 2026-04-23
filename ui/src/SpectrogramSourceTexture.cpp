// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramSourceTexture.h"

#include <QRunnable>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QtQuick/qsgtexture_platform.h>

namespace {

struct CreatedSourceTexture {
    GLuint textureId{0};
    QSize size;
    QSGTexture *texture{nullptr};
    QPointer<QQuickWindow> ownerWindow;
    QPointer<QOpenGLContext> ownerContext;
    QPointer<QOpenGLContextGroup> shareGroup;
};

bool canDeleteTextureOnContext(
    QOpenGLContext *currentContext,
    const QPointer<QOpenGLContext> &ownerContext,
    const QPointer<QOpenGLContextGroup> &shareGroup) {
    if (currentContext == nullptr) {
        return false;
    }
    if (!ownerContext.isNull()) {
        return currentContext == ownerContext
            || QOpenGLContext::areSharing(currentContext, ownerContext);
    }
    return !shareGroup.isNull() && currentContext->shareGroup() == shareGroup;
}

void finishCleanupTicket(QObject *cleanupTicket) {
    if (cleanupTicket == nullptr) {
        return;
    }

    QMetaObject::invokeMethod(cleanupTicket, &QObject::deleteLater, Qt::QueuedConnection);
}

class TextureDeleteRenderJob final : public QRunnable {
public:
    TextureDeleteRenderJob(
        const GLuint textureId,
        QQuickWindow *ownerWindow,
        QOpenGLContext *ownerContext,
        QOpenGLContextGroup *shareGroup,
        QObject *cleanupTicket)
        : m_textureId(textureId)
        , m_ownerWindow(ownerWindow)
        , m_ownerContext(ownerContext)
        , m_shareGroup(shareGroup)
        , m_cleanupTicket(cleanupTicket) {
    }

    void run() override {
        if (m_cleanupTicket.isNull()) {
            return;
        }

        QOpenGLContext *context = QOpenGLContext::currentContext();
        if (canDeleteTextureOnContext(context, m_ownerContext, m_shareGroup)) {
            if (QOpenGLFunctions *functions = context->functions()) {
                GLuint textureId = m_textureId;
                functions->glDeleteTextures(1, &textureId);
            }
            finishCleanupTicket(m_cleanupTicket.data());
            return;
        }

        if (!m_ownerContext.isNull() && !m_ownerWindow.isNull()) {
            m_ownerWindow->scheduleRenderJob(
                new TextureDeleteRenderJob(
                    m_textureId,
                    m_ownerWindow.data(),
                    m_ownerContext.data(),
                    m_shareGroup.data(),
                    m_cleanupTicket.data()),
                QQuickWindow::AfterSynchronizingStage);
            m_ownerWindow->update();
            return;
        }

        finishCleanupTicket(m_cleanupTicket.data());
    }

private:
    GLuint m_textureId{0};
    QPointer<QQuickWindow> m_ownerWindow;
    QPointer<QOpenGLContext> m_ownerContext;
    QPointer<QOpenGLContextGroup> m_shareGroup;
    QPointer<QObject> m_cleanupTicket;
};

bool createWrappedTexture(
    QQuickWindow *window,
    const int width,
    const int height,
    CreatedSourceTexture *createdTexture) {
    if (window == nullptr || createdTexture == nullptr) {
        return false;
    }

    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        return false;
    }
    QOpenGLFunctions *functions = context->functions();
    if (functions == nullptr) {
        return false;
    }

    GLuint textureId = 0;
    functions->glGenTextures(1, &textureId);
    if (textureId == 0) {
        return false;
    }

    functions->glBindTexture(GL_TEXTURE_2D, textureId);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    functions->glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    QSGTexture *texture = QNativeInterface::QSGOpenGLTexture::fromNative(
        textureId, window, QSize(width, height), {});
    if (texture == nullptr) {
        functions->glDeleteTextures(1, &textureId);
        return false;
    }

    createdTexture->textureId = textureId;
    createdTexture->size = QSize(width, height);
    createdTexture->texture = texture;
    createdTexture->ownerWindow = window;
    createdTexture->ownerContext = context;
    createdTexture->shareGroup = context->shareGroup();
    return true;
}

} // namespace

SpectrogramSourceTexture::~SpectrogramSourceTexture() {
    release();
}

QSize SpectrogramSourceTexture::size() const {
    return m_size;
}

QSGTexture *SpectrogramSourceTexture::texture() const {
    return m_texture;
}

int SpectrogramSourceTexture::pendingUploadSpanCount() const {
    return m_pendingUploadedSpans.size();
}

bool SpectrogramSourceTexture::ensureCreated(QQuickWindow *window, const int width, const int height) {
    if (window == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    if (m_texture != nullptr && m_size == QSize(width, height)) {
        return true;
    }

    CreatedSourceTexture createdTexture;
    if (!createWrappedTexture(window, width, height, &createdTexture)) {
        return false;
    }

    release();
    m_textureId = createdTexture.textureId;
    m_size = createdTexture.size;
    m_texture = createdTexture.texture;
    m_ownerWindow = createdTexture.ownerWindow;
    m_ownerContext = createdTexture.ownerContext;
    m_shareGroup = createdTexture.shareGroup;
    return true;
}

bool SpectrogramSourceTexture::uploadRgbaColumns(
    QQuickWindow *window,
    const int xOffset,
    const int width,
    const int height,
    const QByteArray &rgbaBytes) {
    if (window == nullptr || m_size.isEmpty()) {
        return false;
    }
    if (xOffset < 0 || width <= 0 || height <= 0) {
        return false;
    }
    if (height != m_size.height() || xOffset + width > m_size.width()) {
        return false;
    }
    if (rgbaBytes.size() != width * height * 4) {
        return false;
    }
    if (!ensureCreated(window, m_size.width(), m_size.height())) {
        return false;
    }

    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        return false;
    }
    QOpenGLFunctions *functions = context->functions();
    if (functions == nullptr) {
        return false;
    }

    functions->glBindTexture(GL_TEXTURE_2D, m_textureId);
    functions->glTexSubImage2D(
        GL_TEXTURE_2D, 0, xOffset, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaBytes.constData());
    return true;
}

void SpectrogramSourceTexture::noteUploadedSpan(const SpectrogramSourceUploadSpan &span) {
    m_pendingUploadedSpans.push_back(span);
}

void SpectrogramSourceTexture::clearPendingSpans() {
    m_pendingUploadedSpans.clear();
}

void SpectrogramSourceTexture::release() {
    delete m_texture;
    m_texture = nullptr;

    if (m_textureId != 0) {
        QOpenGLContext *context = QOpenGLContext::currentContext();
        if (canDeleteTextureOnContext(context, m_ownerContext, m_shareGroup)) {
            if (QOpenGLFunctions *functions = context->functions()) {
                GLuint textureId = m_textureId;
                functions->glDeleteTextures(1, &textureId);
            }
        } else if (!m_ownerWindow.isNull()) {
            auto *cleanupTicket = new QObject();
            m_pendingCleanupJob = cleanupTicket;
            m_ownerWindow->scheduleRenderJob(
                new TextureDeleteRenderJob(
                    m_textureId,
                    m_ownerWindow.data(),
                    m_ownerContext.data(),
                    m_shareGroup.data(),
                    cleanupTicket),
                QQuickWindow::AfterSynchronizingStage);
            m_ownerWindow->update();
        }
        m_textureId = 0;
    }

    m_size = QSize();
    clearPendingSpans();
    m_ownerWindow.clear();
    m_ownerContext.clear();
    m_shareGroup.clear();
}
