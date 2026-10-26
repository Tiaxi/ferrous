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

    // True once finished() has been emitted, so callers that connect late can
    // still observe completion.
    bool isFinished() const;

private:
    QString m_filePath;
    QSize m_requestedSize;
    QImage m_image;
    QString m_errorString;
    QAtomicInt m_cancelled;
    QAtomicInt m_finished;

    void finish();
};

class CoverImageProvider : public QQuickAsyncImageProvider {
public:
    explicit CoverImageProvider(int maxThreads = 4);

    QQuickImageResponse *requestImageResponse(
        const QString &id, const QSize &requestedSize) override;

    // The provider URL of the image at filePath. A nonzero stamp, which the
    // backend changes when the image is replaced at the same path, goes into
    // the URL as a "#v=<stamp>" suffix: Qt caches images by URL, so a new
    // stamp loads the new image.
    static QString urlForPath(const QString &filePath, quint64 stamp = 0);

    // The file path of a provider image id: the URL after "image://covers/",
    // which Qt passes with its fragment, so without the stamp suffix.
    static QString pathForId(const QString &id);
    // The stamp of a provider image id; zero if it has none.
    static quint64 stampForId(const QString &id);

    // Blocks until every started response has finished.
    void waitForDone();

private:
    QThreadPool m_pool;
};
