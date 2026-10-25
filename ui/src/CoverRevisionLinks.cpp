// SPDX-License-Identifier: GPL-3.0-or-later

#include "CoverRevisionLinks.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <utility>

namespace {

const QRegularExpression &linkNamePattern() {
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{20}-[0-9]+\\.[0-9a-z]{1,8}$"));
    return pattern;
}

const QRegularExpression &tempNamePattern() {
    static const QRegularExpression pattern(
        QStringLiteral("^\\.[0-9a-f]{20}-[0-9]+\\.[0-9a-z]{1,8}\\.tmp-[0-9a-f]{8}$"));
    return pattern;
}

// <sha1 of the path, 20 hex digits>-<stamp>.<extension>, the extension kept
// for consumers that go by it.
QString linkName(const QString &coverPath, quint64 stamp) {
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(coverPath.toUtf8(), QCryptographicHash::Sha1).toHex().left(20));
    QString suffix = QFileInfo(coverPath).suffix().toLower();
    static const QRegularExpression suffixPattern(QStringLiteral("^[0-9a-z]{1,8}$"));
    if (!suffixPattern.match(suffix).hasMatch()) {
        suffix = QStringLiteral("img");
    }
    return QStringLiteral("%1-%2.%3").arg(digest).arg(stamp).arg(suffix);
}

// Removes the links this class made but those in `kept`, and temporary
// links a stopped run left. Files that are not links, or that are named
// otherwise, are never removed.
void pruneLinks(const QString &directory, const std::deque<QString> &kept) {
    const QFileInfoList entries = QDir(directory).entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        if (!entry.isSymLink()) {
            continue;
        }
        const QString name = entry.fileName();
        const QString path = entry.filePath();
        // Links are made one at a time, so a temporary one is a leftover.
        const bool leftover = tempNamePattern().match(name).hasMatch();
        const bool old = linkNamePattern().match(name).hasMatch()
            && std::find(kept.cbegin(), kept.cend(), path) == kept.cend();
        if (leftover || old) {
            QFile::remove(path);
        }
    }
}

struct LinkResult {
    QString link;
    QString error;
};

// `recent` holds the links made, newest last; only the worker touches it.
LinkResult makeLink(
    const QString &directory, const QString &coverPath, quint64 stamp, std::deque<QString> &recent) {
    if (!QDir().mkpath(directory)) {
        return {{}, QStringLiteral("could not create '%1'").arg(directory)};
    }
    const QString name = linkName(coverPath, stamp);
    const QString target = QDir(directory).filePath(name);
    const QFileInfo existing(target);
    if (!(existing.isSymLink() && existing.symLinkTarget() == coverPath)) {
        const QString temporary = QDir(directory).filePath(
            QStringLiteral(".%1.tmp-%2")
                .arg(name)
                .arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0')));
        if (!QFile::link(coverPath, temporary)) {
            return {{}, QStringLiteral("could not create a link in '%1'").arg(directory)};
        }
        if (std::rename(QFile::encodeName(temporary).constData(),
                        QFile::encodeName(target).constData())
            != 0) {
            const QString reason = QString::fromLocal8Bit(std::strerror(errno));
            QFile::remove(temporary);
            return {{}, QStringLiteral("could not rename a link into '%1': %2").arg(target, reason)};
        }
    }
    recent.erase(std::remove(recent.begin(), recent.end(), target), recent.end());
    recent.push_back(target);
    while (recent.size() > static_cast<std::size_t>(CoverRevisionLinks::kKeptLinks)) {
        recent.pop_front();
    }
    pruneLinks(directory, recent);
    return {target, {}};
}

} // namespace

CoverRevisionLinks::CoverRevisionLinks(QString directory, QObject *parent)
    : QObject(parent)
    , m_directory(std::move(directory)) {
    m_pool.setMaxThreadCount(1);
}

CoverRevisionLinks::~CoverRevisionLinks() {
    m_pool.waitForDone();
}

QString CoverRevisionLinks::defaultDirectory() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (base.trimmed().isEmpty()) {
        return {};
    }
    return QDir(base).filePath(QStringLiteral("ferrous/mpris_art"));
}

QString CoverRevisionLinks::linkFor(const QString &coverPath, quint64 stamp) {
    if (m_directory.isEmpty() || coverPath.isEmpty() || stamp == 0) {
        return {};
    }
    const QString key = QString::number(stamp) + QLatin1Char('|') + coverPath;
    if (key == m_readyKey) {
        return m_readyLink;
    }
    if (key == m_requestedKey) {
        return {};
    }
    m_requestedKey = key;
    m_pending = true;
    const quint64 generation = ++m_generation;
    m_pool.start([this, directory = m_directory, coverPath, stamp, key, generation]() {
        const LinkResult result = makeLink(directory, coverPath, stamp, m_recentLinks);
        // The destructor waits for this job, so `this` is alive here, and
        // Qt drops the call if the object is gone by the time it runs.
        QMetaObject::invokeMethod(
            this,
            [this, key, generation, result]() {
                if (generation != m_generation) {
                    return;
                }
                m_pending = false;
                m_readyKey = key;
                m_readyLink = result.link;
                if (!result.error.isEmpty()) {
                    emit linkFailed(result.error);
                }
                emit linkReady();
            },
            Qt::QueuedConnection);
    });
    return {};
}

bool CoverRevisionLinks::pending() const {
    return m_pending;
}

void CoverRevisionLinks::waitForDone() {
    m_pool.waitForDone();
}
