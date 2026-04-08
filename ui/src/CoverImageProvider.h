// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAtomicInt>
#include <QImage>
#include <QQuickAsyncImageProvider>
#include <QQuickImageResponse>
#include <QRunnable>
#include <QSize>
#include <QString>
#include <QThreadPool>

class CoverImageResponse : public QQuickImageResponse, public QRunnable {
    Q_OBJECT
public:
    CoverImageResponse(const QString &filePath, const QSize &requestedSize);

    QQuickTextureFactory *textureFactory() const override;
    QString errorString() const override;
    void cancel() override;
    void run() override;

private:
    QString m_filePath;
    QSize m_requestedSize;
    QImage m_image;
    QString m_errorString;
    QAtomicInt m_cancelled;
};

class CoverImageProvider : public QQuickAsyncImageProvider {
public:
    explicit CoverImageProvider(int maxThreads = 4);

    QQuickImageResponse *requestImageResponse(
        const QString &id, const QSize &requestedSize) override;

    static QString urlForPath(const QString &filePath);

private:
    QThreadPool m_pool;
};
