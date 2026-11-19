// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

#include <QShortcut>

#include <gst/gst.h>

// The waveform's view following the playhead: a middle-drag detaches it,
// as autoscroll after a middle click does, the wheel zooms about the pointer
// while detached, the playhead reaching 85 % of a detached view glides it
// back, and seeks, track changes, Reset Zoom and Follow Playhead bring it
// back at once.
class WaveformFollowSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void middleDragDetachesAndPans();
    void middleClickResetsNeitherVisualizationsZoom();
    void wheelZoomsAboutThePlayheadOrThePointer();
    void zoomOutHandoffKeepsTheDetachedView();
    void detachedViewGlidesBackFromItsReattachPoint_data();
    void detachedViewGlidesBackFromItsReattachPoint();
    void playheadPastTheReattachPointLeavesTheView_data();
    void playheadPastTheReattachPointLeavesTheView();
    void ownSeekKeepsTheViewAndOthersBringItBack_data();
    void ownSeekKeepsTheViewAndOthersBringItBack();
    void followPlayheadReattachesAtOnce_data();
    void followPlayheadReattachesAtOnce();
    void gstreamerSeeksAndTrackChangesBringTheViewBack();
    void visibleRangeIsTheViewAFrameShows();
    void autoscrollSpeedRisesSteeplyWithTheDistance();
    void autoscrollMovesByThePointersDistance();
    void autoscrollOfTheWholeTrackDoesNothing();
    void autoscrollArmsTheReattachPointWhenItStops_data();
    void autoscrollArmsTheReattachPointWhenItStops();
    void autoscrollStopsAsTheViewComesBack_data();
    void autoscrollStopsAsTheViewComesBack();
    void autoscrollInAWindow();
};

namespace {

constexpr double kWidth = 400.0;
constexpr double kDuration = 60.0;

std::pair<double, double> viewOf(WaveformEditorItem &item) {
    QMutexLocker lock(&item.m_stateMutex);
    return item.visibleRangeLocked();
}

// The view at the item's zoom, which a zoom out hands over to.
std::pair<double, double> targetViewOf(WaveformEditorItem &item) {
    QMutexLocker lock(&item.m_stateMutex);
    return item.visibleRangeForZoomLocked(item.m_zoomLevel);
}

// The view of the presented zoom that follows the playhead.
std::pair<double, double> followingViewOf(WaveformEditorItem &item) {
    QMutexLocker lock(&item.m_stateMutex);
    const double span = item.visibleSpanForZoomLocked(item.m_presentedZoomLevel);
    const double start = std::clamp(
        item.followingStartLocked(span, item.displayedPositionSecondsLocked()),
        0.0, item.m_durationSeconds - span);
    return {start, start + span};
}

// One device column of the item's view.
double columnOf(WaveformEditorItem &item) {
    const auto [start, end] = viewOf(item);
    return (end - start) / kWidth;
}

double timeAt(const std::pair<double, double> &view, double x) {
    return view.first + x / kWidth * (view.second - view.first);
}

// Where the playhead is in the view's width, from 0 to 1.
double playheadFraction(WaveformEditorItem &item) {
    const auto [start, end] = viewOf(item);
    return (item.positionSeconds() - start) / (end - start);
}

bool sameView(const std::pair<double, double> &a, const std::pair<double, double> &b) {
    return std::abs(a.first - b.first) < 1e-9 && std::abs(a.second - b.second) < 1e-9;
}

QString describe(const std::pair<double, double> &view) {
    return QStringLiteral("%1..%2").arg(view.first, 0, 'f', 6).arg(view.second, 0, 'f', 6);
}

void sendMouse(WaveformEditorItem &item, QEvent::Type type, double x,
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
void middleDrag(WaveformEditorItem &item, double fromX, double toX) {
    sendMouse(item, QEvent::MouseButtonPress, fromX, Qt::MiddleButton, Qt::MiddleButton);
    for (int step = 1; step <= 4; ++step) {
        sendMouse(item, QEvent::MouseMove, fromX + (toX - fromX) * step / 4.0,
                  Qt::NoButton, Qt::MiddleButton);
    }
    sendMouse(item, QEvent::MouseButtonRelease, toX, Qt::MiddleButton, Qt::NoButton);
}

// A middle click at `x`, which starts autoscroll, or stops it.
void middleClick(WaveformEditorItem &item, double x) {
    middleDrag(item, x, x);
}

// The pointer at `x` of the item, as autoscroll follows it in the window.
void pointerAt(WaveformEditorItem &item, double x) {
    item.m_autoscroll.pointerMoved(item.mapToScene(QPointF(x, 40.0)));
}

void frame(WaveformEditorItem &item);

// `seconds` of the window's frames, 60 a second, as autoscroll moves the
// view with them.
void autoscrollFor(WaveformEditorItem &item, TestTime &time, double seconds) {
    for (int step = 0; step < std::lround(seconds * 60.0); ++step) {
        time.advance(1.0 / 60.0);
        item.m_autoscroll.frameShown();
        frame(item);
    }
}

void wheelAt(WaveformEditorItem &item, double x, int delta) {
    QWheelEvent wheel(QPointF(x, 40.0), QPointF(x, 40.0), QPoint(), QPoint(0, delta),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    item.wheelEvent(&wheel);
}

// A right-click at `x`, which seeks there; returns the time it asked for.
double rightClickAt(WaveformEditorItem &item, double x) {
    QSignalSpy seeks(&item, &WaveformEditorItem::seekRequested);
    sendMouse(item, QEvent::MouseButtonPress, x, Qt::RightButton, Qt::RightButton);
    return seeks.isEmpty() ? std::numeric_limits<double>::quiet_NaN() : seeks.first().first().toDouble();
}

// The engine's sample of a seek that has landed at `seconds`, as
// BridgeClient takes it.
void engineLands(WaveformEditorItem &item, double seconds) {
    PlaybackClock &clock = testClock(item);
    PlaybackSample sample;
    sample.positionSeconds = seconds;
    sample.sampledAtNs = clock.nowNs();
    sample.advancing = item.playing();
    sample.trackToken = clock.state().trackToken;
    clock.applySample(sample, true);
}

// A frame of the window: the item looks at its view again.
void frame(WaveformEditorItem &item) {
    item.handleWindowFrameSwapped();
}

// The item, `kWidth` wide, plays a track of kDuration from `position` at
// `zoom`, on the test's time.
void playZoomed(WaveformEditorItem &item, TestTime &time, double zoom, double position,
                bool playing = true) {
    item.setWidth(kWidth);
    item.setHeight(120.0);
    time.drive(item);
    item.setDurationSeconds(kDuration);
    item.setCrosshairEnabled(true);
    item.setPlaying(playing);
    setTestPosition(item, position, 1);
    item.setZoomLevel(zoom);
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

// The view's range, which the seek bar marks while the view is zoomed, is
// the view a frame of the window showed: following the playhead, panned,
// gliding back, at another zoom, playing or paused, and the whole track at
// the default zoom. None while the track's length is unknown.
void WaveformFollowSmokeTest::visibleRangeIsTheViewAFrameShows() {
    WaveformEditorItem item;
    TestTime time;
    QSignalSpy changes(&item, &WaveformEditorItem::visibleRangeChanged);
    const auto published = [&item]() {
        return std::pair<double, double>{item.visibleStartSeconds(), item.visibleEndSeconds()};
    };
    QVERIFY(std::isnan(item.visibleStartSeconds()) && std::isnan(item.visibleEndSeconds()));
    playZoomed(item, time, 8.0, 30.0);
    item.setSourcePath(QStringLiteral("/fixture/range.wav"));
    item.setOverviewData(QByteArray(512, '\x40'));
    item.setOverviewCoverageSeconds(kDuration);
    item.setOverviewComplete(true);

    // Following, about the playhead: from the zoom, and at a frame.
    QVERIFY(changes.count() > 0);
    QVERIFY2(sameView(published(), viewOf(item)), qPrintable(describe(published())));
    frame(item);
    const int initialChanges = changes.count();
    QVERIFY2(sameView(published(), viewOf(item)), qPrintable(describe(published())));
    QVERIFY(sameView(published(), followingViewOf(item)));
    QCOMPARE(published().second - published().first, kDuration / 8.0);
    QVERIFY(std::abs((published().first + published().second) / 2 - 30.0) <= columnOf(item));
    // Playback moves it on, from the next frame; a frame that shows the
    // same view says nothing.
    const auto before = published();
    time.advance(0.5);
    QVERIFY(sameView(published(), before));
    frame(item);
    QCOMPARE(changes.count(), initialChanges + 1);
    QVERIFY2(sameView(published(), viewOf(item)), qPrintable(describe(published())));
    QVERIFY(std::abs(published().first - (before.first + 0.5)) <= columnOf(item));
    frame(item);
    QCOMPARE(changes.count(), initialChanges + 1);

    // Panned: where the drag put it, from the drag on, as playback goes on.
    middleDrag(item, 300.0, 100.0);
    QVERIFY(item.detached());
    const auto panned = published();
    QVERIFY2(sameView(panned, viewOf(item)), qPrintable(describe(panned)));
    QVERIFY(!sameView(panned, followingViewOf(item)));
    const int pannedChanges = changes.count();
    time.advance(0.5);
    frame(item);
    QVERIFY(sameView(published(), panned));
    QCOMPARE(changes.count(), pannedChanges);

    // Gliding back once the playhead reaches its re-attach point, and
    // following again: the view of every frame.
    bool glided = false;
    for (int step = 0; step < 240; ++step) {
        time.advance(0.05);
        frame(item);
        QVERIFY2(sameView(published(), viewOf(item)),
                 qPrintable(QStringLiteral("%1 at %2").arg(describe(published())).arg(item.positionSeconds())));
        glided = glided || !item.detached();
    }
    QVERIFY(glided && !item.detached());
    QVERIFY2(sameView(published(), followingViewOf(item)), qPrintable(describe(published())));

    // Paused, at another zoom, from the zoom on.
    item.setPlaying(false);
    item.setZoomLevel(2.0);
    QVERIFY2(sameView(published(), viewOf(item)), qPrintable(describe(published())));
    QCOMPARE(published().second - published().first, kDuration / 2.0);

    // At the default zoom, the whole track; without a length, none, from
    // its next frame.
    item.resetZoom();
    QVERIFY(sameView(published(), {0.0, kDuration}));
    item.setDurationSeconds(0.0);
    frame(item);
    QVERIFY(std::isnan(item.visibleStartSeconds()) && std::isnan(item.visibleEndSeconds()));
}

// A middle-drag pans a zoomed view, which then stays where it was put while
// playback goes on; the time grabbed stays under the pointer.
void WaveformFollowSmokeTest::middleDragDetachesAndPans() {
    WaveformEditorItem item;
    TestTime time;
    playZoomed(item, time, 8.0, 30.0);
    // A track whose detail has not come, and its overview.
    item.setSourcePath(QStringLiteral("/fixture/pan.wav"));
    item.setOverviewData(QByteArray(512, '\x40'));
    item.setOverviewCoverageSeconds(kDuration);
    item.setOverviewComplete(true);
    QSignalSpy detachedChanges(&item, &WaveformEditorItem::detachedChanged);
    const double span = kDuration / 8.0;
    const auto following = viewOf(item);
    QVERIFY(sameView(following, followingViewOf(item)));

    // A click, or a move short of a drag, pans nothing: it starts
    // autoscroll, which holds the view where it is until the pointer moves.
    for (const double to : {200.0, 201.0}) {
        middleDrag(item, 200.0, to);
        QVERIFY(item.m_autoscroll.active());
        QVERIFY(item.detached());
        QCOMPARE(item.zoomLevel(), 8.0);
        QVERIFY(sameView(viewOf(item), following));
        item.followPlayhead();
        QVERIFY(!item.m_autoscroll.active());
    }
    QVERIFY(!item.detached());
    QCOMPARE(detachedChanges.count(), 4);
    detachedChanges.clear();

    // 200 px to the left: the view moves on by half its span. A drag starts
    // no autoscroll.
    middleDrag(item, 300.0, 100.0);
    QVERIFY(item.detached());
    QVERIFY(!item.m_autoscroll.active());
    QCOMPARE(detachedChanges.count(), 1);
    const auto panned = viewOf(item);
    QVERIFY2(std::abs(panned.first - (following.first + span / 2)) < columnOf(item),
             qPrintable(describe(panned)));
    QCOMPARE(panned.second - panned.first, span);
    QCOMPARE(item.zoomLevel(), 8.0);

    // A drag past the cache draws it again at once, from the overview,
    // rather than leave the view empty until the detail comes.
    {
        QImage image(static_cast<int>(kWidth), 120, QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&image);
        item.paint(&painter);
    }
    QVERIFY(!item.m_cacheDirty);
    // A span back: the cache of playback reaches a quarter of one behind.
    middleDrag(item, 0.0, kWidth);
    QVERIFY(item.m_cacheDirty);
    middleDrag(item, kWidth, 0.0);
    const auto pannedBack = viewOf(item);
    QVERIFY2(std::abs(pannedBack.first - panned.first) < columnOf(item), qPrintable(describe(pannedBack)));

    // Playback goes on; the view stays.
    for (int step = 0; step < 10; ++step) {
        time.advance(0.1);
        frame(item);
        QVERIFY(sameView(viewOf(item), pannedBack));
    }
    QVERIFY(item.positionSeconds() > 30.9);
    QVERIFY(item.detached());

    // Past the track's start, the view is held there; a move back moves it
    // at once.
    sendMouse(item, QEvent::MouseButtonPress, 0.0, Qt::MiddleButton, Qt::MiddleButton);
    for (const double x : {2000.0, 4000.0, 8000.0}) {
        sendMouse(item, QEvent::MouseMove, x, Qt::NoButton, Qt::MiddleButton);
        QCOMPARE(viewOf(item).first, 0.0);
    }
    sendMouse(item, QEvent::MouseMove, 7900.0, Qt::NoButton, Qt::MiddleButton);
    QVERIFY2(std::abs(viewOf(item).first - span / 4) < columnOf(item), qPrintable(describe(viewOf(item))));
    sendMouse(item, QEvent::MouseButtonRelease, 7900.0, Qt::MiddleButton, Qt::NoButton);
    QVERIFY(item.detached());
    QCOMPARE(detachedChanges.count(), 1);
}

// A middle click resets neither plot's zoom: Reset Zoom is Ctrl+0, View ›
// Reset Zoom and the viewer's Reset. It starts autoscroll instead.
void WaveformFollowSmokeTest::middleClickResetsNeitherVisualizationsZoom() {
    WaveformEditorItem waveform;
    TestTime time;
    playZoomed(waveform, time, 4.0, 20.0);
    const auto view = viewOf(waveform);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(50.0, 40.0), QPointF(50.0, 40.0),
        Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    waveform.mousePressEvent(&press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(50.0, 40.0), QPointF(50.0, 40.0),
        Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
    waveform.mouseReleaseEvent(&release);
    QCOMPARE(waveform.zoomLevel(), 4.0);
    QVERIFY(waveform.m_autoscroll.active());
    QVERIFY(sameView(viewOf(waveform), view));

    SpectrogramItem spectrogram;
    spectrogram.setWidth(kWidth);
    spectrogram.setHeight(120.0);
    spectrogram.setZoomEnabled(true);
    spectrogram.setDisplayMode(1);
    QSignalSpy resets(&spectrogram, &SpectrogramItem::zoomResetRequested);
    QSignalSpy zooms(&spectrogram, &SpectrogramItem::zoomRequested);
    const double zoom = spectrogram.zoomLevel();
    QMouseEvent click(QEvent::MouseButtonPress, QPointF(50.0, 40.0), QPointF(50.0, 40.0),
        Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    spectrogram.mousePressEvent(&click);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(zooms.count(), 0);
    QCOMPARE(spectrogram.zoomLevel(), zoom);
}

// Following, the wheel zooms about the playhead and the view follows on;
// detached, about the pointer, also while a zoom out is handed over.
void WaveformFollowSmokeTest::wheelZoomsAboutThePlayheadOrThePointer() {
    WaveformEditorItem item;
    TestTime time;
    playZoomed(item, time, 8.0, 30.0);
    // A cache of the overview, which a zoom out shows until it has the
    // detail of the wider view.
    item.setOverviewData(QByteArray(512, '\x40'));
    item.setOverviewCoverageSeconds(kDuration);
    item.setOverviewComplete(true);
    const auto draw = [&item]() {
        QImage image(static_cast<int>(kWidth), 120, QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&image);
        item.paint(&painter);
    };

    wheelAt(item, 50.0, 120);
    QCOMPARE(item.zoomLevel(), 10.0);
    QVERIFY(!item.detached());
    QVERIFY(sameView(viewOf(item), followingViewOf(item)));
    QVERIFY(std::abs(playheadFraction(item) - 0.5) < 1.0 / kWidth + 1e-9);

    middleDrag(item, 300.0, 100.0);
    QVERIFY(item.detached());

    // In: the time under the pointer stays there.
    const double grabbed = timeAt(viewOf(item), 100.0);
    wheelAt(item, 100.0, 120);
    QCOMPARE(item.zoomLevel(), 12.5);
    QVERIFY(item.detached());
    QVERIFY2(std::abs(timeAt(viewOf(item), 100.0) - grabbed) <= columnOf(item),
             qPrintable(describe(viewOf(item))));

    // Out, from a view drawn in a cache: the cache shows until the wider
    // view's detail comes, and the zoom it goes to keeps the time under
    // the pointer too.
    draw();
    const auto shown = viewOf(item);
    const double pointed = timeAt(shown, 300.0);
    wheelAt(item, 300.0, -240);
    QCOMPARE(item.zoomLevel(), 8.0);
    QVERIFY(item.m_zoomOutHandoffPending);
    QVERIFY2(sameView(viewOf(item), shown), qPrintable(describe(viewOf(item))));
    const auto target = targetViewOf(item);
    QVERIFY2(std::abs(timeAt(target, 300.0) - pointed) <= (target.second - target.first) / kWidth,
             qPrintable(describe(target)));
    QVERIFY(item.detached());
}

// A zoom out that waits for the wider view's detail keeps showing the
// detached view until it is handed over: the playhead past the re-attach
// point of the wider view leaves it where it is, and a zoom out to the
// whole track shows the detached view until the whole track shows.
void WaveformFollowSmokeTest::zoomOutHandoffKeepsTheDetachedView() {
    WaveformEditorItem item;
    TestTime time;
    playZoomed(item, time, 8.0, 30.0);
    item.setSourcePath(QStringLiteral("/fixture/handoff.wav"));
    item.setOverviewData(QByteArray(512, '\x40'));
    item.setOverviewCoverageSeconds(kDuration);
    item.setOverviewComplete(true);
    const auto draw = [&item]() {
        QImage image(static_cast<int>(kWidth), 120, QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&image);
        item.paint(&painter);
    };
    const auto commitHandoff = [&item]() {
        {
            QMutexLocker lock(&item.m_stateMutex);
            item.invalidateCacheLocked();
        }
        item.publishDetached();
    };

    // The playhead at 82 %, short of the re-attach point; zoomed out about
    // 95 %, it is past the wider view's.
    middleDrag(item, 0.0, 0.32 * kWidth);
    QVERIFY(std::abs(playheadFraction(item) - 0.82) < 2.0 / kWidth);
    draw();
    const auto shown = viewOf(item);
    wheelAt(item, 0.95 * kWidth, -240);
    QVERIFY(item.m_zoomOutHandoffPending);
    const auto target = targetViewOf(item);
    QVERIFY((item.positionSeconds() - target.first) / (target.second - target.first) > 0.85);
    time.advance(0.05);
    frame(item);
    QVERIFY(item.detached());
    QVERIFY(sameView(viewOf(item), shown));
    QVERIFY(sameView(targetViewOf(item), target));
    commitHandoff();
    for (int step = 0; step < 20; ++step) {
        time.advance(0.1);
        frame(item);
    }
    QVERIFY(item.detached());
    QVERIFY(sameView(viewOf(item), target));

    // Out to the whole track: the detached view, and Follow Playhead,
    // stay until it is handed over, here by a switch of the lanes; the
    // frame that shows the whole track ends it.
    draw();
    const auto before = viewOf(item);
    wheelAt(item, 200.0, -120 * 12);
    QCOMPARE(item.zoomLevel(), 1.0);
    QVERIFY(item.m_zoomOutHandoffPending);
    QVERIFY(item.detached());
    QVERIFY(sameView(viewOf(item), before));
    item.setViewMode(1);
    QVERIFY(!item.m_zoomOutHandoffPending);
    QCOMPARE(viewOf(item), std::make_pair(0.0, kDuration));
    draw();
    QTRY_VERIFY(!item.detached());

    // A playhead that reaches the re-attach point of the view still shown
    // glides it, though the wider view is short of its own; the glide goes
    // on in the wider view once it shows, the playhead where it was.
    item.setViewMode(0);
    item.setZoomLevel(8.0);
    QVERIFY(!item.detached());
    middleDrag(item, 0.0, 0.34 * kWidth);
    QVERIFY(std::abs(playheadFraction(item) - 0.84) < 2.0 / kWidth);
    draw();
    wheelAt(item, 0.0, -120);
    QVERIFY(item.m_zoomOutHandoffPending);
    const auto wider = targetViewOf(item);
    QVERIFY((item.positionSeconds() - wider.first) / (wider.second - wider.first) < 0.7);
    for (int step = 0; step < 60; ++step) {
        time.advance(1.0 / 240.0);
        frame(item);
        const auto view = viewOf(item);
        QVERIFY2(item.positionSeconds() >= view.first && item.positionSeconds() <= view.second,
                 qPrintable(describe(view)));
        QVERIFY(playheadFraction(item) <= 0.85 + 2.0 / kWidth);
    }
    QVERIFY(!item.detached());
    const double fraction = playheadFraction(item);
    commitHandoff();
    QVERIFY(!item.m_zoomOutHandoffPending);
    QCOMPARE(item.m_follow, WaveformEditorItem::Follow::Gliding);
    QVERIFY2(std::abs(playheadFraction(item) - fraction) < 2.0 / kWidth,
             qPrintable(QStringLiteral("%1 then %2").arg(fraction).arg(playheadFraction(item))));
    for (int step = 0; step < 240 * 3; ++step) {
        time.advance(1.0 / 240.0);
        frame(item);
    }
    QCOMPARE(item.m_follow, WaveformEditorItem::Follow::Playhead);
    QVERIFY(sameView(viewOf(item), followingViewOf(item)));

    // Past the re-attach point of the view shown, but short of the wider
    // view's: the wider view glides once the playhead reaches its own.
    item.setZoomLevel(8.0);
    middleDrag(item, 0.0, 0.4 * kWidth);
    QVERIFY(std::abs(playheadFraction(item) - 0.9) < 2.0 / kWidth);
    draw();
    wheelAt(item, 0.0, -120);
    QVERIFY(item.m_zoomOutHandoffPending);
    const auto widened = targetViewOf(item);
    QVERIFY((item.positionSeconds() - widened.first) / (widened.second - widened.first) < 0.75);
    time.advance(0.05);
    frame(item);
    QVERIFY(item.detached());
    commitHandoff();
    QVERIFY(sameView(viewOf(item), widened));
    for (int step = 0; step < 240 * 5 && item.m_follow != WaveformEditorItem::Follow::Playhead; ++step) {
        time.advance(1.0 / 240.0);
        frame(item);
        const auto view = viewOf(item);
        QVERIFY2(item.positionSeconds() >= view.first && item.positionSeconds() <= view.second,
                 qPrintable(describe(view)));
        QVERIFY(playheadFraction(item) <= 0.85 + 2.0 / kWidth);
    }
    QCOMPARE(item.m_follow, WaveformEditorItem::Follow::Playhead);

    // Past the view shown, but short of the wider view's re-attach point:
    // the playhead reaching that point while the wider view waits for its
    // detail leaves the view shown where it is; the wider view glides.
    middleDrag(item, 0.0, 0.6 * kWidth);
    QVERIFY(playheadFraction(item) > 1.0);
    draw();
    const auto putAside = viewOf(item);
    wheelAt(item, 0.0, -240);
    QVERIFY(item.m_zoomOutHandoffPending);
    const auto aside = targetViewOf(item);
    QVERIFY((item.positionSeconds() - aside.first) / (aside.second - aside.first) < 0.75);
    // The playhead is at 70 % of the wider view, its point 2.14 s on.
    for (int step = 0; step < 600; ++step) {
        time.advance(1.0 / 240.0);
        frame(item);
        QVERIFY(sameView(viewOf(item), putAside));
    }
    QVERIFY(item.detached());
    const auto gliding = targetViewOf(item);
    QVERIFY(!sameView(gliding, aside));
    QVERIFY((item.positionSeconds() - gliding.first) / (gliding.second - gliding.first) <= 0.85 + 2.0 / kWidth);
    commitHandoff();
    QCOMPARE(item.m_follow, WaveformEditorItem::Follow::Gliding);
    QVERIFY(sameView(viewOf(item), gliding));
    for (int step = 0; step < 240 * 3 && item.m_follow != WaveformEditorItem::Follow::Playhead; ++step) {
        time.advance(1.0 / 240.0);
        frame(item);
        QVERIFY(playheadFraction(item) <= 0.85 + 2.0 / kWidth);
    }
    QCOMPARE(item.m_follow, WaveformEditorItem::Follow::Playhead);
}

void WaveformFollowSmokeTest::detachedViewGlidesBackFromItsReattachPoint_data() {
    QTest::addColumn<double>("zoom");
    QTest::addRow("7.5 s") << 8.0;
    // 30 ms: the glide's 2 s are 66 views.
    QTest::addRow("30 ms") << 2000.0;
}

// A playhead that moves through a detached view and reaches 85 % of its
// width starts the view gliding back: faster than playback, it brings the
// playhead to the middle over 2 s of playback, then follows it there. The
// playhead never leaves the view, however deep the zoom.
void WaveformFollowSmokeTest::detachedViewGlidesBackFromItsReattachPoint() {
    QFETCH(double, zoom);
    WaveformEditorItem item;
    TestTime time;
    playZoomed(item, time, zoom, 30.0);
    QSignalSpy detachedChanges(&item, &WaveformEditorItem::detachedChanged);
    const double span = kDuration / zoom;
    const double column = span / kWidth;
    // The view a quarter of its span ahead: the playhead at a quarter.
    middleDrag(item, 300.0, 200.0);
    QVERIFY(item.detached());
    QVERIFY(std::abs(playheadFraction(item) - 0.25) < 2.0 / kWidth);
    const auto detached = viewOf(item);

    constexpr double kStep = 1.0 / 240.0;
    double glideStarted = -1.0;
    double following = -1.0;
    double elapsed = 0.0;
    auto previous = detached;
    double previousPosition = item.positionSeconds();
    while (elapsed < 0.75 * span + 3.0) {
        time.advance(kStep);
        elapsed += kStep;
        frame(item);
        const auto view = viewOf(item);
        const double position = item.positionSeconds();
        const double fraction = playheadFraction(item);
        QVERIFY2(position >= view.first && position <= view.second,
                 qPrintable(QStringLiteral("%1 s in: playhead at %2, view %3")
                                .arg(elapsed).arg(position, 0, 'f', 6).arg(describe(view))));
        QVERIFY2(fraction <= 0.85 + 2.0 / kWidth, qPrintable(QString::number(fraction)));
        double frameChange = 0.0;
        double speed = 0.0;
        {
            QMutexLocker lock(&item.m_stateMutex);
            frameChange = item.frameChangeSecondsLocked(view.first, view.second, position, int(kWidth));
            speed = item.scrollPixelsPerSecondLocked(int(kWidth), position);
        }
        if (item.detached()) {
            // Still: drawn again as the playhead reaches its next column.
            QVERIFY(sameView(view, detached));
            QVERIFY(frameChange > position && frameChange <= position + column);
            QCOMPARE(speed, 0.0);
        } else if (following < 0.0 && item.m_follow == WaveformEditorItem::Follow::Gliding) {
            if (glideStarted < 0.0) glideStarted = elapsed;
            // Every frame differs, and the view moves on by more than the
            // playhead, from the playhead's place, not a clock of its own.
            QVERIFY(frameChange > position && frameChange - position < 1e-9);
            QVERIFY(speed >= kWidth / span);
            // From the frame before, past the step the glide began in.
            if (glideStarted < elapsed) {
                QVERIFY2(view.first - previous.first >= position - previousPosition - column - 1e-9,
                         qPrintable(QStringLiteral("view %1 after %2").arg(describe(view), describe(previous))));
            }
        } else if (following < 0.0) {
            QCOMPARE(item.m_follow, WaveformEditorItem::Follow::Playhead);
            following = elapsed;
        }
        previous = view;
        previousPosition = position;
    }
    QVERIFY(glideStarted > 0.0);
    QVERIFY(following > 0.0);
    // 2 s of playback, from where the playhead was 85 % into the view.
    QVERIFY2(std::abs(following - glideStarted - 2.0) < 2 * kStep,
             qPrintable(QStringLiteral("glided %1 s").arg(following - glideStarted)));
    QVERIFY2(std::abs(glideStarted - 0.6 * span) < 2 * kStep + 2 * column,
             qPrintable(QStringLiteral("glide began %1 s in").arg(glideStarted)));
    // It follows, with the playhead on the middle column.
    QVERIFY(!item.detached());
    QVERIFY(sameView(viewOf(item), followingViewOf(item)));
    QVERIFY(std::abs(playheadFraction(item) - 0.5) < 1.0 / kWidth + 1e-9);
    QCOMPARE(detachedChanges.count(), 2);
}

void WaveformFollowSmokeTest::playheadPastTheReattachPointLeavesTheView_data() {
    QTest::addColumn<double>("dragBy");
    // The view a whole span back: the playhead is past its right edge.
    QTest::addRow("past the view") << kWidth;
    // 0.4 of a span back: the playhead at 90 %, past the re-attach point,
    // as the drag ends.
    QTest::addRow("past the re-attach point") << 0.4 * kWidth;
}

// A playhead past a detached view, or past its re-attach point when the
// drag ends, leaves the view where the user put it: re-attaching starts
// only once the playhead crosses that point from the left, as after the
// plot's own seek to the start of the view.
void WaveformFollowSmokeTest::playheadPastTheReattachPointLeavesTheView() {
    QFETCH(double, dragBy);
    WaveformEditorItem item;
    TestTime time;
    playZoomed(item, time, 8.0, 30.0);
    const double span = kDuration / 8.0;
    middleDrag(item, 0.0, dragBy);
    QVERIFY(item.detached());
    QVERIFY(playheadFraction(item) > 0.85);
    const auto put = viewOf(item);
    for (int step = 0; step < 60; ++step) {
        time.advance(0.1);
        frame(item);
        QVERIFY(sameView(viewOf(item), put));
    }
    QVERIFY(item.positionSeconds() > put.second);
    QVERIFY(item.detached());

    // The plot's own seek to a tenth of the view keeps it; the playhead
    // then crosses the re-attach point from the left.
    const double target = rightClickAt(item, 0.1 * kWidth);
    QVERIFY(std::abs(target - timeAt(put, 0.1 * kWidth)) < span / kWidth);
    testClock(item).holdAt(target);
    engineLands(item, target + 0.02);
    QVERIFY(item.detached());
    QVERIFY(sameView(viewOf(item), put));
    for (int step = 0; step < 240 * 10 && item.m_follow != WaveformEditorItem::Follow::Playhead; ++step) {
        time.advance(1.0 / 240.0);
        frame(item);
    }
    QVERIFY(!item.detached());
    QCOMPARE(item.m_follow, WaveformEditorItem::Follow::Playhead);
    QVERIFY(sameView(viewOf(item), followingViewOf(item)));
}

void WaveformFollowSmokeTest::ownSeekKeepsTheViewAndOthersBringItBack_data() {
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
    QTest::addRow("next track") << QStringLiteral("next track") << false;
    QTest::addRow("reset zoom") << QStringLiteral("reset zoom") << false;
}

// A right-click seek in a detached view keeps it, as it lands and as the
// engine confirms it; any other seek, a stop, a track change, including a
// gapless successor and Repeat-One, and Reset Zoom bring the view back to
// the playhead.
void WaveformFollowSmokeTest::ownSeekKeepsTheViewAndOthersBringItBack() {
    QFETCH(QString, event);
    QFETCH(bool, keeps);
    WaveformEditorItem item;
    TestTime time;
    playZoomed(item, time, 8.0, 30.0, !event.endsWith(QStringLiteral("paused")));
    item.setSourcePath(QStringLiteral("/fixture/one.wav"));
    // Behind the playhead, so that nothing glides back.
    middleDrag(item, 0.0, kWidth);
    QVERIFY(item.detached());
    const auto put = viewOf(item);
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
               || event == QStringLiteral("repeat one")
               || event == QStringLiteral("next track")) {
        // A gapless successor and Repeat-One start without a seek; only the
        // track token tells them apart. Repeat-One plays the same file.
        if (event == QStringLiteral("next track")) {
            item.setSourcePath(QStringLiteral("/fixture/two.wav"));
        }
        PlaybackSample next;
        next.positionSeconds = 0.0;
        next.sampledAtNs = clock.nowNs();
        next.advancing = true;
        next.trackToken = token + 1;
        clock.applySample(next, false);
    } else {
        item.resetZoom();
        QCOMPARE(item.zoomLevel(), 1.0);
    }
    frame(item);
    QCOMPARE(item.detached(), keeps);
    if (keeps) {
        QVERIFY(sameView(viewOf(item), put));
    } else {
        QCOMPARE(item.m_follow, WaveformEditorItem::Follow::Playhead);
        QVERIFY(sameView(viewOf(item), followingViewOf(item)));
    }
}

void WaveformFollowSmokeTest::followPlayheadReattachesAtOnce_data() {
    QTest::addColumn<bool>("playing");
    QTest::addRow("playing") << true;
    QTest::addRow("paused") << false;
}

// Follow Playhead brings a detached view back to the playhead at once,
// playing or paused; following, it does nothing.
void WaveformFollowSmokeTest::followPlayheadReattachesAtOnce() {
    QFETCH(bool, playing);
    WaveformEditorItem item;
    TestTime time;
    playZoomed(item, time, 8.0, 30.0, playing);
    QSignalSpy detachedChanges(&item, &WaveformEditorItem::detachedChanged);
    const auto following = viewOf(item);
    item.followPlayhead();
    QCOMPARE(detachedChanges.count(), 0);
    QVERIFY(sameView(viewOf(item), following));

    middleDrag(item, 0.0, 300.0);
    QVERIFY(item.detached());
    QVERIFY(!sameView(viewOf(item), following));
    item.followPlayhead();
    QVERIFY(!item.detached());
    QCOMPARE(detachedChanges.count(), 2);
    QVERIFY(sameView(viewOf(item), followingViewOf(item)));
    QVERIFY(std::abs(playheadFraction(item) - 0.5) < 1.0 / kWidth + 1e-9);
}

// With the real bridge and the GStreamer backend, playing in real time
// into a fake audio sink: the plot's own seek keeps a detached view as the
// engine confirms it, and a seek from elsewhere, a gapless successor and
// Repeat-One bring it back.
void WaveformFollowSmokeTest::gstreamerSeeksAndTrackChangesBringTheViewBack() {
    // The tests' fakesink plays as fast as it can; fakeaudiosink, from the
    // GStreamer bad plugins the app requires, keeps to the clock. Without
    // it, the backend would fall back to the desktop's audio output.
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
    constexpr double kTrackSeconds = 6.0;
    QVERIFY(writeWav(first, kTrackSeconds));
    QVERIFY(writeWav(second, kTrackSeconds));
    BridgeClient bridge;
    bridge.setRepeatMode(0);
    bridge.clearQueue();
    bridge.replaceWithPaths({first, second});
    const bool started = QTest::qWaitFor([&]() {
        return bridge.playbackState() == QStringLiteral("Playing")
            && bridge.currentTrackPath() == first
            && bridge.playbackClock()->currentPositionSeconds() > 0.2;
    }, 10000);
    QVERIFY2(started, qPrintable(QStringLiteral("%1 %2 at %3").arg(bridge.playbackState(), bridge.currentTrackPath()).arg(bridge.playbackClock()->currentPositionSeconds())));

    WaveformEditorItem item;
    item.setWidth(kWidth);
    item.setHeight(120.0);
    item.setCrosshairEnabled(true);
    item.setDurationSeconds(kTrackSeconds);
    item.setPlaying(true);
    item.setClock(bridge.playbackClock());
    item.setZoomLevel(8.0);
    QObject::connect(&item, &WaveformEditorItem::seekRequested, &bridge, &BridgeClient::seek);
    const auto detachBehind = [&item]() {
        // The view a whole span back, so that it stays.
        middleDrag(item, 0.0, kWidth);
        return item.detached();
    };
    // The clock holds a seek's target until the engine has done it.
    const auto engineDone = [&bridge]() {
        return bridge.playbackClock()->advancing();
    };

    // The plot's own seek, past the re-attach point of a view behind the
    // playhead, keeps the view while the engine seeks and confirms it.
    bridge.seek(3.0);
    QTRY_VERIFY_WITH_TIMEOUT(engineDone(), 10000);
    QVERIFY(detachBehind());
    const auto put = viewOf(item);
    rightClickAt(item, 0.95 * kWidth);
    QTRY_VERIFY_WITH_TIMEOUT(engineDone(), 10000);
    QVERIFY(item.detached());
    QVERIFY(sameView(viewOf(item), put));

    // A seek from elsewhere, as the seek bar's, brings it back.
    bridge.seek(0.5);
    QTRY_VERIFY_WITH_TIMEOUT(!item.detached(), 10000);
    QVERIFY(sameView(viewOf(item), followingViewOf(item)));

    // A gapless successor.
    bridge.seek(kTrackSeconds - 1.5);
    QTRY_VERIFY_WITH_TIMEOUT(engineDone(), 10000);
    QVERIFY(detachBehind());
    QTRY_VERIFY_WITH_TIMEOUT(bridge.currentTrackPath() == second, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!item.detached(), 10000);

    // Repeat-One: the same file again.
    bridge.setRepeatMode(1);
    bridge.seek(kTrackSeconds - 1.5);
    QTRY_VERIFY_WITH_TIMEOUT(engineDone(), 10000);
    QVERIFY(detachBehind());
    const quint64 token = bridge.playbackClock()->state().trackToken;
    QTRY_VERIFY_WITH_TIMEOUT(bridge.playbackClock()->state().trackToken != token, 10000);
    QCOMPARE(bridge.currentTrackPath(), second);
    QTRY_VERIFY_WITH_TIMEOUT(!item.detached(), 10000);

    // Autoscroll, which the plot's own seek leaves running, and which a
    // seek from elsewhere stops, bringing the view back.
    bridge.setRepeatMode(0);
    bridge.seek(3.0);
    QTRY_VERIFY_WITH_TIMEOUT(engineDone(), 10000);
    middleClick(item, 200.0);
    QVERIFY(item.m_autoscroll.active() && item.detached());
    rightClickAt(item, 300.0);
    QTRY_VERIFY_WITH_TIMEOUT(engineDone(), 10000);
    QVERIFY(item.m_autoscroll.active() && item.detached());
    bridge.seek(0.5);
    QTRY_VERIFY_WITH_TIMEOUT(!item.m_autoscroll.active(), 10000);
    QVERIFY(!item.detached());
    QVERIFY(sameView(viewOf(item), followingViewOf(item)));

    // A track change stops it: Repeat-One, the same file again.
    bridge.setRepeatMode(1);
    bridge.seek(kTrackSeconds - 1.5);
    QTRY_VERIFY_WITH_TIMEOUT(engineDone(), 10000);
    middleClick(item, 200.0);
    QVERIFY(item.m_autoscroll.active());
    const quint64 repeatToken = bridge.playbackClock()->state().trackToken;
    QTRY_VERIFY_WITH_TIMEOUT(bridge.playbackClock()->state().trackToken != repeatToken, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!item.m_autoscroll.active(), 10000);
    QVERIFY(!item.detached());

    bridge.setRepeatMode(0);
    bridge.stop();
    bridge.clearQueue();
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 0, 10000);
}

// The speed curve: none within the dead zone, either side, then rising
// as the square of the distance past it, kEdgeViewsPerSecond at half the
// plot's width from the click, on past the plot up to kMaxViewsPerSecond,
// and towards the side the pointer is on.
void WaveformFollowSmokeTest::autoscrollSpeedRisesSteeplyWithTheDistance() {
    using A = ViewAutoscroll;
    constexpr double kPlot = 1'000.0;
    const double reach = kPlot / 2.0 - A::kDeadZonePixels;
    QCOMPARE(A::viewsPerSecond(0.0, kPlot), 0.0);
    QCOMPARE(A::viewsPerSecond(A::kDeadZonePixels, kPlot), 0.0);
    QCOMPARE(A::viewsPerSecond(-A::kDeadZonePixels, kPlot), 0.0);
    QVERIFY(A::viewsPerSecond(A::kDeadZonePixels + 1.0, kPlot) > 0.0);
    QVERIFY(A::viewsPerSecond(-A::kDeadZonePixels - 1.0, kPlot) < 0.0);
    // Four views a second at the plot's edge, from a click in its middle.
    QCOMPARE(A::kEdgeViewsPerSecond, 4.0);
    QVERIFY(std::abs(A::viewsPerSecond(kPlot / 2.0, kPlot) - 4.0) < 1e-12);
    QVERIFY(std::abs(A::viewsPerSecond(-kPlot / 2.0, kPlot) + 4.0) < 1e-12);
    // Fine near the click: a quarter of that half way out.
    QVERIFY(std::abs(A::viewsPerSecond(A::kDeadZonePixels + reach / 2.0, kPlot) - 1.0) < 1e-12);
    // On past the plot, up to the cap, which it keeps.
    QVERIFY(A::viewsPerSecond(kPlot * 0.6, kPlot) > 4.0);
    QVERIFY(A::viewsPerSecond(kPlot * 0.6, kPlot) < A::kMaxViewsPerSecond);
    QCOMPARE(A::viewsPerSecond(kPlot * 5.0, kPlot), A::kMaxViewsPerSecond);
    QCOMPARE(A::viewsPerSecond(-kPlot * 5.0, kPlot), -A::kMaxViewsPerSecond);
    // Monotonic, and odd.
    double last = 0.0;
    for (double offset = 0.0; offset <= kPlot * 2.0; offset += 0.5) {
        const double speed = A::viewsPerSecond(offset, kPlot);
        QVERIFY2(speed >= last, qPrintable(QString::number(offset)));
        QCOMPARE(A::viewsPerSecond(-offset, kPlot), -speed);
        last = speed;
    }
    QCOMPARE(last, A::kMaxViewsPerSecond);
    // Nothing for a distance that is not a number.
    QCOMPARE(A::viewsPerSecond(std::nan(""), kPlot), 0.0);
}

// Autoscroll after a middle click moves the view by the pointer's distance
// from the click (see autoscrollSpeedRisesSteeplyWithTheDistance), right
// of the click towards the end of the track and left of it towards the
// start. It asks for frames only while the view moves.
void WaveformFollowSmokeTest::autoscrollMovesByThePointersDistance() {
    WaveformEditorItem item;
    TestTime time;
    playZoomed(item, time, 8.0, 30.0, false);
    const double span = kDuration / 8.0;
    const double column = span / kWidth;
    const auto following = viewOf(item);
    middleClick(item, 200.0);
    QVERIFY(item.m_autoscroll.active());
    QVERIFY(item.detached());
    QVERIFY(sameView(viewOf(item), following));

    // Within the dead zone, either side, the view holds still and asks for
    // no frames.
    const quint64 requests = item.m_playbackFrameRequests;
    for (const double x : {200.0, 211.0, 189.0}) {
        pointerAt(item, x);
        autoscrollFor(item, time, 0.5);
        QVERIFY(sameView(viewOf(item), following));
        QCOMPARE(item.m_autoscroll.m_lastFrameNs, qint64(0));
    }
    QCOMPARE(item.m_playbackFrameRequests, requests);

    // Speed and direction against the distance: a view a second half way
    // to the plot's edge, four at its edge, eight, at most, past it.
    const double reach = kWidth / 2.0 - ViewAutoscroll::kDeadZonePixels;
    struct Case { double x; double viewsPerSecond; };
    for (const Case &c : {Case{200.0 + 12.0 + reach / 2.0, 1.0}, Case{400.0, 4.0},
                          Case{0.0, -4.0}, Case{200.0 - 12.0 - reach / 2.0, -1.0},
                          Case{2000.0, 8.0}, Case{-2000.0, -8.0}}) {
        pointerAt(item, c.x);
        QVERIFY(std::abs(item.m_autoscroll.currentViewsPerSecond() - c.viewsPerSecond) < 1e-9);
        const auto before = viewOf(item);
        autoscrollFor(item, time, 0.25);
        const double moved = viewOf(item).first - before.first;
        QVERIFY2(std::abs(moved - c.viewsPerSecond * span * 0.25) <= column + 1e-9,
                 qPrintable(QStringLiteral("%1 px: moved %2").arg(c.x).arg(moved)));
        QCOMPARE(viewOf(item).second - viewOf(item).first, span);
        QVERIFY(item.detached());
    }

    // Held at the end of the track, it asks for no more frames; a move back
    // moves the view from there at once.
    pointerAt(item, 2000.0);
    autoscrollFor(item, time, 8.0);
    QCOMPARE(viewOf(item).second, kDuration);
    QCOMPARE(item.m_autoscroll.m_lastFrameNs, qint64(0));
    pointerAt(item, 0.0);
    autoscrollFor(item, time, 0.25);
    QVERIFY(std::abs(viewOf(item).second - (kDuration - span)) <= column + 1e-9);

    // The wheel zooms about the pointer meanwhile, and autoscroll goes on
    // at the new zoom.
    pointerAt(item, 300.0);
    const double underPointer = timeAt(viewOf(item), 300.0);
    wheelAt(item, 300.0, 120);
    QVERIFY(item.m_autoscroll.active() && item.detached());
    QVERIFY(std::abs(timeAt(viewOf(item), 300.0) - underPointer) <= columnOf(item) + 1e-9);
    const auto zoomed = viewOf(item);
    QVERIFY(zoomed.second - zoomed.first < span);
    autoscrollFor(item, time, 0.5);
    QVERIFY(viewOf(item).first > zoomed.first);

    // Held at the end of the track, a zoom about the pointer near the
    // plot's right edge makes room to scroll on: it goes on at once.
    pointerAt(item, 390.0);
    autoscrollFor(item, time, 10.0);
    QCOMPARE(viewOf(item).second, kDuration);
    QCOMPARE(item.m_autoscroll.m_lastFrameNs, qint64(0));
    wheelAt(item, 390.0, 120);
    const auto zoomedAtEnd = viewOf(item);
    QVERIFY(zoomedAtEnd.second < kDuration);
    QVERIFY(item.m_autoscroll.m_lastFrameNs != 0);
    autoscrollFor(item, time, 0.1);
    QVERIFY(viewOf(item).first > zoomedAtEnd.first);

    // Another middle click stops it, where it is.
    const auto stopped = viewOf(item);
    item.m_autoscroll.stop();
    QVERIFY(!item.m_autoscroll.active());
    autoscrollFor(item, time, 0.5);
    QVERIFY(sameView(viewOf(item), stopped));
    QVERIFY(item.detached());

    // The press that stopped it, become a drag, pans; the next click, even
    // one of the same timestamp, starts autoscroll again.
    item.m_autoscroll.m_stopPressTimestamp = 0;
    middleDrag(item, 300.0, 200.0);
    QVERIFY(!item.m_autoscroll.active());
    middleClick(item, 200.0);
    QVERIFY(item.m_autoscroll.active());
}

// A view of the whole track has nowhere to scroll to: a middle click there
// does nothing, nor without zoom at all.
void WaveformFollowSmokeTest::autoscrollOfTheWholeTrackDoesNothing() {
    WaveformEditorItem item;
    TestTime time;
    playZoomed(item, time, 1.0, 30.0);
    middleClick(item, 200.0);
    QVERIFY(!item.m_autoscroll.active());
    QVERIFY(!item.detached());
    item.setZoomEnabled(false);
    item.setZoomLevel(8.0);
    middleClick(item, 200.0);
    QVERIFY(!item.m_autoscroll.active());
    QVERIFY(!item.detached());
}

void WaveformFollowSmokeTest::autoscrollArmsTheReattachPointWhenItStops_data() {
    QTest::addColumn<double>("pointer");
    QTest::addColumn<bool>("glidesBack");
    // The view scrolled on past the playhead: the playhead, left of the
    // re-attach point as it stops, reaches it and glides back.
    QTest::addRow("playhead left of the re-attach point") << 400.0 << true;
    // Scrolled back: the playhead past the view leaves it where it is.
    QTest::addRow("playhead past the view") << 0.0 << false;
}

// While autoscroll runs, nothing glides back, however far the playhead
// goes. When it stops, the view arms its re-attach point as at the end of a
// drag: a playhead left of it glides the view back as it reaches it, one
// past it, or past the view, leaves the view where it is.
void WaveformFollowSmokeTest::autoscrollArmsTheReattachPointWhenItStops() {
    QFETCH(double, pointer);
    QFETCH(bool, glidesBack);
    WaveformEditorItem item;
    TestTime time;
    playZoomed(item, time, 8.0, 30.0);
    const double span = kDuration / 8.0;
    middleClick(item, 200.0);
    // Held still in the dead zone while the playhead plays through the
    // view and past it: no glide.
    const auto held = viewOf(item);
    autoscrollFor(item, time, span);
    QVERIFY(item.positionSeconds() > held.second);
    QVERIFY(sameView(viewOf(item), held));
    QVERIFY(item.detached());
    QCOMPARE(item.m_follow, WaveformEditorItem::Follow::Detached);

    // A view's span on or back from there: four views a second at the
    // plot's edge.
    pointerAt(item, pointer);
    autoscrollFor(item, time, 0.25);
    QVERIFY(item.detached());
    item.m_autoscroll.stop();
    QCOMPARE(item.m_reattachArmed, glidesBack);
    bool glided = false;
    for (int step = 0; step < 60 * 20; ++step) {
        time.advance(1.0 / 60.0);
        frame(item);
        glided = glided || item.m_follow != WaveformEditorItem::Follow::Detached;
    }
    QCOMPARE(glided, glidesBack);
    QCOMPARE(item.detached(), !glidesBack);
}

void WaveformFollowSmokeTest::autoscrollStopsAsTheViewComesBack_data() {
    QTest::addColumn<QString>("event");
    QTest::addColumn<bool>("keeps");
    QTest::addRow("own seek") << QStringLiteral("own seek") << true;
    QTest::addRow("follow playhead") << QStringLiteral("follow playhead") << false;
    QTest::addRow("seek bar") << QStringLiteral("seek bar") << false;
    QTest::addRow("stop") << QStringLiteral("stop") << false;
    QTest::addRow("gapless successor") << QStringLiteral("gapless successor") << false;
    QTest::addRow("repeat one") << QStringLiteral("repeat one") << false;
    QTest::addRow("next track") << QStringLiteral("next track") << false;
    QTest::addRow("reset zoom") << QStringLiteral("reset zoom") << false;
    QTest::addRow("zoom out to the whole track") << QStringLiteral("zoom out") << false;
    QTest::addRow("hidden") << QStringLiteral("hidden") << false;
}

// Follow Playhead, Reset Zoom, a seek other than the plot's own, a stop and
// a track change, including a gapless successor and Repeat-One, stop
// autoscroll and bring the view back to the playhead; the plot's own seek
// keeps both. Hiding the plot, as a switch to the spectrogram does, stops
// it too.
void WaveformFollowSmokeTest::autoscrollStopsAsTheViewComesBack() {
    QFETCH(QString, event);
    QFETCH(bool, keeps);
    WaveformEditorItem item;
    TestTime time;
    playZoomed(item, time, 8.0, 30.0);
    item.setSourcePath(QStringLiteral("/fixture/one.wav"));
    QSignalSpy stops(&item.m_autoscroll, &ViewAutoscroll::stopped);
    middleClick(item, 200.0);
    pointerAt(item, 100.0);
    autoscrollFor(item, time, 0.5);
    QVERIFY(item.m_autoscroll.active() && item.detached());
    const auto scrolled = viewOf(item);
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
               || event == QStringLiteral("repeat one")
               || event == QStringLiteral("next track")) {
        if (event == QStringLiteral("next track")) {
            item.setSourcePath(QStringLiteral("/fixture/two.wav"));
        }
        PlaybackSample next;
        next.positionSeconds = 0.0;
        next.sampledAtNs = clock.nowNs();
        next.advancing = true;
        next.trackToken = token + 1;
        clock.applySample(next, false);
    } else if (event == QStringLiteral("reset zoom")) {
        item.resetZoom();
    } else if (event == QStringLiteral("zoom out")) {
        wheelAt(item, 100.0, -120 * 20);
        QCOMPARE(item.zoomLevel(), 1.0);
    } else {
        item.setVisible(false);
    }
    frame(item);
    QCOMPARE(item.m_autoscroll.active(), keeps);
    QCOMPARE(stops.count(), keeps ? 0 : 1);
    if (keeps) {
        QVERIFY(item.detached());
        QVERIFY(sameView(viewOf(item), scrolled));
        autoscrollFor(item, time, 0.5);
        QVERIFY(viewOf(item).first < scrolled.first);
    } else if (event == QStringLiteral("hidden")) {
        // Stopped as at the end of a drag: where it was.
        QVERIFY(sameView(viewOf(item), scrolled));
    } else {
        QVERIFY(!item.detached());
        QCOMPARE(item.m_follow, WaveformEditorItem::Follow::Playhead);
        QVERIFY(sameView(viewOf(item), followingViewOf(item)));
        // The pointer moves nothing any more.
        pointerAt(item, 0.0);
        autoscrollFor(item, time, 0.5);
        QVERIFY(sameView(viewOf(item), followingViewOf(item)));
    }
}

// In a window: a middle click on the plot starts autoscroll, and the
// pointer, also past the plot, moves the view with the window's frames,
// with a horizontal resize cursor and nothing else to show it. Escape stops
// it before the window's own Escape takes the key, and another middle
// click, anywhere in the window, stops it without panning or starting it
// again. A middle-drag still pans. The window losing focus stops it.
void WaveformFollowSmokeTest::autoscrollInAWindow() {
    QQuickWindow window;
    window.resize(700, 200);
    auto *item = new WaveformEditorItem(window.contentItem());
    item->setX(100.0);
    item->setY(20.0);
    item->setWidth(kWidth);
    item->setHeight(120.0);
    item->setDurationSeconds(kDuration);
    setTestPosition(*item, 30.0, 1);
    item->setZoomLevel(8.0);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QShortcut escape(QKeySequence(Qt::Key_Escape), &window);
    QSignalSpy escapes(&escape, &QShortcut::activated);
    const QPoint click(300, 80);
    const auto following = viewOf(*item);

    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, click);
    QVERIFY(item->m_autoscroll.active());
    QVERIFY(item->detached());
    QVERIFY(QGuiApplication::overrideCursor() != nullptr);
    QCOMPARE(QGuiApplication::overrideCursor()->shape(), Qt::SizeHorCursor);
    // Right of the click: the frames move the view on.
    QTest::mouseMove(&window, QPoint(380, 80));
    QTRY_VERIFY_WITH_TIMEOUT(viewOf(*item).first > following.first + 1.0, 5000);
    const double leaving = item->m_autoscroll.currentViewsPerSecond();
    QVERIFY(leaving > 0.0);
    // Out of the window, whose Leave tells no position, it goes on at the
    // speed it had where the pointer was last seen, with the frames alone
    // and no more events, by whichever edge it left: here the bottom.
    QTest::mouseMove(&window, QPoint(380, 199));
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&window, &leave);
    QCOMPARE(item->m_autoscroll.currentViewsPerSecond(), leaving);
    auto outside = viewOf(*item);
    QTRY_VERIFY_WITH_TIMEOUT(viewOf(*item).first > outside.first + 1.0, 5000);
    QCOMPARE(item->m_autoscroll.currentViewsPerSecond(), leaving);
    // Back in on the other side of the click, it scrolls that way at once,
    // and follows the pointer again.
    QEnterEvent enter(QPointF(20, 80), QPointF(20, 80), window.mapToGlobal(QPointF(20, 80)));
    QCoreApplication::sendEvent(&window, &enter);
    QVERIFY(item->m_autoscroll.currentViewsPerSecond() < 0.0);
    const auto back = viewOf(*item);
    QTRY_VERIFY_WITH_TIMEOUT(viewOf(*item).first < back.first - 1.0, 5000);
    QTest::mouseMove(&window, QPoint(300, 80));
    QCOMPARE(item->m_autoscroll.currentViewsPerSecond(), 0.0);
    // A position past the window's edge, as a window may be told of, is
    // that edge.
    QMouseEvent past(QEvent::MouseMove, QPointF(5'000, 80), QPointF(5'000, 80),
        window.mapToGlobal(QPointF(5'000, 80)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &past);
    QCOMPARE(item->m_autoscroll.m_pointerScene.x(), 700.0);

    // Escape stops it, and only it, with the pointer outside too.
    QTest::mouseMove(&window, QPoint(380, 80));
    QCoreApplication::sendEvent(&window, &leave);
    QTest::keyClick(&window, Qt::Key_Escape);
    QVERIFY(!item->m_autoscroll.active());
    QCOMPARE(escapes.count(), 0);
    QVERIFY(QGuiApplication::overrideCursor() == nullptr);
    QVERIFY(item->detached());
    const auto stopped = viewOf(*item);
    QTest::mouseMove(&window, QPoint(20, 80));
    QTest::qWait(100);
    QVERIFY(sameView(viewOf(*item), stopped));
    QTest::keyClick(&window, Qt::Key_Escape);
    QCOMPARE(escapes.count(), 1);

    // A middle click starts it again; another on the plot stops it, and
    // does not start it again as it is released.
    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, click);
    QVERIFY(item->m_autoscroll.active());
    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, QPoint(320, 80));
    QVERIFY(!item->m_autoscroll.active());
    QVERIFY(sameView(viewOf(*item), stopped));

    // A middle press that stops it, then drags, pans as a drag does.
    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, click);
    QVERIFY(item->m_autoscroll.active());
    QTest::mousePress(&window, Qt::MiddleButton, Qt::NoModifier, QPoint(300, 80));
    QVERIFY(!item->m_autoscroll.active());
    QTest::mouseMove(&window, QPoint(250, 80));
    QTest::mouseMove(&window, QPoint(200, 80));
    QTest::mouseRelease(&window, Qt::MiddleButton, Qt::NoModifier, QPoint(200, 80));
    QVERIFY(!item->m_autoscroll.active());
    QVERIFY(std::abs(viewOf(*item).first - (stopped.first + 100.0 * kDuration / 8.0 / kWidth))
            <= columnOf(*item) + 1e-9);
    item->followPlayhead();
    item->setZoomLevel(8.0);
    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, click);
    QTest::keyClick(&window, Qt::Key_Escape);
    const auto restopped = viewOf(*item);

    // A middle click starts it again; another, off the plot, stops it, and
    // neither pans the view nor starts autoscroll again as it is released.
    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, click);
    QVERIFY(item->m_autoscroll.active());
    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, QPoint(600, 180));
    QVERIFY(!item->m_autoscroll.active());
    QVERIFY(QGuiApplication::overrideCursor() == nullptr);
    QVERIFY(sameView(viewOf(*item), restopped));
    QTest::mouseMove(&window, QPoint(20, 80));
    QTest::qWait(100);
    QVERIFY(sameView(viewOf(*item), restopped));

    // A middle-drag pans, as before, and starts no autoscroll.
    QTest::mousePress(&window, Qt::MiddleButton, Qt::NoModifier, QPoint(400, 80));
    QTest::mouseMove(&window, QPoint(350, 80));
    QTest::mouseMove(&window, QPoint(300, 80));
    QTest::mouseRelease(&window, Qt::MiddleButton, Qt::NoModifier, QPoint(300, 80));
    QVERIFY(!item->m_autoscroll.active());
    QVERIFY(std::abs(viewOf(*item).first - (restopped.first + 100.0 * kDuration / 8.0 / kWidth))
            <= columnOf(*item) + 1e-9);

    // Another window taking the focus stops it, also a dialog of this
    // window's, with which Qt still counts this one as active.
    QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, click);
    QVERIFY(item->m_autoscroll.active());
    QWindow dialog;
    dialog.setTransientParent(&window);
    dialog.resize(100, 100);
    dialog.show();
    dialog.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&dialog));
    QVERIFY(window.isActive());
    QTRY_VERIFY(!item->m_autoscroll.active());
    QVERIFY(QGuiApplication::overrideCursor() == nullptr);
}

std::unique_ptr<QmlSmokeTest> makeWaveformFollowSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<WaveformFollowSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_waveform_follow.moc"
