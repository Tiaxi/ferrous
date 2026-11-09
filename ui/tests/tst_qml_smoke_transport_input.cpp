// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

// Playback controls, keyboard and wheel input, and fullscreen viewers.
class TransportInputSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void fullscreenVisualizationInhibitsDisplaySleepOnlyWhileActive();
    void playbackControllerReadsTheBridgeClock();
    void mainWindowStopsPositionFramesWhileHidden();
    void albumArtZoomFollowsTheWheelDelta();
    void albumArtZoomKeepsTheImagePointUnderThePointer_data();
    void albumArtZoomKeepsTheImagePointUnderThePointer();
    void albumArtSmallerThanTheRequestIsShownScaledUp();
    void albumArtViewerShowsThePreloadedCover_data();
    void albumArtViewerShowsThePreloadedCover();
    void spaceTogglesPlaybackWhenTheLibraryHasFocus();
    void spaceTypesIntoTextFields();
    void playbackShortcutsWorkInTheViewers();
    void viewerShortcutsOpenAndCloseTheViewers();
    void fullscreenViewerWindowsAreMadeOnFirstOpenAndKept();
    void visualizationModeSwitchTakesKeyboardFocus();
    void visualizationModeSwitchStaysClearOfTheCornerOverlays();
    void visualizationModeSwitchShowsOnlyWithThePointerOrFocus();
    void visualizationModeSwitchLeavesTheCornerToTheView();
    void viewersShowTheTrackInATitleStripOverTheWholeScreen();
    void fullscreenViewersGiveTheViewTheWholeScreen();
    void fullscreenVisualizationKeepsItsControlsBelowTheStrip();
    void viewersInTheMainWindowAreNamedInItsTitle();
    void viewerZoomShowsAReadoutAndAResetButton();
    void viewerControlsComeBackForTheKeyboard();
    void viewerKeysWorkFromEveryControl();
    void itunesPreviewAppliesOrGoesBackToTheResults();
    void itunesPreviewButtonsTakeSpaceFromThePlaybackShortcut();
    void itunesPreviewButtonsStackWhenTheyDoNotFitSideBySide();
    void itunesPreviewCannotApplyACandidateThatIsNotReady();
    void itunesPreviewShowsTheSearchedAlbumsArtAfterPlaybackMovesOn();
    void itunesPreviewApplyShowsTheAppliedCoverAfterPlaybackMovesOn();
    void viewerControlsStayWhileOneHasFocus();
    void albumArtDetailsLieOverTheArt();
    void albumArtViewerShowsWhenItsImageCannotBeRead();
    void albumArtInfoButtonIsTintedOnlyWhileChecked();
    void fullscreenViewersCanKeepTheirTitleStripHidden();
    void albumArtStaysPutWhenTheDetailsOpen_data();
    void albumArtStaysPutWhenTheDetailsOpen();
    void menuMnemonicsOpenTheMenusAndShowShortcuts();
};

void TransportInputSmokeTest::fullscreenVisualizationInhibitsDisplaySleepOnlyWhileActive() {
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

    QtObject {
        id: sleepInhibitor
        objectName: "sleepInhibitor"
        property bool inhibited: false
        property int updateCount: 0

        function setInhibited(value) {
            if (inhibited === value) {
                return
            }
            inhibited = value
            updateCount += 1
        }
    }

    Viewers.SpectrogramViewerShell {
        objectName: "spectrogramViewerShell"
        anchors.fill: parent
        windowRoot: windowRoot
        uiPalette: Components.UiPalette { windowRoot: windowRoot }
        viewerOpen: false
        useWholeScreenViewerMode: true
        titleText: "Ferrous"
        closeViewer: function() {}
        displaySleepInhibitor: sleepInhibitor
        displaySleepInhibitionEnabled: true
        playbackPlaying: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *shell = root->findChild<QObject *>(QStringLiteral("spectrogramViewerShell"));
    QObject *sleepInhibitor = root->findChild<QObject *>(QStringLiteral("sleepInhibitor"));
    QVERIFY(shell != nullptr);
    QVERIFY(sleepInhibitor != nullptr);
    QVERIFY(!sleepInhibitor->property("inhibited").toBool());

    // A fullscreen view that is not playing must not keep the screen awake.
    QVERIFY(shell->setProperty("viewerOpen", true));
    QTRY_VERIFY(shell->property("fullscreenDisplayActive").toBool());
    QTest::qWait(20);
    QVERIFY(!sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 0);

    QVERIFY(shell->setProperty("playbackPlaying", true));
    QTRY_VERIFY(sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 1);

    QVERIFY(shell->setProperty("playbackPlaying", false));
    QTRY_VERIFY(!sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 2);
    QVERIFY(shell->property("fullscreenDisplayActive").toBool());

    QVERIFY(shell->setProperty("playbackPlaying", true));
    QTRY_VERIFY(sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 3);

    QVERIFY(shell->setProperty("displaySleepInhibitionEnabled", false));
    QTRY_VERIFY(!sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 4);

    QVERIFY(shell->setProperty("displaySleepInhibitionEnabled", true));
    QTRY_VERIFY(sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 5);

    QVERIFY(shell->setProperty("useWholeScreenViewerMode", false));
    QTRY_VERIFY(!shell->property("fullscreenDisplayActive").toBool());
    QTRY_VERIFY(!sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 6);

    QVERIFY(shell->setProperty("useWholeScreenViewerMode", true));
    QTRY_VERIFY(sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 7);

    QVERIFY(shell->setProperty("viewerOpen", false));
    QTRY_VERIFY(!sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 8);
}

void TransportInputSmokeTest::playbackControllerReadsTheBridgeClock() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property var playbackClock: null
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
        property var seekCalls: []
        function seek(value) { seekCalls = seekCalls.concat([value]) }
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QObject *bridge = root->findChild<QObject *>(QStringLiteral("bridge"));
    QVERIFY(controller != nullptr);
    QVERIFY(bridge != nullptr);
    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QCOMPARE(controller->property("positionSeconds").toDouble(), 0.0);
    QCOMPARE(controller->property("positionText").toString(), QStringLiteral("00:00"));

    // The controller shows the bridge's clock rather than a value of its own.
    PlaybackClock clock;
    bridge->setProperty("playbackClock", QVariant::fromValue(static_cast<QObject *>(&clock)));
    clock.holdAt(72.0);
    QCOMPARE(controller->property("positionSeconds").toDouble(), 72.0);
    QCOMPARE(controller->property("positionText").toString(), QStringLiteral("01:12"));

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekCommitted",
        Q_ARG(QVariant, QVariant::fromValue(48.0))));
    QCOMPARE(bridge->property("seekCalls").toList().size(), 1);

    // A stopped track is not seekable: no seek is sent.
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekCommitted",
        Q_ARG(QVariant, QVariant::fromValue(90.0))));
    QCOMPARE(bridge->property("seekCalls").toList().size(), 1);

    // The shown position moves by the step the view asks for; a relative
    // seek starts from the position now.
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    qint64 now = 5'000'000'000'000LL;
    clock.setTimeSource([&now]() { return now; });
    controller->setProperty("positionStepSeconds", 0.5);
    QCOMPARE(clock.positionStepSeconds(), 0.5);
    clock.applySample(PlaybackSample{72.0, now, true, 180.0, 1}, true);
    now += 200'000'000LL;
    QCOMPARE(controller->property("positionSeconds").toDouble(), 72.0);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekBy",
        Q_ARG(QVariant, QVariant::fromValue(10.0))));
    const QVariantList seekCalls = bridge->property("seekCalls").toList();
    QCOMPARE(seekCalls.size(), 2);
    QVERIFY(std::abs(seekCalls.last().toDouble() - 82.2) < 1e-9);
}

void TransportInputSmokeTest::mainWindowStopsPositionFramesWhileHidden() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));
    engine.load(QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml")));
    QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    // The fallback bridge has no clock; give it one that plays.
    PlaybackClock clock;
    auto *bridge = window->property("uiBridge").value<QObject *>();
    QVERIFY(bridge != nullptr);
    QVERIFY(bridge->setProperty("playbackClock", QVariant::fromValue(&clock)));
    PlaybackSample sample;
    sample.positionSeconds = 10.0;
    sample.sampledAtNs = monotonicNowNs();
    sample.advancing = true;
    sample.durationSeconds = 600.0;
    sample.trackToken = 1;
    clock.applySample(sample, true);
    QSignalSpy position(&clock, &PlaybackClock::positionChanged);
    const auto framesTick = [&position]() {
        position.clear();
        return QTest::qWaitFor([&position]() { return position.count() >= 3; }, 1000);
    };
    const auto framesStopped = [&position]() {
        position.clear();
        QTest::qWait(150);
        return position.isEmpty();
    };
    QTRY_VERIFY(clock.frameUpdatesEnabled());
    QVERIFY(framesTick());

    window->hide();
    QTRY_VERIFY(!clock.frameUpdatesEnabled());
    QVERIFY(framesStopped());
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QTRY_VERIFY(clock.frameUpdatesEnabled());
    QVERIFY(framesTick());

    window->showMinimized();
    if (window->visibility() == QWindow::Minimized) {
        QTRY_VERIFY(!clock.frameUpdatesEnabled());
        QVERIFY(framesStopped());
        window->showNormal();
        QTRY_VERIFY(clock.frameUpdatesEnabled());
        QVERIFY(framesTick());
    } else {
        qInfo("The %s platform does not minimize windows",
            qPrintable(QGuiApplication::platformName()));
    }
    QVERIFY(bridge->setProperty("playbackClock", QVariant()));
}

void TransportInputSmokeTest::albumArtZoomFollowsTheWheelDelta() {
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

Window {
    id: surfaceWindow
    width: 400
    height: 400
    visible: true
    property alias surface: surface

    Action { id: replaceFromItunesAction }

    Viewers.AlbumArtSurface {
        id: surface
        anchors.fill: parent
        viewerOpen: true
        viewerSource: ""
        infoVisible: false
        initialViewToken: 0
        viewerDecodeWidth: 400
        viewerDecodeHeight: 400
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        closeViewer: function() {}
        focusFullscreen: function() {}
        comparisonModeAvailable: false
        uiPalette: Components.UiPalette { windowRoot: surfaceWindow }
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QObject *surface = qvariant_cast<QObject *>(root->property("surface"));
    QVERIFY(surface != nullptr);
    const QPointF center(200, 200);
    const auto zoom = [surface]() { return surface->property("zoom").toDouble(); };
    const double perNotch = surface->property("zoomPerNotch").toDouble();
    QVERIFY(perNotch > 1.0);

    // An eighth of a notch zooms by an eighth of a notch's factor.
    QVERIFY(sendWheel(window, center, QPoint(), QPoint(0, 15)));
    QVERIFY(std::abs(zoom() - std::pow(perNotch, 0.125)) < 1e-9);
    double expected = zoom();
    QVERIFY(sendWheel(window, center, QPoint(), QPoint(0, 360)));
    expected *= std::pow(perNotch, 3.0);
    QVERIFY(std::abs(zoom() - expected) < 1e-9);

    // A pixel-only event counts by its pixels.
    QVERIFY(sendWheel(window, center, QPoint(0, 60), QPoint()));
    expected *= std::pow(perNotch, 0.5);
    QVERIFY(std::abs(zoom() - expected) < 1e-9);

    // Events with no vertical delta leave the zoom alone.
    sendWheel(window, center, QPoint(), QPoint(120, 0));
    QVERIFY(std::abs(zoom() - expected) < 1e-9);

    // Zooming out is as fine-grained, down to the fitted size.
    QVERIFY(sendWheel(window, center, QPoint(), QPoint(0, -15)));
    expected /= std::pow(perNotch, 0.125);
    QVERIFY(std::abs(zoom() - expected) < 1e-9);
    QVERIFY(sendWheel(window, center, QPoint(), QPoint(0, -1200)));
    QCOMPARE(zoom(), 1.0);
}

void TransportInputSmokeTest::albumArtZoomKeepsTheImagePointUnderThePointer_data() {
    QTest::addColumn<int>("viewportWidth");
    // The square cover fills a square viewer, so zooming at any point on it
    // keeps that point under the pointer.
    QTest::newRow("cover fills the viewer") << 400;
    // In a wider viewer the cover leaves bands at its sides, and the pan
    // limits stop it short of keeping points near its sides in place.
    QTest::newRow("cover narrower than the viewer") << 600;
}

void TransportInputSmokeTest::albumArtZoomKeepsTheImagePointUnderThePointer() {
    QFETCH(int, viewportWidth);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(400, 400, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(coverPath));

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("covers"), new CoverImageProvider);
    engine.rootContext()->setContextProperty(
        QStringLiteral("testCoverUrl"), CoverImageProvider::urlForPath(coverPath));
    engine.rootContext()->setContextProperty(QStringLiteral("testViewportWidth"), viewportWidth);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "components" as Components
import "viewers" as Viewers

Window {
    id: surfaceWindow
    width: testViewportWidth
    height: 400
    visible: true
    property alias surface: surface
    property int closeCount: 0

    Action { id: replaceFromItunesAction }

    Viewers.AlbumArtSurface {
        id: surface
        anchors.fill: parent
        viewerOpen: true
        viewerSource: testCoverUrl
        infoVisible: false
        initialViewToken: 0
        viewerDecodeWidth: 400
        viewerDecodeHeight: 400
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        closeViewer: function() { closeCount += 1 }
        focusFullscreen: function() {}
        comparisonModeAvailable: false
        uiPalette: Components.UiPalette { windowRoot: surfaceWindow }
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *surface = qvariant_cast<QQuickItem *>(root->property("surface"));
    QVERIFY(surface != nullptr);
    QList<QQuickItem *> items;
    collectQuickItems(surface, &items);
    QQuickItem *image = nullptr;
    for (QQuickItem *each : items) {
        if (each->inherits("QQuickImage")) {
            image = each;
            break;
        }
    }
    QVERIFY(image != nullptr);
    // Image.Ready is 1.
    QTRY_COMPARE_WITH_TIMEOUT(image->property("status").toInt(), 1, 5000);
    // The image fills the item that is panned and zoomed, inside the viewport.
    QQuickItem *transform = image->parentItem();
    QVERIFY(transform != nullptr);
    QQuickItem *viewport = transform->parentItem();
    QVERIFY(viewport != nullptr);
    QCOMPARE(viewport->size(), QSizeF(viewportWidth, 400));
    QCOMPARE(transform->size(), QSizeF(400, 400));

    const auto zoom = [surface]() { return surface->property("zoom").toDouble(); };
    const auto pan = [surface]() {
        return QPointF(surface->property("panX").toDouble(), surface->property("panY").toDouble());
    };
    const QPointF viewportCenter(viewport->width() / 2, viewport->height() / 2);
    const QPointF imageCenter(image->width() / 2, image->height() / 2);
    const auto verifyFitted = [&]() -> QString {
        const QPointF shownCenter = image->mapToItem(viewport, imageCenter);
        if (zoom() != 1.0 || pan() != QPointF() || shownCenter != viewportCenter) {
            return QStringLiteral("not fitted: zoom %1, pan %2,%3, centre at %4,%5")
                .arg(zoom())
                .arg(pan().x())
                .arg(pan().y())
                .arg(shownCenter.x())
                .arg(shownCenter.y());
        }
        return {};
    };

    int keptAxes = 0;
    int clampedAxes = 0;
    // One axis of a zoom at `pointer`: the image point that was under the
    // pointer now shows at `shown`, with the pan at `panNow` (`panBefore`
    // before the zoom) and limited to +-`limit`.
    const auto verifyAxis = [&](char axis, double pointer, double shown, double panNow,
                                double panBefore, double limit) -> QString {
        const double drift = shown - pointer;
        // Panning moves the image one to one, so this pan would bring the
        // point back under the pointer.
        const double needed = panNow - drift;
        if (std::abs(needed) <= limit + 0.5) {
            ++keptAxes;
            if (std::abs(drift) > 0.5) {
                return QStringLiteral("%1 drifted %2 px from the pointer at zoom %3")
                    .arg(axis)
                    .arg(drift)
                    .arg(zoom());
            }
            return {};
        }
        // The pan limits stop the point short of the pointer: the image
        // still moves as far toward it as they allow.
        ++clampedAxes;
        if (std::abs(std::abs(panNow) - limit) > 1e-6 || panNow * needed < 0) {
            return QStringLiteral("%1 pan %2 is not at the limit %3 toward the pointer (needs %4)")
                .arg(axis)
                .arg(panNow)
                .arg(limit)
                .arg(needed);
        }
        const double driftUnpanned = drift - (panNow - std::clamp(panBefore, -limit, limit));
        if (std::abs(drift) > std::abs(driftUnpanned) + 1e-6) {
            return QStringLiteral("%1 moved away from the pointer: drift %2, %3 without panning")
                .arg(axis)
                .arg(drift)
                .arg(driftUnpanned);
        }
        return {};
    };
    const auto verifyZoom = [&](QPointF pointer, QPointF imagePoint, QPointF panBefore) -> QString {
        const QPointF shown = image->mapToItem(viewport, imagePoint);
        const QPointF panNow = pan();
        const double limitX = std::max(0.0, (transform->width() * zoom() - viewport->width()) / 2);
        const double limitY = std::max(0.0, (transform->height() * zoom() - viewport->height()) / 2);
        QString failure = verifyAxis('x', pointer.x(), shown.x(), panNow.x(), panBefore.x(), limitX);
        if (failure.isEmpty()) {
            failure = verifyAxis('y', pointer.y(), shown.y(), panNow.y(), panBefore.y(), limitY);
        }
        if (!failure.isEmpty()) {
            failure += QStringLiteral(" (pointer %1,%2)").arg(pointer.x()).arg(pointer.y());
        }
        return failure;
    };

    // Near each corner of the cover, and at its centre.
    const QRectF coverRect = transform->mapRectToItem(viewport, QRectF(QPointF(), transform->size()));
    const QRectF inset = coverRect.adjusted(10, 10, -10, -10);
    const QList<QPointF> pointers{
        inset.topLeft(), inset.topRight(), inset.bottomLeft(), inset.bottomRight(), inset.center()};
    QVERIFY2(verifyFitted().isEmpty(), qPrintable(verifyFitted()));

    for (const QPointF &pointer : pointers) {
        const QPointF scenePointer = viewport->mapToItem(window->contentItem(), pointer);
        // Zoom in a notch at a time, then back out to the fitted view.
        for (const int delta : {120, -120}) {
            for (int step = 0; step < 12 && (delta > 0 || zoom() > 1.0); ++step) {
                const QPointF imagePoint = image->mapFromItem(viewport, pointer);
                const QPointF panBefore = pan();
                const double zoomBefore = zoom();
                QVERIFY(sendWheel(window, scenePointer, QPoint(), QPoint(0, delta)));
                QVERIFY(delta > 0 ? zoom() > zoomBefore : zoom() < zoomBefore);
                const QString failure = verifyZoom(pointer, imagePoint, panBefore);
                QVERIFY2(failure.isEmpty(), qPrintable(failure));
            }
        }
        QVERIFY2(verifyFitted().isEmpty(), qPrintable(verifyFitted()));
    }

    for (const QPointF &pointer : pointers) {
        const QPoint scenePointer = viewport->mapToItem(window->contentItem(), pointer).toPoint();
        // A double-click zooms to twice the fitted size at the pointer.
        const QPointF imagePoint = image->mapFromItem(viewport, pointer);
        const QPointF panBefore = pan();
        QTest::mouseMove(window, scenePointer);
        QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, scenePointer);
        QCOMPARE(zoom(), 2.0);
        const QString failure = verifyZoom(pointer, imagePoint, panBefore);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        // Another fits the cover again.
        QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, scenePointer);
        QVERIFY2(verifyFitted().isEmpty(), qPrintable(verifyFitted()));
    }
    QCOMPARE(root->property("closeCount").toInt(), 0);

    QVERIFY(keptAxes > 0);
    if (viewportWidth == 400) {
        QCOMPARE(clampedAxes, 0);
    } else {
        QVERIFY(clampedAxes > 0);
    }
}

void TransportInputSmokeTest::albumArtSmallerThanTheRequestIsShownScaledUp() {
    // The cover provider decodes a cover smaller than the requested size at
    // its own size; the album art viewer and the tile still show it fitted
    // to their area, as they did when it was enlarged on decoding.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(300, 200, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(coverPath));

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("covers"), new CoverImageProvider);
    engine.rootContext()->setContextProperty(
        QStringLiteral("testCoverUrl"), CoverImageProvider::urlForPath(coverPath));
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "components" as Components
import "components" as Components
import "viewers" as Viewers

Window {
    id: surfaceWindow
    width: 1200
    height: 600
    visible: true
    property alias surface: surface
    property alias tile: tile

    Action { id: replaceFromItunesAction }
    QtObject {
        id: bridge
        property string currentTrackCoverPath: testCoverUrl
    }

    Viewers.AlbumArtSurface {
        id: surface
        anchors.fill: parent
        viewerOpen: true
        viewerSource: testCoverUrl
        infoVisible: false
        initialViewToken: 0
        viewerDecodeWidth: 1200
        viewerDecodeHeight: 600
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        closeViewer: function() {}
        focusFullscreen: function() {}
        comparisonModeAvailable: false
        uiPalette: Components.UiPalette { windowRoot: surfaceWindow }
    }

    Components.AlbumArtTile {
        id: tile
        width: 320
        height: 320
        uiBridge: bridge
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        openAlbumArtViewer: function() {}
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));

    const auto imageIn = [&root](const char *name) -> QQuickItem * {
        auto *item = qvariant_cast<QQuickItem *>(root->property(name));
        QList<QQuickItem *> items;
        if (item != nullptr) {
            collectQuickItems(item, &items);
        }
        for (QQuickItem *each : items) {
            if (each->inherits("QQuickImage")) {
                return each;
            }
        }
        return nullptr;
    };
    struct Expected {
        const char *view;
        QSizeF painted;
        QPointF paintedCenter;
    };
    // The viewer fits the cover to its 1200x600 area, centred; the tile to
    // its 320x320 square.
    const QList<Expected> views{
        {"surface", QSizeF(900, 600), QPointF(600, 300)},
        {"tile", QSizeF(320, 320.0 * 2 / 3), QPointF(160, 160)},
    };
    for (const Expected &expected : views) {
        QQuickItem *image = imageIn(expected.view);
        QVERIFY2(image != nullptr, expected.view);
        // Image.Ready is 1.
        QTRY_COMPARE_WITH_TIMEOUT(image->property("status").toInt(), 1, 5000);
        QCOMPARE(QSize(image->property("implicitWidth").toInt(),
                       image->property("implicitHeight").toInt()),
                 QSize(300, 200));
        const QSizeF painted(image->property("paintedWidth").toReal(),
                             image->property("paintedHeight").toReal());
        QVERIFY2(std::abs(painted.width() - expected.painted.width()) < 0.5
                     && std::abs(painted.height() - expected.painted.height()) < 0.5,
                 qPrintable(QStringLiteral("%1 painted %2x%3")
                                .arg(QLatin1String(expected.view))
                                .arg(painted.width())
                                .arg(painted.height())));
        const QPointF center = image->mapToScene(
            QPointF(image->width() / 2, image->height() / 2));
        QCOMPARE(center, expected.paintedCenter);
    }
}

namespace {

// The cover provider, recording the size of every image it is asked for.
class RecordingCoverImageProvider final : public CoverImageProvider {
public:
    QQuickImageResponse *requestImageResponse(
        const QString &id, const QSize &requestedSize) override {
        {
            QMutexLocker locker(&m_mutex);
            m_requests.append(requestedSize);
        }
        return CoverImageProvider::requestImageResponse(id, requestedSize);
    }

    QList<QSize> requests() const {
        QMutexLocker locker(&m_mutex);
        return m_requests;
    }

private:
    mutable QMutex m_mutex;
    QList<QSize> m_requests;
};

} // namespace

void TransportInputSmokeTest::albumArtViewerShowsThePreloadedCover_data() {
    QTest::addColumn<double>("scale");
    QTest::addColumn<int>("viewerMode");
    QTest::newRow("popup") << 1.0 << 0;
    QTest::newRow("popup at 2x") << 2.0 << 0;
    QTest::newRow("whole screen") << 1.0 << 1;
}

void TransportInputSmokeTest::albumArtViewerShowsThePreloadedCover() {
    // The hidden preload decodes the current cover as the album art viewer
    // asks for it, so that opening the viewer decodes nothing more: the
    // cover is decoded once at the viewer's size.
    QFETCH(double, scale);
    QFETCH(int, viewerMode);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(300, 200, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(coverPath));

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    auto *provider = new RecordingCoverImageProvider;
    engine.addImageProvider(QStringLiteral("covers"), provider);
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    if (scale != 1.0) {
        QScreen *screen = hiDpiTestScreen(scale);
        if (screen == nullptr) {
            QSKIP("Needs the offscreen platform's HiDPI test screen at that scale");
        }
        QVERIFY(moveWindowToScreen(*window, screen));
    }
    if (std::abs(window->effectiveDevicePixelRatio() - scale) > 1e-9) {
        QSKIP("The window is at another scale");
    }
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *preload = window->findChild<QQuickItem *>(QStringLiteral("albumArtViewerPreloadImage"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && preload != nullptr && surface != nullptr);
    QQuickItem *viewerImage = nullptr;
    QList<QQuickItem *> surfaceItems;
    collectQuickItems(surface, &surfaceItems);
    for (QQuickItem *each : surfaceItems) {
        if (each->inherits("QQuickImage")) {
            viewerImage = each;
            break;
        }
    }
    QVERIFY(viewerImage != nullptr);
    bridge->setProperty("viewerFullscreenMode", viewerMode);

    // Image.Ready is 1. The implicit size is in logical pixels.
    const auto readyWithTheCover = [scale](QQuickItem *image) {
        return image->property("status").toInt() == 1
            && QSizeF(image->property("implicitWidth").toReal(),
                      image->property("implicitHeight").toReal()) * scale == QSizeF(300, 200);
    };
    bridge->setProperty("currentTrackCoverPath", CoverImageProvider::urlForPath(coverPath));
    QTRY_VERIFY_WITH_TIMEOUT(readyWithTheCover(preload), 5000);

    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewerForCurrentArt"));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(readyWithTheCover(viewerImage), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(readyWithTheCover(preload), 5000);
    QCOMPARE(viewerImage->property("sourceSize"), preload->property("sourceSize"));
    QCOMPARE(viewerImage->window()->effectiveDevicePixelRatio(), scale);

    // Give a second request time to reach the provider before counting.
    QTest::qWait(100);
    const QSize viewerRequest =
        viewerImage->property("sourceSize").toSize() * viewerImage->window()->effectiveDevicePixelRatio();
    const QList<QSize> requests = provider->requests();
    QVERIFY2(requests.count(viewerRequest) == 1,
             qPrintable(QStringLiteral("%1 requests at %2x%3")
                            .arg(requests.count(viewerRequest))
                            .arg(viewerRequest.width())
                            .arg(viewerRequest.height())));

    QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
}

void TransportInputSmokeTest::spaceTogglesPlaybackWhenTheLibraryHasFocus() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    libraryModel.setExpandedKeys(QStringList{
        QStringLiteral("artist|Artist A"),
        QStringLiteral("album|Artist A|Album A"),
    });
    libraryModel.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary());
    QTRY_COMPARE(libraryModel.rowCount(), 3);
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    QVERIFY(playPause != nullptr);
    QCOMPARE(playPause->property("shortcut").value<QKeySequence>(), QKeySequence(Qt::Key_Space));
    QSignalSpy toggled(playPause, SIGNAL(triggered(QObject*)));
    auto *libraryView = qvariant_cast<QQuickItem *>(window->property("libraryViewRef"));
    QVERIFY(libraryView != nullptr);
    QVERIFY(QMetaObject::invokeMethod(libraryView, "forceActiveFocus"));
    QTRY_VERIFY(libraryView->hasActiveFocus());
    QObject *library = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(library != nullptr);
    QVERIFY(QMetaObject::invokeMethod(library, "selectIndex", Q_ARG(QVariant, 0)));

    QTest::keyClick(window, Qt::Key_Space);
    QCOMPARE(toggled.count(), 1);
    // Space no longer folds the selected artist.
    QCOMPARE(libraryModel.rowCount(), 3);
    QTest::keyClick(window, Qt::Key_Space);
    QCOMPARE(toggled.count(), 2);
}

void TransportInputSmokeTest::spaceTypesIntoTextFields() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    QObject *seekBackward = window->findChild<QObject *>(QStringLiteral("seekBackwardAction"));
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(playPause != nullptr && seekBackward != nullptr && bridge != nullptr);
    QSignalSpy toggled(playPause, SIGNAL(triggered(QObject*)));
    QSignalSpy seeked(seekBackward, SIGNAL(triggered(QObject*)));
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    bridge->setProperty("durationSeconds", 100.0);
    QObject *search = nullptr;
    for (QObject *action : window->findChildren<QObject *>()) {
        if (action->property("text").toString() == QStringLiteral("&Find…")) {
            search = action;
            break;
        }
    }
    QVERIFY(search != nullptr);
    QVERIFY(QMetaObject::invokeMethod(search, "trigger"));
    auto *field = window->findChild<QQuickItem *>(QStringLiteral("globalSearchQueryField"));
    QVERIFY(field != nullptr);
    QTRY_VERIFY(field->hasActiveFocus());

    for (const char key : {'a', ' ', 'b'}) {
        QTest::keyClick(window, key);
    }
    QTRY_COMPARE(field->property("text").toString(), QStringLiteral("a b"));
    QCOMPARE(toggled.count(), 0);
    // Shift+arrows select in the field rather than seek.
    QTest::keyClick(window, Qt::Key_Left, Qt::ShiftModifier);
    QCOMPARE(seeked.count(), 0);
    QCOMPARE(field->property("selectedText").toString(), QStringLiteral("b"));
    QTest::keyClick(window, Qt::Key_Escape);
}

void TransportInputSmokeTest::playbackShortcutsWorkInTheViewers() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    QObject *stop = window->findChild<QObject *>(QStringLiteral("stopAction"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && playPause != nullptr
            && stop != nullptr && surface != nullptr);
    QSignalSpy toggled(playPause, SIGNAL(triggered(QObject*)));
    QSignalSpy stopped(stop, SIGNAL(triggered(QObject*)));
    // No action takes Ctrl+Space, which switches input methods in Fcitx
    // and IBus, or Ctrl+M, which shows the menu bar in KDE applications.
    for (QObject *object : window->findChildren<QObject *>()) {
        const QKeySequence shortcut = object->property("shortcut").value<QKeySequence>();
        QVERIFY2(shortcut != QKeySequence(QStringLiteral("Ctrl+Space"))
                     && shortcut != QKeySequence(QStringLiteral("Ctrl+M")),
                 qPrintable(object->property("text").toString()));
    }
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    bridge->setProperty("durationSeconds", 100.0);

    // The viewer as a modal popup inside the main window.
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    QVERIFY(popup != nullptr);
    QTRY_VERIFY(popup->property("opened").toBool());
    QTest::keyClick(window, Qt::Key_Space);
    QCOMPARE(toggled.count(), 1);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(stopped.count(), 1);
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(bridge->property("lastSeekSeconds").toDouble(), 5.0);
    QTest::keyClick(window, Qt::Key_Down, Qt::AltModifier);
    QVERIFY(std::abs(bridge->property("volume").toDouble() - 0.95) < 1e-9);
    QTest::keyClick(window, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(bridge->property("volume").toDouble(), 0.0);
    QTest::keyClick(window, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
    QVERIFY(std::abs(bridge->property("volume").toDouble() - 0.95) < 1e-9);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());

    // The viewer as a window of its own over the whole screen.
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(surface->window() != nullptr && surface->window() != window);
    QQuickWindow *fullscreen = surface->window();
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    QTest::keyClick(fullscreen, Qt::Key_Space);
    QCOMPARE(toggled.count(), 2);
    QTest::keyClick(fullscreen, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(stopped.count(), 2);
    QTest::keyClick(fullscreen, Qt::Key_Left, Qt::ShiftModifier);
    QCOMPARE(bridge->property("lastSeekSeconds").toDouble(), 0.0);
    QTest::keyClick(fullscreen, Qt::Key_Up, Qt::AltModifier);
    QVERIFY(std::abs(bridge->property("volume").toDouble() - 1.0) < 1e-9);
    QTest::keyClick(fullscreen, Qt::Key_Escape);
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
}

void TransportInputSmokeTest::viewerShortcutsOpenAndCloseTheViewers() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(16, 16, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(coverPath));

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    QObject *albumArtPopup = window->findChild<QObject *>(QStringLiteral("albumArtViewerPopup"));
    QVERIFY(bridge != nullptr && viewer != nullptr && surface != nullptr
            && popup != nullptr && albumArtPopup != nullptr);
    const auto spectrogramOpen = [viewer]() {
        return viewer->property("spectrogramViewerOpen").toBool();
    };
    const auto albumArtOpen = [viewer]() {
        return viewer->property("albumArtViewerOpen").toBool();
    };

    // F11 opens and closes the visualization viewer as a popup.
    QTest::keyClick(window, Qt::Key_F11);
    QTRY_VERIFY(spectrogramOpen());
    QTRY_VERIFY(popup->property("opened").toBool());
    QTest::keyClick(window, Qt::Key_F11);
    QTRY_VERIFY(!spectrogramOpen());
    QTRY_VERIFY(!popup->property("visible").toBool());

    // Shift+F11 opens the album art viewer once there is album art, and
    // F11 switches from it to the visualization viewer.
    QTest::keyClick(window, Qt::Key_F11, Qt::ShiftModifier);
    QVERIFY(!albumArtOpen());
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    QTest::keyClick(window, Qt::Key_F11, Qt::ShiftModifier);
    QTRY_VERIFY(albumArtOpen());
    QTRY_VERIFY(albumArtPopup->property("opened").toBool());
    QTest::keyClick(window, Qt::Key_F11);
    QTRY_VERIFY(spectrogramOpen());
    QVERIFY(!albumArtOpen());
    QTRY_VERIFY(popup->property("opened").toBool());
    QTest::keyClick(window, Qt::Key_F11);
    QTRY_VERIFY(!spectrogramOpen());
    QTRY_VERIFY(!popup->property("visible").toBool());

    // In a window of its own, too.
    bridge->setProperty("viewerFullscreenMode", 1);
    QTest::keyClick(window, Qt::Key_F11);
    QTRY_VERIFY(spectrogramOpen());
    QTRY_VERIFY(surface->window() != nullptr && surface->window() != window);
    QQuickWindow *fullscreen = surface->window();
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    QTest::keyClick(fullscreen, Qt::Key_F11);
    QTRY_VERIFY(!spectrogramOpen());
    QTRY_VERIFY(!fullscreen->isVisible());
}

void TransportInputSmokeTest::fullscreenViewerWindowsAreMadeOnFirstOpenAndKept() {
    // The viewers' windows over the whole screen are made the first time
    // each viewer opens there, not at startup, and not for the viewer in
    // the main window. Closed, a viewer's window is hidden and kept, with
    // its graphics and scene graph, so that opening it again makes no
    // window and sets up no graphics; hidden, it counts as off screen.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(16, 16, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(coverPath));

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *albumArt = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && surface != nullptr && albumArt != nullptr);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    const QString spectrogramName = QStringLiteral("spectrogramFullscreenWindow");
    const QString albumArtName = QStringLiteral("albumArtFullscreenWindow");
    const auto windowsNamed = [](const QString &name) {
        qsizetype count = 0;
        for (QWindow *each : QGuiApplication::allWindows()) {
            count += each->objectName() == name ? 1 : 0;
        }
        return count;
    };
    QCOMPARE(windowsNamed(spectrogramName), 0);
    QCOMPARE(windowsNamed(albumArtName), 0);
    QVERIFY(viewerWindowNamed(engine, spectrogramName) == nullptr);
    QVERIFY(viewerWindowNamed(engine, albumArtName) == nullptr);

    // In the main window, they make none.
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
    QCOMPARE(surface->window(), window);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewerForCurrentArt"));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
    QCoreApplication::processEvents();
    QCOMPARE(windowsNamed(spectrogramName), 0);
    QCOMPARE(windowsNamed(albumArtName), 0);

    bridge->setProperty("viewerFullscreenMode", 1);
    struct Viewer {
        const char *open;
        const char *close;
        const char *openProperty;
        QString windowName;
        QQuickItem *content;
    };
    const QList<Viewer> viewers{
        {"openSpectrogramViewer", "closeSpectrogramViewer", "spectrogramViewerOpen",
         spectrogramName, surface},
        {"openAlbumArtViewerForCurrentArt", "closeAlbumArtViewer", "albumArtViewerOpen",
         albumArtName, albumArt},
    };
    for (const Viewer &shown : viewers) {
        const QByteArray name = shown.windowName.toUtf8();
        QVERIFY(QMetaObject::invokeMethod(viewer, shown.open));
        // Made as the viewer opens, before the platform shows it.
        QQuickWindow *fullscreen = viewerWindowNamed(engine, shown.windowName);
        QVERIFY2(fullscreen != nullptr, name.constData());
        QSignalSpy initialized(fullscreen, &QQuickWindow::sceneGraphInitialized);
        QSignalSpy invalidated(fullscreen, &QQuickWindow::sceneGraphInvalidated);
        QSignalSpy destroyed(fullscreen, &QObject::destroyed);
        QVERIFY(fullscreen->isPersistentGraphics());
        QVERIFY(fullscreen->isPersistentSceneGraph());
        WindowPresence *presence = WindowPresence::of(fullscreen);
        for (int open = 0; open < 3; ++open) {
            if (open > 0) {
                QVERIFY(QMetaObject::invokeMethod(viewer, shown.open));
            }
            QTRY_VERIFY2(viewer->property(shown.openProperty).toBool(), name.constData());
            QCOMPARE(viewerWindowNamed(engine, shown.windowName), fullscreen);
            QCOMPARE(windowsNamed(shown.windowName), 1);
            QVERIFY(QTest::qWaitForWindowExposed(fullscreen));
            QTRY_COMPARE(shown.content->window(), fullscreen);
            QTRY_VERIFY2(presence->shown(), name.constData());

            QVERIFY(QMetaObject::invokeMethod(viewer, shown.close));
            QTRY_VERIFY2(!fullscreen->isVisible(), name.constData());
            // Hidden, it is off screen, and what it showed is back in the
            // main window, which is on screen.
            QVERIFY2(!presence->shown(), name.constData());
            if (shown.content == surface) {
                QTRY_COMPARE(surface->window(), window);
                QVERIFY(WindowPresence::of(window)->shown());
            }
        }
        QCOMPARE(destroyed.count(), 0);
        QCOMPARE(windowsNamed(shown.windowName), 1);
        // Its scene graph was set up once, at the first open, if the
        // renderer has one, and kept while it was hidden.
        QVERIFY2(initialized.count() <= 1, name.constData());
        QCOMPARE(invalidated.count(), 0);
    }
    QVERIFY(finishIncubation(engine));
    const QString log = takeCapturedMessagesText();
    QVERIFY2(onlyErrorReports(log), qPrintable(log));
}

void TransportInputSmokeTest::visualizationModeSwitchTakesKeyboardFocus() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    QVERIFY(viewer != nullptr && playPause != nullptr);
    QSignalSpy toggled(playPause, SIGNAL(triggered(QObject*)));
    QQuickItem *spectrogramButton =
        visibleItemNamed(window, QStringLiteral("visualizationModeButton0"));
    QQuickItem *waveformButton =
        visibleItemNamed(window, QStringLiteral("visualizationModeButton1"));
    QVERIFY(spectrogramButton != nullptr && waveformButton != nullptr);
    QVERIFY(spectrogramButton->activeFocusOnTab());
    QVERIFY(waveformButton->activeFocusOnTab());
    // The plate, round the buttons' row.
    QQuickItem *modeSwitch = waveformButton->parentItem()->parentItem();
    QVERIFY(modeSwitch != nullptr);
    QCOMPARE(modeSwitch->objectName(), QStringLiteral("visualizationModeSwitch"));
    // Without the pointer, faded out but still there for Tab.
    QVERIFY(modeSwitch->isVisible());
    QTRY_COMPARE(modeSwitch->opacity(), 0.0);

    // Tab reaches the switch.
    QQuickItem *beforeSwitch = spectrogramButton->nextItemInFocusChain(false);
    QVERIFY(beforeSwitch != nullptr);
    beforeSwitch->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(spectrogramButton->hasActiveFocus());
    QTRY_COMPARE(modeSwitch->opacity(), 1.0);
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(waveformButton->hasActiveFocus());

    // Space picks the focused mode instead of playing or pausing.
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 1);
    QCOMPARE(toggled.count(), 0);
    QTest::keyClick(window, Qt::Key_Tab, Qt::ShiftModifier);
    QTRY_VERIFY(spectrogramButton->hasActiveFocus());
    // A tool button: Space presses it.
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 0);
    QCOMPARE(toggled.count(), 0);
}

namespace {

void setChannelCount(QObject *bridge, QQuickItem *spectrogramSurface, int channels) {
    bridge->setProperty("currentTrackChannels", channels);
    QVariant descriptors;
    QMetaObject::invokeMethod(
        spectrogramSurface, "descriptorsForChannelCount",
        Q_RETURN_ARG(QVariant, descriptors), Q_ARG(QVariant, QVariant(channels)));
    spectrogramSurface->setProperty("channelDescriptors", descriptors);
}

} // namespace

void TransportInputSmokeTest::visualizationModeSwitchStaysClearOfTheCornerOverlays() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    window->resize(window->minimumWidth(), window->minimumHeight());
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *autoHide =
        window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    auto *spectrogramSurface =
        window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && autoHide != nullptr && popup != nullptr
            && spectrogramSurface != nullptr && waveformSurface != nullptr);
    bridge->setProperty("spectrogramViewMode", 1);
    bridge->setProperty("channelButtonsVisibility", 2);
    // Stays shown while the layouts are checked, until the test hides it.
    autoHide->setProperty("hideDelay", 600000);

    const auto checkLayouts = [&](QQuickItem *controlsRoot, const QString &closeButtonName) {
        QQuickItem *modeSwitchItem =
            firstVisibleItemNamed(controlsRoot, QStringLiteral("visualizationModeSwitch"));
        QVERIFY(modeSwitchItem != nullptr);
        QQuickItem *closeButton = closeButtonName.isEmpty()
            ? nullptr : firstVisibleItemNamed(controlsRoot, closeButtonName);
        QCOMPARE(closeButton != nullptr, !closeButtonName.isEmpty());
        for (const int displayMode : {0, 1}) {
            bridge->setProperty("spectrogramDisplayMode", displayMode);
            for (const int visualizationMode : {0, 1}) {
                viewer->setProperty("visualizationMode", visualizationMode);
                for (const int channels : {1, 2, 8}) {
                    setChannelCount(bridge, spectrogramSurface, channels);
                    QQuickItem *surface =
                        visualizationMode == 0 ? spectrogramSurface : waveformSurface;
                    QTRY_VERIFY(surface->isVisible());
                    QCoreApplication::processEvents();

                    const QRectF view = sceneRectOf(surface);
                    const QRectF modeSwitch = sceneRectOf(modeSwitchItem);
                    const double margin = modeSwitch.height();
                    // In the top right corner, off the centre line; over
                    // the whole screen, below the title strip over the view.
                    const double stripHeight = controlsRoot->window() != window ? 33.0 : 0.0;
                    QVERIFY(view.contains(modeSwitch));
                    QVERIFY(modeSwitch.left() > view.center().x());
                    QVERIFY(modeSwitch.top() >= view.top() + stripHeight);
                    QVERIFY(modeSwitch.top() - (view.top() + stripHeight) < margin);
                    if (closeButton != nullptr) {
                        const QRectF close = sceneRectOf(closeButton);
                        QVERIFY(close.right() <= view.right());
                        QVERIFY(view.right() - close.right() < margin);
                        QVERIFY(modeSwitch.right() <= close.left());
                        QVERIFY(close.left() - modeSwitch.right() < margin);
                    } else {
                        QVERIFY(view.right() - modeSwitch.right() < margin);
                    }

                    // Beside the lanes' labels and buttons, not over them.
                    const QList<QQuickItem *> strips = visibleItemsNamed(
                        controlsRoot, QStringLiteral("plotLaneHead"));
                    QTRY_COMPARE(visibleItemsNamed(controlsRoot, QStringLiteral("plotLaneHead")).size(),
                                 channels);
                    QVERIFY(!strips.isEmpty() && sceneRectOf(strips.constFirst()).width() > 0.0);

                    QObject *painted = visualizationMode == 0
                        ? static_cast<QObject *>(firstVisibleItemNamed(
                              surface, QStringLiteral("spectrogramPaneItem")))
                        : surface->property("waveformItem").value<QObject *>();
                    QVERIFY(painted != nullptr);
                    // The layout sizes the panes it was just given as it polishes.
                    for (QQuickItem *pane : visibleItemsNamed(
                             surface, QStringLiteral("spectrogramPaneItem"))) {
                        QTRY_COMPARE(pane->width(), surface->width());
                    }
                    const auto counterRect = [&]() {
                        auto *spectrogram = qobject_cast<SpectrogramItem *>(painted);
                        auto *waveform = qobject_cast<WaveformEditorItem *>(painted);
                        const QRectF local = spectrogram != nullptr
                            ? spectrogram->fpsCounterRect() : waveform->fpsCounterRect();
                        return qobject_cast<QQuickItem *>(painted)->mapRectToScene(local);
                    };
                    const auto setFps = [&](int fps) {
                        if (auto *spectrogram = qobject_cast<SpectrogramItem *>(painted)) {
                            QMutexLocker lock(&spectrogram->m_stateMutex);
                            spectrogram->m_fpsValue = fps;
                        } else {
                            auto *waveform = qobject_cast<WaveformEditorItem *>(painted);
                            QMutexLocker lock(&waveform->m_stateMutex);
                            waveform->m_fpsValue = fps;
                        }
                    };

                    // The counter sits beside the switch at any width, and
                    // neither moves the other.
                    bridge->setProperty("showFps", true);
                    QTRY_COMPARE(painted->property("showFpsOverlay").toBool(), true);
                    double counterRight = -1.0;
                    for (const int fps : {9, 999}) {
                        setFps(fps);
                        const QRectF counter = counterRect();
                        QVERIFY(!counter.isEmpty());
                        QVERIFY(view.contains(counter));
                        QVERIFY(!counter.intersects(modeSwitch));
                        QVERIFY(counter.right() <= modeSwitch.left());
                        QVERIFY(modeSwitch.left() - counter.right() < margin);
                        if (closeButton != nullptr) {
                            QVERIFY(!counter.intersects(sceneRectOf(closeButton)));
                        }
                        for (QQuickItem *strip : strips) {
                            QVERIFY(!sceneRectOf(strip).intersects(modeSwitch));
                            QVERIFY(!sceneRectOf(strip).intersects(counter));
                        }
                        QVERIFY(counterRight < 0.0 || qFuzzyCompare(counter.right(), counterRight));
                        counterRight = counter.right();
                        QCOMPARE(sceneRectOf(modeSwitchItem), modeSwitch);
                    }
                    bridge->setProperty("showFps", false);
                    QTRY_COMPARE(painted->property("showFpsOverlay").toBool(), false);
                    setFps(0);
                    QVERIFY(counterRect().isEmpty());
                    QCOMPARE(sceneRectOf(modeSwitchItem), modeSwitch);
                    for (QQuickItem *strip : strips) {
                        QVERIFY(!sceneRectOf(strip).intersects(modeSwitch));
                    }
                }
            }
        }
    };

    // Docked, at the narrowest the window lets the pane be.
    QTRY_VERIFY(spectrogramSurface->width() > 0.0);
    checkLayouts(window->contentItem(), QString());
    if (QTest::currentTestFailed()) {
        return;
    }

    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(popup->property("opened").toBool());
    checkLayouts(qvariant_cast<QQuickItem *>(popup->property("contentItem")),
                 QStringLiteral("spectrogramPopupCloseButton"));
    if (QTest::currentTestFailed()) {
        return;
    }
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
    QTRY_VERIFY(!popup->property("visible").toBool());

    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(spectrogramSurface->window() != nullptr && spectrogramSurface->window() != window);
    QQuickWindow *fullscreen = spectrogramSurface->window();
    QVERIFY(QTest::qWaitForWindowExposed(fullscreen));
    QTRY_VERIFY(qFuzzyCompare(spectrogramSurface->width(), fullscreen->contentItem()->width()));
    // Over the whole screen, the viewer's controls are in the title strip
    // over the view's top edge, so the switch, below it, has the corner to
    // itself.
    QQuickItem *strip =
        firstVisibleItemNamed(fullscreen->contentItem(), QStringLiteral("spectrogramFullscreenStrip"));
    QVERIFY(strip != nullptr);
    QCOMPARE(sceneRectOf(spectrogramSurface).top(), 0.0);
    QCOMPARE(sceneRectOf(strip).top(), 0.0);
    QVERIFY(sceneRectOf(strip).contains(sceneRectOf(firstVisibleItemNamed(
        fullscreen->contentItem(), QStringLiteral("spectrogramFullscreenCloseButton")))));
    checkLayouts(fullscreen->contentItem(), QString());
    if (QTest::currentTestFailed()) {
        return;
    }

    // The controls hiding leave the counter where it is.
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(waveformSurface->isVisible());
    bridge->setProperty("showFps", true);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    QTRY_VERIFY(waveform->showFpsOverlay());
    {
        QMutexLocker lock(&waveform->m_stateMutex);
        waveform->m_fpsValue = 60;
    }
    const QRectF shownControlsCounter = waveform->fpsCounterRect();
    QVERIFY(!shownControlsCounter.isEmpty());
    autoHide->setProperty("hideDelay", 1);
    QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
    {
        QMutexLocker lock(&waveform->m_stateMutex);
        waveform->m_fpsValue = 60;
    }
    QCOMPARE(waveform->fpsCounterRect(), shownControlsCounter);
    autoHide->setProperty("hideDelay", 5000);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
}

void TransportInputSmokeTest::visualizationModeSwitchShowsOnlyWithThePointerOrFocus() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && surface != nullptr);
    // Rolling: the plot's right edge is now.
    QVERIFY(bridge->setProperty("spectrogramDisplayMode", 0));
    QTRY_VERIFY(surface->isVisible());
    QQuickItem *modeSwitch =
        firstVisibleItemNamed(window->contentItem(), QStringLiteral("visualizationModeSwitch"));
    QQuickItem *spectrogramButton =
        visibleItemNamed(window, QStringLiteral("visualizationModeButton0"));
    QQuickItem *waveformButton =
        visibleItemNamed(window, QStringLiteral("visualizationModeButton1"));
    QVERIFY(modeSwitch != nullptr && spectrogramButton != nullptr && waveformButton != nullptr);

    const QRectF pane = sceneRectOf(surface);
    QVERIFY(pane.contains(sceneRectOf(modeSwitch)));
    // A point of the window off the pane.
    QPoint away;
    for (const QPoint candidate : {QPoint(4, window->height() - 4), QPoint(4, 4),
                                   QPoint(window->width() - 4, window->height() - 4)}) {
        if (!pane.contains(candidate)) {
            away = candidate;
            break;
        }
    }
    QVERIFY(!pane.contains(away) && !away.isNull());

    // The pointer away: nothing on the plot's newest edge, but the switch is
    // still a Tab stop with its name.
    QTest::mouseMove(window, away);
    QTRY_COMPARE(modeSwitch->opacity(), 0.0);
    QVERIFY(modeSwitch->isVisible());
    QVERIFY(spectrogramButton->activeFocusOnTab() && waveformButton->activeFocusOnTab());
    QAccessible::setActive(true);
    const QScopeGuard stopAccessibility([]() { QAccessible::setActive(false); });
    QAccessibleInterface *switchAccessible = QAccessible::queryAccessibleInterface(modeSwitch);
    QVERIFY(switchAccessible != nullptr);
    QCOMPARE(switchAccessible->text(QAccessible::Name), QStringLiteral("Visualization"));
    QAccessibleInterface *spectrogramAccessible =
        QAccessible::queryAccessibleInterface(spectrogramButton);
    QVERIFY(spectrogramAccessible != nullptr);
    QCOMPARE(spectrogramAccessible->text(QAccessible::Name), QStringLiteral("Spectrogram"));
    QVERIFY(spectrogramAccessible->state().focusable);

    // The pointer anywhere on the pane shows it: on the plot's middle, and
    // on the left, away from the switch.
    QTest::mouseMove(window, pane.center().toPoint());
    QTRY_COMPARE(modeSwitch->opacity(), 1.0);
    QTest::mouseMove(window, away);
    QTRY_COMPARE(modeSwitch->opacity(), 0.0);
    QTest::mouseMove(window, QPoint(static_cast<int>(pane.left()) + 60,
                                    static_cast<int>(pane.bottom()) - 10));
    QTRY_COMPARE(modeSwitch->opacity(), 1.0);
    // On the switch itself, it stays.
    QTest::mouseMove(window, sceneRectOf(spectrogramButton).center().toPoint());
    QTRY_VERIFY(modeSwitch->property("pointerHovered").toBool());
    QCOMPARE(modeSwitch->opacity(), 1.0);
    QTest::mouseMove(window, away);
    QTRY_COMPARE(modeSwitch->opacity(), 0.0);

    // Tab focus shows it, and it stays while focus is on either button.
    QQuickItem *beforeSwitch = spectrogramButton->nextItemInFocusChain(false);
    QVERIFY(beforeSwitch != nullptr);
    beforeSwitch->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(spectrogramButton->hasActiveFocus());
    QVERIFY(modeSwitch->property("focusInside").toBool());
    QTRY_COMPARE(modeSwitch->opacity(), 1.0);
    QVERIFY(!switchAccessible->state().invisible);
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(waveformButton->hasActiveFocus());
    QTest::qWait(50);
    QCOMPARE(modeSwitch->opacity(), 1.0);
    // Focus leaving it hides it again.
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(!waveformButton->hasActiveFocus());
    QVERIFY(!modeSwitch->property("focusInside").toBool());
    QTRY_COMPARE(modeSwitch->opacity(), 0.0);

    // Ctrl+1 and Ctrl+2 pick the modes while it is hidden.
    window->contentItem()->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_2, Qt::ControlModifier);
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 1);
    QCOMPARE(modeSwitch->property("selectedMode").toInt(), 1);
    QCOMPARE(modeSwitch->opacity(), 0.0);
    QTest::keyClick(window, Qt::Key_1, Qt::ControlModifier);
    QTRY_COMPARE(viewer->property("visualizationMode").toInt(), 0);
    QCOMPARE(modeSwitch->property("selectedMode").toInt(), 0);
    QCOMPARE(modeSwitch->opacity(), 0.0);

    // In the viewer in the main window, the pointer on the view shows it too.
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    QVERIFY(popup != nullptr);
    QTRY_VERIFY(popup->property("opened").toBool());
    QTRY_VERIFY(surface->isVisible());
    QTest::mouseMove(window, sceneRectOf(surface).center().toPoint());
    QTRY_COMPARE(modeSwitch->opacity(), 1.0);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
    QTest::mouseMove(window, away);
    QTRY_COMPARE(modeSwitch->opacity(), 0.0);
}

void TransportInputSmokeTest::visualizationModeSwitchLeavesTheCornerToTheView() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && popup != nullptr
            && waveformSurface != nullptr);
    QQuickItem *modeSwitch =
        firstVisibleItemNamed(window->contentItem(), QStringLiteral("visualizationModeSwitch"));
    QVERIFY(modeSwitch != nullptr);
    bridge->setProperty("showSpectrogramCrosshair", true);
    bridge->setProperty("durationSeconds", 100.0);
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(waveformSurface->isVisible());
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    QTRY_VERIFY(waveform->crosshairEnabled());

    // On the switch, the pointer brings it up.
    const QRectF switchRect = sceneRectOf(modeSwitch);
    QTest::mouseMove(window, switchRect.center().toPoint());
    QTRY_VERIFY(modeSwitch->property("pointerHovered").toBool());

    // Beside it, the view has the pointer: the crosshair follows it,
    const QPoint nearSwitch(static_cast<int>(switchRect.left()) - 12,
                            static_cast<int>(switchRect.center().y()));
    QVERIFY(!switchRect.contains(nearSwitch));
    QTest::mouseMove(window, nearSwitch);
    QTRY_VERIFY(!modeSwitch->property("pointerHovered").toBool());
    QTRY_VERIFY(waveform->m_hoverActive);
    QCOMPARE(waveform->mapToScene(waveform->m_hoverPosition).toPoint(), nearSwitch);

    // in the spectrogram too,
    viewer->setProperty("visualizationMode", 0);
    auto *spectrogramSurface =
        window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    QVERIFY(spectrogramSurface != nullptr);
    QTRY_VERIFY(spectrogramSurface->isVisible());
    auto *spectrogram = qobject_cast<SpectrogramItem *>(firstVisibleItemNamed(
        spectrogramSurface, QStringLiteral("spectrogramPaneItem")));
    QVERIFY(spectrogram != nullptr);
    QTest::mouseMove(window, nearSwitch + QPoint(1, 1));
    QTRY_VERIFY(spectrogram->m_hoverActive);
    viewer->setProperty("visualizationMode", 1);
    QTRY_VERIFY(waveformSurface->isVisible());
    QTest::mouseMove(window, nearSwitch);

    // and its seek and the double-click that opens the viewer work there.
    QSignalSpy seeks(waveform, &WaveformEditorItem::seekRequested);
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, nearSwitch);
    QTRY_COMPARE(seeks.count(), 1);
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, nearSwitch);
    QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
    QTRY_VERIFY(popup->property("opened").toBool());

    // In the viewer, a double-click there closes it, as it does elsewhere.
    QQuickItem *viewerSwitch = firstVisibleItemNamed(
        qvariant_cast<QQuickItem *>(popup->property("contentItem")),
        QStringLiteral("visualizationModeSwitch"));
    QVERIFY(viewerSwitch != nullptr);
    const QRectF viewerSwitchRect = sceneRectOf(viewerSwitch);
    const QPoint nearViewerSwitch(static_cast<int>(viewerSwitchRect.left()) - 12,
                                  static_cast<int>(viewerSwitchRect.center().y()));
    QTest::mouseMove(window, nearViewerSwitch);
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, nearViewerSwitch);
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
}

namespace {

// Gives the fallback bridge a playing track, as the viewers' boards show it.
void setPlayingTrack(QObject *bridge) {
    bridge->setProperty("currentTrackTitle", QStringLiteral("Morning Tide Field"));
    bridge->setProperty("currentTrackArtist", QStringLiteral("The Quiet Meridian"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Small Hours"));
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/Morning Tide Field.flac"));
    bridge->setProperty("durationText", QStringLiteral("03:49"));
    bridge->setProperty("durationSeconds", 229.0);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
}

QString savedCover(const QTemporaryDir &dir, int width = 64, int height = 64) {
    const QString path = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(width, height, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    return cover.save(path) ? path : QString();
}

// The colour property `name` of `object`, as 8-bit RGBA, as it is drawn.
QRgb colorOf(QObject *object, const char *name) {
    return object->property(name).value<QColor>().rgba();
}

} // namespace

void TransportInputSmokeTest::viewersShowTheTrackInATitleStripOverTheWholeScreen() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    QVERIFY(bridge != nullptr && viewer != nullptr && palette != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    bridge->setProperty("viewerFullscreenMode", 1);
    const QString mainTitle = QStringLiteral("Morning Tide Field — Ferrous");
    QTRY_COMPARE(window->title(), mainTitle);

    struct Viewer {
        const char *open;
        const char *openProperty;
        QString windowName;
        QString stripName;
        QString closeName;
        QString surfaceName;
        QString title;
        // The plot set the strip takes its colours from.
        const char *plotColors;
    };
    const QList<Viewer> viewers{
        {"openSpectrogramViewer", "spectrogramViewerOpen",
         QStringLiteral("spectrogramFullscreenWindow"), QStringLiteral("spectrogramFullscreenStrip"),
         QStringLiteral("spectrogramFullscreenCloseButton"), QStringLiteral("mainSpectrogramSurface"),
         QStringLiteral("Morning Tide Field — Ferrous Visualization"), "spectrogramPlot"},
        {"openAlbumArtViewerForCurrentArt", "albumArtViewerOpen",
         QStringLiteral("albumArtFullscreenWindow"), QStringLiteral("albumArtFullscreenStrip"),
         QStringLiteral("albumArtFullscreenCloseButton"), QStringLiteral("albumArtSurface"),
         QStringLiteral("Morning Tide Field — Ferrous Album Art"), "waveformPlot"},
    };
    for (const Viewer &shown : viewers) {
        const QByteArray name = shown.windowName.toUtf8();
        QVERIFY(QMetaObject::invokeMethod(viewer, shown.open));
        QTRY_VERIFY(viewer->property(shown.openProperty).toBool());
        QQuickWindow *fullscreen = viewerWindowNamed(engine, shown.windowName);
        QVERIFY(fullscreen != nullptr);
        QVERIFY(QTest::qWaitForWindowActive(fullscreen));
        // The viewer's window names it; the main window keeps its title.
        QCOMPARE(fullscreen->title(), shown.title);
        QCOMPARE(window->title(), mainTitle);

        // A 32 px band on the plot's plate colour with its edge under it,
        // over the top of the view.
        QObject *plotColors = qvariant_cast<QObject *>(palette->property(shown.plotColors));
        QVERIFY(plotColors != nullptr);
        QQuickItem *strip = visibleItemNamed(fullscreen, shown.stripName);
        QVERIFY2(strip != nullptr, name.constData());
        QTRY_COMPARE(sceneRectOf(strip), QRectF(0, 0, fullscreen->width(), 33));
        QCOMPARE(colorOf(strip, "color"), colorOf(plotColors, "plotPlate"));
        QQuickItem *hairline = itemNamed(strip, QStringLiteral("viewerStripHairline"));
        QVERIFY(hairline != nullptr);
        QCOMPARE(sceneRectOf(hairline), QRectF(0, 32, fullscreen->width(), 1));
        QCOMPARE(colorOf(hairline, "color"), colorOf(plotColors, "plotPlateEdge"));

        // The title semibold, then "Artist — Album" and the time, secondary.
        QQuickItem *title = itemNamed(strip, QStringLiteral("viewerStripTitle"));
        QQuickItem *context = itemNamed(strip, QStringLiteral("viewerStripContext"));
        QQuickItem *time = itemNamed(strip, QStringLiteral("viewerStripTime"));
        QVERIFY(title != nullptr && context != nullptr && time != nullptr);
        QCOMPARE(title->property("text").toString(), QStringLiteral("Morning Tide Field"));
        QCOMPARE(qvariant_cast<QFont>(title->property("font")).weight(), QFont::DemiBold);
        QCOMPARE(colorOf(title, "color"), colorOf(plotColors, "plotText"));
        QCOMPARE(context->property("text").toString(),
                 QStringLiteral("The Quiet Meridian — Small Hours"));
        QCOMPARE(colorOf(context, "color"), colorOf(plotColors, "plotTextSecondary"));
        QVERIFY2(time->property("text").toString().endsWith(QStringLiteral(" / 3:49")),
                 qPrintable(time->property("text").toString()));
        QCOMPARE(colorOf(time, "color"), colorOf(plotColors, "plotTextSecondary"));
        QVERIFY(sceneRectOf(title).right() < sceneRectOf(context).left());
        QVERIFY(sceneRectOf(context).right() < sceneRectOf(time).left());
        // With room for them, nothing is cut short.
        QTRY_VERIFY(!context->property("truncated").toBool());
        QVERIFY(!title->property("truncated").toBool());
        QCOMPARE(context->width(), context->implicitWidth());

        // Short of room, the time stays whole and the title elides, keeping
        // clear of the context.
        const QString longTitle = QStringLiteral("A Remarkably Long Track Title That Will Certainly "
                                                 "Need Eliding Somewhere in the Strip, Again and Again");
        bridge->setProperty("currentTrackTitle", longTitle + QStringLiteral(" ") + longTitle);
        QTRY_VERIFY(title->property("truncated").toBool());
        QTRY_VERIFY(sceneRectOf(title).right() < sceneRectOf(context).left()
                    && sceneRectOf(context).right() < sceneRectOf(time).left());
        QVERIFY(!time->property("truncated").toBool());
        QCOMPARE(time->width(), time->implicitWidth());
        QVERIFY(sceneRectOf(time).right() <= sceneRectOf(visibleItemNamed(fullscreen, shown.closeName)).left());
        bridge->setProperty("currentTrackTitle", QStringLiteral("Morning Tide Field"));
        QTRY_VERIFY(!title->property("truncated").toBool());

        // A flat 24 px Close at the right, 4 px in, in the plot's colours;
        // the view fills the window under the strip.
        QQuickItem *close = visibleItemNamed(fullscreen, shown.closeName);
        QVERIFY2(close != nullptr, name.constData());
        QVERIFY(strip->isAncestorOf(close));
        QCOMPARE(sceneRectOf(close).size(), QSizeF(24, 24));
        QCOMPARE(sceneRectOf(close).right(), fullscreen->width() - 4.0);
        QCOMPARE(sceneRectOf(close).top(), 4.0);
        QQuickItem *closeIcon = itemNamed(close, QStringLiteral("viewerToolButtonIcon"));
        QVERIFY(closeIcon != nullptr);
        QCOMPARE(colorOf(closeIcon, "color"), colorOf(plotColors, "plotText"));
        auto *surface = window->findChild<QQuickItem *>(shown.surfaceName);
        if (surface == nullptr) {
            surface = fullscreen->findChild<QQuickItem *>(shown.surfaceName);
        }
        QVERIFY(surface != nullptr);
        QTRY_COMPARE(sceneRectOf(surface), QRectF(0, 0, fullscreen->width(), fullscreen->height()));
        // The only close button: the old 40 px one has gone.
        QCOMPARE(visibleItemsNamed(fullscreen->contentItem(), QStringLiteral("viewerCloseButton")).size(), 0);

        QTest::mouseClick(fullscreen, Qt::LeftButton, Qt::NoModifier, centerInWindow(close));
        QTRY_VERIFY2(!viewer->property(shown.openProperty).toBool(), name.constData());
        QTRY_VERIFY(!fullscreen->isVisible());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }

    // Stopped, the strip names no track, as the window's title does not.
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QQuickWindow *fullscreen = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    QCOMPARE(fullscreen->title(), QStringLiteral("Ferrous Visualization"));
    QQuickItem *strip = visibleItemNamed(fullscreen, QStringLiteral("spectrogramFullscreenStrip"));
    QVERIFY(strip != nullptr);
    QVERIFY(!itemNamed(strip, QStringLiteral("viewerStripTitle"))->isVisible());
    QVERIFY(!itemNamed(strip, QStringLiteral("viewerStripTime"))->isVisible());
    QTRY_VERIFY(fullscreen->activeFocusItem() != nullptr);
    QTest::keyClick(fullscreen, Qt::Key_Escape);
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
}

void TransportInputSmokeTest::fullscreenViewersGiveTheViewTheWholeScreen() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir, 400, 400);
    QVERIFY(!coverPath.isEmpty());
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(bridge != nullptr && viewer != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    bridge->setProperty("channelButtonsVisibility", 2);
    bridge->setProperty("viewerFullscreenMode", 1);

    struct Viewer {
        const char *open;
        const char *openProperty;
        QString windowName;
        QString autoHideName;
        QString stripName;
        QString closeName;
        QString surfaceName;
        QString viewFocusName;
    };
    const QList<Viewer> viewers{
        {"openSpectrogramViewer", "spectrogramViewerOpen", QStringLiteral("spectrogramFullscreenWindow"),
         QStringLiteral("spectrogramFullscreenControlsAutoHide"), QStringLiteral("spectrogramFullscreenStrip"),
         QStringLiteral("spectrogramFullscreenCloseButton"), QStringLiteral("mainSpectrogramSurface"),
         QStringLiteral("spectrogramFullscreenViewFocus")},
        {"openAlbumArtViewerForCurrentArt", "albumArtViewerOpen", QStringLiteral("albumArtFullscreenWindow"),
         QStringLiteral("albumArtFullscreenControlsAutoHide"), QStringLiteral("albumArtFullscreenStrip"),
         QStringLiteral("albumArtFullscreenCloseButton"), QStringLiteral("albumArtSurface"),
         QStringLiteral("albumArtFullscreenViewFocus")},
    };
    for (const Viewer &shown : viewers) {
        const QByteArray name = shown.windowName.toUtf8();
        QObject *autoHide = window->findChild<QObject *>(shown.autoHideName);
        QVERIFY2(autoHide != nullptr, name.constData());
        QVERIFY(QMetaObject::invokeMethod(viewer, shown.open));
        QTRY_VERIFY(viewer->property(shown.openProperty).toBool());
        QQuickWindow *fullscreen = viewerWindowNamed(engine, shown.windowName);
        QVERIFY(fullscreen != nullptr);
        QVERIFY(QTest::qWaitForWindowActive(fullscreen));
        QVERIFY(autoHide->property("active").toBool());
        auto *surface = window->findChild<QQuickItem *>(shown.surfaceName);
        if (surface == nullptr) {
            surface = fullscreen->findChild<QQuickItem *>(shown.surfaceName);
        }
        QQuickItem *strip = visibleItemNamed(fullscreen, shown.stripName);
        QQuickItem *close = visibleItemNamed(fullscreen, shown.closeName);
        QQuickItem *viewFocus = fullscreen->findChild<QQuickItem *>(shown.viewFocusName);
        QVERIFY2(surface != nullptr && strip != nullptr && close != nullptr && viewFocus != nullptr,
                 name.constData());
        const QRectF wholeWindow(0, 0, fullscreen->width(), fullscreen->height());

        // With the controls shown, the strip lies over the view's top edge,
        // and the view has the whole window.
        QTRY_VERIFY(autoHide->property("controlsVisible").toBool());
        QTRY_COMPARE(sceneRectOf(surface), wholeWindow);
        QCOMPARE(sceneRectOf(strip), QRectF(0, 0, fullscreen->width(), 33));
        QVERIFY(strip->z() > 0);
        QTRY_COMPARE(strip->opacity(), 1.0);

        // The pointer resting, the strip hides with them; the view keeps its
        // size, and nothing of the strip is left over it.
        autoHide->setProperty("hideDelay", 1);
        QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
        QTRY_VERIFY2(!strip->isVisible(), name.constData());
        QCOMPARE(sceneRectOf(surface), wholeWindow);
        QVERIFY(visibleItemNamed(fullscreen, shown.closeName) == nullptr);

        // The pointer on the view brings it back, over the same view.
        autoHide->setProperty("hideDelay", 5000);
        QTest::mouseMove(fullscreen, wholeWindow.center().toPoint());
        QTRY_VERIFY2(autoHide->property("controlsVisible").toBool(), name.constData());
        QTRY_VERIFY(strip->isVisible());
        QTRY_COMPARE(strip->opacity(), 1.0);
        QCOMPARE(sceneRectOf(surface), wholeWindow);

        // Keyboard focus on a strip button keeps it, however long the
        // pointer rests; a press on the view takes the focus, and it hides.
        for (int press = 0; press < 12 && !close->hasActiveFocus(); ++press) {
            QTest::keyClick(fullscreen, Qt::Key_Tab);
        }
        QVERIFY2(close->hasActiveFocus(), name.constData());
        autoHide->setProperty("hideDelay", 1);
        QTest::qWait(50);
        QVERIFY2(autoHide->property("controlsVisible").toBool(), name.constData());
        QVERIFY(strip->isVisible() && close->isVisible());
        QCOMPARE(strip->opacity(), 1.0);
        QTest::mouseClick(fullscreen, Qt::LeftButton, Qt::NoModifier,
                          QPoint(fullscreen->width() / 2, fullscreen->height() / 2));
        QTRY_VERIFY2(fullscreen->activeFocusItem() == viewFocus, name.constData());
        QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
        QTRY_VERIFY(!strip->isVisible());

        // A key brings the strip back for the keyboard.
        autoHide->setProperty("hideDelay", 5000);
        QTest::keyClick(fullscreen, Qt::Key_Tab);
        QTRY_VERIFY(autoHide->property("controlsVisible").toBool());
        QTRY_VERIFY(strip->isVisible());

        // The pointer over the album art's details, which take its hover
        // from the art, brings the strip back too; the details start below
        // it.
        if (shown.surfaceName == QStringLiteral("albumArtSurface")) {
            // Off the strip button the Tab above reached, which holds it.
            viewFocus->forceActiveFocus(Qt::MouseFocusReason);
            QVERIFY(viewer->setProperty("albumArtInfoVisible", true));
            QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
            QQuickItem *details = itemNamed(surface, QStringLiteral("albumArtInfoTitle"));
            QVERIFY(column != nullptr && details != nullptr);
            QTRY_VERIFY(column->isVisible());
            QTRY_VERIFY(sceneRectOf(details).top() >= sceneRectOf(strip).bottom());
            autoHide->setProperty("hideDelay", 1);
            QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
            QTRY_VERIFY(!strip->isVisible());
            autoHide->setProperty("hideDelay", 5000);
            const QRectF columnRect = sceneRectOf(column);
            QTest::mouseMove(fullscreen, columnRect.center().toPoint());
            QTest::mouseMove(fullscreen, columnRect.center().toPoint() + QPoint(4, 6));
            QTRY_VERIFY2(autoHide->property("controlsVisible").toBool(), "over the details");
            QTRY_VERIFY(strip->isVisible());
            QVERIFY(viewer->setProperty("albumArtInfoVisible", false));
        }

        QVERIFY(QMetaObject::invokeMethod(viewer, shown.openProperty[0] == 's'
                                                      ? "closeSpectrogramViewer"
                                                      : "closeAlbumArtViewer"));
        QTRY_VERIFY(!viewer->property(shown.openProperty).toBool());
        QTRY_VERIFY(!fullscreen->isVisible());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }

    // In the main window, the album art viewer's controls never hide.
    QObject *albumArtAutoHide =
        window->findChild<QObject *>(QStringLiteral("albumArtFullscreenControlsAutoHide"));
    QVERIFY(albumArtAutoHide != nullptr);
    bridge->setProperty("viewerFullscreenMode", 0);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewerForCurrentArt"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("albumArtViewerPopup"));
    QVERIFY(popup != nullptr);
    QTRY_VERIFY(popup->property("opened").toBool());
    QVERIFY(!albumArtAutoHide->property("active").toBool());
    albumArtAutoHide->setProperty("hideDelay", 1);
    QTest::qWait(50);
    QQuickItem *popupClose = visibleItemNamed(window, QStringLiteral("albumArtPopupCloseButton"));
    QVERIFY(popupClose != nullptr);
    QCOMPARE(sceneRectOf(popupClose).top(), 6.0);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
}

void TransportInputSmokeTest::fullscreenVisualizationKeepsItsControlsBelowTheStrip() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *autoHide =
        window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && autoHide != nullptr && surface != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("showSpectrogramScale", true);
    bridge->setProperty("channelButtonsVisibility", 2);
    // Per channel, each lane with its head.
    bridge->setProperty("spectrogramViewMode", 1);
    setChannelCount(bridge, surface, 2);
    QTRY_COMPARE(surface->property("laneCount").toInt(), 2);

    // The pane's: the mode switch 4 px from the top, the first lane's head
    // 5 px.
    const auto controlsTop = [](QQuickItem *in) {
        QQuickItem *modeSwitch = firstVisibleItemNamed(in, QStringLiteral("visualizationModeSwitch"));
        QQuickItem *laneHead = firstVisibleItemNamed(in, QStringLiteral("plotLaneHead"));
        return std::make_pair(modeSwitch ? sceneRectOf(modeSwitch).top() : -1.0,
                              laneHead ? sceneRectOf(laneHead).top() : -1.0);
    };
    QTRY_COMPARE(controlsTop(window->contentItem()),
                 std::make_pair(sceneRectOf(surface).top() + 4.0, sceneRectOf(surface).top() + 5.0));

    // The viewer in the main window has no strip: the same.
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    QVERIFY(popup != nullptr);
    QTRY_VERIFY(popup->property("opened").toBool());
    QTRY_COMPARE(controlsTop(window->contentItem()), std::make_pair(4.0, 5.0));
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
    QTRY_VERIFY(!popup->property("visible").toBool());

    // Over the whole screen, the controls that show with the strip keep
    // below it, whether it shows or not, so they never move and are never
    // under it; the time scale stays on the view's top edge, which the
    // strip leaves clear while it is hidden.
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QQuickWindow *fullscreen = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
    QVERIFY(fullscreen != nullptr);
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    QQuickItem *strip = visibleItemNamed(fullscreen, QStringLiteral("spectrogramFullscreenStrip"));
    QVERIFY(strip != nullptr);
    QTRY_COMPARE(sceneRectOf(surface).top(), 0.0);
    QTRY_COMPARE(controlsTop(fullscreen->contentItem()), std::make_pair(33.0 + 4.0, 33.0 + 5.0));
    auto *spectrogram = qobject_cast<SpectrogramItem *>(
        firstVisibleItemNamed(surface, QStringLiteral("spectrogramPaneItem")));
    QVERIFY(spectrogram != nullptr);
    QVERIFY(spectrogram->showTimeLabels());
    QCOMPARE(sceneRectOf(spectrogram).top(), 0.0);
    // The plot keeps its corner clear of the mode switch where it now is.
    QTRY_COMPARE(spectrogram->reservedCorner().height(), 33.0 + 4.0 + 28.0);
    for (const int mode : {1, 0}) {
        viewer->setProperty("visualizationMode", mode);
        QTRY_COMPARE(viewer->property("visualizationMode").toInt(), mode);
        QQuickItem *modeSwitch =
            firstVisibleItemNamed(fullscreen->contentItem(), QStringLiteral("visualizationModeSwitch"));
        QVERIFY(modeSwitch != nullptr);
        QTRY_COMPARE(sceneRectOf(modeSwitch).top(), 33.0 + 4.0);
        QVERIFY(!sceneRectOf(strip).intersects(sceneRectOf(modeSwitch)));
    }
    QTRY_COMPARE(controlsTop(fullscreen->contentItem()), std::make_pair(33.0 + 4.0, 33.0 + 5.0));
    autoHide->setProperty("hideDelay", 1);
    QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
    QTRY_VERIFY(!strip->isVisible());
    QQuickItem *laneHead = firstVisibleItemNamed(fullscreen->contentItem(), QStringLiteral("plotLaneHead"));
    QVERIFY(laneHead != nullptr);
    QCOMPARE(sceneRectOf(laneHead).top(), 33.0 + 5.0);
    autoHide->setProperty("hideDelay", 5000);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());

    // Back in the pane, as before.
    QTRY_COMPARE(controlsTop(window->contentItem()),
                 std::make_pair(sceneRectOf(surface).top() + 4.0, sceneRectOf(surface).top() + 5.0));
}

void TransportInputSmokeTest::viewersInTheMainWindowAreNamedInItsTitle() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(bridge != nullptr && viewer != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    const QString mainTitle = QStringLiteral("Morning Tide Field — Ferrous");
    QTRY_COMPARE(window->title(), mainTitle);

    struct Viewer {
        const char *open;
        const char *openProperty;
        QString popupName;
        QString controlsName;
        QString closeName;
        QString surfaceName;
        QString title;
    };
    const QList<Viewer> viewers{
        {"openSpectrogramViewer", "spectrogramViewerOpen", QStringLiteral("spectrogramViewerPopup"),
         QStringLiteral("spectrogramPopupControls"), QStringLiteral("spectrogramPopupCloseButton"),
         QStringLiteral("mainSpectrogramSurface"),
         QStringLiteral("Morning Tide Field — Ferrous Visualization")},
        {"openAlbumArtViewerForCurrentArt", "albumArtViewerOpen", QStringLiteral("albumArtViewerPopup"),
         QStringLiteral("albumArtPopupControls"), QStringLiteral("albumArtPopupCloseButton"),
         QStringLiteral("albumArtSurface"),
         QStringLiteral("Morning Tide Field — Ferrous Album Art")},
    };
    for (const Viewer &shown : viewers) {
        const QByteArray name = shown.popupName.toUtf8();
        QObject *popup = window->findChild<QObject *>(shown.popupName);
        QVERIFY(popup != nullptr);
        QVERIFY(QMetaObject::invokeMethod(viewer, shown.open));
        QTRY_VERIFY2(popup->property("opened").toBool(), name.constData());
        QTRY_COMPARE(window->title(), shown.title);

        // No title strip under the window's own title bar: the view fills
        // the window, and the controls are on a plate in its corner.
        QCOMPARE(visibleItemsNamed(window->contentItem(), QStringLiteral("spectrogramFullscreenStrip")).size(), 0);
        QCOMPARE(visibleItemsNamed(window->contentItem(), QStringLiteral("albumArtFullscreenStrip")).size(), 0);
        auto *surface = window->findChild<QQuickItem *>(shown.surfaceName);
        QVERIFY(surface != nullptr);
        QTRY_COMPARE(sceneRectOf(surface), QRectF(0, 0, window->width(), window->height()));
        QQuickItem *controls = visibleItemNamed(window, shown.controlsName);
        QVERIFY2(controls != nullptr, name.constData());
        QCOMPARE(sceneRectOf(controls).right(), window->width() - 4.0);
        QCOMPARE(sceneRectOf(controls).top(), 4.0);
        QQuickItem *close = visibleItemNamed(window, shown.closeName);
        QVERIFY(close != nullptr && controls->isAncestorOf(close));
        QCOMPARE(sceneRectOf(close).size(), QSizeF(24, 24));

        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(close));
        QTRY_VERIFY2(!viewer->property(shown.openProperty).toBool(), name.constData());
        QTRY_COMPARE(window->title(), mainTitle);
    }
}

void TransportInputSmokeTest::viewerZoomShowsAReadoutAndAResetButton() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *resetAction = window->findChild<QObject *>(QStringLiteral("resetVisualizationAction"));
    auto *spectrogramSurface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    auto *waveformSurface = window->findChild<QQuickItem *>(QStringLiteral("mainWaveformSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && resetAction != nullptr
            && spectrogramSurface != nullptr && waveformSurface != nullptr);
    auto *waveform = qobject_cast<WaveformEditorItem *>(
        waveformSurface->property("waveformItem").value<QObject *>());
    QVERIFY(waveform != nullptr);
    setPlayingTrack(bridge);
    const auto spectrogramZoom = [spectrogramSurface]() {
        return spectrogramSurface->property("zoomLevel").toDouble();
    };

    // View › Reset Visualization View brings the spectrogram back to its
    // default zoom, as it does the waveform.
    spectrogramSurface->setProperty("_widgetZoomLevel", 3.0);
    QCOMPARE(spectrogramZoom(), 3.0);
    QVERIFY(QMetaObject::invokeMethod(resetAction, "trigger"));
    QTRY_COMPARE(spectrogramZoom(), 1.0);

    for (const int fullscreenMode : {1, 0}) {
        bridge->setProperty("viewerFullscreenMode", fullscreenMode);
        QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
        QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
        QQuickWindow *host = window;
        QQuickItem *controls = nullptr;
        if (fullscreenMode == 1) {
            host = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
            QVERIFY(host != nullptr);
            QVERIFY(QTest::qWaitForWindowActive(host));
            controls = visibleItemNamed(host, QStringLiteral("spectrogramFullscreenStrip"));
        } else {
            QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
            QTRY_VERIFY(popup->property("opened").toBool());
            controls = visibleItemNamed(window, QStringLiteral("spectrogramPopupControls"));
        }
        QVERIFY(controls != nullptr);
        QQuickItem *readout = itemNamed(controls, QStringLiteral("viewerZoomReadout"));
        QQuickItem *reset = itemNamed(controls, QStringLiteral("viewerResetButton"));
        QVERIFY(readout != nullptr && reset != nullptr);
        // The window has its size: the controls end 4 px from its right.
        QTRY_COMPARE(sceneRectOf(controls).right(),
                     fullscreenMode == 1 ? double(host->width()) : host->width() - 4.0);

        for (const int mode : {0, 1}) {
            viewer->setProperty("visualizationMode", mode);
            // Nothing at the default zoom.
            QTRY_VERIFY(!readout->isVisible());
            QVERIFY(!reset->isVisible());

            // Zoomed: a quiet readout and Reset.
            if (mode == 0) {
                spectrogramSurface->setProperty("_viewerZoomLevel", 2.0);
            } else {
                waveform->setZoomLevel(4.0);
            }
            // In place at once, inside the window, without waiting for the
            // window to draw.
            QVERIFY(readout->isVisible() && reset->isVisible());
            QVERIFY2(clearOfTheOtherButtons(reset) && clearOfTheOtherButtons(readout)
                         && sceneRectOf(readout).right() <= sceneRectOf(reset).left(),
                     qPrintable(QStringLiteral("%1 %2").arg(sceneRectOf(readout).left())
                                    .arg(sceneRectOf(reset).right())));
            QTRY_VERIFY(readout->isVisible());
            QCOMPARE(readout->property("text").toString(),
                     mode == 0 ? QStringLiteral("Zoom 200%") : QStringLiteral("Zoom 400%"));
            QVERIFY(reset->isVisible());
            // Laid out: the readout, then Reset beside it.
            QTRY_VERIFY(clearOfTheOtherButtons(reset) && clearOfTheOtherButtons(readout)
                        && sceneRectOf(readout).right() <= sceneRectOf(reset).left());
            // A double-click on the readout leaves the viewer open.
            QTest::mouseDClick(host, Qt::LeftButton, Qt::NoModifier, centerInWindow(readout));
            QCoreApplication::processEvents();
            QVERIFY(viewer->property("spectrogramViewerOpen").toBool());
            QTest::mouseClick(host, Qt::LeftButton, Qt::NoModifier, centerInWindow(reset));
            if (mode == 0) {
                QTRY_COMPARE(spectrogramZoom(), 1.0);
            } else {
                QTRY_COMPARE(waveform->zoomLevel(), 1.0);
            }
            QTRY_VERIFY(!readout->isVisible());
            QVERIFY(!reset->isVisible());
            QVERIFY(viewer->property("spectrogramViewerOpen").toBool());
        }
        viewer->setProperty("visualizationMode", 0);
        QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
        QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
}

void TransportInputSmokeTest::viewerControlsComeBackForTheKeyboard() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *autoHide =
        window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    QVERIFY(bridge != nullptr && viewer != nullptr && autoHide != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QQuickWindow *fullscreen = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
    QVERIFY(fullscreen != nullptr);
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    QQuickItem *strip = visibleItemNamed(fullscreen, QStringLiteral("spectrogramFullscreenStrip"));
    QQuickItem *close = visibleItemNamed(fullscreen, QStringLiteral("spectrogramFullscreenCloseButton"));
    QVERIFY(strip != nullptr && close != nullptr);

    // The controls hide when the pointer rests, and the strip with them.
    autoHide->setProperty("hideDelay", 1);
    QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
    QTRY_VERIFY(!close->isVisible());
    QTRY_VERIFY(!strip->isVisible());

    // Tab brings them back and moves into them, and they stay while one of
    // them has the keyboard.
    QTest::keyClick(fullscreen, Qt::Key_Tab);
    QTRY_VERIFY(autoHide->property("controlsVisible").toBool());
    QQuickItem *focused = fullscreen->activeFocusItem();
    QVERIFY(focused != nullptr);
    QVERIFY2(focused->property("visualFocus").toBool(), qPrintable(focused->objectName()));
    QVERIFY(close->isVisible());
    QTest::qWait(50);
    QVERIFY(autoHide->property("controlsVisible").toBool());
    for (int press = 0; press < 8 && !close->hasActiveFocus(); ++press) {
        QTest::keyClick(fullscreen, Qt::Key_Tab);
    }
    QVERIFY(close->hasActiveFocus());
    QQuickItem *ring = itemNamed(close, QStringLiteral("viewerToolButtonFocusRing"));
    QVERIFY(ring != nullptr && ring->isVisible());
    QTest::qWait(50);
    QVERIFY(close->isVisible());

    // Space presses the focused Close.
    QTest::keyClick(fullscreen, Qt::Key_Space);
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
    autoHide->setProperty("hideDelay", 5000);
}

namespace {

// The items Tab stops on in `host` from where the focus is, in order, until
// it comes round again.
QList<QQuickItem *> tabStops(QQuickWindow *host) {
    QList<QQuickItem *> stops;
    for (int press = 0; press < 24; ++press) {
        QTest::keyClick(host, Qt::Key_Tab);
        QQuickItem *item = host->activeFocusItem();
        if (item == nullptr || stops.contains(item)) {
            break;
        }
        stops << item;
    }
    return stops;
}

// Tabs in `host` until `stop` has focus; whether it got there.
bool tabTo(QQuickWindow *host, QQuickItem *stop) {
    for (int press = 0; press < 24 && host->activeFocusItem() != stop; ++press) {
        QTest::keyClick(host, Qt::Key_Tab);
    }
    return host->activeFocusItem() == stop;
}

} // namespace

void TransportInputSmokeTest::viewerKeysWorkFromEveryControl() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    const QString candidatePath = dir.filePath(QStringLiteral("candidate.png"));
    QVERIFY(QImage(coverPath).save(candidatePath));
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(bridge != nullptr && viewer != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    const QVariantMap candidate{
        {QStringLiteral("normalizedUrl"), QUrl::fromLocalFile(candidatePath).toString()},
        {QStringLiteral("normalizedPath"), candidatePath},
    };
    const QVariantMap folderImage{
        {QStringLiteral("path"), coverPath},
        {QStringLiteral("fileName"), QStringLiteral("cover.png")},
        {QStringLiteral("embeddedCover"), false},
    };
    const auto contextMenuShown = [window]() {
        return anyMenuVisible(window, QStringLiteral("albumArtViewerContextMenu"));
    };

    // The ways the viewers open: the visualization, the album art with its
    // folder and Copy Path, and the album art comparing an iTunes candidate.
    enum class Shown { Visualization, Art, Comparison };
    const auto openViewer = [&](Shown shown) {
        if (shown == Shown::Visualization) {
            return QMetaObject::invokeMethod(viewer, "openSpectrogramViewer");
        }
        if (shown == Shown::Art) {
            const bool opened = QMetaObject::invokeMethod(viewer, "openAlbumArtViewerForCurrentArt");
            viewer->setProperty("albumArtViewerFileInfo", folderImage);
            return opened;
        }
        return QMetaObject::invokeMethod(viewer, "openAlbumArtViewerForSuggestion",
                                         Q_ARG(QVariant, QVariant(candidate)));
    };
    const auto isOpen = [viewer](Shown shown) {
        return viewer->property(shown == Shown::Visualization ? "spectrogramViewerOpen"
                                                              : "albumArtViewerOpen").toBool();
    };

    for (const int fullscreenMode : {1, 0}) {
        bridge->setProperty("viewerFullscreenMode", fullscreenMode);
        for (const Shown shown : {Shown::Visualization, Shown::Art, Shown::Comparison}) {
            const QString where = QStringLiteral("fullscreen %1, viewer %2")
                                      .arg(fullscreenMode).arg(int(shown));
            const auto hostWindow = [&]() -> QQuickWindow * {
                if (fullscreenMode == 0) {
                    return window;
                }
                return viewerWindowNamed(engine, shown == Shown::Visualization
                                                     ? QStringLiteral("spectrogramFullscreenWindow")
                                                     : QStringLiteral("albumArtFullscreenWindow"));
            };
            const auto openAndWait = [&]() {
                if (!openViewer(shown) || !QTest::qWaitFor([&]() { return isOpen(shown); })) {
                    return static_cast<QQuickWindow *>(nullptr);
                }
                QQuickWindow *host = hostWindow();
                if (host == nullptr || !QTest::qWaitForWindowActive(host)
                    || !QTest::qWaitFor([host]() { return host->activeFocusItem() != nullptr; })) {
                    return static_cast<QQuickWindow *>(nullptr);
                }
                return host;
            };

            QQuickWindow *host = openAndWait();
            QVERIFY2(host != nullptr, qPrintable(where));
            const QList<QQuickItem *> stops = tabStops(host);
            QVERIFY2(stops.size() >= 2, qPrintable(where));
            QQuickItem *close = visibleItemNamed(host, shown == Shown::Visualization
                ? (fullscreenMode == 1 ? QStringLiteral("spectrogramFullscreenCloseButton")
                                       : QStringLiteral("spectrogramPopupCloseButton"))
                : (fullscreenMode == 1 ? QStringLiteral("albumArtFullscreenCloseButton")
                                       : QStringLiteral("albumArtPopupCloseButton")));
            QVERIFY2(stops.contains(close), qPrintable(where));
            if (shown == Shown::Art) {
                QVERIFY2(stops.contains(visibleItemNamed(host, QStringLiteral("albumArtCopyPathButton"))),
                         qPrintable(where));
            } else if (shown == Shown::Comparison) {
                QVERIFY2(stops.contains(visibleItemNamed(host, QStringLiteral("albumArtCompareButton"))),
                         qPrintable(where));
            }
            QTest::keyClick(host, Qt::Key_Escape);
            QTRY_VERIFY2(!isOpen(shown), qPrintable(where));

            for (QQuickItem *stop : stops) {
                const QString at = where + QStringLiteral(", at ") + stop->objectName()
                    + QLatin1Char(' ') + QString::fromLatin1(stop->metaObject()->className());
                host = openAndWait();
                QVERIFY2(host != nullptr, qPrintable(at));
                QVERIFY2(tabTo(host, stop), qPrintable(at));
                if (shown == Shown::Comparison) {
                    // Left and Right switch between the two images.
                    const bool current = viewer->property("albumArtViewerShowsCurrentTrack").toBool();
                    QTest::keyClick(host, Qt::Key_Left);
                    QTRY_VERIFY2(viewer->property("albumArtViewerShowsCurrentTrack").toBool() != current,
                                 qPrintable(at));
                    QTest::keyClick(host, Qt::Key_Right);
                    QTRY_VERIFY2(viewer->property("albumArtViewerShowsCurrentTrack").toBool() == current,
                                 qPrintable(at));
                    QVERIFY2(tabTo(host, stop), qPrintable(at));
                }
                if (shown != Shown::Visualization) {
                    // The menu key opens the context menu; Escape closes it alone.
                    QTest::keyClick(host, Qt::Key_F10, Qt::ShiftModifier);
                    QTRY_VERIFY2(contextMenuShown(), qPrintable(at));
                    QTest::keyClick(QGuiApplication::focusWindow(), Qt::Key_Escape);
                    QTRY_VERIFY2(!contextMenuShown(), qPrintable(at));
                    QVERIFY2(isOpen(shown), qPrintable(at));
                    QTRY_VERIFY2(host->isActive(), qPrintable(at));
                    QVERIFY2(tabTo(host, stop), qPrintable(at));
                }
                // Escape closes the viewer from here.
                QTest::keyClick(host, Qt::Key_Escape);
                QTRY_VERIFY2(!isOpen(shown), qPrintable(at));
            }
            window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(window));
        }
    }
}

void TransportInputSmokeTest::viewerControlsStayWhileOneHasFocus() {
    // The visualization viewer's controls hide while the pointer rests,
    // unless one of them has focus, whatever gave it: the keyboard, a click,
    // or assistive technology. (The album art viewer's: see
    // fullscreenViewersGiveTheViewTheWholeScreen.)
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *autoHide =
        window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    QVERIFY(bridge != nullptr && viewer != nullptr && autoHide != nullptr);
    setPlayingTrack(bridge);
    const auto controlsVisible = [autoHide]() {
        return autoHide->property("controlsVisible").toBool();
    };
    // A real pointer move over the view: the strip's hover reports where
    // the pointer is as its controls come and go, so a made-up position
    // would wake them again.
    QQuickWindow *pointerWindow = nullptr;
    const auto wake = [&pointerWindow](double x) {
        QTest::mouseMove(pointerWindow, QPoint(int(x) + 100, 300));
        return true;
    };

    for (const int fullscreenMode : {1, 0}) {
        bridge->setProperty("viewerFullscreenMode", fullscreenMode);
        QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
        QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
        QQuickWindow *host = fullscreenMode == 1
            ? viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow")) : window;
        QVERIFY(host != nullptr);
        QVERIFY(QTest::qWaitForWindowActive(host));
        QTRY_VERIFY(host->activeFocusItem() != nullptr);
        pointerWindow = host;
        QQuickItem *viewFocus = host->findChild<QQuickItem *>(
            fullscreenMode == 1 ? QStringLiteral("spectrogramFullscreenViewFocus")
                                : QStringLiteral("spectrogramPopupViewFocus"));
        QVERIFY(viewFocus != nullptr);
        auto *surface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
        QVERIFY(surface != nullptr);
        const QPoint viewPoint = sceneRectOf(surface).center().toPoint();
        QQuickItem *close = visibleItemNamed(host, fullscreenMode == 1
            ? QStringLiteral("spectrogramFullscreenCloseButton")
            : QStringLiteral("spectrogramPopupCloseButton"));
        QQuickItem *modeButton = visibleItemNamed(host, QStringLiteral("visualizationModeButton1"));
        QVERIFY(close != nullptr && modeButton != nullptr);

        double x = 10;
        for (QQuickItem *control : {close, modeButton}) {
            for (const Qt::FocusReason reason : {Qt::OtherFocusReason, Qt::TabFocusReason}) {
                const QString where = QStringLiteral("fullscreen %1, %2, reason %3")
                                          .arg(fullscreenMode).arg(control->objectName()).arg(int(reason));
                QVERIFY(wake(++x));
                QTRY_VERIFY(controlsVisible());
                control->forceActiveFocus(reason);
                QVERIFY2(control->hasActiveFocus(), qPrintable(where));
                QCOMPARE(control->property("visualFocus").toBool(), reason == Qt::TabFocusReason);
                autoHide->setProperty("hideDelay", 1);
                QTest::qWait(50);
                QVERIFY2(controlsVisible(), qPrintable(where));
                QVERIFY2(control->isVisible(), qPrintable(where));

                // A click on the view takes the focus back: they hide again.
                QTest::mouseClick(host, Qt::LeftButton, Qt::NoModifier, viewPoint);
                QTRY_VERIFY2(host->activeFocusItem() == viewFocus, qPrintable(where));
                QTRY_VERIFY2(!controlsVisible(), qPrintable(where));
                autoHide->setProperty("hideDelay", 5000);
            }
        }
        QVERIFY(wake(++x));
        QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
        QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
}

void TransportInputSmokeTest::albumArtDetailsLieOverTheArt() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir, 300, 300);
    QVERIFY(!coverPath.isEmpty());
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("testCoverUrl"),
                                             QUrl::fromLocalFile(coverPath).toString());
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "components" as Components
import "viewers" as Viewers

Window {
    id: surfaceWindow
    width: 1000
    height: 600
    visible: true
    property alias surface: surface
    readonly property var tokens: surface.uiPalette

    Action { id: replaceFromItunesAction }

    Viewers.AlbumArtSurface {
        id: surface
        anchors.fill: parent
        viewerOpen: true
        viewerSource: testCoverUrl
        infoVisible: true
        initialViewToken: 0
        viewerDecodeWidth: 1000
        viewerDecodeHeight: 600
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        closeViewer: function() {}
        focusFullscreen: function() {}
        comparisonModeAvailable: false
        uiPalette: Components.UiPalette { windowRoot: surfaceWindow }
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *surface = qvariant_cast<QQuickItem *>(root->property("surface"));
    QObject *palette = qvariant_cast<QObject *>(root->property("tokens"));
    QVERIFY(surface != nullptr && palette != nullptr);
    const QString folder = QStringLiteral("/music/The Quiet Meridian/Small Hours (Remastered "
                                          "Deluxe Edition With Bonus Tracks)/Disc One");
    surface->setProperty("fileInfo", QVariantMap{
        {QStringLiteral("path"), folder + QStringLiteral("/cover.jpg")},
        {QStringLiteral("fileName"), QStringLiteral("cover.jpg")},
        {QStringLiteral("width"), 1400},
        {QStringLiteral("height"), 1400},
        {QStringLiteral("fileSizeText"), QStringLiteral("412 KiB")},
        {QStringLiteral("format"), QStringLiteral("JPEG")},
        {QStringLiteral("embeddedCover"), false},
    });

    // The art on the view colour, under a 280 px column behind a 1 px seam
    // that lies over the view's right edge: the art keeps the whole view.
    QQuickItem *floor = itemNamed(surface, QStringLiteral("albumArtFloor"));
    QQuickItem *viewport = itemNamed(surface, QStringLiteral("albumArtViewport"));
    QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
    QQuickItem *seam = itemNamed(surface, QStringLiteral("albumArtInfoSeam"));
    QQuickItem *backing = itemNamed(surface, QStringLiteral("albumArtInfoBacking"));
    QVERIFY(floor != nullptr && viewport != nullptr && column != nullptr && seam != nullptr
            && backing != nullptr);
    QCOMPARE(colorOf(floor, "color"), colorOf(palette, "view"));
    QTRY_COMPARE(sceneRectOf(column), QRectF(721, 0, 279, 600));
    QCOMPARE(sceneRectOf(seam), QRectF(720, 0, 1, 600));
    QCOMPARE(colorOf(seam, "color"), colorOf(palette, "seam"));
    QCOMPARE(sceneRectOf(backing), sceneRectOf(column));
    QCOMPARE(colorOf(backing, "color"), colorOf(palette, "view"));
    QCOMPARE(qAlpha(colorOf(backing, "color")), 255);
    QCOMPARE(sceneRectOf(viewport), QRectF(0, 0, 1000, 600));
    // The square art fits the view's height, centred in the whole view,
    // however the decode request is shaped.
    QQuickItem *image = itemNamed(surface, QStringLiteral("albumArtImage"));
    QVERIFY(image != nullptr);
    const auto paintedArt = [image]() {
        const QSizeF painted(image->property("paintedWidth").toReal(),
                             image->property("paintedHeight").toReal());
        const QPointF topLeft = image->mapToScene(
            QPointF((image->width() - painted.width()) / 2, (image->height() - painted.height()) / 2));
        return QRectF(topLeft, painted);
    };
    QTRY_COMPARE(paintedArt(), QRectF(200, 0, 600, 600));
    // The details draw over the art's right edge, which runs under them.
    const QImage shown = window->grabWindow().convertToFormat(QImage::Format_RGB32);
    QCOMPARE(shown.pixel(700, 590), QColor(Qt::darkCyan).rgb());
    QCOMPARE(shown.pixel(760, 590), QColor::fromRgba(colorOf(palette, "view")).rgb());

    // The file's name in semibold, then the rows: label, then value.
    QQuickItem *title = itemNamed(column, QStringLiteral("albumArtInfoTitle"));
    QVERIFY(title != nullptr);
    QCOMPARE(title->property("text").toString(), QStringLiteral("cover.jpg"));
    QCOMPARE(qvariant_cast<QFont>(title->property("font")).weight(), QFont::DemiBold);
    QStringList values;
    for (QQuickItem *value : visibleItemsNamed(column, QStringLiteral("albumArtInfoValue"))) {
        values << value->property("text").toString();
    }
    const QString size = QLocale().toString(1400.0, 'f', 0);
    QCOMPARE(values, (QStringList{QStringLiteral("%1 × %1 px").arg(size), QStringLiteral("412 KiB"),
                                  QStringLiteral("JPEG"), QStringLiteral("Folder image")}));
    QCOMPARE(visibleTextItems(column, QStringLiteral("Dimensions")).size(), 1);
    QCOMPARE(visibleTextItems(column, QStringLiteral("Source")).size(), 1);

    // The folder on its own line, then its path on a line of its own,
    // elided in the middle, with the whole path in its tooltip.
    QQuickItem *folderLabel = itemNamed(column, QStringLiteral("albumArtFolderLabel"));
    QQuickItem *path = itemNamed(column, QStringLiteral("albumArtFolderPath"));
    QQuickItem *copy = itemNamed(column, QStringLiteral("albumArtCopyPathButton"));
    QVERIFY(folderLabel != nullptr && path != nullptr && copy != nullptr);
    QVERIFY(folderLabel->isVisible() && path->isVisible() && copy->isVisible());
    QCOMPARE(folderLabel->property("text").toString(), QStringLiteral("Folder"));
    QTRY_VERIFY2(sceneRectOf(path).top() >= sceneRectOf(folderLabel).bottom(),
                 qPrintable(QStringLiteral("%1 %2").arg(sceneRectOf(path).top())
                                .arg(sceneRectOf(folderLabel).bottom())));
    QCOMPARE(path->property("text").toString(), folder);
    QCOMPARE(path->property("elide").toInt(), int(Qt::ElideMiddle));
    QTRY_VERIFY(path->property("truncated").toBool());
    QVERIFY(sceneRectOf(path).left() == sceneRectOf(folderLabel).left());
    QTest::mouseMove(window, centerInWindow(path));
    QTRY_COMPARE(visibleTextItems(window->contentItem(), folder).size(), 1);

    // Copy Path puts the folder on the clipboard.
    QGuiApplication::clipboard()->clear();
    QCOMPARE(copy->property("text").toString(), QStringLiteral("Copy Path"));
    QCOMPARE(sceneRectOf(copy).size(), QSizeF(24, 24));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(copy));
    QTRY_COMPARE(QGuiApplication::clipboard()->text(), folder);

    // Art the track embeds has no folder of its own.
    surface->setProperty("sourceKind", QStringLiteral("embedded"));
    QTRY_VERIFY(!path->isVisible());
    QVERIFY(!folderLabel->isVisible());
    QCOMPARE(title->property("text").toString(), QStringLiteral("Embedded Image"));
    QCOMPARE(visibleTextItems(column, QStringLiteral("Embedded in the track")).size(), 1);

    // Without the details, the art is where it was, and shows whole.
    surface->setProperty("infoVisible", false);
    QTRY_VERIFY(!column->isVisible());
    QVERIFY(!seam->isVisible());
    QCOMPARE(sceneRectOf(viewport), QRectF(0, 0, 1000, 600));
    QCOMPARE(paintedArt(), QRectF(200, 0, 600, 600));
    QTRY_COMPARE(window->grabWindow().convertToFormat(QImage::Format_RGB32).pixel(760, 590),
                 QColor(Qt::darkCyan).rgb());
}

void TransportInputSmokeTest::albumArtViewerShowsWhenItsImageCannotBeRead() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Cannot open: .*missing\\.png")));
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("testMissingUrl"),
        QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing.png"))).toString());
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "components" as Components
import "viewers" as Viewers

Window {
    id: surfaceWindow
    width: 600
    height: 400
    visible: true
    property alias surface: surface

    Action { id: replaceFromItunesAction }

    Viewers.AlbumArtSurface {
        id: surface
        anchors.fill: parent
        viewerOpen: true
        viewerSource: testMissingUrl
        infoVisible: false
        initialViewToken: 0
        viewerDecodeWidth: 600
        viewerDecodeHeight: 400
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        closeViewer: function() {}
        focusFullscreen: function() {}
        comparisonModeAvailable: false
        uiPalette: Components.UiPalette { windowRoot: surfaceWindow }
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *surface = qvariant_cast<QQuickItem *>(root->property("surface"));
    QVERIFY(surface != nullptr);
    QQuickItem *errorState = itemNamed(surface, QStringLiteral("albumArtErrorState"));
    QVERIFY(errorState != nullptr);

    // The view says so in its own place, centred in it, rather than in a
    // banner.
    QTRY_VERIFY(errorState->isVisible());
    QCOMPARE(itemNamed(errorState, QStringLiteral("albumArtErrorText"))->property("text").toString(),
             QStringLiteral("Couldn't load this image."));
    const QPointF errorCenter = sceneRectOf(errorState).center();
    QVERIFY(std::abs(errorCenter.x() - 300) <= 1 && std::abs(errorCenter.y() - 200) <= 1);

    // An image it can read replaces the message.
    surface->setProperty("viewerSource", QUrl::fromLocalFile(coverPath).toString());
    QTRY_COMPARE(surface->property("imageStatus").toInt(), 1);
    QVERIFY(!errorState->isVisible());
    // Only the image's own warning, which the test expects.
    const QString log = takeCapturedMessagesText();
    QVERIFY2(log.contains(QStringLiteral("Cannot open")) && !log.contains(QLatin1Char('\n')),
             qPrintable(log));
}

void TransportInputSmokeTest::fullscreenViewersCanKeepTheirTitleStripHidden() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir, 400, 400);
    QVERIFY(!coverPath.isEmpty());
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *plotLayer = window->findChild<QObject *>(QStringLiteral("plotLayer"));
    QObject *autoHide =
        window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    auto *albumArt = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && plotLayer != nullptr && autoHide != nullptr
            && albumArt != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(bridge->property("showFullscreenTitleStrip").toBool());
    QVERIFY(bridge->setProperty("showFullscreenTitleStrip", false));

    // The visualization: the pointer brings the controls back, without the
    // strip, and the mode switch sits on the top edge.
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
    QQuickWindow *fullscreen = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
    QVERIFY(fullscreen != nullptr);
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    QQuickItem *strip = itemNamed(fullscreen->contentItem(), QStringLiteral("spectrogramFullscreenStrip"));
    QVERIFY(strip != nullptr);
    QCOMPARE(plotLayer->property("topInset").toReal(), 0.0);
    for (const QPoint at : {QPoint(200, 200), QPoint(240, 260)}) {
        QTest::mouseMove(fullscreen, at);
    }
    QTest::keyClick(fullscreen, Qt::Key_Shift);
    QTRY_VERIFY(autoHide->property("controlsVisible").toBool());
    QCoreApplication::processEvents();
    QVERIFY(!strip->isVisible());
    QQuickItem *modeSwitch = nullptr;
    QTRY_VERIFY((modeSwitch = firstVisibleItemNamed(fullscreen->contentItem(),
                                                    QStringLiteral("visualizationModeSwitch")))
                != nullptr);
    QTRY_COMPARE(modeSwitch->opacity(), 1.0);
    QVERIFY(sceneRectOf(modeSwitch).top() >= 0.0);
    QVERIFY2(sceneRectOf(modeSwitch).top() < 33.0, qPrintable(QString::number(sceneRectOf(modeSwitch).top())));
    // They still hide as the pointer rests.
    autoHide->setProperty("hideDelay", 50);
    QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
    QTRY_VERIFY(!modeSwitch->isVisible());
    QVERIFY(!strip->isVisible());

    // Turned on again, it shows with the controls, which keep below it.
    QVERIFY(bridge->setProperty("showFullscreenTitleStrip", true));
    QTRY_COMPARE(plotLayer->property("topInset").toReal(), 33.0);
    autoHide->setProperty("hideDelay", 600000);
    QTest::mouseMove(fullscreen, QPoint(300, 300));
    QTRY_VERIFY(strip->isVisible());
    QTRY_VERIFY(modeSwitch->isVisible());
    QTRY_VERIFY(sceneRectOf(modeSwitch).top() >= 33.0);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!fullscreen->isVisible());
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));

    // The album art: no strip, the details from the top edge, and I still
    // shows and hides them.
    QVERIFY(bridge->setProperty("showFullscreenTitleStrip", false));
    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewer"));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QQuickWindow *albumArtWindow = viewerWindowNamed(engine, QStringLiteral("albumArtFullscreenWindow"));
    QVERIFY(albumArtWindow != nullptr);
    QVERIFY(QTest::qWaitForWindowActive(albumArtWindow));
    QQuickItem *albumArtStrip =
        itemNamed(albumArtWindow->contentItem(), QStringLiteral("albumArtFullscreenStrip"));
    QVERIFY(albumArtStrip != nullptr);
    QTest::mouseMove(albumArtWindow, QPoint(200, 200));
    QTest::mouseMove(albumArtWindow, QPoint(220, 240));
    QCoreApplication::processEvents();
    QVERIFY(!albumArtStrip->isVisible());
    QCOMPARE(albumArt->property("infoTopInset").toReal(), 0.0);
    QTest::keyClick(albumArtWindow, Qt::Key_I);
    QTRY_VERIFY(viewer->property("albumArtInfoVisible").toBool());
    QTRY_VERIFY(visibleItemNamed(albumArtWindow, QStringLiteral("albumArtInfoColumn")) != nullptr);
    QVERIFY(!albumArtStrip->isVisible());
    QTest::keyClick(albumArtWindow, Qt::Key_I);
    QTRY_VERIFY(!viewer->property("albumArtInfoVisible").toBool());
    // With the strip on, the details keep below it.
    QVERIFY(bridge->setProperty("showFullscreenTitleStrip", true));
    QTRY_COMPARE(albumArt->property("infoTopInset").toReal(), 33.0);
    QTest::mouseMove(albumArtWindow, QPoint(300, 300));
    QTRY_VERIFY(albumArtStrip->isVisible());
    QTest::keyClick(albumArtWindow, Qt::Key_Escape);
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
}

void TransportInputSmokeTest::albumArtInfoButtonIsTintedOnlyWhileChecked() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir, 400, 400);
    QVERIFY(!coverPath.isEmpty());
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && palette != nullptr && surface != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewer"));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QQuickWindow *fullscreen = viewerWindowNamed(engine, QStringLiteral("albumArtFullscreenWindow"));
    QVERIFY(fullscreen != nullptr);
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    QQuickItem *info = visibleItemNamed(fullscreen, QStringLiteral("albumArtFullscreenInfoButton"));
    QVERIFY(info != nullptr);
    QQuickItem *tint = itemNamed(info, QStringLiteral("viewerToolButtonChecked"));
    QQuickItem *ring = itemNamed(info, QStringLiteral("viewerToolButtonFocusRing"));
    QVERIFY(tint != nullptr && ring != nullptr);
    QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
    QVERIFY(column != nullptr);

    // Off: neither.
    QVERIFY(!viewer->property("albumArtInfoVisible").toBool());
    QVERIFY(!tint->isVisible() && !ring->isVisible());
    QVERIFY(!column->isVisible());

    // A click shows the details: the tint, and no ring.
    QTest::mouseClick(fullscreen, Qt::LeftButton, Qt::NoModifier, centerInWindow(info));
    QTRY_VERIFY(viewer->property("albumArtInfoVisible").toBool());
    QTRY_VERIFY(info->property("checked").toBool());
    QVERIFY(tint->isVisible());
    QCOMPARE(colorOf(tint, "color"), colorOf(palette, "selection"));
    QVERIFY(!ring->isVisible());
    QTRY_VERIFY(column->isVisible());

    // I hides them again, and the button follows.
    QTest::keyClick(fullscreen, Qt::Key_I);
    QTRY_VERIFY(!viewer->property("albumArtInfoVisible").toBool());
    QVERIFY(!info->property("checked").toBool());
    QVERIFY(!tint->isVisible());

    // The keyboard reaches it: the ring for focus, and Space toggles it,
    // with both the tint and the ring.
    for (int press = 0; press < 8 && !info->hasActiveFocus(); ++press) {
        QTest::keyClick(fullscreen, Qt::Key_Tab);
    }
    QVERIFY(info->hasActiveFocus());
    QVERIFY(ring->isVisible());
    QVERIFY(!tint->isVisible());
    QTest::keyClick(fullscreen, Qt::Key_Space);
    QTRY_VERIFY(viewer->property("albumArtInfoVisible").toBool());
    QVERIFY(tint->isVisible() && ring->isVisible());

    // Zoomed, the strip has the readout and Reset, which fits the art again.
    QQuickItem *strip = visibleItemNamed(fullscreen, QStringLiteral("albumArtFullscreenStrip"));
    QVERIFY(strip != nullptr);
    QQuickItem *readout = itemNamed(strip, QStringLiteral("viewerZoomReadout"));
    QQuickItem *reset = itemNamed(strip, QStringLiteral("viewerResetButton"));
    QVERIFY(readout != nullptr && reset != nullptr);
    QVERIFY(!readout->isVisible() && !reset->isVisible());
    surface->setProperty("zoom", 2.5);
    // In place at once, without waiting for the window to draw.
    QVERIFY(readout->isVisible() && reset->isVisible());
    QVERIFY(clearOfTheOtherButtons(reset) && clearOfTheOtherButtons(readout)
            && sceneRectOf(readout).right() <= sceneRectOf(reset).left());
    QTRY_VERIFY(readout->isVisible());
    QCOMPARE(readout->property("text").toString(), QStringLiteral("Zoom 250%"));
    QTRY_VERIFY(clearOfTheOtherButtons(reset) && clearOfTheOtherButtons(readout)
                && sceneRectOf(readout).right() <= sceneRectOf(reset).left());
    QTest::mouseClick(fullscreen, Qt::LeftButton, Qt::NoModifier, centerInWindow(reset));
    QTRY_COMPARE(surface->property("zoom").toDouble(), 1.0);
    QTRY_VERIFY(!readout->isVisible());

    QTest::keyClick(fullscreen, Qt::Key_Escape);
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
}

void TransportInputSmokeTest::albumArtStaysPutWhenTheDetailsOpen_data() {
    QTest::addColumn<int>("viewerMode");
    QTest::newRow("popup") << 0;
    QTest::newRow("whole screen") << 1;
}

void TransportInputSmokeTest::albumArtStaysPutWhenTheDetailsOpen() {
    // The details lie over the art's right edge: opening or closing them
    // moves, zooms and pans the art not at all, and the pointer on them
    // works the details rather than the art beneath.
    QFETCH(int, viewerMode);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Wider than the view, so that the art runs under the details.
    const QString coverPath = savedCover(dir, 1600, 800);
    QVERIFY(!coverPath.isEmpty());
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && palette != nullptr && surface != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    bridge->setProperty("viewerFullscreenMode", viewerMode);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewer"));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QQuickWindow *shownIn = window;
    if (viewerMode == 1) {
        shownIn = viewerWindowNamed(engine, QStringLiteral("albumArtFullscreenWindow"));
        QVERIFY(shownIn != nullptr);
        QVERIFY(QTest::qWaitForWindowActive(shownIn));
    }
    QTRY_VERIFY(surface->window() == shownIn && surface->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(surface->property("imageStatus").toInt(), 1, 5000);
    QVERIFY(!viewer->property("albumArtInfoVisible").toBool());

    QQuickItem *viewport = itemNamed(surface, QStringLiteral("albumArtViewport"));
    QQuickItem *image = itemNamed(surface, QStringLiteral("albumArtImage"));
    QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
    QQuickItem *seam = itemNamed(surface, QStringLiteral("albumArtInfoSeam"));
    QObject *contextMenu = surface->findChild<QObject *>(QStringLiteral("albumArtViewerContextMenu"));
    QVERIFY(viewport != nullptr && image != nullptr && column != nullptr && seam != nullptr
            && contextMenu != nullptr);
    QTRY_VERIFY(viewport->width() > 400 && viewport->height() > 200);
    const auto paintedArt = [image]() {
        const QSizeF painted(image->property("paintedWidth").toReal(),
                             image->property("paintedHeight").toReal());
        const QPointF origin((image->width() - painted.width()) / 2,
                             (image->height() - painted.height()) / 2);
        return QRectF(image->mapToScene(origin),
                      image->mapToScene(origin + QPointF(painted.width(), painted.height())));
    };
    struct View {
        QRectF art;
        double zoom;
        double panX;
        double panY;
    };
    const auto view = [&]() {
        return View{paintedArt(), surface->property("zoom").toDouble(),
                    surface->property("panX").toDouble(), surface->property("panY").toDouble()};
    };
    const auto sameView = [](const View &a, const View &b) {
        return a.art == b.art && a.zoom == b.zoom && a.panX == b.panX && a.panY == b.panY;
    };
    const auto describe = [](const View &v) {
        return QStringLiteral("art %1,%2 %3x%4 zoom %5 pan %6,%7")
            .arg(v.art.x()).arg(v.art.y()).arg(v.art.width()).arg(v.art.height())
            .arg(v.zoom).arg(v.panX).arg(v.panY);
    };
    const QRectF viewRect = sceneRectOf(viewport);

    // Zoomed and panned by the pointer on the art.
    const QPointF artPoint = viewRect.center() - QPointF(viewRect.width() / 4, 0);
    QVERIFY(sendWheel(shownIn, artPoint, QPoint(), QPoint(0, 600)));
    QVERIFY(surface->property("zoom").toDouble() > 1.0);
    QTest::mousePress(shownIn, Qt::LeftButton, Qt::NoModifier, artPoint.toPoint());
    QTest::mouseMove(shownIn, (artPoint + QPointF(40, 30)).toPoint());
    QTest::mouseMove(shownIn, (artPoint + QPointF(80, 60)).toPoint());
    QTest::mouseRelease(shownIn, Qt::LeftButton, Qt::NoModifier, (artPoint + QPointF(80, 60)).toPoint());
    QVERIFY(surface->property("panX").toDouble() != 0.0 || surface->property("panY").toDouble() != 0.0);
    const View before = view();

    // I shows the details over the view's right edge: the art stays put.
    QTest::keyClick(shownIn, Qt::Key_I);
    QTRY_VERIFY(viewer->property("albumArtInfoVisible").toBool());
    QTRY_VERIFY(column->isVisible() && seam->isVisible());
    QVERIFY2(sameView(view(), before), qPrintable(describe(view()) + QStringLiteral(" / ")
                                                   + describe(before)));
    QCOMPARE(sceneRectOf(viewport), viewRect);
    const QRectF columnRect = sceneRectOf(column);
    QCOMPARE(columnRect.right(), viewRect.right());
    QCOMPARE(columnRect.top(), viewRect.top());
    QCOMPARE(columnRect.bottom(), viewRect.bottom());
    QCOMPARE(columnRect.width(), 279.0);
    QVERIFY(paintedArt().right() > columnRect.right());
    // The column draws over the art, on its own backing, and the art beside
    // it still shows.
    const QImage shown = shownIn->grabWindow().convertToFormat(QImage::Format_RGB32);
    const qreal dpr = shown.devicePixelRatio();
    const auto pixelAt = [&shown, dpr](QPointF point) {
        return shown.pixel(int(point.x() * dpr), int(point.y() * dpr));
    };
    // Low in the art that shows, below the details' text.
    const QRectF artShown = paintedArt().intersected(viewRect);
    QVERIFY2(artShown.bottom() - 8 > columnRect.top() + columnRect.height() / 2,
             qPrintable(QStringLiteral("%1").arg(artShown.bottom())));
    const qreal sampleY = artShown.bottom() - 8;
    QCOMPARE(pixelAt(QPointF(columnRect.center().x(), sampleY)),
             QColor::fromRgba(colorOf(palette, "view")).rgb());
    QCOMPARE(pixelAt(QPointF(columnRect.left() - 40, sampleY)), QColor(Qt::darkCyan).rgb());

    // The wheel, a drag, a double-click and the context button on the
    // details neither zoom nor pan the art, nor close the viewer.
    const QPoint onColumn = QPointF(columnRect.center().x(), sampleY - 32).toPoint();
    QTest::mouseMove(shownIn, onColumn);
    sendWheel(shownIn, onColumn, QPoint(), QPoint(0, 360));
    sendWheel(shownIn, onColumn, QPoint(), QPoint(0, -720));
    QTest::mousePress(shownIn, Qt::LeftButton, Qt::NoModifier, onColumn);
    QTest::mouseMove(shownIn, onColumn + QPoint(-60, -40));
    QTest::mouseMove(shownIn, onColumn + QPoint(-120, -80));
    QTest::mouseRelease(shownIn, Qt::LeftButton, Qt::NoModifier, onColumn + QPoint(-120, -80));
    QTest::mouseDClick(shownIn, Qt::LeftButton, Qt::NoModifier, onColumn);
    // The seam is the details' too.
    const QPoint onSeam = sceneRectOf(seam).center().toPoint();
    QTest::mouseDClick(shownIn, Qt::LeftButton, Qt::NoModifier, onSeam);
    sendWheel(shownIn, onSeam, QPoint(), QPoint(0, 360));
    QTest::mouseClick(shownIn, Qt::RightButton, Qt::NoModifier, onColumn);
    QTest::qWait(50);
    QVERIFY2(sameView(view(), before), qPrintable(describe(view()) + QStringLiteral(" / ")
                                                   + describe(before)));
    QVERIFY(!contextMenu->property("visible").toBool());
    QVERIFY(viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(viewer->property("albumArtInfoVisible").toBool());

    // Closing them again leaves the art as it was.
    QTest::keyClick(shownIn, Qt::Key_I);
    QTRY_VERIFY(!viewer->property("albumArtInfoVisible").toBool());
    QTRY_VERIFY(!column->isVisible());
    QVERIFY2(sameView(view(), before), qPrintable(describe(view()) + QStringLiteral(" / ")
                                                   + describe(before)));
    QCOMPARE(sceneRectOf(viewport), viewRect);

    QTest::keyClick(shownIn, Qt::Key_Escape);
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
}

void TransportInputSmokeTest::menuMnemonicsOpenTheMenusAndShowShortcuts() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *playback = nullptr;
    for (QObject *object : window->findChildren<QObject *>()) {
        if (object->property("title").toString() == QStringLiteral("&Playback")) {
            playback = object;
            break;
        }
    }
    QVERIFY(playback != nullptr);
    QVERIFY(!playback->property("visible").toBool());
    QTest::keyClick(window, Qt::Key_P, Qt::AltModifier);
    QTRY_VERIFY(playback->property("visible").toBool());

    // The Play/Pause entry shows its shortcut.
    bool shown = false;
    for (QObject *object : window->findChildren<QObject *>()) {
        QObject *action = qvariant_cast<QObject *>(object->property("action"));
        if (action != nullptr
                && action->objectName() == QStringLiteral("playPauseAction")
                && object->property("visible").toBool()) {
            shown = action->property("shortcut").value<QKeySequence>()
                == QKeySequence(Qt::Key_Space);
        }
    }
    QVERIFY(shown);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!playback->property("visible").toBool());
}

namespace {

// Generated candidates for the iTunes artwork fake: ready rows, each with
// its own image, as the search and the download leave them.
QVariantList itunesArtworkFixture(const QTemporaryDir &dir, int count) {
    QVariantList rows;
    for (int i = 0; i < count; ++i) {
        const QString path = dir.filePath(QStringLiteral("candidate-%1.png").arg(i));
        QImage image(48, 48, QImage::Format_RGB32);
        image.fill(QColor::fromHsv((i * 37) % 360, 160, 200));
        if (!image.save(path)) {
            return {};
        }
        const QString url = QUrl::fromLocalFile(path).toString();
        rows.push_back(QVariantMap{
            {QStringLiteral("albumTitle"), QStringLiteral("Small Hours %1").arg(i)},
            {QStringLiteral("artistName"), QStringLiteral("The Quiet Meridian")},
            {QStringLiteral("previewSource"), url},
            {QStringLiteral("normalizedUrl"), url},
            {QStringLiteral("normalizedPath"), path},
            {QStringLiteral("assetReady"), true},
        });
    }
    return rows;
}

// A local fake of the iTunes artwork search and downloads, for the Replace
// from iTunes dialog of Main.qml: it holds the suggestions a test sets and
// records the ones prepared and applied. The playing track, its art and the
// image details come from `real`, the window's bridge.
QObject *injectItunesArtworkFake(QQmlEngine &engine, QObject *dialog, QObject *real) {
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral(R"QML(
import QtQuick 2.15
QtObject {
    id: fake
    required property var real
    property var itunesArtworkResults: []
    property var itunesArtworkTarget: ({})
    property bool itunesArtworkLoading: false
    property string itunesArtworkStatusText: ""
    property var preparedItunesArtwork: []
    property var appliedItunesArtwork: []
    readonly property string currentTrackPath: fake.real.currentTrackPath
    readonly property string currentTrackCoverPath: fake.real.currentTrackCoverPath
    signal itunesArtworkChanged()
    signal snapshotChanged()
    signal imageFileDetailsChanged(string path)
    function itunesArtworkResultAt(index) { return fake.itunesArtworkResults[index] || ({}) }
    function prepareItunesArtworkSuggestion(index) {
        fake.preparedItunesArtwork = fake.preparedItunesArtwork.concat([index])
    }
    function applyItunesArtworkSuggestion(index) {
        fake.appliedItunesArtwork = fake.appliedItunesArtwork.concat([index])
    }
    function clearItunesArtworkSuggestions() {
        fake.itunesArtworkResults = []
        fake.itunesArtworkTarget = ({})
    }
    function requestImageFileDetails(path) { fake.real.requestImageFileDetails(path) }
    function cachedImageFileDetails(path) { return fake.real.cachedImageFileDetails(path) }
    property Connections forward: Connections {
        target: fake.real
        function onSnapshotChanged() { fake.snapshotChanged() }
        function onImageFileDetailsChanged(path) { fake.imageFileDetailsChanged(path) }
    }
}
)QML"),
                      QUrl());
    QObject *fake = component.createWithInitialProperties(
        QVariantMap{{QStringLiteral("real"), QVariant::fromValue(real)}});
    if (fake == nullptr) {
        qWarning() << component.errorString();
        return nullptr;
    }
    fake->setParent(dialog);
    dialog->setProperty("uiBridge", QVariant::fromValue(fake));
    return fake;
}

// Replaces the fake's suggestion at `index` with `changes` applied, as a
// download that starts, ends or fails does.
void updateItunesArtworkRow(QObject *fake, int index, const QVariantMap &changes) {
    QVariantList rows = fake->property("itunesArtworkResults").toList();
    QVariantMap row = rows.value(index).toMap();
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        row.insert(it.key(), it.value());
    }
    rows[index] = row;
    fake->setProperty("itunesArtworkResults", rows);
    QMetaObject::invokeMethod(fake, "itunesArtworkChanged");
}

// Opens Replace from iTunes with `rows` for `target`, as a search finds them.
bool openItunesArtworkDialog(QQuickWindow *window, QObject *dialog, QObject *fake,
                             const QVariantList &rows, const QVariantMap &target) {
    fake->setProperty("itunesArtworkResults", rows);
    fake->setProperty("itunesArtworkTarget", target);
    return QMetaObject::invokeMethod(window, "openItunesArtworkDialog")
        && QTest::qWaitFor([dialog]() { return dialog->property("opened").toBool(); });
}

// Whether `item` is `ancestor` or inside it.
bool isWithin(const QQuickItem *item, const QQuickItem *ancestor) {
    for (; item != nullptr; item = item->parentItem()) {
        if (item == ancestor) {
            return true;
        }
    }
    return false;
}

// During a preview in `host`: its label and buttons are at the foot of the
// image's details, Back to Results before Apply on one row, and the
// viewer's controls named by `prefix` have none of them, nor Image Details.
void checkPreviewControlsInTheDetails(QQuickWindow *host, const QString &prefix,
                                      const QByteArray &at) {
    QQuickItem *column = visibleItemNamed(host, QStringLiteral("albumArtInfoColumn"));
    QVERIFY2(column != nullptr, at.constData());
    QQuickItem *label = visibleItemNamed(host, QStringLiteral("albumArtPreviewLabel"));
    QQuickItem *back = visibleItemNamed(host, QStringLiteral("albumArtPreviewBackButton"));
    QQuickItem *apply = visibleItemNamed(host, QStringLiteral("albumArtPreviewApplyButton"));
    QVERIFY2(label != nullptr && back != nullptr && apply != nullptr, at.constData());
    const QRectF columnRect = column->mapRectToScene(QRectF(0, 0, column->width(), column->height()));
    const auto rectOf = [](QQuickItem *item) {
        return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    };
    for (QQuickItem *item : {label, back, apply}) {
        QVERIFY2(isWithin(item, column), at.constData());
        QVERIFY2(columnRect.contains(rectOf(item)), at.constData());
    }
    // At the foot: the buttons near the bottom edge, the label above them,
    // Back to Results then Apply, ending at the column's end.
    QVERIFY2(columnRect.bottom() - rectOf(back).bottom() <= 24.0, at.constData());
    QCOMPARE(rectOf(back).top(), rectOf(apply).top());
    QVERIFY2(rectOf(label).bottom() <= rectOf(back).top(), at.constData());
    QVERIFY2(rectOf(back).right() <= rectOf(apply).left(), at.constData());
    QVERIFY2(columnRect.right() - rectOf(apply).right() <= 24.0, at.constData());
    QVERIFY2(rectOf(back).top() > columnRect.center().y(), at.constData());
    // Nothing of the preview on the strip or the corner plate.
    for (const QString &name : {QStringLiteral("PreviewLabel"), QStringLiteral("BackButton"),
                                QStringLiteral("ApplyButton"), QStringLiteral("InfoButton")}) {
        QVERIFY2(visibleItemNamed(host, prefix + name) == nullptr,
                 qPrintable(QStringLiteral("%1: %2").arg(QString::fromUtf8(at), prefix + name)));
    }
}

} // namespace

void TransportInputSmokeTest::itunesPreviewAppliesOrGoesBackToTheResults() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    const QString coverUrl = QUrl::fromLocalFile(coverPath).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 12);
    QCOMPARE(fixture.size(), 12);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
    QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr);
    QObject *fake = injectItunesArtworkFake(engine, dialog, bridge);
    QVERIFY(fake != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", coverUrl);
    const QVariantMap target{
        {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
        {QStringLiteral("album"), QStringLiteral("Small Hours")},
        {QStringLiteral("artist"), QStringLiteral("The Quiet Meridian")},
        {QStringLiteral("coverPath"), coverUrl},
    };
    const int previewed = 7;
    const QString candidateUrl = fixture.at(previewed).toMap().value(QStringLiteral("normalizedUrl")).toString();
    const auto applied = [fake]() { return fake->property("appliedItunesArtwork").toList(); };
    const auto resultsView = [dialog]() {
        return dialog->findChild<QQuickItem *>(QStringLiteral("itunesArtworkResultsView"));
    };

    // The viewer over the whole screen first, as in
    // viewerKeysWorkFromEveryControl: under the offscreen platform, its window
    // made after the popup viewer was used does not take the focus.
    for (const int fullscreenMode : {1, 0}) {
        bridge->setProperty("viewerFullscreenMode", fullscreenMode);
        for (const bool viewerOpenBefore : {false, true}) {
            const QString where = QStringLiteral("fullscreen %1, viewer open before %2")
                                      .arg(fullscreenMode).arg(viewerOpenBefore);
            const QByteArray at = where.toUtf8();
            // The window the viewer is in.
            const auto viewerHost = [&]() -> QQuickWindow * {
                return fullscreenMode == 0
                    ? window : viewerWindowNamed(engine, QStringLiteral("albumArtFullscreenWindow"));
            };
            // The viewer's controls: the corner plate in the main window, or
            // the title strip over the whole screen.
            const QString prefix = fullscreenMode == 0 ? QStringLiteral("albumArtPopup")
                                                       : QStringLiteral("albumArtFullscreen");
            if (viewerOpenBefore) {
                QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewer"));
                QTRY_VERIFY2(viewer->property("albumArtViewerOpen").toBool(), at.constData());
                QVERIFY2(QTest::qWaitForWindowActive(viewerHost()), at.constData());
            }
            // The dialog is over the viewer when that is open, in its window.
            QQuickWindow *dialogHost = viewerOpenBefore ? viewerHost() : window;
            QVERIFY2(openItunesArtworkDialog(window, dialog, fake, fixture, target), at.constData());
            QTRY_VERIFY2(resultsView() != nullptr && resultsView()->property("count").toInt() == 12,
                         at.constData());
            QCOMPARE(qvariant_cast<QQuickItem *>(dialog->property("parent"))->window(), dialogHost);
            resultsView()->setProperty("contentY", 400.0);
            QTRY_COMPARE(resultsView()->property("contentY").toReal(), 400.0);

            // Preview hides the dialog, keeping it, and shows the candidate
            // in the viewer, with its details and, at their foot, Back to
            // Results and Apply; the viewer's controls have none of them,
            // nor the Image Details button, and I keeps the details.
            const auto preview = [&]() {
                QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview",
                                                  Q_ARG(QVariant, previewed)));
                QTRY_VERIFY2(!dialog->property("visible").toBool(), at.constData());
                QVERIFY2(dialog->property("previewShown").toBool(), at.constData());
                QTRY_VERIFY2(viewer->property("albumArtViewerOpen").toBool(), at.constData());
                QCOMPARE(viewer->property("albumArtViewerSource").toString(), candidateUrl);
                QVERIFY(!viewer->property("albumArtViewerShowsCurrentTrack").toBool());
                QQuickWindow *host = viewerHost();
                QVERIFY2(host != nullptr && QTest::qWaitForWindowActive(host), at.constData());
                QTRY_VERIFY2(visibleItemNamed(host, QStringLiteral("albumArtPreviewBackButton")) != nullptr,
                             at.constData());
                QQuickItem *label = visibleItemNamed(host, QStringLiteral("albumArtPreviewLabel"));
                QVERIFY2(label != nullptr, at.constData());
                QCOMPARE(label->property("text").toString(), QStringLiteral("iTunes Candidate"));
                QCOMPARE(host->title(),
                         QStringLiteral("Morning Tide Field — iTunes Candidate — Ferrous Album Art"));
                QQuickItem *apply = visibleItemNamed(host, QStringLiteral("albumArtPreviewApplyButton"));
                QVERIFY2(apply != nullptr, at.constData());
                QTRY_VERIFY2(apply->isEnabled(), at.constData());
                checkPreviewControlsInTheDetails(host, prefix, at);
                QTest::keyClick(host, Qt::Key_I);
                QCoreApplication::processEvents();
                QVERIFY2(viewer->property("albumArtInfoVisible").toBool(), at.constData());
                QVERIFY2(visibleItemNamed(host, QStringLiteral("albumArtInfoColumn")) != nullptr,
                         at.constData());
                // The results stay as they were, hidden.
                QCOMPARE(resultsView()->property("count").toInt(), 12);
            };
            const auto backAtTheResults = [&]() {
                QTRY_VERIFY2(dialog->property("opened").toBool(), at.constData());
                QVERIFY(!dialog->property("previewShown").toBool());
                QCOMPARE(qvariant_cast<QQuickItem *>(dialog->property("parent"))->window(), dialogHost);
                QCOMPARE(resultsView()->property("count").toInt(), 12);
                QTRY_COMPARE(resultsView()->property("contentY").toReal(), 400.0);
                QCOMPARE(fake->property("itunesArtworkResults").toList().size(), 12);
                // The viewer shows what it did before: nothing, or the art.
                QTRY_COMPARE(viewer->property("albumArtViewerOpen").toBool(), viewerOpenBefore);
                QVERIFY(!viewer->property("albumArtPreviewActive").toBool());
                if (viewerOpenBefore) {
                    QCOMPARE(viewer->property("albumArtViewerSource").toString(), coverUrl);
                    QVERIFY(viewer->property("albumArtViewerShowsCurrentTrack").toBool());
                    QTRY_VERIFY2(viewerHost()->isVisible(), at.constData());
                    if (fullscreenMode == 0) {
                        QTRY_VERIFY2(window->findChild<QObject *>(QStringLiteral("albumArtViewerPopup"))
                                         ->property("visible").toBool(), at.constData());
                    }
                }
                QVERIFY(applied().isEmpty());
            };

            preview();
            // Comparing with the current art works in the preview, which can
            // still be applied from there.
            QQuickWindow *host = viewerHost();
            QTest::keyClick(host, Qt::Key_Left);
            QTRY_VERIFY2(viewer->property("albumArtViewerShowsCurrentTrack").toBool(), at.constData());
            QCOMPARE(visibleItemNamed(host, QStringLiteral("albumArtPreviewLabel"))->property("text").toString(),
                     QStringLiteral("Current Album Art"));
            QVERIFY(visibleItemNamed(host, QStringLiteral("albumArtPreviewApplyButton"))->isEnabled());
            QTest::keyClick(host, Qt::Key_Right);
            QTRY_VERIFY2(!viewer->property("albumArtViewerShowsCurrentTrack").toBool(), at.constData());

            // Back to Results, clicked: the preview is in front of the dialog.
            QVERIFY2(clickVisibleButton(host, QStringLiteral("albumArtPreviewBackButton")), at.constData());
            backAtTheResults();

            // Escape goes back too; the popup viewer stays open for it, rather
            // than closing and opening again.
            preview();
            QSignalSpy popupClosed(window->findChild<QObject *>(QStringLiteral("albumArtViewerPopup")),
                                   SIGNAL(closed()));
            QVERIFY(popupClosed.isValid());
            QTest::keyClick(viewerHost(), Qt::Key_Escape);
            backAtTheResults();
            if (viewerOpenBefore) {
                QCOMPARE(popupClosed.count(), 0);
            }

            // Apply asks first, as the album has art, over the viewer; Cancel
            // stays in the preview, and Replace applies, closes the dialog
            // and leaves the viewer showing the cover.
            preview();
            host = viewerHost();
            QVERIFY2(clickVisibleButton(host, QStringLiteral("albumArtPreviewApplyButton")), at.constData());
            QTRY_VERIFY2(visibleItemNamed(host, QStringLiteral("confirmDialogConfirmButton")) != nullptr,
                         at.constData());
            QVERIFY(applied().isEmpty());
            QVERIFY2(clickVisibleButton(host, QStringLiteral("confirmDialogCancelButton")), at.constData());
            QTRY_VERIFY2(visibleItemNamed(host, QStringLiteral("confirmDialogConfirmButton")) == nullptr,
                         at.constData());
            QVERIFY(dialog->property("previewShown").toBool());
            QVERIFY(viewer->property("albumArtPreviewActive").toBool());
            QVERIFY2(clickVisibleButton(host, QStringLiteral("albumArtPreviewApplyButton")), at.constData());
            QTRY_VERIFY2(visibleItemNamed(host, QStringLiteral("confirmDialogConfirmButton")) != nullptr,
                         at.constData());
            QVERIFY2(clickVisibleButton(host, QStringLiteral("confirmDialogConfirmButton")), at.constData());
            QTRY_COMPARE(applied(), (QVariantList{previewed}));
            QVERIFY(!dialog->property("previewShown").toBool());
            QVERIFY(!dialog->property("visible").toBool());
            QVERIFY(fake->property("itunesArtworkResults").toList().isEmpty());
            QVERIFY(viewer->property("albumArtViewerOpen").toBool());
            QVERIFY(!viewer->property("albumArtPreviewActive").toBool());
            QVERIFY(viewer->property("albumArtViewerShowsCurrentTrack").toBool());
            QCOMPARE(viewer->property("albumArtViewerSource").toString(), coverUrl);
            QTRY_VERIFY2(visibleItemNamed(host, QStringLiteral("albumArtPreviewBackButton")) == nullptr,
                         at.constData());
            // Out of the preview, the Image Details button is back.
            QTRY_VERIFY2(visibleItemNamed(host, prefix + QStringLiteral("InfoButton")) != nullptr,
                         at.constData());

            // Closing the viewer now closes it, with no dialog to go back to.
            QTest::keyClick(host, Qt::Key_Escape);
            QTRY_VERIFY2(!viewer->property("albumArtViewerOpen").toBool(), at.constData());
            QVERIFY(!dialog->property("visible").toBool());
            fake->setProperty("appliedItunesArtwork", QVariantList{});
            window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(window));
        }
    }

    // An album without art applies at once, and the viewer shows the new
    // cover when the album has it.
    bridge->setProperty("viewerFullscreenMode", 0);
    bridge->setProperty("currentTrackCoverPath", QString());
    QVariantMap bareTarget = target;
    bareTarget.insert(QStringLiteral("coverPath"), QString());
    QVERIFY(openItunesArtworkDialog(window, dialog, fake, fixture, bareTarget));
    QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, previewed)));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QTRY_VERIFY(visibleItemNamed(window, QStringLiteral("albumArtPreviewApplyButton")) != nullptr);
    QTRY_VERIFY(visibleItemNamed(window, QStringLiteral("albumArtPreviewApplyButton"))->isEnabled());
    QVERIFY(clickVisibleButton(window, QStringLiteral("albumArtPreviewApplyButton")));
    QTRY_COMPARE(applied(), (QVariantList{previewed}));
    QVERIFY(visibleItemNamed(window, QStringLiteral("confirmDialogConfirmButton")) == nullptr);
    QVERIFY(!dialog->property("visible").toBool());
    QVERIFY(viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(viewer->property("albumArtViewerShowsCurrentTrack").toBool());
    bridge->setProperty("currentTrackCoverPath", candidateUrl);
    QVERIFY(QMetaObject::invokeMethod(bridge, "snapshotChanged"));
    QTRY_COMPARE(viewer->property("albumArtViewerSource").toString(), candidateUrl);
}

void TransportInputSmokeTest::itunesPreviewButtonsTakeSpaceFromThePlaybackShortcut() {
    // Space on a focused Back to Results or Apply presses it; the viewer's
    // Play/Pause shortcut does not take it.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    const QString coverUrl = QUrl::fromLocalFile(coverPath).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 3);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr && playPause != nullptr);
    QObject *fake = injectItunesArtworkFake(engine, dialog, bridge);
    QVERIFY(fake != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", coverUrl);
    const QVariantMap target{
        {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
        {QStringLiteral("album"), QStringLiteral("Small Hours")},
        {QStringLiteral("artist"), QStringLiteral("The Quiet Meridian")},
        {QStringLiteral("coverPath"), coverUrl},
    };
    QSignalSpy toggled(playPause, SIGNAL(triggered(QObject*)));

    // Over the whole screen first, as in itunesPreviewAppliesOrGoesBackToTheResults.
    for (const int fullscreenMode : {1, 0}) {
        const QByteArray at = QByteArray("fullscreen ") + QByteArray::number(fullscreenMode);
        bridge->setProperty("viewerFullscreenMode", fullscreenMode);
        const auto viewerHost = [&]() -> QQuickWindow * {
            return fullscreenMode == 0
                ? window : viewerWindowNamed(engine, QStringLiteral("albumArtFullscreenWindow"));
        };
        const auto preview = [&]() -> QQuickWindow * {
            if (!openItunesArtworkDialog(window, dialog, fake, fixture, target)) {
                return nullptr;
            }
            QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, 1));
            if (!QTest::qWaitFor([&]() { return viewer->property("albumArtPreviewActive").toBool()
                                         && !dialog->property("visible").toBool(); })) {
                return nullptr;
            }
            QQuickWindow *host = viewerHost();
            return host != nullptr && QTest::qWaitForWindowActive(host) ? host : nullptr;
        };
        const QString playbackBefore = bridge->property("playbackState").toString();

        // Back to Results.
        QQuickWindow *host = preview();
        QVERIFY2(host != nullptr, at.constData());
        QQuickItem *back = nullptr;
        QTRY_VERIFY2((back = visibleItemNamed(host, QStringLiteral("albumArtPreviewBackButton"))) != nullptr,
                     at.constData());
        QVERIFY2(tabTo(host, back), at.constData());
        QTest::keyClick(host, Qt::Key_Space);
        QTRY_VERIFY2(dialog->property("opened").toBool(), at.constData());
        QVERIFY2(!viewer->property("albumArtPreviewActive").toBool(), at.constData());
        QCOMPARE(toggled.count(), 0);
        QCOMPARE(bridge->property("playbackState").toString(), playbackBefore);

        // Apply: the album has art, so it asks first.
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        host = preview();
        QVERIFY2(host != nullptr, at.constData());
        QQuickItem *apply = nullptr;
        QTRY_VERIFY2((apply = visibleItemNamed(host, QStringLiteral("albumArtPreviewApplyButton"))) != nullptr
                         && apply->isEnabled(), at.constData());
        QVERIFY2(tabTo(host, apply), at.constData());
        QTest::keyClick(host, Qt::Key_Space);
        QTRY_VERIFY2(visibleItemNamed(host, QStringLiteral("confirmDialogConfirmButton")) != nullptr,
                     at.constData());
        QCOMPARE(toggled.count(), 0);
        QCOMPARE(bridge->property("playbackState").toString(), playbackBefore);
        QVERIFY2(clickVisibleButton(host, QStringLiteral("confirmDialogCancelButton")), at.constData());
        QTRY_VERIFY2(visibleItemNamed(host, QStringLiteral("confirmDialogConfirmButton")) == nullptr,
                     at.constData());

        // Away from the buttons, Space still plays and pauses.
        QQuickItem *viewFocus = itemNamed(host->contentItem(), QStringLiteral("albumArtFullscreenViewFocus"));
        if (viewFocus != nullptr) {
            viewFocus->forceActiveFocus();
            QTest::keyClick(host, Qt::Key_Space);
            QTRY_COMPARE(toggled.count(), 1);
            toggled.clear();
        }
        QTest::keyClick(host, Qt::Key_Escape);
        QTRY_VERIFY2(dialog->property("opened").toBool(), at.constData());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
}

void TransportInputSmokeTest::itunesPreviewButtonsStackWhenTheyDoNotFitSideBySide() {
    // Back to Results and Apply stay whole inside the details column at
    // larger fonts. With the usual font they share a row at its end; with
    // one too large for that, they stack. Breeze's buttons no longer fit
    // side by side at 14 pt; the test's style's are narrower, so 24 pt
    // makes sure they stack.
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    const QString coverUrl = QUrl::fromLocalFile(coverPath).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 3);

    for (const qreal pointSize : {0.0, 14.0, 24.0}) {
        const QByteArray at = QByteArray("point size ") + QByteArray::number(pointSize);
        if (pointSize > 0) {
            QFont larger = applicationFont;
            larger.setPointSizeF(pointSize);
            QGuiApplication::setFont(larger);
        }
        LibraryTreeModel libraryModel;
        QQmlApplicationEngine engine;
        QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
        QVERIFY2(window != nullptr, "Main.qml failed to show");
        QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
        QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
        QObject *dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
        QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr);
        QObject *fake = injectItunesArtworkFake(engine, dialog, bridge);
        QVERIFY(fake != nullptr);
        setPlayingTrack(bridge);
        bridge->setProperty("currentTrackCoverPath", coverUrl);
        const QVariantMap target{
            {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
            {QStringLiteral("coverPath"), coverUrl},
        };
        QVERIFY(openItunesArtworkDialog(window, dialog, fake, fixture, target));
        QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, 1)));
        QTRY_VERIFY(viewer->property("albumArtPreviewActive").toBool());
        QQuickItem *column = nullptr;
        QQuickItem *back = nullptr;
        QQuickItem *apply = nullptr;
        QTRY_VERIFY2((column = visibleItemNamed(window, QStringLiteral("albumArtInfoColumn"))) != nullptr
                         && (back = visibleItemNamed(window, QStringLiteral("albumArtPreviewBackButton"))) != nullptr
                         && (apply = visibleItemNamed(window, QStringLiteral("albumArtPreviewApplyButton"))) != nullptr,
                     at.constData());
        if (pointSize > 0) {
            QCOMPARE(qvariant_cast<QFont>(back->property("font")).pointSizeF(), pointSize);
        }
        QCoreApplication::processEvents();
        const QRectF columnRect = sceneRectOf(column);
        // Both whole, inside the column with its margins, apart.
        QTRY_VERIFY2(columnRect.adjusted(15.5, 0, -15.5, 0).contains(sceneRectOf(back))
                         && columnRect.adjusted(15.5, 0, -15.5, 0).contains(sceneRectOf(apply)),
                     qPrintable(QStringLiteral("%1: column %2..%3, back %4..%5, apply %6..%7")
                                    .arg(QString::fromLatin1(at))
                                    .arg(columnRect.left()).arg(columnRect.right())
                                    .arg(sceneRectOf(back).left()).arg(sceneRectOf(back).right())
                                    .arg(sceneRectOf(apply).left()).arg(sceneRectOf(apply).right())));
        QVERIFY2(!sceneRectOf(back).intersects(sceneRectOf(apply)), at.constData());
        QVERIFY2(back->width() >= back->implicitWidth() - 0.5 && apply->width() >= apply->implicitWidth() - 0.5,
                 at.constData());
        const bool fitsInARow = back->implicitWidth() + apply->implicitWidth() + 4
            <= columnRect.width() - 32;
        if (fitsInARow) {
            // One row, Back to Results then Apply, at the column's end.
            QCOMPARE(sceneRectOf(back).top(), sceneRectOf(apply).top());
            QVERIFY(sceneRectOf(back).right() <= sceneRectOf(apply).left());
            QVERIFY(columnRect.right() - 16.5 <= sceneRectOf(apply).right());
        } else {
            // Stacked, Back to Results above Apply, each the column's width.
            QVERIFY2(pointSize > 0, "the usual font fits them in a row");
            QVERIFY(columnRect.contains(sceneRectOf(back).adjusted(0, -0.5, 0, 0)));
            QVERIFY(sceneRectOf(back).bottom() <= sceneRectOf(apply).top());
            QCOMPARE(sceneRectOf(back).width(), sceneRectOf(apply).width());
            QCOMPARE(sceneRectOf(back).width(), columnRect.width() - 32);
        }
        if (pointSize >= 24.0) {
            QVERIFY2(!fitsInARow, "24 pt does not fit them in a row");
        }
        QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(finishIncubation(engine));
    }
}

void TransportInputSmokeTest::itunesPreviewCannotApplyACandidateThatIsNotReady() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    const QString coverUrl = QUrl::fromLocalFile(coverPath).toString();
    QVariantList fixture = itunesArtworkFixture(dir, 4);
    QCOMPARE(fixture.size(), 4);
    // Candidate 2 is still downloading, with only its thumbnail; candidate
    // 3's file is not an image.
    QVariantMap loading = fixture.at(2).toMap();
    loading.insert(QStringLiteral("normalizedUrl"), QString());
    loading.insert(QStringLiteral("normalizedPath"), QString());
    loading.insert(QStringLiteral("assetReady"), false);
    fixture[2] = loading;
    const QString brokenPath = dir.filePath(QStringLiteral("broken.png"));
    {
        QFile broken(brokenPath);
        QVERIFY(broken.open(QIODevice::WriteOnly));
        QVERIFY(broken.write("not an image") > 0);
    }
    QVariantMap broken = fixture.at(3).toMap();
    broken.insert(QStringLiteral("normalizedUrl"), QUrl::fromLocalFile(brokenPath).toString());
    broken.insert(QStringLiteral("normalizedPath"), brokenPath);
    fixture[3] = broken;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
    QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr);
    QObject *fake = injectItunesArtworkFake(engine, dialog, bridge);
    QVERIFY(fake != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", coverUrl);
    bridge->setProperty("viewerFullscreenMode", 0);
    const QVariantMap target{
        {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
        {QStringLiteral("album"), QStringLiteral("Small Hours")},
        {QStringLiteral("coverPath"), QString()},
    };
    const auto applied = [fake]() { return fake->property("appliedItunesArtwork").toList(); };
    const auto applyButton = [window]() {
        return visibleItemNamed(window, QStringLiteral("albumArtPreviewApplyButton"));
    };
    QVERIFY(openItunesArtworkDialog(window, dialog, fake, fixture, target));

    // Preview waits for the download: the dialog stays until it is done.
    QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, 2)));
    QTRY_COMPARE(fake->property("preparedItunesArtwork").toList(), (QVariantList{2}));
    updateItunesArtworkRow(fake, 2, {{QStringLiteral("assetLoading"), true}});
    QVERIFY(dialog->property("visible").toBool());
    QVERIFY(!viewer->property("albumArtViewerOpen").toBool());
    const QString readyPath = dir.filePath(QStringLiteral("candidate-2.png"));
    updateItunesArtworkRow(fake, 2, {
        {QStringLiteral("assetLoading"), false},
        {QStringLiteral("assetReady"), true},
        {QStringLiteral("normalizedPath"), readyPath},
        {QStringLiteral("normalizedUrl"), QUrl::fromLocalFile(readyPath).toString()},
    });
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QTRY_VERIFY(applyButton() != nullptr && applyButton()->isEnabled());

    // Loading again, or failed, it cannot be applied from the preview.
    updateItunesArtworkRow(fake, 2, {{QStringLiteral("assetLoading"), true}});
    QTRY_VERIFY(!applyButton()->isEnabled());
    QVERIFY(!clickVisibleButton(window, QStringLiteral("albumArtPreviewApplyButton")));
    updateItunesArtworkRow(fake, 2, {
        {QStringLiteral("assetLoading"), false},
        {QStringLiteral("assetError"), QStringLiteral("The artwork could not be downloaded.")},
    });
    QVERIFY(!applyButton()->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(dialog, "applyPreviewedSuggestion"));
    QVERIFY(applied().isEmpty());
    updateItunesArtworkRow(fake, 2, {{QStringLiteral("assetError"), QString()}});
    QTRY_VERIFY(applyButton()->isEnabled());

    // A candidate whose image the viewer cannot read cannot be applied.
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(!viewer->property("albumArtViewerOpen").toBool());
    // The viewer and the dialog's check of the candidate each warn once.
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Error decoding: .*broken\\.png")));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Error decoding: .*broken\\.png")));
    QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, 3)));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QQuickItem *surface = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(surface != nullptr);
    // Image.Error.
    QTRY_COMPARE(surface->property("imageStatus").toInt(), 3);
    // The viewer's warning for it, which the test expects.
    const QStringList decodeWarnings = takeCapturedMessagesText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &warning : decodeWarnings) {
        QVERIFY2(warning.contains(QStringLiteral("Error decoding")) && warning.contains(QStringLiteral("broken.png")),
                 qPrintable(warning));
    }
    QTRY_VERIFY(applyButton() != nullptr);
    QVERIFY(!applyButton()->isEnabled());
    QVERIFY(!clickVisibleButton(window, QStringLiteral("albumArtPreviewApplyButton")));
    QVERIFY(applied().isEmpty());
    // Nor while the viewer shows the current art to compare, which decodes.
    QTest::keyClick(window, Qt::Key_Left);
    QTRY_VERIFY(viewer->property("albumArtViewerShowsCurrentTrack").toBool());
    QCOMPARE(viewer->property("albumArtViewerSource").toString(), coverUrl);
    QTRY_COMPARE(surface->property("imageStatus").toInt(), 1);
    QVERIFY(!applyButton()->isEnabled());
    QVERIFY(!dialog->property("previewCandidateReady").toBool());
    QVERIFY(!clickVisibleButton(window, QStringLiteral("albumArtPreviewApplyButton")));
    QVERIFY(QMetaObject::invokeMethod(dialog, "applyPreviewedSuggestion"));
    QVERIFY(applied().isEmpty());
    QVERIFY(clickVisibleButton(window, QStringLiteral("albumArtPreviewBackButton")));
    QTRY_VERIFY(dialog->property("opened").toBool());
}

void TransportInputSmokeTest::itunesPreviewShowsTheSearchedAlbumsArtAfterPlaybackMovesOn() {
    QTemporaryDir dir;
    QTemporaryDir otherDir;
    QVERIFY(dir.isValid() && otherDir.isValid());
    const QString coverUrl = QUrl::fromLocalFile(savedCover(dir)).toString();
    const QString otherCoverUrl = QUrl::fromLocalFile(savedCover(otherDir, 40, 40)).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 3);
    QCOMPARE(fixture.size(), 3);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
    QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr);
    QObject *fake = injectItunesArtworkFake(engine, dialog, bridge);
    QVERIFY(fake != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", coverUrl);
    bridge->setProperty("viewerFullscreenMode", 0);
    // The suggestions are for the playing track's album, which has art.
    QVERIFY(openItunesArtworkDialog(window, dialog, fake, fixture, QVariantMap{
        {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
        {QStringLiteral("album"), QStringLiteral("Small Hours")},
        {QStringLiteral("coverPath"), coverUrl},
    }));
    const auto moveOnTo = [bridge](const QString &cover) {
        bridge->setProperty("currentTrackPath", QStringLiteral("/music/Elsewhere/01 Lantern Rows.flac"));
        bridge->setProperty("currentTrackCoverPath", cover);
        QMetaObject::invokeMethod(bridge, "snapshotChanged");
    };
    const auto backToTheResults = [&]() {
        QVERIFY(clickVisibleButton(window, QStringLiteral("albumArtPreviewBackButton")));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
    };

    for (const QString &otherCover : {QString(), otherCoverUrl}) {
        const QByteArray at = otherCover.isEmpty() ? "next album without art" : "next album with art";
        // Playback moves on to another album: the dialog keeps the searched
        // album's art, and its Preview shows that art.
        moveOnTo(otherCover);
        QTRY_COMPARE(dialog->property("currentArtworkSource").toString(), coverUrl);
        QVERIFY2(clickVisibleButton(window, QStringLiteral("itunesArtworkCurrentPreviewButton")), at.constData());
        QTRY_VERIFY2(viewer->property("albumArtViewerOpen").toBool(), at.constData());
        QTRY_VERIFY2(!dialog->property("visible").toBool(), at.constData());
        QCOMPARE(viewer->property("albumArtViewerSource").toString(), coverUrl);
        QTRY_VERIFY2(visibleItemNamed(window, QStringLiteral("albumArtPreviewBackButton")) != nullptr,
                     at.constData());
        // It stays on that art while playback goes on.
        QMetaObject::invokeMethod(bridge, "snapshotChanged");
        QCOMPARE(viewer->property("albumArtViewerSource").toString(), coverUrl);
        backToTheResults();

        // A candidate compares with the searched album's art too.
        QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, 1)));
        QTRY_VERIFY2(viewer->property("albumArtViewerOpen").toBool(), at.constData());
        QTest::keyClick(window, Qt::Key_Left);
        QTRY_VERIFY2(viewer->property("albumArtViewerShowsCurrentTrack").toBool(), at.constData());
        QCOMPARE(viewer->property("albumArtViewerSource").toString(), coverUrl);
        QMetaObject::invokeMethod(bridge, "snapshotChanged");
        QCOMPARE(viewer->property("albumArtViewerSource").toString(), coverUrl);
        backToTheResults();
    }

    // Without the searched album's art there is nothing to preview: the
    // dialog stays, rather than hiding with no preview to go back from.
    fake->setProperty("itunesArtworkTarget", QVariantMap{
        {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
        {QStringLiteral("album"), QStringLiteral("Small Hours")},
        {QStringLiteral("coverPath"), QString()},
    });
    QMetaObject::invokeMethod(bridge, "snapshotChanged");
    QTRY_COMPARE(dialog->property("currentArtworkSource").toString(), QString());
    QVERIFY(QMetaObject::invokeMethod(dialog, "showPreview", Q_ARG(QVariant, -1)));
    QVERIFY(dialog->property("opened").toBool());
    QVERIFY(!dialog->property("previewShown").toBool());
    QVERIFY(!viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(!viewer->property("albumArtPreviewActive").toBool());
}

void TransportInputSmokeTest::itunesPreviewApplyShowsTheAppliedCoverAfterPlaybackMovesOn() {
    QTemporaryDir dir;
    QTemporaryDir otherDir;
    QTemporaryDir nextDir;
    QVERIFY(dir.isValid() && otherDir.isValid() && nextDir.isValid());
    const QString coverUrl = QUrl::fromLocalFile(savedCover(dir)).toString();
    const QString otherCoverUrl = QUrl::fromLocalFile(savedCover(otherDir, 40, 40)).toString();
    const QString nextCoverUrl = QUrl::fromLocalFile(savedCover(nextDir, 32, 32)).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 3);
    QCOMPARE(fixture.size(), 3);
    const QString appliedUrl = fixture.at(1).toMap().value(QStringLiteral("normalizedUrl")).toString();
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
    QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr);
    QObject *fake = injectItunesArtworkFake(engine, dialog, bridge);
    QVERIFY(fake != nullptr);
    bridge->setProperty("viewerFullscreenMode", 0);
    const auto playTrack = [bridge](const QString &path, const QString &cover) {
        bridge->setProperty("currentTrackPath", path);
        bridge->setProperty("currentTrackCoverPath", cover);
        QMetaObject::invokeMethod(bridge, "snapshotChanged");
    };
    const auto viewerSource = [viewer]() { return viewer->property("albumArtViewerSource").toString(); };

    for (const QString &otherCover : {otherCoverUrl, QString()}) {
        const QByteArray at = otherCover.isEmpty() ? "next album without art" : "next album with art";
        // The suggestions are for album A, which has art; playback then
        // moves on to album B.
        setPlayingTrack(bridge);
        playTrack(QStringLiteral("/music/Morning Tide Field.flac"), coverUrl);
        QVERIFY2(openItunesArtworkDialog(window, dialog, fake, fixture, QVariantMap{
            {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
            {QStringLiteral("album"), QStringLiteral("Small Hours")},
            {QStringLiteral("coverPath"), coverUrl},
        }), at.constData());
        playTrack(QStringLiteral("/music/Elsewhere/01 Lantern Rows.flac"), otherCover);

        // Apply from the preview, past the question: the viewer shows the
        // cover applied to A, not B's art, nor nothing.
        QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, 1)));
        QTRY_VERIFY2(viewer->property("albumArtViewerOpen").toBool(), at.constData());
        QTRY_VERIFY2(visibleItemNamed(window, QStringLiteral("albumArtPreviewApplyButton")) != nullptr
                         && visibleItemNamed(window, QStringLiteral("albumArtPreviewApplyButton"))->isEnabled(),
                     at.constData());
        QVERIFY(clickVisibleButton(window, QStringLiteral("albumArtPreviewApplyButton")));
        QTRY_VERIFY2(visibleItemNamed(window, QStringLiteral("confirmDialogConfirmButton")) != nullptr,
                     at.constData());
        QVERIFY(clickVisibleButton(window, QStringLiteral("confirmDialogConfirmButton")));
        QTRY_COMPARE(fake->property("appliedItunesArtwork").toList(), (QVariantList{1}));
        QTRY_VERIFY2(!dialog->property("visible").toBool(), at.constData());
        QVERIFY(viewer->property("albumArtViewerOpen").toBool());
        QVERIFY(!viewer->property("albumArtPreviewActive").toBool());
        QCOMPARE(viewerSource(), appliedUrl);
        QTRY_VERIFY2(visibleItemNamed(window, QStringLiteral("albumArtPreviewBackButton")) == nullptr,
                     at.constData());

        // It stays while B plays on, and follows playback at the next track.
        QMetaObject::invokeMethod(bridge, "snapshotChanged");
        QCOMPARE(viewerSource(), appliedUrl);
        playTrack(QStringLiteral("/music/Elsewhere/02 Harbour Steps.flac"), nextCoverUrl);
        QTRY_COMPARE(viewerSource(), nextCoverUrl);
        QVERIFY(viewer->property("albumArtViewerShowsCurrentTrack").toBool());
        playTrack(QStringLiteral("/music/Elsewhere/03 Low Tide.flac"), otherCoverUrl);
        QTRY_COMPARE(viewerSource(), otherCoverUrl);

        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY2(!viewer->property("albumArtViewerOpen").toBool(), at.constData());
        fake->setProperty("appliedItunesArtwork", QVariantList{});
    }
}

std::unique_ptr<QmlSmokeTest> makeTransportInputSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<TransportInputSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_transport_input.moc"
