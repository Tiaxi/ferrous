// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDataStream>
#include <QFile>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <atomic>
#include <bit>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include "TestPlatform.h"
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

// A 400 px meter in a window, playing a 100 s track on its own clock.
struct MeterFixture {
    MeterFixture() {
        view.resize(400, 20);
        // As QML does: the item joins the window after it is made.
        item.setParentItem(view.contentItem());
        item.setSize(QSizeF(400, 20));
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
    int fill(int channel = 0) const {
        const auto index = static_cast<std::size_t>(channel) * 2;
        return item.m_paintedPixels.size() > index ? item.m_paintedPixels[index] : -2;
    }
    int peak(int channel = 0) const {
        const auto index = static_cast<std::size_t>(channel) * 2 + 1;
        return item.m_paintedPixels.size() > index ? item.m_paintedPixels[index] : -2;
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
        if (item.showsPainted(item.m_shown, item.deviceWidth())) return false;
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

// Device pixels of `db` on the 400 px meter.
int pixels(double db) { return qRound(400 * (db + 60) / 60); }
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
    void sceneGraphDrawsFixedGradientAndThinSurroundRows();
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
    channel.decay(0.5);
    QCOMPARE(channel.level, -36.0);
    QCOMPARE(channel.peak, -6.0);
    QCOMPARE(channel.hold, 1.0);
    channel.observe(-20, 0);
    QCOMPARE(channel.hold, 1.0);
    channel.decay(1.25);
    QCOMPARE(channel.level, -60.0);
    QCOMPARE(channel.peak, -9.0);
    QCOMPARE(channel.hold, 0.0);
    channel.observe(6, 0);
    QCOMPARE(channel.level, 0.0);
    QCOMPARE(channel.peak, 0.0);
    QCOMPARE(channel.hold, 1.5);
    channel.decay(10);
    QCOMPARE(channel.level, -60.0);
    QCOMPARE(channel.peak, -60.0);
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
            item.m_window = {0, 2, 1.0 / 1024, 2, std::vector<float>(4096)};
            const double quietLeft = silence ? -60.0 : -36.0;
            const double quietRight = silence ? -60.0 : -48.0;
            for (int point = 0; point < 2048; ++point) {
                item.m_window.peaksDb[point * 2] = point < 128 ? 0.0F : float(quietLeft);
                item.m_window.peaksDb[point * 2 + 1] = point < 128 ? -6.0F : float(quietRight);
            }
            item.advance(0.125, 0.125);
            QCOMPARE(item.m_channels[0].level, 0.0);
            QCOMPARE(item.m_channels[1].level, -6.0);
            for (int frame = 1; frame <= fps; ++frame) {
                item.advance(0.125 + double(frame) / fps, 1.0 / fps);
                if (frame == fps / 2) {
                    QVERIFY(std::abs(item.m_channels[0].level + 30) < 1e-8);
                    QVERIFY(std::abs(item.m_channels[1].level + 36) < 1e-8);
                }
            }
            QVERIFY(std::abs(item.m_channels[0].level - quietLeft) < 1e-8);
            QVERIFY(std::abs(item.m_channels[1].level - quietRight) < 1e-8);
            // Faster bar release leaves the independent peak hold intact.
            QCOMPARE(item.m_channels[0].peak, 0.0);
            QCOMPARE(item.m_channels[1].peak, -6.0);
            QVERIFY(std::abs(item.m_channels[0].hold - 0.5) < 1e-8);
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

void LevelMeterTest::sceneGraphDrawsFixedGradientAndThinSurroundRows() {
    QQuickWindow view;
    view.resize(600, 20);
    LevelMeterItem item(view.contentItem());
    item.setSize(QSizeF(600, 20));
    view.show();
    QTRY_VERIFY(view.isExposed());
    for (int count : {1, 2, 6, 8}) {
        item.setChannelCountHint(count);
        item.m_channels.assign(static_cast<std::size_t>(count), {});
        for (auto &channel : item.m_channels) channel.observe(-6, 0);
        item.m_channels[0].peak = -2;
        item.m_shown = item.m_channels;
        item.update();
        // Grabbing draws the window's scene as it is now.
        const QImage image = view.grabWindow();
        QVERIFY(!image.isNull());
        QCOMPARE(item.height(), 20.0);
        // grabWindow returns physical pixels; its QImage DPR metadata can be 1.
        const double scaleX = double(image.width()) / view.width();
        const double scaleY = double(image.height()) / view.height();
        const auto pixel = [&](int x, int y) {
            return image.pixelColor(qRound(x * scaleX), qRound(y * scaleY));
        };
        const auto low = pixel(100, 0);
        const auto high = pixel(500, 0);
        QVERIFY(low.green() > low.red());
        QVERIFY(high.red() > low.red());
        QCOMPARE(pixel(595, 0), QColor("#090c0e"));
        const int peakX = qRound(580 * scaleX);
        QCOMPARE(image.pixelColor(peakX, 0), QColor("#d6d3b9"));
        // Check physical neighbors so a two-pixel marker fails at any scale.
        QCOMPARE(image.pixelColor(peakX - 1, 0), QColor("#090c0e"));
        QCOMPARE(image.pixelColor(peakX + 1, 0), QColor("#090c0e"));
        if (count == 2) {
            QCOMPARE(pixel(10, 9), QColor("#202427"));
            QCOMPARE(pixel(10, 10), QColor("#202427"));
            QVERIFY(pixel(10, 11).green() > pixel(10, 11).red());
        }
    }
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
    // The hold ended 1.5 s after the loud passage; the peak fell at 12 dB/s.
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
    QCOMPARE(item.m_shown[0].level, -60.0);
    QCOMPARE(item.m_shown[0].peak, -6.0 - 12 * 0.65);
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
    meter.item.setWidth(600);
    QTRY_COMPARE(meter.fill(), qRound(600 * 54.0 / 60));
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
    // The bar falls to the floor in 0.1 s, a frame for each tick.
    QVERIFY(meter.framesUntil(1.2) >= 5);
    QCOMPARE(meter.fill(), 0);
    // The peak holds for 1.5 s from the last bin: no frames, and the timer
    // waits for the hold to end and the marker to leave its pixel (6.25 ms).
    QVERIFY(meter.item.m_wakeTimer.isActive());
    const int expected = int(std::ceil((meter.item.m_shown[0].hold + 0.00625) * 1000));
    QVERIFY2(std::abs(meter.item.m_wakeTimer.interval() - expected) <= 1,
             qPrintable(QString::number(meter.item.m_wakeTimer.interval())));
    QCOMPARE(meter.framesUntil(2.45), 0);
    QCOMPARE(meter.peak(), pixels(-54));
    // Then it falls 6 dB at 12 dB/s, at most a frame per pixel, and settles.
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
    // The marker leaves its pixel 0.075 dB below, 6.25 ms after the hold.
    QVERIFY(item.m_wakeTimer.isActive());
    QCOMPARE(item.m_wakeTimer.interval(), 1007);
    // A falling bar wakes for each pixel: half of one, 0.075 dB, takes 1.25 ms.
    item.m_channels[0] = {-30, -30, 1.0};
    item.m_shown = item.m_channels;
    QVERIFY(meter.frame());
    QVERIFY(!meter.frame());
    QCOMPARE(item.m_wakeTimer.interval(), 2);
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
        // The bar fell 48 dB and the hold ran 0.8 s since the last loud bin.
        QVERIFY(std::abs(state.level - (-54.0)) < 0.1);
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
        QCOMPARE(meter.peak(), peak > -60 ? qRound(400 * (peak + 60) / 60) : -1);
    }
    // The second peak held the marker again until about 3.1 s; without it
    // the marker would be near -47 dB by now.
    QVERIFY2(reference.channel.peak > -29.5 && reference.channel.peak < -29.2,
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
    QGuiApplication app(argc, argv);
    LevelMeterTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_level_meter.moc"
