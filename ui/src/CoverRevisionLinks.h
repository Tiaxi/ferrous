// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>
#include <QThreadPool>

#include <deque>

// Paths that change with a cover's revision, for consumers that cache art by
// file path, such as GNOME Shell's MPRIS controls: Gio drops a URL's query,
// so cover.jpg?v=1 and cover.jpg?v=2 are the same file to it.
//
// Each revision gets a symbolic link named by the cover path and its stamp,
// in a folder of Ferrous's cache. The link leaves the cover file untouched,
// and readers follow it to the current file. Links are made on a worker
// thread, created under a temporary name and renamed into place. Only links
// named the way this class names them are removed: the few made last stay,
// so that a consumer still reading the previous revision finds it, and the
// link last made known stays until a newer one is.
class CoverRevisionLinks : public QObject {
    Q_OBJECT

public:
    explicit CoverRevisionLinks(QString directory, QObject *parent = nullptr);
    ~CoverRevisionLinks() override;

    // $XDG_CACHE_HOME/ferrous/mpris_art.
    static QString defaultDirectory();

    // How many links stay in the folder.
    static constexpr int kKeptLinks = 4;

    // The link for coverPath at stamp once it exists. Until then, or if it
    // could not be made, returns an empty string; the first call for a
    // revision asks the worker to make it, and linkReady() follows.
    QString linkFor(const QString &coverPath, quint64 stamp);
    // Whether the link last asked for is still being made.
    bool pending() const;

    // Blocks until the worker has finished; for tests and shutdown.
    void waitForDone();

signals:
    // The link last asked for exists now, or could not be made; linkFor()
    // tells which.
    void linkReady();
    // Why the link last asked for could not be made.
    void linkFailed(const QString &message);

private:
    QString m_directory;
    QThreadPool m_pool;
    // The links made, newest last. The pool runs one job at a time, and
    // only its jobs touch this.
    std::deque<QString> m_recentLinks;
    QString m_requestedKey;
    QString m_readyKey;
    QString m_readyLink;
    quint64 m_generation{0};
    bool m_pending{false};
};
