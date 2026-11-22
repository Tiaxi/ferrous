// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

#include <QShortcut>

#include <gst/gst.h>

// The centered spectrogram's view following the playhead, as the waveform's
// does (tst_qml_smoke_waveform_follow.cpp): a middle-drag detaches it, as
// autoscroll after a middle click does, the
// wheel zooms about the pointer while detached, the playhead reaching 85 %
// of a detached view glides it back, and seeks, track changes, a mode
// switch and Follow Playhead bring it back at once. The lanes of a plot
// share the view, and the analysis decodes it where it is.
class SpectrogramFollowSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void middleDragDetachesAndPans();
    void middleDragPansEveryLaneOfThePlot();
    void lanesOfASurfaceShareTheirView();
    void middleDragInRollingModeOrOfTheWholeTrackDoesNothing();
    void wheelZoomsAboutThePointerWhileDetached();
    void wheelZoomsAboutThePlayheadWhileFollowing();
    void wheelZoomsAboutThePointerAtAnEndOfTheTrack();
    void farPanShowsTheOldCanvasAtItsOwnTimes();
    void detachedViewGlidesBackFromItsReattachPoint();
    void playheadPastTheReattachPointLeavesTheView();
    void ownSeekKeepsTheViewAndOthersBringItBack_data();
    void ownSeekKeepsTheViewAndOthersBringItBack();
    void followPlayheadReattachesAtOnce_data();
    void followPlayheadReattachesAtOnce();
    void movedViewKeepsWhatItShowsWhileItsColumnsCome();
    void staleChunksAfterFastPansAreDiscarded();
    void secondRefillKeepsTheColumnsTheFirstDrew();
    void refillForAWiderViewKeepsTheColumnsOfTheViewBefore();
    void gstreamerMovedViewIsDecodedAndSeeksBringItBack();
    void autoscrollMovesByThePointersDistance();
    void autoscrollMovesEveryLaneOfThePlot();
    void autoscrollInRollingModeOrOfTheWholeTrackDoesNothing();
    void autoscrollArmsTheReattachPointWhenItStops_data();
    void autoscrollArmsTheReattachPointWhenItStops();
    void autoscrollStopsAsTheViewComesBack_data();
    void autoscrollStopsAsTheViewComesBack();
    void autoscrollInAWindow();
    void crosshairStaysWholeThroughAMiddleDrag_data();
    void crosshairStaysWholeThroughAMiddleDrag();
};

namespace {

// A pane 400 px wide of a 120 s track at 48 kHz, its columns 1024 samples
// apart and a pixel each: 46.875 columns a second, so the view shows 400
// columns, 8.53 s. Every column of the track is decoded.
constexpr double kWidth = 400.0;
constexpr int kWindow = 400;
constexpr int kRate = 48'000;
constexpr int kHop = 1'024;
constexpr double kColumnsPerSecond = static_cast<double>(kRate) / kHop;
constexpr int kColumns = 5'625;
constexpr int kBins = 16;

qint64 columnAt(double seconds) {
    return static_cast<qint64>(std::floor(seconds * kColumnsPerSecond));
}

double positionOf(SpectrogramItem &item) {
    QMutexLocker lock(&item.m_stateMutex);
    return std::max(0.0, item.currentRenderPositionSecondsLocked(item.m_now()));
}

SpectrogramItem::CenteredView viewOf(SpectrogramItem &item) {
    QMutexLocker lock(&item.m_stateMutex);
    return item.centeredViewLocked(
        std::max(0.0, item.currentRenderPositionSecondsLocked(item.m_now())),
        item.canvasWidthLocked(), item.canvasHeightLocked(), item.canvasZoomLocked(),
        kColumnsPerSecond);
}

// Where a view that follows the playhead starts.
qint64 followingLeftOf(SpectrogramItem &item) {
    return std::clamp<qint64>(
        columnAt(positionOf(item)) - kWindow / 2, 0, kColumns - kWindow);
}

ViewFollow::Mode modeOf(SpectrogramItem &item) {
    QMutexLocker lock(&item.m_stateMutex);
    return item.followStateLocked(
        std::max(0.0, item.currentRenderPositionSecondsLocked(item.m_now()))).mode;
}

// Where the playhead is in the view's width, from 0 to 1.
double playheadFraction(SpectrogramItem &item) {
    return static_cast<double>(columnAt(positionOf(item)) - viewOf(item).left) / kWindow;
}

void sendMouse(SpectrogramItem &item, QEvent::Type type, double x,
               Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(x, 40.0), QPointF(x, 40.0), button, buttons, Qt::NoModifier);
    if (type == QEvent::MouseButtonPress) {
        item.mousePressEvent(&event);
    } else if (type == QEvent::MouseMove) {
        item.mouseMoveEvent(&event);
    } else {
        item.mouseReleaseEvent(&event);
    }
}

// A middle-drag from `fromX` to `toX`, in steps, as the pointer moves.
void middleDrag(SpectrogramItem &item, double fromX, double toX) {
    sendMouse(item, QEvent::MouseButtonPress, fromX, Qt::MiddleButton, Qt::MiddleButton);
    for (int step = 1; step <= 4; ++step) {
        sendMouse(item, QEvent::MouseMove, fromX + (toX - fromX) * step / 4.0,
                  Qt::NoButton, Qt::MiddleButton);
    }
    sendMouse(item, QEvent::MouseButtonRelease, toX, Qt::MiddleButton, Qt::NoButton);
}

// A middle click at `x`, which starts autoscroll, or stops it.
void middleClick(SpectrogramItem &item, double x) {
    middleDrag(item, x, x);
}

// The pointer at `x` of the item, as autoscroll follows it in the window.
void pointerAt(SpectrogramItem &item, double x) {
    item.m_autoscroll.pointerMoved(item.mapToScene(QPointF(x, 40.0)));
}

void frame(SpectrogramItem &item);

// `seconds` of the window's frames, 60 a second, as autoscroll moves the
// view with them.
void autoscrollFor(SpectrogramItem &item, TestTime &time, double seconds) {
    for (int step = 0; step < std::lround(seconds * 60.0); ++step) {
        time.advance(1.0 / 60.0);
        item.m_autoscroll.frameShown();
        frame(item);
    }
    // What the lanes are told of the view, queued.
    QCoreApplication::processEvents();
}

void wheelAt(SpectrogramItem &item, double x, int delta) {
    QWheelEvent wheel(QPointF(x, 40.0), QPointF(x, 40.0), QPoint(), QPoint(0, delta),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    item.wheelEvent(&wheel);
}

// A right-click at `x`, which seeks there; returns the time it asked for.
double rightClickAt(SpectrogramItem &item, double x) {
    QSignalSpy seeks(&item, &SpectrogramItem::seekRequested);
    sendMouse(item, QEvent::MouseButtonPress, x, Qt::RightButton, Qt::RightButton);
    return seeks.isEmpty() ? std::numeric_limits<double>::quiet_NaN()
                           : seeks.first().first().toDouble();
}

// The engine's sample of a seek that has landed at `seconds`, as
// BridgeClient takes it.
void engineLands(SpectrogramItem &item, double seconds) {
    PlaybackClock &clock = testClock(item);
    PlaybackSample sample;
    sample.positionSeconds = seconds;
    sample.sampledAtNs = clock.nowNs();
    sample.advancing = item.isPlaying();
    sample.trackToken = clock.state().trackToken;
    clock.applySample(sample, true);
}

// A frame of the window: the item looks at its view again.
void frame(SpectrogramItem &item) {
    item.handleWindowAfterAnimating();
}

// The track's columns, all of them, for track `token` of a session of
// `generation`.
void feedTrack(SpectrogramItem &item, quint64 token = 1, quint64 generation = 1) {
    item.feedPrecomputedChunk(QByteArray(kBins * kColumns, '\x50'), kBins, 0, kColumns, 0,
        kColumns, kRate, kHop, true, true, token, true, generation, 0, 2'048,
        quint64(120) * kRate, kColumns);
}

// A restart of the session for track 1, of `generation`, from column
// `start`, as the analysis sends it: the analysis thread's reset, then the
// worker's, which waits for the session's first columns. A refill of the
// centered view keeps the track's columns (src/analysis/mod.rs,
// refill_centered_spectrogram); a restart for other settings clears them.
void restartSession(SpectrogramItem &item, qint64 start, quint64 generation, bool clearsHistory) {
    item.feedPrecomputedChunk(QByteArray(), 0, 0, 0, 0, 0, 0, 0, false, true, 1, clearsHistory,
        generation);
    item.feedPrecomputedChunk(QByteArray(), kBins, 0, 0, static_cast<int>(start), kColumns, kRate,
        kHop, false, true, 1, clearsHistory, generation, 0, 2'048, quint64(120) * kRate, kColumns);
}

// `count` columns of track 1 from column `start`, of a session of
// `generation`, each `value` throughout.
void sessionColumns(SpectrogramItem &item, qint64 start, qint64 count, quint64 generation,
                    char value) {
    item.feedPrecomputedChunk(QByteArray(kBins * static_cast<int>(count), value), kBins, 0,
        static_cast<int>(count), static_cast<int>(start), kColumns, kRate, kHop, false, false, 1,
        false, generation, 0, 2'048, quint64(120) * kRate, kColumns);
}

// Empty when the ring holds the columns from `left` to `right` as the
// session that sent `value` drew them; else what it holds instead.
QString ringUnlike(SpectrogramItem &item, qint64 left, qint64 right, char value) {
    QMutexLocker lock(&item.m_stateMutex);
    for (qint64 column = left; column <= right; ++column) {
        const quint8 *data = item.ringColumnLocked(column, false);
        if (data == nullptr || data[0] != static_cast<quint8>(value)) {
            return QStringLiteral("column %1 of %2..%3 is %4").arg(column).arg(left).arg(right)
                .arg(data == nullptr ? QStringLiteral("missing")
                                     : QStringLiteral("another session's"));
        }
    }
    return QString();
}

// The item, centered and kWidth wide, plays the track from `position`, on
// the test's time.
void prepare(SpectrogramItem &item, TestTime &time, double position, bool playing = true) {
    time.drive(item);
    item.setWidth(kWidth);
    item.setHeight(100.0);
    item.setDisplayMode(1);
    item.setZoomEnabled(true);
    item.setCrosshairEnabled(true);
    feedTrack(item);
    setTestPosition(item, position, 1);
    item.setPlaying(playing);
    setTestPosition(item, position, 1);
    frame(item);
}

// The view's zoom requests reach it, as SpectrogramSurface passes them on.
void takeZoomRequests(SpectrogramItem &item) {
    QObject::connect(&item, &SpectrogramItem::zoomRequested, &item,
        [&item](double zoom) { item.setZoomLevel(zoom); });
}

// `seconds` of a stereo 48 kHz sawtooth, as a 16-bit WAV file at `path`.
bool writeWav(const QString &path, double seconds) {
    const auto frames = static_cast<quint32>(seconds * 48'000);
    QByteArray wav("RIFF", 4);
    appendLe<quint32>(wav, 36 + frames * 4);
    wav.append("WAVEfmt ", 8);
    appendLe<quint32>(wav, 16);
    appendLe<quint16>(wav, 1);
    appendLe<quint16>(wav, 2);
    appendLe<quint32>(wav, 48'000);
    appendLe<quint32>(wav, 48'000 * 4);
    appendLe<quint16>(wav, 4);
    appendLe<quint16>(wav, 16);
    wav.append("data", 4);
    appendLe<quint32>(wav, frames * 4);
    for (quint32 frame = 0; frame < frames; ++frame) {
        const qint16 sample = static_cast<qint16>((frame % 48) * 512);
        appendLe<qint16>(wav, sample);
        appendLe<qint16>(wav, -sample);
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(wav) == wav.size();
}

} // namespace

// A middle-drag pans a zoomed centered view, which then stays where it was
// put while playback goes on; the column grabbed stays under the pointer.
void SpectrogramFollowSmokeTest::middleDragDetachesAndPans() {
    SpectrogramItem item;
    TestTime time;
    prepare(item, time, 60.0);
    QSignalSpy detachedChanges(&item, &SpectrogramItem::detachedChanged);
    const qint64 following = viewOf(item).left;
    QCOMPARE(following, columnAt(60.0) - kWindow / 2);
    QVERIFY(viewOf(item).scrolls);

    // A click, or a move short of a drag, pans nothing: it starts
    // autoscroll, which holds the view where it is until the pointer moves.
    for (const double to : {200.0, 201.0}) {
        middleDrag(item, 200.0, to);
        QVERIFY(item.m_autoscroll.active());
        QVERIFY(item.detached());
        QCOMPARE(viewOf(item).left, following);
        item.followPlayhead();
        QCoreApplication::processEvents();
        QVERIFY(!item.m_autoscroll.active());
    }
    QVERIFY(!item.detached());
    QCOMPARE(viewOf(item).left, following);
    QCOMPARE(detachedChanges.count(), 4);
    detachedChanges.clear();

    // 200 px to the left: the view moves on by 200 columns, and stays. A
    // drag starts no autoscroll.
    middleDrag(item, 300.0, 100.0);
    QVERIFY(item.detached());
    QVERIFY(!item.m_autoscroll.active());
    QCOMPARE(detachedChanges.count(), 1);
    QCOMPARE(viewOf(item).left, following + 200);
    QVERIFY(!viewOf(item).scrolls);
    time.advance(1.0);
    frame(item);
    QVERIFY(item.detached());
    QCOMPARE(viewOf(item).left, following + 200);
    // The seek bar's band marks the view where it was put.
    QVERIFY(std::abs(item.visibleStartSeconds()
                     - static_cast<double>(following + 200) / kColumnsPerSecond) < 1e-9);
    QVERIFY(std::abs(item.visibleEndSeconds()
                     - static_cast<double>(following + 200 + kWindow) / kColumnsPerSecond) < 1e-9);
    // Back by 300 px.
    middleDrag(item, 100.0, 400.0);
    QCOMPARE(viewOf(item).left, following - 100);
    QCOMPARE(detachedChanges.count(), 1);

    // Held at the start of the track, a move back moves the view at once.
    SpectrogramItem early;
    TestTime earlyTime;
    prepare(early, earlyTime, 6.0);
    sendMouse(early, QEvent::MouseButtonPress, 100.0, Qt::MiddleButton, Qt::MiddleButton);
    sendMouse(early, QEvent::MouseMove, 300.0, Qt::NoButton, Qt::MiddleButton);
    QCOMPARE(viewOf(early).left, qint64(0));
    sendMouse(early, QEvent::MouseMove, 290.0, Qt::NoButton, Qt::MiddleButton);
    QCOMPARE(viewOf(early).left, qint64(10));
    sendMouse(early, QEvent::MouseButtonRelease, 290.0, Qt::MiddleButton, Qt::NoButton);
    // And at its end.
    SpectrogramItem late;
    TestTime lateTime;
    prepare(late, lateTime, 118.0, false);
    QCOMPARE(viewOf(late).left, qint64(kColumns - kWindow));
    sendMouse(late, QEvent::MouseButtonPress, 300.0, Qt::MiddleButton, Qt::MiddleButton);
    sendMouse(late, QEvent::MouseMove, 100.0, Qt::NoButton, Qt::MiddleButton);
    QCOMPARE(viewOf(late).left, qint64(kColumns - kWindow));
    sendMouse(late, QEvent::MouseMove, 110.0, Qt::NoButton, Qt::MiddleButton);
    QCOMPARE(viewOf(late).left, qint64(kColumns - kWindow - 10));
    sendMouse(late, QEvent::MouseButtonRelease, 110.0, Qt::MiddleButton, Qt::NoButton);
    QVERIFY(late.detached());
}

// The lanes of a plot, a channel each, show one view: a drag on one moves
// all of them, and Follow Playhead on any brings all of them back.
void SpectrogramFollowSmokeTest::middleDragPansEveryLaneOfThePlot() {
    SpectrogramViewFollow follow;
    SpectrogramItem left;
    SpectrogramItem right;
    TestTime time;
    left.setViewFollow(&follow);
    right.setViewFollow(&follow);
    prepare(left, time, 60.0);
    prepare(right, time, 60.0);
    middleDrag(right, 300.0, 100.0);
    QVERIFY(left.detached() && right.detached());
    QCOMPARE(viewOf(left).left, viewOf(right).left);
    QCOMPARE(viewOf(left).left, columnAt(60.0) - kWindow / 2 + 200);
    // A seek from elsewhere, which each lane hears, is taken once.
    testClock(left).holdAt(20.0);
    testClock(right).holdAt(20.0);
    QVERIFY(!left.detached() && !right.detached());
    middleDrag(left, 100.0, 300.0);
    QVERIFY(right.detached());
    left.followPlayhead();
    QVERIFY(!left.detached() && !right.detached());
    QCOMPARE(viewOf(right).left, followingLeftOf(right));
    // A lane given no view of the plot's has one of its own.
    right.setViewFollow(nullptr);
    QVERIFY(right.viewFollow() != nullptr && right.viewFollow() != &follow);
}

// The lanes SpectrogramSurface makes, a channel each, share one view, which
// the surface tells detached of; Reset Zoom brings it back to the playhead.
void SpectrogramFollowSmokeTest::lanesOfASurfaceShareTheirView() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<SpectrogramViewFollow>("FerrousUi", 1, 0, "SpectrogramViewFollow");
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Window 2.15
import "components" as Components
import "viewers" as Viewers

Window {
    id: paletteRoot
    width: 600
    height: 240
    visible: true

    QtObject {
        id: bridge
        property string playbackState: "Paused"
        property int spectrogramViewMode: 1
        property int spectrogramDisplayMode: 1
        property real dbRange: 90
        property bool logScale: false
        property bool spectrogramFrequencyCap: false
        property bool showFps: false
        property int sampleRateHz: 48000
        property bool showSpectrogramCrosshair: true
        property bool showSpectrogramScale: false
        property bool spectrogramZoomEnabled: true
        property bool stretchShortTracks: true
        property int channelButtonsVisibility: 1
        property var mutedChannels: []
        property var soloedChannels: []
        property bool showLevelMeter: false
        property string currentTrackPath: "/fixture/track.flac"
        property bool waveformEndsEarly: false
        property real waveformCoverageSeconds: 0
        property var currentTrackChannelLabels: []
        property bool connected: true
        property real durationSeconds: 0
        property int currentTrackChannels: 2
        signal precomputedSpectrogramChannelsReady(int channelCount, bool bufferReset)
        signal playbackChanged()
        function isChannelSilent(channelIndex) { return false }
        function setSpectrogramZoomLevel(level) {}
        function registerSpectrogramItem(item, channelIndex) {}
        function unregisterSpectrogramItem(item) {}
        function setSpectrogramWidgetWidth(width) {}
        function toggleChannelMute(channelIndex) {}
        function soloChannel(channelIndex) {}
    }

    Components.UiPalette { id: palette; windowRoot: paletteRoot }

    Viewers.SpectrogramSurface {
        objectName: "followSpectrogramSurface"
        anchors.fill: parent
        uiBridge: bridge
        uiPalette: palette
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *surface = qobject_cast<QQuickItem *>(
        root->findChild<QObject *>(QStringLiteral("followSpectrogramSurface")));
    QVERIFY(surface != nullptr);
    QVariantList descriptors;
    for (int i = 0; i < 2; ++i) {
        descriptors.push_back(QVariantMap{
            {QStringLiteral("label"), i == 0 ? QStringLiteral("L") : QStringLiteral("R")},
            {QStringLiteral("showLabel"), true},
            {QStringLiteral("muted"), false},
            {QStringLiteral("channelIndex"), i},
        });
    }
    surface->setProperty("channelDescriptors", descriptors);
    QList<SpectrogramItem *> lanes;
    QTRY_VERIFY([&]() {
        lanes.clear();
        collectQuickItems(surface, &lanes);
        return lanes.size() == 2;
    }());
    SpectrogramViewFollow *shared = lanes[0]->viewFollow();
    QVERIFY(shared != nullptr);
    QCOMPARE(lanes[1]->viewFollow(), shared);
    QVERIFY(!surface->property("detached").toBool());
    shared->detach(30.0, 0.5);
    QVERIFY(lanes[0]->detached() && lanes[1]->detached());
    QVERIFY(surface->property("detached").toBool());
    QVERIFY(QMetaObject::invokeMethod(surface, "followPlayhead"));
    QVERIFY(!surface->property("detached").toBool());
    shared->detach(30.0, 0.5);
    QVERIFY(QMetaObject::invokeMethod(surface, "resetZoom"));
    QVERIFY(!surface->property("detached").toBool() && !lanes[1]->detached());
}

// A rolling view stays live, and a view of the whole track has nowhere to
// pan to.
void SpectrogramFollowSmokeTest::middleDragInRollingModeOrOfTheWholeTrackDoesNothing() {
    SpectrogramItem rolling;
    TestTime time;
    prepare(rolling, time, 60.0);
    rolling.setDisplayMode(0);
    middleDrag(rolling, 300.0, 100.0);
    QVERIFY(!rolling.detached());

    // 5,200 of the 5,625 columns: the whole track shows.
    SpectrogramItem whole;
    TestTime wholeTime;
    prepare(whole, wholeTime, 60.0);
    whole.setWidth(5'200.0);
    QCOMPARE(viewOf(whole).left, qint64(0));
    middleDrag(whole, 3'000.0, 1'000.0);
    QVERIFY(!whole.detached());
    QCOMPARE(viewOf(whole).left, qint64(0));
}

// Detached, the wheel zooms about the pointer: the time under it stays
// there, in the view the zoom asks for, and the view stays detached.
void SpectrogramFollowSmokeTest::wheelZoomsAboutThePointerWhileDetached() {
    SpectrogramItem item;
    TestTime time;
    prepare(item, time, 60.0);
    takeZoomRequests(item);
    middleDrag(item, 300.0, 100.0);
    QVERIFY(item.detached());
    const double pointer = 100.0;
    double before = 0.0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        before = item.viewSecondsAtLocked(pointer);
    }
    QVERIFY(std::abs(before - (static_cast<double>(viewOf(item).left) + pointer)
                / kColumnsPerSecond) < 1e-9);
    for (const int delta : {240, -120, 360}) {
        wheelAt(item, pointer, delta);
        QVERIFY(item.detached());
        const SpectrogramViewFollow::Snapshot follow = item.viewFollow()->snapshot();
        QVERIFY(std::abs(follow.anchorSeconds - before) < 1e-9);
        QCOMPARE(follow.anchorFraction, pointer / kWidth);
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_awaitingZoomData);
        // The view the zoom asks for has the time at the pointer, to within
        // a column of its own.
        const auto target = item.centeredZoomTargetLocked(
            item.canvasWidthLocked(), 60.0, kColumnsPerSecond);
        QVERIFY(target.valid);
        const double at = (static_cast<double>(target.range.left)
                              + pointer * item.m_canvasScale / target.canvasZoom)
            / target.columnsPerSecond;
        QVERIFY2(std::abs(at - before) <= 1.0 / target.columnsPerSecond,
            qPrintable(QStringLiteral("%1 for %2").arg(at).arg(before)));
        // The preview scales about it.
        QCOMPARE(item.zoomPreviewPivotSecondsLocked(60.0, false), before);
        // The time under the pointer as it shows stays.
        QVERIFY(std::abs(item.viewSecondsAtLocked(pointer) - before)
            <= 1.0 / target.columnsPerSecond);
    }
}

// Following, the wheel zooms about the playhead, and the view goes on
// following it.
void SpectrogramFollowSmokeTest::wheelZoomsAboutThePlayheadWhileFollowing() {
    SpectrogramItem item;
    TestTime time;
    prepare(item, time, 60.0);
    takeZoomRequests(item);
    QSignalSpy detachedChanges(&item, &SpectrogramItem::detachedChanged);
    wheelAt(item, 50.0, 240);
    QVERIFY(!item.detached());
    QCOMPARE(detachedChanges.count(), 0);
    QCOMPARE(modeOf(item), ViewFollow::Mode::Playhead);
    QMutexLocker lock(&item.m_stateMutex);
    QVERIFY(item.m_zoomLevel > 1.0);
    const double position = item.currentRenderPositionSecondsLocked(item.m_now());
    QCOMPARE(item.zoomPreviewPivotSecondsLocked(position, false), position);
    const auto target = item.centeredZoomTargetLocked(
        item.canvasWidthLocked(), position, kColumnsPerSecond);
    QVERIFY(target.valid);
    QCOMPARE(target.range.left, target.playheadColumn - target.windowColumns / 2);
}

// A detached view held at an end of the track is not where its anchor would
// put it: the wheel zooms about the time the view shows at the pointer.
void SpectrogramFollowSmokeTest::wheelZoomsAboutThePointerAtAnEndOfTheTrack() {
    SpectrogramItem item;
    TestTime time;
    prepare(item, time, 100.0, false);
    takeZoomRequests(item);
    // To the end of the track, and past it, where the view is held.
    middleDrag(item, 400.0, -1'000.0);
    QVERIFY(item.detached());
    QCOMPARE(viewOf(item).left, qint64(kColumns - kWindow));
    const double pointer = 300.0;
    for (const int delta : {-240, 240, 240}) {
        double shown = 0.0;
        double columnSeconds = 0.0;
        {
            QMutexLocker lock(&item.m_stateMutex);
            shown = item.viewSecondsAtLocked(pointer);
            columnSeconds =
                1.0 / item.requestedViewLocked(item.currentRenderPositionSecondsLocked(item.m_now()))
                          .columnsPerSecond;
        }
        wheelAt(item, pointer, delta);
        QVERIFY(item.detached());
        const SpectrogramViewFollow::Snapshot follow = item.viewFollow()->snapshot();
        QVERIFY2(std::abs(follow.anchorSeconds - shown) <= columnSeconds,
            qPrintable(QStringLiteral("%1 for %2").arg(follow.anchorSeconds).arg(shown)));
        QCOMPARE(follow.anchorFraction, pointer / kWidth);
    }
}

// A pan by more than half the view, whose canvas the next rasterization
// replaces, shows the canvas it has at its own times meanwhile, mostly off
// the view, rather than at the times of the view it moved to.
void SpectrogramFollowSmokeTest::farPanShowsTheOldCanvasAtItsOwnTimes() {
    QQuickWindow window;
    window.resize(400, 100);
    presentWithoutDrawing(window);
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    TestTime time;
    prepare(item, time, 60.0);
    QSGNode *node = paintSettled(item, nullptr);
    const qint64 canvasLeft = viewOf(item).left;
    middleDrag(item, 350.0, 50.0);
    const qint64 left = viewOf(item).left;
    QCOMPARE(left, canvasLeft + 300);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, canvasLeft);
        const SpectrogramZoomPreview alignment = item.staleCanvasAlignmentLocked(
            0.0, kWindow, kWidth, 0.0, left, false, item.canvasZoomLocked(), kColumnsPerSecond);
        QVERIFY(alignment.active);
        QCOMPARE(alignment.map(0.0), -300.0);
    }
    // Following, a jump keeps the canvas where it is until it is replaced.
    item.followPlayhead();
    {
        QMutexLocker lock(&item.m_stateMutex);
        const SpectrogramZoomPreview alignment = item.staleCanvasAlignmentLocked(
            0.0, kWindow, kWidth, 0.0, left, false, item.canvasZoomLocked(), kColumnsPerSecond);
        QVERIFY(!alignment.active);
    }
    delete node;
}

// The playhead reaching 85 % of a detached view glides it back, over 2 s
// of playback worked out from the playhead's position, to the middle,
// where the view follows it again. The playhead stays in the view on every
// frame; frames are due on every refresh only while it glides.
void SpectrogramFollowSmokeTest::detachedViewGlidesBackFromItsReattachPoint() {
    QQuickWindow window;
    window.resize(400, 100);
    presentWithoutDrawing(window);
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    TestTime time;
    prepare(item, time, 60.0);
    QSGNode *node = paintSettled(item, nullptr);
    // Back by 100 columns: the playhead is at 75 % of the view.
    middleDrag(item, 100.0, 200.0);
    QVERIFY(item.detached());
    const qint64 put = viewOf(item).left;
    QCOMPARE(put, columnAt(60.0) - kWindow / 2 - 100);
    QVERIFY(std::abs(playheadFraction(item) - 0.75) < 0.01);
    const double reattach = (static_cast<double>(put) + 0.85 * kWindow) / kColumnsPerSecond;
    QVERIFY(std::abs(item.viewFollow()->snapshot().reattachSeconds - reattach) < 1e-9);

    // Held still, the next frame is due when the playhead reaches its next
    // pixel, or the re-attach point.
    node = paintSettled(item, node);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_idleView.valid);
        QVERIFY(item.m_idleView.changeSeconds <= reattach + 1.0 / kColumnsPerSecond);
    }

    bool glided = false;
    bool sawFrameDueEveryRefresh = false;
    double previousLeft = static_cast<double>(put);
    for (int step = 0; step < 240 * 5 && !(glided && modeOf(item) == ViewFollow::Mode::Playhead);
         ++step) {
        time.advance(1.0 / 240.0);
        frame(item);
        const double position = positionOf(item);
        const SpectrogramItem::CenteredView view = viewOf(item);
        const double fraction = playheadFraction(item);
        QVERIFY2(fraction >= 0.0 && fraction < 1.0,
            qPrintable(QStringLiteral("%1 at %2 s").arg(fraction).arg(position)));
        // The view never goes back.
        QVERIFY(static_cast<double>(view.left) >= previousLeft);
        previousLeft = static_cast<double>(view.left);
        if (position < reattach) {
            QCOMPARE(view.left, put);
            QVERIFY(item.detached());
        } else if (position < reattach + ViewFollow::kReattachGlideSeconds) {
            glided = true;
            QCOMPARE(modeOf(item), ViewFollow::Mode::Gliding);
            // Gliding is not detached: nothing to follow.
            QVERIFY(!item.detached());
            if (!sawFrameDueEveryRefresh) {
                node = item.updatePaintNode(node, nullptr);
                QMutexLocker lock(&item.m_stateMutex);
                sawFrameDueEveryRefresh = !item.m_idleView.valid;
            }
        }
    }
    QVERIFY(glided);
    QVERIFY(sawFrameDueEveryRefresh);
    QCOMPARE(modeOf(item), ViewFollow::Mode::Playhead);
    QCOMPARE(item.viewFollow()->snapshot().state.mode, ViewFollow::Mode::Playhead);
    QCOMPARE(viewOf(item).left, followingLeftOf(item));
    QVERIFY(std::abs(playheadFraction(item) - 0.5) <= 1.0 / kWindow);
    // Following again, frames wait for the scroll's next step.
    node = paintSettled(item, node);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_idleView.valid);
    }
    delete node;
}

// A playhead past the right edge, or past 85 % of the view when the drag
// ends, leaves the view where it was put; the plot's own seek to the left of
// that point lets it glide back again.
void SpectrogramFollowSmokeTest::playheadPastTheReattachPointLeavesTheView() {
    for (const double drag : {300.0, 160.0}) {
        SpectrogramItem item;
        TestTime time;
        prepare(item, time, 60.0);
        middleDrag(item, 0.0, drag);
        QVERIFY(item.detached());
        const qint64 put = viewOf(item).left;
        QVERIFY(playheadFraction(item) > ViewFollow::kReattachViewFraction);
        QVERIFY(std::isinf(item.viewFollow()->snapshot().reattachSeconds));
        for (int step = 0; step < 5 * 60; ++step) {
            time.advance(1.0 / 60.0);
            frame(item);
        }
        QVERIFY(item.detached());
        QCOMPARE(viewOf(item).left, put);
        // The playhead past the view is not drawn at its edge.
        if (drag == 300.0) {
            QVERIFY(playheadFraction(item) >= 1.0);
        }

        // A right-click seek near the view's start: it plays through the
        // view and glides back.
        PlaybackClock &clock = testClock(item);
        const double target = rightClickAt(item, 40.0);
        QVERIFY(std::abs(target - (static_cast<double>(put) + 40.0) / kColumnsPerSecond)
            < 1.0 / kColumnsPerSecond);
        clock.holdAt(target);
        engineLands(item, target);
        frame(item);
        QVERIFY(item.detached());
        QCOMPARE(viewOf(item).left, put);
        QVERIFY(std::isfinite(item.viewFollow()->snapshot().reattachSeconds));
        for (int step = 0; step < 10 * 60 && modeOf(item) != ViewFollow::Mode::Playhead;
             ++step) {
            time.advance(1.0 / 60.0);
            frame(item);
        }
        QCOMPARE(modeOf(item), ViewFollow::Mode::Playhead);
        QVERIFY(!item.detached());
    }
}

void SpectrogramFollowSmokeTest::ownSeekKeepsTheViewAndOthersBringItBack_data() {
    QTest::addColumn<QString>("event");
    QTest::addColumn<bool>("keeps");
    QTest::addRow("own seek") << QStringLiteral("own seek") << true;
    QTest::addRow("own seek, paused") << QStringLiteral("own seek, paused") << true;
    QTest::addRow("seek bar") << QStringLiteral("seek bar") << false;
    QTest::addRow("later seek back to the own seek's time")
        << QStringLiteral("later seek back") << false;
    QTest::addRow("stop") << QStringLiteral("stop") << false;
    QTest::addRow("gapless successor") << QStringLiteral("gapless successor") << false;
    QTest::addRow("repeat one") << QStringLiteral("repeat one") << false;
    QTest::addRow("switch to rolling") << QStringLiteral("switch to rolling") << false;
}

// A right-click seek in a detached view keeps it, as it lands and as the
// engine confirms it; any other seek, a stop, a track change, including a
// gapless successor and Repeat-One, and a switch to rolling bring the view
// back to the playhead.
void SpectrogramFollowSmokeTest::ownSeekKeepsTheViewAndOthersBringItBack() {
    QFETCH(QString, event);
    QFETCH(bool, keeps);
    SpectrogramItem item;
    TestTime time;
    prepare(item, time, 60.0, !event.endsWith(QStringLiteral("paused")));
    // Behind the playhead, so that nothing glides back.
    middleDrag(item, 0.0, kWidth);
    QVERIFY(item.detached());
    const qint64 put = viewOf(item).left;
    PlaybackClock &clock = testClock(item);
    const quint64 token = clock.state().trackToken;
    if (event.startsWith(QStringLiteral("own seek"))) {
        const double target = rightClickAt(item, 100.0);
        clock.holdAt(target);
        QVERIFY(item.detached());
        engineLands(item, target + 0.04);
    } else if (event == QStringLiteral("seek bar")) {
        clock.holdAt(10.0);
        engineLands(item, 10.03);
    } else if (event == QStringLiteral("later seek back")) {
        const double target = rightClickAt(item, 100.0);
        clock.holdAt(target);
        engineLands(item, target);
        time.advance(1.0);
        frame(item);
        QVERIFY(item.detached());
        // Shift+Left, 5 s after, lands where the right-click did.
        clock.holdAt(target);
    } else if (event == QStringLiteral("stop")) {
        item.setPlaying(false);
        PlaybackSample stopped;
        stopped.trackToken = token;
        clock.applySample(stopped, true);
    } else if (event == QStringLiteral("gapless successor")
               || event == QStringLiteral("repeat one")) {
        // A gapless successor and Repeat-One start without a seek; only the
        // track token tells them apart from playback going on.
        PlaybackSample next;
        next.positionSeconds = 0.0;
        next.sampledAtNs = clock.nowNs();
        next.advancing = true;
        next.trackToken = token + 1;
        clock.applySample(next, false);
    } else {
        item.setDisplayMode(0);
        item.setDisplayMode(1);
    }
    frame(item);
    QCOMPARE(item.detached(), keeps);
    if (keeps) {
        QCOMPARE(viewOf(item).left, put);
    } else {
        QCOMPARE(modeOf(item), ViewFollow::Mode::Playhead);
        QCOMPARE(viewOf(item).left, followingLeftOf(item));
    }
}

void SpectrogramFollowSmokeTest::followPlayheadReattachesAtOnce_data() {
    QTest::addColumn<bool>("playing");
    QTest::addRow("playing") << true;
    QTest::addRow("paused") << false;
}

// Follow Playhead brings a detached view back to the playhead at once,
// playing or paused; following, it does nothing.
void SpectrogramFollowSmokeTest::followPlayheadReattachesAtOnce() {
    QFETCH(bool, playing);
    SpectrogramItem item;
    TestTime time;
    prepare(item, time, 60.0, playing);
    QSignalSpy detachedChanges(&item, &SpectrogramItem::detachedChanged);
    const qint64 following = viewOf(item).left;
    item.followPlayhead();
    QCOMPARE(detachedChanges.count(), 0);
    QCOMPARE(viewOf(item).left, following);

    middleDrag(item, 0.0, 300.0);
    QVERIFY(item.detached());
    QVERIFY(viewOf(item).left != following);
    item.followPlayhead();
    QVERIFY(!item.detached());
    QCOMPARE(detachedChanges.count(), 2);
    QCOMPARE(viewOf(item).left, followingLeftOf(item));
    QVERIFY(std::abs(playheadFraction(item) - 0.5) <= 1.0 / kWindow);
}

// While the analysis decodes a view the user moved, the canvas stays under
// the columns as they come, at the times of its own, and is not frozen: no
// flash of the floor, and what it showed of the view stays until its
// columns arrive. A view that follows the playhead goes on as after a seek.
void SpectrogramFollowSmokeTest::movedViewKeepsWhatItShowsWhileItsColumnsCome() {
    QQuickWindow window;
    window.resize(400, 100);
    presentWithoutDrawing(window);
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    TestTime time;
    prepare(item, time, 60.0);
    QSGNode *node = paintSettled(item, nullptr);
    QVERIFY(!item.m_canvas.isNull());
    // Moved by half a view.
    middleDrag(item, 300.0, 100.0);
    node = paintSettled(item, node);
    const qint64 left = viewOf(item).left;

    // The analysis restarts its session around the view: the ring is
    // cleared at once, and the session's reset waits for its first columns.
    item.feedPrecomputedChunk(QByteArray(), 0, 0, 0, 0, 0, 0, 0, false, true, 1, true, 0);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_canvas.isNull());
        QVERIFY(item.m_precomputedReady);
        QVERIFY(!item.m_zoomFillActive);
        QVERIFY(!item.m_seekFillActive);
    }
    const int start = static_cast<int>(left) - 94;
    item.feedPrecomputedChunk(QByteArray(), kBins, 0, 0, start, kColumns, kRate, kHop, false,
        true, 1, true, 2, 0, 2'048, quint64(120) * kRate, kColumns);
    // The view's middle column comes first.
    const int middle = static_cast<int>(left) + kWindow / 2;
    item.feedPrecomputedChunk(QByteArray(kBins, '\x70'), kBins, 0, 1, middle, kColumns, kRate,
        kHop, false, false, 1, false, 2, 0, 2'048, quint64(120) * kRate, kColumns);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_canvas.isNull());
        QVERIFY(item.ringSlotForDisplayIndexLocked(middle, false) >= 0);
        QVERIFY(item.ringSlotForDisplayIndexLocked(middle - 1, false) < 0);
    }
    // Drawn again with the column it has, the view keeps the canvas it
    // showed under the columns still to come.
    node = paintSettled(item, node);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_backdrop.isNull());
        QCOMPARE(item.m_backdrop.trackToken, quint64(1));
        QVERIFY(item.m_precomputedCanvasDisplayLeft == left);
    }
    QVERIFY(item.detached());
    QCOMPARE(viewOf(item).left, left);

    // Following, a seek's refill leaves the old picture frozen until the
    // playhead's column arrives, as before.
    item.followPlayhead();
    feedTrack(item, 1, 3);
    node = paintSettled(item, node);
    item.feedPrecomputedChunk(QByteArray(), 0, 0, 0, 0, 0, 0, 0, false, true, 1, true, 0);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_zoomFillActive);
        QVERIFY(item.m_seekFillActive);
    }
    delete node;
}

// A drag moves the view on faster than the analysis decodes it: a session
// started for a view passed since is superseded, and its columns, arriving
// late, never land in the view where the drag ended.
void SpectrogramFollowSmokeTest::staleChunksAfterFastPansAreDiscarded() {
    SpectrogramItem item;
    TestTime time;
    prepare(item, time, 60.0);
    middleDrag(item, 300.0, 100.0);
    // A session for the view at 80 s, then one for the view at 90 s.
    restartSession(item, columnAt(80.0) - 200, 2, false);
    restartSession(item, columnAt(90.0) - 200, 3, false);
    // The newer one's columns, then the older one's, late.
    sessionColumns(item, columnAt(90.0) - 200, 400, 3, '\x30');
    sessionColumns(item, columnAt(80.0) - 200, 400, 2, '\x60');
    // Its own, after them, at the same place as the newer one's.
    sessionColumns(item, columnAt(90.0) - 200, 10, 2, '\x60');
    QCOMPARE(ringUnlike(item, columnAt(90.0) - 200, columnAt(90.0) + 199, '\x30'), QString());
    QMutexLocker lock(&item.m_stateMutex);
    QVERIFY(item.ringColumnLocked(columnAt(80.0), false) == nullptr);
}

// Two refills in quick succession, as two quick pans make: the columns the
// first session drew, which the second session's window covers, stay shown
// until the second session's columns replace them, rather than going
// missing until it decodes them again. A restart that clears the history
// still clears them, also when the refill comes before its data, and so
// does a refill in a rolling view; a view that follows the playhead still
// holds its picture until the playhead's columns come.
void SpectrogramFollowSmokeTest::secondRefillKeepsTheColumnsTheFirstDrew() {
    QQuickWindow window;
    window.resize(400, 100);
    presentWithoutDrawing(window);
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    TestTime time;
    prepare(item, time, 60.0);
    QSGNode *node = paintSettled(item, nullptr);

    // A pan far past the session's window: a session around the view,
    // whose columns all arrive, the view's middle first.
    middleDrag(item, 350.0, 50.0);
    const SpectrogramItem::CenteredView first = viewOf(item);
    const qint64 firstStart = first.left - 94;
    restartSession(item, firstStart, 2, false);
    const qint64 middle = (first.left + first.right) / 2;
    sessionColumns(item, middle, first.right + 200 - middle, 2, '\x70');
    sessionColumns(item, firstStart, middle - firstStart, 2, '\x70');
    node = paintSettled(item, node);
    QCOMPARE(ringUnlike(item, first.left, first.right, '\x70'), QString());

    // A quick pan back by an eighth of the view, which the analysis
    // refills too, before its session has drawn anything.
    middleDrag(item, 100.0, 150.0);
    const SpectrogramItem::CenteredView second = viewOf(item);
    QCOMPARE(second.left, first.left - 50);
    QVERIFY(second.left >= firstStart);
    const qint64 secondStart = second.left - 94;
    restartSession(item, secondStart, 3, false);
    QCOMPARE(ringUnlike(item, second.left, second.right, '\x70'), QString());

    // Its first columns, the view's middle, replace the first session's
    // there; the rest of the view stays as the first session drew it.
    const qint64 secondMiddle = (second.left + second.right) / 2;
    sessionColumns(item, secondMiddle, 8, 3, '\x30');
    QCOMPARE(ringUnlike(item, secondMiddle, secondMiddle + 7, '\x30'), QString());
    QCOMPARE(ringUnlike(item, second.left, secondMiddle - 1, '\x70'), QString());
    QCOMPARE(ringUnlike(item, secondMiddle + 8, second.right, '\x70'), QString());
    node = paintSettled(item, node);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_canvasComplete);
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, second.left);
        // Only what the session has room for: none left of its first.
        QVERIFY(item.ringColumnLocked(secondStart - 1, false) == nullptr);
    }
    // The session goes on and replaces the rest.
    sessionColumns(item, secondMiddle + 8, second.right - secondMiddle - 7, 3, '\x30');
    sessionColumns(item, secondStart, secondMiddle - secondStart, 3, '\x30');
    QCOMPARE(ringUnlike(item, second.left, second.right, '\x30'), QString());

    // A restart for other settings, as a switch from the channels to their
    // downmix makes, clears the history with the worker's reset; a refill
    // before its data comes does not bring the columns before back.
    item.feedPrecomputedChunk(QByteArray(), kBins, 0, 0, static_cast<int>(secondStart),
        kColumns, kRate, kHop, false, true, 1, true, 4, 0, 2'048, quint64(120) * kRate,
        kColumns);
    restartSession(item, secondStart, 5, false);
    sessionColumns(item, secondMiddle, 8, 5, '\x20');
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.ringColumnLocked(secondMiddle - 1, false) == nullptr);
        QVERIFY(item.ringColumnLocked(secondMiddle + 8, false) == nullptr);
    }
    QCOMPARE(ringUnlike(item, secondMiddle, secondMiddle + 7, '\x20'), QString());

    // A refill from left of the column the ring is numbered from numbers it
    // from a capacity before: the columns stay in their slots, and the
    // session's own land in theirs.
    qint64 capacity = 0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_ringColumnBase.value_or(-1), secondStart);
        capacity = item.m_ringCapacity;
        QVERIFY(secondMiddle + 8 - (secondStart - 10) < capacity);
    }
    restartSession(item, secondStart - 10, 6, false);
    sessionColumns(item, secondStart - 10, 10, 6, '\x40');
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_ringColumnBase.value_or(-1), secondStart - capacity);
    }
    QCOMPARE(ringUnlike(item, secondStart - 10, secondStart - 1, '\x40'), QString());
    QCOMPARE(ringUnlike(item, secondMiddle, secondMiddle + 7, '\x20'), QString());

    // A switch from the downmix to the channels, whose restart a refill
    // superseded before its reset came: the refill's columns, of another
    // layout, replace the ring's rather than join them.
    QCOMPARE(ringUnlike(item, secondStart - 10, secondStart - 1, '\x40'), QString());
    restartSession(item, secondStart - 10, 7, false);
    item.feedPrecomputedChunk(QByteArray(kBins * 2 * 8, '\x60'), kBins, 0, 8,
        static_cast<int>(secondMiddle), kColumns, kRate, kHop, false, false, 1, false, 7, 0,
        2'048, quint64(120) * kRate, kColumns);
    QCOMPARE(ringUnlike(item, secondMiddle, secondMiddle + 7, '\x60'), QString());
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.ringColumnLocked(secondStart - 10, false) == nullptr);
    }

    // A refill whose session cannot read its file ends with a reset that
    // clears the ring the refill kept.
    item.feedPrecomputedChunk(QByteArray(), 0, 0, 0, 0, 0, 0, 0, false, true, 1, false, 8);
    item.feedPrecomputedChunk(QByteArray(), 0, 0, 0, 0, 0, 0, 0, false, true, 1, true, 8);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_ringCapacity, 0);
    }

    // A view that follows the playhead, sought to where the ring has no
    // columns, holds its picture after the refill until the playhead's
    // columns come, as after any seek.
    item.followPlayhead();
    restartSession(item, columnAt(60.0) - 300, 9, true);
    sessionColumns(item, columnAt(60.0) - 300, 600, 9, '\x50');
    node = paintSettled(item, node);
    setTestPosition(item, 100.0, 1);
    restartSession(item, columnAt(100.0) - 200, 10, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_zoomFillActive);
        QVERIFY(item.m_seekFillActive);
    }
    sessionColumns(item, columnAt(100.0), 8, 10, '\x30');
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_zoomFillActive);
        QVERIFY(!item.m_seekFillActive);
    }

    // A view that turns rolling between a refill's resets places the
    // refill's columns where their column puts them, as the worker made
    // them for the centered view.
    item.feedPrecomputedChunk(QByteArray(), 0, 0, 0, 0, 0, 0, 0, false, true, 1, false, 11);
    item.setDisplayMode(0);
    item.feedPrecomputedChunk(QByteArray(), kBins, 0, 0, static_cast<int>(columnAt(100.0) - 200),
        kColumns, kRate, kHop, false, true, 1, false, 11, 0, 2'048, quint64(120) * kRate,
        kColumns);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_precomputedResetPending);
        QVERIFY(item.m_precomputedPendingResetByColumn);
    }
    // So too when the view is rolling already as the refill comes, also
    // after a reset that cleared the ring while it waited for its session.
    item.feedPrecomputedChunk(QByteArray(), 0, 0, 0, 0, 0, 0, 0, false, true, 1, true, 12);
    item.feedPrecomputedChunk(QByteArray(), 0, 0, 0, 0, 0, 0, 0, false, true, 1, false, 12);
    item.feedPrecomputedChunk(QByteArray(), kBins, 0, 0, static_cast<int>(columnAt(100.0) - 200),
        kColumns, kRate, kHop, false, true, 1, false, 12, 0, 2'048, quint64(120) * kRate,
        kColumns);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_precomputedResetPending);
        QVERIFY(item.m_precomputedPendingResetByColumn);
    }

    // A rolling view takes its columns in the order they come: a refill
    // that reaches it there clears them.
    feedTrack(item, 1, 13);
    restartSession(item, columnAt(80.0) - 200, 14, false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_ringCapacity, 0);
    }
    delete node;
}

// A refill for a view that has grown, before any column of the session
// before it grew the ring: the new session, decoding for the wider view,
// starts further left, and the ring has room for it and for the columns
// of the view before, which stay.
void SpectrogramFollowSmokeTest::refillForAWiderViewKeepsTheColumnsOfTheViewBefore() {
    SpectrogramItem item;
    TestTime time;
    prepare(item, time, 60.0);
    // A longer track, of which a session decoded 6,000 columns, as many as
    // a ring for the narrow view holds.
    constexpr int kLong = 20'000;
    const auto chunk = [&item](qint64 start, int count, quint64 generation, bool reset,
                               int sessionWidth, char value) {
        item.feedPrecomputedChunk(QByteArray(kBins * count, value), kBins, 0, count,
            static_cast<int>(start), kLong, kRate, kHop, false, reset, 1, reset, generation,
            sessionWidth, 2'048, quint64(kLong) * kHop, kLong);
    };
    chunk(10'000, 0, 2, true, 0, '\x00');
    chunk(10'000, 6'000, 2, false, 0, '\x70');
    int narrowCapacity = 0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        narrowCapacity = item.m_ringCapacity;
    }
    QVERIFY(narrowCapacity >= 6'000);
    QVERIFY(9'000 + narrowCapacity < 16'000);
    QCOMPARE(ringUnlike(item, 10'000, 15'999, '\x70'), QString());

    // The refill for a view 6,000 pixels wide.
    item.feedPrecomputedChunk(QByteArray(), 0, 0, 0, 0, 0, 0, 0, false, true, 1, false, 3);
    item.feedPrecomputedChunk(QByteArray(), kBins, 0, 0, 9'000, kLong, kRate, kHop, false,
        true, 1, false, 3, 6'000, 2'048, quint64(kLong) * kHop, kLong);
    item.feedPrecomputedChunk(QByteArray(kBins * 8, '\x30'), kBins, 0, 8, 12'000, kLong, kRate,
        kHop, false, false, 1, false, 3, 6'000, 2'048, quint64(kLong) * kHop, kLong);
    QCOMPARE(ringUnlike(item, 12'000, 12'007, '\x30'), QString());
    QCOMPARE(ringUnlike(item, 10'000, 11'999, '\x70'), QString());
    QCOMPARE(ringUnlike(item, 12'008, 15'999, '\x70'), QString());
    // The session's columns from its first on have room too.
    item.feedPrecomputedChunk(QByteArray(kBins * 8, '\x30'), kBins, 0, 8, 9'000, kLong, kRate,
        kHop, false, false, 1, false, 3, 6'000, 2'048, quint64(kLong) * kHop, kLong);
    QCOMPARE(ringUnlike(item, 9'000, 9'007, '\x30'), QString());
}

// With the real bridge and the GStreamer backend, playing in real time
// into a fake audio sink: a middle-drag to a part of the track the session
// has not decoded has the analysis decode that view, the plot's own seek
// keeps it as the engine confirms it, and a seek from elsewhere, a gapless
// successor and Repeat-One bring it back.
void SpectrogramFollowSmokeTest::gstreamerMovedViewIsDecodedAndSeeksBringItBack() {
    QVERIFY(gst_init_check(nullptr, nullptr, nullptr));
    GstElementFactory *fakeAudioSink = gst_element_factory_find("fakeaudiosink");
    QVERIFY2(fakeAudioSink != nullptr, "fakeaudiosink, from the GStreamer bad plugins, is missing");
    gst_object_unref(fakeAudioSink);
    const QByteArray sink = qgetenv("FERROUS_GST_OUTPUT_SINK");
    qputenv("FERROUS_GST_OUTPUT_SINK", "fakeaudiosink");
    const auto restoreSink = qScopeGuard([&sink]() { qputenv("FERROUS_GST_OUTPUT_SINK", sink); });
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = QFileInfo(dir.path()).canonicalFilePath();
    const QString first = base + QStringLiteral("/first.wav");
    const QString second = base + QStringLiteral("/second.wav");
    constexpr double kTrackSeconds = 40.0;
    QVERIFY(writeWav(first, kTrackSeconds));
    QVERIFY(writeWav(second, kTrackSeconds));

    // A pane 400 px wide, shown, whose columns the analysis sends.
    QQuickWindow window;
    window.resize(400, 100);
    presentWithoutDrawing(window);
    auto *item = new SpectrogramItem(window.contentItem());
    item->setWidth(kWidth);
    item->setHeight(100.0);
    item->setDisplayMode(1);
    item->setZoomEnabled(true);
    item->setCrosshairEnabled(true);
    item->setPlaying(true);
    BridgeClient bridge;
    bridge.setRepeatMode(0);
    bridge.clearQueue();
    bridge.setSpectrogramDisplayMode(1);
    bridge.setSpectrogramWidgetWidth(400);
    bridge.registerSpectrogramItem(item, 0);
    item->setClock(bridge.playbackClock());
    QObject::connect(item, &SpectrogramItem::seekRequested, &bridge, &BridgeClient::seek);
    QObject::connect(item, &SpectrogramItem::backendZoomRequested, &bridge,
        &BridgeClient::setSpectrogramZoomLevel);
    const auto finish = qScopeGuard([&bridge, item]() {
        bridge.setRepeatMode(0);
        bridge.stop();
        bridge.clearQueue();
        bridge.unregisterSpectrogramItem(item);
    });
    bridge.replaceWithPaths({first, second});
    const bool started = QTest::qWaitFor([&]() {
        return bridge.playbackState() == QStringLiteral("Playing")
            && bridge.currentTrackPath() == first
            && bridge.playbackClock()->currentPositionSeconds() > 0.2;
    }, 10000);
    QVERIFY2(started, qPrintable(QStringLiteral("%1 %2").arg(bridge.playbackState(), bridge.currentTrackPath())));
    const auto columnsPerSecond = [item]() {
        QMutexLocker lock(&item->m_stateMutex);
        return item->m_precomputedHopSize > 0
            ? static_cast<double>(item->m_precomputedSampleRateHz) / item->m_precomputedHopSize
            : 0.0;
    };
    const auto view = [item, &columnsPerSecond]() {
        const double rate = columnsPerSecond();
        QMutexLocker lock(&item->m_stateMutex);
        return item->centeredViewLocked(
            std::max(0.0, item->currentRenderPositionSecondsLocked(item->m_now())),
            item->canvasWidthLocked(), item->canvasHeightLocked(), item->canvasZoomLocked(),
            rate);
    };
    const auto holds = [item](const SpectrogramItem::CenteredView &shown) {
        QMutexLocker lock(&item->m_stateMutex);
        return shown.right > shown.left && item->ringHoldsColumnsLocked(shown.left, shown.right);
    };
    const auto engineDone = [&bridge]() { return bridge.playbackClock()->advancing(); };
    // A frame now and then, as the window shows them.
    QTimer frames;
    QObject::connect(&frames, &QTimer::timeout, item, [item]() { item->handleWindowAfterAnimating(); });
    frames.start(16);

    // Zoomed in 8 times, 375 columns a second: the session decodes about
    // 20 s ahead of the playhead, and a view 35 s on is not among them for
    // another 15 s of playback.
    QTRY_VERIFY_WITH_TIMEOUT(columnsPerSecond() > 40.0, 10000);
    item->setZoomLevel(8.0);
    QTRY_VERIFY_WITH_TIMEOUT(columnsPerSecond() > 300.0 && holds(view()), 10000);
    const double moveColumns = 35.0 * columnsPerSecond();
    middleDrag(*item, kWidth, kWidth - moveColumns);
    QVERIFY(item->detached());
    const auto put = view();
    const double playhead = bridge.playbackClock()->currentPositionSeconds();
    QVERIFY(static_cast<double>(put.left) / columnsPerSecond() > playhead + 30.0);
    // The analysis decodes the view where it was put, long before
    // playback's lookahead would reach it.
    QTRY_VERIFY_WITH_TIMEOUT(holds(put), 5000);
    QCOMPARE(view().left, put.left);

    // A quick pan back, to a view that starts before the session's first
    // column, restarts the session around it. The columns the session
    // before drew of that view stay until the new session's replace them.
    const auto sessionState = [item]() {
        QMutexLocker lock(&item->m_stateMutex);
        return std::pair(item->m_ringColumnBase.value_or(-1), item->m_precomputedSessionGeneration);
    };
    const auto [firstColumn, firstGeneration] = sessionState();
    QVERIFY(firstColumn > 0 && firstColumn <= put.left);
    SpectrogramItem::CenteredView drawn = put;
    drawn.left = firstColumn;
    QTRY_VERIFY_WITH_TIMEOUT(holds(drawn), 5000);
    const qint64 back = put.left - (firstColumn - kWindow / 4);
    middleDrag(*item, 0.0, static_cast<double>(back));
    const auto backView = view();
    QCOMPARE(backView.left, firstColumn - kWindow / 4);
    SpectrogramItem::CenteredView kept = backView;
    kept.left = firstColumn;
    QString lost;
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, item, [&]() {
        if (lost.isEmpty() && !holds(kept)) {
            lost = QStringLiteral("columns %1..%2 went missing, generation %3")
                .arg(kept.left).arg(kept.right).arg(sessionState().second);
        }
    });
    watch.start(0);
    QTRY_VERIFY_WITH_TIMEOUT(sessionState().second > firstGeneration && holds(backView), 5000);
    watch.stop();
    QVERIFY2(lost.isEmpty(), qPrintable(lost));
    QCOMPARE(view().left, backView.left);

    // The plot's own seek, into the view, keeps it while the engine seeks
    // and confirms it.
    const double target = rightClickAt(*item, 0.1 * kWidth);
    QVERIFY(target > 25.0);
    QTRY_VERIFY_WITH_TIMEOUT(engineDone(), 10000);
    QVERIFY(item->detached());
    QCOMPARE(view().left, backView.left);

    // A seek from elsewhere, as the seek bar's, brings it back, and the
    // analysis decodes the playhead's view again.
    bridge.seek(2.0);
    QTRY_VERIFY_WITH_TIMEOUT(!item->detached(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(engineDone() && holds(view()), 10000);

    // A gapless successor, at the default zoom: a view moved back, whose
    // playhead has passed it, follows the next track.
    item->setZoomLevel(1.0);
    QTRY_VERIFY_WITH_TIMEOUT(columnsPerSecond() > 40.0 && columnsPerSecond() < 50.0, 10000);
    bridge.seek(kTrackSeconds - 2.0);
    QTRY_VERIFY_WITH_TIMEOUT(engineDone(), 10000);
    middleDrag(*item, 0.0, kWidth);
    QVERIFY(item->detached());
    QTRY_VERIFY_WITH_TIMEOUT(bridge.currentTrackPath() == second, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!item->detached(), 10000);

    // Repeat-One: the same file again.
    bridge.setRepeatMode(1);
    bridge.seek(kTrackSeconds - 2.0);
    QTRY_VERIFY_WITH_TIMEOUT(engineDone() && view().left > 0, 10000);
    middleDrag(*item, 0.0, kWidth);
    QVERIFY(item->detached());
    const quint64 token = bridge.playbackClock()->state().trackToken;
    QTRY_VERIFY_WITH_TIMEOUT(bridge.playbackClock()->state().trackToken != token, 10000);
    QCOMPARE(bridge.currentTrackPath(), second);
    QTRY_VERIFY_WITH_TIMEOUT(!item->detached(), 10000);

    // Autoscroll, zoomed in 8 times, a view of about a second scrolling
    // four views a second towards the end: the analysis decodes the view where it
    // goes, and has it once it stops. A seek from elsewhere stops it.
    bridge.setRepeatMode(0);
    bridge.seek(5.0);
    item->setZoomLevel(8.0);
    QTRY_VERIFY_WITH_TIMEOUT(engineDone() && columnsPerSecond() > 300.0 && holds(view()), 10000);
    QObject::connect(&frames, &QTimer::timeout, item,
        [item]() { item->m_autoscroll.frameShown(); });
    middleClick(*item, 200.0);
    QVERIFY(item->m_autoscroll.active() && item->detached());
    const qint64 clicked = view().left;
    pointerAt(*item, kWidth);
    QTRY_VERIFY_WITH_TIMEOUT(view().left > clicked + 4 * kWindow, 10000);
    item->m_autoscroll.stop();
    QVERIFY(item->detached());
    QTRY_VERIFY_WITH_TIMEOUT(holds(view()), 5000);
    middleClick(*item, 200.0);
    QVERIFY(item->m_autoscroll.active());
    bridge.seek(2.0);
    QTRY_VERIFY_WITH_TIMEOUT(!item->m_autoscroll.active() && !item->detached(), 10000);

    // A track change stops it: Repeat-One, the same file again.
    bridge.setRepeatMode(1);
    bridge.seek(kTrackSeconds - 2.0);
    QTRY_VERIFY_WITH_TIMEOUT(engineDone() && view().left > 0, 10000);
    middleClick(*item, 200.0);
    QVERIFY(item->m_autoscroll.active());
    const quint64 repeatToken = bridge.playbackClock()->state().trackToken;
    QTRY_VERIFY_WITH_TIMEOUT(bridge.playbackClock()->state().trackToken != repeatToken, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!item->m_autoscroll.active() && !item->detached(), 10000);
    frames.stop();
}

// Autoscroll after a middle click moves the centered view by the pointer's
// distance from the click, as the waveform's does: none within the dead
// zone, a view a second half way to the plot's edge, four at the edge, up
// to eight, and at most kAutoscrollMaxSecondsPerSecond of the track, so
// that the analysis keeps up. It asks for frames only while the view
// moves.
void SpectrogramFollowSmokeTest::autoscrollMovesByThePointersDistance() {
    SpectrogramItem item;
    TestTime time;
    prepare(item, time, 60.0, false);
    const qint64 following = viewOf(item).left;
    middleClick(item, 200.0);
    QVERIFY(item.m_autoscroll.active());
    QVERIFY(item.detached());
    QVERIFY(item.viewFollow()->autoscrolling());
    QCOMPARE(viewOf(item).left, following);
    QVERIFY(!viewOf(item).scrolls);

    // Within the dead zone the view holds still and asks for no frames.
    for (const double x : {200.0, 211.0, 189.0}) {
        pointerAt(item, x);
        autoscrollFor(item, time, 0.5);
        QCOMPARE(viewOf(item).left, following);
        QCOMPARE(item.m_autoscroll.m_lastFrameNs, qint64(0));
    }

    // A view a second half way to the plot's edge, four at its edge, eight,
    // at most, past it.
    const double reach = kWidth / 2.0 - ViewAutoscroll::kDeadZonePixels;
    struct Case { double x; double viewsPerSecond; };
    for (const Case &c : {Case{200.0 + 12.0 + reach / 2.0, 1.0}, Case{400.0, 4.0},
                          Case{0.0, -4.0}, Case{200.0 - 12.0 - reach / 2.0, -1.0},
                          Case{2000.0, 8.0}, Case{-2000.0, -8.0}}) {
        pointerAt(item, c.x);
        QVERIFY(std::abs(item.m_autoscroll.currentViewsPerSecond() - c.viewsPerSecond) < 1e-9);
        const qint64 before = viewOf(item).left;
        autoscrollFor(item, time, 0.25);
        const qint64 moved = viewOf(item).left - before;
        QVERIFY2(std::abs(static_cast<double>(moved) - c.viewsPerSecond * kWindow * 0.25) <= 1.0,
                 qPrintable(QStringLiteral("%1 px: moved %2").arg(c.x).arg(moved)));
        QVERIFY(item.detached());
    }

    // Held at the end of the track, it asks for no more frames; a move back
    // moves the view from there at once.
    pointerAt(item, 2000.0);
    autoscrollFor(item, time, 70.0 / 8.53 / 4.0);
    QCOMPARE(viewOf(item).left, qint64(kColumns - kWindow));
    QCOMPARE(item.m_autoscroll.m_lastFrameNs, qint64(0));
    pointerAt(item, 0.0);
    autoscrollFor(item, time, 0.25);
    QVERIFY(std::abs(viewOf(item).left - (kColumns - kWindow - kWindow)) <= 1);

    // The wheel zooms about the pointer meanwhile, and autoscroll goes on.
    takeZoomRequests(item);
    wheelAt(item, 300.0, 120);
    QVERIFY(item.m_autoscroll.active() && item.detached());
    QCOMPARE(item.viewFollow()->snapshot().anchorFraction, 300.0 / kWidth);
    const double anchor = item.viewFollow()->snapshot().anchorSeconds;
    autoscrollFor(item, time, 0.5);
    QVERIFY(item.viewFollow()->snapshot().anchorSeconds < anchor);

    // Held at the end of the track, a zoom about the pointer near the
    // plot's right edge makes room to scroll on: it goes on at once.
    pointerAt(item, 390.0);
    autoscrollFor(item, time, 10.0);
    QCOMPARE(item.m_autoscroll.m_lastFrameNs, qint64(0));
    wheelAt(item, 390.0, 120);
    QVERIFY(item.m_autoscroll.m_lastFrameNs != 0);
    const double zoomedAnchor = item.viewFollow()->snapshot().anchorSeconds;
    autoscrollFor(item, time, 0.1);
    QVERIFY(item.viewFollow()->snapshot().anchorSeconds > zoomedAnchor);

    // Four views a second of a view 42.7 s wide, at the plot's edge, are
    // within the cap of seconds of the track a second; eight, past the
    // plot, are not, and scroll at that cap.
    QCOMPARE(SpectrogramItem::kAutoscrollMaxSecondsPerSecond, 200.0);
    SpectrogramItem wide;
    TestTime wideTime;
    prepare(wide, wideTime, 30.0, false);
    wide.setWidth(2'000.0);
    frame(wide);
    middleClick(wide, 1'000.0);
    QVERIFY(wide.m_autoscroll.active());
    struct WideCase { double x; double columnsPerSecond; };
    for (const WideCase &c : {WideCase{2'000.0, 4.0 * 2'000.0},
             WideCase{10'000.0, SpectrogramItem::kAutoscrollMaxSecondsPerSecond * kColumnsPerSecond},
             WideCase{0.0, -4.0 * 2'000.0}}) {
        pointerAt(wide, c.x);
        const qint64 wideBefore = viewOf(wide).left;
        autoscrollFor(wide, wideTime, 0.1);
        const double wideMoved = static_cast<double>(viewOf(wide).left - wideBefore);
        QVERIFY2(std::abs(wideMoved - c.columnsPerSecond * 0.1) <= 1.0,
                 qPrintable(QStringLiteral("%1 px: moved %2").arg(c.x).arg(wideMoved)));
    }

    // The press that stopped it, become a drag, pans; the next click, even
    // one of the same timestamp, starts autoscroll again.
    wide.m_autoscroll.stop();
    wide.m_autoscroll.m_stopPressTimestamp = 0;
    middleDrag(wide, 1'000.0, 900.0);
    QVERIFY(!wide.m_autoscroll.active());
    middleClick(wide, 1'000.0);
    QVERIFY(wide.m_autoscroll.active());
}

// The lanes of a plot share their view: autoscroll on one moves all of
// them, and a middle click on another, which the window gives to the
// autoscroll, stops it.
void SpectrogramFollowSmokeTest::autoscrollMovesEveryLaneOfThePlot() {
    SpectrogramViewFollow follow;
    SpectrogramItem left;
    SpectrogramItem right;
    TestTime time;
    left.setViewFollow(&follow);
    right.setViewFollow(&follow);
    prepare(left, time, 60.0, false);
    prepare(right, time, 60.0, false);
    const qint64 following = viewOf(left).left;
    middleClick(right, 200.0);
    QVERIFY(left.detached() && right.detached());
    // A view a second, half way to the plot's edge.
    pointerAt(right, 200.0 + ViewAutoscroll::kDeadZonePixels
        + (kWidth / 2.0 - ViewAutoscroll::kDeadZonePixels) / 2.0);
    autoscrollFor(right, time, 0.5);
    QVERIFY(std::abs(viewOf(left).left - (following + kWindow / 2)) <= 1);
    QCOMPARE(viewOf(left).left, viewOf(right).left);
    // Follow Playhead on the other lane stops it.
    left.followPlayhead();
    QCoreApplication::processEvents();
    QVERIFY(!right.m_autoscroll.active());
    QVERIFY(!left.detached() && !right.detached());

    // The wheel on the other lane zooms both, as the surface passes it on:
    // a view held at the end of the track goes on at once.
    takeZoomRequests(left);
    QObject::connect(&left, &SpectrogramItem::zoomRequested, &right,
        [&right](double zoom) { right.setZoomLevel(zoom); });
    middleClick(right, 200.0);
    pointerAt(right, 390.0);
    autoscrollFor(right, time, 20.0);
    QCOMPARE(viewOf(right).left, qint64(kColumns - kWindow));
    QCOMPARE(right.m_autoscroll.m_lastFrameNs, qint64(0));
    wheelAt(left, 390.0, 120);
    QVERIFY(right.m_autoscroll.m_lastFrameNs != 0);
    const double zoomedAnchor = follow.snapshot().anchorSeconds;
    autoscrollFor(right, time, 0.1);
    QVERIFY(follow.snapshot().anchorSeconds > zoomedAnchor);
    right.m_autoscroll.stop();

    // A lane that goes while it autoscrolls, as when the channels change,
    // leaves the view to the others, as at the end of a drag.
    auto *gone = new SpectrogramItem;
    gone->setViewFollow(&follow);
    prepare(*gone, time, 60.0, false);
    middleClick(*gone, 200.0);
    QVERIFY(follow.autoscrolling());
    delete gone;
    QVERIFY(!follow.autoscrolling());
    QVERIFY(left.detached());
}

// A rolling view stays live, and a view of the whole track has nowhere to
// scroll to: a middle click there does nothing.
void SpectrogramFollowSmokeTest::autoscrollInRollingModeOrOfTheWholeTrackDoesNothing() {
    SpectrogramItem rolling;
    TestTime time;
    prepare(rolling, time, 60.0);
    rolling.setDisplayMode(0);
    middleClick(rolling, 200.0);
    QVERIFY(!rolling.m_autoscroll.active());
    QVERIFY(!rolling.detached());
    QVERIFY(!rolling.viewFollow()->autoscrolling());

    SpectrogramItem whole;
    TestTime wholeTime;
    prepare(whole, wholeTime, 60.0);
    whole.setWidth(5'200.0);
    middleClick(whole, 3'000.0);
    QVERIFY(!whole.m_autoscroll.active());
    QVERIFY(!whole.detached());
    QCOMPARE(viewOf(whole).left, qint64(0));
}

void SpectrogramFollowSmokeTest::autoscrollArmsTheReattachPointWhenItStops_data() {
    QTest::addColumn<double>("pointer");
    QTest::addColumn<bool>("glidesBack");
    QTest::addRow("playhead left of the re-attach point") << 400.0 << true;
    QTest::addRow("playhead past the view") << 0.0 << false;
}

// While autoscroll runs, nothing glides back; when it stops, the view arms
// its re-attach point as at the end of a drag.
void SpectrogramFollowSmokeTest::autoscrollArmsTheReattachPointWhenItStops() {
    QFETCH(double, pointer);
    QFETCH(bool, glidesBack);
    SpectrogramItem item;
    TestTime time;
    prepare(item, time, 60.0);
    middleClick(item, 200.0);
    const qint64 held = viewOf(item).left;
    // Held in the dead zone while the playhead plays through the view and
    // past it: no glide.
    autoscrollFor(item, time, 9.0);
    QVERIFY(playheadFraction(item) > 1.0);
    QCOMPARE(viewOf(item).left, held);
    QCOMPARE(modeOf(item), ViewFollow::Mode::Detached);
    QVERIFY(std::isinf(item.viewFollow()->snapshot().reattachSeconds));

    // A view's span on or back from there: four views a second at the
    // plot's edge.
    pointerAt(item, pointer);
    autoscrollFor(item, time, 0.25);
    item.m_autoscroll.stop();
    QVERIFY(!item.viewFollow()->autoscrolling());
    QCOMPARE(std::isfinite(item.viewFollow()->snapshot().reattachSeconds), glidesBack);
    bool glided = false;
    for (int step = 0; step < 60 * 20; ++step) {
        time.advance(1.0 / 60.0);
        frame(item);
        glided = glided || modeOf(item) != ViewFollow::Mode::Detached;
    }
    QCOMPARE(glided, glidesBack);
    QCOMPARE(item.detached(), !glidesBack);
}

void SpectrogramFollowSmokeTest::autoscrollStopsAsTheViewComesBack_data() {
    QTest::addColumn<QString>("event");
    QTest::addColumn<bool>("keeps");
    QTest::addRow("own seek") << QStringLiteral("own seek") << true;
    QTest::addRow("follow playhead") << QStringLiteral("follow playhead") << false;
    QTest::addRow("seek bar") << QStringLiteral("seek bar") << false;
    QTest::addRow("stop") << QStringLiteral("stop") << false;
    QTest::addRow("gapless successor") << QStringLiteral("gapless successor") << false;
    QTest::addRow("repeat one") << QStringLiteral("repeat one") << false;
    QTest::addRow("switch to rolling") << QStringLiteral("switch to rolling") << false;
    QTest::addRow("zoom out to the whole track") << QStringLiteral("zoom out") << false;
    QTest::addRow("hidden") << QStringLiteral("hidden") << false;
}

// Follow Playhead (and Reset Zoom, which calls it), a seek other than the
// plot's own, a stop, a track change, including a gapless successor and
// Repeat-One, and a switch to rolling stop autoscroll and bring the view
// back to the playhead; the plot's own seek keeps both. Hiding the plot, as
// a switch to the waveform does, stops it too.
void SpectrogramFollowSmokeTest::autoscrollStopsAsTheViewComesBack() {
    QFETCH(QString, event);
    QFETCH(bool, keeps);
    SpectrogramItem item;
    TestTime time;
    prepare(item, time, 60.0);
    takeZoomRequests(item);
    QSignalSpy stops(&item.m_autoscroll, &ViewAutoscroll::stopped);
    middleClick(item, 200.0);
    pointerAt(item, 100.0);
    autoscrollFor(item, time, 0.5);
    QVERIFY(item.m_autoscroll.active() && item.detached());
    const qint64 scrolled = viewOf(item).left;
    PlaybackClock &clock = testClock(item);
    const quint64 token = clock.state().trackToken;
    if (event == QStringLiteral("own seek")) {
        const double target = rightClickAt(item, 300.0);
        clock.holdAt(target);
        engineLands(item, target + 0.04);
    } else if (event == QStringLiteral("follow playhead")) {
        item.followPlayhead();
    } else if (event == QStringLiteral("seek bar")) {
        clock.holdAt(10.0);
        engineLands(item, 10.03);
    } else if (event == QStringLiteral("stop")) {
        item.setPlaying(false);
        PlaybackSample stopped;
        stopped.trackToken = token;
        clock.applySample(stopped, true);
    } else if (event == QStringLiteral("gapless successor")
               || event == QStringLiteral("repeat one")) {
        PlaybackSample next;
        next.positionSeconds = 0.0;
        next.sampledAtNs = clock.nowNs();
        next.advancing = true;
        next.trackToken = token + 1;
        clock.applySample(next, false);
    } else if (event == QStringLiteral("switch to rolling")) {
        item.setDisplayMode(0);
        item.setDisplayMode(1);
    } else if (event == QStringLiteral("zoom out")) {
        item.setWidth(5'200.0);
    } else {
        item.setVisible(false);
    }
    frame(item);
    QCoreApplication::processEvents();
    QCOMPARE(item.m_autoscroll.active(), keeps);
    QCOMPARE(stops.count(), keeps ? 0 : 1);
    QCOMPARE(item.viewFollow()->autoscrolling(), keeps);
    if (keeps) {
        QVERIFY(item.detached());
        QCOMPARE(viewOf(item).left, scrolled);
        autoscrollFor(item, time, 0.5);
        QVERIFY(viewOf(item).left < scrolled);
    } else if (event == QStringLiteral("hidden")) {
        // Stopped as at the end of a drag: where it was.
        QCOMPARE(viewOf(item).left, scrolled);
    } else {
        QVERIFY(!item.detached());
        QCOMPARE(modeOf(item), ViewFollow::Mode::Playhead);
        if (event != QStringLiteral("zoom out")) {
            QCOMPARE(viewOf(item).left, followingLeftOf(item));
        }
    }
}

// In a window: a middle click on a centered view starts autoscroll, and the
// pointer moves the view with the window's frames, with a horizontal
// resize cursor; Escape stops it before the window's own Escape takes the
// key, and another middle click stops it. A rolling view ignores the click.
void SpectrogramFollowSmokeTest::autoscrollInAWindow() {
    QQuickWindow window;
    window.resize(600, 200);
    auto *item = new SpectrogramItem(window.contentItem());
    item->setX(100.0);
    item->setY(20.0);
    item->setWidth(kWidth);
    item->setHeight(100.0);
    item->setDisplayMode(1);
    item->setZoomEnabled(true);
    feedTrack(*item);
    setTestPosition(*item, 60.0, 1);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QShortcut escape(QKeySequence(Qt::Key_Escape), &window);
    QSignalSpy escapes(&escape, &QShortcut::activated);
    const QPoint click(300, 70);
    const qint64 following = viewOf(*item).left;

    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, click);
    QVERIFY(item->m_autoscroll.active());
    QVERIFY(item->detached());
    QVERIFY(QGuiApplication::overrideCursor() != nullptr);
    QCOMPARE(QGuiApplication::overrideCursor()->shape(), Qt::SizeHorCursor);
    QTest::mouseMove(&window, QPoint(380, 70));
    QTRY_VERIFY_WITH_TIMEOUT(viewOf(*item).left > following + 50, 5000);

    // Out of the window, whose Leave tells no position, it goes on at the
    // speed it had where the pointer was last seen, with the frames alone
    // and no more events, by whichever edge it left: here the top.
    QTest::mouseMove(&window, QPoint(380, 0));
    const double leaving = item->m_autoscroll.currentViewsPerSecond();
    QVERIFY(leaving > 0.0);
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&window, &leave);
    QCOMPARE(item->m_autoscroll.currentViewsPerSecond(), leaving);
    const qint64 outside = viewOf(*item).left;
    QTRY_VERIFY_WITH_TIMEOUT(viewOf(*item).left > outside + 20, 5000);
    QCOMPARE(item->m_autoscroll.currentViewsPerSecond(), leaving);
    // Back in on the other side of the click, it scrolls that way at once.
    QEnterEvent enter(QPointF(20, 70), QPointF(20, 70), window.mapToGlobal(QPointF(20, 70)));
    QCoreApplication::sendEvent(&window, &enter);
    QVERIFY(item->m_autoscroll.currentViewsPerSecond() < 0.0);
    const qint64 back = viewOf(*item).left;
    QTRY_VERIFY_WITH_TIMEOUT(viewOf(*item).left < back - 20, 5000);
    QTest::mouseMove(&window, QPoint(380, 70));
    QCoreApplication::sendEvent(&window, &leave);

    // Escape stops it, and only it, with the pointer outside too.
    QTest::keyClick(&window, Qt::Key_Escape);
    QVERIFY(!item->m_autoscroll.active());
    QCOMPARE(escapes.count(), 0);
    QVERIFY(QGuiApplication::overrideCursor() == nullptr);
    QVERIFY(item->detached());
    const qint64 stopped = viewOf(*item).left;
    QTest::qWait(100);
    QCOMPARE(viewOf(*item).left, stopped);

    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, click);
    QVERIFY(item->m_autoscroll.active());
    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, QPoint(560, 70));
    QVERIFY(!item->m_autoscroll.active());
    QTest::qWait(100);
    QCOMPARE(viewOf(*item).left, stopped);

    // One on the plot stops it too, without starting it again, and a drag
    // from it pans as a drag does.
    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, click);
    QVERIFY(item->m_autoscroll.active());
    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, QPoint(320, 70));
    QVERIFY(!item->m_autoscroll.active());
    QCOMPARE(viewOf(*item).left, stopped);
    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, click);
    QVERIFY(item->m_autoscroll.active());
    QTest::mousePress(&window, Qt::MiddleButton, Qt::NoModifier, QPoint(300, 70));
    QVERIFY(!item->m_autoscroll.active());
    QTest::mouseMove(&window, QPoint(250, 70));
    QTest::mouseMove(&window, QPoint(200, 70));
    QTest::mouseRelease(&window, Qt::MiddleButton, Qt::NoModifier, QPoint(200, 70));
    QVERIFY(!item->m_autoscroll.active());
    QCOMPARE(viewOf(*item).left, stopped + 100);

    // Rolling, the click does nothing.
    item->setDisplayMode(0);
    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, click);
    QVERIFY(!item->m_autoscroll.active());
    QVERIFY(!item->detached());
    QVERIFY(QGuiApplication::overrideCursor() == nullptr);
}

namespace {

// A spectrogram surface in a window 600 px wide, of a track that is decoded.
const char kSurfaceInAWindow[] = R"QML(
import QtQuick 2.15
import QtQuick.Window 2.15
import "components" as Components
import "viewers" as Viewers

Window {
    id: paletteRoot
    width: 600
    height: 240
    visible: true

    QtObject {
        id: bridge
        objectName: "surfaceBridge"
        property string playbackState: "Paused"
        property int spectrogramViewMode: 1
        property int spectrogramDisplayMode: 1
        property real dbRange: 90
        property bool logScale: false
        property bool spectrogramFrequencyCap: false
        property bool showFps: false
        property int sampleRateHz: 48000
        property bool showSpectrogramCrosshair: true
        property bool showSpectrogramScale: false
        property bool spectrogramZoomEnabled: true
        property bool stretchShortTracks: true
        property string currentTrackPath: "/fixture/track.flac"
        property var currentTrackChannelLabels: []
        signal precomputedSpectrogramChannelsReady(int channelCount, bool bufferReset)
        signal playbackChanged()
        function isChannelSilent(channelIndex) { return false }
        function setSpectrogramZoomLevel(level) {}
        function registerSpectrogramItem(item, channelIndex) {}
        function unregisterSpectrogramItem(item) {}
        function setSpectrogramWidgetWidth(width) {}
    }

    Components.UiPalette { id: palette; windowRoot: paletteRoot }

    Viewers.SpectrogramSurface {
        objectName: "surfaceInAWindow"
        anchors.fill: parent
        uiBridge: bridge
        uiPalette: palette
    }
}
)QML";

struct SurfaceInAWindow {
    QQmlApplicationEngine engine;
    std::unique_ptr<QObject> root;
    QQuickWindow *window = nullptr;
    QQuickItem *surface = nullptr;
    // From the top.
    QList<SpectrogramItem *> lanes;

    QString load(int channels, int displayMode) {
        QString error;
        root.reset(createQmlObjectFromSource(engine, QByteArray(kSurfaceInAWindow),
            QUrl::fromLocalFile(
                QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml")),
            &error));
        window = qobject_cast<QQuickWindow *>(root.get());
        if (window == nullptr) return QStringLiteral("no window: ") + error;
        surface = window->findChild<QQuickItem *>(QStringLiteral("surfaceInAWindow"));
        QObject *bridge = window->findChild<QObject *>(QStringLiteral("surfaceBridge"));
        if (surface == nullptr || bridge == nullptr) return QStringLiteral("no surface");
        bridge->setProperty("spectrogramDisplayMode", displayMode);
        QVariant descriptors;
        QMetaObject::invokeMethod(surface, "descriptorsForChannelCount",
            Q_RETURN_ARG(QVariant, descriptors), Q_ARG(QVariant, channels));
        surface->setProperty("channelDescriptors", descriptors);
        const bool made = QTest::qWaitFor([&]() {
            lanes.clear();
            collectQuickItems(surface, &lanes);
            return lanes.size() == channels;
        });
        if (!made || !QTest::qWaitForWindowExposed(window)) return QStringLiteral("no lanes");
        std::sort(lanes.begin(), lanes.end(), [](SpectrogramItem *a, SpectrogramItem *b) {
            return a->mapToScene(QPointF()).y() < b->mapToScene(QPointF()).y();
        });
        for (SpectrogramItem *lane : std::as_const(lanes)) {
            feedTrack(*lane);
            setTestPosition(*lane, 60.0, 1);
        }
        return QString();
    }
};

// Empty when each lane has laid its crosshair's line out at `x`, and the
// lowest its time there too; else what the first that has not shows.
QString crosshairNotAt(const SurfaceInAWindow &plot, int x) {
    const double scale = plot.window->effectiveDevicePixelRatio();
    const int column = static_cast<int>(std::floor(x * scale + 1e-9));
    for (int index = 0; index < plot.lanes.size(); ++index) {
        SpectrogramItem &lane = *plot.lanes[index];
        QMutexLocker lock(&lane.m_stateMutex);
        const QRect line = lane.m_crosshairVerticalLineRect;
        if (line.isEmpty() || line.x() != column) {
            return QStringLiteral("lane %1's line is at %2, not %3")
                .arg(index).arg(line.x()).arg(column);
        }
        if (!lane.showCrosshairTime()) continue;
        bool timed = false;
        for (const SpectrogramOverlayImage &image : std::as_const(lane.m_crosshair.images)) {
            timed = timed || (image.kind == SpectrogramOverlayImage::ReadoutPlate
                && std::abs(image.rect.center().x() - x) <= 2.0);
        }
        if (!timed) return QStringLiteral("no time at %1").arg(x);
    }
    return QString();
}

} // namespace

void SpectrogramFollowSmokeTest::crosshairStaysWholeThroughAMiddleDrag_data() {
    QTest::addColumn<int>("channels");
    QTest::addColumn<int>("displayMode");
    QTest::addColumn<int>("fromLane");
    QTest::newRow("stereo, centered") << 2 << 1 << 0;
    QTest::newRow("stereo, centered, from the lowest") << 2 << 1 << 1;
    QTest::newRow("stereo, rolling") << 2 << 0 << 0;
    QTest::newRow("5.1, centered") << 6 << 1 << 0;
    QTest::newRow("5.1, centered, from a middle lane") << 6 << 1 << 2;
    QTest::newRow("5.1, rolling") << 6 << 0 << 0;
}

// Through a middle-drag every lane's line, and the time on the lowest, is at
// the pointer on each of its moves: short of the drag distance, panning, in
// rolling mode, which does not pan, in another lane than it began in, and
// over a gapless or Repeat-One change of the track.
void SpectrogramFollowSmokeTest::crosshairStaysWholeThroughAMiddleDrag() {
    QFETCH(int, channels);
    QFETCH(int, displayMode);
    QFETCH(int, fromLane);
    SurfaceInAWindow plot;
    const QString error = plot.load(channels, displayMode);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QQuickWindow *window = plot.window;
    const auto middleOf = [&](int lane) {
        SpectrogramItem *item = plot.lanes[lane];
        return static_cast<int>(item->mapToScene(QPointF(0.0, item->height() / 2)).y());
    };
    const int y = middleOf(fromLane);
    QTest::mouseMove(window, QPoint(400, y));
    QTRY_COMPARE(crosshairNotAt(plot, 400), QString());

    QTest::mousePress(window, Qt::MiddleButton, Qt::NoModifier, QPoint(400, y));
    for (const int x : {397, 380, 331, 281, 232}) {
        QTest::mouseMove(window, QPoint(x, y));
        // Paused, so only the crosshair's own frame lays it out.
        QTRY_COMPARE(crosshairNotAt(plot, x), QString());
    }
    QCOMPARE(plot.surface->property("detached").toBool(), displayMode == 1);

    // Into another lane, still dragging.
    const int other = fromLane == 0 ? channels - 1 : 0;
    QTest::mouseMove(window, QPoint(200, middleOf(other)));
    QTRY_COMPARE(crosshairNotAt(plot, 200), QString());

    // The next track, gapless or the same again, with the button held; a
    // chunk of the last one's session that comes late changes nothing.
    for (const quint64 token : {quint64(2), quint64(3)}) {
        for (SpectrogramItem *lane : std::as_const(plot.lanes)) {
            feedTrack(*lane, token, token);
            setTestPosition(*lane, 0.5, token);
            lane->feedPrecomputedChunk(QByteArray(kBins * 8, '\x30'), kBins, 0, 8, 0, kColumns,
                kRate, kHop, false, false, token - 1, false, token - 1);
        }
        const int x = token == 2 ? 170 : 140;
        QTest::mouseMove(window, QPoint(x, y));
        QTRY_COMPARE(crosshairNotAt(plot, x), QString());
    }

    QTest::mouseRelease(window, Qt::MiddleButton, Qt::NoModifier, QPoint(140, y));
    QTRY_COMPARE(crosshairNotAt(plot, 140), QString());
    QTest::mouseMove(window, QPoint(260, y));
    QTRY_COMPARE(crosshairNotAt(plot, 260), QString());
}

std::unique_ptr<QmlSmokeTest> makeSpectrogramFollowSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<SpectrogramFollowSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_spectrogram_follow.moc"
