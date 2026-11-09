// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

// Every lane of the waveform across the whole view, at many sizes, zooms,
// rates and scales: a CTest entry of its own, as it paints far more than
// the other waveform tests, which an unoptimized build takes long to do.
class WaveformCoverageSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void waveformEditorDrawsEveryLaneAcrossTheView_data();
    void waveformEditorDrawsEveryLaneAcrossTheView();
};

void WaveformCoverageSmokeTest::waveformEditorDrawsEveryLaneAcrossTheView_data() {
    QTest::addColumn<QSize>("size");
    QTest::addColumn<int>("channels");
    QTest::addColumn<int>("rate");
    QTest::addColumn<int>("stepsOut");
    QTest::addColumn<bool>("playing");
    QTest::addColumn<bool>("split");
    QTest::addColumn<double>("scale");
    // The main window's pane and the fullscreen viewer's.
    const std::pair<const char *, QSize> sizes[] = {
        {"pane", QSize(1720, 300)}, {"viewer", QSize(2000, 837)}, {"wide-viewer", QSize(3440, 1412)}};
    for (const auto &[name, size] : sizes) {
        for (const int channels : {2, 6}) {
            for (const int rate : {48'000, 96'000}) {
                for (const int stepsOut : {0, 5, 8, 12}) {
                    for (const bool playing : {false, true}) {
                        QTest::addRow("%s-%dch-%dk-%dout-%s", name, channels, rate / 1000, stepsOut,
                            playing ? "playing" : "paused")
                            << size << channels << rate << stepsOut << playing << true << 1.0;
                    }
                }
            }
        }
        // The channels mixed in one lane.
        for (const int stepsOut : {0, 5, 8}) {
            QTest::addRow("%s-mixed-%dout-playing", name, stepsOut)
                << size << 6 << 96'000 << stepsOut << true << false << 1.0;
        }
    }
    // At 2x, around the zoom that draws samples directly: from 4 steps
    // out, samples are 3.3 logical pixels apart, to 7, 1.7, where tiles
    // draw them, 3.4 device pixels apart.
    const std::pair<const char *, QSize> hiDpiSizes[] = {
        {"pane", QSize(1720, 300)}, {"wide-viewer", QSize(3440, 1412)}};
    for (const auto &[name, size] : hiDpiSizes) {
        for (const int stepsOut : {4, 5, 6, 7, 8, 10}) {
            QTest::addRow("2x-%s-6ch-96k-%dout-playing", name, stepsOut)
                << size << 6 << 96'000 << stepsOut << true << true << 2.0;
        }
    }
}

void WaveformCoverageSmokeTest::waveformEditorDrawsEveryLaneAcrossTheView() {
    QFETCH(QSize, size);
    QFETCH(int, channels);
    QFETCH(int, rate);
    QFETCH(int, stepsOut);
    QFETCH(bool, playing);
    QFETCH(bool, split);
    QFETCH(double, scale);
    // At another scale, the item is in a window on a screen of it, which
    // the test draws rather than the window.
    QQuickWindow window;
    if (scale != 1.0) {
        QScreen *screen = hiDpiTestScreen(scale);
        if (screen == nullptr) QSKIP("Needs the offscreen platform's HiDPI test screens");
        QVERIFY(moveWindowToScreen(window, screen));
    }
    TestTime time;
    WaveformEditorItem item;
    if (scale != 1.0) item.setParentItem(window.contentItem());
    time.drive(item);
    item.setWidth(size.width());
    item.setHeight(size.height());
    item.setDurationSeconds(60.0);
    item.setViewMode(split ? 1 : 0);
    item.setChannelCountHint(channels);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(8192, static_cast<char>(40)));
    // What the view shows over the frames below, with the margins tiles
    // are made for.
    giveSampleDetail(item, rate, channels, 29.8, 30.45);
    // The deepest zoom, and steps of the wheel out from it.
    item.setZoomLevel(item.maximumZoomLevel() / std::pow(1.25, stepsOut));
    setTestPosition(item, 30.0);
    item.setPlaying(playing);
    setTestPosition(item, 30.0);
    QImage image(size * scale, QImage::Format_RGB32);
    image.setDevicePixelRatio(scale);
    const auto drawAndCheck = [&](const QString &when) {
        image.fill(Qt::black);
        QPainter painter(&image);
        item.paint(&painter);
        painter.end();
        double playheadX = -10.0;
        {
            QMutexLocker lock(&item.m_stateMutex);
            const auto [start, end] = item.visibleRangeLocked();
            playheadX = (item.displayedPositionSecondsLocked() - start) / (end - start)
                * size.width() * scale;
        }
        // The playhead, with its casing on each side.
        const int margin = 2 * static_cast<int>(std::ceil(scale));
        const QString empty = lanesWithEmptyColumns(
            image, split ? channels : 1, static_cast<int>(std::floor(playheadX)) - margin,
            static_cast<int>(std::floor(playheadX)) + margin);
        if (!empty.isEmpty()) {
            image.save(QStringLiteral("%1/waveform-lanes-%2.png")
                .arg(QDir::tempPath(), QString::fromLatin1(QTest::currentDataTag())));
        }
        return empty.isEmpty() ? QString() : when + QStringLiteral(": ") + empty;
    };
    // A sixth of a second of playback at 60 frames a second, each frame
    // drawn: the view is a new one each frame at these zooms. Tiles still
    // being made show the detail's envelope in their place.
    const int frames = playing ? 10 : 2;
    for (int frame = 0; frame < frames; ++frame) {
        if (frame > 0 && playing) time.advance(1.0 / 60.0);
        const QString empty = drawAndCheck(QStringLiteral("frame %1").arg(frame));
        QVERIFY2(empty.isEmpty(), qPrintable(empty));
        // As the window does once the frame is on screen.
        item.handleWindowFrameSwapped();
    }
    // The tiles then come, and are drawn.
    bool tiled = false;
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        tiled = item.playbackTilesEligibleLocked(start, end);
    }
    if (tiled) {
        const auto tilesCoverTheView = [&item]() {
            QMutexLocker lock(&item.m_stateMutex);
            const auto range = item.visibleRangeLocked();
            return item.playbackTilesCoverLocked(range.first, range.second);
        };
        QTRY_VERIFY_WITH_TIMEOUT(tilesCoverTheView(), kSlowBuildTimeoutMs);
        const QString empty = drawAndCheck(QStringLiteral("with its tiles"));
        QVERIFY2(empty.isEmpty(), qPrintable(empty));
    }
}

std::unique_ptr<QmlSmokeTest> makeWaveformCoverageSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<WaveformCoverageSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_waveform_coverage.moc"
