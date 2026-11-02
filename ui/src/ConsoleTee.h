// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

#include <thread>
#include <vector>

// Copies what the process writes to its console streams to the diagnostics
// log, line by line, and on to where the streams went before.
//
// Each stream's thread sleeps until there is output or shutdown, so an idle
// app has no wakeups for it. Shutdown copies everything written before it,
// in order, and puts the streams back.
class ConsoleTee {
public:
    explicit ConsoleTee(QString logPath);
    ~ConsoleTee();

    ConsoleTee(const ConsoleTee &) = delete;
    ConsoleTee &operator=(const ConsoleTee &) = delete;

    // Copies what is written to `fd`, such as STDOUT_FILENO, to the log with
    // `tag`. Fails, leaving `fd` as it was, when it cannot be redirected.
    bool install(int fd, const QString &tag);
    // Copies what is still in the streams and puts them back. Idempotent.
    void shutdown();

private:
    struct Stream {
        int targetFd{-1};
        int savedFd{-1};
        int readFd{-1};
        std::thread worker;
    };

    void forward(int readFd, int savedFd, const QString &tag) const;
    void appendLines(const QString &tag, const QList<QByteArray> &lineBytesList) const;

    QString m_logPath;
    std::vector<Stream> m_streams;
    // Readable once shutdown starts; never read, so every thread sees it.
    int m_shutdownReadFd{-1};
    int m_shutdownWriteFd{-1};
    bool m_shutDown{false};
};
