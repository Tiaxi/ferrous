// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramSourceTexture.h"

#include <QCoreApplication>
#include <QObject>
#include <QRunnable>
#include <QThread>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QtQuick/qsgtexture_platform.h>

#include <mutex>

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

struct DeferredCleanupState {
    DeferredCleanupState(
        QSGTexture *wrappedTexture,
        const GLuint nativeTextureId,
        QQuickWindow *window,
        QOpenGLContext *context,
        QOpenGLContextGroup *group,
        std::shared_ptr<DeferredCleanupTicket> ticket,
        std::shared_ptr<std::atomic_int> attemptCount)
        : ownerWindow(window)
        , ownerContext(context)
        , shareGroup(group)
        , cleanupTicket(std::move(ticket))
        , cleanupAttemptCount(std::move(attemptCount))
        , texture(wrappedTexture)
        , textureId(nativeTextureId) {
    }

    void noteAttempt() const {
        cleanupAttemptCount->fetch_add(1);
    }

    [[nodiscard]] QSGTexture *takeTexture() {
        const std::lock_guard<std::mutex> lock(mutex);
        QSGTexture *wrappedTexture = texture;
        texture = nullptr;
        return wrappedTexture;
    }

    [[nodiscard]] GLuint takeTextureId() {
        const std::lock_guard<std::mutex> lock(mutex);
        const GLuint nativeTextureId = textureId;
        textureId = 0;
        return nativeTextureId;
    }

    [[nodiscard]] std::pair<QSGTexture *, GLuint> takeResources() {
        const std::lock_guard<std::mutex> lock(mutex);
        QSGTexture *wrappedTexture = texture;
        const GLuint nativeTextureId = textureId;
        texture = nullptr;
        textureId = 0;
        return {wrappedTexture, nativeTextureId};
    }

    void abandonNativeTexture() {
        const std::lock_guard<std::mutex> lock(mutex);
        textureId = 0;
    }

    QPointer<QQuickWindow> ownerWindow;
    QPointer<QOpenGLContext> ownerContext;
    QPointer<QOpenGLContextGroup> shareGroup;
    std::shared_ptr<DeferredCleanupTicket> cleanupTicket;
    std::shared_ptr<std::atomic_int> cleanupAttemptCount;

private:
    mutable std::mutex mutex;
    QSGTexture *texture{nullptr};
    GLuint textureId{0};
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

void queueWrapperDeletionOnGuiThread(const std::shared_ptr<DeferredCleanupState> &cleanupState) {
    QSGTexture *texture = cleanupState->takeTexture();
    if (texture == nullptr) {
        finishCleanupTicket(cleanupState->cleanupTicket);
        return;
    }

    QCoreApplication *application = QCoreApplication::instance();
    if (application != nullptr
        && QMetaObject::invokeMethod(
            application,
            [texture, cleanupTicket = cleanupState->cleanupTicket]() {
                delete texture;
                finishCleanupTicket(cleanupTicket);
            },
            Qt::QueuedConnection)) {
        return;
    }

    delete texture;
    finishCleanupTicket(cleanupState->cleanupTicket);
}

class TextureDeleteRenderJob final : public QRunnable {
public:
    explicit TextureDeleteRenderJob(std::shared_ptr<DeferredCleanupState> cleanupState)
        : m_cleanupState(std::move(cleanupState)) {
    }

    void run() override {
        m_cleanupState->noteAttempt();
        QOpenGLContext *context = QOpenGLContext::currentContext();
        if (canDestroyWrapperOnContext(context, m_cleanupState->ownerContext)) {
            const auto [texture, textureId] = m_cleanupState->takeResources();
            delete texture;
            deleteNativeTexture(textureId, context);
            finishCleanupTicket(m_cleanupState->cleanupTicket);
            return;
        }

        if (m_cleanupState->ownerContext.isNull()) {
            if (canDeleteNativeTextureOnContext(context, m_cleanupState->ownerContext, m_cleanupState->shareGroup)) {
                deleteNativeTexture(m_cleanupState->takeTextureId(), context);
                queueWrapperDeletionOnGuiThread(m_cleanupState);
            }
            return;
        }

        if (canDeleteNativeTextureOnContext(context, m_cleanupState->ownerContext, m_cleanupState->shareGroup)) {
            deleteNativeTexture(m_cleanupState->takeTextureId(), context);
        }

        // Keep the cleanup state alive until the owner context/window destruction
        // path can hand wrapper cleanup to a safe fallback.
    }

private:
    std::shared_ptr<DeferredCleanupState> m_cleanupState;
};

bool tryScheduleCleanupAttemptOnOwnerWindow(const std::shared_ptr<DeferredCleanupState> &cleanupState) {
    QQuickWindow *ownerWindow = cleanupState->ownerWindow.data();
    if (ownerWindow == nullptr) {
        return false;
    }

    const QPointer<QQuickWindow> ownerWindowPointer(ownerWindow);
    const auto scheduleOnOwnerThread = [ownerWindowPointer, cleanupState]() {
        if (ownerWindowPointer.isNull()) {
            return;
        }

        ownerWindowPointer->scheduleRenderJob(
            new TextureDeleteRenderJob(cleanupState),
            QQuickWindow::AfterSynchronizingStage);
        ownerWindowPointer->update();
    };

    if (ownerWindow->thread() == QThread::currentThread()) {
        scheduleOnOwnerThread();
        return true;
    }

    return QMetaObject::invokeMethod(ownerWindow, scheduleOnOwnerThread, Qt::QueuedConnection);
}

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

int SpectrogramSourceTexture::pendingCleanupAttemptCount() const {
    return m_pendingCleanupAttemptCount->load();
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
    auto cleanupState = std::make_shared<DeferredCleanupState>(
        texture,
        textureId,
        ownerWindow,
        ownerContext,
        shareGroup,
        cleanupTicket,
        m_pendingCleanupAttemptCount);
    QObject::connect(ownerWindow, &QObject::destroyed, [cleanupState](QObject *) {
        cleanupState->abandonNativeTexture();
        queueWrapperDeletionOnGuiThread(cleanupState);
    });
    if (ownerContext != nullptr) {
        QObject::connect(ownerContext, &QObject::destroyed, [cleanupState](QObject *) {
            if (tryScheduleCleanupAttemptOnOwnerWindow(cleanupState)) {
                return;
            }
            cleanupState->abandonNativeTexture();
            queueWrapperDeletionOnGuiThread(cleanupState);
        });
    }

    const QPointer<QOpenGLContext> ownerContextPointer(ownerContext);
    const QPointer<QOpenGLContextGroup> shareGroupPointer(shareGroup);
    if (tryScheduleCleanupAttemptOnOwnerWindow(cleanupState)) {
        return;
    }

    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (canDeleteNativeTextureOnContext(context, ownerContextPointer, shareGroupPointer)) {
        deleteNativeTexture(cleanupState->takeTextureId(), context);
    } else {
        cleanupState->abandonNativeTexture();
    }
    queueWrapperDeletionOnGuiThread(cleanupState);
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
