// SPDX-License-Identifier: GPL-3.0-or-later

#include "CoverImageProvider.h"

#include <QImageReader>
#include <QQuickTextureFactory>

// --- CoverImageResponse ---

CoverImageResponse::CoverImageResponse(
    const QString &filePath, const QSize &requestedSize)
    : m_filePath(filePath), m_requestedSize(requestedSize) {
    setAutoDelete(false);
}

QQuickTextureFactory *CoverImageResponse::textureFactory() const {
    if (m_image.isNull()) {
        return nullptr;
    }
    return QQuickTextureFactory::textureFactoryForImage(m_image);
}

QString CoverImageResponse::errorString() const {
    return m_errorString;
}

void CoverImageResponse::cancel() {
    m_cancelled.storeRelaxed(1);
}

void CoverImageResponse::run() {
    if (m_cancelled.loadRelaxed()) {
        emit finished();
        return;
    }

    QImageReader reader(m_filePath);
    if (m_requestedSize.isValid()) {
        reader.setScaledSize(
            reader.size().scaled(m_requestedSize, Qt::KeepAspectRatio));
    }

    m_image = reader.read();

    if (m_image.isNull() && !m_cancelled.loadRelaxed()) {
        m_errorString = reader.errorString();
    }

    emit finished();
}

// --- CoverImageProvider ---

CoverImageProvider::CoverImageProvider(int maxThreads)
    : QQuickAsyncImageProvider() {
    m_pool.setMaxThreadCount(maxThreads);
}

QQuickImageResponse *CoverImageProvider::requestImageResponse(
    const QString &id, const QSize &requestedSize) {
    auto *response = new CoverImageResponse(id, requestedSize);
    m_pool.start(response);
    return response;
}

QString CoverImageProvider::urlForPath(const QString &filePath) {
    if (filePath.isEmpty()) {
        return {};
    }
    return QStringLiteral("image://covers/") + filePath;
}
