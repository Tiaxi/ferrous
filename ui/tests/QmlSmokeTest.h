// SPDX-License-Identifier: GPL-3.0-or-later

// What the QML smoke tests share: the test classes' base, and the helpers
// that tests of more than one class use. Each test class is in a file of its
// own, tst_qml_smoke_<name>.cpp, with the helpers only its tests use; the
// test program and its areas are in tst_qml_smoke.cpp.

#pragma once

#include <QAccessible>
#include <QApplication>
#include <QClipboard>
#include <QFont>
#include <QFontDatabase>
#include "../src/GlobalSearchResultsModel.h"
#include <QDateTime>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFile>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QHoverEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QPainter>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QStyleHints>
#include <QSGImageNode>
#include <QSGRectangleNode>
#include <QSGSimpleRectNode>
#include <QSGSimpleTextureNode>
#include <QSGRendererInterface>
#include <QSGTransformNode>
#include <QQmlComponent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlIncubator>
#include <QQmlProperty>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMetaMethod>
#include <QRegularExpression>
#include <QMutex>
#include <QMutexLocker>
#include <QScopedPointer>
#include <QSemaphore>
#include <QScopeGuard>
#include <QSet>
#include <QDir>
#include <QTemporaryFile>
#include <QFutureWatcher>
#include <atomic>
#include <functional>
#include <iterator>
#include <memory>
#include <QtEndian>
#include <QtTest/QtTest>
#include <qqml.h>
#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <utility>

#include "../src/BridgeClient.h"
#include "../src/CoverImageProvider.h"
#include "../src/DiagnosticsLog.h"
#include "../src/DisplaySleepInhibitor.h"
#include "../src/FerrousBridgeFfi.h"
#include "../src/LibraryTreeModel.h"
#include "../src/MprisController.h"
#include "../src/QmlTypes.h"
#include "../src/SpectrogramSeekTrace.h"
#include "../src/WaveformSceneGraph.h"
#include "../src/WindowActivation.h"
#include "TestPlatform.h"
#include "TestUserDirectories.h"
#include "WaveformReaderLog.h"
#define protected public
#define private public
#include "../src/SpectrogramItem.h"
#include "../src/TagEditorController.h"
#include "../src/WaveformItem.h"
#include "../src/WaveformEditorItem.h"
#include "../src/LevelMeterItem.h"
#include "../src/WindowPresence.h"
#undef private
#undef protected

// The playback clock of an item under test, which the item owns.
template <typename Item>
PlaybackClock &testClock(Item &item) {
    auto *clock = item.template findChild<PlaybackClock *>(
        QStringLiteral("testPlaybackClock"), Qt::FindDirectChildrenOnly);
    if (clock == nullptr) {
        clock = new PlaybackClock(&item);
        clock->setObjectName(QStringLiteral("testPlaybackClock"));
        item.setClock(clock);
    }
    return *clock;
}

// Report to the item's clock that playback is at `seconds` now, as the
// engine does: the position advances from there while the item plays. The
// clock moves there directly rather than correcting towards it.
template <typename Item>
void setTestPosition(Item &item, double seconds, quint64 trackToken = 0) {
    PlaybackClock &clock = testClock(item);
    PlaybackSample sample;
    sample.positionSeconds = seconds;
    sample.sampledAtNs = clock.nowNs();
    sample.trackToken = trackToken != 0 ? trackToken : clock.state().trackToken;
    clock.applySample(sample, false);
    if (item.property("playing").toBool()) {
        sample.advancing = true;
        clock.applySample(sample, false);
    }
}

// Report to the item's clock that playback was at `seconds` `agoSeconds`
// ago and has advanced since.
template <typename Item>
void setTestPositionAgo(Item &item, double seconds, double agoSeconds) {
    PlaybackClock &clock = testClock(item);
    PlaybackSample sample;
    sample.positionSeconds = seconds;
    sample.sampledAtNs = clock.nowNs() - static_cast<qint64>(agoSeconds * 1e9);
    sample.trackToken = clock.state().trackToken;
    clock.applySample(sample, false);
    sample.advancing = true;
    clock.applySample(sample, false);
}

// Lets `window` count as on screen without showing it, so that the test,
// not the window, draws the items in it.
void presentWithoutDrawing(QWindow &window);

// Makes `window`'s presence see it exposed or not, as an expose event
// from the platform tells it; a window that stops being exposed still
// counts as shown until its grace ends.
void setTestExposed(QWindow &window, bool exposed);

// Ends the grace of `window`'s presences, as if they had stayed unexposed
// for it.
void endTestUnexposedGrace(QWindow &window);

// Time a test sets for an item and its clock, so that nothing the test
// checks depends on how fast it runs. It outlives the items it drives.
struct TestTime {
    qint64 nowNs{monotonicNowNs()};

    void drive(SpectrogramItem &item) {
        item.m_now = [this]() {
            return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(nowNs));
        };
        testClock(item).setTimeSource([this]() { return nowNs; });
    }
    void drive(WaveformEditorItem &item) {
        item.m_nowNs = [this]() { return nowNs; };
        testClock(item).setTimeSource([this]() { return nowNs; });
    }
    void advance(double seconds) { nowNs += static_cast<qint64>(std::llround(seconds * 1e9)); }
};

// How many canvas spans are drawn.
int drawnCanvasFragments(QSGNode *node);

// A pane playing back 384 kHz columns 64 samples apart: 6000 columns a
// second, one a pixel, with 12000 columns decoded and the playhead at
// 0.5 s. That is eight times the columns of the deepest zoom, 750 a
// second at every rate, as a margin.
void prepareHighRatePlayback(SpectrogramItem &item, int width, int height, int bins, bool rolling);

// The offscreen screen at `scale` that main() adds for HiDPI tests, if any;
// see addHiDpiTestScreens() in tst_qml_smoke.cpp.
QScreen *hiDpiTestScreen(double scale);

// Moves a window to a screen, as dragging it there would, and waits for its
// device pixel ratio to follow.
bool moveWindowToScreen(QQuickWindow &window, QScreen *screen);

// A window on the HiDPI test screen at `scale`, shown.
bool showOnScreen(QQuickWindow &window, double scale);

// What an overlay layer of a spectrogram's scene node shows: its lines,
// and its images with their textures' sizes.
struct OverlayOnScreen {
    QVector<QRectF> lines;
    QVector<QPair<QRectF, QSize>> images;
};

// The spectrogram's floor, plotBg, where it has no column to draw: the
// colour map's floor, black.
const QRgb kSpectrogramFloor = qRgb(0, 0, 0);

// A canvas pixel without a column: transparent, so that older data drawn
// under the canvas shows there, and else the floor.
const QRgb kSpectrogramNoColumn = qRgba(0, 0, 0, 0);

// The children of a spectrogram's scene node: the background, the canvas
// tiles, the newest columns, the playhead's casing and the playhead, the
// frequency scale, the time scale, the crosshair and the FPS counter.
constexpr int kPlayheadChild = 4;

OverlayOnScreen overlayOnScreen(QSGNode *node, int layer);

// Lets the item's zoom debounce send its request to the backend.
bool sendPendingBackendZoom(SpectrogramItem &item);

// Rasterizes an item's canvas at once, as its worker would.
void rebuildCanvasNow(
    SpectrogramItem &item, int width, int height,
    qint64 displayLeft, qint64 displayRight, bool rollingMode);

// The item's canvas pixels from the view's start, unwrapped.
QImage canvasInViewOrder(const SpectrogramItem &item);

bool spectrogramRasterPending(SpectrogramItem &item);

// Paints the item, then again once each rasterization the painting starts
// has arrived.
QSGNode *paintSettled(SpectrogramItem &item, QSGNode *node);

struct BinaryTreeRow {
    quint8 rowType{0};
    quint16 depth{0};
    qint32 sourceIndex{-1};
    quint32 trackNumber{0};
    quint32 childCount{0};
    QString title;
    QString key;
    QString artist;
    QString path;
    QString coverPath;
    QString trackPath;
    QStringList playPaths;
    // Encoded after coverPath.
    quint64 coverStamp{0};
    // Encoded after childCount.
    qint32 year{0};
};

template <typename T>
void appendLe(QByteArray &out, T value) {
    const T little = qToLittleEndian(value);
    out.append(reinterpret_cast<const char *>(&little), static_cast<int>(sizeof(T)));
}

void appendUtf8(QByteArray &out, const QString &value);

QByteArray encodeRows(const QVector<BinaryTreeRow> &rows);

QByteArray sampleArtistAlbumTreeBinary(quint64 albumCoverStamp = 0);

QString formatQmlErrors(const QList<QQmlError> &errors);

QMutex &capturedMessageMutex();

QStringList &capturedMessages();

void clearCapturedMessages();

QString takeCapturedMessagesText();

QObject *findObjectByStringProperty(QObject *root, const char *propertyName, const QString &expectedValue);

QQuickItem *findQuickItemByObjectName(QQuickItem *root, const QString &objectName);

template <typename Item>
void collectQuickItems(QQuickItem *root, QList<Item *> *out) {
    if (auto *item = qobject_cast<Item *>(root)) {
        out->append(item);
    }
    for (QQuickItem *child : root->childItems()) {
        collectQuickItems(child, out);
    }
}

QObject *createQmlObjectFromSource(
    QQmlEngine &engine,
    const QByteArray &qmlSource,
    const QUrl &baseUrl,
    QString *errorText);

// Records each signal of an item that reaches its slots while the item's
// state mutex is held. A slot that reads the item, as a QML binding does,
// would take the mutex again and deadlock.
class StateMutexProbe final : public QObject {
    Q_OBJECT

public:
    StateMutexProbe(QObject *item, QMutex *mutex) : m_mutex(mutex) {
        const QMetaObject *meta = item->metaObject();
        const QMetaMethod probe =
            staticMetaObject.method(staticMetaObject.indexOfSlot("delivered()"));
        for (int index = QObject::staticMetaObject.methodCount();
             index < meta->methodCount(); ++index) {
            const QMetaMethod method = meta->method(index);
            if (method.methodType() == QMetaMethod::Signal) {
                connect(item, method, this, probe, Qt::DirectConnection);
            }
        }
    }

    QStringList heldSignals;
    int deliveries{0};

public slots:
    void delivered() {
        ++deliveries;
        if (m_mutex->tryLock()) {
            m_mutex->unlock();
            return;
        }
        heldSignals << QString::fromLatin1(
            sender()->metaObject()->method(senderSignalIndex()).methodSignature());
    }

private:
    QMutex *m_mutex;
};

// Main.qml with the fallback bridge, shown.
QQuickWindow *loadMainWindowShown(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel);

// The messages of the error history, newest first.
QStringList errorHistoryMessages(QObject *errors);

// The shown text items under `root` whose text is `text`.
QList<QQuickItem *> visibleTextItems(QQuickItem *root, const QString &text);

// The items named `objectName` under `root`, in the order of the item tree.
QList<QQuickItem *> itemsNamed(QQuickItem *root, const QString &objectName);

// The first item named `objectName` under `root`.
QQuickItem *itemNamed(QQuickItem *root, const QString &objectName);

// The first item named `objectName` that is visible in `window`.
QQuickItem *visibleItemNamed(QQuickWindow *window, const QString &objectName);

// An item's rectangle in its window.
QRectF sceneRectOf(const QQuickItem *item);

// Whether the row of buttons that `button` is in has placed it clear of the
// others, inside the row and inside the window: a button that has just
// appeared is in its place only after the row's next layout, and until then
// the row keeps the width it had without it, and its other items where they
// were the last time they showed.
bool clearOfTheOtherButtons(QQuickItem *button);

// The middle of `item`, in its window.
QPoint centerInWindow(QQuickItem *item);

// Waits until the engine has built the objects it builds in the background,
// such as the overflow menus of Kirigami's inline messages, so that
// destroying the engine leaves none half made.
bool finishIncubation(QQmlEngine &engine);

// Loads and shows Main.qml without a backend, so it uses its fallback bridge.
QQuickWindow *loadMainWindowForQuit(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel);

inline auto keepRunningWhenTheWindowCloses() {
    const bool quitOnLastWindowClosed = QGuiApplication::quitOnLastWindowClosed();
    QGuiApplication::setQuitOnLastWindowClosed(false);
    return qScopeGuard([quitOnLastWindowClosed]() {
        QGuiApplication::setQuitOnLastWindowClosed(quitOnLastWindowClosed);
    });
}

bool clickButton(QObject *window, const QString &objectName);

// Makes `window` the one MPRIS Quit and Raise act on, until the guard ends.
inline auto useAsMainWindow(QWindow *window) {
    setMainActivationWindow(window);
    return qScopeGuard([]() { setMainActivationWindow(nullptr); });
}

// Calls MPRIS Quit as the session bus would. The controller is gone before
// the event loop runs, so it never registers on the session bus.
void callMprisQuit();

// The first item named `objectName` below `root` that is shown.
QQuickItem *shownItemNamed(QQuickItem *root, const QString &objectName);

// How long a test waits for workers and decodes, which an unoptimized build
// on a busy machine makes many times slower.
constexpr int kSlowBuildTimeoutMs = 30'000;

// Gives `item` raw samples of `channels` channels at `rate` from `start` to
// `end` seconds, a sine of its own in each channel, as a detail window at
// sample resolution.
// With `sameSource`, the window is the next of the same file, as one a
// view asks for while it moves on; otherwise its data is new, as another
// file's.
void giveSampleDetail(WaveformEditorItem &item, int rate, int channels, double start, double end,
    bool sameSource = false);

// The columns of each of `lanes` lanes of `image` with no waveform pixel,
// as "lane L: N empty from X" for each lane that has any, but the columns
// `skipFirst` to `skipLast`, which the playhead covers.
QString lanesWithEmptyColumns(const QImage &image, int lanes, int skipFirst = -1, int skipLast = -2);

double median(std::vector<double> values);

// A centered pane whose whole track is in view, as when zoomed out: 380
// columns at 48 kHz and the hop of a zoom of 0.05, drawn a column to a
// logical pixel, 2.34 columns a second. Playing from `position`.
void prepareWholeTrackPlayback(SpectrogramItem &item, double position);

// Whether the item waits for its next playback frame.
bool playbackFrameScheduled(const SpectrogramItem &item);

// 4000 columns at 48 kHz and a hop of 1024, 46.875 columns a second, each
// column of another colour than its neighbours.
constexpr int kStripeBins = 64;

constexpr int kStripeColumns = 4000;

constexpr double kStripeColumnsPerSecond = 48'000.0 / 1024.0;

void feedColumnStripes(SpectrogramItem &item);

// A pane on `time` playing from 40 s, rolling (mode 0) or centered and
// scrolling (mode 1).
void prepareScrollingPlayback(SpectrogramItem &item, TestTime &time, int mode, double height = 100.0);

void addScrollingRows();

// Loads Main.qml with its fallback bridge and makes it the active window,
// where its shortcuts work.
QQuickWindow *loadActiveMainWindow(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel);

// The window of a viewer over the whole screen, named `name`: made the
// first time the viewer opens there. Null until then.
QQuickWindow *viewerWindowNamed(QQmlApplicationEngine &engine, const QString &name);

// Sends a wheel event at `scenePosition`; returns whether something took it.
bool sendWheel(QQuickWindow *window, QPointF scenePosition, QPoint pixelDelta, QPoint angleDelta);

bool anyMenuVisible(QObject *root, const QString &objectName);

// Every visible item named `objectName` under `root`.
QList<QQuickItem *> visibleItemsNamed(QQuickItem *root, const QString &objectName);

// The first visible item named `objectName` under `root`.
QQuickItem *firstVisibleItemNamed(QQuickItem *root, const QString &objectName);

// Gives the fallback bridge a playing track, as the viewers' boards show it.
void setPlayingTrack(QObject *bridge);

// Saves a `width` by `height` cover image in `dir`: its path, or an empty one
// if it could not be saved.
QString savedCover(const QTemporaryDir &dir, int width = 64, int height = 64);

// The colour property `name` of `object`, as 8-bit RGBA, as it is drawn.
QRgb colorOf(QObject *object, const char *name);

// Tabs in `host` until `stop` has focus; whether it got there.
bool tabTo(QQuickWindow *host, QQuickItem *stop);

// The visible button named `objectName` in `window`, clicked as a user would.
bool clickVisibleButton(QQuickWindow *window, const QString &objectName);

// Whether `popup` fades out: closed, but visible until the style's exit
// transition ends.
bool popupFadingOut(QObject *popup);

// Clicks the visible, enabled button `objectName` as a user would, and the
// same place again as `popup`, which the first click closed, fades out: a
// quick second click. Whether both clicks were made, the second in the fade.
bool clickTwiceAsItCloses(QQuickWindow *window, const QString &objectName, QObject *popup);

// Whether the keyboard is in `popup`'s content itself, where a dialog's
// Enter presses its default button, rather than in the popup or a control.
bool keyboardInPopupContent(QObject *popup);

// Tabs in `window` until the keyboard is on a button of `popup`'s footer;
// whether it got there without leaving the popup.
bool tabToPopupFooter(QQuickWindow *window, QObject *popup);

// Whether no popup in any window is opening or closing. The app's style
// fades popups in and out: in its enter or exit transition a popup is
// visible but not opened, and a modal one that fades out still takes the
// keys and the clicks.
bool popupsSettled();

// What the tests of every area share. Each area has one or more test
// classes, which CTest runs in a process of their own; see main() in
// tst_qml_smoke.cpp.
class QmlSmokeTest : public QObject {
    Q_OBJECT

public:
    // Each test's user directories are made in `userDirectoriesParent`.
    explicit QmlSmokeTest(QString userDirectoriesParent)
        : m_userDirectoriesParent(std::move(userDirectoriesParent)) {}

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();

private:
    QString m_userDirectoriesParent;
    // The current test's own user directories; see init().
    std::optional<TestUserDirectories> m_userDirectories;
};
