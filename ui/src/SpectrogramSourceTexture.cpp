// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramSourceTexture.h"

#include <QCoreApplication>
#include <QObject>
#include <QRunnable>
#include <QThread>
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

struct DeferredCleanupTicket {
    explicit DeferredCleanupTicket(std::shared_ptr<std::atomic_int> pendingCleanupCount)
        : pendingCleanupCount(std::move(pendingCleanupCount)) {
    }

    void finish() {
        if (!finished.exchange(true)) {
            pendingCleanupCount->fetch_sub(1);
        }
    }

    std::shared_ptr<std::atomic_int> pendingCleanupCount;
    std::atomic_bool finished{false};
};

bool canDeleteNativeTextureOnContext(
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

bool canDestroyWrapperOnContext(
    QOpenGLContext *currentContext, const QPointer<QOpenGLContext> &ownerContext) {
    return currentContext != nullptr && !ownerContext.isNull() && currentContext == ownerContext;
}

void deleteNativeTexture(const GLuint textureId, QOpenGLContext *context) {
    if (textureId == 0 || context == nullptr) {
        return;
    }
    if (QOpenGLFunctions *functions = context->functions()) {
        GLuint nativeTextureId = textureId;
        functions->glDeleteTextures(1, &nativeTextureId);
    }
}

void finishCleanupTicket(const std::shared_ptr<DeferredCleanupTicket> &cleanupTicket) {
    if (!cleanupTicket) {
        return;
    }

    cleanupTicket->finish();
}

void queueWrapperDeletionOnGuiThread(
    QSGTexture *texture, const std::shared_ptr<DeferredCleanupTicket> &cleanupTicket) {
    if (texture == nullptr) {
        finishCleanupTicket(cleanupTicket);
        return;
    }

    QCoreApplication *application = QCoreApplication::instance();
    if (application != nullptr
        && QMetaObject::invokeMethod(
            application,
            [texture, cleanupTicket]() {
                delete texture;
                finishCleanupTicket(cleanupTicket);
            },
            Qt::QueuedConnection)) {
        return;
    }

    delete texture;
    finishCleanupTicket(cleanupTicket);
}

class TextureDeleteRenderJob final : public QRunnable {
public:
    TextureDeleteRenderJob(
        QSGTexture *texture,
        const GLuint textureId,
        QOpenGLContext *ownerContext,
        QOpenGLContextGroup *shareGroup,
        std::shared_ptr<DeferredCleanupTicket> cleanupTicket)
        : m_texture(texture)
        , m_textureId(textureId)
        , m_ownerContext(ownerContext)
        , m_shareGroup(shareGroup)
        , m_cleanupTicket(std::move(cleanupTicket)) {
    }

    void run() override {
        QOpenGLContext *context = QOpenGLContext::currentContext();
        if (canDestroyWrapperOnContext(context, m_ownerContext)) {
            delete m_texture;
            m_texture = nullptr;
            deleteNativeTexture(m_textureId, context);
            finishCleanupTicket(m_cleanupTicket);
            return;
        }

        if (m_ownerContext.isNull()) {
            delete m_texture;
            m_texture = nullptr;
            if (canDeleteNativeTextureOnContext(context, m_ownerContext, m_shareGroup)) {
                deleteNativeTexture(m_textureId, context);
            }
            finishCleanupTicket(m_cleanupTicket);
            return;
        }

        // Keep the cleanup ticket alive so the owner can observe that cleanup
        // did not run under a compatible scene-graph context.
    }

private:
    QSGTexture *m_texture{nullptr};
    GLuint m_textureId{0};
    QPointer<QOpenGLContext> m_ownerContext;
    QPointer<QOpenGLContextGroup> m_shareGroup;
    std::shared_ptr<DeferredCleanupTicket> m_cleanupTicket;
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
    if (m_texture != nullptr && m_size == QSize(width, height) && m_ownerWindow == window) {
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
    if (!canDeleteNativeTextureOnContext(context, m_ownerContext, m_shareGroup)) {
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

int SpectrogramSourceTexture::pendingCleanupGenerationCount() const {
    return m_pendingCleanupCount->load();
}

void SpectrogramSourceTexture::scheduleDeferredCleanup(
    QSGTexture *texture,
    const GLuint textureId,
    QQuickWindow *ownerWindow,
    QOpenGLContext *ownerContext,
    QOpenGLContextGroup *shareGroup) {
    if ((texture == nullptr && textureId == 0) || ownerWindow == nullptr) {
        return;
    }

    auto cleanupTicket = std::make_shared<DeferredCleanupTicket>(m_pendingCleanupCount);
    cleanupTicket->pendingCleanupCount->fetch_add(1);
    QObject::connect(ownerWindow, &QObject::destroyed, [cleanupTicket](QObject *) {
        finishCleanupTicket(cleanupTicket);
    });

    const QPointer<QQuickWindow> ownerWindowPointer(ownerWindow);
    const QPointer<QOpenGLContext> ownerContextPointer(ownerContext);
    const QPointer<QOpenGLContextGroup> shareGroupPointer(shareGroup);
    const auto scheduleOnOwnerThread =
        [ownerWindowPointer,
         texture,
         textureId,
         ownerContextPointer,
         shareGroupPointer,
         cleanupTicket]() {
            if (ownerWindowPointer.isNull()) {
                queueWrapperDeletionOnGuiThread(texture, cleanupTicket);
                return;
            }

            ownerWindowPointer->scheduleRenderJob(
                new TextureDeleteRenderJob(
                    texture,
                    textureId,
                    ownerContextPointer.data(),
                    shareGroupPointer.data(),
                    cleanupTicket),
                QQuickWindow::AfterSynchronizingStage);
            ownerWindowPointer->update();
        };

    if (ownerWindow->thread() == QThread::currentThread()) {
        scheduleOnOwnerThread();
        return;
    }

    if (!QMetaObject::invokeMethod(ownerWindow, scheduleOnOwnerThread, Qt::QueuedConnection)) {
        QOpenGLContext *context = QOpenGLContext::currentContext();
        if (canDeleteNativeTextureOnContext(context, ownerContextPointer, shareGroupPointer)) {
            deleteNativeTexture(textureId, context);
        }
        queueWrapperDeletionOnGuiThread(texture, cleanupTicket);
    }
}

void SpectrogramSourceTexture::release() {
    QOpenGLContext *context = QOpenGLContext::currentContext();
    const bool can_destroy_wrapper_now = canDestroyWrapperOnContext(context, m_ownerContext);
    const bool can_delete_native_now =
        canDeleteNativeTextureOnContext(context, m_ownerContext, m_shareGroup);

    if (can_destroy_wrapper_now) {
        delete m_texture;
        m_texture = nullptr;
        deleteNativeTexture(m_textureId, context);
        m_textureId = 0;
    } else if ((m_texture != nullptr || m_textureId != 0) && !m_ownerWindow.isNull()) {
        scheduleDeferredCleanup(
            m_texture, m_textureId, m_ownerWindow.data(), m_ownerContext.data(), m_shareGroup.data());
        m_texture = nullptr;
        m_textureId = 0;
    } else if (m_texture == nullptr && can_delete_native_now) {
        deleteNativeTexture(m_textureId, context);
        m_textureId = 0;
    } else {
        Q_ASSERT_X(
            m_texture == nullptr && m_textureId == 0,
            "SpectrogramSourceTexture::release",
            "releasing a live retained source texture requires either the exact owner "
            "context or a live owning QQuickWindow");
    }

    m_size = QSize();
    clearPendingSpans();
    m_ownerWindow.clear();
    m_ownerContext.clear();
    m_shareGroup.clear();
}
