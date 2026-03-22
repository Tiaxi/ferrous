#include <QApplication>
#include <QDateTime>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>
#include <qqml.h>

#include <atomic>
#include <array>
#include <thread>
#include <vector>

#if defined(Q_OS_UNIX)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#include "BridgeClient.h"
#include "AppInstanceController.h"
#include "DiagnosticsLog.h"
#include "LibraryTreeModel.h"
#include "MprisController.h"
#include "SpectrogramItem.h"
#include "TagEditorController.h"
#include "WaveformItem.h"

namespace {

bool hasInstalledDesktopEntry(const QString &desktopFileName) {
    return !QStandardPaths::locate(
                QStandardPaths::ApplicationsLocation,
                desktopFileName,
                QStandardPaths::LocateFile)
                .isEmpty();
}

class ConsoleTeeManager {
public:
    explicit ConsoleTeeManager(QString logPath)
        : m_logPath(std::move(logPath)) {
#if defined(Q_OS_UNIX)
        installForFd(STDOUT_FILENO, QStringLiteral("stdout"));
        installForFd(STDERR_FILENO, QStringLiteral("stderr"));
#endif
    }

    ~ConsoleTeeManager() {
        shutdown();
    }

private:
#if defined(Q_OS_UNIX)
    struct StreamPipe {
        int targetFd{-1};
        int savedFd{-1};
        int readFd{-1};
        QString tag;
        std::thread worker;
    };
#endif

    void appendLine(const QString &tag, const QByteArray &lineBytes) {
        appendLines(tag, QList<QByteArray>{lineBytes});
    }

    void appendLines(const QString &tag, const QList<QByteArray> &lineBytesList) {
        if (m_logPath.isEmpty()) {
            return;
        }
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
        const bool written = DiagnosticsLog::appendLines(m_logPath, lines);
        (void)written;
    }

#if defined(Q_OS_UNIX)
    void installForFd(int fd, const QString &tag) {
        if (m_logPath.isEmpty()) {
            return;
        }

        int pipeFds[2] = {-1, -1};
        if (::pipe(pipeFds) != 0) {
            return;
        }

        const int savedFd = ::dup(fd);
        if (savedFd < 0) {
            ::close(pipeFds[0]);
            ::close(pipeFds[1]);
            return;
        }

        const int readFlags = ::fcntl(pipeFds[0], F_GETFL, 0);
        if (readFlags < 0 || ::fcntl(pipeFds[0], F_SETFL, readFlags | O_NONBLOCK) < 0) {
            ::close(pipeFds[0]);
            ::close(pipeFds[1]);
            ::close(savedFd);
            return;
        }

        if (::dup2(pipeFds[1], fd) < 0) {
            ::close(pipeFds[0]);
            ::close(pipeFds[1]);
            ::close(savedFd);
            return;
        }
        ::close(pipeFds[1]);

        StreamPipe stream;
        stream.targetFd = fd;
        stream.savedFd = savedFd;
        stream.readFd = pipeFds[0];
        stream.tag = tag;
        stream.worker = std::thread([this, readFd = stream.readFd, savedFd, tag]() {
            std::array<char, 4096> buffer{};
            QByteArray pending;

            while (true) {
                if (m_shutdownRequested.load(std::memory_order_relaxed)) {
                    break;
                }

                pollfd pfd{};
                pfd.fd = readFd;
                pfd.events = POLLIN | POLLERR | POLLHUP | POLLNVAL;
                const int pollResult = ::poll(&pfd, 1, 100);
                if (pollResult < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
                if (pollResult == 0) {
                    continue;
                }
                if ((pfd.revents & POLLNVAL) != 0) {
                    break;
                }
                if ((pfd.revents & (POLLIN | POLLERR | POLLHUP)) == 0) {
                    continue;
                }

                const ssize_t readBytes = ::read(readFd, buffer.data(), buffer.size());
                if (readBytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    continue;
                }
                if (readBytes <= 0) {
                    break;
                }

                ssize_t offset = 0;
                while (offset < readBytes) {
                    const ssize_t written = ::write(
                        savedFd,
                        buffer.data() + offset,
                        static_cast<size_t>(readBytes - offset));
                    if (written <= 0) {
                        break;
                    }
                    offset += written;
                }

                pending.append(buffer.data(), static_cast<int>(readBytes));
                QList<QByteArray> completedLines;
                while (true) {
                    const int newline = pending.indexOf('\n');
                    if (newline < 0) {
                        break;
                    }
                    completedLines.push_back(pending.left(newline));
                    pending.remove(0, newline + 1);
                }
                if (!completedLines.isEmpty()) {
                    appendLines(tag, completedLines);
                }
            }

            if (!pending.isEmpty()) {
                appendLine(tag, pending);
            }
        });
        m_streams.push_back(std::move(stream));
    }

    void shutdown() {
        if (m_shutdownRequested.exchange(true, std::memory_order_relaxed)) {
            return;
        }

        for (auto &stream : m_streams) {
            if (stream.targetFd >= 0 && stream.savedFd >= 0) {
                ::dup2(stream.savedFd, stream.targetFd);
            }
        }

        for (auto &stream : m_streams) {
            if (stream.savedFd >= 0) {
                ::close(stream.savedFd);
                stream.savedFd = -1;
            }
            if (stream.worker.joinable()) {
                stream.worker.join();
            }
            if (stream.readFd >= 0) {
                ::close(stream.readFd);
                stream.readFd = -1;
            }
        }
        m_streams.clear();
    }

    std::vector<StreamPipe> m_streams;
#else
    void shutdown() {}
#endif

    QString m_logPath;
    std::atomic_bool m_shutdownRequested{false};
};

} // namespace

int main(int argc, char *argv[]) {
    QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Ferrous"));
    QApplication::setApplicationDisplayName(QStringLiteral("Ferrous"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/assets/ferrous.svg")));
    if (hasInstalledDesktopEntry(QStringLiteral("ferrous.desktop"))) {
        QApplication::setDesktopFileName(QStringLiteral("ferrous"));
    }
    ConsoleTeeManager consoleTee(DiagnosticsLog::defaultLogPath());
    AppInstanceController instanceController;
    QString instanceError;
    switch (instanceController.initialize(QCoreApplication::arguments().mid(1), &instanceError)) {
    case AppInstanceController::StartupResult::ExitAfterForward:
        return 0;
    case AppInstanceController::StartupResult::ExitWithError:
        if (!instanceError.trimmed().isEmpty()) {
            std::fprintf(stderr, "%s\n", qPrintable(instanceError));
        }
        return 1;
    case AppInstanceController::StartupResult::ContinuePrimary:
        break;
    }
    BridgeClient bridge;
    instanceController.setOpenHandler([&bridge](const QStringList &paths) {
        bridge.replaceWithPaths(paths);
    });
    MprisController mpris(&bridge);
    LibraryTreeModel libraryModel;
    TagEditorController tagEditor(&bridge);
    QQmlApplicationEngine engine;
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("tagEditor"), &tagEditor);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(1); },
        Qt::QueuedConnection);

    engine.loadFromModule("FerrousUi", "Main");
    return app.exec();
}
