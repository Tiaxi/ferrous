// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDataStream>
#include <QFile>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QImage>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include "SpectrogramColorMap.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"
#include "WaveformReaderLog.h"
#define private public
#include "LevelMeterItem.h"
#undef private

namespace {
template <typename T> void append(QByteArray &bytes, T value) {
    const auto little = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char *>(&little), sizeof(little));
}

QByteArray windowBytes(int channels, const std::vector<float> &extrema) {
    QByteArray bytes("WVF2");
    append(bytes, quint32(48000));
    append(bytes, quint16(channels));
    append(bytes, quint16(0));
    append(bytes, std::bit_cast<quint64>(0.0));
    append(bytes, std::bit_cast<quint64>(double(extrema.size() / (channels * 2)) * 64 / 48000));
    append(bytes, quint32(64));
    append(bytes, quint32(extrema.size() / (channels * 2)));
    for (float value : extrema) append(bytes, std::bit_cast<quint32>(value));
    for (std::size_t i = 0; i < extrema.size() / channels; ++i) append(bytes, quint32(0));
    return bytes;
}

bool writeWave(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 frames = 48000;
    constexpr quint16 channels = 8;
    constexpr quint32 bytes = frames * channels * 2;
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + bytes);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << channels << quint32(48000)
           << quint32(48000 * channels * 2) << quint16(channels * 2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << bytes;
    for (quint32 frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < channels; ++channel) {
            stream << qint16((frame % 2 ? -1 : 1) * (16384 >> channel));
        }
    }
    return stream.status() == QDataStream::Ok;
}

constexpr double kBinSeconds = 64.0 / 48000.0;

// Two channels of `db(t)` from `start` to `end`, in bins the size Rust makes.
LevelMeterItem::Window levelWindow(double start, double end, const std::function<float(double)> &db) {
    LevelMeterItem::Window window{start, end, kBinSeconds, 2, {}};
    const auto points = static_cast<std::size_t>(std::ceil((end - start) / kBinSeconds));
    window.peaksDb.reserve(points * 2);
    for (std::size_t point = 0; point < points; ++point) {
        const float value = db(start + double(point) * kBinSeconds);
        window.peaksDb.push_back(value);
        window.peaksDb.push_back(value);
    }
    return window;
}

// A meter with a 400 px scale in a window (one lane 410 px tall: 3 px
// insets, the 3 px lamp and the gap under it), playing a 100 s track on its
// own clock.
struct MeterFixture {
    MeterFixture() {
        view.resize(10, 410);
        // As QML does: the item joins the window after it is made.
        item.setParentItem(view.contentItem());
        item.setSize(QSizeF(10, 410));
        item.setActive(true);
    }
    void setAdvancing(bool advancing) {
        PlaybackSample sample;
        sample.positionSeconds = clock.positionAt(clock.nowNs());
        sample.sampledAtNs = clock.nowNs();
        sample.advancing = advancing;
        sample.durationSeconds = 100;
        sample.trackToken = 1;
        clock.applySample(sample, false);
    }
    double position() const { return clock.positionAt(clock.nowNs()); }
    int fill(int lane = 0) const {
        const auto index = static_cast<std::size_t>(lane);
        return item.m_painted.size() > index ? item.m_painted[index].fill : -2;
    }
    int peak(int lane = 0) const {
        const auto index = static_cast<std::size_t>(lane);
        return item.m_painted.size() > index ? item.m_painted[index].peak : -2;
    }

    PlaybackClock clock;
    QQuickWindow view;
    LevelMeterItem item;
};

// A meter playing `db(t)` in real time, with frames counted.
struct PlayingMeter : MeterFixture {
    explicit PlayingMeter(std::function<float(double)> db) {
        item.setClock(&clock);
        item.setDurationSeconds(100);
        item.m_decodeWindow = [db](const QString &, double start, double end,
                                   const LevelMeterItem::Cancellation &) {
            return levelWindow(start, end, db);
        };
        QObject::connect(&view, &QQuickWindow::frameSwapped, &view, [this] { ++frames; });
        view.show();
        item.setSourcePath(QStringLiteral("fixture.wav"));
        setAdvancing(true);
        item.setPlaying(true);
    }
    // Frames per second over `ms` of real time.
    double framesPerSecond(int ms) {
        frames = 0;
        QTest::qWait(ms);
        return frames * 1000.0 / ms;
    }

    int frames{0};
};

// A meter playing `db(t)` on time the test sets. The test ticks its frames
// and draws them with grabWindow, so nothing depends on real time; between
// ticks the meter is idle, as while its wake timer waits.
struct SteppedMeter : MeterFixture {
    static constexpr qint64 kStartNs = 1'000'000'000'000;

    explicit SteppedMeter(const std::function<float(double)> &db) {
        clock.setTimeSource([this] { return nowNs; });
        item.m_now = [this] {
            return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(nowNs));
        };
        item.setClock(&clock);
        item.setDurationSeconds(100);
        view.show();
        exposed = QTest::qWaitForWindowExposed(&view);
        // The first sample starts a track, which clears the meter.
        setAdvancing(true);
        // Without a source the meter requests nothing; it plays this window.
        item.m_window = levelWindow(0, 10, db);
        item.setPlaying(true);
    }
    void at(double seconds) {
        time = seconds;
        nowNs = kStartNs + static_cast<qint64>(std::llround(seconds * 1e9));
    }
    // Ticks a frame and draws it if a bar or marker moved; returns whether it drew.
    bool frame() {
        item.tick();
        if (item.showsPainted(item.m_shown)) return false;
        view.grabWindow();
        return true;
    }
    // Ticks frames at 60 Hz until `seconds`; returns how many drew.
    int framesUntil(double seconds) {
        int drawn = 0;
        while (time + 1.0 / 60 <= seconds + 1e-9) {
            at(time + 1.0 / 60);
            drawn += frame() ? 1 : 0;
        }
        return drawn;
    }

    qint64 nowNs{kStartNs};
    double time{0.0};
    bool exposed{false};
};

// A meter that takes every bin of the first channel when the bin ends: what
// a meter ticked at every bin would show.
struct ReferenceMeter {
    explicit ReferenceMeter(LevelMeterItem::Window data) : window(std::move(data)) {}
    // Takes the bins that ended by `position`; returns the newest one's end.
    double hear(double position) {
        const auto heard = static_cast<std::size_t>(std::floor((position - window.start) / window.step));
        for (; bins < heard; ++bins) {
            channel.decay(window.step);
            channel.observe(window.peaksDb[bins * static_cast<std::size_t>(window.channels)], 0);
        }
        return window.start + double(bins) * window.step;
    }
    // Empty if `item` shows the reference as of the newest bin at `position`
    // and holds it decayed to `position`; otherwise what differs.
    QString mismatch(const LevelMeterItem &item, double position) {
        const double heardAt = hear(position);
        auto now = channel;
        now.decay(position - heardAt);
        const auto &shown = item.m_shown[0];
        const auto &state = item.m_channels[0];
        const auto close = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        if (close(shown.level, channel.level) && close(shown.peak, channel.peak)
            && close(shown.hold, channel.hold) && close(item.m_shownAt, heardAt)
            && close(state.level, now.level) && close(state.peak, now.peak) && close(state.hold, now.hold)) return {};
        return QStringLiteral("at %1: shown %2 dB, peak %3 dB, hold %4 s at %5; reference %6, %7, %8 at %9")
            .arg(position).arg(shown.level).arg(shown.peak).arg(shown.hold).arg(item.m_shownAt)
            .arg(channel.level).arg(channel.peak).arg(channel.hold).arg(heardAt);
    }

    LevelMeterItem::Window window;
    LevelMeterItem::Channel channel;
    std::size_t bins{0};
};

// The level `pixels` device pixels up a scale `scalePixels` tall: the
// inverse of LevelMeterItem::meterPosition, by bisection.
double levelAtPixels(double pixels, double scalePixels) {
    double low = LevelMeterItem::kFloorDb;
    double high = 0.0;
    for (int i = 0; i < 60; ++i) {
        const double middle = (low + high) / 2.0;
        (LevelMeterItem::meterPosition(middle, scalePixels) < pixels ? low : high) = middle;
    }
    return (low + high) / 2.0;
}

// The colour-map fill's colour of the row `row` device pixels up a scale
// `scalePixels` tall: the map's colour of the level at the row's middle.
QColor rampColor(int row, double scalePixels, double dbRange) {
    return LevelMeterItem::colorMapColor(levelAtPixels(row + 0.5, scalePixels), dbRange);
}

// A meter drawn offscreen at `dpr` device pixels to a logical one, through
// the software renderer, as a 2x screen draws it.
struct ScaledMeter {
    ScaledMeter(QSize size, double dpr)
        : canvas(size * dpr, QImage::Format_ARGB32_Premultiplied) {
        canvas.setDevicePixelRatio(dpr);
        QQuickRenderTarget target = QQuickRenderTarget::fromPaintDevice(&canvas);
        target.setDevicePixelRatio(dpr);
        view.setRenderTarget(target);
        view.resize(size);
        view.contentItem()->setSize(size);
        item.setParentItem(view.contentItem());
        item.setSize(size);
        // The software renderer draws to the paint device as it is; only
        // a renderer on a graphics API needs QQuickRenderControl::initialize.
        initialized = view.rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
    }
    QImage render() {
        control.polishItems();
        control.sync();
        control.render();
        QImage image = canvas.convertToFormat(QImage::Format_RGB32);
        image.setDevicePixelRatio(1.0);
        return image;
    }

    QQuickRenderControl control;
    QQuickWindow view{&control};
    QImage canvas;
    LevelMeterItem item;
    bool initialized{false};
};

// Device pixels of `db` on the 400 px scale.
int pixels(double db) { return qRound(LevelMeterItem::meterPosition(db, 400)); }
constexpr double kRelease = LevelMeterItem::kReleaseDbPerSecond;
constexpr double kPeakRelease = LevelMeterItem::kPeakReleaseDbPerSecond;
}

class LevelMeterTest : public QObject {
    Q_OBJECT
private slots:
    void attackReleaseAndPeakHold();
    void ballisticsAreRefreshRateIndependent();
    void loudToQuietAndSilenceReleasePromptly();
    void protocolPreservesNativePeaksAndRejectsMalformedData();
    void rustDecoderProvidesAllChannelsAndHonorsCancellation();
    void consumesOnlyAudibleBinsAndClearsDiscontinuities();
    void followsThePlaybackClock();
    void lateDataDoesNotReplayOldPeaks();
    void requestsAreBoundedAndObsoleteResultsAreDiscarded();
    void destructionCancelsPendingDecode();
    void readsItsFileOnlyWhileVisibleForPlayback();
    void scaleAndLanesAreLaidOutLikeThePlot();
    void sceneGraphDrawsAColumnPerLane();
    void meterIsPartOfThePlot();
    void colorMapFillIsARamp_data();
    void colorMapFillIsARamp();
    void loudFillIsCappedAndItsPeakIsFull();
    void stoppedMeterIsAnEmptyTrough();
    void oneLaneShowsTheLoudestChannel();
    void readoutGivesExactLevelsOnlyToReaders();
    void shownLevelsFollowTheBallistics();
    void steadyLevelsDrawNoFrames();
    void silenceDrawsNoFrames();
    void pauseDrawsOnlyTheDecay();
    void changingLevelsRedraw();
    void idleMeterWakesForItsNextPixel();
    void pauseAndResumeKeepTheTimeBetween();
    void idleWakesTakeEveryBufferedBin();
    void drawsOffscreenUnlessAPlatformIsChosen();
};

void LevelMeterTest::attackReleaseAndPeakHold() {
    LevelMeterItem::Channel channel;
    channel.observe(-6, 0);
    QCOMPARE(channel.level, -6.0);
    QCOMPARE(channel.peak, -6.0);
    // The fill falls at 20 dB/s at once; the peak holds 1.5 s, then falls
    // at 20 dB/s.
    channel.decay(0.5);
    QCOMPARE(channel.level, -16.0);
    QCOMPARE(channel.peak, -6.0);
    QCOMPARE(channel.hold, 1.0);
    channel.observe(-20, 0);
    QCOMPARE(channel.hold, 1.0);
    channel.decay(1.25);
    QCOMPARE(channel.level, -41.0);
    QCOMPARE(channel.peak, -11.0);
    QCOMPARE(channel.hold, 0.0);
    QCOMPARE(channel.clip, 0.0);
    channel.observe(6, 0);
    QCOMPARE(channel.level, 0.0);
    QCOMPARE(channel.peak, 0.0);
    QCOMPARE(channel.hold, 1.5);
    // Full scale lights the clip lamp for 2 s.
    QCOMPARE(channel.clip, 2.0);
    channel.decay(1.5);
    QCOMPARE(channel.clip, 0.5);
    channel.observe(-0.5, 0);
    QCOMPARE(channel.clip, 0.5);
    // A bin heard late lights it for what is left of its 2 s.
    channel.observe(0, 1.75);
    QCOMPARE(channel.clip, 0.5);
    channel.observe(0, 1.25);
    QCOMPARE(channel.clip, 0.75);
    channel.decay(10);
    QCOMPARE(channel.level, -60.0);
    QCOMPARE(channel.peak, -60.0);
    QCOMPARE(channel.clip, 0.0);
}

void LevelMeterTest::ballisticsAreRefreshRateIndependent() {
    const std::vector<std::pair<double, double>> events{
        {0.001, -18}, {0.007, -2}, {0.019, -24}, {0.312, -8}, {0.974, -12}, {1.831, -9}};
    const auto run = [&](int fps) {
        LevelMeterItem::Channel channel;
        std::size_t next = 0;
        for (int frame = 1; frame <= fps * 3; ++frame) {
            const double now = double(frame) / fps;
            channel.decay(1.0 / fps);
            while (next < events.size() && events[next].first <= now) {
                channel.observe(events[next].second, now - events[next].first);
                ++next;
            }
        }
        return channel;
    };
    const auto reference = run(60);
    for (int fps : {30, 144, 360, 1000}) {
        const auto actual = run(fps);
        QVERIFY(std::abs(reference.level - actual.level) < 1e-8);
        QVERIFY(std::abs(reference.peak - actual.peak) < 1e-8);
        QVERIFY(std::abs(reference.hold - actual.hold) < 1e-8);
    }
}

void LevelMeterTest::loudToQuietAndSilenceReleasePromptly() {
    for (bool silence : {false, true}) {
        for (int fps : {30, 60, 144, 360}) {
            LevelMeterItem item;
            item.m_playing = true;
            item.m_window = {0, 4, 1.0 / 1024, 2, std::vector<float>(8192)};
            const double quietLeft = silence ? -60.0 : -36.0;
            const double quietRight = silence ? -60.0 : -48.0;
            for (int point = 0; point < 4096; ++point) {
                item.m_window.peaksDb[point * 2] = point < 128 ? 0.0F : float(quietLeft);
                item.m_window.peaksDb[point * 2 + 1] = point < 128 ? -6.0F : float(quietRight);
            }
            item.advance(0.125, 0.125);
            QCOMPARE(item.m_channels[0].level, 0.0);
            QCOMPARE(item.m_channels[1].level, -6.0);
            for (int frame = 1; frame <= fps * 3; ++frame) {
                item.advance(0.125 + double(frame) / fps, 1.0 / fps);
                if (frame == fps / 2) {
                    QVERIFY(std::abs(item.m_channels[0].level + 10) < 1e-8);
                    QVERIFY(std::abs(item.m_channels[1].level + 16) < 1e-8);
                }
                if (frame == fps) {
                    // The hold outlasts the fill's fall.
                    QCOMPARE(item.m_channels[0].peak, 0.0);
                    QCOMPARE(item.m_channels[1].peak, -6.0);
                    QVERIFY(std::abs(item.m_channels[0].hold - 0.5) < 1e-8);
                }
            }
            QVERIFY(std::abs(item.m_channels[0].level - quietLeft) < 1e-8);
            QVERIFY(std::abs(item.m_channels[1].level - quietRight) < 1e-8);
            // The peaks fell at 20 dB/s for the 1.5 s after their hold.
            QVERIFY(std::abs(item.m_channels[0].peak - std::max(quietLeft, -30.0)) < 1e-8);
            QVERIFY(std::abs(item.m_channels[1].peak - std::max(quietRight, -36.0)) < 1e-8);
        }
    }
}

void LevelMeterTest::protocolPreservesNativePeaksAndRejectsMalformedData() {
    const auto bytes = windowBytes(2, {-0.5F, -0.1F, 0.1F, 0.25F, -1.0F, 1.0F, 0, 0});
    const auto data = LevelMeterItem::parseWindow(bytes);
    QCOMPARE(data.channels, 2);
    QCOMPARE(data.peaksDb.size(), std::size_t(4));
    QVERIFY(std::abs(data.peaksDb[0] + 6.0206) < 0.0001);
    QVERIFY(std::abs(data.peaksDb[1] + 12.0412) < 0.0001);
    QCOMPARE(data.peaksDb[2], 0.0F);
    QCOMPARE(data.peaksDb[3], -60.0F);
    QCOMPARE(LevelMeterItem::parseWindow(bytes.chopped(1)).channels, 0);
    auto invalid = bytes;
    invalid[0] = 'X';
    QCOMPARE(LevelMeterItem::parseWindow(invalid).channels, 0);
    invalid = bytes;
    qToLittleEndian<quint32>(0, invalid.data() + 28);
    QCOMPARE(LevelMeterItem::parseWindow(invalid).channels, 0);
    invalid = bytes;
    qToLittleEndian<quint64>(std::bit_cast<quint64>(std::nan("")), invalid.data() + 12);
    QCOMPARE(LevelMeterItem::parseWindow(invalid).channels, 0);
    QCOMPARE(LevelMeterItem::parseWindow(windowBytes(1, {std::nanf(""), 1})).channels, 0);
    QCOMPARE(LevelMeterItem::parseWindow(windowBytes(1, {1, -1})).channels, 0);
}

void LevelMeterTest::rustDecoderProvidesAllChannelsAndHonorsCancellation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("surround.wav"));
    QVERIFY(writeWave(path));
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    const auto data = LevelMeterItem::decodeWindow(path, 0.123, 0.8, cancelled);
    QCOMPARE(data.channels, 8);
    QVERIFY(data.step < 0.002);
    QVERIFY(data.start >= 0.123);
    for (std::size_t i = 0; i < data.peaksDb.size(); ++i) {
        QVERIFY(std::abs(data.peaksDb[i] + 6.020599913 * (i % 8 + 1)) < 0.0001);
    }
    cancelled->store(true);
    QCOMPARE(LevelMeterItem::decodeWindow(path, 0.0, 1.0, cancelled).channels, 0);
}

void LevelMeterTest::consumesOnlyAudibleBinsAndClearsDiscontinuities() {
    PlaybackClock clock;
    LevelMeterItem item;
    item.setClock(&clock);
    item.m_playing = true;
    item.m_window = {0, 0.025, 0.01, 2, {-6, -12, -2, -20, -1, -30}};
    item.advance(0.005, 0.005);
    QCOMPARE(item.m_channels[0].peak, -60.0);
    item.advance(0.01, 0.005);
    QCOMPARE(item.m_channels[0].peak, -6.0);
    QCOMPARE(item.m_channels[1].peak, -12.0);
    item.advance(0.02, 0.01);
    QCOMPARE(item.m_channels[0].peak, -2.0);
    item.advance(0.025, 0.005);
    QCOMPARE(item.m_channels[0].peak, -1.0); // Partial bin at EOF is included.
    item.setPlaying(false);
    item.advance(0.025, 0.5);
    QVERIFY(item.m_channels[0].level < -10);
    QCOMPARE(item.m_channels[0].peak, -1.0);
    // A seek is a jump of the clock.
    clock.holdAt(0.02);
    QCOMPARE(item.m_channels[0].peak, -60.0);
    QCOMPARE(item.m_window.channels, 0);
    item.m_channels[0].observe(-3, 0);
    item.setSourcePath(QStringLiteral("next.wav"));
    QCOMPARE(item.m_channels[0].peak, -60.0);
    item.m_channels[0].observe(-3, 0);
    item.setSourcePath(QString());
    QCOMPARE(item.m_channels[0].peak, -60.0);
    // Samples that continue playback keep the levels.
    item.m_channels[0].observe(-3, 0);
    PlaybackSample sample;
    sample.positionSeconds = 0.02;
    sample.sampledAtNs = clock.nowNs();
    clock.applySample(sample, false);
    QCOMPARE(item.m_channels[0].peak, -3.0);
}

void LevelMeterTest::followsThePlaybackClock() {
    PlaybackClock clock;
    LevelMeterItem item;
    QCOMPARE(item.currentPosition(std::chrono::steady_clock::now()), 0.0);
    item.setClock(&clock);
    PlaybackSample sample;
    sample.positionSeconds = 12.0;
    sample.sampledAtNs = monotonicNs(std::chrono::steady_clock::now());
    sample.advancing = true;
    sample.durationSeconds = 20.0;
    sample.trackToken = 1;
    clock.applySample(sample, true);
    const auto frame = std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(sample.sampledAtNs + 250'000'000));
    // The meter reads the clock at its own frame time.
    QVERIFY(std::abs(item.currentPosition(frame) - 12.25) < 1e-9);
    item.setDurationSeconds(12.1);
    QCOMPARE(item.currentPosition(frame), 12.1);
}

void LevelMeterTest::lateDataDoesNotReplayOldPeaks() {
    LevelMeterItem item;
    item.m_playing = true;
    item.m_window = {0, 1, 0.01, 1, std::vector<float>(100, -60)};
    item.m_window.peaksDb[0] = 0;
    // Frames ran on to 0.7 s before the window arrived.
    auto late = std::exchange(item.m_window, {});
    item.advance(0.7, 0.7);
    item.m_window = std::move(late);
    item.advance(0.8, 0.1);
    QCOMPARE(item.m_channels[0].peak, -60.0);
    item.m_stagedWindow = {0.7, 1.7, 0.01, 1, std::vector<float>(100, -60)};
    item.m_stagedWindow.peaksDb[0] = 0;
    item.advance(0.81, 0.01);
    QCOMPARE(item.m_channels[0].peak, -60.0);
    QCOMPARE(item.m_stagedWindow.channels, 0);
    QCOMPARE(item.m_window.start, 0.7);
}

void LevelMeterTest::requestsAreBoundedAndObsoleteResultsAreDiscarded() {
    PlaybackClock clock;
    QQuickWindow view;
    view.resize(400, 40);
    LevelMeterItem item(view.contentItem());
    item.setClock(&clock);
    item.setSize(QSizeF(400, 20));
    item.setDurationSeconds(10);
    auto gate = std::make_shared<QSemaphore>();
    auto started = std::make_shared<std::atomic_int>(0);
    item.m_decodeWindow = [gate, started](const QString &, double, double, const LevelMeterItem::Cancellation &) {
        ++*started;
        gate->tryAcquire(1, 5000);
        return LevelMeterItem::Window{0, 2, 0.001, 2, std::vector<float>(4000, 0)};
    };
    view.show();
    item.setPlaying(true);
    item.setSourcePath(QStringLiteral("first.wav"));
    QTRY_COMPARE(started->load(), 1);
    const auto cancelled = item.m_cancelled;
    // Seeks in a row each restart the request.
    for (int i = 0; i < 20; ++i) clock.holdAt(i * 0.01);
    QVERIFY(cancelled->load());
    QCOMPARE(started->load(), 1);
    QCOMPARE(item.findChildren<QFutureWatcherBase *>().size(), 1);
    item.setSourcePath(QStringLiteral("second.wav"));
    gate->release();
    QTRY_COMPARE(started->load(), 2);
    QCOMPARE(item.m_channels[0].peak, -60.0);
    item.setVisible(false);
    gate->release();
    QTRY_VERIFY(!item.m_requestActive);
    QCOMPARE(item.m_stagedWindow.channels, 0);
    QCOMPARE(item.m_window.channels, 0);
    QCOMPARE(started->load(), 2);
    item.setVisible(true);
    QTRY_COMPARE(started->load(), 3);
    const auto destroyedCancellation = item.m_cancelled;
    item.setPlaying(false);
    QVERIFY(destroyedCancellation->load());
    gate->release();
    QTRY_VERIFY(!item.m_requestActive);
}

void LevelMeterTest::destructionCancelsPendingDecode() {
    QQuickWindow view;
    view.resize(400, 20);
    auto item = std::make_unique<LevelMeterItem>(view.contentItem());
    item->setSize(QSizeF(400, 20));
    item->setDurationSeconds(10);
    auto started = std::make_shared<std::atomic_bool>(false);
    auto finished = std::make_shared<std::atomic_bool>(false);
    auto gate = std::make_shared<QSemaphore>();
    item->m_decodeWindow = [started, finished, gate](const QString &, double, double, const LevelMeterItem::Cancellation &) {
        started->store(true);
        gate->tryAcquire(1, 5000);
        finished->store(true);
        return LevelMeterItem::Window{};
    };
    view.show();
    item->setPlaying(true);
    item->setSourcePath(QStringLiteral("fixture.wav"));
    QTRY_VERIFY(started->load());
    const auto cancelled = item->m_cancelled;
    item.reset();
    QVERIFY(cancelled->load());
    gate->release();
    QTRY_VERIFY(finished->load());
    QCoreApplication::processEvents();
}

void LevelMeterTest::readsItsFileOnlyWhileVisibleForPlayback() {
    // The waveform detail service keeps what the meter reads of a file
    // while it is visible in a window on screen, and frees it after.
    WaveformReaderLog log;
    PlaybackClock clock;
    QQuickWindow view;
    view.resize(400, 40);
    auto item = std::make_unique<LevelMeterItem>(view.contentItem());
    item->setClock(&clock);
    item->setSize(QSizeF(400, 20));
    item->setDurationSeconds(10);
    item->m_decodeWindow = [](const QString &, double, double, const LevelMeterItem::Cancellation &) {
        return LevelMeterItem::Window{};
    };
    item->setPlaying(true);
    item->setSourcePath(QStringLiteral("first.wav"));
    QVERIFY(!item->visibleForPlayback());
    QCOMPARE(log.held(), QStringList());
    view.show();
    QTRY_VERIFY(item->visibleForPlayback());
    QCOMPARE(log.held(), QStringList{QStringLiteral("first.wav meter")});

    // The next track: the new file is retained before the old one goes.
    WaveformReaderLog::order().clear();
    item->setSourcePath(QStringLiteral("second.wav"));
    QCOMPARE(WaveformReaderLog::order(),
             (QStringList{QStringLiteral("+second.wav meter"), QStringLiteral("-first.wav meter")}));
    QTRY_VERIFY(WaveformReaderLog::releasesUnread().load() >= 1);

    item->setVisible(false);
    QCOMPARE(log.held(), QStringList());
    item->setVisible(true);
    QCOMPARE(log.held(), QStringList{QStringLiteral("second.wav meter")});
    // Its window off screen, as minimized.
    view.hide();
    QTRY_VERIFY(!item->visibleForPlayback());
    QCOMPARE(log.held(), QStringList());
    view.show();
    QTRY_VERIFY(item->visibleForPlayback());
    QCOMPARE(log.held(), QStringList{QStringLiteral("second.wav meter")});
    // Stopped: the meter's source is cleared.
    item->setSourcePath(QString());
    QCOMPARE(log.held(), QStringList());
    item->setSourcePath(QStringLiteral("second.wav"));
    const int passes = WaveformReaderLog::releasesUnread().load();
    item.reset();
    QCOMPARE(log.held(), QStringList());
    QTRY_VERIFY(WaveformReaderLog::releasesUnread().load() > passes);
}

void LevelMeterTest::scaleAndLanesAreLaidOutLikeThePlot() {
    // The listed levels are a fifth of the scale apart; -60 dB is its floor.
    QCOMPARE(LevelMeterItem::meterPosition(0, 400), 400.0);
    QCOMPARE(LevelMeterItem::meterPosition(-6, 400), 320.0);
    QCOMPARE(LevelMeterItem::meterPosition(-12, 400), 240.0);
    QCOMPARE(LevelMeterItem::meterPosition(-24, 400), 160.0);
    QCOMPARE(LevelMeterItem::meterPosition(-48, 400), 80.0);
    QCOMPARE(LevelMeterItem::meterPosition(-60, 400), 0.0);
    QCOMPARE(LevelMeterItem::meterPosition(-36, 400), 120.0);
    QCOMPARE(LevelMeterItem::meterPosition(3, 400), 400.0);
    QCOMPARE(LevelMeterItem::meterPosition(-80, 400), 0.0);
    // Lanes with a 1 px seam between them, on the rows the waveform draws
    // its seams on.
    using Rows = std::pair<double, double>;
    QCOMPARE(LevelMeterItem::laneRows(0, 1, 200), Rows(0, 200));
    QCOMPARE(LevelMeterItem::laneRows(0, 2, 201), Rows(0, 101));
    QCOMPARE(LevelMeterItem::laneRows(1, 2, 201), Rows(102, 201));
    QCOMPARE(LevelMeterItem::laneRows(2, 6, 300), Rows(101, 150));
    QCOMPARE(LevelMeterItem::laneRows(5, 6, 300), Rows(251, 300));
}

void LevelMeterTest::sceneGraphDrawsAColumnPerLane() {
    constexpr double width = LevelMeterItem::kColumnWidth;
    QQuickWindow view;
    view.resize(int(width), 402);
    LevelMeterItem item(view.contentItem());
    item.setSize(QSizeF(width, 402));
    item.setActive(true);
    // Distinct colours, to tell the parts apart.
    item.setBackgroundColor(QColor(0x10, 0x00, 0x00));
    item.setSeamColor(QColor(0x20, 0x00, 0x00));
    item.setTroughColor(QColor(0x30, 0x00, 0x00));
    item.setFillColor(QColor(0x40, 0x00, 0x00));
    item.setPeakColor(QColor(0x50, 0x00, 0x00));
    item.setClipOffColor(QColor(0x60, 0x00, 0x00));
    item.setClipColor(QColor(0x70, 0x00, 0x00));
    item.setGapColor(QColor(0x80, 0x00, 0x00));
    view.show();
    QTRY_VERIFY(view.isExposed());
    for (int lanes : {1, 2, 6}) {
        item.setLaneCount(lanes);
        item.setChannelCountHint(lanes);
        item.m_channels.assign(static_cast<std::size_t>(lanes), {});
        for (auto &channel : item.m_channels) channel.observe(-30, 0);
        // The first channel peaked at -9 dB and clipped.
        item.m_channels[0].peak = -9;
        item.m_channels[0].clip = 1;
        item.m_shown = item.m_channels;
        item.update();
        const QImage image = view.grabWindow();
        QVERIFY(!image.isNull());
        const double scale = double(image.width()) / view.width();
        const auto pixel = [&](double x, double y) {
            return image.pixelColor(int(x * scale), int(y * scale)).red();
        };
        for (int lane = 0; lane < lanes; ++lane) {
            const auto [top, bottom] = LevelMeterItem::laneRows(lane, lanes, 402);
            const double height = bottom - top;
            const double meterBottom = bottom - 3;
            const double scaleHeight = height - 6 - 3 - 1;
            const auto at = [&](double db) { return meterBottom - LevelMeterItem::meterPosition(db, scaleHeight); };
            // The seam on the column's left; the column's floor above the
            // meter; the trough over the fill; the lamp; the gap under it.
            QCOMPARE(pixel(0, top + height / 2), 0x20);
            QCOMPARE(pixel(5, top + 1), 0x10);
            QCOMPARE(pixel(5, bottom - 1), 0x10);
            QCOMPARE(pixel(5, at(-18)), 0x30);
            QCOMPARE(pixel(5, at(-40) - 0.5), 0x40);
            QCOMPARE(pixel(5, top + 4), lane == 0 ? 0x70 : 0x60);
            QCOMPARE(pixel(5, top + 6.5), 0x80);
            // A meter, not a scrollbar (#373 H5, H8): the 16 px track the
            // lane's height, x 1 to 17, and on it the 15 px bar, x 2 to 17,
            // out to the column's right edge with no floor beside it, and
            // the track showing 1 px left of it.
            for (const double y : {at(-40) - 0.5, top + 4}) {
                QCOMPARE(pixel(1.5, y), 0x30);
            }
            for (double x = 2.5; x < width; x += 1) {
                QCOMPARE(pixel(x, at(-18)), 0x30);
                QCOMPARE(pixel(x, at(-40) - 0.5), 0x40);
                QCOMPARE(pixel(x, top + 4), lane == 0 ? 0x70 : 0x60);
            }
            QCOMPARE(pixel(1.5, at(-18)), 0x30);
            // Without the plot's scale, no ticks: the trough above the
            // fill, the fill below its top.
            QCOMPARE(pixel(5, std::round(at(-24)) + 0.5), 0x30);
            QCOMPARE(pixel(5, std::round(at(-12)) + 0.5), 0x30);
            QCOMPARE(pixel(5, std::round(at(-48)) + 0.5), 0x40);
            if (lane == 0) {
                // The peak: 2 px across the track at -9 dB.
                const double peak = at(-9);
                QCOMPARE(pixel(5, peak - 0.5), 0x50);
                QCOMPARE(pixel(5, peak + 0.5), 0x50);
                QCOMPARE(pixel(1.5, peak - 0.5), 0x50);
                QCOMPARE(pixel(width - 0.5, peak - 0.5), 0x50);
                QCOMPARE(pixel(5, peak - 2.5), 0x30);
            }
        }
    }
}

// The meter as part of the plot (#373 H7 P11, H8): its bar out to the
// column's right edge, with no strip of the plot's floor beside it; on the
// spectrogram, the fill in the colour map's colour of its level, up to its
// last colour under kFillLightness of the map's top; the scale's ticks only
// while the plot shows its scale, fewer in a short lane; the peak tick a
// mark of its own on any fill.
void LevelMeterTest::meterIsPartOfThePlot() {
    // The colour map's colours, darker for quieter levels, from the
    // spectrogram's mapping: -60 dBFS is a dark magenta in its default
    // 132 dB range, and full scale white.
    const QColor quiet = LevelMeterItem::colorMapColor(-60, 132);
    const QColor middle = LevelMeterItem::colorMapColor(-40, 132);
    const QColor loud = LevelMeterItem::colorMapColor(-3, 132);
    const auto lightness = [](const QColor &color) { return LevelMeterItem::lightness(color); };
    QVERIFY(lightness(quiet) < lightness(middle) && lightness(middle) < lightness(loud));
    QVERIFY2(quiet.red() > quiet.green() && quiet.blue() > quiet.green(), qPrintable(quiet.name()));
    // Over the cap, along the ramp (#373 H9 T2): full scale, and every
    // level whose colour is over the cap, takes the map's own colour of the
    // highest level still under it, an orange, not a darkened white or
    // khaki yellow.
    const double capPosition = LevelMeterItem::fillCapPosition();
    const QColor capColor = QColor::fromRgb(SpectrogramColorMap::colorAt(capPosition));
    QVERIFY(LevelMeterItem::lightness(capColor) <= LevelMeterItem::kFillLightness);
    QVERIFY(LevelMeterItem::lightness(QColor::fromRgb(SpectrogramColorMap::colorAt(capPosition + 1e-3)))
            > LevelMeterItem::kFillLightness);
    const QColor top = LevelMeterItem::colorMapColor(0, 132);
    QCOMPARE(top, capColor);
    QVERIFY2(top.red() == 255 && top.green() > 0x80 && top.green() < 0xc0 && top.blue() == 0, qPrintable(top.name()));
    QVERIFY2(LevelMeterItem::lightness(top) > LevelMeterItem::kFillLightness - 0.01, qPrintable(top.name()));
    QCOMPARE(LevelMeterItem::colorMapColor(-8, 132), capColor);
    QCOMPARE(quiet, QColor::fromRgb(SpectrogramColorMap::colorOfLevel(-60, 132)));
    // Every fill is a colour of the map: under the cap its level's own,
    // over it the cap's, never a scaled one, whose hue or saturation the
    // map's colour of the same lightness would not have.
    for (const double range : {50.0, 90.0, 132.0, 150.0}) {
        for (double db = -60; db <= 0; db += 0.5) {
            const QColor mapped = QColor::fromRgb(SpectrogramColorMap::colorOfLevel(db, range));
            const QColor fill = LevelMeterItem::colorMapColor(db, range);
            const QString where = QStringLiteral("%1 dB in %2: %3").arg(db).arg(range).arg(fill.name());
            QVERIFY2(LevelMeterItem::lightness(fill) <= LevelMeterItem::kFillLightness, qPrintable(where));
            if (LevelMeterItem::lightness(mapped) <= LevelMeterItem::kFillLightness) {
                QCOMPARE(fill, mapped);
            } else {
                QCOMPARE(fill, capColor);
                QCOMPARE(fill.hsvHue(), capColor.hsvHue());
                QCOMPARE(fill.hsvSaturation(), 255);
            }
        }
    }
    // In a narrower range, whose floor is nearer, a level is lower on the
    // map, as the spectrogram draws it.
    QVERIFY(lightness(LevelMeterItem::colorMapColor(-40, 60)) < lightness(middle));
    // The ticks: four in a full lane, two in a short one.
    QCOMPARE(LevelMeterItem::ticksDb(200), (std::vector<double>{0, -12, -24, -48}));
    QCOMPARE(LevelMeterItem::ticksDb(50), (std::vector<double>{-12, -24}));

    constexpr double width = LevelMeterItem::kColumnWidth;
    QQuickWindow view;
    view.resize(int(width), 402);
    LevelMeterItem item(view.contentItem());
    QCOMPARE(item.implicitWidth(), width);
    item.setSize(QSizeF(width, 402));
    item.setActive(true);
    item.setBackgroundColor(QColor(0x10, 0x00, 0x00));
    item.setSeamColor(QColor(0x20, 0x00, 0x00));
    item.setTroughColor(QColor(0x30, 0x00, 0x00));
    item.setFillColor(QColor(0x40, 0x00, 0x00));
    item.setPeakColor(QColor(0x50, 0x00, 0x00));
    item.setClipOffColor(QColor(0x60, 0x00, 0x00));
    item.setGapColor(QColor(0x80, 0x00, 0x00));
    item.setTickColor(QColor(0x90, 0x00, 0x00));
    item.setColorMapFill(true);
    view.show();
    QTRY_VERIFY(view.isExposed());
    for (int lanes : {1, 6}) {
        for (const double level : {-50.0, -30.0, -8.0}) {
            for (const bool scaleShown : {false, true}) {
                item.setLaneCount(lanes);
                item.setChannelCountHint(lanes);
                item.setScaleShown(scaleShown);
                item.m_channels.assign(static_cast<std::size_t>(lanes), {});
                for (auto &channel : item.m_channels) {
                    channel.observe(level, 0);
                    // No peak tick here: it has its own check below.
                    channel.peak = LevelMeterItem::kFloorDb;
                }
                item.m_shown = item.m_channels;
                item.update();
                const QImage image = view.grabWindow();
                QVERIFY(!image.isNull());
                const double scale = double(image.width()) / view.width();
                const auto color = [&](double x, double y) { return image.pixelColor(int(x * scale), int(y * scale)); };
                const auto red = [&](double x, double y) { return color(x, y).red(); };
                const QString where = QStringLiteral("%1 lanes at %2 dB, scale %3").arg(lanes).arg(level).arg(scaleShown);
                for (int lane = 0; lane < lanes; ++lane) {
                    const auto [top, bottom] = LevelMeterItem::laneRows(lane, lanes, 402);
                    const double height = bottom - top;
                    const double meterBottom = bottom - 3;
                    const double scaleHeight = height - 6 - 3 - 1;
                    const auto at = [&](double db) { return meterBottom - LevelMeterItem::meterPosition(db, scaleHeight); };
                    // The plot's floor only above and below the meter; the
                    // lamp and the fill out to the column's right edge.
                    for (double x = LevelMeterItem::kTrackLeft + 0.5; x < width; x += 1) {
                        for (const double y : {top + 1, bottom - 1}) {
                            QVERIFY2(red(x, y) == 0x10, qPrintable(where + QStringLiteral(": x %1, y %2").arg(x).arg(y)));
                        }
                        if (x > LevelMeterItem::kBarLeft) {
                            QVERIFY2(red(x, top + 4) == 0x60 && color(x, meterBottom - 1.5) == color(5, meterBottom - 1.5),
                                     qPrintable(where + QStringLiteral(": x %1").arg(x)));
                        }
                    }
                    // The fill, a ramp of the map's colours: at its bottom
                    // the colour of the floor's level, at its top that of
                    // its own (#373 H10 V2).
                    const double fillTop = at(level);
                    const double scalePixels = scaleHeight * scale;
                    const int rows = qRound(LevelMeterItem::meterPosition(level, scalePixels));
                    for (const auto &[y, row] : {std::pair{meterBottom - 0.5, 0}, std::pair{fillTop + 0.5, rows - 1}}) {
                        const QColor fill = color(5, y);
                        const QColor wanted = rampColor(row, scalePixels, 132);
                        QVERIFY2(std::abs(fill.red() - wanted.red()) <= 8 && std::abs(fill.green() - wanted.green()) <= 8
                                     && std::abs(fill.blue() - wanted.blue()) <= 8,
                                 qPrintable(where + QStringLiteral(": fill %1 at y %2, the map %3")
                                                        .arg(fill.name()).arg(y).arg(wanted.name())));
                    }
                    // The ticks only with the scale, where they are; the
                    // trough or the fill without.
                    const std::vector<double> ticks = LevelMeterItem::ticksDb(height);
                    for (const double tick : {0.0, -12.0, -24.0, -48.0}) {
                        const bool listed = std::find(ticks.begin(), ticks.end(), tick) != ticks.end();
                        const double y = std::max(meterBottom - scaleHeight, std::round(at(tick))) + 0.5;
                        const int under = tick < level ? 0x00 : 0x30;
                        if (scaleShown && listed) {
                            QVERIFY2(red(5, y) == 0x90, qPrintable(where + QStringLiteral(": tick at %1 dB").arg(tick)));
                        } else if (under == 0x30) {
                            QVERIFY2(red(5, y) == 0x30, qPrintable(where + QStringLiteral(": no tick at %1 dB, lane %2, y %3, drawn %4").arg(tick).arg(lane).arg(y).arg(color(5, y).name())));
                        } else {
                            QVERIFY2(red(5, y) != 0x90, qPrintable(where + QStringLiteral(": no tick at %1 dB").arg(tick)));
                        }
                    }
                }
            }
        }
    }

    // The peak tick on the fill, a level just under it: a 1 px gap under
    // it keeps it apart from the fill's colour.
    item.setLaneCount(1);
    item.setChannelCountHint(1);
    item.setScaleShown(false);
    item.m_channels.assign(1, {});
    item.m_channels[0].observe(-20, 0);
    item.m_channels[0].level = -20.1;
    item.m_shown = item.m_channels;
    item.update();
    const QImage image = view.grabWindow();
    const double scale = double(image.width()) / view.width();
    const double meterBottom = 402 - 3;
    const double scaleHeight = 402 - 6 - 3 - 1;
    const double peak = meterBottom - LevelMeterItem::meterPosition(-20, scaleHeight);
    const double tickY = std::round(peak - 1.0);
    QCOMPARE(image.pixelColor(int(5 * scale), int((tickY + 0.5) * scale)).red(), 0x50);
    QCOMPARE(image.pixelColor(int(5 * scale), int((tickY + 1.5) * scale)).red(), 0x50);
    QCOMPARE(image.pixelColor(int(5 * scale), int((tickY + 2.5) * scale)).red(), 0x80);
    QVERIFY(image.pixelColor(int(5 * scale), int((tickY + 4.5) * scale)).red() > 0x80);
}

// The colour map's fill, loud, in the plot's colours (#373 H8): no pixel of
// it lighter than kFillLightness of the map's top, white, so the meter is
// not the brightest thing in the window; its peak tick at full brightness.
void LevelMeterTest::loudFillIsCappedAndItsPeakIsFull() {
    constexpr double width = LevelMeterItem::kColumnWidth;
    QQuickWindow view;
    view.resize(int(width), 300);
    view.setColor(Qt::black);
    LevelMeterItem item(view.contentItem());
    item.setSize(QSizeF(width, 300));
    item.setActive(true);
    item.setBackgroundColor(Qt::black);
    item.setSeamColor(QColor(0x0a, 0x03, 0x18));
    item.setTroughColor(QColor(0x2b, 0x1f, 0x43));
    item.setGapColor(QColor(0x0a, 0x03, 0x18));
    item.setPeakColor(Qt::white);
    item.setClipOffColor(QColor(0x33, 0x33, 0x33));
    item.setClipColor(QColor(0xed, 0x5a, 0x6a));
    item.setColorMapFill(true);
    view.show();
    QTRY_VERIFY(view.isExposed());
    for (const double range : {50.0, 132.0, 150.0}) {
        for (const double level : {-6.0, -1.0, -0.01}) {
            item.setDbRange(range);
            item.setLaneCount(1);
            item.setChannelCountHint(1);
            item.m_channels.assign(1, {});
            item.m_channels[0].observe(level, 0);
            item.m_shown = item.m_channels;
            item.update();
            const QImage image = view.grabWindow();
            QVERIFY(!image.isNull());
            const double scale = double(image.width()) / view.width();
            const double meterBottom = 300 - 3;
            const double scaleHeight = 300 - 6 - 3 - 1;
            // On the scale, under the lamp and its gap.
            const double tickY = std::max(3.0 + LevelMeterItem::kLampHeight + 1.0,
                std::round(meterBottom - LevelMeterItem::meterPosition(level, scaleHeight) - 1.0));
            const QString where = QStringLiteral("%1 dB in %2").arg(level).arg(range);
            // The peak tick, white, across the bar.
            for (double x = LevelMeterItem::kBarLeft + 0.5; x < width; x += 1) {
                QVERIFY2(image.pixelColor(int(x * scale), int((tickY + 0.5) * scale)) == QColor(Qt::white),
                         qPrintable(where));
            }
            // The fill, under the tick and its gap, its lightest pixel at
            // most at the cap.
            double lightest = 0.0;
            for (int y = int((tickY + 3) * scale); y < int(meterBottom * scale); ++y) {
                for (int x = int(LevelMeterItem::kBarLeft * scale); x < image.width(); ++x) {
                    lightest = std::max(lightest, LevelMeterItem::lightness(image.pixelColor(x, y)));
                }
            }
            QVERIFY2(lightest > 0.5 && lightest <= LevelMeterItem::kFillLightness,
                     qPrintable(where + QStringLiteral(": %1").arg(lightest)));
            // Each row of the fill is the map's colour of its level, and
            // loud, over the cap, the map's colour at the cap, unscaled
            // (#373 H9 T2, H10 V2).
            const QColor capColor = QColor::fromRgb(SpectrogramColorMap::colorAt(LevelMeterItem::fillCapPosition()));
            const double scalePixels = scaleHeight * scale;
            for (int y = int((tickY + 3) * scale); y < int(meterBottom * scale); y += 7) {
                const QColor pixel = image.pixelColor(int(8 * scale), y);
                const QColor wanted = rampColor(int(meterBottom * scale) - 1 - y, scalePixels, range);
                QVERIFY2(pixel == wanted, qPrintable(where + QStringLiteral(": y %1 is %2, the map %3")
                                                         .arg(y).arg(pixel.name(), wanted.name())));
            }
            const QColor topmost = image.pixelColor(int(8 * scale), int((tickY + 3) * scale));
            if (LevelMeterItem::lightness(QColor::fromRgb(SpectrogramColorMap::colorOfLevel(level - 1.0, range)))
                > LevelMeterItem::kFillLightness) {
                QVERIFY2(topmost == capColor, qPrintable(where + QStringLiteral(": top %1, the cap %2")
                                                             .arg(topmost.name(), capColor.name())));
            }
        }
    }
}

void LevelMeterTest::colorMapFillIsARamp_data() {
    QTest::addColumn<double>("dpr");
    QTest::addColumn<double>("level");
    for (const double dpr : {1.0, 2.0}) {
        for (const double level : {-40.0, -20.0, -4.0}) {
            QTest::addRow("%gx, %g dB", dpr, level) << dpr << level;
        }
    }
}

// On the spectrogram, the fill is a ramp of the plot's colour map (#373 H10
// V2): each of its rows, from the bottom up to the level, takes the map's
// colour of the level at that row, up to the cap; above the level, only
// the trough and the peak tick.
void LevelMeterTest::colorMapFillIsARamp() {
    QFETCH(double, dpr);
    QFETCH(double, level);
    constexpr double width = LevelMeterItem::kColumnWidth;
    constexpr int height = 300;
    ScaledMeter meter(QSize(int(width), height), dpr);
    QVERIFY(meter.initialized);
    LevelMeterItem &item = meter.item;
    const QColor background(0x10, 0x00, 0x00);
    const QColor trough(0x30, 0x00, 0x00);
    const QColor peak(0x50, 0x00, 0x00);
    const QColor gap(0x80, 0x00, 0x00);
    item.setActive(true);
    item.setBackgroundColor(background);
    item.setSeamColor(QColor(0x20, 0x00, 0x00));
    item.setTroughColor(trough);
    item.setFillColor(QColor(0x40, 0x00, 0x00));
    item.setPeakColor(peak);
    item.setClipOffColor(QColor(0x60, 0x00, 0x00));
    item.setGapColor(gap);
    item.setTickColor(QColor(0x90, 0x00, 0x00));
    item.setColorMapFill(true);
    item.setDbRange(132);
    item.setLaneCount(1);
    item.setChannelCountHint(1);
    item.m_channels.assign(1, {});
    item.m_channels[0].observe(level, 0);
    // The peak held 6 dB over the level, apart from the fill.
    item.m_channels[0].peak = std::min(0.0, level + 6.0);
    item.m_channels[0].hold = 1.0;
    item.m_shown = item.m_channels;
    item.update();
    const QImage image = meter.render();
    QCOMPARE(image.size(), QSize(int(width * dpr), int(height * dpr)));

    // In device pixels: the scale under the lamp and its gap, and the fill.
    const int meterBottom = int((height - LevelMeterItem::kInset) * dpr);
    const int scaleTop = int((LevelMeterItem::kInset + LevelMeterItem::kLampHeight + 1.0) * dpr);
    const double scalePixels = meterBottom - scaleTop;
    const int rows = qRound(LevelMeterItem::meterPosition(level, scalePixels));
    QVERIFY(rows > 10);
    const auto pixel = [&](int x, int row) { return image.pixelColor(x, meterBottom - 1 - row); };
    const int barLeft = int(LevelMeterItem::kBarLeft * dpr);
    // The bottom, the middle and the top of the fill, across the bar.
    for (const int row : {0, 1, rows / 2, rows - 2, rows - 1}) {
        const QColor wanted = rampColor(row, scalePixels, 132);
        for (int x = barLeft; x < image.width(); ++x) {
            QVERIFY2(pixel(x, row) == wanted, qPrintable(QStringLiteral("row %1, x %2: %3, the map %4")
                .arg(row).arg(x).arg(pixel(x, row).name(), wanted.name())));
        }
    }
    // A ramp: lighter towards the level, and never over the cap.
    const auto lightness = [&](int row) { return LevelMeterItem::lightness(pixel(barLeft, row)); };
    QVERIFY(lightness(0) < lightness(rows / 2) && lightness(rows / 2) <= lightness(rows - 1));
    for (int row = 0; row < rows; ++row) {
        QVERIFY2(lightness(row) <= LevelMeterItem::kFillLightness, qPrintable(QString::number(row)));
    }
    // Above the level, the trough and the peak tick only.
    int peakRows = 0;
    for (int row = rows; row < scalePixels; ++row) {
        for (int x = barLeft; x < image.width(); ++x) {
            const QColor shown = pixel(x, row);
            QVERIFY2(shown == trough || shown == peak, qPrintable(QStringLiteral("row %1, x %2: %3")
                .arg(row).arg(x).arg(shown.name())));
        }
        peakRows += pixel(barLeft, row) == peak ? 1 : 0;
    }
    QCOMPARE(peakRows, int(2 * dpr));
    // The column's floor under the meter.
    QCOMPARE(image.pixelColor(barLeft, image.height() - 1), background);
}

void LevelMeterTest::stoppedMeterIsAnEmptyTrough() {
    QQuickWindow view;
    view.resize(10, 200);
    LevelMeterItem item(view.contentItem());
    item.setSize(QSizeF(10, 200));
    item.setTroughColor(QColor(0x30, 0x00, 0x00));
    item.setClipOffColor(QColor(0x60, 0x00, 0x00));
    item.setFillColor(QColor(0x40, 0x00, 0x00));
    view.show();
    QTRY_VERIFY(view.isExposed());
    item.m_channels.assign(2, {});
    for (auto &channel : item.m_channels) channel.observe(-6, 0);
    item.m_shown = item.m_channels;
    QVERIFY(!item.active());
    item.update();
    QImage image = view.grabWindow();
    const double scale = double(image.width()) / view.width();
    // No fill, scale, lamp or tick: the trough from top to bottom.
    for (double y : {4.0, 6.5, 100.0, 190.0}) {
        QCOMPARE(image.pixelColor(int(5 * scale), int(y * scale)).red(), 0x30);
    }
    const auto readout = item.laneReadout(0);
    QVERIFY(std::isinf(readout.value("level").toDouble()));
    QVERIFY(!readout.value("clipping").toBool());
    // Its width stays the same: the plot narrows only with the option.
    QCOMPARE(item.implicitWidth(), LevelMeterItem::kColumnWidth);
    item.setActive(true);
    item.update();
    image = view.grabWindow();
    QCOMPARE(image.pixelColor(int(5 * scale), int(4 * scale)).red(), 0x60);
    QCOMPARE(image.pixelColor(int(5 * scale), int(190 * scale)).red(), 0x40);
}

void LevelMeterTest::oneLaneShowsTheLoudestChannel() {
    LevelMeterItem item;
    item.setActive(true);
    item.m_shown.assign(3, {});
    item.m_shown[0].observe(-20, 0);
    item.m_shown[1].observe(-8, 0.5);
    item.m_shown[2].observe(0, 1.0);
    item.setLaneCount(1);
    auto readout = item.laneReadout(0);
    // The loudest fill is the second channel's, the highest peak and the
    // clip the third's: its full-scale bin is still held after 1 s.
    QCOMPARE(readout.value("level").toDouble(), -18.0);
    QCOMPARE(readout.value("peak").toDouble(), 0.0);
    QVERIFY(readout.value("clipping").toBool());
    // A lane each: each its own channel.
    item.setLaneCount(3);
    readout = item.laneReadout(0);
    QCOMPARE(readout.value("level").toDouble(), -20.0);
    QVERIFY(!readout.value("clipping").toBool());
    readout = item.laneReadout(2);
    QCOMPARE(readout.value("level").toDouble(), -20.0);
    QVERIFY(readout.value("clipping").toBool());
}

void LevelMeterTest::readoutGivesExactLevelsOnlyToReaders() {
    SteppedMeter meter([](double t) -> float { return t < 0.5 ? -12.5F : -60.0F; });
    QVERIFY(meter.exposed);
    QSignalSpy readouts(&meter.item, &LevelMeterItem::readoutChanged);
    meter.framesUntil(0.3);
    QTest::qWait(300);
    // Nobody reads the levels: nobody hears of them.
    QCOMPARE(readouts.count(), 0);
    meter.item.setReadoutWanted(true);
    QCOMPARE(readouts.count(), 1);
    QCOMPARE(meter.item.readoutRevision(), 1);
    QCOMPARE(meter.item.laneReadout(0).value("level").toDouble(), -12.5);
    meter.framesUntil(0.4);
    // At most every 250 ms.
    QTRY_COMPARE(readouts.count(), 2);
    meter.framesUntil(0.45);
    QTest::qWait(50);
    QCOMPARE(readouts.count(), 2);
}

void LevelMeterTest::shownLevelsFollowTheBallistics() {
    // Loud, a lower passage under the held peak, a short burst, then silence.
    const auto program = [](double t) -> float {
        if (t < 0.2) return -3;
        if (t < 0.7) return -20;
        if (t < 0.75) return -9;
        return -60;
    };
    LevelMeterItem item;
    item.m_playing = true;
    item.m_window = levelWindow(0, 8, program);
    ReferenceMeter reference(item.m_window);
    const std::vector<double> frameSeconds{1.0 / 60, 1.0 / 144, 0.0123, 0.031, 1.0 / 360};
    double position = 0;
    for (int frame = 0; position < 7.0; ++frame) {
        const double elapsed = frameSeconds[static_cast<std::size_t>(frame) % frameSeconds.size()];
        position += elapsed;
        item.advance(position, elapsed);
        // The bars show the newest bin heard; the state runs on to the frame.
        const QString mismatch = reference.mismatch(item, position);
        QVERIFY2(mismatch.isEmpty(), qPrintable(mismatch));
        if (position > 0.05 && position < 1.6) QCOMPARE(item.m_shown[0].peak, -3.0);
    }
    // The hold ended 1.5 s after the loud passage; the peak fell at 20 dB/s.
    QCOMPARE(item.m_shown[0].peak, -60.0);
    QCOMPARE(item.m_shown[0].level, -60.0);
    // Paused, the bars decay from the state at the frame.
    item.m_window = {};
    item.m_playing = false;
    item.m_channels[0] = {-12, -6, 0.25};
    for (const double elapsed : {0.1, 0.1, 0.2, 0.5}) {
        auto expected = item.m_channels[0];
        expected.decay(elapsed);
        item.advance(position, elapsed);
        QCOMPARE(item.m_shown[0].level, expected.level);
        QCOMPARE(item.m_shown[0].peak, expected.peak);
        QVERIFY(std::isnan(item.m_shownAt));
    }
    QCOMPARE(item.m_shown[0].level, -12.0 - kRelease * 0.9);
    QCOMPARE(item.m_shown[0].peak, -6.0 - kPeakRelease * 0.65);
}

void LevelMeterTest::steadyLevelsDrawNoFrames() {
    PlayingMeter meter([](double) { return -6.0F; });
    QTRY_COMPARE(meter.fill(), pixels(-6));
    QCOMPARE(meter.peak(), pixels(-6));
    QTest::qWait(300);
    const double fps = meter.framesPerSecond(1500);
    qInfo("steady tone: %.1f frames/s", fps);
    QCOMPARE(fps, 0.0);
    QCOMPARE(meter.fill(), pixels(-6));
    // It keeps reading ahead, and windows keep coming.
    QVERIFY(meter.item.m_wakeTimer.isActive());
    QVERIFY(meter.item.m_wakeTimer.remainingTime() <= 500);
    QVERIFY(meter.item.m_window.end > meter.position());
    // A new size is drawn at once.
    meter.frames = 0;
    meter.item.setHeight(610);
    QTRY_COMPARE(meter.fill(), qRound(LevelMeterItem::meterPosition(-6, 600)));
    QVERIFY(meter.frames > 0);
    QCOMPARE(meter.framesPerSecond(500), 0.0);
}

void LevelMeterTest::silenceDrawsNoFrames() {
    PlayingMeter meter([](double) { return -60.0F; });
    QTRY_VERIFY(meter.item.m_window.channels > 0);
    QTest::qWait(300);
    const double fps = meter.framesPerSecond(1500);
    qInfo("silence: %.1f frames/s", fps);
    QCOMPARE(fps, 0.0);
    QCOMPARE(meter.fill(), 0);
    QCOMPARE(meter.peak(), -1);
    // Stopped, nothing is drawn and nothing waits.
    meter.item.setSourcePath(QString());
    QTest::qWait(50);
    QCOMPARE(meter.framesPerSecond(500), 0.0);
    QVERIFY(!meter.item.m_wakeTimer.isActive());
}

void LevelMeterTest::pauseDrawsOnlyTheDecay() {
    SteppedMeter meter([](double) { return -54.0F; });
    QVERIFY(meter.exposed);
    meter.framesUntil(0.5);
    QCOMPARE(meter.fill(), pixels(-54));
    QCOMPARE(meter.peak(), pixels(-54));
    QCOMPARE(meter.framesUntil(1.0), 0);
    meter.setAdvancing(false);
    meter.item.setPlaying(false);
    // The bar falls to the floor in 0.3 s, a frame for each tick.
    QVERIFY(meter.framesUntil(1.35) >= 5);
    QCOMPARE(meter.fill(), 0);
    // The peak holds for 1.5 s from the last bin: no frames, and the timer
    // waits for the hold to end and the tick to leave its pixel: half of
    // one is 0.075 dB there, 3.75 ms.
    QVERIFY(meter.item.m_wakeTimer.isActive());
    const int expected = int(std::ceil((meter.item.m_shown[0].hold + 0.075 / kPeakRelease) * 1000));
    QVERIFY2(std::abs(meter.item.m_wakeTimer.interval() - expected) <= 1,
             qPrintable(QString::number(meter.item.m_wakeTimer.interval())));
    QCOMPARE(meter.framesUntil(2.45), 0);
    QCOMPARE(meter.peak(), pixels(-54));
    // Then it falls 6 dB at 20 dB/s, at most a frame per pixel, and settles.
    const int falling = meter.framesUntil(3.1);
    QCOMPARE(meter.peak(), -1);
    QVERIFY(falling > 5);
    QVERIFY(falling <= pixels(-54) + 1);
    QCOMPARE(meter.framesUntil(4.0), 0);
    QVERIFY(!meter.item.m_wakeTimer.isActive());
}

void LevelMeterTest::changingLevelsRedraw() {
    // Quiet, a step up at 1 s, then alternating every 0.1 s from 1.5 s.
    SteppedMeter meter([](double t) -> float {
        if (t < 1.0) return -40;
        if (t < 1.5) return -6;
        return std::fmod(t, 0.2) < 0.1 ? -6 : -30;
    });
    QVERIFY(meter.exposed);
    meter.framesUntil(0.4);
    QCOMPARE(meter.fill(), pixels(-40));
    QCOMPARE(meter.framesUntil(0.9), 0);
    // Idle, the meter waits for the end of the first loud bin.
    std::size_t loud = 0;
    while (meter.item.m_window.peaksDb[loud * 2] != -6.0F) ++loud;
    const double loudEnd = double(loud + 1) * kBinSeconds;
    QVERIFY(meter.item.m_wakeTimer.isActive());
    const int wait = meter.item.m_wakeTimer.interval();
    QVERIFY2(std::abs(wait - std::ceil((loudEnd - meter.time) * 1000)) <= 1, qPrintable(QString::number(wait)));
    // Its wake draws the step.
    meter.at(meter.time + wait / 1000.0);
    QVERIFY(meter.frame());
    QCOMPARE(meter.fill(), pixels(-6));
    meter.framesUntil(1.6);
    const int drawn = meter.framesUntil(2.6);
    // The quiet halves fall a pixel or more each frame; the loud halves hold.
    qInfo("alternating levels: %d of 60 frames drawn", drawn);
    QVERIFY(drawn >= 30);
}

void LevelMeterTest::idleMeterWakesForItsNextPixel() {
    SteppedMeter meter([](double) { return -60.0F; });
    QVERIFY(meter.exposed);
    meter.setAdvancing(false);
    meter.item.setPlaying(false);
    auto &item = meter.item;
    // The bar is down; the peak holds at -30 dB for another second.
    item.m_channels[0] = {-60, -30, 1.0};
    item.m_shown = item.m_channels;
    QVERIFY(meter.frame());
    QCOMPARE(meter.peak(), pixels(-30));
    QVERIFY(!meter.frame());
    // The tick leaves its pixel 0.15 dB below, 7.5 ms after the hold:
    // between -48 and -24 dB a pixel is 0.3 dB.
    QVERIFY(item.m_wakeTimer.isActive());
    QCOMPARE(item.m_wakeTimer.interval(), 1008);
    // A falling bar wakes for each pixel: half of one, 0.15 dB, takes 7.5 ms.
    item.m_channels[0] = {-30, -30, 1.0};
    item.m_shown = item.m_channels;
    QVERIFY(meter.frame());
    QVERIFY(!meter.frame());
    QCOMPARE(item.m_wakeTimer.interval(), 8);
    // A lit clip lamp wakes when it goes out.
    item.m_channels[0] = {-60, -60, 0.0, 0.5};
    item.m_shown = item.m_channels;
    QVERIFY(meter.frame());
    QVERIFY(meter.item.m_painted[0].clip);
    QVERIFY(!meter.frame());
    QCOMPARE(item.m_wakeTimer.interval(), 500);
}

void LevelMeterTest::pauseAndResumeKeepTheTimeBetween() {
    {
        // Resumed after an idle pause, the hold has run on meanwhile.
        SteppedMeter meter([](double) { return -30.0F; });
        QVERIFY(meter.exposed);
        meter.framesUntil(0.5);
        meter.setAdvancing(false);
        meter.item.setPlaying(false);
        meter.framesUntil(1.0);
        const double hold = meter.item.m_channels[0].hold;
        QVERIFY(std::abs(hold - 1.0) < 0.01);
        meter.at(1.8);
        meter.setAdvancing(true);
        meter.item.setPlaying(true);
        QVERIFY(std::abs(meter.item.m_channels[0].hold - (hold - 0.8)) < 1e-9);
        QCOMPARE(meter.item.m_channels[0].peak, -30.0);
    }
    {
        // Paused after an idle stretch of playback, that stretch counts.
        SteppedMeter meter([](double t) { return t < 1.0 ? -6.0F : -60.0F; });
        QVERIFY(meter.exposed);
        ReferenceMeter reference(meter.item.m_window);
        meter.framesUntil(1.0);
        meter.at(1.8);
        meter.setAdvancing(false);
        meter.item.setPlaying(false);
        const double heardAt = reference.hear(1.8);
        auto expected = reference.channel;
        expected.decay(1.8 - heardAt);
        const auto &state = meter.item.m_channels[0];
        QVERIFY(std::abs(state.level - expected.level) < 1e-9);
        QVERIFY(std::abs(state.peak - expected.peak) < 1e-9);
        QVERIFY(std::abs(state.hold - expected.hold) < 1e-9);
        // The bar fell 16 dB and the hold ran 0.8 s since the last loud bin.
        QVERIFY(std::abs(state.level - (-22.0)) < 0.1);
        QVERIFY(std::abs(state.hold - 0.7) < 0.01);
    }
}

void LevelMeterTest::idleWakesTakeEveryBufferedBin() {
    // A loud start, then silence with two short peaks: one under the held
    // peak, and one barely above the falling peak, which holds it again.
    SteppedMeter meter([](double t) -> float {
        if (t < 0.05) return -6;
        if (t >= 0.7 && t < 0.702) return -20;
        if (t >= 1.6 && t < 1.602) return -6.58F;
        return -60;
    });
    QVERIFY(meter.exposed);
    ReferenceMeter reference(meter.item.m_window);
    // Irregular wakes, with sleeps of up to 0.5 s over buffered peaks.
    for (const double time : {0.016, 0.033, 0.1, 0.5, 1.0, 1.1, 1.55, 1.9, 2.4, 3.0, 3.5, 4.5, 5.0}) {
        meter.at(time);
        meter.frame();
        const QString mismatch = reference.mismatch(meter.item, meter.position());
        QVERIFY2(mismatch.isEmpty(), qPrintable(mismatch));
        const double peak = reference.channel.peak;
        QCOMPARE(meter.peak(), peak > -60 ? pixels(peak) : -1);
    }
    // The second peak held the marker again until about 3.1 s; without it
    // the marker would be at the floor by now.
    const double expected = -6.58 - kPeakRelease * (5.0 - 1.6 - LevelMeterItem::kPeakHoldSeconds);
    QVERIFY2(std::abs(reference.channel.peak - expected) < 0.2,
             qPrintable(QString::number(reference.channel.peak)));
}

void LevelMeterTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    disableGpuShaderCaches();
    QTemporaryDir testStorage;
    if (!testStorage.isValid()) return 1;
    qputenv("XDG_DATA_HOME", QFile::encodeName(testStorage.filePath("data")));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(testStorage.filePath("config")));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(testStorage.filePath("cache")));
    // No desktop session's platform theme; TestSessionIsolation.h.
    isolateFromDesktopSession();
    // The software renderer, whose pixels the tests compare, whatever the
    // environment asks for; ScaledMeter draws through it.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    LevelMeterTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_level_meter.moc"
