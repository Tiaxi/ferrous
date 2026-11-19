// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

#include <gst/gst.h>

// The waveform's view following the playhead: a middle-drag detaches it,
// the wheel zooms about the pointer while detached, the playhead reaching
// 85 % of a detached view glides it back, and seeks, track changes, Reset
// Zoom and Follow Playhead bring it back at once.
class WaveformFollowSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void middleDragDetachesAndPans();
    void middleClickResetsNeitherVisualizationsZoom();
    void wheelZoomsAboutThePlayheadOrThePointer();
    void detachedViewGlidesBackFromItsReattachPoint_data();
    void detachedViewGlidesBackFromItsReattachPoint();
    void playheadPastTheReattachPointLeavesTheView_data();
    void playheadPastTheReattachPointLeavesTheView();
    void ownSeekKeepsTheViewAndOthersBringItBack_data();
    void ownSeekKeepsTheViewAndOthersBringItBack();
    void followPlayheadReattachesAtOnce_data();
    void followPlayheadReattachesAtOnce();
    void gstreamerSeeksAndTrackChangesBringTheViewBack();
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

// A middle-drag pans a zoomed view, which then stays where it was put while
// playback goes on; the time grabbed stays under the pointer.
void WaveformFollowSmokeTest::middleDragDetachesAndPans() {
    WaveformEditorItem item;
    TestTime time;
    playZoomed(item, time, 8.0, 30.0);
    QSignalSpy detachedChanges(&item, &WaveformEditorItem::detachedChanged);
    const double span = kDuration / 8.0;
    const auto following = viewOf(item);
    QVERIFY(sameView(following, followingViewOf(item)));

    // A click, or a move short of a drag, changes nothing.
    middleDrag(item, 200.0, 200.0);
    middleDrag(item, 200.0, 201.0);
    QVERIFY(!item.detached());
    QCOMPARE(item.zoomLevel(), 8.0);
    QVERIFY(sameView(viewOf(item), following));
    QCOMPARE(detachedChanges.count(), 0);

    // 200 px to the left: the view moves on by half its span.
    middleDrag(item, 300.0, 100.0);
    QVERIFY(item.detached());
    QCOMPARE(detachedChanges.count(), 1);
    const auto panned = viewOf(item);
    QVERIFY2(std::abs(panned.first - (following.first + span / 2)) < columnOf(item),
             qPrintable(describe(panned)));
    QCOMPARE(panned.second - panned.first, span);
    QCOMPARE(item.zoomLevel(), 8.0);

    // Playback goes on; the view stays.
    for (int step = 0; step < 10; ++step) {
        time.advance(0.1);
        frame(item);
        QVERIFY(sameView(viewOf(item), panned));
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
// Reset Zoom and the viewer's Reset.
void WaveformFollowSmokeTest::middleClickResetsNeitherVisualizationsZoom() {
    WaveformEditorItem waveform;
    TestTime time;
    playZoomed(waveform, time, 4.0, 20.0);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(50.0, 40.0), QPointF(50.0, 40.0),
        Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    waveform.mousePressEvent(&press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(50.0, 40.0), QPointF(50.0, 40.0),
        Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
    waveform.mouseReleaseEvent(&release);
    QCOMPARE(waveform.zoomLevel(), 4.0);
    QVERIFY(!waveform.detached());

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

    bridge.setRepeatMode(0);
    bridge.stop();
    bridge.clearQueue();
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 0, 10000);
}

std::unique_ptr<QmlSmokeTest> makeWaveformFollowSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<WaveformFollowSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_waveform_follow.moc"
