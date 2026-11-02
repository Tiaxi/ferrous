// SPDX-License-Identifier: GPL-3.0-or-later
//
// Measurement-only probe for the Ferrous desktop app, loaded with LD_PRELOAD.
// It changes nothing in the app's behaviour except while a driver asks it to
// (stall watchdog, synthetic input, QML evaluation), and writes one text log:
//
//   F  frame on the render thread: window, frame-begin, sync start/end,
//      render start/end, frame-end (ns, CLOCK_MONOTONIC), render-thread CPU
//      ns, Mesa/driver thread CPU ns, texture upload bytes/count, buffer
//      upload bytes/count, textures created, draw calls
//   A  afterAnimating on the GUI thread: window, time, GUI thread CPU ns
//   S  GUI-thread stall seen by the watchdog: post time, latency ns
//   M  marker from the driver
//   R  reply to a command
//
// Commands arrive one per line on the FIFO $FERROUS_PROBE_OUT/cmd; see
// README.md. The log is $FERROUS_PROBE_OUT/probe.log.
//
// QRhiResourceUpdateBatch upload calls are interposed (they cross from
// libQt6Quick into libQt6Gui through the PLT) to count bytes per frame.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMetaObject>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRasterWindow>
#include <QPainter>
#include <QScreen>
#include <QSocketNotifier>
#include <QTimer>
#include <QVariant>
#include <qpa/qwindowsysteminterface.h>
#include <rhi/qrhi.h>
#include <QtQuick/private/qquickitem_p.h>
#include <QtQuick/private/qquickwindow_p.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

qint64 monoNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

qint64 threadCpuNs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return qint64(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// CPU clock of another thread of this process (Linux: MAKE_THREAD_CPUCLOCK).
qint64 tidCpuNs(pid_t tid) {
    const clockid_t clk = clockid_t((~unsigned(tid) << 3) | 6);
    timespec ts;
    if (clock_gettime(clk, &ts) != 0) {
        return 0;
    }
    return qint64(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// ---------------------------------------------------------------- log

std::mutex g_logMutex;
std::string g_logBuffer;
FILE *g_logFile = nullptr;
std::string g_outDir;

void logLine(const std::string &line) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logBuffer += line;
    g_logBuffer += '\n';
}

void flushLog() {
    std::string chunk;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        chunk.swap(g_logBuffer);
    }
    if (g_logFile != nullptr && !chunk.empty()) {
        fwrite(chunk.data(), 1, chunk.size(), g_logFile);
        fflush(g_logFile);
    }
}

// ---------------------------------------------------------------- uploads

std::atomic<qint64> g_texBytes{0};
std::atomic<qint64> g_texCount{0};
std::atomic<qint64> g_bufBytes{0};
std::atomic<qint64> g_bufCount{0};
std::atomic<qint64> g_texCreated{0};
std::atomic<qint64> g_draws{0};

void *nextSym(const char *name) {
    // The private-API symbol version of the Qt the probe was built with,
    // e.g. Qt_6.11_PRIVATE_API.
    void *p = dlvsym(RTLD_NEXT, name, "Qt_" QT_STRINGIFY(QT_VERSION_MAJOR) "." QT_STRINGIFY(QT_VERSION_MINOR) "_PRIVATE_API");
    if (p == nullptr) {
        p = dlsym(RTLD_NEXT, name);
    }
    if (p == nullptr) {
        fprintf(stderr, "ferrous-probe: cannot find %s\n", name);
        abort();
    }
    return p;
}

qint64 descBytes(const QRhiTextureUploadDescription &desc) {
    qint64 total = 0;
    for (auto it = desc.cbeginEntries(); it != desc.cendEntries(); ++it) {
        const QRhiTextureSubresourceUploadDescription sub = it->description();
        if (!sub.image().isNull()) {
            const QSize s = sub.sourceSize().isEmpty() ? sub.image().size() : sub.sourceSize();
            total += qint64(s.width()) * s.height() * (sub.image().depth() / 8);
        } else {
            total += sub.data().size();
        }
    }
    return total;
}

// ---------------------------------------------------------------- frames

struct FrameState {
    qint64 begin = 0, syncStart = 0, syncEnd = 0, renderStart = 0, renderEnd = 0;
    qint64 cpuBegin = 0, driverBegin = 0;
    qint64 tex0 = 0, texN0 = 0, buf0 = 0, bufN0 = 0, created0 = 0, draws0 = 0;
};

std::mutex g_driverMutex;
std::vector<pid_t> g_driverTids;  // Mesa glthread and amdgpu threads.

void refreshDriverTids() {
    std::vector<pid_t> tids;
    DIR *dir = opendir("/proc/self/task");
    if (dir == nullptr) {
        return;
    }
    while (dirent *e = readdir(dir)) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (f == nullptr) {
            continue;
        }
        char comm[64] = {0};
        if (fgets(comm, sizeof comm, f) != nullptr) {
            // Mesa names its threads <process>:<role>, e.g. ferrous:gl0.
            if (strchr(comm, ':') != nullptr) {
                tids.push_back(pid_t(atoi(e->d_name)));
            }
        }
        fclose(f);
    }
    closedir(dir);
    std::lock_guard<std::mutex> lock(g_driverMutex);
    g_driverTids = tids;
}

qint64 driverCpuNs() {
    std::lock_guard<std::mutex> lock(g_driverMutex);
    qint64 total = 0;
    for (pid_t tid : g_driverTids) {
        total += tidCpuNs(tid);
    }
    return total;
}

class Probe;
Probe *g_probe = nullptr;

// Dirty-item census: which items made each frame, collected at the start of
// the scene graph sync while the GUI thread is blocked.
std::atomic<bool> g_dirtyOn{false};
std::mutex g_dirtyMutex;
QHash<QString, qint64> g_dirtyCounts;
qint64 g_dirtyFrames = 0;

QString itemLabel(QQuickItem *item) {
    QString id;
    if (QQmlContext *ctx = qmlContext(item)) {
        id = ctx->nameForObject(item);
    }
    QString parentId;
    if (QQuickItem *p = item->parentItem()) {
        if (QQmlContext *ctx = qmlContext(p)) {
            parentId = ctx->nameForObject(p);
        }
    }
    return QString("%1#%2/%3<%4").arg(item->metaObject()->className(), id, item->objectName(), parentId);
}

void collectDirty(QQuickWindow *w) {
    if (!g_dirtyOn.load()) {
        return;
    }
    QQuickWindowPrivate *wp = QQuickWindowPrivate::get(w);
    QSet<QString> seen;
    for (QQuickItem *item = wp->dirtyItemList; item != nullptr;
         item = QQuickItemPrivate::get(item)->nextDirtyItem) {
        const quint32 attrs = QQuickItemPrivate::get(item)->dirtyAttributes;
        seen.insert(itemLabel(item) + QString(" attrs=0x%1").arg(attrs, 0, 16));
    }
    std::lock_guard<std::mutex> lock(g_dirtyMutex);
    ++g_dirtyFrames;
    for (const QString &k : seen) {
        g_dirtyCounts[k] += 1;
    }
}

class CoverWindow : public QRasterWindow {
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(QRect(QPoint(), size()), QColor(40, 40, 40));
    }
};

class Probe : public QObject {
public:
    Probe() {
        g_outDir = qEnvironmentVariable("FERROUS_PROBE_OUT").toStdString();
        QDir().mkpath(QString::fromStdString(g_outDir));
        g_logFile = fopen((g_outDir + "/probe.log").c_str(), "a");
        logLine("M " + std::to_string(monoNs()) + " probe-start pid=" + std::to_string(getpid()));
        m_writer = std::thread([this] {
            pthread_setname_np(pthread_self(), "ferrous-probe");
            while (!m_stop.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                flushLog();
            }
            flushLog();
        });
        const std::string fifo = g_outDir + "/cmd";
        unlink(fifo.c_str());
        mkfifo(fifo.c_str(), 0600);
        // O_RDWR keeps the FIFO open when a writer closes it.
        m_fifoFd = open(fifo.c_str(), O_RDWR | O_NONBLOCK);
        m_notifier = new QSocketNotifier(m_fifoFd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, [this] { readCommands(); });
        // Windows appear after QML loads; look for them a few times, then
        // only on the `hook` command, so idle wakeups stay the app's own.
        for (int ms : {500, 1000, 2000, 4000, 8000, 15000}) {
            QTimer::singleShot(ms, this, [this] { hookWindows(); });
        }
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { shutdown(); });
        // FERROUS_PROBE_STALL=<ms> starts the watchdog with the app, for
        // startup measurements.
        if (!qEnvironmentVariableIsEmpty("FERROUS_PROBE_STALL")) {
            m_stallThresholdNs = qint64(qEnvironmentVariable("FERROUS_PROBE_STALL").toDouble() * 1e6);
            setStall(true);
        }
    }

    void shutdown() {
        setStall(false);
        logLine("M " + std::to_string(monoNs()) + " probe-quit");
        m_stop.store(true);
        if (m_writer.joinable()) {
            m_writer.join();
        }
    }

    bool event(QEvent *e) override {
        if (e->type() == m_pingType) {
            const qint64 now = monoNs();
            const qint64 posted = m_pingPosted.load();
            const qint64 latency = now - posted;
            m_latencyHist[std::min<qint64>(latency / 250000, kHistBins - 1)]++;
            if (latency >= m_stallThresholdNs) {
                logLine("S " + std::to_string(posted) + " " + std::to_string(latency));
            }
            m_pingPending.store(false);
            return true;
        }
        return QObject::event(e);
    }

private:
    static constexpr int kHistBins = 400;  // 250 µs bins up to 100 ms.

    void hookWindows() {
        refreshDriverTids();
        for (QWindow *w : QGuiApplication::topLevelWindows()) {
            auto *qw = qobject_cast<QQuickWindow *>(w);
            if (qw == nullptr || m_hooked.contains(qw)) {
                continue;
            }
            m_hooked.append(qw);
            const int id = m_hooked.size() - 1;
            auto *state = new FrameState;
            logLine("M " + std::to_string(monoNs()) + " hook window=" + std::to_string(id)
                    + " class=" + qw->metaObject()->className()
                    + " title=" + (qw->objectName().isEmpty() ? std::string("-") : qw->objectName().toStdString()));
            connect(qw, &QQuickWindow::beforeFrameBegin, qw, [state] {
                state->begin = monoNs();
                state->cpuBegin = threadCpuNs();
                state->driverBegin = driverCpuNs();
                state->tex0 = g_texBytes.load();
                state->texN0 = g_texCount.load();
                state->buf0 = g_bufBytes.load();
                state->bufN0 = g_bufCount.load();
                state->created0 = g_texCreated.load();
                state->draws0 = g_draws.load();
                state->syncStart = state->syncEnd = state->renderStart = state->renderEnd = 0;
            }, Qt::DirectConnection);
            connect(qw, &QQuickWindow::beforeSynchronizing, qw, [state, qw] {
                state->syncStart = monoNs();
                collectDirty(qw);
            }, Qt::DirectConnection);
            connect(qw, &QQuickWindow::afterSynchronizing, qw, [state] { state->syncEnd = monoNs(); }, Qt::DirectConnection);
            connect(qw, &QQuickWindow::beforeRendering, qw, [state] { state->renderStart = monoNs(); }, Qt::DirectConnection);
            connect(qw, &QQuickWindow::afterRendering, qw, [state] { state->renderEnd = monoNs(); }, Qt::DirectConnection);
            connect(qw, &QQuickWindow::afterFrameEnd, qw, [state, id] {
                const qint64 end = monoNs();
                char line[400];
                snprintf(line, sizeof line, "F %d %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld", id,
                         (long long)state->begin, (long long)state->syncStart, (long long)state->syncEnd,
                         (long long)state->renderStart, (long long)state->renderEnd, (long long)end,
                         (long long)(threadCpuNs() - state->cpuBegin),
                         (long long)(driverCpuNs() - state->driverBegin),
                         (long long)(g_texBytes.load() - state->tex0), (long long)(g_texCount.load() - state->texN0),
                         (long long)(g_bufBytes.load() - state->buf0), (long long)(g_bufCount.load() - state->bufN0),
                         (long long)(g_texCreated.load() - state->created0),
                         (long long)(g_draws.load() - state->draws0));
                logLine(line);
            }, Qt::DirectConnection);
            connect(qw, &QQuickWindow::afterAnimating, this, [id] {
                logLine("A " + std::to_string(id) + " " + std::to_string(monoNs()) + " " + std::to_string(threadCpuNs()));
            }, Qt::DirectConnection);
        }
    }

    QQuickWindow *mainWindow() {
        hookWindows();
        for (QQuickWindow *w : m_hooked) {
            if (w != nullptr && qmlContext(w) != nullptr) {
                return w;
            }
        }
        return m_hooked.isEmpty() ? nullptr : m_hooked.first().data();
    }

    void setStall(bool on) {
        if (on == m_stallOn.load()) {
            return;
        }
        m_stallOn.store(on);
        if (on) {
            std::fill(std::begin(m_latencyHist), std::end(m_latencyHist), 0);
            m_watchdog = std::thread([this] {
                pthread_setname_np(pthread_self(), "ferrous-probe-wd");
                while (m_stallOn.load()) {
                    timespec ts{0, m_pingPeriodNs};
                    nanosleep(&ts, nullptr);
                    if (!m_pingPending.load()) {
                        m_pingPending.store(true);
                        m_pingPosted.store(monoNs());
                        QCoreApplication::postEvent(this, new QEvent(m_pingType), Qt::HighEventPriority);
                    }
                }
            });
        } else {
            m_watchdog.join();
            std::string hist = "R " + std::to_string(monoNs()) + " stall-hist";
            for (int i = 0; i < kHistBins; ++i) {
                if (m_latencyHist[i] != 0) {
                    hist += " " + std::to_string(i) + ":" + std::to_string(m_latencyHist[i]);
                }
            }
            logLine(hist);
        }
    }

    void reply(const std::string &what) {
        logLine("R " + std::to_string(monoNs()) + " " + what);
        flushLog();
    }

    QPointF parsePoint(const QStringList &a, int i) { return QPointF(a.value(i).toDouble(), a.value(i + 1).toDouble()); }

    void mouse(QQuickWindow *w, QEvent::Type type, QPointF p, Qt::MouseButtons state, Qt::MouseButton button) {
        QWindowSystemInterface::handleMouseEvent(w, ulong(monoNs() / 1000000), p, w->mapToGlobal(p), state, button, type);
    }

    void dumpItems(QQuickItem *item, int depth, QString &out) {
        if (item == nullptr) {
            return;
        }
        QString id;
        if (QQmlContext *ctx = qmlContext(item)) {
            id = ctx->nameForObject(item);
        }
        const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        out += QString("%1%2 id=%3 name=%4 vis=%5 rect=%6,%7,%8x%9\n")
                   .arg(QString(depth, ' '), item->metaObject()->className(), id, item->objectName())
                   .arg(item->isVisible())
                   .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
        for (QQuickItem *c : item->childItems()) {
            dumpItems(c, depth + 1, out);
        }
    }

    void readCommands() {
        char buf[8192];
        ssize_t n;
        while ((n = read(m_fifoFd, buf, sizeof buf)) > 0) {
            m_pending.append(buf, n);
        }
        int nl;
        while ((nl = m_pending.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(m_pending.left(nl)).trimmed();
            m_pending.remove(0, nl + 1);
            if (!line.isEmpty()) {
                handle(line);
            }
        }
    }

    void handle(const QString &line) {
        const QStringList a = line.split(' ', Qt::SkipEmptyParts);
        const QString cmd = a.value(0);
        QQuickWindow *w = mainWindow();
        if (cmd == "mark") {
            logLine("M " + std::to_string(monoNs()) + " " + line.mid(5).toStdString());
            flushLog();
        } else if (cmd == "hook") {
            hookWindows();
            reply("hooked " + std::to_string(m_hooked.size()));
        } else if (cmd == "stall") {
            if (a.size() > 2) {
                m_stallThresholdNs = qint64(a.value(2).toDouble() * 1e6);
            }
            setStall(a.value(1) == "on");
            reply("stall " + a.value(1).toStdString());
        } else if (cmd == "eval" && w != nullptr) {
            QQmlExpression expr(qmlContext(w), w, line.mid(5));
            const QVariant v = expr.evaluate();
            QString text = expr.hasError() ? "ERROR " + expr.error().toString()
                                           : QString::fromUtf8(QJsonDocument::fromVariant(QVariantList{v}).toJson(QJsonDocument::Compact));
            reply("eval " + text.toStdString());
        } else if (cmd == "dump" && w != nullptr) {
            QString out;
            for (QQuickWindow *hw : m_hooked) {
                if (hw != nullptr) {
                    out += QString("window %1 %2x%3 dpr=%4\n").arg(hw->metaObject()->className()).arg(hw->width()).arg(hw->height()).arg(hw->devicePixelRatio());
                    dumpItems(hw->contentItem(), 0, out);
                }
            }
            QFile f(QString::fromStdString(g_outDir) + "/" + a.value(1, "items.txt"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(out.toUtf8());
            }
            reply("dumped");
        } else if (cmd == "click" && w != nullptr) {
            const QPointF p = parsePoint(a, 1);
            mouse(w, QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
            mouse(w, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
            mouse(w, QEvent::MouseButtonRelease, p, Qt::NoButton, Qt::LeftButton);
        } else if (cmd == "dblclick" && w != nullptr) {
            const QPointF p = parsePoint(a, 1);
            mouse(w, QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
            mouse(w, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
            mouse(w, QEvent::MouseButtonRelease, p, Qt::NoButton, Qt::LeftButton);
            mouse(w, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
            mouse(w, QEvent::MouseButtonDblClick, p, Qt::LeftButton, Qt::LeftButton);
            mouse(w, QEvent::MouseButtonRelease, p, Qt::NoButton, Qt::LeftButton);
        } else if (cmd == "move" && w != nullptr) {
            mouse(w, QEvent::MouseMove, parsePoint(a, 1), Qt::NoButton, Qt::NoButton);
        } else if (cmd == "drag" && w != nullptr) {
            // drag x0 y0 x1 y1 duration_ms: press, moves every ~4 ms, release.
            const QPointF p0 = parsePoint(a, 1);
            const QPointF p1 = parsePoint(a, 3);
            const int ms = a.value(5, "500").toInt();
            mouse(w, QEvent::MouseMove, p0, Qt::NoButton, Qt::NoButton);
            mouse(w, QEvent::MouseButtonPress, p0, Qt::LeftButton, Qt::LeftButton);
            auto *timer = new QTimer(this);
            timer->setTimerType(Qt::PreciseTimer);
            auto *clock = new QElapsedTimer;
            clock->start();
            QPointer<QQuickWindow> pw(w);
            connect(timer, &QTimer::timeout, this, [this, timer, clock, pw, p0, p1, ms] {
                if (pw.isNull()) {
                    timer->deleteLater();
                    delete clock;
                    return;
                }
                const double t = std::min(1.0, clock->elapsed() / double(ms));
                const QPointF p = p0 + (p1 - p0) * t;
                if (t < 1.0) {
                    mouse(pw, QEvent::MouseMove, p, Qt::LeftButton, Qt::NoButton);
                } else {
                    mouse(pw, QEvent::MouseMove, p1, Qt::LeftButton, Qt::NoButton);
                    mouse(pw, QEvent::MouseButtonRelease, p1, Qt::NoButton, Qt::LeftButton);
                    timer->stop();
                    timer->deleteLater();
                    delete clock;
                    reply("drag-done");
                }
            });
            timer->start(4);
        } else if (cmd == "wheel" && w != nullptr) {
            const QPointF p = parsePoint(a, 1);
            const int dy = a.value(3, "-120").toInt();
            QWindowSystemInterface::handleWheelEvent(w, p, w->mapToGlobal(p), QPoint(), QPoint(0, dy));
        } else if (cmd == "key" && w != nullptr) {
            // key <Qt::Key decimal> [text]
            const int key = a.value(1).toInt();
            const QString text = a.value(2);
            QWindowSystemInterface::handleKeyEvent(w, QEvent::KeyPress, key, Qt::NoModifier, text);
            QWindowSystemInterface::handleKeyEvent(w, QEvent::KeyRelease, key, Qt::NoModifier, text);
        } else if (cmd == "type" && w != nullptr) {
            // type <text>: one key press per character, every `interval` ms.
            const QString text = line.mid(5);
            for (int i = 0; i < text.size(); ++i) {
                QPointer<QQuickWindow> pw(w);
                const QString ch = text.mid(i, 1);
                QTimer::singleShot(i * 120, this, [pw, ch] {
                    if (pw.isNull()) {
                        return;
                    }
                    const int key = ch.at(0).isLetter() ? Qt::Key_A + (ch.at(0).toLower().unicode() - 'a') : ch.at(0).unicode();
                    QWindowSystemInterface::handleKeyEvent(pw, QEvent::KeyPress, key, Qt::NoModifier, ch);
                    QWindowSystemInterface::handleKeyEvent(pw, QEvent::KeyRelease, key, Qt::NoModifier, ch);
                });
            }
        } else if (cmd == "resize" && w != nullptr) {
            w->resize(a.value(1).toInt(), a.value(2).toInt());
        } else if (cmd == "minimize" && w != nullptr) {
            w->showMinimized();
        } else if (cmd == "restore" && w != nullptr) {
            w->showNormal();
            w->raise();
            w->requestActivate();
        } else if (cmd == "maximize" && w != nullptr) {
            w->showMaximized();
        } else if (cmd == "cover") {
            if (m_cover == nullptr) {
                m_cover = new CoverWindow;
                m_cover->setTitle("ferrous-probe cover");
            }
            if (a.value(1) == "off") {
                m_cover->hide();
            } else {
                const QSize s = w != nullptr && w->screen() != nullptr ? w->screen()->size() : QSize(3440, 1440);
                m_cover->resize(a.value(1, QString::number(s.width())).toInt(), a.value(2, QString::number(s.height())).toInt());
                m_cover->show();
                m_cover->raise();
                m_cover->requestActivate();
            }
            reply("cover");
        } else if (cmd == "dirty") {
            if (a.value(1) == "on") {
                std::lock_guard<std::mutex> lock(g_dirtyMutex);
                g_dirtyCounts.clear();
                g_dirtyFrames = 0;
                g_dirtyOn.store(true);
                reply("dirty on");
            } else {
                g_dirtyOn.store(false);
                std::lock_guard<std::mutex> lock(g_dirtyMutex);
                QList<QPair<qint64, QString>> rows;
                for (auto it = g_dirtyCounts.cbegin(); it != g_dirtyCounts.cend(); ++it) {
                    rows.append({it.value(), it.key()});
                }
                std::sort(rows.begin(), rows.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
                QFile f(QString::fromStdString(g_outDir) + "/" + a.value(2, "dirty.txt"));
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    f.write(QString("frames %1\n").arg(g_dirtyFrames).toUtf8());
                    for (const auto &r : rows) {
                        f.write(QString("%1 %2\n").arg(r.first).arg(r.second).toUtf8());
                    }
                }
                reply("dirty off");
            }
        } else if (cmd == "setprop" && w != nullptr) {
            // setprop <objectName> <property> <number>: on every item so named.
            int n = 0;
            QList<QQuickItem *> stack{w->contentItem()};
            while (!stack.isEmpty()) {
                QQuickItem *item = stack.takeLast();
                if (item->objectName() == a.value(1)) {
                    item->setProperty(a.value(2).toUtf8().constData(), a.value(3).toDouble());
                    ++n;
                }
                stack.append(item->childItems());
            }
            reply("setprop " + std::to_string(n));
        } else if (cmd == "winstate") {
            std::string out = "winstate";
            QList<QWindow *> wins;
            for (QQuickWindow *hw : m_hooked) {
                if (hw != nullptr) {
                    wins.append(hw);
                }
            }
            if (m_cover != nullptr) {
                wins.append(m_cover);
            }
            for (QWindow *win : wins) {
                out += QString(" [%1 vis=%2 visibility=%3 exposed=%4 active=%5 state=%6 geo=%7,%8,%9x%10]")
                           .arg(win->objectName().isEmpty() ? win->title() : win->objectName())
                           .arg(win->isVisible()).arg(int(win->visibility())).arg(win->isExposed())
                           .arg(win->isActive()).arg(int(win->windowStates()))
                           .arg(win->x()).arg(win->y()).arg(win->width()).arg(win->height())
                           .toStdString();
            }
            reply(out);
        } else if (cmd == "quit") {
            QCoreApplication::quit();
        } else {
            reply("unknown " + line.toStdString());
        }
    }

    std::thread m_writer;
    std::thread m_watchdog;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_stallOn{false};
    std::atomic<bool> m_pingPending{false};
    std::atomic<qint64> m_pingPosted{0};
    long m_pingPeriodNs = 1000000;
    qint64 m_stallThresholdNs = 3000000;
    qint64 m_latencyHist[kHistBins] = {};
    const QEvent::Type m_pingType = QEvent::Type(QEvent::registerEventType());
    int m_fifoFd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QByteArray m_pending;
    QList<QPointer<QQuickWindow>> m_hooked;
    CoverWindow *m_cover = nullptr;
};

void startProbe() {
    if (qEnvironmentVariableIsEmpty("FERROUS_PROBE_OUT") || g_probe != nullptr) {
        return;
    }
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()) == nullptr) {
        return;
    }
    g_probe = new Probe;
}

}  // namespace

Q_COREAPP_STARTUP_FUNCTION(startProbe)

// ---------------------------------------------------------------- interposers

using UploadDescFn = void (*)(QRhiResourceUpdateBatch *, QRhiTexture *, const QRhiTextureUploadDescription &);
using UploadImageFn = void (*)(QRhiResourceUpdateBatch *, QRhiTexture *, const QImage &);
using DynArrFn = void (*)(QRhiResourceUpdateBatch *, QRhiBuffer *, quint32, QByteArray);
using DynPtrFn = void (*)(QRhiResourceUpdateBatch *, QRhiBuffer *, quint32, quint32, const void *);
using StaticPtrFn = void (*)(QRhiResourceUpdateBatch *, QRhiBuffer *, quint32, quint32, const void *);
using StaticPtrAllFn = void (*)(QRhiResourceUpdateBatch *, QRhiBuffer *, const void *);
using StaticArrFn = void (*)(QRhiResourceUpdateBatch *, QRhiBuffer *, quint32, QByteArray);
using StaticArrAllFn = void (*)(QRhiResourceUpdateBatch *, QRhiBuffer *, QByteArray);
using NewTexFn = QRhiTexture *(*)(QRhi *, QRhiTexture::Format, const QSize &, int, QRhiTexture::Flags);

void QRhiResourceUpdateBatch::uploadTexture(QRhiTexture *tex, const QRhiTextureUploadDescription &desc) {
    static auto real = reinterpret_cast<UploadDescFn>(nextSym("_ZN23QRhiResourceUpdateBatch13uploadTextureEP11QRhiTextureRK28QRhiTextureUploadDescription"));
    g_texBytes += descBytes(desc);
    g_texCount += 1;
    real(this, tex, desc);
}

void QRhiResourceUpdateBatch::uploadTexture(QRhiTexture *tex, const QImage &image) {
    static auto real = reinterpret_cast<UploadImageFn>(nextSym("_ZN23QRhiResourceUpdateBatch13uploadTextureEP11QRhiTextureRK6QImage"));
    g_texBytes += image.sizeInBytes();
    g_texCount += 1;
    real(this, tex, image);
}

void QRhiResourceUpdateBatch::updateDynamicBuffer(QRhiBuffer *buf, quint32 offset, QByteArray data) {
    static auto real = reinterpret_cast<DynArrFn>(nextSym("_ZN23QRhiResourceUpdateBatch19updateDynamicBufferEP10QRhiBufferj10QByteArray"));
    g_bufBytes += data.size();
    g_bufCount += 1;
    real(this, buf, offset, std::move(data));
}

void QRhiResourceUpdateBatch::updateDynamicBuffer(QRhiBuffer *buf, quint32 offset, quint32 size, const void *data) {
    static auto real = reinterpret_cast<DynPtrFn>(nextSym("_ZN23QRhiResourceUpdateBatch19updateDynamicBufferEP10QRhiBufferjjPKv"));
    g_bufBytes += size;
    g_bufCount += 1;
    real(this, buf, offset, size, data);
}

void QRhiResourceUpdateBatch::uploadStaticBuffer(QRhiBuffer *buf, quint32 offset, quint32 size, const void *data) {
    static auto real = reinterpret_cast<StaticPtrFn>(nextSym("_ZN23QRhiResourceUpdateBatch18uploadStaticBufferEP10QRhiBufferjjPKv"));
    g_bufBytes += size;
    g_bufCount += 1;
    real(this, buf, offset, size, data);
}

void QRhiResourceUpdateBatch::uploadStaticBuffer(QRhiBuffer *buf, const void *data) {
    static auto real = reinterpret_cast<StaticPtrAllFn>(nextSym("_ZN23QRhiResourceUpdateBatch18uploadStaticBufferEP10QRhiBufferPKv"));
    g_bufBytes += buf->size();
    g_bufCount += 1;
    real(this, buf, data);
}

void QRhiResourceUpdateBatch::uploadStaticBuffer(QRhiBuffer *buf, quint32 offset, QByteArray data) {
    static auto real = reinterpret_cast<StaticArrFn>(nextSym("_ZN23QRhiResourceUpdateBatch18uploadStaticBufferEP10QRhiBufferj10QByteArray"));
    g_bufBytes += data.size();
    g_bufCount += 1;
    real(this, buf, offset, std::move(data));
}

void QRhiResourceUpdateBatch::uploadStaticBuffer(QRhiBuffer *buf, QByteArray data) {
    static auto real = reinterpret_cast<StaticArrAllFn>(nextSym("_ZN23QRhiResourceUpdateBatch18uploadStaticBufferEP10QRhiBuffer10QByteArray"));
    g_bufBytes += data.size();
    g_bufCount += 1;
    real(this, buf, std::move(data));
}

QRhiTexture *QRhi::newTexture(QRhiTexture::Format format, const QSize &pixelSize, int sampleCount, QRhiTexture::Flags flags) {
    static auto real = reinterpret_cast<NewTexFn>(nextSym("_ZN4QRhi10newTextureEN11QRhiTexture6FormatERK5QSizei6QFlagsINS0_4FlagEE"));
    g_texCreated += 1;
    return real(this, format, pixelSize, sampleCount, flags);
}

using DrawFn = void (*)(QRhiCommandBuffer *, quint32, quint32, quint32, quint32);
using DrawIndexedFn = void (*)(QRhiCommandBuffer *, quint32, quint32, quint32, qint32, quint32);

void QRhiCommandBuffer::draw(quint32 vertexCount, quint32 instanceCount, quint32 firstVertex, quint32 firstInstance) {
    static auto real = reinterpret_cast<DrawFn>(nextSym("_ZN17QRhiCommandBuffer4drawEjjjj"));
    g_draws += 1;
    real(this, vertexCount, instanceCount, firstVertex, firstInstance);
}

void QRhiCommandBuffer::drawIndexed(quint32 indexCount, quint32 instanceCount, quint32 firstIndex, qint32 vertexOffset, quint32 firstInstance) {
    static auto real = reinterpret_cast<DrawIndexedFn>(nextSym("_ZN17QRhiCommandBuffer11drawIndexedEjjjij"));
    g_draws += 1;
    real(this, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}
