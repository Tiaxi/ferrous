// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

// The spectrogram's controls and overlays, its crosshair and readout,
// and click-to-seek.
class SpectrogramSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void spectrogramWholeScreenControlsIgnoreDuplicateHoverPoints();
    void spectrogramFullscreenIdleHidesOverlaysButKeepsChannelMarkers();
    void spectrogramHoverChannelButtonsRemainClickable();
    void spectrogramSoloButtonsShowEverySoloedChannel();
    void spectrogramBackgroundsAreThePlotFloor();
    void testMutedChannelRendersGrayscale();
    void spectrogramForceFpsOverlayDoesNotOverrideQmlBinding();
    void spectrogramFpsOverlayToggleKeepsSceneNode();
    void spectrogramOverlaysKeepClearOfTheReservedCorner();
    void spectrogramCrosshairAndGridPropertiesAndHoverTracking();
    void spectrogramPixelToFrequency();
    void spectrogramOverlayBenchmark();
    void spectrogramCrosshairOverlayGeneratesOnHover();
    void spectrogramCrosshairOverlayMovesOnlyItsTimeLabel();
    void spectrogramCrosshairReadoutClampsInsideTheLane_data();
    void spectrogramCrosshairReadoutClampsInsideTheLane();
    void spectrogramCrosshairReadoutFitsShortLanes_data();
    void spectrogramCrosshairReadoutFitsShortLanes();
    void crosshairReadoutPlatesFollowThePointerSmoothly_data();
    void crosshairReadoutPlatesFollowThePointerSmoothly();
    void spectrogramCrosshairTimeKeepsClearOfTheLaneHead();
    void crosshairReadoutLeavesAPlateOutInNarrowPlots_data();
    void crosshairReadoutLeavesAPlateOutInNarrowPlots();
    void spectrogramCrosshairReadoutStaysReadableInNarrowLanes_data();
    void spectrogramCrosshairReadoutStaysReadableInNarrowLanes();
    void spectrogramCrosshairReadoutKeepsItsPlatesAsTheLevelChanges();
    void crosshairReadoutWidestWidthsFollowTheFont();
    void spectrogramSurfaceFollowsTheStretchedDefaultZoom();
    void spectrogramCrosshairWidestValueCoversRoundedHertz();
    void spectrogramCrosshairReadoutReadsEachView_data();
    void spectrogramCrosshairReadoutReadsEachView();
    void spectrogramCrosshairReadoutAcrossAGaplessBoundary();
    void spectrogramCrosshairTimeShowsOnTheLowestLane();
    void spectrogramCrosshairReadoutPlatesTakeThePlotColors();
    void spectrogramCrosshairReadoutShowsAfterEveryClick();
    void spectrogramGridOverlayGeneratesWhenEnabled();
    void spectrogramTimeScaleOfAStretchedShortTrackShowsTenths();
    void spectrogramOverlayDisabledProducesNullImage();
    void spectrogramOverlayDirtiedByGeometryChange();
    void spectrogramOverlayDirtiedByLogScaleChange();
    void spectrogramOverlayStalenessDetectsBinChange();
    void spectrogramOverlayRebuildsViaUpdatePaintNodeOnStaleInput();
    void spectrogramOverlayStalenessDetectsDisplayRangeChange();
    void spectrogramClickToSeekEmitsSignalWhenCrosshairEnabled();
    void spectrogramClickToSeekUsesCurrentPositionWhenCrosshairCacheIsStale();
    void spectrogramClickToSeekIgnoresLaggingDecodedTailInCenteredMode();
    void spectrogramClickToSeekMatchesTheHeldView_data();
    void spectrogramClickToSeekMatchesTheHeldView();
    void spectrogramClickToSeekSuppressedWhenCrosshairDisabled();
    void spectrogramLeftClickDoesNotSeek();
    void spectrogramClickToSeekDisabledInRollingMode();
    void spectrogramClickToSeekMatchesCrosshairAtDevicePixels();
    void spectrogramOverlaysDrawNoPaneSizedImages();
};

void SpectrogramSmokeTest::spectrogramWholeScreenControlsIgnoreDuplicateHoverPoints() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "components" as Components
import "viewers" as Viewers

ApplicationWindow {
    id: windowRoot
    width: 640
    height: 480
    visible: true

    Viewers.SpectrogramViewerShell {
        objectName: "spectrogramViewerShell"
        anchors.fill: parent
        windowRoot: windowRoot
        uiPalette: Components.UiPalette { windowRoot: windowRoot }
        viewerOpen: true
        useWholeScreenViewerMode: true
        titleText: "Ferrous"
        closeViewer: function() {}
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *autoHide = root->findChild<QObject *>(
        QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    QObject *shell = root->findChild<QObject *>(QStringLiteral("spectrogramViewerShell"));
    QObject *fullscreenCloseButton = root->findChild<QObject *>(
        QStringLiteral("spectrogramFullscreenCloseButton"));
    QVERIFY(autoHide != nullptr);
    QVERIFY(shell != nullptr);
    QVERIFY(fullscreenCloseButton != nullptr);
    QCOMPARE(autoHide->property("hideDelay").toInt(), 5000);
    QVERIFY(autoHide->property("active").toBool());
    QVERIFY(fullscreenCloseButton->property("visible").toBool());

    auto *hideTimer = autoHide->property("hideTimer").value<QObject *>();
    QVERIFY(hideTimer != nullptr);
    // The pointer where the test reports it, below the title strip, whose
    // hover reports where the pointer is as its controls come and go.
    auto *fullscreen = qobject_cast<QQuickWindow *>(
        root->findChild<QObject *>(QStringLiteral("spectrogramFullscreenWindow")));
    QVERIFY(fullscreen != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(fullscreen));
    QTest::mouseMove(fullscreen, QPoint(100, 80));
    autoHide->setProperty("hideDelay", 40);
    const auto reportPointerPosition = [autoHide](double x, double y) {
        return QMetaObject::invokeMethod(
            autoHide,
            "pointerMoved",
            Q_ARG(QVariant, QVariant::fromValue(x)),
            Q_ARG(QVariant, QVariant::fromValue(y)));
    };
    QVERIFY(reportPointerPosition(100.0, 80.0));
    QTRY_VERIFY(!fullscreenCloseButton->property("visible").toBool());

    // Hover ownership can change when a control disappears, producing a
    // duplicate point notification without physical cursor movement. It must
    // not wake the controls or restart the timer.
    QVERIFY(reportPointerPosition(100.0, 80.0));
    QVERIFY(!fullscreenCloseButton->property("visible").toBool());
    QVERIFY(!hideTimer->property("running").toBool());

    // A move shows them, and hides them again after the delay.
    QVERIFY(reportPointerPosition(101.0, 80.0));
    QVERIFY(fullscreenCloseButton->property("visible").toBool());
    QVERIFY(hideTimer->property("running").toBool());
    QCOMPARE(hideTimer->property("interval").toInt(), 40);
    QTRY_VERIFY(!fullscreenCloseButton->property("visible").toBool());

    shell->setProperty("viewerOpen", false);
    QVERIFY(!autoHide->property("active").toBool());
    QVERIFY(autoHide->property("controlsVisible").toBool());
}

void SpectrogramSmokeTest::spectrogramFullscreenIdleHidesOverlaysButKeepsChannelMarkers() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");

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
        property string playbackState: "Stopped"
        property int spectrogramViewMode: 1
        property int spectrogramDisplayMode: 0
        property real dbRange: 90
        property bool logScale: false
        property bool spectrogramFrequencyCap: false
        property bool showFps: false
        property int sampleRateHz: 48000
        property bool showSpectrogramCrosshair: true
        property bool showSpectrogramScale: true
        property bool spectrogramZoomEnabled: true
        property bool stretchShortTracks: true
        property int channelButtonsVisibility: 2
        property var mutedChannels: []
        property bool showLevelMeter: false
        property string currentTrackPath: ""
        property bool waveformEndsEarly: false
        property real waveformCoverageSeconds: 0
        property bool connected: true
        property real durationSeconds: 0
        property int currentTrackChannels: 0
        property var soloedChannels: []
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
        id: surface
        objectName: "spectrogramSurface"
        anchors.fill: parent
        plotWidth: layer.plotWidth
        uiBridge: bridge
        uiPalette: palette
    }

    // The labels and buttons are on the plot layer, as in the window.
    Viewers.PlotLayer {
        id: layer
        objectName: "plotLayer"
        anchors.fill: parent
        uiBridge: bridge
        uiPalette: palette
        controlsVisible: surface.interactiveOverlaysVisible
        laneCount: surface.laneCount
        laneLabels: surface.laneLabels
        hoveredLane: surface.hoveredLane
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *surface = root->findChild<QObject *>(QStringLiteral("spectrogramSurface"));
    QVERIFY(surface != nullptr);
    for (const int count : {1, 2, 3, 4, 5, 6, 7, 8, 10}) {
        QVariant descriptors;
        QVERIFY(QMetaObject::invokeMethod(surface, "descriptorsForChannelCount",
            Q_RETURN_ARG(QVariant, descriptors), Q_ARG(QVariant, count)));
        const QVariantList channels = descriptors.value<QJSValue>().toVariant().toList();
        QCOMPARE(channels.size(), count);
        for (int i = 0; i < count; ++i) {
            const QString expected = count == 1 ? QStringLiteral("M")
                : count == 2 ? (i == 0 ? QStringLiteral("L") : QStringLiteral("R"))
                : QString::number(i + 1);
            QCOMPARE(channels[i].toMap().value(QStringLiteral("label")).toString(), expected);
            QVERIFY(channels[i].toMap().value(QStringLiteral("showLabel")).toBool());
        }
    }
    const QVariantMap descriptor{
        {QStringLiteral("label"), QStringLiteral("L")},
        {QStringLiteral("showLabel"), true},
        {QStringLiteral("muted"), false},
        {QStringLiteral("channelIndex"), 0},
    };
    surface->setProperty("channelDescriptors", QVariantList{descriptor});

    QQuickItem *surfaceItem = qobject_cast<QQuickItem *>(surface);
    QVERIFY(surfaceItem != nullptr);
    QTRY_VERIFY(findQuickItemByObjectName(
                    surfaceItem, QStringLiteral("spectrogramPaneItem"))
                != nullptr);
    auto *spectrogramItem = qobject_cast<SpectrogramItem *>(findQuickItemByObjectName(
        surfaceItem, QStringLiteral("spectrogramPaneItem")));
    auto *layer = qobject_cast<QQuickItem *>(root->findChild<QObject *>(QStringLiteral("plotLayer")));
    QVERIFY(layer != nullptr);
    QTRY_VERIFY(findQuickItemByObjectName(layer, QStringLiteral("plotLaneButtons")) != nullptr);
    QQuickItem *channelMarker = findQuickItemByObjectName(layer, QStringLiteral("plotLaneLabel"));
    QQuickItem *muteButton = findQuickItemByObjectName(layer, QStringLiteral("plotLaneButtons"));
    QQuickItem *soloButton = findQuickItemByObjectName(layer, QStringLiteral("visualizationModeSwitch"));
    QQuickItem *pointerArea = findQuickItemByObjectName(
        surfaceItem, QStringLiteral("spectrogramSurfacePointerArea"));
    QVERIFY(spectrogramItem != nullptr);
    QVERIFY(channelMarker != nullptr);
    QVERIFY(muteButton != nullptr);
    QVERIFY(soloButton != nullptr);
    QVERIFY(pointerArea != nullptr);
    QTRY_VERIFY(channelMarker->property("visible").toBool());
    QTRY_VERIFY(muteButton->property("visible").toBool());
    QTRY_VERIFY(soloButton->property("visible").toBool());
    QTRY_VERIFY(spectrogramItem->crosshairEnabled());
    QCOMPARE(surface->property("pointerCursorShape").toInt(), static_cast<int>(Qt::ArrowCursor));
    QCOMPARE(pointerArea->property("cursorShape").toInt(), static_cast<int>(Qt::ArrowCursor));

    surface->setProperty("interactiveOverlaysVisible", false);
    QTRY_VERIFY(!muteButton->property("visible").toBool());
    QTRY_VERIFY(!soloButton->property("visible").toBool());
    QTRY_VERIFY(!spectrogramItem->crosshairEnabled());
    QVERIFY(channelMarker->property("visible").toBool());
    QCOMPARE(surface->property("pointerCursorShape").toInt(), static_cast<int>(Qt::BlankCursor));
    QCOMPARE(pointerArea->property("cursorShape").toInt(), static_cast<int>(Qt::BlankCursor));

    surface->setProperty("interactiveOverlaysVisible", true);
    QTRY_VERIFY(muteButton->property("visible").toBool());
    QTRY_VERIFY(soloButton->property("visible").toBool());
    QTRY_VERIFY(spectrogramItem->crosshairEnabled());
    QVERIFY(channelMarker->property("visible").toBool());
    QCOMPARE(surface->property("pointerCursorShape").toInt(), static_cast<int>(Qt::ArrowCursor));
    QCOMPARE(pointerArea->property("cursorShape").toInt(), static_cast<int>(Qt::ArrowCursor));
}

void SpectrogramSmokeTest::spectrogramHoverChannelButtonsRemainClickable() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");

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

    QtObject {
        id: bridge
        objectName: "channelButtonBridge"
        property string playbackState: "Stopped"
        property int spectrogramViewMode: 1
        property int spectrogramDisplayMode: 0
        property real dbRange: 90
        property bool logScale: false
        property bool spectrogramFrequencyCap: false
        property bool showFps: false
        property int sampleRateHz: 48000
        property bool showSpectrogramCrosshair: true
        property bool showSpectrogramScale: true
        property bool spectrogramZoomEnabled: true
        property bool stretchShortTracks: true
        property int channelButtonsVisibility: 1
        property var soloedChannels: []
        property int muteCalls: 0
        property bool showLevelMeter: false
        property string currentTrackPath: ""
        property bool waveformEndsEarly: false
        property real waveformCoverageSeconds: 0
        property bool connected: true
        property real durationSeconds: 0
        property int currentTrackChannels: 0
        property var mutedChannels: []
        property int soloCalls: 0
        signal precomputedSpectrogramChannelsReady(int channelCount, bool bufferReset)
        signal playbackChanged()
        function isChannelSilent(channelIndex) { return false }
        function setSpectrogramZoomLevel(level) {}
        function registerSpectrogramItem(item, channelIndex) {}
        function unregisterSpectrogramItem(item) {}
        function setSpectrogramWidgetWidth(width) {}
        function toggleChannelMute(channelIndex) { ++muteCalls }
        function soloChannel(channelIndex) { ++soloCalls }
    }

    Components.UiPalette { id: palette; windowRoot: paletteRoot }

    Viewers.SpectrogramSurface {
        id: surface
        objectName: "clickableSpectrogramSurface"
        anchors.fill: parent
        plotWidth: layer.plotWidth
        uiBridge: bridge
        uiPalette: palette
    }

    Viewers.PlotLayer {
        id: layer
        objectName: "plotLayer"
        anchors.fill: parent
        uiBridge: bridge
        uiPalette: palette
        laneCount: surface.laneCount
        laneLabels: surface.laneLabels
        hoveredLane: surface.hoveredLane
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    auto *window = qobject_cast<QQuickWindow *>(root.data());
    QVERIFY(window != nullptr);
    auto *surface = qobject_cast<QQuickItem *>(root->findChild<QObject *>(
        QStringLiteral("clickableSpectrogramSurface")));
    QObject *bridge = root->findChild<QObject *>(QStringLiteral("channelButtonBridge"));
    QVERIFY(surface != nullptr);
    QVERIFY(bridge != nullptr);
    surface->setProperty("channelDescriptors", QVariantList{QVariantMap{
        {QStringLiteral("label"), QStringLiteral("L")},
        {QStringLiteral("showLabel"), true},
        {QStringLiteral("muted"), false},
        {QStringLiteral("channelIndex"), 0},
    }});

    QQuickItem *layer = qobject_cast<QQuickItem *>(root->findChild<QObject *>(QStringLiteral("plotLayer")));
    QVERIFY(layer != nullptr);
    QQuickItem *buttons = nullptr;
    QQuickItem *muteButton = nullptr;
    QQuickItem *soloButton = nullptr;
    QTRY_VERIFY([&]() {
        buttons = findQuickItemByObjectName(layer, QStringLiteral("plotLaneButtons"));
        muteButton = findQuickItemByObjectName(layer, QStringLiteral("plotLaneMuteButton"));
        soloButton = findQuickItemByObjectName(layer, QStringLiteral("plotLaneSoloButton"));
        // The buttons are 8 px right of the lane's label.
        return buttons != nullptr && muteButton != nullptr && soloButton != nullptr
            && muteButton->mapToScene(QPointF(0, 0)).x() > 20.0;
    }());
    // Hidden, the buttons stay in the scene, for the keyboard.
    QVERIFY(buttons->isVisible());
    QCOMPARE(buttons->opacity(), 0.0);

    // Hover the pane to reveal the controls, then move onto each control.
    // The plate's own hover keeps it shown until the click is released.
    QTest::mouseMove(window, QPoint(320, 160));
    QTRY_COMPARE(buttons->opacity(), 1.0);
    const QPoint muteCenter = muteButton->mapToScene(QPointF(
        muteButton->width() * 0.5, muteButton->height() * 0.5)).toPoint();
    QTest::mouseMove(window, muteCenter);
    QTRY_VERIFY(muteButton->property("hovered").toBool());
    QCOMPARE(buttons->opacity(), 1.0);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, muteCenter);
    QTRY_COMPARE(bridge->property("muteCalls").toInt(), 1);

    const QPoint soloCenter = soloButton->mapToScene(QPointF(
        soloButton->width() * 0.5, soloButton->height() * 0.5)).toPoint();
    QTest::mouseMove(window, soloCenter);
    QTRY_VERIFY(soloButton->property("hovered").toBool());
    QCOMPARE(buttons->opacity(), 1.0);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, soloCenter);
    QTRY_COMPARE(bridge->property("soloCalls").toInt(), 1);

    // Off the pane they hide again; keyboard focus on either shows them,
    // and they stay while it is inside.
    QTest::mouseMove(window, QPoint(320, 310));
    QTest::mouseMove(window, QPoint(-10, -10));
    QTRY_COMPARE(buttons->opacity(), 0.0);
    muteButton->forceActiveFocus(Qt::TabFocusReason);
    QTRY_COMPARE(buttons->opacity(), 1.0);
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(soloButton->hasActiveFocus());
    QCOMPARE(buttons->opacity(), 1.0);
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_COMPARE(bridge->property("soloCalls").toInt(), 2);
    soloButton->setFocus(false);
    layer->forceActiveFocus();
    QTRY_COMPARE(buttons->opacity(), 0.0);
}

// The pane, each lane and the viewer are the spectrogram's floor colour,
// the same in both themes, rather than blacks of their own.
void SpectrogramSmokeTest::spectrogramBackgroundsAreThePlotFloor() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import "components" as Components
import "panes" as Panes
import "viewers" as Viewers

ApplicationWindow {
    id: window
    width: 640
    height: 480
    visible: true
    palette.window: "#eff0f1"
    palette.base: "#ffffff"

    Components.UiPalette {
        id: tokens
        objectName: "tokens"
        windowRoot: window
    }

    QtObject {
        id: bridge
        objectName: "floorBridge"
        property string playbackState: "Stopped"
        property var mutedChannels: []
        property int spectrogramViewMode: 1
        property int spectrogramDisplayMode: 0
        property real dbRange: 90
        property bool logScale: false
        property bool spectrogramFrequencyCap: false
        property bool showFps: false
        property int sampleRateHz: 48000
        property bool showSpectrogramCrosshair: true
        property bool showSpectrogramScale: true
        property bool spectrogramZoomEnabled: true
        property bool stretchShortTracks: true
        property int channelButtonsVisibility: 2
        property var soloedChannels: []
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

    Panes.SpectrogramPane {
        objectName: "floorPane"
        width: 320
        height: 200
        openViewer: function() {}
        uiPalette: tokens

        Viewers.SpectrogramSurface {
            objectName: "floorSurface"
            anchors.fill: parent
            uiBridge: bridge
            uiPalette: tokens
        }
    }

    Viewers.SpectrogramViewerShell {
        objectName: "floorShell"
        anchors.fill: parent
        windowRoot: window
        uiPalette: tokens
        viewerOpen: false
        useWholeScreenViewerMode: false
        titleText: "Ferrous"
        closeViewer: function() {}
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *tokens = root->findChild<QObject *>(QStringLiteral("tokens"));
    auto *pane = root->findChild<QQuickItem *>(QStringLiteral("floorPane"));
    auto *surface = root->findChild<QQuickItem *>(QStringLiteral("floorSurface"));
    QVERIFY(tokens != nullptr && pane != nullptr && surface != nullptr);
    // The plot's floor is the colour map's: its lowest entry, for the colour
    // and the greyscale (muted) map alike, which silence is drawn in.
    QObject *plot = tokens->property("spectrogramPlot").value<QObject *>();
    QVERIFY(plot != nullptr);
    const QColor floor = plot->property("plotBg").value<QColor>();
    QCOMPARE(plot->property("plotOutline").value<QColor>(), floor);
    for (const bool muted : {false, true}) {
        SpectrogramItem item;
        QCOMPARE(QColor(item.m_palette32.back()), floor);
        QCOMPARE(QColor(item.m_palette32Gray.back()), floor);
        item.setWidth(200);
        item.setHeight(100);
        item.setDisplayMode(1);
        item.setPlaying(false);
        item.setChannelMuted(muted);
        constexpr int bins = 8;
        item.feedPrecomputedChunk(
            QByteArray(bins * 100, '\0'), bins, 0, 100,
            0, 400, 48000, 1024, false, true, 1, false, 1);
        QSGNode *node = paintSettled(item, nullptr);
        // Silence, and the canvas past the columns, are the floor.
        QCOMPARE(QColor(item.m_canvas.pixel(50, 50)), floor);
        QCOMPARE(QColor(item.m_canvas.pixel(170, 50)), floor);
        delete node;
    }

    QCOMPARE(pane->property("color").value<QColor>(), floor);
    QVariantList descriptors;
    for (int channel = 0; channel < 2; ++channel) {
        descriptors.push_back(QVariantMap{
            {QStringLiteral("label"), QString::number(channel)},
            {QStringLiteral("showLabel"), true},
            {QStringLiteral("muted"), false},
            {QStringLiteral("channelIndex"), channel},
        });
    }
    surface->setProperty("channelDescriptors", descriptors);
    // The lanes are a Repeater's delegates: items under the surface, not
    // its QObject children.
    const std::function<void(QQuickItem *, QList<QQuickItem *> *)> collectLanes =
        [&collectLanes](QQuickItem *item, QList<QQuickItem *> *found) {
            if (item->objectName() == QStringLiteral("spectrogramLaneFloor")) {
                found->push_back(item);
            }
            for (QQuickItem *child : item->childItems()) {
                collectLanes(child, found);
            }
        };
    QList<QQuickItem *> lanes;
    QTRY_VERIFY((lanes.clear(), collectLanes(surface, &lanes), lanes.size() == 2));
    for (QQuickItem *lane : std::as_const(lanes)) {
        QCOMPARE(lane->property("color").value<QColor>(), floor);
    }
    QObject *popupFloor = root->findChild<QObject *>(QStringLiteral("spectrogramPopupFloor"));
    QVERIFY(popupFloor != nullptr);
    QCOMPARE(popupFloor->property("color").value<QColor>(), floor);

    // As drawn: full bleed, with no border; the empty plot, the item's own
    // canvas included, is the floor; the lanes meet at a 1 px seam.
    auto *window = qobject_cast<QQuickWindow *>(root.data());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    QTest::qWait(100);
    const QImage image = window->grabWindow();
    const double scale = double(image.width()) / window->width();
    const auto drawn = [&](double x, double y) {
        return image.pixelColor(int(x * scale), int(y * scale));
    };
    QCOMPARE(drawn(0.5, 50), floor);
    QCOMPARE(drawn(319.5, 50), floor);
    QCOMPARE(drawn(160, 0.5), floor);
    QCOMPARE(drawn(160, 50), floor);
    QCOMPARE(drawn(160, 150), floor);
    QCOMPARE(drawn(160, 100.5), plot->property("plotLaneSeam").value<QColor>());
}

void SpectrogramSmokeTest::spectrogramSoloButtonsShowEverySoloedChannel() {
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
        objectName: "soloBridge"
        property string playbackState: "Stopped"
        property var mutedChannels: []
        property int spectrogramViewMode: 1
        property int spectrogramDisplayMode: 0
        property real dbRange: 90
        property bool logScale: false
        property bool spectrogramFrequencyCap: false
        property bool showFps: false
        property int sampleRateHz: 48000
        property bool showSpectrogramCrosshair: true
        property bool showSpectrogramScale: true
        property bool spectrogramZoomEnabled: true
        property bool stretchShortTracks: true
        property int channelButtonsVisibility: 2
        property var soloedChannels: []
        property bool showLevelMeter: false
        property string currentTrackPath: ""
        property bool waveformEndsEarly: false
        property real waveformCoverageSeconds: 0
        property bool connected: true
        property real durationSeconds: 0
        property int currentTrackChannels: 0
        signal precomputedSpectrogramChannelsReady(int channelCount, bool bufferReset)
        signal playbackChanged()
        onMutedChannelsChanged: playbackChanged()
        onSoloedChannelsChanged: playbackChanged()
        function isChannelSilent(channelIndex) {
            const soloing = soloedChannels.indexOf(true) >= 0
            return mutedChannels[channelIndex] === true
                || (soloing && soloedChannels[channelIndex] !== true)
        }
        function setSpectrogramZoomLevel(level) {}
        function registerSpectrogramItem(item, channelIndex) {}
        function unregisterSpectrogramItem(item) {}
        function setSpectrogramWidgetWidth(width) {}
        function toggleChannelMute(channelIndex) {}
        function soloChannel(channelIndex) {}
    }

    Components.UiPalette { id: palette; windowRoot: paletteRoot }

    Viewers.SpectrogramSurface {
        id: surface
        objectName: "soloSpectrogramSurface"
        anchors.fill: parent
        uiBridge: bridge
        uiPalette: palette
    }

    Viewers.PlotLayer {
        objectName: "soloPlotLayer"
        anchors.fill: parent
        uiBridge: bridge
        uiPalette: palette
        laneCount: surface.laneCount
        laneLabels: surface.laneLabels
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *surface = qobject_cast<QQuickItem *>(root->findChild<QObject *>(
        QStringLiteral("soloSpectrogramSurface")));
    QObject *bridge = root->findChild<QObject *>(QStringLiteral("soloBridge"));
    auto *layer = qobject_cast<QQuickItem *>(root->findChild<QObject *>(
        QStringLiteral("soloPlotLayer")));
    QVERIFY(surface != nullptr);
    QVERIFY(layer != nullptr);
    QVERIFY(bridge != nullptr);
    QVariantList descriptors;
    for (int channel = 0; channel < 3; ++channel) {
        descriptors.push_back(QVariantMap{
            {QStringLiteral("label"), QString::number(channel)},
            {QStringLiteral("showLabel"), true},
            {QStringLiteral("muted"), false},
            {QStringLiteral("channelIndex"), channel},
        });
    }
    surface->setProperty("channelDescriptors", descriptors);
    bridge->setProperty("soloedChannels", QVariantList{true, false, true});

    // Whether each channel's button shows it active, in pane order.
    const std::function<void(QQuickItem *, const QString &, QList<bool> *)> collect =
        [&collect](QQuickItem *item, const QString &button, QList<bool> *shown) {
            if (item->objectName() == button) {
                shown->push_back(item->property("checked").toBool());
            }
            for (QQuickItem *child : item->childItems()) {
                collect(child, button, shown);
            }
        };
    const auto soloShown = [&]() {
        QList<bool> shown;
        collect(layer, QStringLiteral("plotLaneSoloButton"), &shown);
        return shown;
    };
    const auto muteShown = [&]() {
        QList<bool> shown;
        collect(layer, QStringLiteral("plotLaneMuteButton"), &shown);
        return shown;
    };
    // Whether each pane is greyed out, in pane order.
    const std::function<void(QQuickItem *, QList<bool> *)> collectGrey =
        [&collectGrey](QQuickItem *item, QList<bool> *grey) {
            if (item->objectName() == QStringLiteral("spectrogramPaneItem")) {
                grey->push_back(item->property("channelMuted").toBool());
            }
            for (QQuickItem *child : item->childItems()) {
                collectGrey(child, grey);
            }
        };
    const auto greyShown = [&]() {
        QList<bool> grey;
        collectGrey(surface, &grey);
        return grey;
    };
    QTRY_COMPARE(soloShown(), (QList<bool>{true, false, true}));
    bridge->setProperty("soloedChannels", QVariantList{false, false, true});
    QTRY_COMPARE(soloShown(), (QList<bool>{false, false, true}));
    bridge->setProperty("soloedChannels", QVariantList{});
    QTRY_COMPARE(soloShown(), (QList<bool>{false, false, false}));

    // A channel can show both, and a solo lights no mute button.
    bridge->setProperty("soloedChannels", QVariantList{true, false, false});
    bridge->setProperty("mutedChannels", QVariantList{true, false, false});
    QTRY_COMPARE(soloShown(), (QList<bool>{true, false, false}));
    QTRY_COMPARE(muteShown(), (QList<bool>{true, false, false}));
    QTRY_COMPARE(greyShown(), (QList<bool>{true, true, true}));

    // A change of the solos alone greys out the panes they leave out.
    bridge->setProperty("mutedChannels", QVariantList{});
    QTRY_COMPARE(greyShown(), (QList<bool>{false, true, true}));
    bridge->setProperty("soloedChannels", QVariantList{false, true, false});
    QTRY_COMPARE(greyShown(), (QList<bool>{true, false, true}));
    bridge->setProperty("soloedChannels", QVariantList{});
    QTRY_COMPARE(greyShown(), (QList<bool>{false, false, false}));
}

void SpectrogramSmokeTest::spectrogramForceFpsOverlayDoesNotOverrideQmlBinding() {
    // forceFpsOverlay is a CONSTANT property set at construction time from
    // the FERROUS_PROFILE_UI env var.  The setter for showFpsOverlay must
    // NOT OR in the force flag — QML's index===0 gate must be respected.
    SpectrogramItem item;
    QCOMPARE(item.forceFpsOverlay(), false); // no env var in test

    // Explicitly setting showFpsOverlay to false must stay false,
    // not be overridden by the force flag.
    item.setShowFpsOverlay(true);
    QCOMPARE(item.showFpsOverlay(), true);
    item.setShowFpsOverlay(false);
    QCOMPARE(item.showFpsOverlay(), false);
}

void SpectrogramSmokeTest::spectrogramFpsOverlayToggleKeepsSceneNode() {
    QQuickWindow window;
    window.resize(400, 100);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    prepareWholeTrackPlayback(item, 50.0);
    item.setPlaying(false);
    QSGNode *node = paintSettled(item, nullptr);
    QVERIFY(SpectrogramItem::sceneStatistics(node).canvasTextureBytes > 0);
    for (const bool shown : {true, false}) {
        item.setShowFpsOverlay(shown);
        {
            QMutexLocker lock(&item.m_stateMutex);
            item.m_fpsValue = 60;
            item.m_overlayDirty = true;
        }
        // The overlay comes and goes; the canvas's textures stay.
        QSGNode *next = item.updatePaintNode(node, nullptr);
        QCOMPARE(next, node);
        QCOMPARE(SpectrogramItem::sceneStatistics(node).canvasTextures, 0);
        QCOMPARE(SpectrogramItem::sceneStatistics(node).canvasUploads, 0);
        auto *overlay = static_cast<QSGSimpleTextureNode *>(node->lastChild());
        QCOMPARE(!overlay->rect().isEmpty(), shown);
    }
    delete node;
}

void SpectrogramSmokeTest::spectrogramOverlaysKeepClearOfTheReservedCorner() {
    QQuickWindow window;
    window.resize(400, 100);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    prepareWholeTrackPlayback(item, 50.0);
    item.setPlaying(false);
    item.setGridEnabled(true);
    item.setCrosshairEnabled(true);
    item.setShowFpsOverlay(true);
    QSGNode *node = paintSettled(item, nullptr);
    const int width = static_cast<int>(item.width());
    const int height = static_cast<int>(item.height());
    const QSizeF reserved(120.0, 40.0);
    const QRect corner(width - 120, 0, 120, 40);
    const auto layOut = [&](double pointerY) {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(height);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(20.0, pointerY);
        item.updateFreqGridOverlayLocked(width, height);
        item.updateCrosshairOverlayLocked(width, height, 0, false, 46.875, 0.0);
    };

    // The frequency labels keep 34 px clear of the lane's top, where the
    // mode switch and the lane's label are, wherever the corner is; they
    // end 10 px from the edge, each beside its 6 px tick.
    for (const QSizeF cornerSize : {QSizeF(), reserved}) {
        item.setReservedCorner(cornerSize);
        layOut(5.0);
        QVERIFY(!item.m_freqGrid.images.isEmpty());
        QCOMPARE(item.m_freqGrid.images.size(), item.m_freqGrid.lines.size());
        for (const SpectrogramOverlayImage &image : item.m_freqGrid.images) {
            QVERIFY2(image.rect.top() >= 34.0, qPrintable(image.text));
            QCOMPARE(image.rect.right(), width - 10.0);
            QVERIFY(image.text.endsWith(QStringLiteral("Hz")));
        }
        for (const QRect &line : item.m_freqGrid.lines) {
            QCOMPARE(line.width(), 6);
            QCOMPARE(line.right(), width - 1);
        }
    }

    // The counter ends beside the corner, wherever its text starts.
    double counterRight = -1.0;
    for (const int fps : {9, 60, 999}) {
        {
            QMutexLocker lock(&item.m_stateMutex);
            item.m_fpsValue = fps;
            item.m_overlayDirty = true;
        }
        const QRectF counter = item.fpsCounterRect();
        QVERIFY(!counter.isEmpty());
        QVERIFY(!counter.intersects(QRectF(corner)));
        QVERIFY(counterRight < 0.0 || qFuzzyCompare(counter.right(), counterRight));
        counterRight = counter.right();
        QCOMPARE(item.updatePaintNode(node, nullptr), node);
        auto *overlay = static_cast<QSGSimpleTextureNode *>(node->lastChild());
        QVERIFY(qAbs(overlay->rect().right() - counter.right()) < 1.0);
        QVERIFY(!overlay->rect().intersects(QRectF(corner)));
    }
    delete node;
}

void SpectrogramSmokeTest::spectrogramCrosshairAndGridPropertiesAndHoverTracking() {
    // Verify crosshairEnabled/gridEnabled properties default to false,
    // emit change signals, and that hover events update internal state.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    // Default state: both overlays disabled, no hover.
    QCOMPARE(item.crosshairEnabled(), false);
    QCOMPARE(item.gridEnabled(), false);
    QCOMPARE(item.m_hoverActive, false);

    // Enable crosshair — signal fires, dirty flag set.
    QSignalSpy crosshairSpy(&item, &SpectrogramItem::crosshairEnabledChanged);
    item.setCrosshairEnabled(true);
    QCOMPARE(item.crosshairEnabled(), true);
    QCOMPARE(crosshairSpy.count(), 1);
    QCOMPARE(item.m_crosshairDirty, true);

    // No-op when setting same value.
    item.setCrosshairEnabled(true);
    QCOMPARE(crosshairSpy.count(), 1);

    // Enable grid.
    QSignalSpy gridSpy(&item, &SpectrogramItem::gridEnabledChanged);
    item.setGridEnabled(true);
    QCOMPARE(item.gridEnabled(), true);
    QCOMPARE(gridSpy.count(), 1);
    QVERIFY(item.m_freqGridDirty || item.m_timeGridDirty);

    // Simulate hover enter — m_hoverActive should become true.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairDirty = false;
    }
    QHoverEvent enterEvent(QEvent::HoverEnter, QPointF(100.0, 50.0), QPointF(100.0, 50.0), QPointF());
    item.hoverEnterEvent(&enterEvent);
    QCOMPARE(item.m_hoverActive, true);
    QVERIFY(std::abs(item.m_hoverPosition.x() - 100.0) < 0.01);
    QVERIFY(std::abs(item.m_hoverPosition.y() - 50.0) < 0.01);
    QCOMPARE(item.m_crosshairDirty, true); // crosshair enabled → dirty

    // Simulate hover move.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairDirty = false;
    }
    QHoverEvent moveEvent(QEvent::HoverMove, QPointF(150.0, 75.0), QPointF(150.0, 75.0), QPointF(100.0, 50.0));
    item.hoverMoveEvent(&moveEvent);
    QCOMPARE(item.m_hoverActive, true);
    QVERIFY(std::abs(item.m_hoverPosition.x() - 150.0) < 0.01);
    QVERIFY(std::abs(item.m_hoverPosition.y() - 75.0) < 0.01);
    QCOMPARE(item.m_crosshairDirty, true);

    // Simulate hover leave — m_hoverActive becomes false.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairDirty = false;
    }
    QHoverEvent leaveEvent(QEvent::HoverLeave, QPointF(), QPointF(), QPointF(150.0, 75.0));
    item.hoverLeaveEvent(&leaveEvent);
    QCOMPARE(item.m_hoverActive, false);
    QCOMPARE(item.m_crosshairDirty, true);

    // When crosshair is disabled, hover events do NOT mark dirty.
    item.setCrosshairEnabled(false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairDirty = false;
    }
    QHoverEvent enterEvent2(QEvent::HoverEnter, QPointF(50.0, 30.0), QPointF(50.0, 30.0), QPointF());
    item.hoverEnterEvent(&enterEvent2);
    QCOMPARE(item.m_hoverActive, true); // position still tracked
    QCOMPARE(item.m_crosshairDirty, false); // but NOT dirty
}

void SpectrogramSmokeTest::spectrogramPixelToFrequency() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    // Feed data to set binsPerColumn = 4097 (FFT size 8192).
    const int bins = 4097;
    QByteArray data(bins, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 1, 0, 100, 48000, 1024, false, true, 1, false);

    // In linear mode, bottom pixel maps to 0 Hz (DC),
    // top pixel maps to Nyquist (24000 Hz).
    // pixelY=0 -> top -> ~24000 Hz
    // pixelY=99 -> bottom -> 0 Hz
    QVERIFY(item.pixelToFrequencyHz(0, 100) > 20000.0);
    QVERIFY(item.pixelToFrequencyHz(99, 100) < 500.0);

    // Mid-height should be roughly half Nyquist in linear mode.
    const double midFreq = item.pixelToFrequencyHz(50, 100);
    QVERIFY(midFreq > 10000.0 && midFreq < 14000.0);
}

void SpectrogramSmokeTest::spectrogramOverlayBenchmark() {
    if (qEnvironmentVariableIntValue("FERROUS_BENCHMARK_SPECTROGRAM") != 1) {
        QSKIP("Set FERROUS_BENCHMARK_SPECTROGRAM=1 to measure overlay refreshes");
    }
    // The render sync's part of the grid and crosshair overlays of a
    // 3440 x 719 pane with both grids and their labels, in a window at
    // FERROUS_BENCHMARK_SPECTROGRAM_SCALE (default 1).
    const double scale = qEnvironmentVariableIsSet("FERROUS_BENCHMARK_SPECTROGRAM_SCALE")
        ? qEnvironmentVariable("FERROUS_BENCHMARK_SPECTROGRAM_SCALE").toDouble()
        : 1.0;
    QQuickWindow window;
    window.resize(240, 160);
    if (scale != 1.0) {
        QScreen *screen = hiDpiTestScreen(scale);
        if (screen == nullptr) {
            QSKIP("Needs the offscreen platform's HiDPI test screen at that scale");
        }
        QVERIFY(moveWindowToScreen(window, screen));
    }
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    QCOMPARE(window.effectiveDevicePixelRatio(), scale);
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(3440);
    item.setHeight(719);
    item.setDisplayMode(1);
    item.setPlaying(false);
    item.setGridEnabled(true);
    item.setShowTimeLabels(true);
    item.setCrosshairEnabled(true);
    constexpr int bins = 1025;
    constexpr int columns = 20'000;
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x50'), bins, 0, columns,
        0, columns, 48000, 1024, true, true, 1, false, 1);
    setTestPosition(item, 100.0);
    QHoverEvent hover(QEvent::HoverEnter, QPointF(500.0, 300.0), QPointF(500.0, 300.0), QPointF());
    item.hoverEnterEvent(&hover);
    QSGNode *node = paintSettled(item, nullptr);
    constexpr int iterations = 12;
    QElapsedTimer elapsed;
    const auto frame = [&]() {
        elapsed.restart();
        node = item.updatePaintNode(node, nullptr);
        return static_cast<double>(elapsed.nsecsElapsed()) / 1e6;
    };
    const auto report = [](const char *name, const std::vector<double> &ms) {
        const double total = std::accumulate(ms.begin(), ms.end(), 0.0);
        qInfo("overlay %s mean_ms=%.3f worst_ms=%.3f", name, total / static_cast<double>(ms.size()),
            *std::max_element(ms.begin(), ms.end()));
    };
    std::vector<double> idle, timeGrid, grids, crosshair, resize;
    for (int i = 0; i < iterations; ++i) {
        idle.push_back(frame());
    }
    for (int i = 0; i < iterations; ++i) {
        {
            QMutexLocker lock(&item.m_stateMutex);
            item.m_timeGridDirty = true;
        }
        timeGrid.push_back(frame());
    }
    for (int i = 0; i < iterations; ++i) {
        {
            QMutexLocker lock(&item.m_stateMutex);
            item.m_timeGridDirty = true;
            item.m_freqGridDirty = true;
        }
        grids.push_back(frame());
    }
    for (int i = 0; i < iterations; ++i) {
        const QPointF to(600.0 + 7.0 * i, 280.0 + 3.0 * i);
        QHoverEvent move(QEvent::HoverMove, to, to, QPointF());
        item.hoverMoveEvent(&move);
        crosshair.push_back(frame());
    }
    for (int i = 0; i < iterations; ++i) {
        item.setHeight(i % 2 == 0 ? 718 : 719);
        resize.push_back(frame());
        QVERIFY(QTest::qWaitFor([&item]() { return !spectrogramRasterPending(item); }, 5000));
    }
    qInfo("overlay scale=%g", scale);
    report("idle_frame", idle);
    report("time_grid_refresh", timeGrid);
    report("both_grids_refresh", grids);
    report("crosshair_move", crosshair);
    report("resize_frame", resize);
    delete node;
}

void SpectrogramSmokeTest::spectrogramCrosshairOverlayGeneratesOnHover() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_crosshairDirty = true;
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0);
    }

    QVERIFY(item.m_crosshair.valid);
    QCOMPARE(item.m_crosshair.size, QSize(200, 100));

    // Its dashed lines through the pointer, and the readout's plates.
    QVector<SpectrogramOverlayImage::Kind> kinds;
    for (const SpectrogramOverlayImage &image : item.m_crosshair.images) {
        kinds.push_back(image.kind);
    }
    QCOMPARE(kinds, (QVector<SpectrogramOverlayImage::Kind>{
        SpectrogramOverlayImage::DashedColumn,
        SpectrogramOverlayImage::DashedRow,
        SpectrogramOverlayImage::ReadoutPlate,
        SpectrogramOverlayImage::ReadoutPlate}));
    QCOMPARE(item.m_crosshair.images[0].rect, QRectF(50.0, 0.0, 1.0, 100.0));
    QCOMPARE(item.m_crosshair.images[1].rect, QRectF(0.0, 40.0, 200.0, 1.0));
    // The frequency and the level at the pointer, then the time in the
    // track: 128 of 255 is -75 dBFS, and column 50 is at 1.07 s.
    const QStringList lines = item.m_crosshairLaidOutReadout.split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), 2);
    QVERIFY2(lines[0].contains(QStringLiteral("kHz, \u221275 dB")), qPrintable(lines[0]));
    QCOMPARE(lines[1], QStringLiteral("0:01.1"));
    // The GUI thread shows it once the event loop runs.
    QTRY_COMPARE(item.crosshairReadout(), item.m_crosshairLaidOutReadout);
    // Off the pane, there is none.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = false;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, 48000.0 / 1024.0, 0.0);
    }
    QTRY_VERIFY(item.crosshairReadout().isEmpty());
}

void SpectrogramSmokeTest::spectrogramCrosshairOverlayMovesOnlyItsTimeLabel() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);
    item.setShowTimeLabels(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    const double cps = 48000.0 / 1024.0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        item.updateCrosshairOverlayLocked(200, 100, 0, true, cps, 0.0);
        QVERIFY(item.m_crosshair.valid);
        const SpectrogramOverlayLayout first = item.m_crosshair;
        const QString readout = item.m_crosshairLaidOutReadout;
        // The view moving on changes the readout's time, and nothing else.
        item.updateCrosshairOverlayLocked(200, 100, 0, true, cps, 25.0);
        QVERIFY(item.m_crosshair.valid);
        QVERIFY(item.m_crosshairLaidOutReadout != readout);
        QCOMPARE(item.m_crosshairLaidOutReadout.section(QLatin1Char('\n'), 0, 0),
                 readout.section(QLatin1Char('\n'), 0, 0));
        QCOMPARE(item.m_crosshair.images.size(), first.images.size());
        QCOMPARE(item.m_crosshair.images[0].rect, first.images[0].rect);
        QCOMPARE(item.m_crosshair.images[1].rect, first.images[1].rect);
        // No image is as large as the pane: lines are a pixel thick.
        for (const SpectrogramOverlayImage &image : item.m_crosshair.images) {
            QVERIFY(image.rect.width() <= 1.0 || image.rect.height() <= 1.0
                || image.rect.width() * image.rect.height() < 200.0 * 100.0 / 8.0);
        }
    }
}

namespace {

// The readout's plates in a crosshair's layout: the value's, then the time's.
QVector<SpectrogramOverlayImage> readoutPlatesOf(const SpectrogramOverlayLayout &layout) {
    QVector<SpectrogramOverlayImage> plates;
    for (const SpectrogramOverlayImage &image : layout.images) {
        if (image.kind == SpectrogramOverlayImage::ReadoutPlate) {
            plates.push_back(image);
        }
    }
    return plates;
}

// 400 columns of a 48 kHz track, -75 dBFS throughout.
void feedReadoutFixture(SpectrogramItem &item) {
    item.setSampleRateHz(48'000);
    item.setCrosshairEnabled(true);
    constexpr int bins = 4'097;
    constexpr int columns = 400;
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x80'), bins, 0, columns, 0, 1'000,
                              48'000, 1'024, false, true, 1, false);
}

constexpr double kReadoutColumnsPerSecond = 48'000.0 / 1'024.0;

// Lays the crosshair out on a lane `lane` logical pixels in size at `scale`
// device pixels to a logical one, as a frame does: the frequency scale
// first. With `hovered`, the pointer is at `pointer`; otherwise the hovered
// lane's crosshair is at `sharedX`. `drawX` is in logical pixels.
void layOutReadout(SpectrogramItem &item, const QSize &lane, double scale, const QPointF &pointer,
                   bool rolling = false, double drawX = 0.0, bool hovered = true,
                   double sharedX = -1.0) {
    QMutexLocker lock(&item.m_stateMutex);
    item.m_canvasScale = scale;
    item.m_hoverActive = hovered;
    item.m_hoverPosition = pointer;
    item.m_crosshairSharedX = sharedX;
    item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
    const int width = static_cast<int>(std::lround(lane.width() * scale));
    const int height = static_cast<int>(std::lround(lane.height() * scale));
    item.ensureMapping(height);
    if (item.m_gridEnabled) {
        item.updateFreqGridOverlayLocked(width, height);
    }
    item.updateCrosshairOverlayLocked(
        width, height, 0, rolling, kReadoutColumnsPerSecond, drawX * scale);
}

// "1:46.0", as the readout shows a time under an hour.
QString readoutTime(double seconds) {
    const long tenths = std::lround(seconds * 10.0);
    return QStringLiteral("%1:%2.%3")
        .arg(tenths / 600)
        .arg((tenths / 10) % 60, 2, 10, QLatin1Char('0'))
        .arg(tenths % 10);
}

} // namespace

void SpectrogramSmokeTest::spectrogramCrosshairReadoutClampsInsideTheLane_data() {
    QTest::addColumn<double>("scale");
    QTest::addColumn<bool>("scaleShown");
    for (const double scale : {1.0, 2.0}) {
        for (const bool scaleShown : {false, true}) {
            QTest::addRow("%gx%s", scale, scaleShown ? ", frequency scale" : "")
                << scale << scaleShown;
        }
    }
}

void SpectrogramSmokeTest::spectrogramCrosshairReadoutClampsInsideTheLane() {
    QFETCH(double, scale);
    QFETCH(bool, scaleShown);
    SpectrogramItem item;
    feedReadoutFixture(item);
    item.setGridEnabled(scaleShown);
    const QSize lane(300, 120);
    const QRectF laneRect(QPointF(0.0, 0.0), QSizeF(lane));
    const struct {
        const char *where;
        QPointF pointer;
    } cases[] = {
        {"mid-plot", {150.0, 60.0}},
        {"at the right edge", {299.5, 60.0}},
        {"at the bottom edge", {150.0, 119.5}},
        {"in the bottom right corner", {299.5, 119.5}},
        {"in the top left corner", {0.0, 0.0}},
    };
    for (const auto &pointer : cases) {
        layOutReadout(item, lane, scale, pointer.pointer);
        const QVector<SpectrogramOverlayImage> plates = readoutPlatesOf(item.m_crosshair);
        QVERIFY2(plates.size() == 2, pointer.where);
        const QRectF value = plates[0].rect;
        const QRectF time = plates[1].rect;
        const QByteArray where = QByteArray(pointer.where) + ": value "
            + QTest::toString(value) + ", time " + QTest::toString(time);
        // The frequency and level, then the time in the track.
        QVERIFY2(plates[0].text.contains(QStringLiteral("kHz, −75 dB"))
                     || plates[0].text.contains(QStringLiteral("Hz, −75 dB")),
                 qPrintable(plates[0].text));
        QCOMPARE(item.m_crosshairLaidOutReadout,
                 plates[0].text + QLatin1Char('\n') + plates[1].text);

        // Inside the lane, apart, and clear of the frequency scale.
        QVERIFY2(laneRect.contains(value) && laneRect.contains(time), where.constData());
        QVERIFY2(!value.intersects(time), where.constData());
        if (scaleShown) {
            QVERIFY(!item.m_freqGrid.images.isEmpty());
            for (const SpectrogramOverlayImage &label : item.m_freqGrid.images) {
                QVERIFY2(!label.rect.intersects(value) && !label.rect.intersects(time),
                         (where + ", scale label " + QTest::toString(label.rect)).constData());
            }
        } else {
            QCOMPARE(value.right(), lane.width() - 2.0);
        }
        // On the axes: the time on the bottom edge, and where the lane
        // leaves room, each centred on its line. Only a scale label beside
        // the time keeps it from the right edge.
        QCOMPARE(time.bottom(), lane.height() - 2.0);
        if (qstrcmp(pointer.where, "at the right edge") == 0) {
            bool labelBeside = false;
            for (const SpectrogramOverlayImage &label : item.m_freqGrid.images) {
                labelBeside = labelBeside || label.rect.bottom() > time.top() - 4.0;
            }
            if (!scaleShown || !labelBeside) {
                QCOMPARE(time.right(), static_cast<double>(lane.width()));
            }
        }
        if (qstrcmp(pointer.where, "mid-plot") == 0) {
            QVERIFY2(std::abs(value.center().y() - 60.0) <= 1.0, where.constData());
            QVERIFY2(std::abs(time.center().x() - 150.0) <= 1.0, where.constData());
        }
    }
}

void SpectrogramSmokeTest::spectrogramCrosshairReadoutFitsShortLanes_data() {
    QTest::addColumn<int>("laneHeight");
    QTest::addColumn<double>("scale");
    QTest::addColumn<bool>("scaleShown");
    // Eight lanes of the pane at its 220 px minimum are 27 px; six are 36.
    for (const int laneHeight : {27, 31, 36}) {
        for (const double scale : {1.0, 1.5, 2.0}) {
            for (const bool scaleShown : {false, true}) {
                QTest::addRow("%d px, %gx%s", laneHeight, scale,
                              scaleShown ? ", frequency scale" : "")
                    << laneHeight << scale << scaleShown;
            }
        }
    }
}

void SpectrogramSmokeTest::spectrogramCrosshairReadoutFitsShortLanes() {
    QFETCH(int, laneHeight);
    QFETCH(double, scale);
    QFETCH(bool, scaleShown);
    SpectrogramItem item;
    feedReadoutFixture(item);
    item.setGridEnabled(scaleShown);
    // The lowest lane's label and its Mute and Solo buttons, as PlotLayer
    // reports them.
    const QRectF laneHead(4.0, 3.0, 82.0, 26.0);
    item.setLaneHeadBounds(laneHead);
    const QSize lane(600, laneHeight);
    const QRectF laneRect(QPointF(0.0, 0.0), QSizeF(lane));
    const double bottom = laneHeight - 0.5;
    const double middle = laneHeight * 0.5;
    const struct {
        const char *where;
        QPointF pointer;
    } cases[] = {
        {"in the bottom right corner", {599.5, bottom}},
        {"at the right edge", {599.5, middle}},
        {"in the top right corner", {599.5, 0.0}},
        {"near the right edge", {560.0, bottom}},
        {"at the bottom edge", {300.0, bottom}},
        {"in the bottom left corner", {0.0, bottom}},
    };
    for (const auto &pointer : cases) {
        layOutReadout(item, lane, scale, pointer.pointer);
        const QVector<SpectrogramOverlayImage> plates = readoutPlatesOf(item.m_crosshair);
        QVERIFY2(plates.size() == 2, pointer.where);
        const QRectF value = plates[0].rect;
        const QRectF time = plates[1].rect;
        const QByteArray where = QByteArray(pointer.where) + ": value "
            + QTest::toString(value) + ", time " + QTest::toString(time);
        // Both readable: inside the lane, apart, and clear of its head.
        QVERIFY2(laneRect.contains(value) && laneRect.contains(time), where.constData());
        QVERIFY2(!value.intersects(time), where.constData());
        QVERIFY2(!time.intersects(laneHead) && !value.intersects(laneHead), where.constData());
        QCOMPARE(time.bottom(), laneHeight - 2.0);
        for (const SpectrogramOverlayImage &label : item.m_freqGrid.images) {
            QVERIFY2(!label.rect.intersects(value) && !label.rect.intersects(time),
                     (where + ", scale label " + QTest::toString(label.rect)).constData());
        }
    }
}

void SpectrogramSmokeTest::crosshairReadoutPlatesFollowThePointerSmoothly_data() {
    QTest::addColumn<int>("height");
    QTest::addColumn<bool>("scaleShown");
    for (const int height : {27, 36, 120, 220}) {
        for (const bool scaleShown : {false, true}) {
            QTest::addRow("%d px%s", height, scaleShown ? ", frequency scale" : "")
                << height << scaleShown;
        }
    }
}

void SpectrogramSmokeTest::crosshairReadoutPlatesFollowThePointerSmoothly() {
    // Each plate follows its own line: a pointer moving a pixel moves a
    // plate a pixel at most, and the plates never meet, in short lanes too.
    QFETCH(int, height);
    QFETCH(bool, scaleShown);
    constexpr int width = 600;
    const QSize value(66, 22);
    const QSize time(36, 22);
    const QRect laneHead(4, 3, 82, 26);
    CrosshairReadout::RightScale scale;
    if (scaleShown) {
        scale.width = 48;
        scale.reachesBottom = true;
    }
    const QRect lane(0, 0, width, height);
    const QRect scaleColumn(width - scale.width, 0, scale.width, height);
    QRect lastValue;
    QRect lastTime;
    const auto check = [&](int x, int y, bool steppingX) {
        const auto [valueRect, timeRect] = CrosshairReadout::plateRects(
            width, height, x, y, value, time, QSizeF(), scale, laneHead);
        const QByteArray where = QByteArray("pointer at ") + QByteArray::number(x) + ", "
            + QByteArray::number(y) + ": value " + QTest::toString(QRectF(valueRect))
            + ", time " + QTest::toString(QRectF(timeRect));
        if (!(lane.contains(valueRect) && lane.contains(timeRect))
            || valueRect.intersects(timeRect) || timeRect.intersects(laneHead)
            || (scaleShown && (valueRect.intersects(scaleColumn)
                               || timeRect.intersects(scaleColumn)))) {
            QTest::qFail(where.constData(), __FILE__, __LINE__);
            return false;
        }
        if (!lastValue.isNull()) {
            const int valueMove = std::abs(valueRect.left() - lastValue.left())
                + std::abs(valueRect.top() - lastValue.top());
            const int timeMove = std::abs(timeRect.left() - lastTime.left())
                + std::abs(timeRect.top() - lastTime.top());
            // Along x only the time moves; along y only the value.
            if (valueMove > (steppingX ? 0 : 1) || timeMove > (steppingX ? 1 : 0)) {
                QTest::qFail(("jumps at " + where).constData(), __FILE__, __LINE__);
                return false;
            }
        }
        lastValue = valueRect;
        lastTime = timeRect;
        return true;
    };
    for (const int y : {height - 1, height / 2, 0}) {
        lastValue = QRect();
        for (int x = 0; x < width; ++x) {
            if (!check(x, y, true)) return;
        }
        for (int x = width - 1; x >= 0; --x) {
            if (!check(x, y, true)) return;
        }
    }
    for (const int x : {width - 1, width - 40, width / 2, 0}) {
        lastValue = QRect();
        for (int y = 0; y < height; ++y) {
            if (!check(x, y, false)) return;
        }
    }
}

void SpectrogramSmokeTest::crosshairReadoutLeavesAPlateOutInNarrowPlots_data() {
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("height");
    QTest::addColumn<int>("valueWidth");
    QTest::addColumn<bool>("scaleShown");
    for (int width = 120; width <= 240; width += 10) {
        for (const int height : {27, 31, 36}) {
            for (const int valueWidth : {66, 100}) {
                for (const bool scaleShown : {false, true}) {
                    QTest::addRow("%dx%d, value %d px%s", width, height, valueWidth,
                                  scaleShown ? ", frequency scale" : "")
                        << width << height << valueWidth << scaleShown;
                }
            }
        }
    }
}

void SpectrogramSmokeTest::crosshairReadoutLeavesAPlateOutInNarrowPlots() {
    // A lane too narrow for the lane head, the value and the time side by
    // side leaves a plate out rather than overlap them: the value, where the
    // time fits right of the head, and the time too where it doesn't. Which
    // plates show doesn't depend on the pointer, and those that show follow
    // it smoothly.
    QFETCH(int, width);
    QFETCH(int, height);
    QFETCH(int, valueWidth);
    QFETCH(bool, scaleShown);
    const QSize value(valueWidth, 22);
    const QSize time(40, 22);
    // The lowest lane's label and buttons at x = 4 to 86, down the lane.
    const QRect laneHead(4, 3, 83, 26);
    CrosshairReadout::RightScale scale;
    if (scaleShown) {
        scale.width = 48;
        scale.reachesBottom = true;
    }
    const QRect lane(0, 0, width, height);
    const QRect scaleColumn(width - scale.width, 0, scale.width, height);
    // Right of the head, and left of the scale or the plot's right edge.
    const int timeRoom = (scaleShown ? width - scale.width - CrosshairReadout::kGap : width)
        - (laneHead.right() + 1 + CrosshairReadout::kGap);
    const bool timeFits = timeRoom >= time.width();
    const int valueRight = scaleShown ? width - scale.width - CrosshairReadout::kGap
                                      : width - CrosshairReadout::kEdgeMargin;
    const bool bothFit = valueRight - value.width() - CrosshairReadout::kGap
            - (laneHead.right() + 1 + CrosshairReadout::kGap)
        >= time.width();

    bool started = false;
    bool hasValue = false;
    bool hasTime = false;
    QRect lastValue;
    QRect lastTime;
    const auto check = [&](int x, int y, bool steppingX) {
        const auto [valueRect, timeRect] = CrosshairReadout::plateRects(
            width, height, x, y, value, time, QSizeF(), scale, laneHead);
        const QByteArray where = QByteArray("pointer at ") + QByteArray::number(x) + ", "
            + QByteArray::number(y) + ": value " + QTest::toString(QRectF(valueRect))
            + ", time " + QTest::toString(QRectF(timeRect));
        const auto clear = [&](const QRect &plate) {
            return !plate.isValid()
                || (lane.contains(plate) && !plate.intersects(laneHead)
                    && !(scaleShown && plate.intersects(scaleColumn)));
        };
        if (!clear(valueRect) || !clear(timeRect)
            || (valueRect.isValid() && valueRect.intersects(timeRect))) {
            QTest::qFail(where.constData(), __FILE__, __LINE__);
            return false;
        }
        // The same plates wherever the pointer is: the time where it fits,
        // and the value beside it where both do.
        if (!started) {
            started = true;
            hasValue = valueRect.isValid();
            hasTime = timeRect.isValid();
            if (hasTime != timeFits || (timeFits && hasValue != bothFit)) {
                QTest::qFail(("plates shown at " + where).constData(), __FILE__, __LINE__);
                return false;
            }
        } else if (valueRect.isValid() != hasValue || timeRect.isValid() != hasTime) {
            QTest::qFail(("a plate comes or goes at " + where).constData(), __FILE__, __LINE__);
            return false;
        }
        if (!lastValue.isNull() || !lastTime.isNull()) {
            const int valueMove = std::abs(valueRect.left() - lastValue.left())
                + std::abs(valueRect.top() - lastValue.top());
            const int timeMove = std::abs(timeRect.left() - lastTime.left())
                + std::abs(timeRect.top() - lastTime.top());
            if (valueMove > (steppingX ? 0 : 1) || timeMove > (steppingX ? 1 : 0)) {
                QTest::qFail(("jumps at " + where).constData(), __FILE__, __LINE__);
                return false;
            }
        }
        lastValue = valueRect;
        lastTime = timeRect;
        return true;
    };
    for (const int y : {height - 1, height / 2, 0}) {
        lastValue = lastTime = QRect();
        for (int x = 0; x < width; ++x) {
            if (!check(x, y, true)) return;
        }
        for (int x = width - 1; x >= 0; --x) {
            if (!check(x, y, true)) return;
        }
    }
    for (const int x : {width - 1, width - 40, width / 2, 0}) {
        lastValue = lastTime = QRect();
        for (int y = 0; y < height; ++y) {
            if (!check(x, y, false)) return;
        }
    }
}

void SpectrogramSmokeTest::spectrogramCrosshairReadoutStaysReadableInNarrowLanes_data() {
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("laneHeight");
    QTest::addColumn<double>("scale");
    QTest::addColumn<bool>("scaleShown");
    for (const int width : {160, 200, 240}) {
        for (const int laneHeight : {27, 36}) {
            for (const double scale : {1.0, 2.0}) {
                for (const bool scaleShown : {false, true}) {
                    QTest::addRow("%dx%d, %gx%s", width, laneHeight, scale,
                                  scaleShown ? ", frequency scale" : "")
                        << width << laneHeight << scale << scaleShown;
                }
            }
        }
    }
}

void SpectrogramSmokeTest::spectrogramCrosshairReadoutStaysReadableInNarrowLanes() {
    // The lowest lane of a narrow pane, as a small windowed viewer of a 5.1
    // or 7.1 track has it: the plates never meet each other, the lane head
    // or the scale, and never leave the lane, as the pointer moves along the
    // bottom edge and through the middle a pixel at a time.
    QFETCH(int, width);
    QFETCH(int, laneHeight);
    QFETCH(double, scale);
    QFETCH(bool, scaleShown);
    SpectrogramItem item;
    feedReadoutFixture(item);
    item.setGridEnabled(scaleShown);
    const QRectF laneHead(4.0, 3.0, 82.0, 26.0);
    item.setLaneHeadBounds(laneHead);
    const QSize lane(width, laneHeight);
    const QRectF laneRect(QPointF(0.0, 0.0), QSizeF(lane));
    for (const double y : {laneHeight - 0.5, laneHeight * 0.5}) {
        bool started = false;
        bool hasValue = false;
        bool hasTime = false;
        QRectF lastValue;
        QRectF lastTime;
        for (int step = 0; step < width; ++step) {
            const double x = step + 0.5;
            layOutReadout(item, lane, scale, QPointF(x, y));
            QRectF value;
            QRectF time;
            QStringList texts;
            for (const SpectrogramOverlayImage &plate : readoutPlatesOf(item.m_crosshair)) {
                // The time is the plate with minutes and seconds.
                (plate.text.contains(QLatin1Char(':')) ? time : value) = plate.rect;
                texts.push_back(plate.text);
            }
            const QByteArray where = QByteArray("pointer at ") + QByteArray::number(x) + ", "
                + QByteArray::number(y) + ": value " + QTest::toString(value) + ", time "
                + QTest::toString(time);
            QCOMPARE(item.m_crosshairLaidOutReadout, texts.join(QLatin1Char('\n')));
            for (const QRectF &plate : {value, time}) {
                if (plate.isNull()) continue;
                QVERIFY2(laneRect.contains(plate), where.constData());
                QVERIFY2(!plate.intersects(laneHead), where.constData());
                for (const SpectrogramOverlayImage &label : item.m_freqGrid.images) {
                    QVERIFY2(!label.rect.intersects(plate),
                             (where + ", scale label " + QTest::toString(label.rect)).constData());
                }
            }
            QVERIFY2(value.isNull() || time.isNull() || !value.intersects(time),
                     where.constData());
            // Without the scale, 200 px leave the time room right of the head.
            if (width >= 200 && !scaleShown) {
                QVERIFY2(!time.isNull(), where.constData());
            }
            // No plate comes or goes, the value stays, and the time moves a
            // pixel at most.
            if (!started) {
                started = true;
                hasValue = !value.isNull();
                hasTime = !time.isNull();
            } else {
                QVERIFY2(value.isNull() != hasValue && time.isNull() != hasTime,
                         where.constData());
                QVERIFY2(value == lastValue, where.constData());
                QVERIFY2(std::abs(time.left() - lastTime.left()) <= 1.0, where.constData());
            }
            lastValue = value;
            lastTime = time;
        }
    }

    // The example of #494: a 200 px lane 27 px tall, its head at x = 4 to 86,
    // a value plate 100 px wide and a time plate 40: the value is left out.
    const auto [value, time] = CrosshairReadout::plateRects(
        200, 27, 199, 26, QSize(100, 22), QSize(40, 22), QSizeF(),
        CrosshairReadout::RightScale(), QRect(4, 3, 83, 24));
    QVERIFY(value.isNull());
    QCOMPARE(time, QRect(160, 3, 40, 22));
    // At 120 px neither fits right of the head: both are left out.
    const auto [narrowValue, narrowTime] = CrosshairReadout::plateRects(
        120, 27, 60, 26, QSize(100, 22), QSize(40, 22), QSizeF(),
        CrosshairReadout::RightScale(), QRect(4, 3, 83, 24));
    QVERIFY(narrowValue.isNull() && narrowTime.isNull());
}

void SpectrogramSmokeTest::spectrogramCrosshairReadoutKeepsItsPlatesAsTheLevelChanges() {
    // Across audio whose level changes from column to column, the readout
    // shows the same plates wherever the pointer is: whether the value fits
    // beside the time goes by the widest frequency and level, not by the
    // text under the pointer. Narrow lanes around the width where the value
    // stops fitting, as in a small windowed viewer of a 7.1 track.
    SpectrogramItem item;
    item.setSampleRateHz(48'000);
    item.setCrosshairEnabled(true);
    constexpr int bins = 4'097;
    constexpr int columns = 400;
    // Silence, −150, −75 and −3 dB in turn: 1..255 span −150..0 dB.
    constexpr unsigned char levels[] = {0, 1, 128, 250};
    QByteArray data(bins * columns, '\0');
    for (int column = 0; column < columns; ++column) {
        std::fill_n(data.data() + column * bins, bins,
                    static_cast<char>(levels[column % std::size(levels)]));
    }
    item.feedPrecomputedChunk(data, bins, 0, columns, 0, 1'000, 48'000, 1'024, false, true, 1,
                              false);
    const QRectF laneHead(4.0, 3.0, 82.0, 26.0);
    item.setLaneHeadBounds(laneHead);
    constexpr int laneHeight = 27;
    bool valueShown = false;
    bool valueLeftOut = false;
    for (const double scale : {1.0, 2.0}) {
        for (int width = 196; width <= 264; width += 4) {
            const QSize lane(width, laneHeight);
            const QRectF laneRect(QPointF(0.0, 0.0), QSizeF(lane));
            bool started = false;
            bool hasValue = false;
            bool hasTime = false;
            QSet<QString> values;
            for (int step = 0; step < width; ++step) {
                const QPointF pointer(step + 0.5, laneHeight * 0.5);
                layOutReadout(item, lane, scale, pointer);
                QRectF value;
                QRectF time;
                for (const SpectrogramOverlayImage &plate : readoutPlatesOf(item.m_crosshair)) {
                    if (plate.text.contains(QLatin1Char(':'))) {
                        time = plate.rect;
                    } else {
                        value = plate.rect;
                        values.insert(plate.text);
                    }
                }
                const QByteArray where = QByteArray::number(width) + " px at "
                    + QByteArray::number(scale) + "x, pointer at "
                    + QByteArray::number(pointer.x()) + ": value " + QTest::toString(value)
                    + ", time " + QTest::toString(time) + ", readout "
                    + item.m_crosshairLaidOutReadout.toUtf8();
                for (const QRectF &plate : {value, time}) {
                    if (plate.isNull()) continue;
                    QVERIFY2(laneRect.contains(plate) && !plate.intersects(laneHead),
                             where.constData());
                }
                QVERIFY2(value.isNull() || time.isNull() || !value.intersects(time),
                         where.constData());
                if (!started) {
                    started = true;
                    hasValue = !value.isNull();
                    hasTime = !time.isNull();
                } else {
                    QVERIFY2(value.isNull() != hasValue && time.isNull() != hasTime,
                             ("a plate comes or goes at " + where).constData());
                }
            }
            QVERIFY2(hasTime, QByteArray::number(width).constData());
            if (hasValue) {
                // The level under the pointer changed as it moved.
                QVERIFY2(values.size() >= 3, QByteArray::number(width).constData());
                valueShown = true;
            } else {
                valueLeftOut = true;
            }
        }
    }
    // The widths reach both sides of where the value stops fitting.
    QVERIFY(valueShown && valueLeftOut);
}

void SpectrogramSmokeTest::crosshairReadoutWidestWidthsFollowTheFont() {
    // The readout's font takes its family and style from the application's,
    // which can change while the app runs: the widest frequency and level
    // the spectrogram and the waveform measured are measured again in it.
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    SpectrogramItem spectrogram;
    feedReadoutFixture(spectrogram);
    WaveformEditorItem waveform;
    const auto widest = [](SpectrogramItem &spectrogram, WaveformEditorItem &waveform) {
        QMutexLocker spectrogramLock(&spectrogram.m_stateMutex);
        QMutexLocker waveformLock(&waveform.m_stateMutex);
        return std::pair(spectrogram.crosshairWidestValueWidthLocked(),
                         waveform.crosshairWidestLevelWidthLocked());
    };
    const auto before = widest(spectrogram, waveform);
    QVERIFY(before.first > 0 && before.second > 0);
    QCOMPARE(widest(spectrogram, waveform), before);

    // Another family, its letters 2 px further apart.
    QFont font(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    font.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
    QGuiApplication::setFont(font);
    QCOMPARE(CrosshairReadout::font().letterSpacing(), 2.0);
    const auto after = widest(spectrogram, waveform);
    QVERIFY2(after.first > before.first && after.second > before.second,
             qPrintable(QStringLiteral("before %1, %2; after %3, %4")
                            .arg(before.first).arg(before.second)
                            .arg(after.first).arg(after.second)));
    // As items that never measured in the old font measure them.
    SpectrogramItem freshSpectrogram;
    feedReadoutFixture(freshSpectrogram);
    WaveformEditorItem freshWaveform;
    QCOMPARE(after, widest(freshSpectrogram, freshWaveform));

    // And back.
    QGuiApplication::setFont(applicationFont);
    QCOMPARE(widest(spectrogram, waveform), before);
}

void SpectrogramSmokeTest::spectrogramCrosshairWidestValueCoversRoundedHertz() {
    // At 11,025 Hz with 8,192-point FFTs, the bin at 999.948 Hz reads
    // "1000 Hz": a frequency of more digits than "999 Hz", which the widest
    // value the readout measures covers, with the lowest level, as every
    // other frequency of the band.
    SpectrogramItem item;
    item.setSampleRateHz(11'025);
    item.setCrosshairEnabled(true);
    constexpr int bins = 4'097;
    constexpr int columns = 50;
    // −150 dB throughout: the level of the most digits.
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x01'), bins, 0, columns, 0, 1'000,
                              11'025, 1'024, false, true, 1, false);
    const QFontMetrics metrics(CrosshairReadout::font());
    // A row for each bin.
    const QSize lane(300, bins);
    int widest = 0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        QCOMPARE(item.binHzLocked(), 11'025.0 / 8'192.0);
        widest = item.crosshairWidestValueWidthLocked();
    }
    bool sawThousandHertz = false;
    for (int row = 0; row < bins; ++row) {
        layOutReadout(item, lane, 1.0, QPointF(20.5, row + 0.5));
        for (const SpectrogramOverlayImage &plate : readoutPlatesOf(item.m_crosshair)) {
            if (plate.text.contains(QLatin1Char(':'))) continue;
            sawThousandHertz = sawThousandHertz
                || plate.text.startsWith(QStringLiteral("1000 Hz, "));
            QVERIFY2(CrosshairReadout::widestDigitsWidth(plate.text, metrics) <= widest,
                     qPrintable(plate.text + QStringLiteral(" is wider than %1").arg(widest)));
        }
    }
    QVERIFY(sawThousandHertz);
}

void SpectrogramSmokeTest::spectrogramCrosshairTimeKeepsClearOfTheLaneHead() {
    // Six lanes in the pane at its 220 px minimum, with their real heads.
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
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
        property bool showSpectrogramScale: true
        property bool spectrogramZoomEnabled: true
        property bool stretchShortTracks: true
        property int channelButtonsVisibility: 1
        property var mutedChannels: []
        property var soloedChannels: []
        property bool showLevelMeter: false
        property string currentTrackPath: ""
        property bool waveformEndsEarly: false
        property real waveformCoverageSeconds: 0
        property var currentTrackChannelLabels: []
        property bool connected: true
        property real durationSeconds: 0
        property int currentTrackChannels: 6
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

    // Above a strip of the window the pointer leaves the plot for.
    Viewers.SpectrogramSurface {
        id: surface
        objectName: "headSpectrogramSurface"
        width: parent.width
        height: 220
        plotWidth: layer.plotWidth
        laneHeadBounds: layer.lowestLaneHeadBounds
        uiBridge: bridge
        uiPalette: palette
    }

    Viewers.PlotLayer {
        id: layer
        objectName: "headPlotLayer"
        width: parent.width
        height: 220
        uiBridge: bridge
        uiPalette: palette
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
    auto *surface = qobject_cast<QQuickItem *>(
        root->findChild<QObject *>(QStringLiteral("headSpectrogramSurface")));
    auto *layer = qobject_cast<QQuickItem *>(
        root->findChild<QObject *>(QStringLiteral("headPlotLayer")));
    QVERIFY(surface != nullptr && layer != nullptr);
    QVariantList descriptors;
    const QStringList labels{QStringLiteral("L"), QStringLiteral("R"), QStringLiteral("C"),
                             QStringLiteral("LFE"), QStringLiteral("RL"), QStringLiteral("RR")};
    for (int i = 0; i < labels.size(); ++i) {
        descriptors.push_back(QVariantMap{
            {QStringLiteral("label"), labels[i]},
            {QStringLiteral("showLabel"), true},
            {QStringLiteral("muted"), false},
            {QStringLiteral("channelIndex"), i},
        });
    }
    surface->setProperty("channelDescriptors", descriptors);

    // The lowest lane and its head.
    SpectrogramItem *pane = nullptr;
    QTRY_VERIFY([&]() {
        QList<SpectrogramItem *> panes;
        collectQuickItems(surface, &panes);
        pane = nullptr;
        for (SpectrogramItem *candidate : panes) {
            if (candidate->showCrosshairTime()) pane = candidate;
        }
        return panes.size() == 6 && pane != nullptr;
    }());
    QVERIFY(pane->height() < 37.0);
    QQuickItem *head = nullptr;
    QTRY_VERIFY([&]() {
        head = nullptr;
        QList<QQuickItem *> items;
        collectQuickItems(layer, &items);
        for (QQuickItem *candidate : items) {
            if (candidate->objectName() == QStringLiteral("plotLaneHead") && candidate->isVisible()
                && (head == nullptr || candidate->y() > head->y())) {
                head = candidate;
            }
        }
        // The sixth lane's, at the pane's bottom.
        return head != nullptr && head->y() > 180.0;
    }());
    // The head's plates in the lane, buttons included while hidden.
    const QPointF laneOrigin = pane->mapToScene(QPointF(0.0, 0.0));
    QRectF headPlates;
    QList<QQuickItem *> headItems;
    collectQuickItems(head, &headItems);
    for (QQuickItem *plate : headItems) {
        if (plate->objectName() == QStringLiteral("plotLaneLabel")
            || plate->objectName() == QStringLiteral("plotLaneButtons")) {
            headPlates |= plate->mapRectToScene(QRectF(0.0, 0.0, plate->width(), plate->height()))
                .translated(-laneOrigin);
        }
    }
    QVERIFY(headPlates.width() > 40.0);
    QTRY_VERIFY(pane->laneHeadBounds().contains(headPlates));

    constexpr int bins = 4'097;
    constexpr int columns = 2'000;
    pane->feedPrecomputedChunk(QByteArray(bins * columns, '\x80'), bins, 0, columns, 0, columns,
                               48'000, 1'024, true, true, 1, false);
    QTRY_VERIFY(pane->precomputedReady());
    const auto platesAt = [&](const QPoint &at) {
        QTest::mouseMove(window, at);
        QVector<SpectrogramOverlayImage> plates;
        const bool laidOut = QTest::qWaitFor([&]() {
            QMutexLocker lock(&pane->m_stateMutex);
            plates = readoutPlatesOf(pane->m_crosshair);
            return plates.size() == 2 && !pane->m_crosshairDirty
                && std::abs(pane->m_hoverPosition.x() - (at.x() - laneOrigin.x())) < 0.01;
        }, 5'000);
        return laidOut ? plates : QVector<SpectrogramOverlayImage>();
    };
    const int bottom = static_cast<int>(laneOrigin.y() + pane->height()) - 1;
    for (const QPoint at : {QPoint(2, bottom), QPoint(30, bottom), QPoint(70, bottom),
                            QPoint(300, bottom), QPoint(597, bottom)}) {
        const QVector<SpectrogramOverlayImage> plates = platesAt(at);
        QCOMPARE(plates.size(), 2);
        const QByteArray where = QTest::toString(at) + QByteArray(": value ")
            + QTest::toString(plates[0].rect) + ", time " + QTest::toString(plates[1].rect)
            + ", head " + QTest::toString(headPlates);
        QVERIFY2(!plates[1].rect.intersects(headPlates), where.constData());
        QVERIFY2(!plates[0].rect.intersects(plates[1].rect), where.constData());
        QVERIFY2(QRectF(0.0, 0.0, pane->width(), pane->height()).contains(plates[1].rect),
                 where.constData());
    }
}

void SpectrogramSmokeTest::spectrogramCrosshairReadoutReadsEachView_data() {
    QTest::addColumn<bool>("rolling");
    QTest::addColumn<double>("zoom");
    QTest::addColumn<double>("scale");
    for (const bool rolling : {true, false}) {
        for (const double zoom : {1.0, 4.0}) {
            for (const double scale : {1.0, 2.0}) {
                QTest::addRow("%s, %gx zoom, %gx", rolling ? "rolling" : "centred", zoom, scale)
                    << rolling << zoom << scale;
            }
        }
    }
}

void SpectrogramSmokeTest::spectrogramCrosshairReadoutReadsEachView() {
    QFETCH(bool, rolling);
    QFETCH(double, zoom);
    QFETCH(double, scale);
    SpectrogramItem item;
    feedReadoutFixture(item);
    item.setGridEnabled(true);
    double effectiveZoom = 1.0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = zoom;
        effectiveZoom = item.effectiveZoomLocked();
    }
    QVERIFY(std::abs(effectiveZoom - zoom) < 0.01);
    const QSize lane(400, 150);
    constexpr double drawX = 20.0;
    for (const QPointF pointer : {QPointF(120.0, 60.0), QPointF(399.5, 149.5)}) {
        layOutReadout(item, lane, scale, pointer, rolling, drawX);
        const QVector<SpectrogramOverlayImage> plates = readoutPlatesOf(item.m_crosshair);
        QCOMPARE(plates.size(), 2);
        // The time of the column under the vertical line.
        const double lineX = std::floor(pointer.x() * scale) / scale;
        const double seconds = (lineX - drawX) / effectiveZoom / kReadoutColumnsPerSecond;
        QCOMPARE(plates[1].text, readoutTime(seconds));
        for (const SpectrogramOverlayImage &plate : plates) {
            QVERIFY(QRectF(QPointF(0.0, 0.0), QSizeF(lane)).contains(plate.rect));
            // Laid out in whole logical pixels: whole device pixels at any
            // scale.
            QCOMPARE(plate.rect, QRectF(plate.rect.toRect()));
        }
    }
}

void SpectrogramSmokeTest::spectrogramCrosshairReadoutAcrossAGaplessBoundary() {
    SpectrogramItem item;
    feedReadoutFixture(item);
    const QSize lane(300, 120);
    // The current track starts 1 s into the view's timeline, after the
    // previous one, which starts at 0.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_gaplessPositionOffset = 1.0;
        item.m_previousTrackPositionOffset = 0.0;
    }
    // After the boundary, the current track's time: column 80 is at 1.71 s.
    layOutReadout(item, lane, 1.0, QPointF(80.0, 60.0), true);
    QVector<SpectrogramOverlayImage> plates = readoutPlatesOf(item.m_crosshair);
    QCOMPARE(plates.size(), 2);
    QCOMPARE(plates[1].text, QStringLiteral("0:00.7"));
    // Before it, the previous track's: column 23 is at 0.49 s.
    layOutReadout(item, lane, 1.0, QPointF(23.0, 60.0), true);
    plates = readoutPlatesOf(item.m_crosshair);
    QCOMPARE(plates.size(), 2);
    QVERIFY2(plates[0].text.contains(QStringLiteral("−75 dB")), qPrintable(plates[0].text));
    QCOMPARE(plates[1].text, QStringLiteral("0:00.5"));

    // Until the successor continues the view, where the previous track
    // starts is not known: before the boundary, the frequency and level
    // show without a time, rather than no readout.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_previousTrackPositionOffset = 1.0;
    }
    layOutReadout(item, lane, 1.0, QPointF(23.0, 60.0), true);
    plates = readoutPlatesOf(item.m_crosshair);
    QCOMPARE(plates.size(), 1);
    QVERIFY2(plates[0].text.contains(QStringLiteral("−75 dB")), qPrintable(plates[0].text));
    QCOMPARE(item.m_crosshairLaidOutReadout, plates[0].text);
}

void SpectrogramSmokeTest::spectrogramCrosshairTimeShowsOnTheLowestLane() {
    SpectrogramItem item;
    feedReadoutFixture(item);
    const QSize lane(300, 120);
    QSignalSpy showChanged(&item, &SpectrogramItem::showCrosshairTimeChanged);
    item.setShowCrosshairTime(false);
    QCOMPARE(showChanged.count(), 1);
    QVERIFY(item.m_crosshairDirty);
    // A lane above the lowest: the frequency and level only.
    layOutReadout(item, lane, 1.0, QPointF(150.0, 60.0));
    QVector<SpectrogramOverlayImage> plates = readoutPlatesOf(item.m_crosshair);
    QCOMPARE(plates.size(), 1);
    QVERIFY(plates[0].text.contains(QStringLiteral("dB")));

    // The lowest lane, while another is hovered: the time only, on the
    // vertical line the other lane shares.
    item.setShowCrosshairTime(true);
    layOutReadout(item, lane, 1.0, QPointF(), false, 0.0, false, 150.0);
    plates = readoutPlatesOf(item.m_crosshair);
    QCOMPARE(plates.size(), 1);
    QCOMPARE(plates[0].text, readoutTime(150.0 / kReadoutColumnsPerSecond));
    QVERIFY(std::abs(plates[0].rect.center().x() - 150.0) <= 1.0);
    QCOMPARE(plates[0].rect.bottom(), lane.height() - 2.0);
    QCOMPARE(item.m_crosshairLaidOutReadout, plates[0].text);
}

void SpectrogramSmokeTest::spectrogramCrosshairReadoutPlatesTakeThePlotColors() {
    SpectrogramItem item;
    feedReadoutFixture(item);
    const QSize lane(300, 120);
    QSignalSpy colorsChanged(&item, &SpectrogramItem::readoutColorsChanged);
    layOutReadout(item, lane, 1.0, QPointF(150.0, 60.0));
    const QString keyBefore = readoutPlatesOf(item.m_crosshair).constFirst().key;
    const QSizeF sizeBefore = readoutPlatesOf(item.m_crosshair).constFirst().rect.size();

    // A plate's colours name its texture; an edge takes a pixel around it.
    item.setReadoutPlateColor(QColor(30, 30, 30, 200));
    item.setReadoutTextColor(QColor(Qt::white));
    item.setReadoutPlateEdgeColor(QColor(Qt::transparent));
    QCOMPARE(colorsChanged.count(), 3);
    QCOMPARE(item.readoutPlateColor(), QColor(30, 30, 30, 200));
    QVERIFY(item.m_crosshairDirty);
    layOutReadout(item, lane, 1.0, QPointF(150.0, 60.0));
    const SpectrogramOverlayImage plate = readoutPlatesOf(item.m_crosshair).constFirst();
    QVERIFY(plate.key != keyBefore);
    QCOMPARE(plate.rect.size(), sizeBefore - QSizeF(2.0, 2.0));

    // Drawn in those colours, at device resolution.
    CrosshairReadout::Colors colors;
    {
        QMutexLocker lock(&item.m_stateMutex);
        colors = item.m_readoutColors;
    }
    for (const double scale : {1.0, 2.0}) {
        QImage image((plate.rect.size() * scale).toSize(), QImage::Format_ARGB32);
        image.setDevicePixelRatio(scale);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        CrosshairReadout::drawPlate(painter, QRect(QPoint(0, 0), plate.rect.size().toSize()),
                                    plate.text, CrosshairReadout::font(), colors);
        painter.end();
        // The fill at its left edge, between its rounded corners.
        const QColor fill = image.pixelColor(QPoint(0, image.height() / 2));
        QCOMPARE(fill.alpha(), 200);
        QVERIFY2(std::abs(fill.red() - 30) <= 1, qPrintable(fill.name()));
        bool hasText = false;
        for (int x = 0; x < image.width() && !hasText; ++x) {
            hasText = image.pixelColor(x, image.height() / 2).lightness() > 128;
        }
        QVERIFY(hasText);
    }
}

void SpectrogramSmokeTest::spectrogramCrosshairReadoutShowsAfterEveryClick() {
    // The readout is drawn on the plot with the crosshair. A tooltip that a
    // click closed did not show again until its pane was made anew.
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");

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

    QtObject {
        id: bridge
        objectName: "readoutBridge"
        property string playbackState: "Paused"
        property int spectrogramViewMode: 0
        // Centred: a right click seeks where the crosshair is.
        property int spectrogramDisplayMode: 1
        property real dbRange: 90
        property bool logScale: false
        property bool spectrogramFrequencyCap: false
        property bool showFps: false
        property int sampleRateHz: 48000
        property bool showSpectrogramCrosshair: true
        property bool showSpectrogramScale: true
        property bool spectrogramZoomEnabled: true
        property bool stretchShortTracks: true
        property int channelButtonsVisibility: 1
        property var soloedChannels: []
        property bool showLevelMeter: false
        property string currentTrackPath: ""
        property var currentTrackChannelLabels: []
        property int seeks: 0
        signal precomputedSpectrogramChannelsReady(int channelCount, bool bufferReset)
        signal playbackChanged()
        function isChannelSilent(channelIndex) { return false }
        function setSpectrogramZoomLevel(level) {}
        function registerSpectrogramItem(item, channelIndex) {}
        function unregisterSpectrogramItem(item) {}
        function setSpectrogramWidgetWidth(width) {}
    }

    Components.UiPalette { id: palette; windowRoot: paletteRoot }

    // Above a strip of the window the pointer leaves the plot for.
    Viewers.SpectrogramSurface {
        objectName: "readoutSpectrogramSurface"
        width: parent.width
        height: 280
        uiBridge: bridge
        uiPalette: palette
        seekCommitted: function(seconds) { ++bridge.seeks }
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.data());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *surface = qobject_cast<QQuickItem *>(
        root->findChild<QObject *>(QStringLiteral("readoutSpectrogramSurface")));
    QObject *bridge = root->findChild<QObject *>(QStringLiteral("readoutBridge"));
    QVERIFY(surface != nullptr);
    QVERIFY(bridge != nullptr);
    SpectrogramItem *pane = nullptr;
    QTRY_VERIFY((pane = qobject_cast<SpectrogramItem *>(findQuickItemByObjectName(
                     surface, QStringLiteral("spectrogramPaneItem")))) != nullptr);
    QVERIFY(findQuickItemByObjectName(surface, QStringLiteral("spectrogramCrosshairToolTip"))
            == nullptr);
    // The plot's plates, from UiPalette's spectrogramPlot.
    QCOMPARE(pane->readoutPlateColor().rgba(), qRgba(20, 20, 20, 224));
    QCOMPARE(pane->readoutPlateEdgeColor().rgba(), qRgba(255, 255, 255, 36));
    QCOMPARE(pane->readoutTextColor().rgba(), qRgba(0xec, 0xec, 0xec, 255));
    QVERIFY(pane->showCrosshairTime());

    constexpr int bins = 4'097;
    constexpr int columns = 2'000;
    pane->feedPrecomputedChunk(QByteArray(bins * columns, '\x80'), bins, 0, columns, 0, columns,
                               48'000, 1'024, true, true, 1, false);
    QTRY_VERIFY(pane->precomputedReady());

    // Both plates show, in the frame the crosshair does.
    const auto platesShown = [&]() {
        QMutexLocker lock(&pane->m_stateMutex);
        return readoutPlatesOf(pane->m_crosshair).size() == 2
            && pane->m_crosshairLaidOutReadout.count(QLatin1Char('\n')) == 1;
    };
    const auto hoverShowsReadout = [&](const QPoint &at) {
        const QString before = pane->crosshairReadout();
        QTest::mouseMove(window, at);
        return QTest::qWaitFor([&]() {
            return platesShown() && !pane->crosshairReadout().isEmpty()
                && pane->crosshairReadout() != before;
        }, 5'000);
    };

    QVERIFY(hoverShowsReadout(QPoint(200, 160)));
    // A right click seeks; the readout shows on, and again on the next move.
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, QPoint(200, 160));
    QTRY_COMPARE(bridge->property("seeks").toInt(), 1);
    QVERIFY(platesShown());
    QVERIFY(hoverShowsReadout(QPoint(420, 100)));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(420, 100));
    QVERIFY(hoverShowsReadout(QPoint(300, 200)));
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, QPoint(300, 200));
    QTRY_COMPARE(bridge->property("seeks").toInt(), 2);
    // Into the bottom right corner.
    QVERIFY(hoverShowsReadout(QPoint(639, 279)));

    // Off the plot, none.
    QTest::mouseMove(window, QPoint(320, 300));
    QTRY_VERIFY(pane->crosshairReadout().isEmpty());
}

void SpectrogramSmokeTest::spectrogramGridOverlayGeneratesWhenEnabled() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setGridEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        // Sync bins from precomputed state (normally done in updatePaintNode).
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateFreqGridOverlayLocked(200, 100);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0);
    }

    QVERIFY(item.m_freqGrid.valid);
    QCOMPARE(item.m_freqGrid.size, QSize(200, 100));
    // A tick at the edge for each label, not a line across the plot.
    QVERIFY(!item.m_freqGrid.lines.isEmpty());
    QCOMPARE(item.m_freqGrid.images.size(), item.m_freqGrid.lines.size());
    for (const QRect &line : item.m_freqGrid.lines) {
        QCOMPARE(line.width(), 6);
        QCOMPARE(line.height(), 1);
    }
    for (const SpectrogramOverlayImage &image : item.m_freqGrid.images) {
        QCOMPARE(image.kind, SpectrogramOverlayImage::ScaleLabel);
    }
    // No label within 34 px of the top, where the lane's label and the mode
    // switch are; nor, with the corner's controls further down, as below a
    // viewer's title strip, within 2 px of their bottom.
    const auto topLabel = [&item](int height) {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(height);
        item.updateFreqGridOverlayLocked(200, height);
        double top = height;
        for (const SpectrogramOverlayImage &image : item.m_freqGrid.images) {
            top = std::min(top, image.rect.top());
        }
        return top;
    };
    bool cornerMattered = false;
    for (int height = 120; height <= 800; height += 20) {
        item.setReservedCorner(QSizeF());
        const double bare = topLabel(height);
        QVERIFY2(bare >= 34.0, qPrintable(QString::number(height)));
        item.setReservedCorner(QSizeF(60, 32));
        QCOMPARE(topLabel(height), bare);
        item.setReservedCorner(QSizeF(60, 65));
        QVERIFY(item.m_freqGridDirty);
        const double belowStrip = topLabel(height);
        QVERIFY2(belowStrip >= 67.0, qPrintable(QStringLiteral("%1: %2").arg(height).arg(belowStrip)));
        cornerMattered = cornerMattered || bare < 67.0;
    }
    // Some lane had a label where the lower corner is.
    QVERIFY(cornerMattered);
    item.setReservedCorner(QSizeF());
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(100);
        item.updateFreqGridOverlayLocked(200, 100);
    }
    // The time scale is the first lane's, along its top.
    QVERIFY(!item.m_timeGrid.valid);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_showTimeLabels = true;
        item.updateTimeGridOverlayLocked(2000, 100, 0, 0, false, 48000.0 / 1024.0, 0.0);
    }
    QVERIFY(item.m_timeGrid.valid);
    QVERIFY(!item.m_timeGrid.lines.isEmpty());
    QCOMPARE(item.m_timeGrid.images.size(), item.m_timeGrid.lines.size());
    // A 5 px tick down from the top and its label under it, every 10 s at
    // zoom 1: 469 px at 46.9 columns a second.
    for (const QRect &line : item.m_timeGrid.lines) {
        QCOMPARE(line.top(), 0);
        QCOMPARE(line.height(), 5);
    }
    QCOMPARE(item.m_timeGrid.images[0].text, QStringLiteral("0:00"));
    QCOMPARE(item.m_timeGrid.images[1].text, QStringLiteral("0:10"));
    QCOMPARE(item.m_timeGrid.images[1].rect.top(), 5.0);
}

void SpectrogramSmokeTest::spectrogramTimeScaleOfAStretchedShortTrackShowsTenths() {
    // A second stretched to 1000 px, 44 samples a column at 48 kHz: a label
    // every half second, with its tenths, rather than one at 0:00.
    SpectrogramItem item;
    item.setWidth(1000);
    item.setHeight(100);
    item.setSampleRateHz(48000);
    item.setGridEnabled(true);
    const double fit = ferrous_ffi_spectrogram_fit_zoom(48'000, 8192, 48'000, 1000);
    QCOMPARE(ferrous_ffi_spectrogram_column_hop(48'000, static_cast<float>(fit)), 44u);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_showTimeLabels = true;
        item.m_precomputedSampleRateHz = 48000;
        item.m_precomputedHopSize = 44;
        item.m_renderZoomLevel = 1024.0 / 44.0;
        item.updateTimeGridOverlayLocked(1000, 100, 0, 0, false, 48000.0 / 44.0, 0.0);
    }
    QVERIFY(item.m_timeGrid.valid);
    QStringList labels;
    for (const auto &image : item.m_timeGrid.images) {
        labels.push_back(image.text);
    }
    QCOMPARE(labels, (QStringList{QStringLiteral("0:00.0"), QStringLiteral("0:00.5")}));
    // Half a second is 545 px in.
    QCOMPARE(item.m_timeGrid.lines.size(), 2);
    QCOMPARE(item.m_timeGrid.lines[1].left(), 545);

    // Zoomed in 16 times, as the wheel goes, labels are half a second apart
    // too; at zoom 1, ten seconds as before.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_precomputedHopSize = 64;
        item.m_renderZoomLevel = 16.0;
        item.updateTimeGridOverlayLocked(1000, 100, 0, 0, false, 48000.0 / 64.0, 0.0);
    }
    QCOMPARE(item.m_timeGrid.images[1].text, QStringLiteral("0:00.5"));
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_precomputedHopSize = 1024;
        item.m_renderZoomLevel = 1.0;
        item.updateTimeGridOverlayLocked(1000, 100, 0, 0, false, 48000.0 / 1024.0, 0.0);
    }
    QCOMPARE(item.m_timeGrid.images[0].text, QStringLiteral("0:00"));
    QCOMPARE(item.m_timeGrid.images[1].text, QStringLiteral("0:10"));
}

void SpectrogramSmokeTest::spectrogramOverlayDisabledProducesNullImage() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    // Both overlays default to disabled.

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    // Crosshair disabled + hover active: null image.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0);
    }
    QVERIFY(!item.m_crosshair.valid);

    // Grid disabled: nothing to draw.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateFreqGridOverlayLocked(200, 100);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0);
    }
    QVERIFY(!item.m_freqGrid.valid);
    QVERIFY(!item.m_timeGrid.valid);
}

void SpectrogramSmokeTest::spectrogramOverlayDirtiedByGeometryChange() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);
    item.setGridEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        // In a frame's order: the crosshair keeps clear of the scale.
        item.updateFreqGridOverlayLocked(200, 100);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0);
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0);
    }
    QVERIFY(!item.m_crosshairDirty);
    QVERIFY(!item.m_freqGridDirty);
    QVERIFY(!item.m_timeGridDirty);

    item.geometryChange(QRectF(0, 0, 300, 150), QRectF(0, 0, 200, 100));

    QVERIFY(item.m_crosshairDirty);
    QVERIFY(item.m_freqGridDirty || item.m_timeGridDirty);
}

void SpectrogramSmokeTest::spectrogramOverlayDirtiedByLogScaleChange() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);
    item.setGridEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        // In a frame's order: the crosshair keeps clear of the scale.
        item.updateFreqGridOverlayLocked(200, 100);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0);
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0);
    }
    QVERIFY(!item.m_crosshairDirty);
    QVERIFY(!item.m_freqGridDirty);
    QVERIFY(!item.m_timeGridDirty);

    item.setLogScale(true);

    QVERIFY(item.m_crosshairDirty);
    QVERIFY(item.m_freqGridDirty || item.m_timeGridDirty);
}

void SpectrogramSmokeTest::spectrogramOverlayStalenessDetectsBinChange() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);
    item.setGridEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0);
        item.updateFreqGridOverlayLocked(200, 100);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0);
    }

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_binsPerColumn = 2049;
    }

    QVERIFY(item.m_binsPerColumn != item.m_crosshairCachedBinsPerColumn);
    QVERIFY(item.m_binsPerColumn != item.m_freqGridCachedBinsPerColumn);
}

void SpectrogramSmokeTest::spectrogramOverlayRebuildsViaUpdatePaintNodeOnStaleInput() {
    SpectrogramItem item;
    // Wide enough for the time scale's ticks, 10 s apart.
    item.setWidth(1200);
    item.setHeight(100);
    item.setSampleRateHz(48000);
    item.setDisplayMode(1); // Centered
    item.setCrosshairEnabled(true);
    item.setGridEnabled(true);
    // The crosshair's time label follows the view.
    item.setShowTimeLabels(true);

    const int bins = 4097;
    const int columns = 3000;
    QByteArray data(bins * columns, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, columns, 0, columns, 48000, 1024,
                               false, true, 1, false);

    setTestPosition(item, 20.0);
    item.setPlaying(false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(100.0, 50.0);
    }

    QSGNode *node = paintSettled(item, nullptr);
    QVERIFY(node != nullptr);

    qint64 firstCrosshairDisplayLeft;
    const QVector<QRectF> firstTimeGridLines = overlayOnScreen(node, 6).lines;
    QVERIFY(!firstTimeGridLines.isEmpty());
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_crosshair.valid);
        QVERIFY(item.m_freqGrid.valid);
        QVERIFY(!item.m_crosshairDirty);
        QVERIFY(!item.m_freqGridDirty);
        QVERIFY(!item.m_timeGridDirty);
        firstCrosshairDisplayLeft = item.m_crosshairCachedDisplayLeft;
    }

    setTestPosition(item, 30.0);

    QSGNode *node2 = paintSettled(item, node);
    QVERIFY(node2 != nullptr);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_crosshair.valid);
        QVERIFY(item.m_freqGrid.valid);
        QVERIFY(item.m_crosshairCachedDisplayLeft != firstCrosshairDisplayLeft);
    }
    // The time grid follows the view.
    QVERIFY(overlayOnScreen(node2, 6).lines != firstTimeGridLines);

    delete node2;
}

void SpectrogramSmokeTest::spectrogramOverlayStalenessDetectsDisplayRangeChange() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setGridEnabled(true);
    item.setCrosshairEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0);
        item.updateFreqGridOverlayLocked(200, 100);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0);
    }

    QCOMPARE(item.m_crosshairCachedDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_timeGridRenderDisplayLeft, static_cast<qint64>(0));
}

void SpectrogramSmokeTest::testMutedChannelRendersGrayscale()
{
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    // Palette is built in the constructor.  Verify that the color palette
    // has colored entries and the gray palette has grayscale entries at
    // the same index (a mid-intensity entry, not the near-black tail).
    constexpr int midIndex = SpectrogramItem::kGradientTableSize / 4;
    const QRgb colorEntry = item.m_palette32[midIndex];
    const QRgb grayEntry = item.m_palette32Gray[midIndex];

    // Color palette entry should have differing R/G/B channels.
    const int cr = qRed(colorEntry), cg = qGreen(colorEntry), cb = qBlue(colorEntry);
    QVERIFY2(!(cr == cg && cg == cb),
             qPrintable(QStringLiteral("Color palette entry at %1 should not be grayscale: R=%2 G=%3 B=%4")
                            .arg(midIndex).arg(cr).arg(cg).arg(cb)));

    // Gray palette entry should have equal R/G/B channels.
    const int gr = qRed(grayEntry), gg = qGreen(grayEntry), gb = qBlue(grayEntry);
    QVERIFY2(gr == gg && gg == gb,
             qPrintable(QStringLiteral("Gray palette entry at %1 should be grayscale: R=%2 G=%3 B=%4")
                            .arg(midIndex).arg(gr).arg(gg).arg(gb)));

    // channelMuted property defaults to false and round-trips.
    QCOMPARE(item.channelMuted(), false);
    item.setChannelMuted(true);
    QCOMPARE(item.channelMuted(), true);
    // Setting the same value again should not re-emit.
    QSignalSpy spy(&item, &SpectrogramItem::channelMutedChanged);
    item.setChannelMuted(true);
    QCOMPARE(spy.count(), 0);
    item.setChannelMuted(false);
    QCOMPARE(spy.count(), 1);
}

void SpectrogramSmokeTest::spectrogramClickToSeekEmitsSignalWhenCrosshairEnabled() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int sampleRate = 48000;
    constexpr int hopSize = 1024;
    constexpr int totalEstimate = 512;

    // Feed enough data so the ring buffer is populated.
    QByteArray chunk(totalEstimate * binsPerColumn, '\0');
    for (int i = 0; i < chunk.size(); ++i) {
        chunk[i] = static_cast<char>(i & 0xFF);
    }
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, totalEstimate, 0, totalEstimate,
        sampleRate, hopSize, true, true, 42);

    // Set centered mode, position at 5 seconds, crosshair enabled.
    item.setDisplayMode(1);
    setTestPosition(item, 5.0);
    item.setCrosshairEnabled(true);

    // Prime the crosshair cache by simulating a hover so cached
    // displayLeft/drawX/rollingMode are initialized.
    QHoverEvent hoverEnter(
        QEvent::HoverEnter, QPointF(100.0, 90.0),
        QPointF(100.0, 90.0), QPointF());
    item.hoverEnterEvent(&hoverEnter);

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);

    // Right-click at pixel X=100.
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress, QPointF(100.0, 90.0),
        QPointF(100.0, 90.0), Qt::RightButton, Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 1);
    const double seekSeconds = seekSpy.at(0).at(0).toDouble();
    // The exact value depends on display layout, but it must be
    // non-negative (valid time).
    QVERIFY(seekSeconds > 0.0);
    QVERIFY(seekSeconds < 5.0);  // Left of center → earlier than playhead
}

void SpectrogramSmokeTest::spectrogramClickToSeekUsesCurrentPositionWhenCrosshairCacheIsStale() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 20'000;
    constexpr int sampleRate = 48'000;
    constexpr int hopSize = 1'024;
    QByteArray chunk(8'000 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        chunk,
        binsPerColumn,
        0,
        8'000,
        0,
        totalEstimate,
        sampleRate,
        hopSize,
        false,
        true,
        42);

    item.setDisplayMode(1);
    item.setCrosshairEnabled(true);
    setTestPosition(item, 10.0);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairCachedDisplayLeft = 0;
        item.m_crosshairCachedDrawX = 0.0;
        item.m_crosshairCachedRollingMode = false;
    }

    setTestPosition(item, 100.0);

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress,
        QPointF(300.0, 90.0),
        QPointF(300.0, 90.0),
        Qt::RightButton,
        Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 1);
    const double seekSeconds = seekSpy.at(0).at(0).toDouble();
    QVERIFY2(
        seekSeconds > 100.0,
        qPrintable(QStringLiteral("stale cache produced backward seek target %1")
            .arg(seekSeconds, 0, 'f', 3)));
}

void SpectrogramSmokeTest::spectrogramClickToSeekIgnoresLaggingDecodedTailInCenteredMode() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int decodedColumns = 4'000;
    constexpr int totalEstimate = 20'000;
    constexpr int sampleRate = 48'000;
    constexpr int hopSize = 1'024;
    QByteArray chunk(decodedColumns * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        chunk,
        binsPerColumn,
        0,
        decodedColumns,
        0,
        totalEstimate,
        sampleRate,
        hopSize,
        false,
        true,
        42);

    item.setDisplayMode(1);
    item.setCrosshairEnabled(true);
    setTestPosition(item, 103.0);

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress,
        QPointF(300.0, 90.0),
        QPointF(300.0, 90.0),
        Qt::RightButton,
        Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 1);
    const double seekSeconds = seekSpy.at(0).at(0).toDouble();
    QVERIFY2(
        seekSeconds > 103.0,
        qPrintable(QStringLiteral("lagging decoded tail produced backward seek target %1")
            .arg(seekSeconds, 0, 'f', 3)));
}

void SpectrogramSmokeTest::spectrogramClickToSeekMatchesTheHeldView_data() {
    QTest::addColumn<double>("scale");
    QTest::addColumn<bool>("steppedBack");
    for (const double scale : {1.0, 2.0}) {
        QTest::addRow("%gx", scale) << scale << false;
        QTest::addRow("%gx, a column back", scale) << scale << true;
    }
}

void SpectrogramSmokeTest::spectrogramClickToSeekMatchesTheHeldView() {
    QFETCH(double, scale);
    QFETCH(bool, steppedBack);
    QQuickWindow window;
    if (scale == 1.0) {
        window.resize(240, 160);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    } else if (!showOnScreen(window, scale)) {
        QSKIP("Needs the offscreen platform's HiDPI test screens");
    }
    QCOMPARE(window.effectiveDevicePixelRatio(), scale);
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    // The pointer is the test's, not the window's.
    item.setAcceptHoverEvents(false);
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    item.setCrosshairEnabled(true);
    item.setShowTimeLabels(true);
    constexpr int bins = 8;
    constexpr int columns = 2000;
    constexpr double columnsPerSecond = 48000.0 / 1024.0;
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x60'), bins, 0, columns,
        0, columns, 48000, 1024, true, true, 1, false, 1);
    // Between columns, so that the centered view scrolls by part of one.
    setTestPosition(item, 10.31);
    const QPointF pointer(73.75, 30.0);
    QHoverEvent hover(QEvent::HoverEnter, pointer, pointer, QPointF());
    item.hoverEnterEvent(&hover);
    QSGNode *node = paintSettled(item, nullptr);
    if (steppedBack) {
        // The clock steps back a column, as a correction of its course
        // can: the view stays at its canvas's columns rather than jitter
        // back and forth.
        qint64 canvasLeft = 0;
        {
            QMutexLocker lock(&item.m_stateMutex);
            canvasLeft = item.m_precomputedCanvasDisplayLeft;
        }
        setTestPosition(item, 10.31 - 1.0 / columnsPerSecond);
        node = paintSettled(item, node);
        QMutexLocker lock(&item.m_stateMutex);
        const SpectrogramItem::CenteredDisplayRange unheld = SpectrogramItem::centeredDisplayRange(
            static_cast<qint64>(std::floor((10.31 - 1.0 / columnsPerSecond) * columnsPerSecond)),
            200, columns, columns, columnsPerSecond);
        QCOMPARE(unheld.left, canvasLeft - 1);
        QCOMPARE(item.m_crosshairCachedDisplayLeft, canvasLeft);
    }

    // The time the crosshair's tooltip shows, on its second line.
    QString shown;
    {
        QMutexLocker lock(&item.m_stateMutex);
        static const QRegularExpression timeLabel(QStringLiteral("^(\\d+):(\\d\\d\\.\\d)$"));
        const QString time = item.m_crosshairLaidOutReadout.section(QLatin1Char('\n'), 1, 1);
        if (timeLabel.match(time).hasMatch()) {
            shown = time;
        }
    }
    QVERIFY(!shown.isEmpty());
    const QStringList parts = shown.split(QLatin1Char(':'));
    const double shownSeconds = parts.at(0).toDouble() * 60.0 + parts.at(1).toDouble();

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);
    QMouseEvent press(QEvent::MouseButtonPress, pointer, pointer,
        Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    item.mousePressEvent(&press);
    QCOMPARE(seekSpy.count(), 1);
    const double seconds = seekSpy.at(0).at(0).toDouble();
    // To the tenth of a second the tooltip shows.
    QVERIFY2(std::abs(seconds - shownSeconds) <= 0.05 + 1e-9,
        qPrintable(QStringLiteral("seeks to %1 s, shows %2").arg(seconds, 0, 'f', 4).arg(shown)));
    delete node;
}

void SpectrogramSmokeTest::spectrogramClickToSeekSuppressedWhenCrosshairDisabled() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 512;
    QByteArray chunk(totalEstimate * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, totalEstimate, 0, totalEstimate,
        48000, 1024, true, true, 42);

    item.setDisplayMode(1);
    setTestPosition(item, 5.0);
    item.setCrosshairEnabled(false);  // Crosshair OFF

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);

    QMouseEvent pressEvent(
        QEvent::MouseButtonPress, QPointF(100.0, 90.0),
        QPointF(100.0, 90.0), Qt::RightButton, Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 0);
}

void SpectrogramSmokeTest::spectrogramLeftClickDoesNotSeek() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 512;
    QByteArray chunk(totalEstimate * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, totalEstimate, 0, totalEstimate,
        48000, 1024, true, true, 42);

    item.setDisplayMode(1);
    setTestPosition(item, 5.0);
    item.setCrosshairEnabled(true);  // Crosshair ON

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);

    // Left-click should NOT seek.
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress, QPointF(100.0, 90.0),
        QPointF(100.0, 90.0), Qt::LeftButton, Qt::LeftButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 0);
}

void SpectrogramSmokeTest::spectrogramClickToSeekDisabledInRollingMode() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int sampleRate = 48000;
    constexpr int hopSize = 1024;
    constexpr int totalEstimate = 512;

    QByteArray chunk(200 * binsPerColumn, '\0');
    for (int i = 0; i < chunk.size(); ++i) {
        chunk[i] = static_cast<char>(i & 0xFF);
    }
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, 200, 0, totalEstimate,
        sampleRate, hopSize, false, true, 42);

    // Rolling mode (default displayMode=0), crosshair enabled.
    // Seek is disabled in rolling mode because the write-order ring
    // buffer history does not realign to the new position.
    item.setDisplayMode(0);
    setTestPosition(item, 3.0);
    item.setCrosshairEnabled(true);

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);

    QMouseEvent pressEvent(
        QEvent::MouseButtonPress, QPointF(50.0, 90.0),
        QPointF(50.0, 90.0), Qt::RightButton, Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 0);
}

void SpectrogramSmokeTest::spectrogramOverlaysDrawNoPaneSizedImages() {
    // The grids and the crosshair of a 3440 x 719 pane at a scale of 2,
    // refreshed as playback, the pointer and a resize refresh them. No
    // overlay image may approach the pane's 6880 x 1438 device pixels, which
    // would take tens of milliseconds of the render sync to draw and upload.
    QQuickWindow window;
    if (!showOnScreen(window, 2.0)) {
        QSKIP("Needs the offscreen platform's HiDPI test screens");
    }
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(3440);
    item.setHeight(719);
    item.setDisplayMode(1);
    item.setPlaying(false);
    item.setGridEnabled(true);
    item.setShowTimeLabels(true);
    item.setCrosshairEnabled(true);
    constexpr int bins = 64;
    constexpr int columns = 20'000;
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x50'), bins, 0, columns,
        0, columns, 48000, 1024, true, true, 1, false, 1);
    setTestPosition(item, 100.0);
    QHoverEvent hover(QEvent::HoverEnter, QPointF(500.0, 300.0), QPointF(500.0, 300.0), QPointF());
    item.hoverEnterEvent(&hover);
    QSGNode *node = paintSettled(item, nullptr);

    const auto largestOverlayTexture = [&node]() {
        // Every texture under the grid and crosshair layers, which are
        // children 4 to 6, the layer nodes included.
        QSize largest;
        std::function<void(QSGNode *)> visit = [&](QSGNode *visited) {
            if (const auto *texture = dynamic_cast<QSGSimpleTextureNode *>(visited);
                texture != nullptr && !texture->rect().isEmpty() && texture->texture() != nullptr) {
                const QSize size = texture->texture()->textureSize();
                // A crosshair line is a pixel or two thick, however long.
                if (std::min(size.width(), size.height()) > 2
                    && size.width() * size.height() > largest.width() * largest.height()) {
                    largest = size;
                }
            }
            for (int i = 0; i < visited->childCount(); ++i) {
                visit(visited->childAtIndex(i));
            }
        };
        for (int layer = 4; layer <= 6; ++layer) {
            visit(node->childAtIndex(layer));
        }
        return largest;
    };
    const auto checkFrame = [&](const char *what) {
        const QSize largest = largestOverlayTexture();
        QVERIFY2(largest.width() <= 400 && largest.height() <= 100,
                 qPrintable(QStringLiteral("%1: a %2 x %3 overlay image")
                     .arg(QLatin1String(what)).arg(largest.width()).arg(largest.height())));
    };
    QVERIFY(!overlayOnScreen(node, 5).lines.isEmpty());
    QVERIFY(!overlayOnScreen(node, 6).images.isEmpty());
    QVERIFY(!overlayOnScreen(node, 7).images.isEmpty());
    checkFrame("first frame");

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_timeGridDirty = true;
    }
    node = item.updatePaintNode(node, nullptr);
    checkFrame("time grid refresh");
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_timeGridDirty = true;
        item.m_freqGridDirty = true;
    }
    node = item.updatePaintNode(node, nullptr);
    checkFrame("both grids' refresh");
    const QPointF to(900.0, 200.0);
    QHoverEvent move(QEvent::HoverMove, to, to, QPointF());
    item.hoverMoveEvent(&move);
    node = item.updatePaintNode(node, nullptr);
    checkFrame("crosshair move");
    item.setHeight(700);
    node = paintSettled(item, node);
    checkFrame("resize");
    delete node;
}

void SpectrogramSmokeTest::spectrogramClickToSeekMatchesCrosshairAtDevicePixels() {
    QQuickWindow window;
    if (!showOnScreen(window, 2.0)) {
        QSKIP("Needs the offscreen platform's HiDPI test screens");
    }
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    item.setCrosshairEnabled(true);
    item.setShowTimeLabels(true);
    constexpr int bins = 8;
    constexpr int columns = 2000;
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x60'), bins, 0, columns,
        0, columns, 48000, 1024, true, true, 1, false, 1);
    // Three quarters into column 483, so that the centered view has
    // scrolled by part of a column, and a whole device pixel of it.
    setTestPosition(item, 10.32);
    const QPointF pointer(73.75, 30.0);
    QHoverEvent hover(QEvent::HoverEnter, pointer, pointer, QPointF());
    item.hoverEnterEvent(&hover);
    QSGNode *node = paintSettled(item, nullptr);

    qint64 displayLeft = 0;
    double drawX = 0.0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        displayLeft = item.m_crosshairCachedDisplayLeft;
        drawX = item.m_crosshairCachedDrawX;
        QVERIFY(displayLeft > 0);
        QVERIFY(drawX < 0.0);
        // Drawn on device column 147, the one under the pointer.
        QCOMPARE(item.m_crosshairVerticalLineRect.left(), 147);
    }

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);
    QMouseEvent press(QEvent::MouseButtonPress, pointer, pointer,
        Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    item.mousePressEvent(&press);
    QCOMPARE(seekSpy.count(), 1);
    const double seconds = seekSpy.at(0).at(0).toDouble();

    // It seeks to the time the crosshair's label shows, which is where the
    // canvas draws device column 147.
    const double columnsPerSecond = 48000.0 / 1024.0;
    QCOMPARE(seconds, (static_cast<double>(displayLeft) + (147 / 2.0 - drawX)) / columnsPerSecond);
    const double column = static_cast<double>(displayLeft) + (147.0 - drawX * 2.0) / 2.0;
    QVERIFY(std::abs(seconds * columnsPerSecond - column) < 1e-9);
    delete node;
}

void SpectrogramSmokeTest::spectrogramSurfaceFollowsTheStretchedDefaultZoom() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");

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
    width: 1000
    height: 200
    property alias bridge: bridge

    QtObject {
        id: bridge
        property string playbackState: "Playing"
        property int spectrogramViewMode: 0
        property int spectrogramDisplayMode: 1
        property real dbRange: 90
        property bool logScale: false
        property bool spectrogramFrequencyCap: false
        property bool showFps: false
        property int sampleRateHz: 48000
        property bool showSpectrogramCrosshair: false
        property bool showSpectrogramScale: true
        property bool spectrogramZoomEnabled: true
        property bool stretchShortTracks: true
        property int channelButtonsVisibility: 1
        property var mutedChannels: []
        property var soloedChannels: []
        property real lastZoomRequest: 0
        property int zoomRequests: 0
        signal precomputedSpectrogramChannelsReady(int channelCount, bool bufferReset)
        signal playbackChanged()
        function isChannelSilent(channelIndex) { return false }
        function setSpectrogramZoomLevel(level) { lastZoomRequest = level; ++zoomRequests }
        function registerSpectrogramItem(item, channelIndex) {}
        function unregisterSpectrogramItem(item) {}
        function setSpectrogramWidgetWidth(width) {}
    }

    Components.UiPalette { id: palette; windowRoot: paletteRoot }

    Viewers.SpectrogramSurface {
        id: surface
        objectName: "spectrogramSurface"
        anchors.fill: parent
        uiBridge: bridge
        uiPalette: palette
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *bridge = qvariant_cast<QObject *>(root->property("bridge"));
    auto *surface = qobject_cast<QQuickItem *>(root->findChild<QObject *>(QStringLiteral("spectrogramSurface")));
    QVERIFY(bridge != nullptr && surface != nullptr);
    SpectrogramItem *item = nullptr;
    QTRY_VERIFY((item = qobject_cast<SpectrogramItem *>(findQuickItemByObjectName(
                     surface, QStringLiteral("spectrogramPaneItem")))) != nullptr);
    QCOMPARE(item->width(), 1000.0);
    const auto zoom = [surface]() { return surface->property("zoomLevel").toDouble(); };
    QCOMPARE(zoom(), 1.0);
    QCOMPARE(surface->property("defaultZoomLevel").toDouble(), 1.0);

    // A second at 48 kHz, which the backend starts at zoom 1 and does not
    // know the length of yet.
    constexpr int bins = 4;
    constexpr quint64 frames = 48'000;
    const int end = static_cast<int>(ferrous_ffi_spectrogram_decoded_end_column(frames, 8192, 1.0f, 48'000));
    item->feedPrecomputedChunk(QByteArray(), bins, 0, 0, 0, end, 48'000, 1024, false, true, 5, true,
                               0, 0, 8192, 0, 0);
    item->feedPrecomputedChunk(QByteArray(16 * bins, '\x40'), bins, 0, 16, 0, end, 48'000, 1024,
                               false, false, 5, false, 0, 0, 8192, 0, 0);
    QCOMPARE(zoom(), 1.0);
    const int requestsBefore = bridge->property("zoomRequests").toInt();

    // Its length arrives: the view follows the stretched default, as a
    // zoom of the wheel does, and the backend restarts for it.
    item->feedPrecomputedChunk(QByteArray(16 * bins, '\x40'), bins, 0, 16, 16, end, 48'000, 1024,
                               false, false, 5, false, 0, 0, 8192, frames, end);
    const double fit = ferrous_ffi_spectrogram_fit_zoom(frames, 8192, 48'000, 1000);
    QVERIFY(fit > 16.0);
    QCOMPARE(surface->property("defaultZoomLevel").toDouble(), fit);
    QCOMPARE(zoom(), fit);
    QCOMPARE(item->zoomLevel(), fit);
    QTRY_VERIFY(bridge->property("zoomRequests").toInt() > requestsBefore);
    QCOMPARE(static_cast<float>(bridge->property("lastZoomRequest").toDouble()), static_cast<float>(fit));

    // The wheel zooms out from it, and back in no further than it.
    const auto wheel = [item](int angle) {
        QWheelEvent event(QPointF(500, 100), QPointF(500, 100), QPoint(), QPoint(0, angle),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        item->wheelEvent(&event);
    };
    wheel(-120);
    QVERIFY(qAbs(zoom() - fit / 1.25) < 1e-9);
    for (int i = 0; i < 3; ++i) {
        wheel(120);
    }
    QCOMPARE(zoom(), fit);

    // A zoom the user chose stays when the default moves.
    QVERIFY(surface->setProperty("_widgetZoomLevel", 3.0));
    QCOMPARE(item->zoomLevel(), 3.0);
    bridge->setProperty("stretchShortTracks", false);
    QCOMPARE(surface->property("defaultZoomLevel").toDouble(), 1.0);
    QCOMPARE(zoom(), 3.0);
    bridge->setProperty("stretchShortTracks", true);
    QCOMPARE(zoom(), 3.0);

    // Reset Zoom returns to the stretched default, which follows the
    // setting and the plot's width from then on.
    QVERIFY(QMetaObject::invokeMethod(surface, "resetZoom"));
    QCOMPARE(zoom(), fit);
    QCOMPARE(item->zoomLevel(), fit);
    bridge->setProperty("stretchShortTracks", false);
    QCOMPARE(zoom(), 1.0);
    QCOMPARE(item->zoomLevel(), 1.0);
    bridge->setProperty("stretchShortTracks", true);
    QCOMPARE(zoom(), fit);
    root->setProperty("width", 1500);
    QTRY_COMPARE(item->width(), 1500.0);
    const double wider = ferrous_ffi_spectrogram_fit_zoom(frames, 8192, 48'000, 1500);
    QVERIFY(wider > fit);
    QCOMPARE(zoom(), wider);
    QCOMPARE(item->zoomLevel(), wider);
}

std::unique_ptr<QmlSmokeTest> makeSpectrogramSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<SpectrogramSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_spectrogram.moc"
