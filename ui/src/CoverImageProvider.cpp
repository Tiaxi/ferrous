// SPDX-License-Identifier: GPL-3.0-or-later

#include "CoverImageProvider.h"

#include <QImageReader>
#include <QQuickTextureFactory>

#include <algorithm>

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

bool CoverImageResponse::isFinished() const {
    return m_finished.loadAcquire() != 0;
}

void CoverImageResponse::finish() {
    m_finished.storeRelease(1);
    emit finished();
}

void CoverImageResponse::run() {
    if (m_cancelled.loadRelaxed()) {
        finish();
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

    finish();
}

// --- CoverImageProvider ---

CoverImageProvider::CoverImageProvider(int maxThreads)
    : QQuickAsyncImageProvider() {
    m_pool.setMaxThreadCount(maxThreads);
}

QQuickImageResponse *CoverImageProvider::requestImageResponse(
    const QString &id, const QSize &requestedSize) {
    auto *response = new CoverImageResponse(pathForId(id), requestedSize);
    m_pool.start(response);
    return response;
}

void CoverImageProvider::waitForDone() {
    m_pool.waitForDone();
}

namespace {
constexpr QLatin1StringView kStampMarker("#v=");
}

QString CoverImageProvider::urlForPath(const QString &filePath, quint64 stamp) {
    if (filePath.isEmpty()) {
        return {};
    }
    QString url = QStringLiteral("image://covers/") + filePath;
    if (stamp != 0) {
        url += kStampMarker + QString::number(stamp);
    }
    return url;
}

namespace {
// Where the stamp suffix of a provider image id starts, or -1. A cover file
// name ends in an image extension, so a trailing "#v=<digits>" is always the
// stamp, even in a path that has '#' elsewhere.
qsizetype stampMarkerIndex(const QString &id) {
    const qsizetype marker = id.lastIndexOf(kStampMarker);
    if (marker < 0) {
        return -1;
    }
    const QStringView digits = QStringView(id).mid(marker + kStampMarker.size());
    const bool allDigits = !digits.isEmpty()
        && std::all_of(digits.begin(), digits.end(), [](QChar c) {
               return c >= u'0' && c <= u'9';
           });
    return allDigits ? marker : -1;
}
} // namespace

QString CoverImageProvider::pathForId(const QString &id) {
    const qsizetype marker = stampMarkerIndex(id);
    return marker < 0 ? id : id.left(marker);
}

quint64 CoverImageProvider::stampForId(const QString &id) {
    const qsizetype marker = stampMarkerIndex(id);
    return marker < 0 ? 0 : id.mid(marker + kStampMarker.size()).toULongLong();
}
