// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

// The waveform, the level meter, and the waveform editor's detail, cache,
// crosshair and rulers.
class WaveformSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void waveformSurfaceFollowsSharedChannelAndOverlaySettings();
    void waveformSurfaceMuteButtonsFollowTheMutedChannels();
    void waveformPointerBringsTheControlsBackOverItsOverlays();
    void pointerOverTheViewBringsTheViewerControlsBack();
    void levelMeterKeepsVisualizationHeightAndFollowsModes();
    void waveformProgressInvalidatesOnlyTailSpan();
    void waveformPeakUpdatesInvalidateChangedSuffix();
    void waveformEditorParsesSignedSampleWindow();
    void waveformEditorCachedDecoderPreservesOverlappingSamples();
    void waveformEditorDownmixUsesMixedExtremaAtEveryZoom();
    void waveformEditorCancelsAndBoundsObsoleteDecodes();
    void waveformEditorDetailArrivesWhileScrolling();
    void waveformEditorSignalsWithoutItsStateLocked();
    void waveformEditorSeeksWhereTheCrosshairShows_data();
    void waveformEditorSeeksWhereTheCrosshairShows();
    void waveformEditorSampleMarkersRequireSampleResolution();
    void waveformEditorConnectsSamplesBeforeMarkers();
    void waveformEditorScrollsInsideCachedDetailWithoutRebuild();
    void waveformEditorUsesChannelHintBeforeDetailArrives();
    void waveformEditorCoalescesPlaybackRequestsWithoutStarvingTimer();
    void waveformEditorPendingWindowSuppressesDuplicateRequest();
    void waveformEditorZoomSupersedesCoarsePendingRequest();
    void waveformEditorResizeRaisesDetailRequestDensity();
    void waveformEditorDeepZoomRequestPreservesViewportDensity();
    void waveformEditorQuantizesRequestForRequiredBinDensity();
    void waveformEditorFullscreenRequestStaysInTargetBin();
    void waveformEditorSampleViewPrefetchesUsefulSpan();
    void waveformEditorPrefetchesBeforeDetailBoundary();
    void waveformEditorFullscreenPrefetchDoesNotWaitPastCoverage();
    void waveformEditorKeepsOverlappingDetailDuringHandoff();
    void waveformEditorWideDetailCacheKeepsViewportDensity();
    void waveformEditorWholeTrackAcceptsDecodedEndpointTolerance();
    void waveformEditorFullscreenWholeTrackRetainsCache();
    void waveformEditorZoomOutDefersOverviewUntilDetailReady();
    void waveformEditorDeferredZoomOutCommitsCompletedDetailCache();
    void waveformEditorZoomOutKeepsReadyDetail();
    void waveformEditorZoomInRetainsCoveredCacheWhileRefining();
    void waveformEditorSparseZoomDetailFallsBackUntilReady();
    void waveformEditorPerChannelZoomShowsOlderDetailUntilTheNewArrives();
    void waveformEditorSparseDetailCoversEachPointsFrames();
    void waveformEditorSeparatesChannelPanes();
    void waveformEditorRestrictsDetailWorkToVisiblePoints();
    void waveformEditorDetailPointsUseAbsoluteSampleTimes();
    void waveformEditorReusesSameSizedCache();
    void waveformEditorCacheHandoffsKeepAbsolutePixelGrid();
    void waveformEditorReplacementCacheDoesNotReuseOldRaster();
    void waveformEditorBuildsReplacementCacheIncrementally();
    void waveformEditorPaintDefersGuiContinuation();
    void waveformEditorZoomedPlaybackUsesScrollingCache();
    void waveformEditorPlaybackOverviewCacheHasForwardHeadroom();
    void waveformEditorCachedPaintClearsUncoveredPixels();
    void waveformEditorFollowsThePlaybackClock();
    void waveformEditorSeeksMoveAtOnce();
    void waveformEditorHoverDrawsCrosshairAndReadouts();
    void waveformEditorCrosshairLabelsMatchSpectrogramMargins();
    void waveformEditorCrosshairLabelsKeepApartInShortPlots();
    void waveformEditorCrosshairLabelsStayReadableInNarrowPlots();
    void waveformEditorDrawsNoCrosshairLabelItLeavesOut();
    void waveformOverlaysKeepClearOfTheReservedCorner();
    void waveformEditorSampleViewRepaintsCrosshairCleanly();
    void waveformEditorSampleViewUsesSmoothInterpolation();
    void waveformEditorReclampsZoomForDecodedSampleRate();
    void waveformEditorRulersFollowGridSetting();
    void waveformEditorRulersRemainVisibleWhenZoomed();
    void waveformEditorPlayheadIsThinAndNeutral();
    void waveformEditorOverviewRespectsCoverageAndChannelIdentity();
    void waveformEditorReferenceLineContrastsWaveform();
    void waveformEditorContrastLinesUseFboSafeCompositionModes();
    void waveformEditorFpsOverlayTracksPaintRate();
    void waveformEditorCachesAtDevicePixels();
    void waveformEditorScaleChangeRedrawsItsCacheOnce();
    void waveformItemCachesAtDevicePixels();
};

namespace {

class CompositionRecordingPaintEngine final : public QPaintEngine {
public:
    CompositionRecordingPaintEngine()
        : QPaintEngine(QPaintEngine::AlphaBlend
                       | QPaintEngine::PorterDuff
                       | QPaintEngine::PainterPaths) {}

    bool begin(QPaintDevice *device) override {
        setPaintDevice(device);
        setActive(true);
        return true;
    }

    bool end() override {
        setActive(false);
        return true;
    }

    void updateState(const QPaintEngineState &state) override {
        if (state.state() & QPaintEngine::DirtyCompositionMode) {
            compositionModes.push_back(state.compositionMode());
        }
    }

    void drawLines(const QLine *, int) override {}
    void drawLines(const QLineF *, int) override {}
    void drawPixmap(const QRectF &, const QPixmap &, const QRectF &) override {}
    Type type() const override { return QPaintEngine::User; }

    QVector<QPainter::CompositionMode> compositionModes;
};

class CompositionRecordingPaintDevice final : public QPaintDevice {
public:
    QPaintEngine *paintEngine() const override {
        return const_cast<CompositionRecordingPaintEngine *>(&engine);
    }

    mutable CompositionRecordingPaintEngine engine;

protected:
    int metric(PaintDeviceMetric metric) const override {
        switch (metric) {
        case PdmWidth:
        case PdmWidthMM:
        case PdmHeight:
        case PdmHeightMM:
            return 64;
        case PdmDpiX:
        case PdmDpiY:
        case PdmPhysicalDpiX:
        case PdmPhysicalDpiY:
            return 96;
        case PdmDevicePixelRatio:
            return 1;
        case PdmDevicePixelRatioScaled:
            return static_cast<int>(QPaintDevice::devicePixelRatioFScale());
        case PdmNumColors:
            return 16'777'216;
        case PdmDepth:
            return 32;
        default:
            return 0;
        }
    }
};

} // namespace

// What a paint draws: its straight lines with their pens, and its text.
class LineAndTextRecordingPaintEngine final : public QPaintEngine {
public:
    LineAndTextRecordingPaintEngine() : QPaintEngine(QPaintEngine::AllFeatures) {}

    bool begin(QPaintDevice *device) override {
        setPaintDevice(device);
        setActive(true);
        return true;
    }
    bool end() override {
        setActive(false);
        return true;
    }
    void updateState(const QPaintEngineState &) override {}
    void drawLines(const QLineF *drawn, int count) override {
        for (int i = 0; i < count; ++i) {
            lines.push_back({drawn[i], painter()->pen()});
        }
    }
    void drawLines(const QLine *drawn, int count) override {
        for (int i = 0; i < count; ++i) {
            lines.push_back({QLineF(drawn[i]), painter()->pen()});
        }
    }
    void drawTextItem(const QPointF &, const QTextItem &item) override {
        texts.push_back(item.text());
    }
    void drawPath(const QPainterPath &) override {}
    void drawPolygon(const QPointF *, int, PolygonDrawMode) override {}
    void drawPolygon(const QPoint *, int, PolygonDrawMode) override {}
    void drawRects(const QRectF *, int) override {}
    void drawRects(const QRect *, int) override {}
    void drawImage(const QRectF &, const QImage &, const QRectF &, Qt::ImageConversionFlags) override {}
    void drawPixmap(const QRectF &, const QPixmap &, const QRectF &) override {}
    Type type() const override { return QPaintEngine::User; }

    QVector<QPair<QLineF, QPen>> lines;
    QStringList texts;
};

class LineAndTextRecordingPaintDevice final : public QPaintDevice {
public:
    explicit LineAndTextRecordingPaintDevice(QSize size) : m_size(size) {}
    QPaintEngine *paintEngine() const override {
        return const_cast<LineAndTextRecordingPaintEngine *>(&engine);
    }

    mutable LineAndTextRecordingPaintEngine engine;

protected:
    int metric(PaintDeviceMetric metric) const override {
        switch (metric) {
        case PdmWidth:
        case PdmWidthMM:
            return m_size.width();
        case PdmHeight:
        case PdmHeightMM:
            return m_size.height();
        case PdmDpiX:
        case PdmDpiY:
        case PdmPhysicalDpiX:
        case PdmPhysicalDpiY:
            return 96;
        case PdmDevicePixelRatio:
            return 1;
        case PdmDevicePixelRatioScaled:
            return static_cast<int>(QPaintDevice::devicePixelRatioFScale());
        case PdmNumColors:
            return 16'777'216;
        case PdmDepth:
            return 32;
        default:
            return 0;
        }
    }

private:
    QSize m_size;
};

// The vertical lines a waveform paint draws in a pen of `style`.
QVector<double> verticalLinesIn(const LineAndTextRecordingPaintEngine &engine, Qt::PenStyle style,
                                const QColor &color = QColor()) {
    QVector<double> xs;
    for (const auto &[line, pen] : engine.lines) {
        if (line.dx() == 0.0 && line.dy() != 0.0 && pen.style() == style
            && (!color.isValid() || pen.color() == color)) {
            xs.push_back(line.x1());
        }
    }
    return xs;
}

// The bridge a waveform surface reads, with the real one's types: its mute
// mask has 64 bits.
class WaveformSurfaceTestBridge final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString playbackState MEMBER m_playbackState CONSTANT)
    Q_PROPERTY(QString currentTrackPath MEMBER m_currentTrackPath CONSTANT)
    Q_PROPERTY(QByteArray waveformPeaksPacked MEMBER m_waveformPeaksPacked CONSTANT)
    Q_PROPERTY(double waveformCoverageSeconds MEMBER m_waveformCoverageSeconds CONSTANT)
    Q_PROPERTY(bool waveformComplete MEMBER m_waveformComplete CONSTANT)
    Q_PROPERTY(double durationSeconds MEMBER m_durationSeconds CONSTANT)
    Q_PROPERTY(int spectrogramViewMode MEMBER m_spectrogramViewMode CONSTANT)
    Q_PROPERTY(bool spectrogramZoomEnabled MEMBER m_flag CONSTANT)
    Q_PROPERTY(bool showSpectrogramScale MEMBER m_unset CONSTANT)
    Q_PROPERTY(bool showSpectrogramCrosshair MEMBER m_unset CONSTANT)
    Q_PROPERTY(bool showFps MEMBER m_unset CONSTANT)
    Q_PROPERTY(int currentTrackChannels MEMBER m_currentTrackChannels NOTIFY trackMetadataChanged)
    Q_PROPERTY(QStringList currentTrackChannelLabels MEMBER m_currentTrackChannelLabels CONSTANT)
    Q_PROPERTY(quint64 mutedChannelsMask MEMBER m_mutedChannelsMask NOTIFY playbackChanged)
    Q_PROPERTY(QList<bool> mutedChannels READ mutedChannels NOTIFY playbackChanged)
    Q_PROPERTY(QList<bool> soloedChannels READ soloedChannels NOTIFY playbackChanged)
    Q_PROPERTY(int channelButtonsVisibility MEMBER m_channelButtonsVisibility CONSTANT)
    // What the plot layer reads: no meter, no cut, not connected.
    Q_PROPERTY(bool showLevelMeter MEMBER m_unset CONSTANT)
    Q_PROPERTY(bool waveformEndsEarly MEMBER m_unset CONSTANT)
    Q_PROPERTY(bool connected MEMBER m_unset CONSTANT)

public:
    QList<bool> mutedChannels() const {
        QList<bool> muted(64);
        for (int channel = 0; channel < 64; ++channel) {
            muted[channel] = (m_mutedChannelsMask & (1ULL << channel)) != 0;
        }
        return muted;
    }
    QList<bool> soloedChannels() const {
        QList<bool> soloed(64);
        for (int channel = 0; channel < 64; ++channel) {
            soloed[channel] = (m_soloedChannelsMask & (1ULL << channel)) != 0;
        }
        return soloed;
    }
    Q_INVOKABLE void toggleChannelMute(int) {}
    Q_INVOKABLE void soloChannel(int) {}
    void setChannels(int channels) {
        m_currentTrackChannels = channels;
        emit trackMetadataChanged();
    }
    void setMutes(quint64 mask, quint64 soloed) {
        m_mutedChannelsMask = mask;
        m_soloedChannelsMask = soloed;
        emit playbackChanged();
    }

signals:
    void trackMetadataChanged();
    void playbackChanged();

private:
    QString m_playbackState{QStringLiteral("Stopped")};
    QString m_currentTrackPath;
    QByteArray m_waveformPeaksPacked;
    double m_waveformCoverageSeconds{0.0};
    bool m_waveformComplete{false};
    double m_durationSeconds{10.0};
    int m_spectrogramViewMode{1};
    bool m_flag{true};
    bool m_unset{false};
    int m_currentTrackChannels{2};
    QStringList m_currentTrackChannelLabels;
    quint64 m_mutedChannelsMask{0};
    quint64 m_soloedChannelsMask{0};
    int m_channelButtonsVisibility{2};
};

void WaveformSmokeTest::waveformSurfaceFollowsSharedChannelAndOverlaySettings() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "components" as Components
import "viewers" as Viewers

Item {
    id: paletteRoot
    width: 640
    height: 320

    QtObject {
        id: bridge
        objectName: "waveformBridge"
        property string playbackState: "Stopped"
        property string currentTrackPath: ""
        property var waveformPeaksPacked: ""
        property real waveformCoverageSeconds: 2.5
        property bool waveformComplete: false
        property real durationSeconds: 10
        property int spectrogramViewMode: 1
        property bool spectrogramZoomEnabled: true
        property bool stretchShortTracks: true
        property bool showSpectrogramScale: true
        property bool showSpectrogramCrosshair: true
        property bool showFps: true
        property int currentTrackChannels: 2
        property var currentTrackChannelLabels: []
        property var mutedChannelsMask: 0
        property var soloedChannels: []
        property int channelButtonsVisibility: 2
        function isChannelSilent(channelIndex) { return false }
        function toggleChannelMute(channelIndex) {}
        function soloChannel(channelIndex) {}
    }

    Viewers.WaveformSurface {
        id: surface
        objectName: "waveformSurface"
        anchors.fill: parent
        uiBridge: bridge
        uiPalette: Components.UiPalette { windowRoot: paletteRoot }
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *surface = root->findChild<QObject *>(QStringLiteral("waveformSurface"));
    auto *waveform = root->findChild<WaveformEditorItem *>(
        QStringLiteral("waveformEditorItem"));
    QVERIFY(surface != nullptr);
    QVERIFY(waveform != nullptr);
    QTRY_COMPARE(waveform->overviewCoverageSeconds(), 2.5);
    QVERIFY(!waveform->overviewComplete());
    QTRY_COMPARE(waveform->viewMode(), 1);
    QTRY_COMPARE(waveform->channelCount(), 2);
    // The scale is the spectrogram's: none on the waveform.
    QVERIFY(!waveform->gridEnabled());
    // The waveform's plot set: on the view colour, in the seek bar's wave
    // colour, with the theme's seams; no green on black.
    QObject *plotColors = surface->property("plotColors").value<QObject *>();
    QObject *palette = surface->property("uiPalette").value<QObject *>();
    QVERIFY(plotColors != nullptr && palette != nullptr);
    QCOMPARE(waveform->backgroundColor(), plotColors->property("plotBg").value<QColor>());
    QCOMPARE(waveform->backgroundColor(), palette->property("view").value<QColor>());
    QCOMPARE(waveform->waveformColor(), palette->property("wave").value<QColor>());
    QCOMPARE(waveform->laneSeamColor(), plotColors->property("plotLaneSeam").value<QColor>());
    QCOMPARE(waveform->playheadColor().alpha(), 255);
    QTRY_VERIFY(waveform->crosshairEnabled());
    QTRY_VERIFY(waveform->showFpsOverlay());
    QVERIFY(!waveform->playing());

    // A zoomed view tells nothing of how much of the track it spans: no
    // plate covers the plot.
    QVERIFY(surface->property("interactiveOverlaysVisible").toBool());
    for (const double zoom : {2.0, 20.0}) {
        waveform->setZoomLevel(zoom);
        QTRY_COMPARE(waveform->zoomLevel(), zoom);
        QVERIFY(root->findChild<QObject *>(
            QStringLiteral("waveformViewportDurationIndicator")) == nullptr);
        QVERIFY(surface->findChildren<QObject *>(QStringLiteral("plotPlateFill")).isEmpty());
        QVERIFY(surface->findChildren<QObject *>(QStringLiteral("plotPlateLabel")).isEmpty());
    }
    waveform->resetZoom();

    QObject *hoverHandler = root->findChild<QObject *>(
        QStringLiteral("waveformSurfaceHoverHandler"));
    QVERIFY(hoverHandler != nullptr);
    QCOMPARE(hoverHandler->parent(), waveform);

    QObject *bridge = root->findChild<QObject *>(QStringLiteral("waveformBridge"));
    QVERIFY(bridge != nullptr);
    for (const int count : {1, 2, 3, 4, 5, 6, 7, 8, 10}) {
        bridge->setProperty("currentTrackChannels", count);
        QTRY_COMPARE(waveform->channelCount(), count);
        for (int i = 0; i < count; ++i) {
            QVariant label;
            QVERIFY(QMetaObject::invokeMethod(surface, "channelLabel",
                Q_RETURN_ARG(QVariant, label), Q_ARG(QVariant, i)));
            const QString expected = count == 1 ? QStringLiteral("M")
                : count == 2 ? (i == 0 ? QStringLiteral("L") : QStringLiteral("R"))
                : QString::number(i + 1);
            QCOMPARE(label.toString(), expected);
        }
    }
    bridge->setProperty("currentTrackChannels", 6);
    const QStringList positions{"L", "R", "C", "LFE", "SL", "SR"};
    bridge->setProperty("currentTrackChannelLabels", positions);
    for (int mode : {0, 1}) {
        bridge->setProperty("spectrogramViewMode", mode);
        const int visibleChannels = mode == 0 ? 1 : positions.size();
        QTRY_COMPARE(waveform->channelCount(), visibleChannels);
        for (int i = 0; i < visibleChannels; ++i) {
            QVariant label;
            QVERIFY(QMetaObject::invokeMethod(surface, "channelLabel",
                Q_RETURN_ARG(QVariant, label), Q_ARG(QVariant, i)));
            QCOMPARE(label.toString(), mode == 0 ? QStringLiteral("M") : positions[i]);
        }
    }
    bridge->setProperty("waveformCoverageSeconds", 5.0);
    QTRY_COMPARE(waveform->overviewCoverageSeconds(), 5.0);
    bridge->setProperty("waveformComplete", true);
    QTRY_VERIFY(waveform->overviewComplete());
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QTRY_VERIFY(waveform->playing());
    // The surface passes its clock to the waveform, which follows it.
    PlaybackClock clock;
    surface->setProperty("clock", QVariant::fromValue(static_cast<QObject *>(&clock)));
    QTRY_COMPARE(waveform->clock(), &clock);
    clock.holdAt(4.5);
    QCOMPARE(waveform->positionSeconds(), 4.5);

    surface->setProperty("interactiveOverlaysVisible", false);
    QTRY_VERIFY(!waveform->crosshairEnabled());
}

void WaveformSmokeTest::waveformSurfaceMuteButtonsFollowTheMutedChannels() {
    QQmlApplicationEngine engine;
    WaveformSurfaceTestBridge bridge;
    engine.rootContext()->setContextProperty(QStringLiteral("testBridge"), &bridge);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "components" as Components
import "viewers" as Viewers

Item {
    id: paletteRoot
    width: 640
    height: 320

    Components.UiPalette { id: palette; windowRoot: paletteRoot }

    Viewers.WaveformSurface {
        id: surface
        anchors.fill: parent
        plotWidth: layer.plotWidth
        uiBridge: testBridge
        uiPalette: palette
    }

    // The channel buttons are the plot layer's.
    Viewers.PlotLayer {
        id: layer
        anchors.fill: parent
        uiBridge: testBridge
        uiPalette: palette
        visualizationMode: 1
        laneCount: surface.laneCount
        laneLabels: surface.laneLabels
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *rootItem = qobject_cast<QQuickItem *>(root.data());
    QVERIFY(rootItem != nullptr);
    auto *waveform = root->findChild<WaveformEditorItem *>(QStringLiteral("waveformEditorItem"));
    QVERIFY(waveform != nullptr);
    // Whether a channel's button shows it active, or -1 without one.
    // A Repeater's delegates are only its parent's child items.
    const std::function<int(QQuickItem *, const QString &, int)> shownIn =
        [&shownIn](QQuickItem *item, const QString &button, int channel) {
            if (item->objectName() == QStringLiteral("plotLaneHead")
                && item->property("channelIndex").toInt() == channel) {
                QQuickItem *found = item->findChild<QQuickItem *>(button);
                return found != nullptr && found->property("checked").toBool() ? 1 : 0;
            }
            for (QQuickItem *child : item->childItems()) {
                const int shown = shownIn(child, button, channel);
                if (shown >= 0) return shown;
            }
            return -1;
        };
    const QString muteButton = QStringLiteral("plotLaneMuteButton");
    const QString soloButton = QStringLiteral("plotLaneSoloButton");

    for (const int channels : {2, 6}) {
        bridge.setChannels(channels);
        QTRY_COMPARE(waveform->channelCount(), channels);
        // Muting some channels, then soloing each one, then soloing two with
        // one of them muted too, and a mute above the 53 bits a QML number
        // keeps.
        QList<QPair<quint64, quint64>> states{{0b10, 0}, {0, 0}};
        for (int soloed = 0; soloed < channels; ++soloed) {
            states.push_back({0, quint64{1} << soloed});
        }
        states.push_back({0b10, 0b11});
        states.push_back({quint64{1} << 63, 0b01});
        for (const auto &[mask, soloed] : states) {
            bridge.setMutes(mask, soloed);
            for (int channel = 0; channel < channels; ++channel) {
                const bool muted = ((mask >> channel) & 1U) != 0;
                const bool leftOut = soloed != 0 && ((soloed >> channel) & 1U) == 0;
                QVERIFY2(shownIn(rootItem, muteButton, channel) == (muted ? 1 : 0),
                    qPrintable(QStringLiteral("%1 channels, mask %2: channel %3 shows %4")
                        .arg(channels).arg(mask, 16, 16, QLatin1Char('0')).arg(channel)
                        .arg(shownIn(rootItem, muteButton, channel))));
                QVERIFY2(shownIn(rootItem, soloButton, channel) == (((soloed >> channel) & 1U) != 0 ? 1 : 0),
                    qPrintable(QStringLiteral("%1 channels, solos %2: channel %3 shows %4")
                        .arg(channels).arg(soloed, 16, 16, QLatin1Char('0')).arg(channel)
                        .arg(shownIn(rootItem, soloButton, channel))));
                // The waveform also greys out what the solos leave out.
                QCOMPARE(waveform->channelDrawnMuted(channel), muted || leftOut);
            }
            QCOMPARE(waveform->mutedChannelsMask(), mask);
            QCOMPARE(waveform->soloedChannelsMask(), soloed);
        }
    }
}

void WaveformSmokeTest::levelMeterKeepsVisualizationHeightAndFollowsModes() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));
    engine.load(QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml")));
    QVERIFY(!engine.rootObjects().isEmpty());
    QObject *root = engine.rootObjects().constFirst();
    root->setProperty("width", 1600);
    root->setProperty("height", 1000);
    auto *meter = root->findChild<LevelMeterItem *>(QStringLiteral("levelMeter"));
    auto *surface = root->findChild<QQuickItem *>(QStringLiteral("visualizationSurface"));
    auto *layer = root->findChild<QQuickItem *>(QStringLiteral("plotLayer"));
    auto *spectrogram = root->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveform = root->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    auto *queue = root->findChild<QQuickItem *>(QStringLiteral("mainQueuePane"));
    auto *controller = root->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *bridge = qvariant_cast<QObject *>(root->property("uiBridge"));
    QVERIFY(meter && surface && layer && spectrogram && waveform && queue && controller && bridge);
    // Off by default; the plot is the whole pane, and the meter takes no
    // height in either state.
    QVERIFY(!bridge->property("showLevelMeter").toBool());
    QTRY_VERIFY(surface->height() > 20);
    QTRY_COMPARE(queue->height(), 580.0);
    QVERIFY(!meter->isVisible());
    QTRY_COMPARE(spectrogram->height(), surface->height());
    QCOMPARE(spectrogram->property("plotWidth").toReal(), surface->width());
    const double originalHeight = spectrogram->height();
    bridge->setProperty("showLevelMeter", true);
    QTRY_VERIFY(meter->isVisible());
    // A column at the right end of the lanes, as tall as the pane, out to
    // its edge, which is the window's (#373 H8: no strip of the plot's
    // floor beside it).
    const double column = LevelMeterItem::kColumnWidth;
    QCOMPARE(meter->width(), column);
    QCOMPARE(meter->height(), surface->height());
    QCOMPARE(meter->mapToItem(surface, QPointF()).x(), surface->width() - column);
    QCOMPARE(meter->mapToScene(QPointF(meter->width(), 0)).x(), double(meter->window()->width()));
    QCOMPARE(spectrogram->height(), originalHeight);
    QCOMPARE(spectrogram->property("plotWidth").toReal(), surface->width() - column);
    QCOMPARE(waveform->property("plotWidth").toReal(), surface->width() - column);
    QCOMPARE(queue->height(), 580.0);
    auto *originalParent = surface->parentItem();
    for (int mode : {0, 1}) {
        controller->setProperty("visualizationMode", mode);
        bridge->setProperty("spectrogramDisplayMode", mode);
        bridge->setProperty("spectrogramViewMode", mode);
        for (int channels : {1, 2, 6, 8}) {
            bridge->setProperty("currentTrackChannels", channels);
            QTRY_COMPARE(meter->channelCountHint(), channels);
            // The meter has a column for each lane the plot shows.
            QTRY_COMPARE(meter->laneCount(), layer->property("laneCount").toInt());
            QCOMPARE(meter->height(), surface->height());
            QCOMPARE(spectrogram->height(), originalHeight);
        }
    }
    bridge->setProperty("spectrogramViewMode", 1);
    controller->setProperty("visualizationMode", 0);
    bridge->setProperty("currentTrackChannels", 6);
    QVERIFY(QMetaObject::invokeMethod(bridge, "precomputedSpectrogramChannelsReady",
        Q_ARG(int, 6), Q_ARG(bool, true)));
    QTRY_COMPARE(meter->laneCount(), 6);
    const auto labels = [&]() { return layer->property("laneLabels").toStringList(); };
    QTRY_COMPARE(labels().value(0), QStringLiteral("1"));
    QPointer<QQuickItem> originalPane = findQuickItemByObjectName(
        spectrogram, QStringLiteral("spectrogramPaneItem"));
    // Late layout metadata relabels the lanes without replacing them.
    const QStringList positions{"C", "L", "R", "SL", "SR", "LFE"};
    bridge->setProperty("currentTrackChannelLabels", positions);
    QTRY_COMPARE(labels(), positions);
    QVERIFY(originalPane);
    QCOMPARE(findQuickItemByObjectName(spectrogram, QStringLiteral("spectrogramPaneItem")), originalPane.data());
    bridge->setProperty("currentTrackChannelLabels", QStringList{});
    QTRY_COMPARE(labels().value(0), QStringLiteral("1"));
    // Every consumer reads the bridge's one clock.
    PlaybackClock clock;
    bridge->setProperty("playbackClock", QVariant::fromValue(static_cast<QObject *>(&clock)));
    QTRY_COMPARE(meter->clock(), &clock);
    QList<SpectrogramItem *> spectrograms;
    collectQuickItems(spectrogram, &spectrograms);
    QVERIFY(!spectrograms.isEmpty());
    for (SpectrogramItem *item : spectrograms) {
        QCOMPARE(item->clock(), &clock);
    }
    auto *waveformEditor = root->findChild<WaveformEditorItem *>(QStringLiteral("waveformEditorItem"));
    QVERIFY(waveformEditor);
    QCOMPARE(waveformEditor->clock(), &clock);
    auto *transportWaveform = root->findChild<WaveformItem *>();
    QVERIFY(transportWaveform);
    clock.holdAt(42.0);
    QTRY_COMPARE(transportWaveform->positionSeconds(), 42.0);
    for (SpectrogramItem *item : spectrograms) {
        QCOMPARE(item->positionSeconds(), 42.0);
    }
    // A jump of the clock clears the meter's levels.
    meter->m_channels[0].observe(-3, 0);
    clock.holdAt(0.0);
    QCOMPARE(meter->m_channels[0].peak, -60.0);
    // Stopped, the column is an empty trough, and reads nothing.
    QVERIFY(!meter->active());
    QVERIFY(meter->sourcePath().isEmpty());
    // The preference pages are made when the dialog opens.
    QVERIFY(QMetaObject::invokeMethod(root->findChild<QObject *>(QStringLiteral("preferencesDialog")), "open"));
    QObject *checkBox = nullptr;
    QTRY_VERIFY((checkBox = root->findChild<QObject *>(QStringLiteral("showLevelMeterCheckBox"))) != nullptr);
    QVERIFY(checkBox->property("checked").toBool());
    bridge->setProperty("connected", true);
    bridge->setProperty("playbackState", QStringLiteral("Paused"));
    bridge->setProperty("currentTrackPath", QStringLiteral("/fixture/first.wav"));
    QTRY_COMPARE(meter->sourcePath(), QStringLiteral("/fixture/first.wav"));
    // Paused, it keeps its file and shows an empty trough.
    QVERIFY(!meter->active());
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    meter->m_cancelled = cancelled;
    meter->m_channels[0].observe(-3, 0);
    checkBox->setProperty("checked", false);
    QVERIFY(QMetaObject::invokeMethod(checkBox, "toggled"));
    // The change waits for OK.
    QVERIFY(bridge->property("showLevelMeter").toBool());
    QVERIFY(QMetaObject::invokeMethod(root->findChild<QObject *>(QStringLiteral("preferencesDialog")), "accept"));
    QTRY_VERIFY(!bridge->property("showLevelMeter").toBool());
    QTRY_VERIFY(!meter->isVisible());
    QTRY_COMPARE(spectrogram->property("plotWidth").toReal(), surface->width());
    QCOMPARE(queue->height(), 580.0);
    QCOMPARE(spectrogram->height(), originalHeight);
    QCOMPARE(waveform->height(), originalHeight);
    QVERIFY(cancelled->load());
    QVERIFY(meter->sourcePath().isEmpty());
    QCOMPARE(meter->m_channels[0].peak, -60.0);
    bridge->setProperty("currentTrackPath", QStringLiteral("/fixture/next.wav"));
    QVERIFY(meter->sourcePath().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(root->findChild<QObject *>(QStringLiteral("preferencesDialog")), "open"));
    QTRY_VERIFY(root->findChild<QObject *>(QStringLiteral("preferencesDialog"))->property("opened").toBool());
    checkBox->setProperty("checked", true);
    QVERIFY(QMetaObject::invokeMethod(checkBox, "toggled"));
    QVERIFY(QMetaObject::invokeMethod(root->findChild<QObject *>(QStringLiteral("preferencesDialog")), "accept"));
    QTRY_VERIFY(meter->isVisible());
    QTRY_COMPARE(meter->sourcePath(), QStringLiteral("/fixture/next.wav"));
    QCOMPARE(queue->height(), 580.0);
    // The viewers show the same pane, with its meter when it is on.
    for (int mode : {0, 1}) {
        controller->setProperty("visualizationMode", mode);
        bridge->setProperty("spectrogramDisplayMode", mode);
        for (int fullscreen : {0, 1}) {
            bridge->setProperty("viewerFullscreenMode", fullscreen);
            controller->setProperty("spectrogramViewerOpen", true);
            QTRY_VERIFY(surface->parentItem() != originalParent);
            for (bool enabled : {false, true}) {
                bridge->setProperty("showLevelMeter", enabled);
                QTRY_COMPARE(meter->isVisible(), enabled);
                QTRY_COMPARE(meter->height(), surface->height());
                QTRY_COMPARE(spectrogram->height(), surface->height());
                QTRY_COMPARE(spectrogram->property("plotWidth").toReal(),
                             surface->width() - (enabled ? column : 0.0));
                QCOMPARE(meter->sourcePath().isEmpty(), !enabled);
                if (enabled) {
                    // Out to the viewer's edge, with no gap.
                    QTRY_COMPARE(meter->mapToScene(QPointF(meter->width(), 0)).x(),
                                 double(meter->window()->width()));
                }
            }
            controller->setProperty("spectrogramViewerOpen", false);
            QTRY_COMPARE(surface->parentItem(), originalParent);
            QTRY_COMPARE(spectrogram->height(), originalHeight);
            QTRY_VERIFY(meter->isVisible());
        }
    }
}

void WaveformSmokeTest::waveformProgressInvalidatesOnlyTailSpan() {
    WaveformItem item;
    item.setWidth(200);
    item.setHeight(24);
    item.setDurationSeconds(10.0);

    QByteArray peaks(100, '\x33');
    item.setPeaksData(peaks);
    item.setGeneratedSeconds(5.0);

    QImage canvas(200, 24, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QVERIFY(!item.m_cacheDirty);
    QVERIFY(item.m_dirtyRect.isNull());

    item.setGeneratedSeconds(7.0);

    QCOMPARE(item.m_dirtyRect, QRect(100, 0, 40, 24));
    QVERIFY(item.m_cacheDirty);
}

void WaveformSmokeTest::waveformPeakUpdatesInvalidateChangedSuffix() {
    WaveformItem item;
    item.setWidth(200);
    item.setHeight(24);
    item.setDurationSeconds(10.0);
    item.setWaveformComplete(true);

    QByteArray peaks(100, '\x22');
    item.setPeaksData(peaks);

    QImage canvas(200, 24, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QVERIFY(!item.m_cacheDirty);
    QVERIFY(item.m_dirtyRect.isNull());

    QByteArray updated = peaks;
    for (int i = 80; i < updated.size(); ++i) {
        updated[i] = '\x66';
    }
    item.setPeaksData(updated);

    QVERIFY(item.m_cacheDirty);
    QVERIFY(item.m_dirtyRect.x() >= 160);
    QCOMPARE(item.m_dirtyRect.height(), 24);
}

void WaveformSmokeTest::waveformEditorCancelsAndBoundsObsoleteDecodes() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setDurationSeconds(10.0);
    auto gate = std::make_shared<QSemaphore>();
    auto started = std::make_shared<std::atomic_int>(0);
    item.m_decodeWindow = [gate, started](const QString &, double, double, int,
                                        const std::shared_ptr<std::atomic_bool> &) {
        ++*started;
        gate->tryAcquire(1, 5000);
        return QByteArray("stale invalid payload");
    };
    item.setSourcePath(QStringLiteral("/fixture/first.wav"));
    item.m_requestTimer.stop();
    item.requestDetailWindow();
    QTRY_COMPARE(started->load(), 1);
    const auto cancelled = item.m_decodeCancelled;
    for (int i = 0; i < 20; ++i) {
        item.setZoomLevel(2.0 + i);
        item.m_requestTimer.stop();
        item.requestDetailWindow();
    }
    QVERIFY(cancelled->load());
    QCOMPARE(started->load(), 1);
    QCOMPARE(item.findChildren<QFutureWatcherBase *>().size(), 1);
    item.setSourcePath(QStringLiteral("/fixture/latest.wav"));
    item.m_requestTimer.stop();
    gate->release();
    QTRY_COMPARE(started->load(), 2);
    item.setSourcePath(QString());
    gate->release();
    QTRY_VERIFY(!item.m_decodeActive);
    QVERIFY(item.m_detail.extrema.empty());
    QCOMPARE(started->load(), 2);
}

void WaveformSmokeTest::waveformEditorDetailArrivesWhileScrolling() {
    // A detail window for [start, end] at 48 kHz, as the decoder sends one.
    const auto window = [](double start, double end, int maxPoints) {
        const quint32 frames = static_cast<quint32>(std::ceil((end - start) * 48'000.0));
        const quint32 framesPerPoint = std::max<quint32>(
            1, (frames + static_cast<quint32>(maxPoints) - 1) / static_cast<quint32>(maxPoints));
        const quint32 points = (frames + framesPerPoint - 1) / framesPerPoint;
        QByteArray bytes("WVF2", 4);
        appendLe<quint32>(bytes, 48'000);
        appendLe<quint16>(bytes, 1);
        appendLe<quint16>(bytes, 0);
        for (const double seconds : {start, end}) {
            quint64 bits = 0;
            std::memcpy(&bits, &seconds, sizeof(bits));
            appendLe<quint64>(bytes, bits);
        }
        appendLe<quint32>(bytes, framesPerPoint);
        appendLe<quint32>(bytes, points);
        bytes.append(QByteArray(static_cast<qsizetype>(points) * 4 * 4, '\0'));
        return bytes;
    };
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(120);
    item.setDurationSeconds(60.0);
    item.setZoomLevel(1'200.0);
    // Frames ask for detail as the view moves; nothing else does.
    item.m_requestTimer.blockSignals(true);
    auto gate = std::make_shared<QSemaphore>();
    item.m_decodeWindow = [gate, window](const QString &, double start, double end, int points,
                                         const std::shared_ptr<std::atomic_bool> &) {
        gate->tryAcquire(1, 5000);
        return window(start, end, points);
    };
    item.setSourcePath(QStringLiteral("/fixture/deep-zoom.wav"));

    // Each frame, playback moves on by more than a pending request's margin
    // to spare (0.3 s here), and a decode takes two frames: the view
    // outruns every request before it arrives, but not its range (0.65 s
    // ahead of the view, and more before the sample rate is known).
    constexpr double kStepSeconds = 0.32;
    constexpr int kDecodeFrames = 2;
    int decodeAge = 0;
    bool detailShown = false;
    for (int frame = 0; frame < 24 && !detailShown; ++frame) {
        if (item.m_decodeActive && ++decodeAge >= kDecodeFrames) {
            gate->release();
            QTRY_VERIFY(!item.m_decodeActive);
            decodeAge = 0;
        }
        bool request = false;
        {
            QMutexLocker lock(&item.m_stateMutex);
            item.m_clockState.anchorSeconds = 20.0 + frame * kStepSeconds;
            const auto [start, end] = item.visibleRangeLocked();
            request = !item.detailOrPendingRequestCoversLocked(start, end);
            detailShown = item.detailCoversRangeLocked(start, end)
                && item.detailResolutionCoversLocked(start, end);
        }
        if (request) item.requestDetailWindow();
    }
    QVERIFY2(detailShown, "the view never had its detail");
    item.setSourcePath(QString());
    gate->release(4);
    QTRY_VERIFY(!item.m_decodeActive);
}

void WaveformSmokeTest::waveformEditorSignalsWithoutItsStateLocked() {
    WaveformEditorItem item;
    StateMutexProbe probe(&item, &item.m_stateMutex);
    item.m_decodeWindow = [](const QString &, double start, double end, int,
                             const std::shared_ptr<std::atomic_bool> &) {
        QByteArray bytes("WVF2", 4);
        appendLe<quint32>(bytes, 48'000);
        appendLe<quint16>(bytes, 2);
        appendLe<quint16>(bytes, 0);
        for (const double seconds : {start, end}) {
            quint64 bits = 0;
            std::memcpy(&bits, &seconds, sizeof(bits));
            appendLe<quint64>(bytes, bits);
        }
        appendLe<quint32>(bytes, 4'096);
        appendLe<quint32>(bytes, 64);
        bytes.append(QByteArray(64 * 6 * 4, '\0'));
        return bytes;
    };
    item.setWidth(320);
    item.setHeight(120);
    item.setDurationSeconds(30.0);
    item.setOverviewData(QByteArray(512, '\x40'));
    item.setOverviewCoverageSeconds(30.0);
    item.setOverviewComplete(true);
    item.setZoomEnabled(false);
    item.setZoomEnabled(true);
    item.setGridEnabled(true);
    item.setCrosshairEnabled(true);
    item.setShowFpsOverlay(true);
    item.setViewMode(1);
    item.setChannelCountHint(2);
    item.setMutedChannelsMask(1);
    item.setSoloedChannelsMask(0b10);
    item.setPlaying(true);
    setTestPosition(item, 4.0);
    // A detail window arrives with the channels and the sample rate.
    item.setSourcePath(QStringLiteral("/fixture/signals.wav"));
    QTRY_COMPARE(item.channelCount(), 2);
    item.setZoomLevel(4.0);
    QHoverEvent hover(QEvent::HoverMove, QPointF(40.0, 30.0), QPointF(40.0, 30.0), QPointF());
    item.hoverMoveEvent(&hover);
    QMouseEvent seek(QEvent::MouseButtonPress, QPointF(40.0, 30.0), QPointF(40.0, 30.0),
        Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    item.mousePressEvent(&seek);
    QWheelEvent wheel(QPointF(40.0, 30.0), QPointF(40.0, 30.0), QPoint(), QPoint(0, 120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    item.wheelEvent(&wheel);
    QMouseEvent reset(QEvent::MouseButtonPress, QPointF(40.0, 30.0), QPointF(40.0, 30.0),
        Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    item.mousePressEvent(&reset);
    item.setPlaying(false);
    item.setSourcePath(QString());
    QTRY_VERIFY(!item.m_decodeActive);
    QVERIFY(probe.deliveries >= 20);
    QVERIFY2(probe.heldSignals.isEmpty(), qPrintable(probe.heldSignals.join(QLatin1String(", "))));
}

void WaveformSmokeTest::waveformEditorSeeksWhereTheCrosshairShows_data() {
    QTest::addColumn<double>("scale");
    QTest::addColumn<double>("zoom");
    QTest::addColumn<double>("position");
    for (const double scale : {1.0, 1.5, 2.0}) {
        // The whole track; zoomed in at its start, where the view stays
        // still; and zoomed in, following the playhead.
        QTest::addRow("%gx, whole track", scale) << scale << 1.0 << 5.0;
        QTest::addRow("%gx, zoomed at the start", scale) << scale << 8.0 << 0.2;
        QTest::addRow("%gx, zoomed and following", scale) << scale << 8.0 << 5.0;
    }
}

void WaveformSmokeTest::waveformEditorSeeksWhereTheCrosshairShows() {
    QFETCH(double, scale);
    QFETCH(double, zoom);
    QFETCH(double, position);
    QQuickWindow window;
    if (scale != 1.0 && !showOnScreen(window, scale)) {
        QSKIP("Needs the offscreen platform's HiDPI test screens");
    }
    auto *item = new WaveformEditorItem(scale != 1.0 ? window.contentItem() : nullptr);
    std::unique_ptr<WaveformEditorItem> unparented(scale != 1.0 ? nullptr : item);
    // The pointer is the test's, not the window's.
    item->setAcceptHoverEvents(false);
    QCOMPARE(item->renderScaleLocked(), scale);
    item->setWidth(320);
    item->setHeight(120);
    item->setDurationSeconds(10.0);
    item->setCrosshairEnabled(true);
    item->setGridEnabled(true);
    item->setZoomLevel(zoom);
    setTestPosition(*item, position);
    const auto record = [item]() {
        auto device = std::make_unique<LineAndTextRecordingPaintDevice>(QSize(320, 120));
        QPainter painter(device.get());
        item->paint(&painter);
        painter.end();
        return device;
    };

    for (const double pointerX : {0.2, 137.7, 211.33, 319.9}) {
        item->setHoverPosition(pointerX, 50.0, true);
        const auto drawn = record();
        // The crosshair is on the device column under the pointer, at its
        // left edge.
        const QVector<double> crosshair = verticalLinesIn(drawn->engine, Qt::DotLine);
        QCOMPARE(crosshair.size(), 1);
        const double columnX = std::floor(pointerX * scale + 1e-6) / scale;
        QVERIFY(std::abs(crosshair.constFirst() - columnX) < 1e-9);
        static const QRegularExpression timeLabel(QStringLiteral("^\\d+:\\d\\d\\.\\d{3}$"));
        const QStringList shown = drawn->engine.texts.filter(timeLabel);
        QCOMPARE(shown.size(), 1);

        // A click there seeks to the time it shows.
        QSignalSpy seekSpy(item, &WaveformEditorItem::seekRequested);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(pointerX, 50.0), QPointF(pointerX, 50.0),
            Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        item->mousePressEvent(&press);
        QCOMPARE(seekSpy.count(), 1);
        const double seconds = seekSpy.at(0).at(0).toDouble();
        QCOMPARE(WaveformEditorItem::formatCrosshairTime(seconds), shown.constFirst());
        double start = 0.0;
        double end = 0.0;
        {
            QMutexLocker lock(&item->m_stateMutex);
            std::tie(start, end) = item->visibleRangeLocked();
        }
        QVERIFY(std::abs(seconds - (start + columnX / 320.0 * (end - start))) < 1e-9);

        // Unless the view moves to follow it, the playhead goes there too.
        testClock(*item).holdAt(seconds);
        double heldStart = 0.0;
        double heldEnd = 0.0;
        {
            QMutexLocker lock(&item->m_stateMutex);
            std::tie(heldStart, heldEnd) = item->visibleRangeLocked();
        }
        if (heldStart == start && heldEnd == end) {
            const QVector<double> playhead =
                verticalLinesIn(record()->engine, Qt::SolidLine, item->playheadColor());
            QCOMPARE(playhead.size(), 1);
            QVERIFY2(std::abs(playhead.constFirst() - columnX) < 1e-9,
                qPrintable(QStringLiteral("playhead at %1, crosshair at %2")
                    .arg(playhead.constFirst()).arg(columnX)));
        } else {
            // Only a zoomed view follows, once the playhead is past its
            // middle.
            QVERIFY(zoom > 1.0 && seconds > (end - start) * 0.5);
        }
        setTestPosition(*item, position);
    }

    // The playhead at a grid line's time is on that line.
    if (zoom == 1.0) {
        testClock(*item).holdAt(5.0);
        const auto drawn = record();
        const QVector<double> playhead = verticalLinesIn(drawn->engine, Qt::SolidLine, item->playheadColor());
        QCOMPARE(playhead.size(), 1);
        QVERIFY(verticalLinesIn(drawn->engine, Qt::SolidLine, QColor(20, 82, 49))
            .contains(playhead.constFirst()));
    }
}

void WaveformSmokeTest::waveformEditorCachedDecoderPreservesOverlappingSamples() {
    QByteArray wav("RIFF", 4);
    constexpr quint32 frames = 30'000;
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
        const qint16 sample = static_cast<qint16>((frame % 16) * 1024);
        appendLe<qint16>(wav, sample);
        appendLe<qint16>(wav, -sample);
    }
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write(wav), wav.size());
    QVERIFY(file.flush());
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    WaveformEditorItem::DetailWindow full;
    QVERIFY(WaveformEditorItem::parseWindow(WaveformEditorItem::decodeWindow(
        file.fileName(), 0.0, 0.6, 30'000, cancelled), &full));
    for (int request = 0; request < 2; ++request) {
        WaveformEditorItem item;
        QVERIFY(WaveformEditorItem::parseWindow(WaveformEditorItem::decodeWindow(
            file.fileName(), 0.25, 0.5, 12'000, cancelled), &item.m_detail));
        QCOMPARE(item.m_detail.framesPerPoint, quint32(1));
        QCOMPARE(item.m_detail.channelCount, 2);
        for (size_t index = 0; index < item.m_detail.extrema.size(); ++index) {
            QCOMPARE(item.m_detail.extrema[index], full.extrema[12'000 * 4 + index]);
        }
        for (float mixed : item.m_detail.downmixExtrema) QCOMPARE(mixed, 0.0F);
    }
    for (int width : {128, 400, 3'440}) {
        for (double zoom : {1.0, 2.0, 8.0}) {
            WaveformEditorItem item;
            item.setWidth(width);
            item.setDurationSeconds(static_cast<double>(frames) / 48'000.0);
            setTestPosition(item, 0.3);
            item.m_sampleRateHz = 48'000;
            item.setZoomLevel(zoom);
            const auto [start, end] = item.visibleRangeLocked();
            const auto [requestStart, requestEnd] = item.requestRangeLocked(start, end);
            const int points = item.detailRequestPointCountLocked(requestStart, requestEnd);
            QVERIFY(WaveformEditorItem::parseWindow(WaveformEditorItem::decodeWindow(
                file.fileName(), requestStart, requestEnd, points, cancelled), &item.m_detail));
            const quint32 bin = item.m_detail.framesPerPoint;
            QVERIFY(bin != 0 && (bin & (bin - 1)) == 0);
            QVERIFY(item.m_detail.pointCount <= points);
            QVERIFY(item.detailResolutionCoversLocked(start, end));
        }
    }
    cancelled->store(true);
    QVERIFY(WaveformEditorItem::decodeWindow(
        file.fileName(), 0.25, 0.5, 12'000, cancelled).isEmpty());
}

void WaveformSmokeTest::waveformEditorParsesSignedSampleWindow() {
    const auto appendF64 = [](QByteArray &out, double value) {
        quint64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendLe<quint64>(out, bits);
    };
    const auto appendF32 = [](QByteArray &out, float value) {
        quint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendLe<quint32>(out, bits);
    };
    QByteArray frame;
    frame.append("WVF2", 4);
    appendLe<quint32>(frame, 48'000);
    appendLe<quint16>(frame, 2);
    appendLe<quint16>(frame, 0);
    appendF64(frame, 1.0);
    appendF64(frame, 1.000'041'666'666'666'7);
    appendLe<quint32>(frame, 1);
    appendLe<quint32>(frame, 2);
    appendF32(frame, -0.75F);
    appendF32(frame, -0.75F);
    appendF32(frame, 0.25F);
    appendF32(frame, 0.25F);
    appendF32(frame, 0.5F);
    appendF32(frame, 0.5F);
    appendF32(frame, -0.125F);
    appendF32(frame, -0.125F);
    appendF32(frame, -0.25F);
    appendF32(frame, -0.25F);
    appendF32(frame, 0.1875F);
    appendF32(frame, 0.1875F);

    WaveformEditorItem::DetailWindow window;
    QVERIFY(WaveformEditorItem::parseWindow(frame, &window));
    QCOMPARE(window.sampleRateHz, 48'000);
    QCOMPARE(window.channelCount, 2);
    QCOMPARE(window.framesPerPoint, quint32(1));
    QCOMPARE(window.pointCount, 2);
    QCOMPARE(window.extrema.at(0), -0.75F);
    QCOMPARE(window.extrema.at(6), -0.125F);
    QCOMPARE(window.downmixExtrema.at(0), -0.25F);
    QCOMPARE(window.downmixExtrema.at(2), 0.1875F);
    QVERIFY(!WaveformEditorItem::parseWindow(frame.chopped(4), &window));
}

void WaveformSmokeTest::waveformEditorDownmixUsesMixedExtremaAtEveryZoom() {
    WaveformEditorItem item;
    item.m_detail.channelCount = 2;
    item.m_detail.pointCount = 4;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 1.0;
    for (int i = 0; i < 4; ++i) {
        item.m_detail.extrema.insert(item.m_detail.extrema.end(), {0.5F, 0.5F, -0.5F, -0.5F});
    }
    item.m_detail.downmixExtrema.assign(8, 0.0F);
    for (int framesPerPoint : {1, 2}) {
        item.m_detail.framesPerPoint = framesPerPoint;
        item.m_detail.sampleRateHz = 4 * framesPerPoint;
        QImage image(400, 100, QImage::Format_RGB32);
        image.fill(Qt::black);
        QPainter painter(&image);
        item.drawDetailLocked(painter, 400, 100, 0.0, 1.0, 1);
        painter.end();
        int signalPixels = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y).green() > 150) {
                    ++signalPixels;
                    QVERIFY(y >= 47 && y <= 52);
                }
            }
        }
        QVERIFY(signalPixels > 0);
    }
}

void WaveformSmokeTest::waveformEditorSampleMarkersRequireSampleResolution() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(1.0);
    item.m_sampleRateHz = 100;
    item.m_detail.sampleRateHz = 100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 1.0;
    item.m_detail.pointCount = 100;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.extrema.assign(200, 0.0F);
    item.setZoomLevel(1.0);
    QVERIFY(item.samplePointsVisible());

    item.m_detail.framesPerPoint = 2;
    QVERIFY(!item.samplePointsVisible());
}

void WaveformSmokeTest::waveformEditorConnectsSamplesBeforeMarkers() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(1.0);
    setTestPosition(item, 0.5);
    item.m_sampleRateHz = 1'000;
    item.m_detail.sampleRateHz = 1'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 1.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 1'000;
    item.m_detail.extrema.reserve(2'000);
    for (int sample = 0; sample < 1'000; ++sample) {
        const float value = static_cast<float>(
            std::sin(static_cast<double>(sample) * 0.075));
        item.m_detail.extrema.push_back(value);
        item.m_detail.extrema.push_back(value);
    }
    item.setZoomLevel(3.0);

    QVERIFY(item.sampleCurveVisibleLocked());
    QVERIFY(!item.samplePointsVisibleLocked());
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    QVERIFY(!item.renderDetailDirectlyLocked(visibleStart, visibleEnd));

    QImage canvas(400, 180, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    item.drawDetailLocked(
        painter, canvas.width(), canvas.height(),
        visibleStart, visibleEnd, 1);
    painter.end();

    int occupiedColumns = 0;
    for (int x = 0; x < canvas.width(); ++x) {
        bool occupied = false;
        for (int y = 0; y < canvas.height(); ++y) {
            const QColor pixel = canvas.pixelColor(x, y);
            if (pixel.green() > 100 && pixel.green() > pixel.red() * 2) {
                occupied = true;
                break;
            }
        }
        if (occupied) ++occupiedColumns;
    }
    QVERIFY(occupiedColumns > 390);

    constexpr double pixelsPerSample = 1.2;
    const double tileDuration = 64.0
        / (pixelsPerSample * item.m_detail.sampleRateHz);
    const double tileStart = tileDuration;
    QImage tile(64, 180, QImage::Format_RGB32);
    tile.fill(Qt::black);
    QPainter tilePainter(&tile);
    item.drawDetailSliceLocked(
        tilePainter, tile.width(), tile.height(),
        tileStart, tileStart + tileDuration, 1, 0, tile.width());
    tilePainter.end();
    for (const int edgeX : {0, tile.width() - 1}) {
        bool edgeOccupied = false;
        for (int y = 0; y < tile.height(); ++y) {
            const QColor pixel = tile.pixelColor(edgeX, y);
            if (pixel.green() > 100 && pixel.green() > pixel.red() * 2) {
                edgeOccupied = true;
                break;
            }
        }
        QVERIFY2(edgeOccupied, "connected sample tiles must meet at both edges");
    }

    // Samples without markers, about 1.9 pixels apart, are drawn into the
    // cache and playback tiles.
    item.setZoomLevel(4.8);
    QVERIFY(!item.samplePointsVisibleLocked());
    const auto [curveStart, curveEnd] = item.visibleRangeLocked();
    QVERIFY(!item.renderDetailDirectlyLocked(curveStart, curveEnd));

    // Samples with markers, from 2 pixels apart, are drawn directly:
    // playing, such a view moves on by most of its width a frame, and
    // rasterizing its markers again each frame costs more than geometry.
    for (const double zoom : {5.2, 9.0, 10.0}) {
        item.setZoomLevel(zoom);
        QVERIFY(item.samplePointsVisibleLocked());
        const auto [markerStart, markerEnd] = item.visibleRangeLocked();
        QVERIFY(item.renderDetailDirectlyLocked(markerStart, markerEnd));
    }
}

void WaveformSmokeTest::waveformEditorScrollsInsideCachedDetailWithoutRebuild() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.m_sampleRateHz = 100;
    item.m_detail.sampleRateHz = 100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.pointCount = 1'000;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.extrema.assign(2'000, 0.0F);
    setTestPosition(item, 5.0);
    item.setZoomLevel(2.0);

    QImage canvas(400, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();
    QVERIFY(!item.m_cacheDirty);
    QCOMPARE(item.m_cacheStartSeconds, 0.0);
    QCOMPARE(item.m_cacheEndSeconds, 10.0);

    setTestPosition(item, 5.1);
    QVERIFY(!item.m_cacheDirty);
}

void WaveformSmokeTest::waveformEditorUsesChannelHintBeforeDetailArrives() {
    WaveformEditorItem item;
    item.setViewMode(1);
    item.setChannelCountHint(2);

    QCOMPARE(item.channelCount(), 2);

    item.m_channelCount = 6;
    QCOMPARE(item.channelCount(), 6);
}

void WaveformSmokeTest::waveformEditorCoalescesPlaybackRequestsWithoutStarvingTimer() {
    WaveformEditorItem item;
    item.scheduleDetailRequest();
    QVERIFY(item.m_requestTimer.isActive());
    const int initialRemaining = item.m_requestTimer.remainingTime();

    // Time passes without the event loop, so the timer cannot fire meanwhile.
    QTest::qSleep(25);
    item.scheduleDetailRequest();

    QVERIFY(item.m_requestTimer.isActive());
    QVERIFY(item.m_requestTimer.remainingTime() < initialRemaining - 10);
}

void WaveformSmokeTest::waveformEditorPendingWindowSuppressesDuplicateRequest() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setDurationSeconds(10.0);
    item.m_requestTimer.stop();
    item.m_requestInFlight = true;
    item.m_requestedStartSeconds = 0.0;
    item.m_requestedEndSeconds = 10.0;
    item.m_requestedMaxPoints = 1'200;
    item.m_requestedRenderWidth = 400;

    setTestPosition(item, 2.0);

    QVERIFY(!item.m_requestTimer.isActive());
}

void WaveformSmokeTest::waveformEditorZoomSupersedesCoarsePendingRequest() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setDurationSeconds(10.0);
    item.m_requestTimer.stop();
    item.m_requestInFlight = true;
    item.m_requestedStartSeconds = 0.0;
    item.m_requestedEndSeconds = 10.0;
    item.m_requestedMaxPoints = 1'200;
    item.m_requestedRenderWidth = 400;

    item.setZoomLevel(4.0);

    QVERIFY(!item.m_requestInFlight);
    QVERIFY(item.m_requestTimer.isActive());
    QCOMPARE(item.zoomLevel(), 4.0);
}

void WaveformSmokeTest::waveformEditorResizeRaisesDetailRequestDensity() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setDurationSeconds(100.0);
    setTestPosition(item, 50.0);
    item.setZoomLevel(4.0);
    const auto [smallStart, smallEnd] = item.requestRangeLocked(37.5, 62.5);
    const int smallRequest = item.detailRequestPointCountLocked(smallStart, smallEnd);
    item.m_detail.startSeconds = smallStart;
    item.m_detail.endSeconds = smallEnd;
    item.m_detail.framesPerPoint = 2;
    item.m_detail.pointCount = smallRequest;
    QVERIFY(item.detailResolutionCoversLocked(37.5, 62.5));

    item.setWidth(1'200);
    const auto [largeStart, largeEnd] = item.requestRangeLocked(37.5, 62.5);
    const int largeRequest = item.detailRequestPointCountLocked(largeStart, largeEnd);

    QVERIFY(largeRequest >= smallRequest * 3);
    QVERIFY(!item.detailResolutionCoversLocked(37.5, 62.5));
}

void WaveformSmokeTest::waveformEditorDeepZoomRequestPreservesViewportDensity() {
    WaveformEditorItem item;
    item.setWidth(3'440);
    item.setDurationSeconds(240.0);
    setTestPosition(item, 120.0);
    item.setZoomLevel(8'301.0);
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    const auto [requestStart, requestEnd] = item.requestRangeLocked(
        visibleStart, visibleEnd);
    const double visibleSpan = visibleEnd - visibleStart;
    const double requestSpan = requestEnd - requestStart;
    const double maximumDensitySpan = visibleSpan * 65'536.0
        / (3'440.0 * 4.0);

    QVERIFY(requestSpan <= maximumDensitySpan + 0.000'001);
    QVERIFY(requestSpan > visibleSpan);
}

void WaveformSmokeTest::waveformEditorQuantizesRequestForRequiredBinDensity() {
    WaveformEditorItem item;
    item.setWidth(1'183);
    item.setDurationSeconds(257.133333);
    setTestPosition(item, 46.3);
    item.m_sampleRateHz = 44'100;
    item.setZoomLevel(1'726.76);
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    const auto [requestStart, requestEnd] = item.requestRangeLocked(
        visibleStart, visibleEnd);
    const int requestedPoints = item.detailRequestPointCountLocked(
        requestStart, requestEnd);
    const double requestFrames = std::ceil(
        (requestEnd - requestStart) * 44'100.0);
    const double framesPerPoint = std::ceil(
        requestFrames / static_cast<double>(requestedPoints));
    const double returnedPoints = std::ceil(requestFrames / framesPerPoint);
    const double visiblePoints = returnedPoints
        * (visibleEnd - visibleStart) / (requestEnd - requestStart);

    QVERIFY(requestedPoints > 20'621);
    QVERIFY(visiblePoints >= item.requiredVisibleDetailPointsLocked(
        visibleEnd - visibleStart) * 0.9);
}

void WaveformSmokeTest::waveformEditorFullscreenRequestStaysInTargetBin() {
    WaveformEditorItem item;
    item.setWidth(3'440);
    item.setDurationSeconds(257.133333);
    setTestPosition(item, 72.0);
    item.m_sampleRateHz = 44'100;
    item.setZoomLevel(475.06);
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    const auto [requestStart, requestEnd] = item.requestRangeLocked(
        visibleStart, visibleEnd);
    const int requestedPoints = item.detailRequestPointCountLocked(
        requestStart, requestEnd);
    const double requestFrames = std::ceil(
        (requestEnd - requestStart) * 44'100.0);
    const double framesPerPoint = std::ceil(
        requestFrames / static_cast<double>(requestedPoints));
    const double returnedPoints = std::ceil(requestFrames / framesPerPoint);
    const double visiblePoints = returnedPoints
        * (visibleEnd - visibleStart) / (requestEnd - requestStart);

    QCOMPARE(framesPerPoint, 1.0);
    QVERIFY(requestedPoints <= 65'536);
    QVERIFY(visiblePoints >= item.requiredVisibleDetailPointsLocked(
        visibleEnd - visibleStart) * 0.9);
}

void WaveformSmokeTest::waveformEditorSampleViewPrefetchesUsefulSpan() {
    WaveformEditorItem item;
    item.setWidth(1'183);
    item.setDurationSeconds(257.133333);
    setTestPosition(item, 40.0);
    item.m_sampleRateHz = 44'100;
    item.setZoomLevel(76'683.55);
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    const auto [requestStart, requestEnd] = item.requestRangeLocked(
        visibleStart, visibleEnd);

    QVERIFY(visibleEnd - visibleStart < 0.004);
    QVERIFY(requestEnd - requestStart > 1.4);
    const int requestedPoints = item.detailRequestPointCountLocked(
        requestStart, requestEnd);
    QVERIFY(requestedPoints > 60'000);
    QVERIFY(requestedPoints <= 65'536);
}

void WaveformSmokeTest::waveformEditorPrefetchesBeforeDetailBoundary() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setDurationSeconds(10.0);
    item.setZoomLevel(10.0);
    setTestPosition(item, 5.0);
    item.m_requestTimer.stop();
    item.m_detail.sampleRateHz = 100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 3.5;
    item.m_detail.endSeconds = 6.5;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 300;
    item.m_detail.extrema.assign(600, 0.0F);

    setTestPosition(item, 5.5);

    QVERIFY(item.visibleRangeLocked().second < item.m_detail.endSeconds);
    QVERIFY(item.m_requestTimer.isActive());
}

void WaveformSmokeTest::waveformEditorFullscreenPrefetchDoesNotWaitPastCoverage() {
    WaveformEditorItem item;
    item.setWidth(3440);
    item.setHeight(1440);
    item.setDurationSeconds(225.413333);
    setTestPosition(item, 70.0);
    item.m_sampleRateHz = 44100;
    item.setZoomLevel(std::pow(1.25, 23));
    const auto [start, end] = item.visibleRangeLocked();
    const auto [requestStart, requestEnd] = item.requestRangeLocked(start, end);
    const double margin = requestEnd - end;
    QVERIFY(margin > 0.07 && margin < 0.08);
    item.m_detail.sampleRateHz = 44100;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = static_cast<int>((requestEnd - requestStart) * 44100);
    item.m_detail.startSeconds = requestStart;
    item.m_detail.endSeconds = requestEnd;
    item.m_playing = true;
    item.m_clockState.anchorSeconds += margin * 0.4 + 0.001;
    item.handleWindowFrameSwapped();
    const auto [nextStart, nextEnd] = item.visibleRangeLocked();
    QVERIFY(item.detailCoversRangeLocked(nextStart, nextEnd));
    QVERIFY(item.m_requestTimer.isActive());
    // A pending 40 ms zoom debounce must be expedited for playback prefetch.
    QCOMPARE(item.m_requestTimer.remainingTime(), 0);
    item.scheduleDetailRequest();
    QCOMPARE(item.m_requestTimer.remainingTime(), 0);
}

void WaveformSmokeTest::waveformEditorKeepsOverlappingDetailDuringHandoff() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);
    item.setZoomLevel(2.0);
    item.m_requestTimer.stop();
    item.m_detail.sampleRateHz = 100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 4.0;
    item.m_detail.endSeconds = 6.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 200;
    item.m_detail.extrema.assign(400, 0.5F);
    item.invalidateCacheLocked();

    QImage canvas(320, 180, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    int waveformPixels = 0;
    for (int y = 20; y < 160; ++y) {
        for (int x = 120; x < 200; ++x) {
            const QColor pixel(canvas.pixel(x, y));
            if (pixel.green() > 100 && pixel.red() < 100) ++waveformPixels;
        }
    }
    QVERIFY(waveformPixels > 20);
    QCOMPARE(canvas.pixelColor(150, 20), QColor(5, 9, 7));
}

void WaveformSmokeTest::waveformEditorWideDetailCacheKeepsViewportDensity() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);
    item.m_detail.sampleRateHz = 48'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 48'000;
    item.m_detail.extrema.assign(96'000, 0.25F);
    item.setZoomLevel(4.0);

    QImage canvas(320, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QVERIFY(item.m_cache.width() >= 959);
    QVERIFY(item.m_cache.width() <= 961);
    QVERIFY(item.m_cacheEndSeconds - item.m_cacheStartSeconds > 7.49);
    QVERIFY(item.m_cacheEndSeconds - item.m_cacheStartSeconds < 7.51);
}

void WaveformSmokeTest::waveformEditorWholeTrackAcceptsDecodedEndpointTolerance() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.m_detail.sampleRateHz = 48'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0 - 1.5 / 48'000.0;
    item.m_detail.framesPerPoint = 375;
    item.m_detail.pointCount = 1'280;
    item.m_detail.extrema.assign(2'560, 0.5F);
    QVERIFY(item.detailCoversRangeLocked(0.0, 10.0));
    QVERIFY(item.detailOrPendingRequestCoversLocked(0.0, 10.0));

    QImage canvas(320, 180, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    int waveformPixels = 0;
    for (int y = 44; y <= 47; ++y) {
        for (int x = 20; x < 300; ++x) {
            const QColor pixel(canvas.pixel(x, y));
            if (pixel.green() > 180 && pixel.red() < 100) ++waveformPixels;
        }
    }
    QVERIFY(waveformPixels > 200);
}

void WaveformSmokeTest::waveformEditorFullscreenWholeTrackRetainsCache() {
    for (bool detailReady : {false, true}) {
        WaveformEditorItem item;
        item.setWidth(1183);
        item.setHeight(180);
        // Playback reports nanoseconds, not the six decimals printed by ffprobe.
        constexpr double duration = 225.413333333;
        item.setDurationSeconds(duration);
        setTestPosition(item, 70.0);
        item.setOverviewComplete(true);
        item.setOverviewData(QByteArray(8192, static_cast<char>(128)));
        item.setCrosshairEnabled(true);
        if (detailReady) {
            item.m_sampleRateHz = 44'100;
            item.m_detail.sampleRateHz = 44'100;
            item.m_detail.channelCount = 1;
            item.m_detail.startSeconds = 0.0;
            item.m_detail.endSeconds = duration;
            item.m_detail.framesPerPoint = 512;
            item.m_detail.pointCount = static_cast<int>(std::ceil(duration * 44'100 / 512));
            item.m_detail.extrema.assign(item.m_detail.pointCount * 2U, 0.25F);
        }
        item.setPlaying(true);
        // Enter fullscreen without changing the whole-track zoom.
        item.setWidth(3440);
        item.setHeight(1440);
        QImage canvas(3440, 1440, QImage::Format_RGB32);
        QPainter painter(&canvas);
        item.paint(&painter);
        const auto cacheKey = item.m_cache.cacheKey();
        QCOMPARE(item.m_cache.width(), 3440);
        for (int frame = 0; frame < 3; ++frame) {
            item.handleWindowFrameSwapped();
            QVERIFY(!item.m_cacheDirty);
            item.setHoverPosition(300.0 + frame * 100, 400.0, true);
            item.paint(&painter);
            QCOMPARE(item.m_cache.cacheKey(), cacheKey);
        }
        // A genuine uncovered raster column must still trigger a refresh.
        item.m_cacheEndSeconds -= item.m_cacheSecondsPerPixel;
        item.handleWindowFrameSwapped();
        QVERIFY(item.m_cacheDirty);

        if (detailReady) {
            // A staged zoom-out must also accept that same rounded endpoint.
            item.m_presentedZoomLevel = 2.0;
            item.m_zoomOutHandoffPending = true;
            item.beginStagedCacheForRangeLocked(0.0, duration, true);
            QVERIFY(!item.m_stagedCache.isNull());
            // Bound the loop: rejection used to restart the same stage forever.
            for (int frame = 0; frame < 30 && !item.m_stagedCache.isNull(); ++frame) {
                item.advanceStagedCacheLocked();
            }
            QVERIFY(item.m_stagedCache.isNull());
            QVERIFY(!item.m_zoomOutHandoffPending);
            QCOMPARE(item.m_presentedZoomLevel, 1.0);
            QVERIFY(!item.m_cacheDirty);
        }
    }
}

void WaveformSmokeTest::waveformEditorZoomOutDefersOverviewUntilDetailReady() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(255)));
    item.m_sampleRateHz = 100;
    item.m_detail.sampleRateHz = 100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 4.0;
    item.m_detail.endSeconds = 6.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 200;
    item.m_detail.extrema.assign(400, 0.25F);
    item.setZoomLevel(5.0);

    QImage zoomedCanvas(320, 180, QImage::Format_RGB32);
    QPainter zoomedPainter(&zoomedCanvas);
    item.paint(&zoomedPainter);
    zoomedPainter.end();

    item.setZoomLevel(3.0);
    item.setZoomLevel(1.0);

    QCOMPARE(item.zoomLevel(), 1.0);
    QCOMPARE(item.m_presentedZoomLevel, 5.0);
    QVERIFY(item.m_zoomOutHandoffPending);
    QVERIFY(!item.m_zoomFallbackToOverview);
    QVERIFY(!item.m_cacheDirty);
    const auto [presentedStart, presentedEnd] = item.visibleRangeLocked();
    QCOMPARE(presentedEnd - presentedStart, 2.0);
    const auto [requestStart, requestEnd] = item.detailRequestVisibleRangeLocked();
    QCOMPARE(requestStart, 0.0);
    QCOMPARE(requestEnd, 10.0);
    item.m_requestTimer.stop();
}

void WaveformSmokeTest::waveformEditorDeferredZoomOutCommitsCompletedDetailCache() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);
    item.m_sampleRateHz = 100;
    item.m_detail.sampleRateHz = 100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 4.0;
    item.m_detail.endSeconds = 6.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 200;
    item.m_detail.extrema.assign(400, 0.25F);
    item.setZoomLevel(5.0);

    QImage canvas(320, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();
    item.setZoomLevel(1.0);
    item.m_requestTimer.stop();
    QVERIFY(item.m_zoomOutHandoffPending);

    item.m_sampleRateHz = 640;
    item.m_detail.sampleRateHz = 640;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 6'400;
    item.m_detail.extrema.assign(12'800, 0.5F);
    item.beginStagedCacheForRangeLocked(0.0, 10.0, true);
    QVERIFY(!item.m_stagedCache.isNull());
    QVERIFY(!item.advanceStagedCacheLocked());
    QCOMPARE(item.m_presentedZoomLevel, 5.0);
    while (!item.m_stagedCache.isNull()) {
        item.advanceStagedCacheLocked();
    }

    QCOMPARE(item.m_presentedZoomLevel, 1.0);
    QVERIFY(!item.m_zoomOutHandoffPending);
    QVERIFY(!item.m_cacheDirty);
    QCOMPARE(item.m_cacheStartSeconds, 0.0);
    QCOMPARE(item.m_cacheEndSeconds, 10.0);
}

void WaveformSmokeTest::waveformEditorZoomOutKeepsReadyDetail() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);
    item.m_sampleRateHz = 640;
    item.m_detail.sampleRateHz = 640;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 6'400;
    item.m_detail.extrema.assign(12'800, 0.25F);
    item.setZoomLevel(5.0);
    QVERIFY(!item.m_zoomFallbackToOverview);

    item.setZoomLevel(2.0);

    QVERIFY(!item.m_zoomFallbackToOverview);
}

void WaveformSmokeTest::waveformEditorZoomInRetainsCoveredCacheWhileRefining() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(128)));

    QImage canvas(320, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();
    QVERIFY(!item.m_cacheDirty);

    item.setZoomLevel(4.0);

    QVERIFY(!item.m_cacheDirty);
    QVERIFY(!item.m_zoomFallbackToOverview);
    item.m_requestTimer.stop();
}

void WaveformSmokeTest::waveformEditorPerChannelZoomShowsOlderDetailUntilTheNewArrives() {
    // Issue 542: the overview, one envelope of every channel, is not drawn
    // in the per-channel view's lanes, so a zoom that fell back to it, until
    // the detail for the zoom arrived, showed empty lanes. The detail the
    // view has, of another zoom, stands in where it reaches: zoomed out,
    // over the part of the view it covers, and zoomed in past its points,
    // each point held until the next.
    WaveformEditorItem item;
    constexpr int width = 400;
    constexpr int height = 180;
    item.setWidth(width);
    item.setHeight(height);
    item.setDurationSeconds(20.0);
    setTestPosition(item, 10.0);
    item.setViewMode(1);
    item.setChannelCountHint(2);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(200)));
    const auto setDetail = [&item](int framesPerPoint) {
        item.m_sampleRateHz = 1'000;
        item.m_detail.sampleRateHz = 1'000;
        item.m_detail.channelCount = 2;
        item.m_detail.startSeconds = 6.0;
        item.m_detail.endSeconds = 14.0;
        item.m_detail.framesPerPoint = static_cast<quint32>(framesPerPoint);
        item.m_detail.pointCount = 8'000 / framesPerPoint;
        item.m_detail.extrema.assign(static_cast<std::size_t>(item.m_detail.pointCount) * 4U, 0.5F);
        for (std::size_t i = 0; i < item.m_detail.extrema.size(); i += 2) {
            item.m_detail.extrema[i] = -0.5F;
        }
    };
    // Paints a frame; every column of the detail's seconds shows the wave
    // in both lanes.
    const auto frameShowsTheDetail = [&item](const char *step) {
        QImage canvas(width, height, QImage::Format_RGB32);
        QPainter painter(&canvas);
        item.paint(&painter);
        painter.end();
        const auto [start, end] = item.visibleRangeLocked();
        for (int x = 0; x < width; ++x) {
            const double seconds = start + (x + 0.5) * (end - start) / width;
            // The playhead, at 10 s, is drawn over it, cased a pixel each
            // side.
            const double playheadX = (10.0 - start) / (end - start) * width;
            if (seconds < 6.0 || seconds >= 14.0 || std::abs(x + 0.5 - playheadX) < 3.0) {
                continue;
            }
            for (const int y : {height / 4 - 10, 3 * height / 4 - 10}) {
                if (canvas.pixelColor(x, y).green() <= 180) {
                    QTest::qFail(qPrintable(QStringLiteral("%1: no wave at %2, %3 (%4 s)")
                        .arg(QString::fromLatin1(step)).arg(x).arg(y).arg(seconds)),
                        __FILE__, __LINE__);
                    return false;
                }
            }
        }
        return true;
    };

    // An envelope of points 5 ms apart: 16 a pixel at 4x.
    setDetail(5);
    item.setZoomLevel(4.0);
    QVERIFY(frameShowsTheDetail("zoomed in"));
    // While playing, the view is drawn in playback tiles, which leave no
    // cache to hold the zoom on: the zoom out shows at once.
    item.invalidateCacheLocked();
    item.setZoomLevel(1.0);
    QVERIFY(!item.m_zoomOutHandoffPending);
    QVERIFY(item.m_zoomFallbackToOverview);
    QVERIFY(frameShowsTheDetail("reset"));
    item.setZoomLevel(4.0);
    QVERIFY(frameShowsTheDetail("in again"));
    // Points 100 ms apart, sixteen pixels at 8x.
    setDetail(100);
    item.invalidateCacheLocked();
    item.setZoomLevel(8.0);
    QVERIFY(frameShowsTheDetail("in past the points"));
    item.m_requestTimer.stop();
}

void WaveformSmokeTest::waveformEditorSparseDetailCoversEachPointsFrames() {
    // Detail of points further apart than a pixel, as a zoom shows until
    // its own detail arrives, draws each point over the pixels of its
    // frames: not from its middle to the next point's, half a point late,
    // and to the ends of the first and the last point, but no further than
    // the detail. Here 21 points of 100 ms, loud and silent in turn, from
    // 0 to 2.1 s, in a view of 200 pixels a second from -0.1 s to 2.2 s.
    WaveformEditorItem::DetailWindow detail;
    detail.sampleRateHz = 1'000;
    detail.channelCount = 1;
    detail.startSeconds = 0.0;
    detail.endSeconds = 2.1;
    detail.framesPerPoint = 100;
    detail.pointCount = 21;
    detail.extrema.resize(42);
    for (int point = 0; point < 21; ++point) {
        const float amplitude = point % 2 == 0 ? 0.8F : 0.0F;
        detail.extrema[point * 2] = -amplitude;
        detail.extrema[point * 2 + 1] = amplitude;
    }
    const WaveformEditorItem::LaneStyle style;
    constexpr int width = 460;
    constexpr int height = 100;
    constexpr double visibleStart = -0.1;
    constexpr double visibleEnd = 2.2;
    // Drawn whole, and in slices that start and end within points.
    const QList<QList<int>> slicings{{0, width}, {0, 7, 33, 251, 415, width}};
    for (const QList<int> &edges : slicings) {
        QImage canvas(width, height, QImage::Format_RGB32);
        canvas.fill(style.background);
        QPainter painter(&canvas);
        for (int i = 0; i + 1 < edges.size(); ++i) {
            WaveformEditorItem::drawDetailSlice(painter, detail, style, width, height,
                visibleStart, visibleEnd, 1, edges[i], edges[i + 1], 1.0);
        }
        painter.end();
        for (int x = 0; x < width; ++x) {
            const double seconds = visibleStart + (x + 0.5) / 200.0;
            const int point = static_cast<int>(std::floor(seconds / 0.1 + 1e-9));
            const bool loud = seconds >= 0.0 && seconds < 2.1 && point % 2 == 0;
            // A loud point reaches well above the lane's middle.
            const bool drawn = canvas.pixelColor(x, height / 4) != QColor(style.background);
            QVERIFY2(drawn == loud, qPrintable(QStringLiteral("slices %1: x %2 (%3 s) is %4")
                .arg(edges.size() - 1).arg(x).arg(seconds).arg(drawn ? "drawn" : "empty")));
        }
    }
}

void WaveformSmokeTest::waveformEditorSparseZoomDetailFallsBackUntilReady() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);
    item.setZoomLevel(100.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(255)));
    item.m_detail.sampleRateHz = 48'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 4.5;
    item.m_detail.endSeconds = 5.5;
    item.m_detail.framesPerPoint = 480;
    item.m_detail.pointCount = 100;
    item.m_detail.extrema.assign(200, 0.0F);
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();

    QVERIFY(item.detailCoversRangeLocked(visibleStart, visibleEnd));
    QVERIFY(!item.detailResolutionCoversLocked(visibleStart, visibleEnd));
    QVERIFY(!item.renderDetailDirectlyLocked(visibleStart, visibleEnd));

    QImage canvas(400, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QVERIFY(canvas.pixelColor(30, 20).green() > 180);
    QVERIFY(canvas.pixelColor(370, 20).green() > 180);
}

void WaveformSmokeTest::waveformEditorSeparatesChannelPanes() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setViewMode(1);
    item.setChannelCountHint(2);
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.sampleRateHz = 1'000;
    item.m_detail.channelCount = 2;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 10'000;
    item.m_detail.extrema.resize(40'000);
    for (int i = 0; i < 20'000; ++i) {
        item.m_detail.extrema[i * 2] = -1.0F;
        item.m_detail.extrema[i * 2 + 1] = 1.0F;
    }

    QImage canvas(320, 180, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QCOMPARE(canvas.pixelColor(160, 90), QColor(79, 94, 86));
    QVERIFY(canvas.pixelColor(160, 89).green() > 180);
    QVERIFY(canvas.pixelColor(160, 91).green() > 180);
}

void WaveformSmokeTest::waveformEditorRestrictsDetailWorkToVisiblePoints() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setDurationSeconds(10.0);
    item.m_detail.sampleRateHz = 48'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 4.75;
    item.m_detail.endSeconds = 5.25;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 24'000;
    item.m_detail.extrema.assign(48'000, 0.0F);

    const auto [first, last] = item.detailPointRangeLocked(4.999, 5.001);

    QVERIFY(first > 11'000);
    QVERIFY(last < 13'000);
    QVERIFY(last - first < 110);
}

void WaveformSmokeTest::waveformEditorDetailPointsUseAbsoluteSampleTimes() {
    WaveformEditorItem item;
    item.m_detail.sampleRateHz = 100;
    item.m_detail.framesPerPoint = 4;
    item.m_detail.pointCount = 50;
    item.m_detail.startSeconds = 0.04;
    item.m_detail.endSeconds = 2.03;

    const double sharedBinTime = item.detailPointTimeLocked(1);
    QVERIFY(std::abs(sharedBinTime - 0.095) < 0.000'001);

    item.m_detail.startSeconds = 0.08;
    item.m_detail.endSeconds = 2.07;
    QVERIFY(std::abs(item.detailPointTimeLocked(0) - sharedBinTime) < 0.000'001);
}

void WaveformSmokeTest::waveformEditorReusesSameSizedCache() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(128)));

    QImage canvas(320, 180, QImage::Format_RGB32);
    QPainter firstPainter(&canvas);
    item.paint(&firstPainter);
    firstPainter.end();
    const uchar *firstBuffer = item.m_cache.constBits();

    item.invalidateCacheLocked();
    QPainter secondPainter(&canvas);
    item.paint(&secondPainter);
    secondPainter.end();

    QCOMPARE(item.m_cache.constBits(), firstBuffer);
}

void WaveformSmokeTest::waveformEditorCacheHandoffsKeepAbsolutePixelGrid() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(100);
    item.setDurationSeconds(10.0);
    item.m_sampleRateHz = 1'000;
    item.m_channelCount = 1;
    item.m_detail.sampleRateHz = 1'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 10'000;
    item.m_zoomLevel = 5.0;
    item.m_presentedZoomLevel = 5.0;
    item.m_clockState.anchorSeconds = 3.37;
    item.m_detail.extrema.reserve(20'000);
    for (int point = 0; point < item.m_detail.pointCount; ++point) {
        const float minimum = -static_cast<float>((point % 17) + 1) / 20.0F;
        const float maximum = static_cast<float>((point % 23) + 1) / 25.0F;
        item.m_detail.extrema.push_back(minimum);
        item.m_detail.extrema.push_back(maximum);
    }

    const auto firstGrid = item.cacheGridForRangeLocked(2.0, 4.0, 0.0, 10.0);
    const auto secondGrid = item.cacheGridForRangeLocked(2.37, 4.37, 0.0, 10.0);
    QCOMPARE(firstGrid.secondsPerPixel, secondGrid.secondsPerPixel);
    const double pixelOffset = (secondGrid.startSeconds - firstGrid.startSeconds)
        / firstGrid.secondsPerPixel;
    const int firstSourceX = static_cast<int>(std::llround(pixelOffset));
    QVERIFY(std::abs(pixelOffset - static_cast<double>(firstSourceX)) < 0.000'001);

    QImage firstCache(firstGrid.width, 100, QImage::Format_RGB32);
    firstCache.fill(QColor(5, 9, 7));
    QPainter firstPainter(&firstCache);
    item.drawDetailLocked(
        firstPainter, firstGrid.width, 100,
        firstGrid.startSeconds, firstGrid.endSeconds, 1);
    firstPainter.end();

    QImage secondCache(secondGrid.width, 100, QImage::Format_RGB32);
    secondCache.fill(QColor(5, 9, 7));
    QPainter secondPainter(&secondCache);
    item.drawDetailLocked(
        secondPainter, secondGrid.width, 100,
        secondGrid.startSeconds, secondGrid.endSeconds, 1);
    secondPainter.end();

    const int overlapWidth = std::min(
        secondCache.width(), firstCache.width() - firstSourceX);
    QVERIFY(overlapWidth > 0);
    for (int y = 0; y < firstCache.height(); ++y) {
        const auto *firstLine = reinterpret_cast<const QRgb *>(
            firstCache.constScanLine(y));
        const auto *secondLine = reinterpret_cast<const QRgb *>(
            secondCache.constScanLine(y));
        // QPainter may quantize antialias coverage by one value where a path
        // meets an image boundary. Interior overlap must remain bit-exact;
        // tile-edge continuity is covered separately with rendered edges.
        for (int x = 1; x + 1 < overlapWidth; ++x) {
            QVERIFY2(
                secondLine[x] == firstLine[firstSourceX + x],
                qPrintable(QStringLiteral(
                    "overlap mismatch x=%1 y=%2 first=%3 second=%4")
                    .arg(x)
                    .arg(y)
                    .arg(firstLine[firstSourceX + x], 0, 16)
                    .arg(secondLine[x], 0, 16)));
        }
    }

    item.m_cache = firstCache;
    item.m_cacheDirty = false;
    item.m_cacheStartSeconds = firstGrid.startSeconds;
    item.m_cacheEndSeconds = firstGrid.endSeconds;
    item.m_cacheSecondsPerPixel = firstGrid.secondsPerPixel;
    item.m_cacheFramesPerPoint = item.m_detail.framesPerPoint;
    item.m_cacheDisplayedChannels = 1;
    item.beginStagedCacheForRangeLocked(2.37, 4.37, false);

    QCOMPARE(item.m_stagedCacheNextX, 0);

    for (int iteration = 0;
         iteration < 20 && !item.m_stagedCache.isNull();
         ++iteration) {
        item.advanceStagedCacheLocked();
    }
    QVERIFY(item.m_stagedCache.isNull());
    QCOMPARE(item.m_cache.size(), secondCache.size());
    for (int y = 0; y < item.m_cache.height(); ++y) {
        const auto *actual = reinterpret_cast<const QRgb *>(
            item.m_cache.constScanLine(y));
        const auto *expected = reinterpret_cast<const QRgb *>(
            secondCache.constScanLine(y));
        for (int x = 0; x < item.m_cache.width(); ++x) {
            QCOMPARE(actual[x], expected[x]);
        }
    }
}

void WaveformSmokeTest::waveformEditorReplacementCacheDoesNotReuseOldRaster() {
    WaveformEditorItem item;
    item.setWidth(1'000);
    item.setHeight(1'440);
    item.setDurationSeconds(10.0);
    item.m_sampleRateHz = 1'000;
    item.m_channelCount = 1;
    item.m_detail.sampleRateHz = 1'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 10'000;
    item.m_detail.extrema.assign(20'000, 0.25F);
    item.rebuildCacheLocked(1'000, 1'440);
    const QImage oldCache = item.m_cache;

    item.beginStagedCacheLocked();

    QVERIFY(!item.m_stagedCache.isNull());
    QCOMPARE(item.m_stagedCacheNextX, 0);
    QVERIFY(item.m_stagedCache.constBits() != oldCache.constBits());
}

void WaveformSmokeTest::waveformEditorBuildsReplacementCacheIncrementally() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(128)));

    QImage canvas(320, 180, QImage::Format_RGB32);
    QPainter initialPainter(&canvas);
    item.paint(&initialPainter);
    initialPainter.end();
    const uchar *initialCache = item.m_cache.constBits();

    item.setZoomLevel(4.0);
    item.m_sampleRateHz = 48'000;
    item.m_channelCount = 1;
    item.m_detail.sampleRateHz = 48'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 96;
    item.m_detail.pointCount = 5'000;
    item.m_detail.extrema.resize(10'000);
    for (int point = 0; point < item.m_detail.pointCount; ++point) {
        item.m_detail.extrema[static_cast<std::size_t>(point) * 2U] = -0.5F;
        item.m_detail.extrema[static_cast<std::size_t>(point) * 2U + 1U] = 0.5F;
    }

    item.beginStagedCacheLocked();
    QVERIFY(!item.m_stagedCache.isNull());
    item.advanceStagedCacheLocked();
    QCOMPARE(item.m_cache.constBits(), initialCache);
    for (int iteration = 0; iteration < 20 && !item.m_stagedCache.isNull(); ++iteration) {
        item.advanceStagedCacheLocked();
    }

    QVERIFY(item.m_stagedCache.isNull());
    QVERIFY(item.m_cache.constBits() != initialCache);
    QVERIFY(item.m_cache.width() >= 959);
    QVERIFY(item.m_cache.width() <= 961);
    QVERIFY(!item.m_cacheDirty);
}

void WaveformSmokeTest::waveformEditorPaintDefersGuiContinuation() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.m_stagedCache = QImage(320, 180, QImage::Format_RGB32);
    item.m_stagedCache.fill(QColor(54, 225, 161));
    item.m_stagedCacheStartSeconds = 0.0;
    item.m_stagedCacheEndSeconds = 1.0;
    item.m_stagedCacheSecondsPerPixel = 1.0 / 320.0;
    item.m_stagedCacheNextX = item.m_stagedCache.width();
    item.m_stagedCacheCommitsDeferredZoom = true;
    item.m_zoomLevel = 1.0;
    item.m_presentedZoomLevel = 2.0;
    item.m_zoomOutHandoffPending = true;
    item.m_durationSeconds = 1.0;

    QSignalSpy samplePointsSpy(
        &item, &WaveformEditorItem::samplePointsVisibleChanged);
    QImage canvas(320, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QCOMPARE(samplePointsSpy.count(), 0);
    QVERIFY(item.m_guiContinuationQueued.load(std::memory_order_acquire));
    QTRY_COMPARE(samplePointsSpy.count(), 1);
    QVERIFY(!item.m_guiContinuationQueued.load(std::memory_order_acquire));

    item.m_stagedCache = QImage(320, 180, QImage::Format_RGB32);
    item.m_stagedCacheNextX = 0;
    QPainter continuationPainter(&canvas);
    item.paint(&continuationPainter);
    continuationPainter.end();

    QVERIFY(!item.m_stagedCache.isNull());
    QVERIFY(item.m_guiRepaintPending.load(std::memory_order_acquire));
    QVERIFY(item.m_guiContinuationQueued.load(std::memory_order_acquire));
    QTRY_VERIFY(!item.m_guiContinuationQueued.load(std::memory_order_acquire));
    QVERIFY(!item.m_guiRepaintPending.load(std::memory_order_acquire));
}

void WaveformSmokeTest::waveformEditorZoomedPlaybackUsesScrollingCache() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);
    item.setZoomLevel(10.0);
    item.m_detail.sampleRateHz = 48'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 3.5;
    item.m_detail.endSeconds = 6.5;
    item.m_detail.framesPerPoint = 90;
    item.m_detail.pointCount = 4'800;
    item.m_detail.extrema.assign(9'600, 0.25F);
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();

    QVERIFY(!item.samplePointsVisibleLocked());
    QVERIFY(!item.renderDetailDirectlyLocked(visibleStart, visibleEnd));

    QImage canvas(400, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QVERIFY(!item.m_cacheDirty);
    QVERIFY(!item.m_cache.isNull());
}

void WaveformSmokeTest::waveformEditorPlaybackOverviewCacheHasForwardHeadroom() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);
    item.setZoomLevel(10.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(128)));
    item.m_playing = true;

    QImage canvas(400, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    const double visibleSpan = visibleEnd - visibleStart;

    QVERIFY(!item.m_cacheDirty);
    QVERIFY(item.m_cacheEndSeconds > visibleEnd + visibleSpan * 1.6);
    setTestPosition(item, 5.5);
    QVERIFY(!item.m_cacheDirty);
}

void WaveformSmokeTest::waveformEditorCachedPaintClearsUncoveredPixels() {
    // A cache that covers only part of the view, as one playback has moved
    // past while no detail window comes: the part it does not cover is
    // cleared. (Paused, the cache would be drawn again for the view.)
    WaveformEditorItem item;
    item.setWidth(100);
    item.setHeight(100);
    item.setDurationSeconds(10.0);
    item.setPlaying(true);
    setTestPosition(item, 0.0);
    {
        QMutexLocker lock(&item.m_stateMutex);
        // A file whose detail has not come, without asking for it.
        item.m_sourcePath = QStringLiteral("/fixture/a.flac");
    }
    item.m_requestTimer.stop();
    item.m_cache = QImage(100, 100, QImage::Format_RGB32);
    item.m_cache.fill(QColor(54, 225, 161));
    item.m_cacheDirty = false;
    item.m_cachedViewportWidth = 100;
    item.m_cachedViewportHeight = 100;
    item.m_cacheStartSeconds = 5.0;
    item.m_cacheEndSeconds = 15.0;
    item.m_cacheSecondsPerPixel = 0.1;

    QImage canvas(100, 100, QImage::Format_RGB32);
    canvas.fill(QColor(255, 0, 255));
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QCOMPARE(canvas.pixelColor(10, 20), QColor(5, 9, 7));
    QCOMPARE(canvas.pixelColor(75, 20), QColor(54, 225, 161));
}

void WaveformSmokeTest::waveformEditorFollowsThePlaybackClock() {
    WaveformEditorItem item;
    item.setDurationSeconds(30.0);
    item.m_requestTimer.stop();
    item.setPlaying(true);
    PlaybackClock &clock = testClock(item);
    setTestPositionAgo(item, 10.0, 0.2);
    // The waveform shows the clock's position at the time it is drawn,
    // without an estimate of its own.
    const double before = clock.positionAt(monotonicNowNs());
    const double displayed = item.displayedPositionSecondsLocked();
    const double after = clock.positionAt(monotonicNowNs());
    QVERIFY(before >= 10.2);
    QVERIFY2(displayed >= before && displayed <= after,
             qPrintable(QStringLiteral("displayed=%1 clock=[%2, %3]")
                            .arg(displayed, 0, 'f', 6)
                            .arg(before, 0, 'f', 6)
                            .arg(after, 0, 'f', 6)));
    QCOMPARE(item.positionSeconds() >= displayed, true);
}

void WaveformSmokeTest::waveformEditorSeeksMoveAtOnce() {
    WaveformEditorItem item;
    item.setDurationSeconds(30.0);
    setTestPosition(item, 10.0);
    item.setPlaying(true);
    for (double target : {9.5, 9.4, 9.45, 0.0}) {
        testClock(item).holdAt(target);
        QCOMPARE(item.positionSeconds(), target);
        QVERIFY(item.playing());
    }
    item.setPlaying(false);
    testClock(item).holdAt(0.25);
    QCOMPARE(item.positionSeconds(), 0.25);
    QVERIFY(!item.playing());
}

void WaveformSmokeTest::waveformEditorHoverDrawsCrosshairAndReadouts() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setCrosshairEnabled(true);

    QImage baseline(320, 180, QImage::Format_RGB32);
    baseline.fill(Qt::black);
    QPainter baselinePainter(&baseline);
    item.paint(&baselinePainter);
    baselinePainter.end();

    item.setHoverPosition(137.0, 53.0, true);
    QImage hovered(320, 180, QImage::Format_RGB32);
    hovered.fill(Qt::black);
    QPainter hoverPainter(&hovered);
    item.paint(&hoverPainter);
    hoverPainter.end();

    int changedPixels = 0;
    for (int y = 0; y < hovered.height(); ++y) {
        for (int x = 0; x < hovered.width(); ++x) {
            if (hovered.pixel(x, y) != baseline.pixel(x, y)) ++changedPixels;
        }
    }
    QVERIFY(changedPixels > 250);
}

void WaveformSmokeTest::waveformEditorCrosshairLabelsMatchSpectrogramMargins() {
    QCOMPARE(
        WaveformEditorItem::formatCrosshairTime(28.98),
        QStringLiteral("0:28.980"));
    QCOMPARE(
        WaveformEditorItem::formatCrosshairTime(3'723.004),
        QStringLiteral("1:02:03.004"));

    constexpr int width = 1'183;
    constexpr int height = 318;
    constexpr int cursorX = 563;
    constexpr int cursorY = 137;
    constexpr int valueWidth = 42;
    constexpr int timeWidth = 48;
    constexpr int textHeight = 12;
    const auto [valueRect, timeRect] =
        WaveformEditorItem::crosshairLabelRects(
            width, height, cursorX, cursorY,
            valueWidth, timeWidth, textHeight);

    QCOMPARE(valueRect.right(), width - 3);
    // Centred on the horizontal line.
    QCOMPARE(valueRect.y(), cursorY - valueRect.height() / 2);
    QCOMPARE(timeRect.x(), cursorX - timeRect.width() / 2);
    QCOMPARE(timeRect.bottom(), height - 3);
    QCOMPARE(valueRect.size(), QSize(valueWidth + 6, textHeight + 6));
    QCOMPARE(timeRect.size(), QSize(timeWidth + 6, textHeight + 6));
}

void WaveformSmokeTest::waveformEditorCrosshairLabelsKeepApartInShortPlots() {
    // The readout spans the whole plot, all lanes of the per-channel view;
    // its layout is the spectrogram's, which also lays out lanes of 27 px.
    constexpr int width = 600;
    const QSizeF reserved(120.0, 40.0);
    for (const int height : {27, 36, 220}) {
        for (const QPoint cursor : {QPoint(599, height - 1), QPoint(560, height - 1),
                                    QPoint(599, 0), QPoint(0, height - 1)}) {
            const auto [value, time] = WaveformEditorItem::crosshairLabelRects(
                width, height, cursor.x(), cursor.y(), 42, 48, 12, reserved);
            const QByteArray where = QByteArray::number(height) + " px at "
                + QTest::toString(cursor) + ": value " + QTest::toString(value)
                + ", time " + QTest::toString(time);
            QVERIFY2(QRect(0, 0, width, height).contains(value)
                         && QRect(0, 0, width, height).contains(time),
                     where.constData());
            QVERIFY2(!value.intersects(time), where.constData());
            QVERIFY2(!value.intersects(QRect(width - 120, 0, 120, 40)), where.constData());
        }
    }

    // At the pane's 220 px minimum, the height of all eight lanes of the
    // per-channel view: in the bottom right corner, the level above the time.
    const auto [value, time] = WaveformEditorItem::crosshairLabelRects(
        width, 220, 599, 219, 42, 48, 12, reserved);
    QVERIFY(value.bottom() < time.top());
    QCOMPARE(value.right(), width - 3);
    // In a plot too short for that, the time stops at the level's left.
    const auto [shortValue, shortTime] = WaveformEditorItem::crosshairLabelRects(
        width, 30, 599, 29, 42, 48, 12, reserved);
    QVERIFY(shortTime.right() < shortValue.left());

    // The lowest of eight lanes of the per-channel view, 27 px at the
    // pane's minimum: its head is on its rows, and the time keeps clear.
    WaveformEditorItem item;
    item.setLaneHeadBounds(QRectF(4.0, 3.0, 82.0, 26.0));
    QRect head;
    {
        QMutexLocker lock(&item.m_stateMutex);
        head = item.lowestLaneHeadLocked(220, 8);
    }
    // PlotLanes.top(7, 8, 220): round(192.5) + 1.
    QCOMPARE(head, QRect(4, 197, 82, 26));
    for (const int x : {0, 40, 90, 300}) {
        const auto [value, time] = WaveformEditorItem::crosshairLabelRects(
            width, 220, x, 219, 42, 48, 12, reserved, head);
        QVERIFY2(!time.intersects(head), qPrintable(QTest::toString(QRectF(time))));
        QVERIFY(!value.intersects(time));
    }
}

void WaveformSmokeTest::waveformEditorCrosshairLabelsStayReadableInNarrowPlots() {
    // The waveform shares the spectrogram's layout: in a plot too narrow for
    // its lane head, the level and the time side by side, it leaves the
    // level out, and the time too where it doesn't fit right of the head.
    // The plates never meet each other or the head and never leave the plot,
    // wherever the pointer is.
    const QFont font = CrosshairReadout::font();
    const QFontMetrics metrics(font);
    // The level changes with the pointer's height; whether it shows goes by
    // the widest it can be.
    const QStringList levels{QStringLiteral("0.0 dB"), QStringLiteral("-6.0 dB"),
                             QStringLiteral("-12.5 dB"), QStringLiteral("-100.0 dB"),
                             QStringLiteral("-∞ dB")};
    const int widestValue = std::max(
        CrosshairReadout::widestDigitsWidth(QStringLiteral("-100.0 dB"), metrics),
        CrosshairReadout::widestDigitsWidth(QStringLiteral("-∞ dB"), metrics));
    const int timeText = metrics.horizontalAdvance(QStringLiteral("0:28.980"));
    const int widestTime = CrosshairReadout::widestDigitsWidth(QStringLiteral("0:28.980"), metrics);
    const auto valueText = [&](int y) {
        return metrics.horizontalAdvance(levels[y % levels.size()]);
    };
    WaveformEditorItem item;
    item.setLaneHeadBounds(QRectF(4.0, 3.0, 82.0, 26.0));
    for (const int height : {27, 36}) {
        QRect head;
        {
            QMutexLocker lock(&item.m_stateMutex);
            head = item.lowestLaneHeadLocked(height, 1);
        }
        QVERIFY(head.isValid());
        for (const int width : {160, 180, 200, 220, 240}) {
            const QRect plot(0, 0, width, height);
            const auto [firstValue, firstTime] = WaveformEditorItem::crosshairLabelRects(
                width, height, 0, 0, valueText(0), timeText, metrics.height(), QSizeF(), head,
                widestValue, widestTime);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const auto [value, time] = WaveformEditorItem::crosshairLabelRects(
                        width, height, x, y, valueText(y), timeText, metrics.height(), QSizeF(),
                        head, widestValue, widestTime);
                    const QByteArray where = QByteArray::number(width) + "x"
                        + QByteArray::number(height) + " at " + QByteArray::number(x) + ", "
                        + QByteArray::number(y) + ": value " + QTest::toString(QRectF(value))
                        + ", time " + QTest::toString(QRectF(time));
                    for (const QRect &plate : {value, time}) {
                        if (plate.isNull()) continue;
                        if (!plot.contains(plate) || plate.intersects(head)) {
                            QFAIL(where.constData());
                        }
                    }
                    if (value.isValid() && value.intersects(time)) QFAIL(where.constData());
                    // The same plates wherever the pointer is.
                    if (value.isNull() != firstValue.isNull()
                        || time.isNull() != firstTime.isNull()) {
                        QFAIL(where.constData());
                    }
                }
            }
            // 200 px leave the time room right of the head, if not the level.
            if (width >= 200) {
                QVERIFY(firstTime.isValid());
            }
        }
    }
}

void WaveformSmokeTest::waveformEditorDrawsNoCrosshairLabelItLeavesOut() {
    // A plate the layout leaves out draws nothing: its text doesn't show
    // without its plate in the top left corner. At 180 px the level is left
    // out; at 120 px the time is too.
    const QFontMetrics metrics(CrosshairReadout::font());
    const int widestValue = std::max(
        CrosshairReadout::widestDigitsWidth(QStringLiteral("-100.0 dB"), metrics),
        CrosshairReadout::widestDigitsWidth(QStringLiteral("-∞ dB"), metrics));
    const int widestTime = CrosshairReadout::widestDigitsWidth(QStringLiteral("0:00.000"), metrics);
    const QRect head(4, 3, 82, 26);
    constexpr int height = 27;
    for (const int width : {180, 120}) {
        const auto [value, time] = WaveformEditorItem::crosshairLabelRects(
            width, height, width - 5, 24, widestValue, widestTime, metrics.height(), QSizeF(),
            head, widestValue, widestTime);
        QVERIFY(value.isEmpty());
        QCOMPARE(time.isEmpty(), width == 120);

        WaveformEditorItem item;
        item.setWidth(width);
        item.setHeight(height);
        item.setDurationSeconds(10.0);
        item.setCrosshairEnabled(true);
        item.setLaneHeadBounds(QRectF(head));
        const auto paint = [&]() {
            QImage image(width, height, QImage::Format_RGB32);
            image.fill(Qt::black);
            QPainter painter(&image);
            item.paint(&painter);
            painter.end();
            return image;
        };
        const QImage baseline = paint();
        // The lines at the right and the bottom, away from the top left.
        item.setHoverPosition(width - 5.0, 24.0, true);
        const QImage hovered = paint();
        const auto changedIn = [&](const QRect &area) {
            int changed = 0;
            for (int y = area.top(); y <= area.bottom(); ++y) {
                for (int x = area.left(); x <= area.right(); ++x) {
                    changed += hovered.pixel(x, y) != baseline.pixel(x, y) ? 1 : 0;
                }
            }
            return changed;
        };
        const QByteArray where = QByteArray::number(width) + " px";
        // The crosshair shows, and nothing where a plate without a rect
        // would draw its text.
        QVERIFY2(changedIn(QRect(0, 24, width, 1)) > width / 4, where.constData());
        QCOMPARE(changedIn(QRect(0, 0, 80, 22)), 0);
        // Right of the head, the time where it shows, and nothing else.
        const int timeArea = changedIn(QRect(90, 0, width - 90 - 6, 24));
        QVERIFY2(width == 120 ? timeArea == 0 : timeArea > 40, where.constData());
    }
}

void WaveformSmokeTest::waveformOverlaysKeepClearOfTheReservedCorner() {
    constexpr int width = 400;
    constexpr int height = 180;
    const QSizeF reserved(120.0, 40.0);
    const QRect corner(width - 120, 0, 120, 40);

    // The readout leaves the corner, only while it would be under it, and
    // keeps 6 px from its plates (#373 H5).
    const auto [topValue, topTime] = WaveformEditorItem::crosshairLabelRects(
        width, height, 100, 5, 42, 48, 12, reserved);
    QVERIFY(!topValue.intersects(corner));
    QCOMPARE(topValue.right(), corner.left() - 1 - ViewCorner::kLabelGap);
    const auto [lowValue, lowTime] = WaveformEditorItem::crosshairLabelRects(
        width, height, 100, 100, 42, 48, 12, reserved);
    QCOMPARE(lowValue.right(), width - 3);
    QCOMPARE(lowTime, topTime);

    WaveformEditorItem item;
    item.setWidth(width);
    item.setHeight(height);
    item.setDurationSeconds(10.0);
    item.setGridEnabled(true);
    const auto paint = [&]() {
        QImage image(width, height, QImage::Format_RGB32);
        image.fill(Qt::black);
        QPainter painter(&image);
        item.paint(&painter);
        painter.end();
        return image;
    };
    const auto difference = [](const QImage &a, const QImage &b) {
        QRect changed;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                if (a.pixel(x, y) != b.pixel(x, y)) {
                    changed |= QRect(x, y, 1, 1);
                }
            }
        }
        return changed;
    };

    // The scale's labels at the corner move out from under it.
    const QImage edgeLabels = paint();
    item.setReservedCorner(reserved);
    const QImage movedLabels = paint();
    const QRect moved = difference(edgeLabels, movedLabels);
    QVERIFY(moved.intersects(corner));
    QVERIFY(moved.left() < corner.left());

    // The counter is drawn where it says it is, beside the corner.
    item.setShowFpsOverlay(true);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_fpsValue = 60;
    }
    const QRectF counter = item.fpsCounterRect();
    QVERIFY(!counter.isEmpty());
    QVERIFY(!counter.intersects(QRectF(corner)));
    const QRect drawn = difference(movedLabels, paint());
    QVERIFY(!drawn.isEmpty());
    QVERIFY(counter.adjusted(-1, -1, 1, 1).contains(QRectF(drawn)));
}

void WaveformSmokeTest::waveformEditorSampleViewRepaintsCrosshairCleanly() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(1.0);
    setTestPosition(item, 0.5);
    item.setCrosshairEnabled(true);
    item.m_sampleRateHz = 100;
    item.m_detail.sampleRateHz = 100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 1.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 100;
    item.m_detail.extrema.assign(200, 0.0F);
    item.setZoomLevel(item.maximumZoomLevel());
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    QVERIFY(item.renderDetailDirectlyLocked(visibleStart, visibleEnd));

    QImage baseline(400, 180, QImage::Format_RGB32);
    baseline.fill(Qt::black);
    QPainter baselinePainter(&baseline);
    item.paint(&baselinePainter);
    baselinePainter.end();

    QImage reused = baseline.copy();
    item.setHoverPosition(80.0, 40.0, true);
    QPainter firstPainter(&reused);
    item.paint(&firstPainter);
    firstPainter.end();
    item.setHoverPosition(240.0, 110.0, true);
    QPainter secondPainter(&reused);
    item.paint(&secondPainter);
    secondPainter.end();

    for (int y = 4; y < 90; ++y) {
        QCOMPARE(reused.pixelColor(80, y), baseline.pixelColor(80, y));
    }
}

void WaveformSmokeTest::waveformEditorSampleViewUsesSmoothInterpolation() {
    const QPolygonF samples{
        QPointF(0.0, 90.0),
        QPointF(10.0, 20.0),
        QPointF(20.0, 160.0),
        QPointF(30.0, 90.0),
    };

    const QPainterPath path = WaveformEditorItem::buildSamplePath(samples);

    QCOMPARE(path.elementCount(), 10);
    QCOMPARE(path.elementAt(1).type, QPainterPath::CurveToElement);
    QCOMPARE(path.elementAt(4).type, QPainterPath::CurveToElement);
    QCOMPARE(path.currentPosition(), samples.constLast());
}

void WaveformSmokeTest::waveformEditorReclampsZoomForDecodedSampleRate() {
    WaveformEditorItem item;
    item.setWidth(100);
    item.setDurationSeconds(100.0);
    item.setZoomLevel(item.maximumZoomLevel());
    QVERIFY(item.zoomLevel() > 100'000.0);

    item.m_sampleRateHz = 1'000;
    QVERIFY(item.clampZoomToMaximumLocked());

    QCOMPARE(item.zoomLevel(), 8'000.0);
    QVERIFY(item.m_cacheDirty);
}

void WaveformSmokeTest::waveformEditorRulersFollowGridSetting() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(255)));
    const auto countRulerPixels = [&item]() {
        QImage canvas(320, 180, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        QPainter painter(&canvas);
        item.paint(&painter);
        painter.end();
        int rulerPixels = 0;
        for (int y = 0; y < canvas.height(); ++y) {
            for (int x = 260; x < canvas.width(); ++x) {
                const QColor pixel(canvas.pixel(x, y));
                const int maximum = std::max({pixel.red(), pixel.green(), pixel.blue()});
                const int minimum = std::min({pixel.red(), pixel.green(), pixel.blue()});
                if (minimum > 80 && maximum - minimum < 40) ++rulerPixels;
            }
        }
        return rulerPixels;
    };

    item.setGridEnabled(false);
    const int hiddenRulerPixels = countRulerPixels();
    item.setGridEnabled(true);
    const int visibleRulerPixels = countRulerPixels();

    QVERIFY(hiddenRulerPixels < 10);
    QVERIFY(visibleRulerPixels > hiddenRulerPixels + 20);
}

void WaveformSmokeTest::waveformEditorRulersRemainVisibleWhenZoomed() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);
    item.m_detail.sampleRateHz = 1'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 10'000;
    item.m_detail.extrema.assign(20'000, 0.5F);
    item.setZoomLevel(4.0);
    item.setGridEnabled(true);

    QImage canvas(320, 180, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    int rulerPixels = 0;
    for (int y = 0; y < 160; ++y) {
        for (int x = 290; x < 320; ++x) {
            const QColor pixel(canvas.pixel(x, y));
            const int maximum = std::max({pixel.red(), pixel.green(), pixel.blue()});
            const int minimum = std::min({pixel.red(), pixel.green(), pixel.blue()});
            if (minimum > 80 && maximum - minimum < 40) ++rulerPixels;
        }
    }
    QVERIFY(rulerPixels > 30);
}

void WaveformSmokeTest::waveformEditorPlayheadIsThinAndNeutral() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);

    QImage canvas(320, 180, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    const QColor playhead(canvas.pixel(160, 40));
    const QColor left(canvas.pixel(159, 40));
    const QColor right(canvas.pixel(161, 40));
    QVERIFY(std::abs(playhead.red() - playhead.green()) < 12);
    QVERIFY(playhead.red() > left.red());
    QCOMPARE(left, right);
}

void WaveformSmokeTest::waveformEditorOverviewRespectsCoverageAndChannelIdentity() {
    WaveformEditorItem item;
    item.setDurationSeconds(10.0);
    item.setOverviewCoverageSeconds(2.5);
    QByteArray peaks(1'000, 0);
    peaks[123] = static_cast<char>(255);
    item.setOverviewData(peaks);
    const auto render = [&](double start, double end, int channels) {
        QImage canvas(100, 100, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        QPainter painter(&canvas);
        item.drawOverviewLocked(painter, 100, 100, start, end, channels);
        painter.end();
        return canvas;
    };
    const auto partial = render(0.0, 10.0, 1);
    QVERIFY(partial.pixelColor(3, 10) != QColor(Qt::black));
    QCOMPARE(partial.pixelColor(12, 10), QColor(Qt::black));
    QCOMPARE(partial.pixelColor(70, 50), QColor(Qt::black));
    QCOMPARE(render(3.0, 4.0, 1).pixelColor(50, 50), QColor(Qt::black));
    item.m_cacheDirty = false;
    item.setOverviewComplete(true);
    QVERIFY(item.m_cacheDirty);
    const auto complete = render(0.0, 10.0, 1);
    QVERIFY(complete.pixelColor(12, 10) != QColor(Qt::black));
    item.setViewMode(1);
    const auto multichannel = render(0.0, 10.0, 2);
    QCOMPARE(multichannel.pixelColor(12, 10), QColor(Qt::black));
    QCOMPARE(multichannel.pixelColor(12, 60), QColor(Qt::black));
    item.setOverviewComplete(false);
    item.setOverviewCoverageSeconds(0.0);
    QCOMPARE(render(0.0, 10.0, 1).pixelColor(12, 50), QColor(Qt::black));
}

void WaveformSmokeTest::waveformEditorReferenceLineContrastsWaveform() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    QByteArray overview(1'024, static_cast<char>(0));
    std::fill(
        overview.begin() + overview.size() / 2,
        overview.end(),
        static_cast<char>(255));
    item.setOverviewComplete(true);
    item.setOverviewData(overview);

    QImage canvas(320, 180, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    const QColor background(canvas.pixel(80, 89));
    const QColor lineOnBackground(canvas.pixel(80, 90));
    const QColor waveform(canvas.pixel(240, 89));
    const QColor lineOnWaveform(canvas.pixel(240, 90));
    const auto luminance = [](const QColor &color) {
        return 0.2126 * color.red()
            + 0.7152 * color.green()
            + 0.0722 * color.blue();
    };
    QVERIFY(lineOnBackground != background);
    QVERIFY(luminance(lineOnWaveform) < luminance(waveform));
    QVERIFY(lineOnWaveform != waveform);
}

void WaveformSmokeTest::waveformEditorContrastLinesUseFboSafeCompositionModes() {
    CompositionRecordingPaintDevice device;
    QPainter painter(&device);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    WaveformEditorItem::drawContrastingLine(
        painter,
        QLine(8, 8, 56, 8),
        QColor(190, 190, 200, 150),
        QColor(8, 18, 14, 235));
    QCOMPARE(painter.compositionMode(), QPainter::CompositionMode_Source);
    painter.end();

    QVERIFY(!device.engine.compositionModes.isEmpty());
    for (const QPainter::CompositionMode mode
         : std::as_const(device.engine.compositionModes)) {
        QVERIFY2(
            mode <= QPainter::CompositionMode_Plus,
            "FBO waveform overlays must avoid advanced OpenGL blend modes");
    }
}

void WaveformSmokeTest::waveformEditorFpsOverlayTracksPaintRate() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setShowFpsOverlay(true);

    QImage canvas(320, 180, QImage::Format_RGB32);
    for (int frame = 0; frame < 50; ++frame) {
        canvas.fill(Qt::black);
        QPainter painter(&canvas);
        item.paint(&painter);
        painter.end();
        QTest::qWait(5);
    }

    QVERIFY(item.m_fpsValue > 0);
    int overlayPixels = 0;
    int rightmostOverlayPixel = -1;
    for (int y = 0; y < 20; ++y) {
        for (int x = 220; x < canvas.width(); ++x) {
            const QColor pixel(canvas.pixel(x, y));
            if (pixel.red() > 80 && pixel.green() > 80 && pixel.blue() > 80) {
                ++overlayPixels;
                rightmostOverlayPixel = std::max(rightmostOverlayPixel, x);
            }
        }
    }
    QVERIFY(overlayPixels > 10);
    QVERIFY(rightmostOverlayPixel >= canvas.width() - 12);
}

void WaveformSmokeTest::waveformPointerBringsTheControlsBackOverItsOverlays() {
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
    width: 640
    height: 320
    visible: true
    property int pointerMoves: 0

    QtObject {
        id: bridge
        objectName: "waveformBridge"
        property string playbackState: "Stopped"
        property string currentTrackPath: ""
        property var waveformPeaksPacked: ""
        property real waveformCoverageSeconds: 0
        property bool waveformComplete: false
        property real durationSeconds: 100
        property int spectrogramViewMode: 1
        property bool spectrogramZoomEnabled: true
        property bool stretchShortTracks: true
        property bool showSpectrogramScale: false
        property bool showSpectrogramCrosshair: true
        property bool showFps: false
        property int currentTrackChannels: 2
        property var currentTrackChannelLabels: []
        property var soloedChannels: []
        property int channelButtonsVisibility: 2
        property int muteCalls: 0
        property int soloCalls: 0
        property var mutedChannels: []
        property bool showLevelMeter: false
        property bool waveformEndsEarly: false
        property bool connected: true
        function isChannelSilent(channelIndex) { return false }
        function toggleChannelMute(channelIndex) { ++muteCalls }
        function soloChannel(channelIndex) { ++soloCalls }
    }

    Components.FullscreenControlsAutoHide {
        id: autoHide
        objectName: "autoHide"
        active: true
    }

    Components.UiPalette { id: palette; windowRoot: paletteRoot }

    function notePointer(x, y) {
        ++pointerMoves
        autoHide.pointerMoved(x, y)
    }

    Viewers.WaveformSurface {
        id: surface
        anchors.fill: parent
        plotWidth: layer.plotWidth
        uiBridge: bridge
        uiPalette: palette
        interactiveOverlaysVisible: autoHide.controlsVisible
        pointerActivity: paletteRoot.notePointer
    }

    // The labels and the channel buttons, as over the window's waveform.
    Viewers.PlotLayer {
        id: layer
        anchors.fill: parent
        uiBridge: bridge
        uiPalette: palette
        visualizationMode: 1
        controlsVisible: autoHide.controlsVisible
        pointerActivity: paletteRoot.notePointer
        laneCount: surface.laneCount
        laneLabels: surface.laneLabels
        hoveredLane: surface.hoveredLane
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.data());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QObject *autoHide = root->findChild<QObject *>(QStringLiteral("autoHide"));
    QObject *bridge = root->findChild<QObject *>(QStringLiteral("waveformBridge"));
    auto *waveform = root->findChild<WaveformEditorItem *>(QStringLiteral("waveformEditorItem"));
    QVERIFY(autoHide != nullptr && bridge != nullptr && waveform != nullptr);
    QTRY_COMPARE(waveform->channelCount(), 2);
    QVERIFY(waveform->crosshairEnabled());
    QObject *hoverHandler = root->findChild<QObject *>(
        QStringLiteral("waveformSurfaceHoverHandler"));
    QVERIFY(hoverHandler != nullptr);

    const auto keepControlsShown = [&]() {
        autoHide->setProperty("hideDelay", 5000);
        QTRY_VERIFY(autoHide->property("controlsVisible").toBool());
    };
    // Hides the controls, then moves the pointer to `at` and sees them
    // return. A tool button there takes the hover from the waveform, as a
    // button does: `onButton`.
    const auto hideThenMoveTo = [&](QPoint at, bool onButton = false) {
        autoHide->setProperty("hideDelay", 1);
        QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
        QTRY_VERIFY(!waveform->crosshairEnabled());
        QSignalSpy controlsChanged(autoHide, SIGNAL(controlsVisibleChanged()));
        QTest::mouseMove(window, at);
        QTRY_VERIFY(!controlsChanged.isEmpty());
        keepControlsShown();
        QTRY_VERIFY(waveform->crosshairEnabled());
        if (onButton) {
            return;
        }
        QTRY_VERIFY(waveform->m_hoverActive);
        QCOMPARE(waveform->mapToScene(waveform->m_hoverPosition).toPoint(), at);
        QVERIFY(hoverHandler->property("hovered").toBool());
    };
    const auto center = [](const QQuickItem *item) { return sceneRectOf(item).center().toPoint(); };
    // Whether a button shows: its plate is in the scene and opaque. Hidden
    // on hover, a plate stays in the scene for the keyboard.
    const auto shows = [](QQuickItem *button) {
        return button->isVisible() && button->parentItem()->parentItem()->opacity() == 1.0;
    };
    // The plate lays out its returned buttons as it next polishes, and a
    // click outside the plate does not reach them.
    const auto waitForRowLaidOut = [&](QQuickItem *mute, QQuickItem *solo) {
        QTRY_VERIFY(shows(mute) && shows(solo));
        QTRY_VERIFY(sceneRectOf(solo->parentItem()).contains(sceneRectOf(mute))
                    && sceneRectOf(solo->parentItem()).contains(sceneRectOf(solo)));
    };

    // Over the waveform, on every channel's row.
    const QRectF view = sceneRectOf(waveform);
    for (const int row : {0, 1}) {
        const QRectF rowRect(view.x(), view.y() + row * view.height() / 2,
                             view.width(), view.height() / 2);
        hideThenMoveTo(rowRect.center().toPoint() + QPoint(row, 0));
        if (QTest::currentTestFailed()) {
            return;
        }
    }

    // What the crosshair draws goes away with the controls and returns with them.
    const auto painted = [&]() {
        QImage image(waveform->size().toSize(), QImage::Format_RGB32);
        image.fill(Qt::black);
        QPainter painter(&image);
        waveform->paint(&painter);
        return image;
    };
    const QImage withCrosshair = painted();
    autoHide->setProperty("hideDelay", 1);
    QTRY_VERIFY(!waveform->crosshairEnabled());
    QVERIFY(painted() != withCrosshair);
    keepControlsShown();
    QTRY_VERIFY(waveform->crosshairEnabled());
    QTRY_COMPARE(painted(), withCrosshair);

    // On a channel's label, which stays while the controls are hidden.
    QQuickItem *label = firstVisibleItemNamed(
        window->contentItem(), QStringLiteral("plotLaneLabel"));
    QVERIFY(label != nullptr);
    hideThenMoveTo(center(label));
    if (QTest::currentTestFailed()) {
        return;
    }

    // Where a button shows when the controls return, and then on it: the
    // pointer keeps the controls up, and the button still takes the click.
    QQuickItem *solo = firstVisibleItemNamed(
        window->contentItem(), QStringLiteral("plotLaneSoloButton"));
    QQuickItem *mute = firstVisibleItemNamed(
        window->contentItem(), QStringLiteral("plotLaneMuteButton"));
    QVERIFY(solo != nullptr && mute != nullptr);
    waitForRowLaidOut(mute, solo);
    const QPoint onSolo = center(solo);
    const QPoint onMute = center(mute);
    hideThenMoveTo(onSolo, true);
    if (QTest::currentTestFailed()) {
        return;
    }
    waitForRowLaidOut(mute, solo);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, onSolo);
    QTRY_COMPARE(bridge->property("soloCalls").toInt(), 1);
    hideThenMoveTo(onMute, true);
    if (QTest::currentTestFailed()) {
        return;
    }
    waitForRowLaidOut(mute, solo);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, onMute);
    QTRY_COMPARE(bridge->property("muteCalls").toInt(), 1);
    QCOMPARE(bridge->property("soloCalls").toInt(), 1);

    // A pointer moving on a button still counts as moving.
    const int movesBefore = root->property("pointerMoves").toInt();
    QTest::mouseMove(window, onSolo + QPoint(1, 0));
    QTRY_VERIFY(root->property("pointerMoves").toInt() > movesBefore);

    // Shown on hover, a row's buttons follow the pointer from row to row,
    // and stay for the click while the pointer is on them.
    bridge->setProperty("channelButtonsVisibility", 1);
    const auto shownSolos = [&]() {
        int shown = 0;
        for (QQuickItem *button : visibleItemsNamed(window->contentItem(), QStringLiteral("plotLaneSoloButton"))) {
            shown += shows(button) ? 1 : 0;
        }
        return shown;
    };
    QTRY_COMPARE(shownSolos(), 1);
    QVERIFY(shows(solo));
    QTest::mouseMove(window, QPoint(onSolo.x(), static_cast<int>(view.bottom()) - 2));
    QTRY_VERIFY(!shows(solo));
    QTRY_COMPARE(shownSolos(), 1);
    QTest::mouseMove(window, QPoint(onSolo.x(), static_cast<int>(view.top()) + 2));
    QTRY_VERIFY(shows(solo));
    waitForRowLaidOut(mute, solo);
    QTest::mouseMove(window, onSolo);
    QVERIFY(shows(solo));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, onSolo);
    QTRY_COMPARE(bridge->property("soloCalls").toInt(), 2);
    QVERIFY(shows(solo));
    bridge->setProperty("channelButtonsVisibility", 2);

    // The controls do not depend on the crosshair being on.
    bridge->setProperty("showSpectrogramCrosshair", false);
    QTRY_VERIFY(!waveform->crosshairEnabled());
    autoHide->setProperty("hideDelay", 1);
    QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
    QSignalSpy controlsChanged(autoHide, SIGNAL(controlsVisibleChanged()));
    QTest::mouseMove(window, view.center().toPoint() + QPoint(7, 0));
    QTRY_VERIFY(!controlsChanged.isEmpty());
    keepControlsShown();

    // Mixed, there are no channel rows over the waveform.
    bridge->setProperty("showSpectrogramCrosshair", true);
    bridge->setProperty("spectrogramViewMode", 0);
    QTRY_COMPARE(waveform->channelCount(), 1);
    QTRY_VERIFY(firstVisibleItemNamed(
                    window->contentItem(), QStringLiteral("plotLaneLabel")) == nullptr);
    hideThenMoveTo(view.center().toPoint() + QPoint(9, 0));
    if (QTest::currentTestFailed()) {
        return;
    }

    // The pointer goes with the controls and returns with them.
    QCOMPARE(window->cursor().shape(), Qt::ArrowCursor);
    autoHide->setProperty("hideDelay", 1);
    QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
    QTRY_COMPARE(window->cursor().shape(), Qt::BlankCursor);
    QTest::mouseMove(window, view.center().toPoint() + QPoint(11, 0));
    keepControlsShown();
    QTRY_COMPARE(window->cursor().shape(), Qt::ArrowCursor);

    // A touch ends without a hover leave, and must not leave the crosshair.
    QPointingDevice *touchScreen = QTest::createTouchDevice();
    const QPoint touched = waveform->mapToScene(
        QPointF(waveform->width() / 2, waveform->height() / 2)).toPoint();
    QTest::touchEvent(window, touchScreen).press(0, touched, window);
    QTRY_VERIFY(hoverHandler->property("hovered").toBool());
    QTest::touchEvent(window, touchScreen).release(0, touched, window);
    QTRY_VERIFY(!hoverHandler->property("hovered").toBool());
    QTRY_VERIFY(!waveform->m_hoverActive);
}

void WaveformSmokeTest::pointerOverTheViewBringsTheViewerControlsBack() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *autoHide =
        window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    auto *spectrogramSurface =
        window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && autoHide != nullptr
            && waveformSurface != nullptr && spectrogramSurface != nullptr);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    QObject *hoverHandler = waveformSurface->findChild<QObject *>(
        QStringLiteral("waveformSurfaceHoverHandler"));
    QVERIFY(hoverHandler != nullptr);
    bridge->setProperty("showSpectrogramCrosshair", true);
    bridge->setProperty("durationSeconds", 100.0);
    bridge->setProperty("channelButtonsVisibility", 2);

    // Docked, nothing hides, and the crosshair and seek work as ever, up to
    // the pointer leaving the view.
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(waveformSurface->isVisible());
    QTRY_VERIFY(waveform->crosshairEnabled());
    QVERIFY(!autoHide->property("active").toBool());
    int nudge = 0;
    for (const int viewMode : {0, 1}) {
        bridge->setProperty("spectrogramViewMode", viewMode);
        QTRY_COMPARE(waveform->viewMode(), viewMode);
        const QRectF view = sceneRectOf(waveform);
        const QPoint docked = view.center().toPoint() + QPoint(++nudge, 0);
        QTest::mouseMove(window, docked);
        QTRY_VERIFY(waveform->m_hoverActive);
        QCOMPARE(waveform->mapToScene(waveform->m_hoverPosition).toPoint(), docked);
        QVERIFY(autoHide->property("controlsVisible").toBool());
        QSignalSpy dockedSeeks(waveform, &WaveformEditorItem::seekRequested);
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, docked);
        QTRY_COMPARE(dockedSeeks.count(), 1);
        const QPoint outside(docked.x(), static_cast<int>(view.top()) - 4);
        QVERIFY(!view.contains(outside));
        QTest::mouseMove(window, outside);
        QTRY_VERIFY(!waveform->m_hoverActive);
    }

    for (const int fullscreen : {1, 0}) {
        bridge->setProperty("viewerFullscreenMode", fullscreen);
        QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
        QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
        QQuickWindow *host = window;
        if (fullscreen == 1) {
            QTRY_VERIFY(waveformSurface->window() != nullptr
                        && waveformSurface->window() != window);
            host = waveformSurface->window();
            QVERIFY(QTest::qWaitForWindowExposed(host));
        } else {
            QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
            QVERIFY(popup != nullptr);
            QTRY_VERIFY(popup->property("opened").toBool());
        }
        // Over the whole screen, all of it: the title strip lies over it.
        QTRY_VERIFY(qFuzzyCompare(waveformSurface->height(), host->contentItem()->height()));
        QVERIFY(autoHide->property("active").toBool());

        for (const int visualization : {1, 0}) {
            viewer->setProperty("visualizationMode", visualization);
            QQuickItem *surface = visualization == 1 ? waveformSurface : spectrogramSurface;
            QTRY_VERIFY(surface->isVisible());
            for (const int viewMode : {0, 1}) {
                bridge->setProperty("spectrogramViewMode", viewMode);
                const QString where = QStringLiteral("fullscreen %1, visualization %2, layout %3")
                                          .arg(fullscreen).arg(visualization).arg(viewMode);
                autoHide->setProperty("hideDelay", 1);
                QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
                QTRY_VERIFY(!waveform->crosshairEnabled());

                QSignalSpy controlsChanged(autoHide, SIGNAL(controlsVisibleChanged()));
                const QPoint at = sceneRectOf(surface).center().toPoint() + QPoint(++nudge, 0);
                QTest::mouseMove(host, at);
                QTRY_VERIFY2(!controlsChanged.isEmpty(), qPrintable(where));
                autoHide->setProperty("hideDelay", 5000);
                QTRY_VERIFY(autoHide->property("controlsVisible").toBool());
                QTRY_VERIFY(waveform->crosshairEnabled());

                if (visualization == 1) {
                    QTRY_VERIFY(waveform->m_hoverActive);
                    QCOMPARE(waveform->mapToScene(waveform->m_hoverPosition).toPoint(), at);
                    QVERIFY(hoverHandler->property("hovered").toBool());
                    QSignalSpy seeks(waveform, &WaveformEditorItem::seekRequested);
                    QTest::mouseClick(host, Qt::RightButton, Qt::NoModifier, at);
                    QTRY_COMPARE(seeks.count(), 1);
                } else {
                    auto *spectrogram = qobject_cast<SpectrogramItem *>(firstVisibleItemNamed(
                        spectrogramSurface, QStringLiteral("spectrogramPaneItem")));
                    QVERIFY(spectrogram != nullptr);
                    QTRY_VERIFY(spectrogram->m_hoverActive);
                }
            }
        }

        // The double-click on the view that closes the viewer still gets there.
        viewer->setProperty("visualizationMode", 1);
        QTRY_VERIFY(waveformSurface->isVisible());
        const QPoint at = sceneRectOf(waveformSurface).center().toPoint();
        QTest::mouseMove(host, at);
        QTest::mouseDClick(host, Qt::LeftButton, Qt::NoModifier, at);
        QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
        QTRY_VERIFY(waveformSurface->window() == window);
    }
}

namespace {

// Whether an image has detail finer than `scale` pixels: some aligned
// block of that many pixels is not all one color.
bool hasDetailFinerThan(const QImage &image, const QRect &area, int scale) {
    for (int y = area.top() - area.top() % scale; y + scale <= area.bottom() + 1; y += scale) {
        for (int x = area.left() - area.left() % scale; x + scale <= area.right() + 1; x += scale) {
            const QRgb first = image.pixel(x, y);
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    if (image.pixel(x + dx, y + dy) != first) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

} // namespace

void WaveformSmokeTest::waveformEditorCachesAtDevicePixels() {
    QQuickWindow window;
    if (!showOnScreen(window, 2.0)) {
        QSKIP("Needs the offscreen platform's HiDPI test screens");
    }
    WaveformEditorItem &item = *new WaveformEditorItem(window.contentItem());
    item.setWidth(160);
    item.setHeight(90);
    item.setDurationSeconds(10.0);
    item.setOverviewComplete(true);
    QByteArray overview(4096, Qt::Uninitialized);
    for (int i = 0; i < overview.size(); ++i) {
        overview[i] = static_cast<char>(80 + 60 * std::sin(i * 0.05));
    }
    item.setOverviewData(overview);
    QImage target(QSize(320, 180), QImage::Format_RGB32);
    target.setDevicePixelRatio(2.0);
    {
        QPainter painter(&target);
        item.paint(&painter);
    }

    QMutexLocker lock(&item.m_stateMutex);
    // Device pixels in both directions: rows differ within pairs too.
    QCOMPARE(item.m_cache.height(), 180);
    QCOMPARE(item.m_cachedViewportWidth, 320);
    QCOMPARE(item.m_cachedViewportHeight, 180);
    bool rowsDiffer = false;
    for (int y = 0; y + 1 < item.m_cache.height() && !rowsDiffer; y += 2) {
        for (int x = 0; x < item.m_cache.width() && !rowsDiffer; ++x) {
            rowsDiffer = item.m_cache.pixel(x, y) != item.m_cache.pixel(x, y + 1);
        }
    }
    QVERIFY(rowsDiffer);

    // Playback tiles are too.
    item.m_playbackTilesInline = true;
    item.m_detail.sampleRateHz = 44'100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 88;
    item.m_detail.pointCount = 5'000;
    item.m_detail.extrema.assign(10'000, 0.25F);
    item.m_zoomLevel = 4.0;
    item.m_presentedZoomLevel = 4.0;
    item.m_playing = true;
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    item.preparePlaybackTilesLocked(visibleStart, visibleEnd, item.renderPixelHeightLocked());
    QVERIFY(item.renderMissingPlaybackTilesLocked(
        visibleStart, visibleEnd, item.renderPixelHeightLocked(), 2) > 0);
    QCOMPARE(item.m_playbackTileHeight, 180);
    for (const auto &[index, tile] : item.m_playbackTiles) {
        Q_UNUSED(index);
        QCOMPARE(tile.strips.size(), 1U);
        QCOMPARE(tile.strips.front().height(), 180);
    }
    QVERIFY(item.playbackTilesEligibleLocked(visibleStart, visibleEnd));
    for (const auto &paint : item.playbackTilePaintsLocked(visibleStart, visibleEnd, 160, 90)) {
        QCOMPARE(paint.target.height(), 90.0);
    }
}

void WaveformSmokeTest::waveformEditorScaleChangeRedrawsItsCacheOnce() {
    QScreen *screen = hiDpiTestScreen(2.0);
    if (screen == nullptr) {
        QSKIP("Needs the offscreen platform's HiDPI test screens");
    }
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    WaveformEditorItem &item = *new WaveformEditorItem(window.contentItem());
    item.setWidth(160);
    item.setHeight(90);
    item.setDurationSeconds(10.0);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_sampleRateHz = 1'000;
        item.m_channelCount = 1;
        item.m_detail.sampleRateHz = 1'000;
        item.m_detail.channelCount = 1;
        item.m_detail.startSeconds = 0.0;
        item.m_detail.endSeconds = 10.0;
        item.m_detail.framesPerPoint = 1;
        item.m_detail.pointCount = 10'000;
        item.m_detail.extrema.assign(20'000, 0.25F);
        item.rebuildCacheLocked(item.renderPixelWidthLocked(), item.renderPixelHeightLocked());
        QCOMPARE(item.m_cache.size(), QSize(160, 90));
        // A replacement is being staged at a scale of 1.
        item.beginStagedCacheLocked();
        QVERIFY(!item.m_stagedCache.isNull());
    }

    QVERIFY(moveWindowToScreen(window, screen));
    {
        QMutexLocker lock(&item.m_stateMutex);
        // The staged cache and the cache at the old scale are dropped.
        QVERIFY(item.m_stagedCache.isNull());
        QVERIFY(item.m_cacheDirty);
    }
    QImage target(QSize(320, 180), QImage::Format_RGB32);
    target.setDevicePixelRatio(2.0);
    {
        QPainter painter(&target);
        item.paint(&painter);
    }
    QMutexLocker lock(&item.m_stateMutex);
    QCOMPARE(item.m_cache.height(), 180);
    QCOMPARE(item.m_cachedViewportWidth, 320);
    QVERIFY(!item.m_cacheDirty);
    const QImage cache = item.m_cache;
    lock.unlock();
    // Painting again keeps it.
    {
        QPainter painter(&target);
        item.paint(&painter);
    }
    lock.relock();
    QVERIFY(item.m_cache.constBits() == cache.constBits());

    // A staged cache begun at one scale is not taken for another.
    item.beginStagedCacheLocked();
    item.m_stagedCacheViewportWidth = 160;
    while (!item.m_stagedCache.isNull() && item.m_stagedCacheNextX < item.m_stagedCache.width()) {
        item.advanceStagedCacheLocked();
    }
    item.advanceStagedCacheLocked();
    QVERIFY(item.m_cachedViewportWidth != item.renderPixelWidthLocked());
}

void WaveformSmokeTest::waveformItemCachesAtDevicePixels() {
    WaveformItem item;
    item.setWidth(200);
    item.setHeight(40);
    item.setDurationSeconds(10.0);
    item.setWaveformComplete(true);
    QByteArray peaks(1000, Qt::Uninitialized);
    for (int i = 0; i < peaks.size(); ++i) {
        peaks[i] = static_cast<char>(128 + 120 * std::sin(i * 0.09));
    }
    item.setPeaksData(peaks);

    for (const double scale : {2.0, 1.5}) {
        const QSize pixels(
            static_cast<int>(std::ceil(200 * scale)), static_cast<int>(std::ceil(40 * scale)));
        QImage target(pixels, QImage::Format_RGB32);
        target.setDevicePixelRatio(scale);
        target.fill(Qt::red);
        {
            QPainter painter(&target);
            item.paint(&painter);
        }
        // The cache has the target's pixels, which show it one to one.
        QCOMPARE(item.m_waveformCache.size(), pixels);
        QCOMPARE(item.m_waveformCache.devicePixelRatio(), scale);
        QCOMPARE(target.convertToFormat(QImage::Format_RGB32),
            item.m_waveformCache.convertToFormat(QImage::Format_RGB32));
        // Bars a device pixel wide, ending on device rows.
        if (scale == 2.0) {
            QVERIFY(hasDetailFinerThan(item.m_waveformCache, QRect(QPoint(0, 0), pixels), 2));
        }
    }

    // In a window at a scale of 2, from the item's own texture.
    QQuickWindow window;
    if (!showOnScreen(window, 2.0)) {
        QSKIP("Needs the offscreen platform's HiDPI test screens");
    }
    WaveformItem &shown = *new WaveformItem(window.contentItem());
    shown.setWidth(200);
    shown.setHeight(40);
    shown.setDurationSeconds(10.0);
    shown.setWaveformComplete(true);
    shown.setPeaksData(peaks);
    QTRY_COMPARE(shown.m_waveformCache.size(), QSize(400, 80));
}

std::unique_ptr<QmlSmokeTest> makeWaveformSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<WaveformSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_waveform.moc"
