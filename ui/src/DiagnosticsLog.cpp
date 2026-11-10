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
#include <QTextStream>

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
// files: audio, playlists and pictures. It must include every audio
// extension the library accepts (`is_supported_audio` in
// src/library/scan.rs).
constexpr std::array<QLatin1StringView, 28> kFileExtensions{
    QLatin1StringView("flac"), QLatin1StringView("mp3"),  QLatin1StringView("ogg"),
    QLatin1StringView("oga"),  QLatin1StringView("opus"), QLatin1StringView("m4a"),
    QLatin1StringView("m4b"),  QLatin1StringView("mp4"),  QLatin1StringView("aac"),
    QLatin1StringView("wav"),  QLatin1StringView("aif"),  QLatin1StringView("aiff"),
    QLatin1StringView("wv"),   QLatin1StringView("ape"),  QLatin1StringView("mpc"),
    QLatin1StringView("dsf"),  QLatin1StringView("dff"),  QLatin1StringView("wma"),
    QLatin1StringView("ac3"),  QLatin1StringView("dts"),
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

// The curly quotes that user-visible messages, such as playback errors,
// quote a file or path in. Nothing escapes them in a name.
constexpr QChar kOpenCurlyQuote{0x201c};
constexpr QChar kCloseCurlyQuote{0x201d};

bool opensPath(QChar c) {
    return c.isSpace() || QStringView(u"\"'([<=,\u201c").contains(c);
}

// The first quote at or after `from` that no backslash escapes, or -1. The
// log quotes a path as quotedPath does, and Rust's `{:?}` the same way.
qsizetype closingQuote(QStringView line, qsizetype from) {
    for (qsizetype j = from; j < line.size(); ++j) {
        if (line.at(j) == QLatin1Char('\\')) {
            ++j;
        } else if (line.at(j) == QLatin1Char('"')) {
            return j;
        }
    }
    return -1;
}

// The quotes from `from` on that no backslash escapes.
qsizetype unescapedQuotes(QStringView line, qsizetype from) {
    qsizetype count = 0;
    for (qsizetype j = from; j < line.size(); ++j) {
        if (line.at(j) == QLatin1Char('\\')) {
            ++j;
        } else if (line.at(j) == QLatin1Char('"')) {
            ++count;
        }
    }
    return count;
}

// How surely the quote at `close` closes the quoted path before it.
enum class Close {
    // Followed by the end of the line, a space or punctuation, with no
    // other quote on the line that a backslash does not escape.
    Certain,
    // Followed so, but other quotes come after it: the path may go on past
    // it, as one an older build logged without escaping its quotes does.
    Ambiguous,
    // Followed by more text: the path goes on.
    Not,
};

Close closeOf(QStringView line, qsizetype close) {
    const bool followedByBreak =
        close + 1 == line.size() || QStringView(u" .,:;)]}").contains(line.at(close + 1));
    if (!followedByBreak) {
        return Close::Not;
    }
    return unescapedQuotes(line, close + 1) == 0 ? Close::Certain : Close::Ambiguous;
}

// `text` with its backslash escapes undone.
QString unescaped(QStringView text) {
    QString out;
    out.reserve(text.size());
    for (qsizetype j = 0; j < text.size(); ++j) {
        if (text.at(j) == QLatin1Char('\\') && j + 1 < text.size()) {
            ++j;
        }
        out += text.at(j);
    }
    return out;
}

// Where the path that starts at `start` ends: the quote or bracket that
// opened before it, or the end of the line.
struct PathEnd {
    qsizetype end;
    // Whether a quote or bracket ends it.
    bool delimited;
    // A quote that may end it, when the line's quotes leave it open whether
    // it does; `end` is then the end of the line.
    qsizetype ambiguousClose{-1};
};

PathEnd pathEnd(QStringView line, qsizetype start) {
    const QChar opener = start > 0 ? line.at(start - 1) : QChar();
    qsizetype end = -1;
    if (opener == QLatin1Char('"')) {
        end = closingQuote(line, start);
        if (end >= 0) {
            switch (closeOf(line, end)) {
            case Close::Certain:
                break;
            case Close::Ambiguous:
                return {line.size(), false, end};
            case Close::Not:
                end = -1;
                break;
            }
        }
    } else {
        // A bracket, a single quote or a curly quote may also be in the
        // path, so the last one closes it; a path ends there or before.
        QChar closer;
        if (opener == QLatin1Char('\'')) {
            closer = QLatin1Char('\'');
        } else if (opener == kOpenCurlyQuote) {
            closer = kCloseCurlyQuote;
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
                const QString quoted = close > i ? unescaped(line.mid(i + 1, close - i - 1)) : QString();
                if (!quoted.isEmpty() && !quoted.contains(QLatin1Char('/')) && hasFileExtension(quoted)) {
                    out += QStringLiteral("\"<file>") + extensionOf(quoted);
                    i = close;
                    continue;
                }
            }
            if (line.at(i) == kOpenCurlyQuote) {
                const qsizetype close = curlyFileNameClose(line, i);
                if (close > i) {
                    out += kOpenCurlyQuote + QStringLiteral("<file>") + extensionOf(line.mid(i + 1, close - i - 1));
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
    // The farthest closing curly quote after the one at `open` that ends a
    // file name with a known extension, or -1. A name may hold curly quotes
    // of its own, so the farthest is taken; a path is left to classify.
    static qsizetype curlyFileNameClose(QStringView line, qsizetype open) {
        for (qsizetype close = line.lastIndexOf(kCloseCurlyQuote); close > open;
             close = line.lastIndexOf(kCloseCurlyQuote, close - 1)) {
            const QStringView quoted = line.mid(open + 1, close - open - 1);
            if (!quoted.isEmpty() && !startsPath(quoted, 0) && hasFileExtension(quoted)) {
                return close;
            }
        }
        return -1;
    }

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
        auto [end, delimited, ambiguousClose] = pathEnd(line, start);
        if (ambiguousClose >= 0) {
            // Where the path ends is not certain, so it is redacted to the
            // end of the line, unless the path up to the quote is one of
            // Ferrous's own folders or the system's, which name nothing of
            // the user's: the backend's start line quotes several.
            const QString beforeQuote = unescaped(line.mid(start, ambiguousClose - start));
            if (kept(beforeQuote) || kept(QStringView(homeShortened(beforeQuote)))) {
                end = ambiguousClose;
                delimited = true;
            }
        }
        const QStringView path = line.mid(start, end - start);
        // A path in double quotes has its quotes and backslashes escaped.
        const bool escaped = start > 0 && line.at(start - 1) == QLatin1Char('"') && delimited;
        const QString candidate = escaped ? unescaped(path) : path.toString();
        const QString extension = delimited ? extensionOf(candidate) : QString();
        if (path.startsWith(QLatin1StringView("file://"))) {
            // A URI has no spaces; its extension is its path's.
            qsizetype uriEnd = 0;
            while (uriEnd < path.size() && !path.at(uriEnd).isSpace()) {
                ++uriEnd;
            }
            const QStringView uri = path.first(delimited ? path.size() : uriEnd);
            return {QStringLiteral("<path>") + extensionOf(uri), uri.size()};
        }
        // A root is the whole path or a folder of it: "/srv/music.old" and
        // "/srv/music (backup)" are other folders, redacted as any other.
        for (qsizetype n = 0; n < m_roots.size(); ++n) {
            const QString &root = m_roots.at(n);
            if (!isBelowOrAt(candidate, root)) {
                continue;
            }
            const QString placeholder = QStringLiteral("<library %1>").arg(n + 1);
            if (candidate.size() == root.size()) {
                return {placeholder, path.size()};
            }
            return {placeholder + QStringLiteral("/\u2026") + extension, path.size()};
        }
        if (kept(candidate) || kept(QStringView(homeShortened(candidate)))) {
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

QStringList readLastLines(const QString &logPath, qsizetype maxLines) {
    QStringList lines;
    if (logPath.isEmpty()) {
        return lines;
    }
    // Not halfway through another thread's write.
    const std::lock_guard<std::mutex> lock(logMutex());
    QFile file(logPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return lines;
    }
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (!line.isNull()) {
            lines.push_back(line);
        }
    }
    if (lines.size() > maxLines) {
        lines = lines.mid(lines.size() - maxLines);
    }
    return lines;
}

QString quotedPath(const QString &path) {
    QString out;
    out.reserve(path.size() + 2);
    out += QLatin1Char('"');
    for (const QChar c : path) {
        if (c == QLatin1Char('"') || c == QLatin1Char('\\')) {
            out += QLatin1Char('\\');
        }
        out += c;
    }
    out += QLatin1Char('"');
    return out;
}

Writer::Writer(Append append)
    : m_append(std::move(append)),
      m_thread([this]() { run(); }) {}

Writer::~Writer() {
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_one();
    m_thread.join();
}

void Writer::post(const QString &logPath, QStringList lines) {
    if (logPath.isEmpty() || lines.isEmpty()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.push_back(Job{logPath, std::move(lines), false, {}});
        ++m_posted;
    }
    m_wake.notify_one();
}

void Writer::removeLog(const QString &logPath) {
    if (logPath.isEmpty()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.push_back(Job{logPath, {}, true, {}});
        ++m_posted;
    }
    m_wake.notify_one();
}

void Writer::then(std::function<void()> task) {
    if (!task) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.push_back(Job{{}, {}, false, std::move(task)});
        ++m_posted;
    }
    m_wake.notify_one();
}

void Writer::waitUntilWritten() {
    std::unique_lock<std::mutex> lock(m_mutex);
    const quint64 target = m_posted;
    m_written.wait(lock, [this, target]() { return m_done >= target; });
}

void Writer::run() {
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_wake.wait(lock, [this]() { return m_stopping || !m_jobs.empty(); });
        if (m_jobs.empty()) {
            // Stopping, with everything written.
            return;
        }
        Job job = std::move(m_jobs.front());
        m_jobs.pop_front();
        quint64 jobs = 1;
        // Lines posted meanwhile go in the same write.
        while (!job.remove && !job.task && !m_jobs.empty() && !m_jobs.front().remove
               && !m_jobs.front().task && m_jobs.front().logPath == job.logPath) {
            job.lines += m_jobs.front().lines;
            m_jobs.pop_front();
            ++jobs;
        }
        lock.unlock();
        if (job.task) {
            job.task();
        } else if (job.remove) {
            QFile::remove(job.logPath);
        } else {
            (void)m_append(job.logPath, job.lines);
        }
        lock.lock();
        m_done += jobs;
        m_written.notify_all();
    }
}

} // namespace DiagnosticsLog
