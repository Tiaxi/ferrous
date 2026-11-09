// SPDX-License-Identifier: GPL-3.0-or-later

#include "DiagnosticsLog.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <utility>

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

// Paths that name no file of the user's: the system's, D-Bus objects, and
// GStreamer elements.
constexpr std::array<QLatin1StringView, 7> kSystemPrefixes{
    QLatin1StringView("/usr/"), QLatin1StringView("/etc/"), QLatin1StringView("/proc/"),
    QLatin1StringView("/sys/"), QLatin1StringView("/dev/"), QLatin1StringView("/org/"),
    QLatin1StringView("/Gst"),
};

// The extensions a file name in the log may have that name the user's
// files: audio, playlists and pictures.
constexpr std::array<QLatin1StringView, 26> kFileExtensions{
    QLatin1StringView("flac"), QLatin1StringView("mp3"),  QLatin1StringView("ogg"),
    QLatin1StringView("oga"),  QLatin1StringView("opus"), QLatin1StringView("m4a"),
    QLatin1StringView("m4b"),  QLatin1StringView("mp4"),  QLatin1StringView("aac"),
    QLatin1StringView("wav"),  QLatin1StringView("aif"),  QLatin1StringView("aiff"),
    QLatin1StringView("wv"),   QLatin1StringView("ape"),  QLatin1StringView("mpc"),
    QLatin1StringView("dsf"),  QLatin1StringView("dff"),  QLatin1StringView("wma"),
    QLatin1StringView("cue"),  QLatin1StringView("m3u"),  QLatin1StringView("m3u8"),
    QLatin1StringView("pls"),  QLatin1StringView("jpg"),  QLatin1StringView("jpeg"),
    QLatin1StringView("png"),  QLatin1StringView("webp"),
};

QString withoutTrailingSlash(QString folder) {
    while (folder.size() > 1 && folder.endsWith(QLatin1Char('/'))) {
        folder.chop(1);
    }
    return folder;
}

bool isBelowOrAt(QStringView path, QStringView folder) {
    return !folder.isEmpty() && path.startsWith(folder)
        && (path.size() == folder.size() || path.at(folder.size()) == QLatin1Char('/'));
}

// ".flac" for a path whose last name ends so, or nothing.
QString extensionOf(QStringView path) {
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    const QStringView name = path.mid(slash + 1);
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || name.size() - dot - 1 < 1 || name.size() - dot - 1 > 5) {
        return {};
    }
    const QStringView suffix = name.mid(dot + 1);
    for (const QChar c : suffix) {
        if (!c.isLetterOrNumber()) {
            return {};
        }
    }
    return QLatin1Char('.') + suffix.toString();
}

bool hasFileExtension(QStringView name) {
    const QString extension = extensionOf(name);
    if (extension.isEmpty()) {
        return false;
    }
    const QStringView suffix = QStringView(extension).mid(1);
    return std::any_of(kFileExtensions.begin(), kFileExtensions.end(), [suffix](QLatin1StringView known) {
        return suffix.compare(known, Qt::CaseInsensitive) == 0;
    });
}

bool opensPath(QChar c) {
    return c.isSpace() || QStringView(u"\"'([<=,").contains(c);
}

// Where a quote that opened before `from` closes: at a quote followed by
// the end of the line, a space or punctuation, which a path seldom has.
qsizetype closingQuote(QStringView line, qsizetype from) {
    for (qsizetype j = line.indexOf(QLatin1Char('"'), from); j >= 0;
         j = line.indexOf(QLatin1Char('"'), j + 1)) {
        if (j + 1 == line.size() || QStringView(u" .,:;)]").contains(line.at(j + 1))) {
            return j;
        }
    }
    return -1;
}

// Where the path that starts at `start` ends, and whether the quote or
// bracket that opened before it says so.
std::pair<qsizetype, bool> pathEnd(QStringView line, qsizetype start) {
    const QChar opener = start > 0 ? line.at(start - 1) : QChar();
    qsizetype end = -1;
    if (opener == QLatin1Char('"')) {
        end = closingQuote(line, start);
    } else {
        // A bracket or a single quote may also be in the path, so the last
        // one closes it; a path ends there or before.
        QChar closer;
        if (opener == QLatin1Char('\'')) {
            closer = QLatin1Char('\'');
        } else if (opener == QLatin1Char('(')) {
            closer = QLatin1Char(')');
        } else if (opener == QLatin1Char('[')) {
            closer = QLatin1Char(']');
        } else if (opener == QLatin1Char('<')) {
            closer = QLatin1Char('>');
        }
        if (!closer.isNull()) {
            end = line.lastIndexOf(closer);
        }
    }
    if (end > start) {
        return {end, true};
    }
    return {line.size(), false};
}

struct Classified {
    QString replacement;
    qsizetype consumed;
};

class LineRedactor {
public:
    explicit LineRedactor(const PathRedaction &redaction) {
        for (const QString &root : redaction.libraryRoots) {
            m_roots.push_back(withoutTrailingSlash(root.trimmed()));
        }
        m_home = withoutTrailingSlash(redaction.homePath.trimmed());
        for (const QString &folder : redaction.keptFolders) {
            if (!folder.trimmed().isEmpty()) {
                m_kept.push_back(withoutTrailingSlash(folder.trimmed()));
            }
        }
    }

    QString redact(QStringView line) const {
        QString out;
        out.reserve(line.size());
        qsizetype i = 0;
        while (i < line.size()) {
            if (startsPath(line, i)) {
                const Classified path = classify(line, i);
                out += path.replacement;
                i += path.consumed;
                continue;
            }
            if (line.at(i) == QLatin1Char('"')) {
                const qsizetype close = closingQuote(line, i + 1);
                const QStringView quoted = close > i ? line.mid(i + 1, close - i - 1) : QStringView();
                if (!quoted.isEmpty() && !quoted.contains(QLatin1Char('/')) && hasFileExtension(quoted)) {
                    out += QStringLiteral("\"<file>") + extensionOf(quoted);
                    i = close;
                    continue;
                }
            }
            out += line.at(i);
            ++i;
        }
        return out;
    }

private:
    static bool startsPath(QStringView line, qsizetype i) {
        if (i > 0 && !opensPath(line.at(i - 1))) {
            return false;
        }
        const QStringView rest = line.mid(i);
        if (rest.startsWith(QLatin1StringView("file://"))) {
            return true;
        }
        // A path is "/" or "~/" and a name; "//" starts a URL's host.
        if (rest.startsWith(QLatin1StringView("~/"))) {
            return true;
        }
        return rest.size() > 1 && rest.at(0) == QLatin1Char('/') && rest.at(1) != QLatin1Char('/')
            && !rest.at(1).isSpace();
    }

    bool kept(QStringView path) const {
        if (std::any_of(kSystemPrefixes.begin(), kSystemPrefixes.end(),
                        [path](QLatin1StringView prefix) { return path.startsWith(prefix); })) {
            return true;
        }
        return std::any_of(m_kept.begin(), m_kept.end(),
                           [path](const QString &folder) { return isBelowOrAt(path, folder); });
    }

    QString homeShortened(QStringView path) const {
        if (isBelowOrAt(path, m_home)) {
            return QLatin1Char('~') + path.mid(m_home.size()).toString();
        }
        return path.toString();
    }

    Classified classify(QStringView line, qsizetype start) const {
        const auto [end, delimited] = pathEnd(line, start);
        const QStringView path = line.mid(start, end - start);
        const QString extension = delimited ? extensionOf(path) : QString();
        if (path.startsWith(QLatin1StringView("file://"))) {
            // A URI has no spaces; its extension is its path's.
            qsizetype uriEnd = 0;
            while (uriEnd < path.size() && !path.at(uriEnd).isSpace()) {
                ++uriEnd;
            }
            const QStringView uri = path.first(delimited ? path.size() : uriEnd);
            return {QStringLiteral("<path>") + extensionOf(uri), uri.size()};
        }
        for (qsizetype n = 0; n < m_roots.size(); ++n) {
            const QString &root = m_roots.at(n);
            if (root.isEmpty() || !path.startsWith(root)) {
                continue;
            }
            const QString placeholder = QStringLiteral("<library %1>").arg(n + 1);
            if (path.size() > root.size() && path.at(root.size()) == QLatin1Char('/')) {
                return {placeholder + QStringLiteral("/\u2026") + extension, path.size()};
            }
            const bool rootAlone = path.size() == root.size()
                || !(path.at(root.size()).isLetterOrNumber()
                     || QStringView(u"_-").contains(path.at(root.size())));
            if (rootAlone) {
                return {placeholder, root.size()};
            }
        }
        if (kept(path) || kept(QStringView(homeShortened(path)))) {
            // Kept as it is up to the end of its word, unless quoted, and the
            // rest of the line is read on.
            qsizetype keptEnd = path.size();
            if (!delimited) {
                keptEnd = 0;
                while (keptEnd < path.size() && !path.at(keptEnd).isSpace()) {
                    ++keptEnd;
                }
            }
            return {homeShortened(path.first(keptEnd)), keptEnd};
        }
        return {QStringLiteral("<path>") + extension, path.size()};
    }

    QStringList m_roots;
    QString m_home;
    QStringList m_kept;
};

} // namespace

QString defaultLogPath() {
    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (baseDir.isEmpty()) {
        baseDir = QDir::tempPath();
    } else {
        baseDir = QDir(baseDir).filePath(QStringLiteral("ferrous"));
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
    if (line.isEmpty()) {
        return false;
    }
    return appendLines(logPath, QStringList{line});
}

bool appendLines(const QString &logPath, const QStringList &lines) {
    if (logPath.trimmed().isEmpty() || lines.isEmpty()) {
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
    for (const QString &line : lines) {
        if (line.isEmpty()) {
            continue;
        }
        file.write(line.toUtf8());
        file.write("\n", 1);
    }
    return true;
}

QString withoutPaths(const QString &text, const PathRedaction &redaction) {
    const LineRedactor redactor(redaction);
    QString out;
    out.reserve(text.size());
    bool first = true;
    for (const QStringView line : QStringView(text).split(QLatin1Char('\n'))) {
        if (!first) {
            out += QLatin1Char('\n');
        }
        first = false;
        out += redactor.redact(line);
    }
    return out;
}

} // namespace DiagnosticsLog
