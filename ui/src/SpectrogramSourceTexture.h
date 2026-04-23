// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QPointer>
#include <QQuickWindow>
#include <QSGTexture>
#include <QRect>
#include <QSize>
#include <QVector>

#include <atomic>
#include <algorithm>
#include <memory>

struct SpectrogramSourceUploadSpan {
    int xBegin{0};
    int xEnd{0};
};

struct SpectrogramSourceColumnRange {
    qint64 xBegin{0};
    qint64 xEnd{-1};

    [[nodiscard]] qint64 width() const {
        return std::max<qint64>(0, xEnd - xBegin + 1);
    }
};

struct SpectrogramRetainedSourceLayout {
    int capacityColumns{0};
    int binsPerColumn{0};
    qint64 displayLeft{0};
    qint64 displayRight{-1};
    bool rollingMode{false};

    [[nodiscard]] SpectrogramSourceColumnRange visibleColumnRange() const {
        return SpectrogramSourceColumnRange{
            .xBegin = displayLeft,
            .xEnd = displayRight,
        };
    }
};

inline QVector<SpectrogramSourceUploadSpan> mergeSpectrogramSourceUploadSpans(
    const QVector<SpectrogramSourceUploadSpan> &spans) {
    if (spans.isEmpty()) {
        return {};
    }

    QVector<SpectrogramSourceUploadSpan> sorted = spans;
    std::sort(sorted.begin(), sorted.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.xBegin < rhs.xBegin;
    });

    QVector<SpectrogramSourceUploadSpan> merged;
    merged.push_back(sorted.front());
    for (int i = 1; i < sorted.size(); ++i) {
        auto &tail = merged.back();
        if (sorted[i].xBegin <= tail.xEnd + 1) {
            tail.xEnd = std::max(tail.xEnd, sorted[i].xEnd);
        } else {
            merged.push_back(sorted[i]);
        }
    }
    return merged;
}

class QOpenGLContext;
class QOpenGLContextGroup;
class QObject;
struct SpectrogramSourceTextureDeferredCleanupState;

class SpectrogramSourceTexture {
public:
    SpectrogramSourceTexture() = default;
    SpectrogramSourceTexture(const SpectrogramSourceTexture &) = delete;
    SpectrogramSourceTexture &operator=(const SpectrogramSourceTexture &) = delete;
    SpectrogramSourceTexture(SpectrogramSourceTexture &&) = delete;
    SpectrogramSourceTexture &operator=(SpectrogramSourceTexture &&) = delete;
    ~SpectrogramSourceTexture();

    [[nodiscard]] QSize size() const;
    [[nodiscard]] QSGTexture *texture() const;
    [[nodiscard]] int pendingUploadSpanCount() const;

    bool ensureCreated(QQuickWindow *window, int width, int height);
    bool uploadRgbaColumns(
        QQuickWindow *window,
        int xOffset,
        int width,
        int height,
        const QByteArray &rgbaBytes);
    void noteUploadedSpan(const SpectrogramSourceUploadSpan &span);
    void clearPendingSpans();
    void release();

private:
    [[nodiscard]] int pendingCleanupAttemptCount() const;
    [[nodiscard]] int pendingCleanupGenerationCount() const;
    void scheduleDeferredCleanup(
        QSGTexture *texture,
        GLuint textureId,
        QQuickWindow *ownerWindow,
        QOpenGLContext *ownerContext,
        QOpenGLContextGroup *shareGroup);

    GLuint m_textureId{0};
    QSize m_size;
    QSGTexture *m_texture{nullptr};
    QVector<SpectrogramSourceUploadSpan> m_pendingUploadedSpans;
    QPointer<QQuickWindow> m_ownerWindow;
    QPointer<QOpenGLContext> m_ownerContext;
    QPointer<QOpenGLContextGroup> m_shareGroup;
    std::shared_ptr<std::atomic_int> m_pendingCleanupAttemptCount{std::make_shared<std::atomic_int>(0)};
    std::shared_ptr<std::atomic_int> m_pendingCleanupCount{std::make_shared<std::atomic_int>(0)};
};
