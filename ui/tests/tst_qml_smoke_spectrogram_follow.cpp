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
    void gstreamerMovedViewIsDecodedAndSeeksBringItBack();
    void autoscrollMovesByThePointersDistance();
    void autoscrollMovesEveryLaneOfThePlot();
    void autoscrollInRollingModeOrOfTheWholeTrackDoesNothing();
    void autoscrollArmsTheReattachPointWhenItStops_data();
    void autoscrollArmsTheReattachPointWhenItStops();
    void autoscrollStopsAsTheViewComesBack_data();
    void autoscrollStopsAsTheViewComesBack();
    void autoscrollInAWindow();
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
    const auto refill = [&item](int start, quint64 generation) {
        item.feedPrecomputedChunk(QByteArray(), 0, 0, 0, 0, 0, 0, 0, false, true, 1, true, 0);
        item.feedPrecomputedChunk(QByteArray(), kBins, 0, 0, start, kColumns, kRate, kHop,
            false, true, 1, true, generation, 0, 2'048, quint64(120) * kRate, kColumns);
    };
    const auto columns = [&item](int start, int count, quint64 generation, char value) {
        item.feedPrecomputedChunk(QByteArray(kBins * count, value), kBins, 0, count, start,
            kColumns, kRate, kHop, false, false, 1, false, generation, 0, 2'048,
            quint64(120) * kRate, kColumns);
    };
    // A session for the view at 80 s, then one for the view at 90 s.
    refill(columnAt(80.0) - 200, 2);
    refill(columnAt(90.0) - 200, 3);
    // The newer one's columns, then the older one's, late.
    columns(static_cast<int>(columnAt(90.0)) - 200, 400, 3, '\x30');
    columns(static_cast<int>(columnAt(80.0)) - 200, 400, 2, '\x60');
    // Its own, after them, at the same place as the newer one's.
    columns(static_cast<int>(columnAt(90.0)) - 200, 10, 2, '\x60');
    QMutexLocker lock(&item.m_stateMutex);
    for (qint64 column = columnAt(90.0) - 200; column < columnAt(90.0) + 200; ++column) {
        const quint8 *data = item.ringColumnLocked(column, false);
        QVERIFY(data != nullptr);
        QCOMPARE(data[0], quint8(0x30));
    }
    QVERIFY(item.ringColumnLocked(columnAt(80.0), false) == nullptr);
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

    // The plot's own seek, into the view, keeps it while the engine seeks
    // and confirms it.
    const double target = rightClickAt(*item, 0.1 * kWidth);
    QVERIFY(target > 25.0);
    QTRY_VERIFY_WITH_TIMEOUT(engineDone(), 10000);
    QVERIFY(item->detached());
    QCOMPARE(view().left, put.left);

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

    // Autoscroll, zoomed in 8 times, a view of about a second scrolling a
    // view a second towards the end: the analysis decodes the view where it
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
// zone, a view a second at half the plot's width, up to two, and at most
// kAutoscrollMaxSecondsPerSecond of the track, so that the analysis keeps
// up. It asks for frames only while the view moves.
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

    const double reach = kWidth / 2.0 - ViewAutoscroll::kDeadZonePixels;
    struct Case { double x; double viewsPerSecond; };
    for (const Case &c : {Case{200.0 + 12.0 + reach / 2.0, 0.5}, Case{400.0, 1.0},
                          Case{0.0, -1.0}, Case{200.0 - 12.0 - reach / 2.0, -0.5},
                          Case{2000.0, 2.0}, Case{-2000.0, -2.0}}) {
        pointerAt(item, c.x);
        const qint64 before = viewOf(item).left;
        autoscrollFor(item, time, 0.5);
        const qint64 moved = viewOf(item).left - before;
        QVERIFY2(std::abs(static_cast<double>(moved) - c.viewsPerSecond * kWindow * 0.5) <= 1.0,
                 qPrintable(QStringLiteral("%1 px: moved %2").arg(c.x).arg(moved)));
        QVERIFY(item.detached());
    }

    // Held at the end of the track, it asks for no more frames; a move back
    // moves the view from there at once.
    pointerAt(item, 2000.0);
    autoscrollFor(item, time, 70.0 / 8.53);
    QCOMPARE(viewOf(item).left, qint64(kColumns - kWindow));
    QCOMPARE(item.m_autoscroll.m_lastFrameNs, qint64(0));
    pointerAt(item, 0.0);
    autoscrollFor(item, time, 0.5);
    QVERIFY(std::abs(viewOf(item).left - (kColumns - kWindow - kWindow / 2)) <= 1);

    // The wheel zooms about the pointer meanwhile, and autoscroll goes on.
    takeZoomRequests(item);
    wheelAt(item, 300.0, 120);
    QVERIFY(item.m_autoscroll.active() && item.detached());
    QCOMPARE(item.viewFollow()->snapshot().anchorFraction, 300.0 / kWidth);
    const double anchor = item.viewFollow()->snapshot().anchorSeconds;
    autoscrollFor(item, time, 0.5);
    QVERIFY(item.viewFollow()->snapshot().anchorSeconds < anchor);

    // A view wider than 40 s scrolls at most 40 s of the track a second.
    SpectrogramItem wide;
    TestTime wideTime;
    prepare(wide, wideTime, 30.0, false);
    wide.setWidth(2'000.0);
    frame(wide);
    middleClick(wide, 1'000.0);
    QVERIFY(wide.m_autoscroll.active());
    pointerAt(wide, 2'000.0);
    const qint64 wideBefore = viewOf(wide).left;
    autoscrollFor(wide, wideTime, 0.5);
    const double wideMoved = static_cast<double>(viewOf(wide).left - wideBefore);
    QVERIFY2(std::abs(wideMoved - SpectrogramItem::kAutoscrollMaxSecondsPerSecond * 0.5
                     * kColumnsPerSecond) <= 1.0,
             qPrintable(QString::number(wideMoved)));
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
    pointerAt(right, 400.0);
    autoscrollFor(right, time, 0.5);
    QVERIFY(std::abs(viewOf(left).left - (following + kWindow / 2)) <= 1);
    QCOMPARE(viewOf(left).left, viewOf(right).left);
    // Follow Playhead on the other lane stops it.
    left.followPlayhead();
    QCoreApplication::processEvents();
    QVERIFY(!right.m_autoscroll.active());
    QVERIFY(!left.detached() && !right.detached());
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

    pointerAt(item, pointer);
    autoscrollFor(item, time, 1.0);
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
    QTest::mouseMove(&window, QPoint(560, 70));
    QTRY_VERIFY_WITH_TIMEOUT(viewOf(*item).left > following + 50, 5000);

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

    // Rolling, the click does nothing.
    item->setDisplayMode(0);
    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, click);
    QVERIFY(!item->m_autoscroll.active());
    QVERIFY(!item->detached());
    QVERIFY(QGuiApplication::overrideCursor() == nullptr);
}

std::unique_ptr<QmlSmokeTest> makeSpectrogramFollowSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<SpectrogramFollowSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_spectrogram_follow.moc"
