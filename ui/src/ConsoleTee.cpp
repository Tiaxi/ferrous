// SPDX-License-Identifier: GPL-3.0-or-later

#include "ConsoleTee.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QStringList>

#include <array>
#include <cstdio>

#include "DiagnosticsLog.h"

#if defined(Q_OS_UNIX)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#endif

namespace {

#if defined(Q_OS_UNIX)
bool setFdFlag(int fd, int getCommand, int setCommand, int flag) {
    const int flags = ::fcntl(fd, getCommand, 0);
    return flags >= 0 && ::fcntl(fd, setCommand, flags | flag) >= 0;
}

void closeFd(int &fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

// Writes all of `size` bytes unless the target fails.
void writeAll(int fd, const char *data, ssize_t size) {
    ssize_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(fd, data + offset, static_cast<size_t>(size - offset));
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return;
        }
        offset += written;
    }
}
#endif

} // namespace

ConsoleTee::ConsoleTee(QString logPath)
    : m_logPath(std::move(logPath)) {
#if defined(Q_OS_UNIX)
    int fds[2] = {-1, -1};
    if (::pipe(fds) == 0) {
        m_shutdownReadFd = fds[0];
        m_shutdownWriteFd = fds[1];
        if (!setFdFlag(m_shutdownReadFd, F_GETFD, F_SETFD, FD_CLOEXEC)
            || !setFdFlag(m_shutdownWriteFd, F_GETFD, F_SETFD, FD_CLOEXEC)) {
            closeFd(m_shutdownReadFd);
            closeFd(m_shutdownWriteFd);
        }
    }
#endif
}

ConsoleTee::~ConsoleTee() {
    shutdown();
#if defined(Q_OS_UNIX)
    closeFd(m_shutdownReadFd);
    closeFd(m_shutdownWriteFd);
#endif
}

void ConsoleTee::appendLines(const QString &tag, const QList<QByteArray> &lineBytesList) const {
    QStringList lines;
    lines.reserve(lineBytesList.size());
    for (const QByteArray &lineBytes : lineBytesList) {
        const QString line = QString::fromUtf8(lineBytes).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
        lines.push_back(QStringLiteral("[%1] [%2] %3").arg(timestamp, tag, line));
    }
    if (lines.isEmpty()) {
        return;
    }
    // The console still has the output when the log cannot be written.
    (void)DiagnosticsLog::appendLines(m_logPath, lines);
}

#if defined(Q_OS_UNIX)

bool ConsoleTee::install(int fd, const QString &tag) {
    if (m_logPath.isEmpty() || m_shutDown || m_shutdownReadFd < 0) {
        return false;
    }

    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        return false;
    }
    int readFd = pipeFds[0];
    int writeFd = pipeFds[1];
    int savedFd = ::dup(fd);
    // The read end is this process's alone; the write end becomes `fd`,
    // which children inherit as before.
    if (savedFd < 0
        || !setFdFlag(readFd, F_GETFL, F_SETFL, O_NONBLOCK)
        || !setFdFlag(readFd, F_GETFD, F_SETFD, FD_CLOEXEC)
        || !setFdFlag(savedFd, F_GETFD, F_SETFD, FD_CLOEXEC)
        || ::dup2(writeFd, fd) < 0) {
        closeFd(readFd);
        closeFd(writeFd);
        closeFd(savedFd);
        return false;
    }
    closeFd(writeFd);

    Stream stream;
    stream.targetFd = fd;
    stream.savedFd = savedFd;
    stream.readFd = readFd;
    stream.worker = std::thread([this, readFd, savedFd, tag]() { forward(readFd, savedFd, tag); });
    m_streams.push_back(std::move(stream));
    return true;
}

void ConsoleTee::forward(int readFd, int savedFd, const QString &tag) const {
#if defined(Q_OS_LINUX)
    // Apart from the main thread, which has the process's name, in tools
    // that list threads.
    pthread_setname_np(pthread_self(), "ferrous-tee");
#endif
    std::array<char, 4096> buffer{};
    QByteArray pending;
    std::array<pollfd, 2> fds{};
    fds[0].fd = readFd;
    fds[0].events = POLLIN;
    fds[1].fd = m_shutdownReadFd;
    fds[1].events = POLLIN;
    // After shutdown starts, what is left in the pipe is read until it is
    // empty, without waiting for more.
    bool draining = false;

    while (true) {
        if (!draining) {
            // No timeout: the output or shutdown wakes the thread.
            if (::poll(fds.data(), fds.size(), -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if ((fds[0].revents & POLLNVAL) != 0) {
                break;
            }
            draining = (fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
            if (!draining && (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }
        }

        const ssize_t readBytes = ::read(readFd, buffer.data(), buffer.size());
        if (readBytes < 0 && errno == EINTR) {
            continue;
        }
        if (readBytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (draining) {
                break;
            }
            continue;
        }
        if (readBytes <= 0) {
            // Every writer has closed the stream.
            break;
        }

        writeAll(savedFd, buffer.data(), readBytes);

        pending.append(buffer.data(), static_cast<qsizetype>(readBytes));
        QList<QByteArray> completedLines;
        qsizetype lineStart = 0;
        while (true) {
            const qsizetype newline = pending.indexOf('\n', lineStart);
            if (newline < 0) {
                break;
            }
            completedLines.push_back(pending.mid(lineStart, newline - lineStart));
            lineStart = newline + 1;
        }
        pending.remove(0, lineStart);
        if (!completedLines.isEmpty()) {
            appendLines(tag, completedLines);
        }
    }

    if (!pending.isEmpty()) {
        appendLines(tag, QList<QByteArray>{pending});
    }
}

void ConsoleTee::shutdown() {
    if (m_shutDown) {
        return;
    }
    m_shutDown = true;
    if (m_streams.empty()) {
        return;
    }

    // What the C library still buffers goes through the pipes too.
    std::fflush(stdout);
    std::fflush(stderr);
    // Later writes go straight to the console. Everything written before is
    // in the pipes, which the threads read until they are empty.
    for (Stream &stream : m_streams) {
        ::dup2(stream.savedFd, stream.targetFd);
    }
    const char wake = 1;
    while (::write(m_shutdownWriteFd, &wake, 1) < 0 && errno == EINTR) {
    }
    for (Stream &stream : m_streams) {
        if (stream.worker.joinable()) {
            stream.worker.join();
        }
        closeFd(stream.savedFd);
        closeFd(stream.readFd);
    }
    m_streams.clear();
}

#else

bool ConsoleTee::install(int, const QString &) {
    return false;
}

void ConsoleTee::forward(int, int, const QString &) const {}

void ConsoleTee::shutdown() {
    m_shutDown = true;
}

#endif
