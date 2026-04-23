// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramSourceTexture.h"

#include <QCoreApplication>
#include <QOffscreenSurface>
#include <QObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QRunnable>
#include <QScreen>
#include <QThread>
#include <QtQuick/qsgtexture_platform.h>

#include <mutex>

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

struct SpectrogramSourceTextureActiveOwnerState {
    SpectrogramSourceTextureActiveOwnerState(
        QQuickWindow *window,
        QOpenGLContext *context,
        QOpenGLContextGroup *group,
        QVector<QPointer<QOpenGLContext>> sharedContextsSnapshot)
        : ownerWindow(window)
        , ownerContext(context)
        , shareGroup(group)
        , sharedContexts(std::move(sharedContextsSnapshot)) {
    }

    QPointer<QQuickWindow> ownerWindow;
    QPointer<QOpenGLContext> ownerContext;
    QPointer<QOpenGLContextGroup> shareGroup;
    QVector<QPointer<QOpenGLContext>> sharedContexts;
    QMetaObject::Connection ownerWindowDestroyedConnection;
    QMetaObject::Connection ownerContextDestroyedConnection;
    QMetaObject::Connection sceneGraphInvalidatedConnection;
};

struct SpectrogramSourceTextureDeferredCleanupState {
    SpectrogramSourceTextureDeferredCleanupState(
        QSGTexture *wrappedTexture,
        const GLuint nativeTextureId,
        QQuickWindow *window,
        QOpenGLContext *context,
        QOpenGLContextGroup *group,
        QVector<QPointer<QOpenGLContext>> sharedContextsSnapshot,
        std::shared_ptr<DeferredCleanupTicket> ticket,
        std::shared_ptr<std::atomic_int> attemptCount,
        std::shared_ptr<std::atomic<std::uintptr_t>> wrapperDeletionThreadId)
        : ownerWindow(window)
        , ownerContext(context)
        , shareGroup(group)
        , sharedContexts(std::move(sharedContextsSnapshot))
        , cleanupTicket(std::move(ticket))
        , cleanupAttemptCount(std::move(attemptCount))
        , wrapperDeletionThreadId(std::move(wrapperDeletionThreadId))
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

    [[nodiscard]] bool hasTexture() const {
        const std::lock_guard<std::mutex> lock(mutex);
        return texture != nullptr;
    }

    [[nodiscard]] bool hasTextureId() const {
        const std::lock_guard<std::mutex> lock(mutex);
        return textureId != 0;
    }

    void abandonNativeTexture() {
        const std::lock_guard<std::mutex> lock(mutex);
        textureId = 0;
    }

    QPointer<QQuickWindow> ownerWindow;
    QPointer<QOpenGLContext> ownerContext;
    QPointer<QOpenGLContextGroup> shareGroup;
    QVector<QPointer<QOpenGLContext>> sharedContexts;
    std::shared_ptr<DeferredCleanupTicket> cleanupTicket;
    std::shared_ptr<std::atomic_int> cleanupAttemptCount;
    std::shared_ptr<std::atomic<std::uintptr_t>> wrapperDeletionThreadId;
    QMetaObject::Connection ownerWindowDestroyedConnection;
    QMetaObject::Connection ownerContextDestroyedConnection;
    QMetaObject::Connection sceneGraphInvalidatedConnection;

private:
    mutable std::mutex mutex;
    QSGTexture *texture{nullptr};
    GLuint textureId{0};
};

namespace {

struct CreatedSourceTexture {
    GLuint textureId{0};
    QSize size;
    QSGTexture *texture{nullptr};
    QPointer<QQuickWindow> ownerWindow;
    QPointer<QOpenGLContext> ownerContext;
    QPointer<QOpenGLContextGroup> shareGroup;
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

void disconnectCleanupWatchers(
    const std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> &cleanupState);
void disconnectActiveOwnerWatchers(
    const std::shared_ptr<SpectrogramSourceTextureActiveOwnerState> &ownerState);

void finishCleanupTicket(const std::shared_ptr<DeferredCleanupTicket> &cleanupTicket) {
    if (!cleanupTicket) {
        return;
    }

    cleanupTicket->finish();
}

void finishDeferredCleanup(
    const std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> &cleanupState) {
    disconnectCleanupWatchers(cleanupState);
    finishCleanupTicket(cleanupState->cleanupTicket);
}

void maybeFinishDeferredCleanup(
    const std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> &cleanupState) {
    if (!cleanupState->hasTexture() && !cleanupState->hasTextureId()) {
        finishDeferredCleanup(cleanupState);
    }
}

void deleteWrapperOnCurrentRenderThread(
    const std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> &cleanupState) {
    if (QSGTexture *texture = cleanupState->takeTexture()) {
        cleanupState->wrapperDeletionThreadId->store(
            reinterpret_cast<std::uintptr_t>(QThread::currentThread()));
        delete texture;
    }
    maybeFinishDeferredCleanup(cleanupState);
}

QVector<QPointer<QOpenGLContext>> snapshotSharedContexts(QOpenGLContextGroup *shareGroup) {
    QVector<QPointer<QOpenGLContext>> sharedContexts;
    if (shareGroup == nullptr) {
        return sharedContexts;
    }

    const QList<QOpenGLContext *> liveContexts = shareGroup->shares();
    sharedContexts.reserve(liveContexts.size());
    for (QOpenGLContext *context : liveContexts) {
        if (context != nullptr) {
            sharedContexts.push_back(context);
        }
    }
    return sharedContexts;
}

void disconnectCleanupWatchers(
    const std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> &cleanupState) {
    QObject::disconnect(cleanupState->ownerWindowDestroyedConnection);
    QObject::disconnect(cleanupState->ownerContextDestroyedConnection);
    QObject::disconnect(cleanupState->sceneGraphInvalidatedConnection);
    cleanupState->ownerWindowDestroyedConnection = {};
    cleanupState->ownerContextDestroyedConnection = {};
    cleanupState->sceneGraphInvalidatedConnection = {};
}

void disconnectActiveOwnerWatchers(
    const std::shared_ptr<SpectrogramSourceTextureActiveOwnerState> &ownerState) {
    if (!ownerState) {
        return;
    }

    QObject::disconnect(ownerState->ownerWindowDestroyedConnection);
    QObject::disconnect(ownerState->ownerContextDestroyedConnection);
    QObject::disconnect(ownerState->sceneGraphInvalidatedConnection);
    ownerState->ownerWindowDestroyedConnection = {};
    ownerState->ownerContextDestroyedConnection = {};
    ownerState->sceneGraphInvalidatedConnection = {};
}

void runDeferredCleanupAttempt(
    const std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> &cleanupState);

bool tryScheduleCleanupAttemptOnOwnerWindow(
    const std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> &cleanupState);
std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> makeDeferredCleanupState(
    QSGTexture *texture,
    GLuint textureId,
    QQuickWindow *ownerWindow,
    QOpenGLContext *ownerContext,
    QOpenGLContextGroup *shareGroup,
    const std::shared_ptr<std::atomic_int> &pendingCleanupCount,
    const std::shared_ptr<std::atomic_int> &attemptCount,
    const std::shared_ptr<std::atomic<std::uintptr_t>> &wrapperDeletionThreadId);
void scheduleDeferredCleanupState(
    const std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> &cleanupState,
    bool watchOwnerWindow,
    bool watchOwnerContext,
    bool preferOwnerWindowAttempt);

bool tryScheduleSharedContextCleanup(
    const std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> &cleanupState) {
    if (cleanupState->sharedContexts.isEmpty()) {
        return false;
    }

    QCoreApplication *application = QCoreApplication::instance();
    if (application == nullptr) {
        return false;
    }

    const auto cleanupOnSharedContext = [cleanupState]() {
        cleanupState->noteAttempt();

        for (const QPointer<QOpenGLContext> &sharedContextPointer : cleanupState->sharedContexts) {
            QOpenGLContext *sharedContext = sharedContextPointer.data();
            if (sharedContext == nullptr || !sharedContext->isValid()) {
                continue;
            }

            QOpenGLContext cleanupContext;
            cleanupContext.setFormat(sharedContext->format());
            cleanupContext.setShareContext(sharedContext);
            if (QScreen *screen = sharedContext->screen()) {
                cleanupContext.setScreen(screen);
            }
            if (!cleanupContext.create()) {
                continue;
            }

            QOffscreenSurface cleanupSurface;
            cleanupSurface.setFormat(cleanupContext.format());
            if (QScreen *screen = sharedContext->screen()) {
                cleanupSurface.setScreen(screen);
            }
            cleanupSurface.create();
            if (!cleanupSurface.isValid() || !cleanupContext.makeCurrent(&cleanupSurface)) {
                continue;
            }

            deleteNativeTexture(cleanupState->takeTextureId(), &cleanupContext);
            cleanupContext.doneCurrent();
            if (cleanupState->hasTexture()) {
                if (tryScheduleCleanupAttemptOnOwnerWindow(cleanupState)) {
                    return;
                }
            }
            maybeFinishDeferredCleanup(cleanupState);
            return;
        }

        cleanupState->abandonNativeTexture();
        if (cleanupState->hasTexture()) {
            if (tryScheduleCleanupAttemptOnOwnerWindow(cleanupState)) {
                return;
            }
        }
        maybeFinishDeferredCleanup(cleanupState);
    };

    return QMetaObject::invokeMethod(application, cleanupOnSharedContext, Qt::QueuedConnection);
}

class TextureDeleteRenderJob final : public QRunnable {
public:
    explicit TextureDeleteRenderJob(
        std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> cleanupState)
        : m_cleanupState(std::move(cleanupState)) {
    }

    void run() override {
        runDeferredCleanupAttempt(m_cleanupState);
    }

private:
    std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> m_cleanupState;
};

void runDeferredCleanupAttempt(
    const std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> &cleanupState) {
    cleanupState->noteAttempt();
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!cleanupState->hasTextureId()) {
        deleteWrapperOnCurrentRenderThread(cleanupState);
        return;
    }

    if (cleanupState->ownerContext.isNull()) {
        if (canDeleteNativeTextureOnContext(context, cleanupState->ownerContext, cleanupState->shareGroup)) {
            deleteNativeTexture(cleanupState->takeTextureId(), context);
        } else if (!tryScheduleSharedContextCleanup(cleanupState)) {
            cleanupState->abandonNativeTexture();
        }
        deleteWrapperOnCurrentRenderThread(cleanupState);
        return;
    }

    if (canDeleteNativeTextureOnContext(context, cleanupState->ownerContext, cleanupState->shareGroup)) {
        deleteNativeTexture(cleanupState->takeTextureId(), context);
        deleteWrapperOnCurrentRenderThread(cleanupState);
        return;
    }

    if (tryScheduleSharedContextCleanup(cleanupState)) {
        deleteWrapperOnCurrentRenderThread(cleanupState);
        return;
    }

    // Keep the cleanup state alive until the owner context/window destruction
    // path can hand wrapper cleanup to a safe fallback.
}

bool tryScheduleCleanupAttemptOnOwnerWindow(
    const std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> &cleanupState) {
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

std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> makeDeferredCleanupState(
    QSGTexture *texture,
    const GLuint textureId,
    QQuickWindow *ownerWindow,
    QOpenGLContext *ownerContext,
    QOpenGLContextGroup *shareGroup,
    const std::shared_ptr<std::atomic_int> &pendingCleanupCount,
    const std::shared_ptr<std::atomic_int> &attemptCount,
    const std::shared_ptr<std::atomic<std::uintptr_t>> &wrapperDeletionThreadId) {
    auto cleanupTicket = std::make_shared<DeferredCleanupTicket>(pendingCleanupCount);
    cleanupTicket->pendingCleanupCount->fetch_add(1);
    return std::make_shared<SpectrogramSourceTextureDeferredCleanupState>(
        texture,
        textureId,
        ownerWindow,
        ownerContext,
        shareGroup,
        snapshotSharedContexts(shareGroup),
        cleanupTicket,
        attemptCount,
        wrapperDeletionThreadId);
}

void scheduleDeferredCleanupState(
    const std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState> &cleanupState,
    const bool watchOwnerWindow,
    const bool watchOwnerContext,
    const bool preferOwnerWindowAttempt) {
    if (watchOwnerWindow && !cleanupState->ownerWindow.isNull()) {
        cleanupState->sceneGraphInvalidatedConnection = QObject::connect(
            cleanupState->ownerWindow.data(),
            &QQuickWindow::sceneGraphInvalidated,
            cleanupState->ownerWindow.data(),
            [cleanupState]() {
                runDeferredCleanupAttempt(cleanupState);
            },
            Qt::DirectConnection);
        cleanupState->ownerWindowDestroyedConnection = QObject::connect(
            cleanupState->ownerWindow.data(),
            &QObject::destroyed,
            [cleanupState](QObject *) {
                if (tryScheduleSharedContextCleanup(cleanupState)) {
                    return;
                }
                cleanupState->abandonNativeTexture();
                maybeFinishDeferredCleanup(cleanupState);
            });
    }

    if (watchOwnerContext && !cleanupState->ownerContext.isNull()) {
        cleanupState->ownerContextDestroyedConnection = QObject::connect(
            cleanupState->ownerContext.data(),
            &QObject::destroyed,
            [cleanupState](QObject *) {
                if (tryScheduleSharedContextCleanup(cleanupState)) {
                    return;
                }
                if (tryScheduleCleanupAttemptOnOwnerWindow(cleanupState)) {
                    return;
                }
                cleanupState->abandonNativeTexture();
                maybeFinishDeferredCleanup(cleanupState);
            });
    }

    if (preferOwnerWindowAttempt && tryScheduleCleanupAttemptOnOwnerWindow(cleanupState)) {
        return;
    }
    if (tryScheduleSharedContextCleanup(cleanupState)) {
        return;
    }

    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (canDeleteNativeTextureOnContext(context, cleanupState->ownerContext, cleanupState->shareGroup)) {
        deleteNativeTexture(cleanupState->takeTextureId(), context);
    } else {
        cleanupState->abandonNativeTexture();
    }
    if (cleanupState->hasTexture()) {
        if (tryScheduleCleanupAttemptOnOwnerWindow(cleanupState)) {
            return;
        }
    }
    maybeFinishDeferredCleanup(cleanupState);
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

SpectrogramSourceTexture::SpectrogramSourceTexture() = default;

SpectrogramSourceTexture::~SpectrogramSourceTexture() {
    const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
    m_ownerStateAlive->store(false);
    release();
}

void SpectrogramSourceTexture::armCleanupOwnerForCurrentTexture() {
    const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
    disarmCleanupOwner();
    if ((m_texture == nullptr && m_textureId == 0) || m_ownerWindow.isNull()) {
        return;
    }

    const auto ownerStateMutex = m_ownerStateMutex;
    const auto ownerStateAlive = m_ownerStateAlive;
    auto ownerState = std::make_shared<SpectrogramSourceTextureActiveOwnerState>(
        m_ownerWindow.data(),
        m_ownerContext.data(),
        m_shareGroup.data(),
        snapshotSharedContexts(m_shareGroup.data()));
    ownerState->sceneGraphInvalidatedConnection = QObject::connect(
        m_ownerWindow.data(),
        &QQuickWindow::sceneGraphInvalidated,
        m_ownerWindow.data(),
        [this, ownerState, ownerStateMutex, ownerStateAlive]() {
            const std::lock_guard<std::recursive_mutex> callbackLock(*ownerStateMutex);
            if (!ownerStateAlive->load()) {
                return;
            }
            cleanUpActiveOwnerOnRenderThread(ownerState);
        },
        Qt::DirectConnection);
    ownerState->ownerWindowDestroyedConnection = QObject::connect(
        m_ownerWindow.data(),
        &QObject::destroyed,
        [this, ownerState, ownerStateMutex, ownerStateAlive](QObject *) {
            const std::lock_guard<std::recursive_mutex> callbackLock(*ownerStateMutex);
            if (!ownerStateAlive->load()) {
                return;
            }
            handOffActiveCleanupOwner(
                ownerState,
                false,
                false,
                !ownerState->ownerContext.isNull());
        });
    if (!m_ownerContext.isNull()) {
        ownerState->ownerContextDestroyedConnection = QObject::connect(
            m_ownerContext.data(),
            &QObject::destroyed,
            [this, ownerState, ownerStateMutex, ownerStateAlive](QObject *) {
                const std::lock_guard<std::recursive_mutex> callbackLock(*ownerStateMutex);
                if (!ownerStateAlive->load()) {
                    return;
                }
                handOffActiveCleanupOwner(ownerState, true, true, false);
            });
    }
    m_activeOwnerState = std::move(ownerState);
}

void SpectrogramSourceTexture::cleanUpActiveOwnerOnRenderThread(
    const std::shared_ptr<SpectrogramSourceTextureActiveOwnerState> &ownerState) {
    if (auto cleanupState = takeActiveCleanupState(ownerState)) {
        runDeferredCleanupAttempt(cleanupState);
    }
}

void SpectrogramSourceTexture::disarmCleanupOwner() {
    const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
    disconnectActiveOwnerWatchers(m_activeOwnerState);
    m_activeOwnerState.reset();
}

std::shared_ptr<SpectrogramSourceTextureDeferredCleanupState>
SpectrogramSourceTexture::takeActiveCleanupState(
    const std::shared_ptr<SpectrogramSourceTextureActiveOwnerState> &ownerState) {
    const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
    if (m_activeOwnerState != ownerState || (m_texture == nullptr && m_textureId == 0)) {
        return {};
    }

    auto cleanupState = makeDeferredCleanupState(
        m_texture,
        m_textureId,
        ownerState->ownerWindow.data(),
        ownerState->ownerContext.data(),
        ownerState->shareGroup.data(),
        m_pendingCleanupCount,
        m_pendingCleanupAttemptCount,
        m_lastWrapperDeletionThreadId);
    cleanupState->sharedContexts = ownerState->sharedContexts;

    disarmCleanupOwner();
    m_texture = nullptr;
    m_textureId = 0;
    m_size = QSize();
    clearPendingSpans();
    m_ownerWindow.clear();
    m_ownerContext.clear();
    m_shareGroup.clear();

    return cleanupState;
}

void SpectrogramSourceTexture::handOffActiveCleanupOwner(
    const std::shared_ptr<SpectrogramSourceTextureActiveOwnerState> &ownerState,
    const bool preferOwnerWindowAttempt,
    const bool watchOwnerWindow,
    const bool watchOwnerContext) {
    if (auto cleanupState = takeActiveCleanupState(ownerState)) {
        scheduleDeferredCleanupState(
            cleanupState,
            watchOwnerWindow,
            watchOwnerContext,
            preferOwnerWindowAttempt);
    }
}

QSize SpectrogramSourceTexture::size() const {
    const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
    return m_size;
}

QSGTexture *SpectrogramSourceTexture::texture() const {
    const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
    return m_texture;
}

int SpectrogramSourceTexture::pendingUploadSpanCount() const {
    const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
    return m_pendingUploadedSpans.size();
}

bool SpectrogramSourceTexture::matches(QQuickWindow *window, const QSize &size) const {
    const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
    return m_texture != nullptr
        && !size.isEmpty()
        && m_size == size
        && m_ownerWindow == window;
}

bool SpectrogramSourceTexture::ensureCreated(QQuickWindow *window, const int width, const int height) {
    if (window == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    {
        const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
        if (matches(window, QSize(width, height))) {
            return true;
        }
    }

    CreatedSourceTexture createdTexture;
    if (!createWrappedTexture(window, width, height, &createdTexture)) {
        return false;
    }

    release();
    {
        const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
        m_textureId = createdTexture.textureId;
        m_size = createdTexture.size;
        m_texture = createdTexture.texture;
        m_ownerWindow = createdTexture.ownerWindow;
        m_ownerContext = createdTexture.ownerContext;
        m_shareGroup = createdTexture.shareGroup;
        armCleanupOwnerForCurrentTexture();
    }
    return true;
}

bool SpectrogramSourceTexture::uploadRgbaColumns(
    QQuickWindow *window,
    const int xOffset,
    const int width,
    const int height,
    const QByteArray &rgbaBytes) {
    QSize textureSize;
    GLuint textureId = 0;
    QPointer<QOpenGLContext> ownerContext;
    QPointer<QOpenGLContextGroup> shareGroup;
    {
        const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
        textureSize = m_size;
        textureId = m_textureId;
        ownerContext = m_ownerContext;
        shareGroup = m_shareGroup;
    }

    if (window == nullptr || textureSize.isEmpty()) {
        return false;
    }
    if (xOffset < 0 || width <= 0 || height <= 0) {
        return false;
    }
    if (height != textureSize.height() || xOffset + width > textureSize.width()) {
        return false;
    }
    if (rgbaBytes.size() != width * height * 4) {
        return false;
    }
    if (!ensureCreated(window, textureSize.width(), textureSize.height())) {
        return false;
    }

    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        return false;
    }
    {
        const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
        textureId = m_textureId;
        ownerContext = m_ownerContext;
        shareGroup = m_shareGroup;
    }
    if (!canDeleteNativeTextureOnContext(context, ownerContext, shareGroup)) {
        return false;
    }
    QOpenGLFunctions *functions = context->functions();
    if (functions == nullptr) {
        return false;
    }

    functions->glBindTexture(GL_TEXTURE_2D, textureId);
    functions->glTexSubImage2D(
        GL_TEXTURE_2D, 0, xOffset, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaBytes.constData());
    return true;
}

void SpectrogramSourceTexture::noteUploadedSpan(const SpectrogramSourceUploadSpan &span) {
    const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
    m_pendingUploadedSpans.push_back(span);
}

void SpectrogramSourceTexture::clearPendingSpans() {
    const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
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

    auto cleanupState = makeDeferredCleanupState(
        texture,
        textureId,
        ownerWindow,
        ownerContext,
        shareGroup,
        m_pendingCleanupCount,
        m_pendingCleanupAttemptCount,
        m_lastWrapperDeletionThreadId);
    scheduleDeferredCleanupState(cleanupState, true, ownerContext != nullptr, true);
}

void SpectrogramSourceTexture::release() {
    const std::lock_guard<std::recursive_mutex> lock(*m_ownerStateMutex);
    QOpenGLContext *context = QOpenGLContext::currentContext();
    const bool can_destroy_wrapper_now = canDestroyWrapperOnContext(context, m_ownerContext);
    const bool can_delete_native_now =
        canDeleteNativeTextureOnContext(context, m_ownerContext, m_shareGroup);

    if (can_destroy_wrapper_now) {
        disarmCleanupOwner();
        m_lastWrapperDeletionThreadId->store(
            reinterpret_cast<std::uintptr_t>(QThread::currentThread()));
        delete m_texture;
        m_texture = nullptr;
        deleteNativeTexture(m_textureId, context);
        m_textureId = 0;
    } else if ((m_texture != nullptr || m_textureId != 0) && m_activeOwnerState != nullptr) {
        auto cleanupState = makeDeferredCleanupState(
            m_texture,
            m_textureId,
            m_activeOwnerState->ownerWindow.data(),
            m_activeOwnerState->ownerContext.data(),
            m_activeOwnerState->shareGroup.data(),
            m_pendingCleanupCount,
            m_pendingCleanupAttemptCount,
            m_lastWrapperDeletionThreadId);
        cleanupState->sharedContexts = m_activeOwnerState->sharedContexts;
        const bool watchOwnerWindow = !m_activeOwnerState->ownerWindow.isNull();
        const bool watchOwnerContext = !m_activeOwnerState->ownerContext.isNull();
        disarmCleanupOwner();
        m_texture = nullptr;
        m_textureId = 0;
        scheduleDeferredCleanupState(
            cleanupState, watchOwnerWindow, watchOwnerContext, watchOwnerWindow);
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
