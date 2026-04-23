// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramSourceTexture.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QtQuick/qsgtexture_platform.h>

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

    release();

    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        return false;
    }
    QOpenGLFunctions *functions = context->functions();
    if (functions == nullptr) {
        return false;
    }

    functions->glGenTextures(1, &m_textureId);
    if (m_textureId == 0) {
        return false;
    }

    functions->glBindTexture(GL_TEXTURE_2D, m_textureId);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    functions->glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    m_texture = QNativeInterface::QSGOpenGLTexture::fromNative(
        m_textureId, window, QSize(width, height), {});
    if (m_texture == nullptr) {
        functions->glDeleteTextures(1, &m_textureId);
        m_textureId = 0;
        return false;
    }

    m_size = QSize(width, height);
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
        if (QOpenGLContext *context = QOpenGLContext::currentContext()) {
            if (QOpenGLFunctions *functions = context->functions()) {
                functions->glDeleteTextures(1, &m_textureId);
            }
        }
        m_textureId = 0;
    }

    m_size = QSize();
    clearPendingSpans();
}
