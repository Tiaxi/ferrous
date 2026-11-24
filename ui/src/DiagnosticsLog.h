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
// Appends `messages` with `tag`, stamped under the log's lock, so that
// threads that write to the log themselves write in the order of their
// stamps.
bool appendStampedLines(const QString &logPath, const QString &tag, const QStringList &messages);
// The last `maxLines` lines of the log at `logPath`; none if it cannot be
// read. Reads from the disk: call it off the GUI thread.
QStringList readLastLines(const QString &logPath, qsizetype maxLines);

// `path` in double quotes, with its quotes and backslashes escaped, as log
// lines quote a path, in C++ and in Rust (`quoted_path`), so that
// withoutPaths can tell where it ends.
QString quotedPath(const QString &path);

// `messages` as lines of the log, "[stamp] [tag] message", stamped now.
QStringList stampedLines(const QString &tag, const QStringList &messages);

// The lines logged on any thread on their way to the log, in the order of
// their stamps: a line is stamped as it is queued, under the queue's lock,
// so a line stamped earlier never comes after it. The app's BridgeClient
// takes them on the GUI thread, for the log and Diagnostics, and logs its
// own lines through it; ConsoleTee queues the console's on its threads.
// Lines wait until a consumer takes them, from the first.
class LineQueue {
public:
    // Asks the consumer to take what waits. Called, under the queue's lock,
    // on the thread that queued, so it must only post, as
    // QMetaObject::invokeMethod does with a queued connection.
    using Wake = std::function<void()>;
    using Rest = std::function<void(const QStringList &lines)>;

    // Queues `messages` with `tag`, stamped now, and wakes the consumer if
    // nothing waited. False, queuing nothing, once the queue is closed: the
    // caller writes them itself.
    bool post(const QString &tag, const QStringList &messages);
    // What waits, in order.
    QStringList take();
    // What waits, then `message` with `tag`, stamped now, as the consumer
    // logs a line of its own.
    QStringList takeWith(const QString &tag, const QString &message);
    // Wakes the consumer with `wake` from now on, and now if lines wait.
    void setWake(Wake wake);
    // Closes the queue, once, as the consumer stops: hands what still waits
    // to `rest` under the lock, so a line queued meanwhile waits for `rest`
    // to return and is then refused, and stops waking the consumer.
    void close(const Rest &rest);
    // Closes the queue as close does, unless a consumer has set a wake: it
    // takes the lines and closes the queue itself.
    void closeUnlessConsumed(const Rest &rest);

private:
    void closeLocked(const Rest &rest);

    std::mutex m_mutex;
    QStringList m_lines;
    Wake m_wake;
    bool m_closed{false};
};

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
