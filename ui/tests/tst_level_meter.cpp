// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDataStream>
#include <QFile>
#include <QFutureWatcher>
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
#include <vector>
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
    void lateDataDoesNotReplayOldPeaks();
    void requestsAreBoundedAndObsoleteResultsAreDiscarded();
    void destructionCancelsPendingDecode();
    void sceneGraphDrawsFixedGradientAndThinSurroundRows();
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
    LevelMeterItem item;
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
    item.resetForSeek(0.02);
    QCOMPARE(item.m_channels[0].peak, -60.0);
    QCOMPARE(item.m_window.channels, 0);
    item.m_channels[0].observe(-3, 0);
    item.setSourcePath(QStringLiteral("next.wav"));
    QCOMPARE(item.m_channels[0].peak, -60.0);
    item.m_channels[0].observe(-3, 0);
    item.setSourcePath(QString());
    QCOMPARE(item.m_channels[0].peak, -60.0);
}

void LevelMeterTest::lateDataDoesNotReplayOldPeaks() {
    LevelMeterItem item;
    item.m_playing = true;
    item.m_window = {0, 1, 0.01, 1, std::vector<float>(100, -60)};
    item.m_window.peaksDb[0] = 0;
    item.advance(0.8, 0.8);
    QCOMPARE(item.m_channels[0].peak, -60.0);
    item.m_stagedWindow = {0.7, 1.7, 0.01, 1, std::vector<float>(100, -60)};
    item.m_stagedWindow.peaksDb[0] = 0;
    item.advance(0.81, 0.01);
    QCOMPARE(item.m_channels[0].peak, -60.0);
    QCOMPARE(item.m_stagedWindow.channels, 0);
    QCOMPARE(item.m_window.start, 0.7);
}

void LevelMeterTest::requestsAreBoundedAndObsoleteResultsAreDiscarded() {
    QQuickWindow view;
    view.resize(400, 40);
    LevelMeterItem item(view.contentItem());
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
    for (int i = 0; i < 20; ++i) item.resetForSeek(i * 0.01);
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
        item.update();
        QTest::qWait(30);
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

QTEST_MAIN(LevelMeterTest)
#include "tst_level_meter.moc"
