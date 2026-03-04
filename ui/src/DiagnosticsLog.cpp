#include "DiagnosticsLog.h"

#include <algorithm>
#include <mutex>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace DiagnosticsLog {
namespace {

constexpr qint64 kDefaultMaxBytes = 10 * 1024 * 1024;
constexpr int kDefaultBackups = 3;

qint64 diagnosticsMaxBytes() {
    bool ok = false;
    const qint64 mb = qEnvironmentVariableIntValue("FERROUS_DIAGNOSTICS_LOG_MAX_MB", &ok);
    if (!ok) {
        return kDefaultMaxBytes;
    }
    return std::clamp(mb, 1LL, 1024LL) * 1024LL * 1024LL;
}

int diagnosticsBackups() {
    bool ok = false;
    const int backups = qEnvironmentVariableIntValue("FERROUS_DIAGNOSTICS_LOG_BACKUPS", &ok);
    if (!ok) {
        return kDefaultBackups;
    }
    return std::clamp(backups, 0, 16);
}

QString backupPath(const QString &basePath, int index) {
    return QStringLiteral("%1.%2").arg(basePath).arg(index);
}

void rotateIfNeeded(const QString &logPath) {
    const QFileInfo info(logPath);
    if (!info.exists()) {
        return;
    }
    const qint64 maxBytes = diagnosticsMaxBytes();
    if (maxBytes <= 0 || info.size() < maxBytes) {
        return;
    }

    const int backups = diagnosticsBackups();
    if (backups <= 0) {
        QFile::remove(logPath);
        return;
    }

    QFile::remove(backupPath(logPath, backups));
    for (int i = backups - 1; i >= 1; --i) {
        const QString from = backupPath(logPath, i);
        const QString to = backupPath(logPath, i + 1);
        if (QFile::exists(from)) {
            QFile::remove(to);
            QFile::rename(from, to);
        }
    }
    QFile::remove(backupPath(logPath, 1));
    QFile::rename(logPath, backupPath(logPath, 1));
}

std::mutex &logMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

QString defaultLogPath() {
    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (baseDir.isEmpty()) {
        baseDir = QDir::tempPath();
    }
    if (baseDir.isEmpty()) {
        return {};
    }

    QDir dir(baseDir);
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }
    return dir.filePath(QStringLiteral("diagnostics.log"));
}

bool appendLine(const QString &logPath, const QString &line) {
    if (logPath.trimmed().isEmpty() || line.isEmpty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(logMutex());

    const QFileInfo info(logPath);
    const QString dirPath = info.absolutePath();
    if (!dirPath.isEmpty()) {
        QDir dir(dirPath);
        if (!dir.exists()) {
            dir.mkpath(QStringLiteral("."));
        }
    }

    rotateIfNeeded(logPath);

    QFile file(logPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return false;
    }
    file.write(line.toUtf8());
    file.write("\n", 1);
    return true;
}

} // namespace DiagnosticsLog

