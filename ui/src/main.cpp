#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QStandardPaths>
#include <qqml.h>
#include <QQuickStyle>

#include <array>
#include <mutex>
#include <thread>
#include <vector>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

#include "BridgeClient.h"
#include "LibraryTreeModel.h"
#include "SpectrogramItem.h"
#include "WaveformItem.h"

namespace {

QString diagnosticsLogPath() {
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
        if (m_logPath.isEmpty()) {
            return;
        }
        QString line = QString::fromUtf8(lineBytes).trimmed();
        if (line.isEmpty()) {
            return;
        }

        const QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
        const QString fullLine = QStringLiteral("[%1] [%2] %3").arg(timestamp, tag, line);

        std::lock_guard<std::mutex> lock(m_logMutex);
        QFile file(m_logPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            return;
        }
        file.write(fullLine.toUtf8());
        file.write("\n", 1);
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
                const ssize_t readBytes = ::read(readFd, buffer.data(), buffer.size());
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
                while (true) {
                    const int newline = pending.indexOf('\n');
                    if (newline < 0) {
                        break;
                    }
                    const QByteArray line = pending.left(newline);
                    pending.remove(0, newline + 1);
                    appendLine(tag, line);
                }
            }

            if (!pending.isEmpty()) {
                appendLine(tag, pending);
            }
        });
        m_streams.push_back(std::move(stream));
    }

    void shutdown() {
        for (auto &stream : m_streams) {
            if (stream.targetFd >= 0 && stream.savedFd >= 0) {
                ::dup2(stream.savedFd, stream.targetFd);
            }
            if (stream.readFd >= 0) {
                ::close(stream.readFd);
                stream.readFd = -1;
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
        }
        m_streams.clear();
    }

    std::vector<StreamPipe> m_streams;
#else
    void shutdown() {}
#endif

    QString m_logPath;
    std::mutex m_logMutex;
};

} // namespace

int main(int argc, char *argv[]) {
    QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    QApplication app(argc, argv);
    ConsoleTeeManager consoleTee(diagnosticsLogPath());
    BridgeClient bridge;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    qmlRegisterType<SpectrogramItem>("FerrousNative", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousNative", 1, 0, "WaveformItem");
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(1); },
        Qt::QueuedConnection);

    engine.loadFromModule("FerrousNative", "Main");
    return app.exec();
}
