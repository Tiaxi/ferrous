// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <chrono>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include "ConsoleTee.h"
#include "TestSessionIsolation.h"

namespace {

QString readProcText(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromLatin1(file.readAll()) : QString();
}

// The tee's threads, by their task directories.
QStringList teeThreads() {
    QStringList threads;
    const QStringList tasks = QDir(QStringLiteral("/proc/self/task")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &task : tasks) {
        const QString base = QStringLiteral("/proc/self/task/%1/").arg(task);
        if (readProcText(base + QStringLiteral("comm")).trimmed() == QLatin1String("ferrous-tee")) {
            threads.push_back(base);
        }
    }
    return threads;
}

// Whether every tee thread sleeps in poll.
bool teeThreadsSleepInPoll(qsizetype expected) {
    const QStringList threads = teeThreads();
    if (threads.size() != expected) {
        return false;
    }
    for (const QString &base : threads) {
        const QString stat = readProcText(base + QStringLiteral("stat"));
        const QString state = stat.mid(stat.lastIndexOf(QLatin1Char(')')) + 2, 1);
        if (state != QLatin1String("S") || !readProcText(base + QStringLiteral("wchan")).contains(QLatin1String("poll"))) {
            return false;
        }
    }
    return true;
}

// The voluntary and involuntary context switches of the tee's threads.
qint64 teeContextSwitches() {
    qint64 total = 0;
    for (const QString &base : teeThreads()) {
        const QStringList lines = readProcText(base + QStringLiteral("status")).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (line.startsWith(QLatin1String("voluntary_ctxt_switches:"))
                || line.startsWith(QLatin1String("nonvoluntary_ctxt_switches:"))) {
                total += line.section(QLatin1Char(':'), 1).trimmed().toLongLong();
            }
        }
    }
    return total;
}

void writeText(int fd, const QByteArray &text) {
    qsizetype offset = 0;
    while (offset < text.size()) {
        const ssize_t written = ::write(fd, text.constData() + offset, static_cast<size_t>(text.size() - offset));
        QVERIFY(written > 0);
        offset += written;
    }
}

// The messages of the log's lines with `tag`, in order.
QStringList loggedMessages(const QString &logPath, const QString &tag) {
    QFile log(logPath);
    if (!log.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QRegularExpression pattern(
        QStringLiteral("^\\[[^\\]]+\\] \\[%1\\] (.*)$").arg(QRegularExpression::escape(tag)));
    QStringList messages;
    const QStringList lines = QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QRegularExpressionMatch match = pattern.match(line);
        if (match.hasMatch()) {
            messages.push_back(match.captured(1));
        }
    }
    return messages;
}

} // namespace

class ConsoleTeeTest : public QObject {
    Q_OBJECT

private:
    // A stream the tee redirects, as it does stdout: an fd of the test's
    // own that writes to `console.txt`.
    struct Console {
        QString consolePath;
        QString logPath;
        int fd{-1};
    };

    QTemporaryDir m_dir;

    Console openConsole(const QString &name) {
        Console console;
        console.consolePath = m_dir.filePath(name + QStringLiteral(".txt"));
        console.logPath = m_dir.filePath(name + QStringLiteral(".log"));
        console.fd = ::open(QFile::encodeName(console.consolePath).constData(),
                            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        return console;
    }

    static QByteArray readFile(const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

private slots:
    void initTestCase();
    void shutdownCopiesEverythingWrittenBeforeItInOrder();
    void idleStreamsDoNotWakeTheirThreads();
    void shutdownDoesNotWaitForOtherWriters();
};

void ConsoleTeeTest::initTestCase() {
    QVERIFY(m_dir.isValid());
}

void ConsoleTeeTest::shutdownCopiesEverythingWrittenBeforeItInOrder() {
    Console console = openConsole(QStringLiteral("order"));
    QVERIFY(console.fd >= 0);
    QStringList expected;
    QByteArray written;
    {
        ConsoleTee tee(console.logPath);
        QVERIFY(tee.install(console.fd, QStringLiteral("out")));
        // More than the pipe holds at once, in writes of every size, some
        // ending within a line, and a last line without its newline.
        for (int i = 0; i < 3000; ++i) {
            const QByteArray line = QByteArrayLiteral("line ") + QByteArray::number(i)
                + QByteArray(i % 97, 'x');
            expected.push_back(QString::fromLatin1(line));
            written += line + '\n';
        }
        expected.push_back(QStringLiteral("unterminated"));
        written += QByteArrayLiteral("unterminated");
        qsizetype offset = 0;
        for (int size = 1; offset < written.size(); size = size * 3 % 8191 + 1) {
            const QByteArray chunk = written.mid(offset, size);
            writeText(console.fd, chunk);
            offset += chunk.size();
        }
        // At once, while the thread is still copying.
        tee.shutdown();

        // The stream is back: later output goes to the console only.
        writeText(console.fd, QByteArrayLiteral("\nafter\n"));
    }
    ::close(console.fd);

    QCOMPARE(readFile(console.consolePath), written + QByteArrayLiteral("\nafter\n"));
    const QStringList logged = loggedMessages(console.logPath, QStringLiteral("out"));
    QCOMPARE(logged.size(), expected.size());
    QCOMPARE(logged, expected);
}

void ConsoleTeeTest::idleStreamsDoNotWakeTheirThreads() {
    Console out = openConsole(QStringLiteral("idle-out"));
    Console err = openConsole(QStringLiteral("idle-err"));
    QVERIFY(out.fd >= 0 && err.fd >= 0);
    {
        ConsoleTee tee(out.logPath);
        QVERIFY(tee.install(out.fd, QStringLiteral("stdout")));
        QVERIFY(tee.install(err.fd, QStringLiteral("stderr")));
        writeText(out.fd, QByteArrayLiteral("ready\n"));
        QTRY_COMPARE(loggedMessages(out.logPath, QStringLiteral("stdout")), QStringList{QStringLiteral("ready")});

        // Once both threads have gone to sleep in poll, nothing wakes them:
        // two that polled ten times a second would switch about twelve
        // times in 600 ms. A busy machine can only delay their sleep.
        QTRY_VERIFY(teeThreadsSleepInPoll(2));
        const qint64 before = teeContextSwitches();
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        const qint64 switches = teeContextSwitches() - before;
        QCOMPARE(switches, qint64{0});
        QVERIFY(teeThreadsSleepInPoll(2));

        // Output still wakes them.
        writeText(err.fd, QByteArrayLiteral("woken\n"));
        QTRY_COMPARE(loggedMessages(out.logPath, QStringLiteral("stderr")), QStringList{QStringLiteral("woken")});
    }
    ::close(out.fd);
    ::close(err.fd);
}

void ConsoleTeeTest::shutdownDoesNotWaitForOtherWriters() {
    Console console = openConsole(QStringLiteral("inherited"));
    QVERIFY(console.fd >= 0);
    int inherited = -1;
    {
        ConsoleTee tee(console.logPath);
        QVERIFY(tee.install(console.fd, QStringLiteral("out")));
        // Another holder of the stream, as a child process that inherited
        // it is: the pipe never reaches its end.
        inherited = ::dup(console.fd);
        QVERIFY(inherited >= 0);
        writeText(inherited, QByteArrayLiteral("from the child\n"));
        writeText(console.fd, QByteArrayLiteral("from the app\n"));
        tee.shutdown();
    }
    ::close(inherited);
    ::close(console.fd);
    QCOMPARE(readFile(console.consolePath), QByteArrayLiteral("from the child\nfrom the app\n"));
    QCOMPARE(loggedMessages(console.logPath, QStringLiteral("out")),
             (QStringList{QStringLiteral("from the child"), QStringLiteral("from the app")}));
}

int main(int argc, char **argv) {
    // No desktop session's platform theme; TestSessionIsolation.h.
    isolateFromDesktopSession();
    QCoreApplication app(argc, argv);
    ConsoleTeeTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_console_tee.moc"
