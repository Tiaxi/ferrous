// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace DiagnosticsLog {

QString defaultLogPath();
bool appendLine(const QString &logPath, const QString &line);
bool appendLines(const QString &logPath, const QStringList &lines);
// The last `maxLines` lines of the log at `logPath`; none if it cannot be
// read. Reads from the disk: call it off the GUI thread.
QStringList readLastLines(const QString &logPath, qsizetype maxLines);

// `path` in double quotes, with its quotes and backslashes escaped, as log
// lines quote a path, in C++ and in Rust (`quoted_path`), so that
// withoutPaths can tell where it ends.
QString quotedPath(const QString &path);

// Writes lines to the log on a thread of its own, in the order they were
// posted, so that the thread that logs never waits for the disk: the GUI
// thread logs through it. The destructor writes what is still waiting.
class Writer {
public:
    using Append = std::function<bool(const QString &logPath, const QStringList &lines)>;

    explicit Writer(Append append = appendLines);
    ~Writer();

    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    // Queues `lines` for the log at `logPath` and returns at once.
    void post(const QString &logPath, QStringList lines);
    // Queues deleting the log at `logPath`, after the lines posted before.
    void removeLog(const QString &logPath);
    // Runs `task` on the writer's thread once everything posted before is
    // written, as reading the log needs.
    void then(std::function<void()> task);
    // Waits until everything posted before has been written.
    void waitUntilWritten();

private:
    struct Job {
        QString logPath;
        QStringList lines;
        bool remove{false};
        std::function<void()> task;
    };

    void run();

    const Append m_append;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_written;
    std::deque<Job> m_jobs;
    quint64 m_posted{0};
    quint64 m_done{0};
    bool m_stopping{false};
    std::thread m_thread;
};

// What withoutPaths needs to know to tell paths apart.
struct PathRedaction {
    // The library roots, numbered in this order as <library 1>, <library 2>.
    QStringList libraryRoots;
    // The user's home folder, which is shown as ~.
    QString homePath;
    // Folders whose paths are kept as they are, such as Ferrous's own
    // config, data and cache folders: they tell nothing about the library.
    QStringList keptFolders;
};

// `text`, lines of the log, with paths and file names replaced by
// placeholders for sharing in a bug report: a path in a library root by
// "<library N>/…", keeping its extension, any other path by "<path>", and a
// quoted file name by "<file>". A path ends at the quote or bracket that
// opened before it, as the log's lines quote paths, or else at the end of
// the line, so that a path with spaces is never cut short.
QString withoutPaths(const QString &text, const PathRedaction &redaction);

} // namespace DiagnosticsLog
